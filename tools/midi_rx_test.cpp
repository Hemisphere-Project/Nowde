// Host test of src/midi_rx.h -- the host link's receive parse (nowde#t-050 ①).
// tools/selftest.py compiles and runs it when a host C++ compiler is on PATH. It pins:
//  - usbPacket() makes the same calls the inline CIN parse in midiProcess() made before Nowde 3,
//    on frames of every length and on a seeded random packet stream (the model below is that code,
//    with ONE deliberate change: an F0 also clears a pending overflow, so a dropped oversize frame
//    with no F7 no longer takes the next frame down with it);
//  - streamByte() reads plain MIDI -- running status, real-time inside SysEx and between data
//    bytes -- into the same calls as the USB-MIDI packets of the same messages.
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

// midiProcess()'s packet loop as of 2.4-rc1, the calls it made recorded as events.
struct OldModel {
  uint8_t buf[CAP];
  size_t idx = 0;
  bool in = false, overflow = false;
  void packet(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3, Rec& r) {
    if (cin == 0x2 && b1 == 0xF1) { r.message(0xF1, b2, 0); return; }
    if ((cin == 0xF || cin == 0x5) && b1 >= 0xF8) { r.message(b1, 0, 0); return; }
    if (cin >= 0x8 && cin <= 0xE) { r.message(b1, b2, b3); return; }
    if (cin >= 0x4 && cin <= 0x7) {
      int n = cin == 0x4 ? 3 : cin == 0x5 ? 1 : cin == 0x6 ? 2 : 3;
      if (b1 == 0xF0) { in = true; idx = 0; overflow = false; }   // the one change: overflow = false
      if (in) {
        const uint8_t bytes[3] = {b1, b2, b3};
        for (int i = 0; i < n; i++) {
          if (idx < CAP) buf[idx++] = bytes[i]; else overflow = true;
          if (bytes[i] == 0xF7) {
            if (overflow) r.sysexOverflow(CAP); else r.sysex(buf, idx);
            in = false; idx = 0; overflow = false;
            break;
          }
        }
      }
    }
  }
};

static uint32_t rng = 0x37u;
static uint32_t next() { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7FFF; }

static std::vector<uint8_t> frame(size_t len) {
  std::vector<uint8_t> f(len);
  f[0] = 0xF0;
  for (size_t i = 1; i + 1 < len; i++) f[i] = static_cast<uint8_t>(next() & 0x7F);
  f[len - 1] = 0xF7;
  return f;
}

