// Host test of the serial host link's wire (nowde#t-050 ②) -- what a node sends on UART / CDC,
// read back by a byte-stream MIDI parser. tools/selftest.py compiles and runs it when a host C++
// compiler is on PATH. The node side is host_link.cpp's: a SysEx frame goes out as its bytes, a
// short message as midiMessageBytes() (src/usb_midi_packets.h); the reader is src/midi_rx.h's
// streamByte(), the node's own receive parse. It pins:
//  - every short message the node can send goes out as status + its data bytes, no padding, and
//    reads back as itself (masked to 7 bits, unused data 0),
//    and as the same call the USB-MIDI packet of that message makes, where the USB parse reads it;
//  - what midiMessageBytes() refuses (F0, F4, F5, F7, data bytes) writes nothing;
//  - SysEx frames of every length 2..CAP+40 read back whole, or as one overflow past CAP;
//  - a seeded random stream of messages and frames, back to back as the TX ring sends them, reads
//    back item for item: the serial link and the USB-MIDI link deliver the same calls.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/midi_rx.h"
#include "../src/usb_midi_packets.h"

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

static const size_t CAP = 512;

struct Ev {
  int kind;   // 0 message, 1 sysex, 2 overflow
  std::vector<uint8_t> bytes;
  bool operator==(const Ev& o) const { return kind == o.kind && bytes == o.bytes; }
};

struct Rec {
  std::vector<Ev> ev;
  void message(uint8_t s, uint8_t a, uint8_t b) { ev.push_back({0, {s, a, b}}); }
  void sysex(const uint8_t* f, size_t n) { ev.push_back({1, std::vector<uint8_t>(f, f + n)}); }
  void sysexOverflow(size_t cap) { ev.push_back({2, {static_cast<uint8_t>(cap >> 8)}}); }
};

static uint32_t rng = 0x50u;
static uint32_t next() { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7FFF; }

// What the message means, whatever link carries it.
static Ev expected(uint8_t s, uint8_t d1, uint8_t d2) {
  d1 &= 0x7F;
  d2 &= 0x7F;
  if (s >= 0xF8 || s == 0xF6) return {0, {s, 0, 0}};
  if (s == 0xF1 || s == 0xF3) return {0, {s, d1, 0}};
  if (s == 0xF2) return {0, {s, d1, d2}};
  const uint8_t kind = s & 0xF0;
  return {0, {s, d1, (kind == 0xC0 || kind == 0xD0) ? uint8_t(0) : d2}};
}

// The node's TX bytes for one item: a frame as it is, a message through midiMessageBytes().
static void send(std::vector<uint8_t>& wire, const std::vector<uint8_t>& item) {
  if (item[0] == 0xF0) {
    wire.insert(wire.end(), item.begin(), item.end());
    return;
  }
  uint8_t b[3];
  const size_t n = midiMessageBytes(item[0], item[1], item[2], b);
  wire.insert(wire.end(), b, b + n);
}

static std::vector<Ev> readStream(const std::vector<uint8_t>& wire) {
  MidiRxParser<CAP> p;
  Rec r;
  for (uint8_t b : wire) p.streamByte(b, r);
  return r.ev;
}

static std::vector<uint8_t> frame(size_t len) {
  std::vector<uint8_t> f(len);
  f[0] = 0xF0;
  for (size_t i = 1; i + 1 < len; i++) f[i] = static_cast<uint8_t>(next() & 0x7F);
  f[len - 1] = 0xF7;
  return f;
}

// The bytes a short message is on the wire: status + its data bytes, nothing after.
static size_t wireLen(uint8_t s) {
  if (s >= 0xF8 || s == 0xF6) return 1;
  const uint8_t kind = s & 0xF0;
  if (s == 0xF1 || s == 0xF3 || kind == 0xC0 || kind == 0xD0) return 2;
  return 3;
}

static bool usbReads(uint8_t s) { return s < 0xF0 || s == 0xF1 || s >= 0xF8; }

int main() {
  // 1. Every status byte, with data bytes that need masking and data bytes that don't.
  size_t messages = 0;
  for (int s = 0x00; s <= 0xFF; s++) {
    for (int k = 0; k < 4; k++) {
      const uint8_t d1 = static_cast<uint8_t>(k & 1 ? next() | 0x80 : next() & 0x7F);
      const uint8_t d2 = static_cast<uint8_t>(k & 2 ? next() | 0x80 : next() & 0x7F);
      uint8_t b[3];
      const size_t n = midiMessageBytes(static_cast<uint8_t>(s), d1, d2, b);
      const bool sendable = s >= 0x80 && s != 0xF0 && s != 0xF4 && s != 0xF5 && s != 0xF7;
      if (!sendable) {
        check(n == 0, "a refused status writes no byte");
        continue;
      }
      check(n == wireLen(static_cast<uint8_t>(s)) && b[0] == s, "a short message is its status + its data, no padding");
      for (size_t i = 1; i < n; i++) check(b[i] < 0x80, "data bytes go out 7-bit");
      const std::vector<Ev> got = readStream(std::vector<uint8_t>(b, b + n));
      check(got.size() == 1 && got[0] == expected(static_cast<uint8_t>(s), d1, d2),
            "a short message reads back as itself");
      if (usbReads(static_cast<uint8_t>(s))) {
        UsbMidiPacket m;
        usbMidiMessagePacket(static_cast<uint8_t>(s), d1, d2, &m);
        MidiRxParser<CAP> u;
        Rec r;
        u.usbPacket(m.cin, m.b1, m.b2, m.b3, r);
        check(r.ev == got, "serial and USB-MIDI deliver the same call");
      }
      messages++;
    }
  }

  // 2. Frames of every length: whole up to CAP, one overflow beyond.
  for (size_t len = 2; len <= CAP + 40; len++) {
    std::vector<uint8_t> wire;
    const std::vector<uint8_t> f = frame(len);
    send(wire, f);
    const std::vector<Ev> got = readStream(wire);
    check(got.size() == 1, "one frame, one call");
    if (len <= CAP) {
      check(got[0].kind == 1 && got[0].bytes == f, "a frame reads back whole");
    } else {
      check(got[0].kind == 2, "a frame past CAP is one overflow");
    }
  }

  // 3. A random stream as the ring sends it: items back to back, each whole.
  const uint8_t sends[] = {0xB0, 0xB5, 0xBF, 0x90, 0x80, 0xC3, 0xD1, 0xE2, 0xA4,
                           0xF1, 0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF};
  std::vector<uint8_t> wire;
  std::vector<Ev> want;
  for (int i = 0; i < 20000; i++) {
    std::vector<uint8_t> item;
    if (next() % 8 == 0) {
      item = frame(2 + next() % 300);
      want.push_back({1, item});
    } else {
      const uint8_t s = sends[next() % sizeof(sends)];
      item = {s, static_cast<uint8_t>(next()), static_cast<uint8_t>(next())};
      want.push_back(expected(s, item[1], item[2]));
    }
    send(wire, item);
  }
  check(readStream(wire) == want, "a mixed stream reads back item for item");

  std::printf("serial_link_test OK (%zu messages, frames 2..%zu B, %zu-byte mixed stream)\n",
              messages, CAP + 40, wire.size());
  return 0;
}