int main() {
  // 1. Frames of every length through the shared packetizer, then the receive parse.
  for (size_t len = 2; len <= CAP + 40; len++) {
    std::vector<uint8_t> f = frame(len);
    MidiRxParser<CAP> p;
    Rec got, want;
    OldModel old;
    usbMidiSysExPackets(f.data(), f.size(), [&](uint8_t c, uint8_t a, uint8_t b, uint8_t d) {
      p.usbPacket(c, a, b, d, got);
      old.packet(c, a, b, d, want);
      return true;
    });
    check(got.ev == want.ev, "usbPacket == the old parse, one frame");
    check(got.ev.size() == 1, "one frame -> one call");
    if (len <= CAP) {
      check(got.ev[0].kind == 1 && got.ev[0].bytes == f, "frame delivered byte for byte");
    } else {
      check(got.ev[0].kind == 2, "oversize frame -> overflow, dropped");
    }
  }

  // 2. A seeded random packet stream: valid short messages, SysEx packets with random bytes
  //    (F0 / F7 / status bytes mixed in), garbage CINs.
  {
    MidiRxParser<CAP> p;
    OldModel old;
    Rec got, want;
    for (int i = 0; i < 200000; i++) {
      uint8_t cin = static_cast<uint8_t>(next() & 0x0F), b[3];
      for (int k = 0; k < 3; k++) {
        uint32_t r = next() % 100;
        b[k] = r < 6 ? 0xF0 : r < 12 ? 0xF7 : r < 15 ? static_cast<uint8_t>(0x80 | (next() & 0x7F))
                                            : static_cast<uint8_t>(next() & 0x7F);
      }
      if (cin >= 0x8 && cin <= 0xE) b[0] = static_cast<uint8_t>((cin << 4) | (next() & 0x0F));
      if (cin == 0xF && (next() & 1)) b[0] = static_cast<uint8_t>(0xF8 + (next() & 7));
      if (cin == 0x4 && next() % 40 == 0) {   // now and then a long run, to reach the overflow
        for (int k = 0; k < 200; k++) {
          p.usbPacket(0x4, 0x11, 0x22, 0x33, got);
          old.packet(0x4, 0x11, 0x22, 0x33, want);
        }
      }
      p.usbPacket(cin, b[0], b[1], b[2], got);
      old.packet(cin, b[0], b[1], b[2], want);
    }
    check(got.ev == want.ev, "usbPacket == the old parse, random stream");
    size_t kinds[3] = {0, 0, 0};
    for (const Ev& e : got.ev) kinds[e.kind]++;
    check(kinds[0] > 1000 && kinds[1] > 1000 && kinds[2] > 0, "the random stream reached all three calls");
    std::printf("midi_rx usb: %zu messages, %zu frames, %zu overflows -- same calls as 2.4-rc1\n",
                kinds[0], kinds[1], kinds[2]);
  }

  // 3. The stream entry: the same messages as plain bytes read into the same calls as their
  //    USB-MIDI packets -- with running status, and real-time bytes dropped in anywhere.
  {
    std::vector<std::vector<uint8_t>> msgs = {
        {0xB0, 100, 3}, {0xB0, 7, 99}, {0xB0, 1, 2}, {0x90, 60, 127}, {0x90, 61, 0},
        {0xC2, 5}, {0xD3, 77}, {0xE0, 0, 64}, {0xF1, 0x23}, {0xF1, 0x31}, {0xFA}, {0xF8},
        frame(2), frame(9), frame(CAP), {0xB5, 64, 0}, {0xB5, 65, 127}, {0xFC}, frame(300)};
    Rec viaUsb, viaStream;
    MidiRxParser<CAP> u, s;
    std::vector<uint8_t> bytes;
    uint8_t running = 0;
    for (const std::vector<uint8_t>& m : msgs) {
      if (m[0] == 0xF0) {
        usbMidiSysExPackets(m.data(), m.size(), [&](uint8_t c, uint8_t a, uint8_t b, uint8_t d) {
          u.usbPacket(c, a, b, d, viaUsb);
          return true;
        });
        for (size_t i = 0; i < m.size(); i++) {
          bytes.push_back(m[i]);
          if (i % 97 == 50) bytes.push_back(0xFE);   // active sensing inside the frame
        }
        running = 0;
        continue;
      }
      UsbMidiPacket pk;
      check(usbMidiMessagePacket(m[0], m.size() > 1 ? m[1] : 0, m.size() > 2 ? m[2] : 0, &pk),
            "test message packs");
      u.usbPacket(pk.cin, pk.b1, pk.b2, pk.b3, viaUsb);
      if (m[0] >= 0xF8) { bytes.push_back(m[0]); continue; }
      if (m[0] != running || m[0] >= 0xF0) bytes.push_back(m[0]);
      running = m[0] < 0xF0 ? m[0] : 0;
      for (size_t i = 1; i < m.size(); i++) {
        bytes.push_back(m[i]);
        if (i == 1 && m.size() == 3) bytes.push_back(0xFE);   // active sensing between data bytes
      }
    }
    for (uint8_t b : bytes) s.streamByte(b, viaStream);
    // the stream also delivers the interleaved real-time bytes (0xFE, in no message above):
    // count them, then drop them before comparing
    Rec streamNoRt;
    size_t interleaved = 0;
    for (const Ev& e : viaStream.ev) {
      if (e.kind == 0 && e.bytes[0] == 0xFE) interleaved++; else streamNoRt.ev.push_back(e);
    }
    size_t inserted = 0;
    for (uint8_t b : bytes) inserted += b == 0xFE;
    check(interleaved == inserted && inserted > 5, "stream: every interleaved real-time byte delivered");
    check(streamNoRt.ev.size() == viaUsb.ev.size(), "stream: as many calls as the packets");
    for (size_t i = 0; i < viaUsb.ev.size(); i++) {
      const Ev& a = viaUsb.ev[i];
      const Ev& b = streamNoRt.ev[i];
      if (a.kind == 0) {
        // USB-MIDI carries 3 bytes for every short message; the stream fills absent ones with 0
        check(b.kind == 0 && a.bytes[0] == b.bytes[0] && a.bytes[1] == b.bytes[1], "stream: message");
        uint8_t k = a.bytes[0] & 0xF0;
        bool two = a.bytes[0] < 0xF0 && k != 0xC0 && k != 0xD0;
        check(!two || a.bytes[2] == b.bytes[2], "stream: second data byte");
      } else {
        check(a == b, "stream: frame byte for byte");
      }
    }
  }

  // 4. Stream edge cases.
  {
    MidiRxParser<CAP> s;
    Rec r;
    const uint8_t in[] = {0x10, 0x20,                      // data with no status: dropped
                          0xF0, 0x01, 0x02, 0xB0, 0x07, 0x40,   // status mid-frame: frame dropped, CC read
                          0x08, 0x09,                      // running status carries on
                          0xF7,                            // stray end: ignored
                          0xF1, 0x12, 0x13,                // quarter frame, then a stray data byte
                          0xF0, 0x7D, 0xF0, 0x7D, 0x01, 0xF7};   // F0 again: the first frame dropped
    for (uint8_t b : in) s.streamByte(b, r);
    std::vector<Ev> want = {{0, {0xB0, 0x07, 0x40}}, {0, {0xB0, 0x08, 0x09}}, {0, {0xF1, 0x12, 0}},
                            {1, {0xF0, 0x7D, 0x01, 0xF7}}};
    check(r.ev == want, "stream edge cases");
    check(!s.inSysEx(), "stream: frame closed");
  }

  std::printf("midi_rx: usbPacket matches the 2.4-rc1 parse; streamByte reads the same messages\n");
  return 0;
}
