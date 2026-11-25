// Host test of src/usb_midi_packets.h -- the one SysEx -> USB-MIDI packetizer (nowde#t-050 ①).
// tools/selftest.py compiles and runs it when a host C++ compiler is on PATH. It pins that the
// shared packetizer puts on the wire exactly the packets the hand-rolled builders it replaced did
// (HELLO, RUNNING_STATE, the MTC full frame, sendSysExBytes and the log frame), for every length.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/usb_midi_packets.h"

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

struct Pkt {
  uint8_t cin, b1, b2, b3;
  bool operator==(const Pkt& o) const { return cin == o.cin && b1 == o.b1 && b2 == o.b2 && b3 == o.b3; }
};

static std::vector<Pkt> shared(const std::vector<uint8_t>& f) {
  std::vector<Pkt> out;
  usbMidiSysExPackets(f.data(), f.size(), [&](uint8_t c, uint8_t a, uint8_t b, uint8_t d) {
    out.push_back({c, a, b, d});
    return true;
  });
  return out;
}

// The rc's HELLO / RUNNING_STATE builder: look for F7 in the next three bytes.
static std::vector<Pkt> oldScanForEnd(const std::vector<uint8_t>& m) {
  std::vector<Pkt> out;
  size_t idx = m.size(), pos = 0;
  while (pos < idx) {
    Pkt p = {0, 0, 0, 0};
    int endPos = -1;
    for (size_t i = 0; i < 3 && pos + i < idx; i++) {
      if (m[pos + i] == 0xF7) { endPos = static_cast<int>(i); break; }
    }
    if (endPos == 0) { p = {0x05, m[pos], 0, 0}; pos += 1; }
    else if (endPos == 1) { p = {0x06, m[pos], m[pos + 1], 0}; pos += 2; }
    else if (endPos == 2) { p = {0x07, m[pos], m[pos + 1], m[pos + 2]}; pos += 3; }
    else {
      p = {0x04, m[pos], static_cast<uint8_t>(pos + 1 < idx ? m[pos + 1] : 0),
           static_cast<uint8_t>(pos + 2 < idx ? m[pos + 2] : 0)};
      pos += 3;
    }
    out.push_back(p);
  }
  return out;
}

int main() {
  unsigned seed = 37;
  auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return static_cast<uint8_t>((seed >> 16) & 0x7F); };

  // Every frame length a node sends (the 512 B assembler is the ceiling), 7-bit payloads.
  for (size_t len = 2; len <= 512; len++) {
    std::vector<uint8_t> f(len);
    f[0] = 0xF0;
    for (size_t i = 1; i + 1 < len; i++) f[i] = rnd();
    f[len - 1] = 0xF7;
    std::vector<Pkt> got = shared(f);
    check(got == oldScanForEnd(f), "same packets as the HELLO / RUNNING_STATE builder");
    check(got.size() == (len + 2) / 3, "ceil(len / 3) packets");
    for (size_t i = 0; i + 1 < got.size(); i++) check(got[i].cin == 0x04, "CIN 4 until the last packet");
    static const uint8_t lastCin[3] = {0x07, 0x05, 0x06};   // by len % 3
    check(got.back().cin == lastCin[len % 3], "the last packet's CIN says how many bytes end the frame");
    std::vector<uint8_t> back;
    for (const Pkt& p : got) {
      const uint8_t n = p.cin == 0x05 ? 1 : p.cin == 0x06 ? 2 : 3;
      back.push_back(p.b1);
      if (n > 1) back.push_back(p.b2);
      if (n > 2) back.push_back(p.b3);
      if (n < 3) check(p.b3 == 0 && (n == 2 || p.b2 == 0), "unused bytes are zero");
    }
    check(back == f, "the packets carry the frame, byte for byte");
  }

  // The MTC full frame (midi.cpp), whose old builder was four fixed packets: 4, 4, 4, 5.
  {
    std::vector<uint8_t> f = {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x61, 0x02, 0x03, 0x04, 0xF7};
    std::vector<Pkt> want = {{0x04, 0xF0, 0x7F, 0x7F}, {0x04, 0x01, 0x01, 0x61},
                             {0x04, 0x02, 0x03, 0x04}, {0x05, 0xF7, 0, 0}};
    check(shared(f) == want, "MTC full frame: the same four packets");
  }

  // A full queue stops the frame where it failed and says so (the log frame's old behaviour).
  {
    std::vector<uint8_t> f(20, 0x11);
    f[0] = 0xF0; f[19] = 0xF7;
    int sent = 0;
    bool ok = usbMidiSysExPackets(f.data(), f.size(), [&](uint8_t, uint8_t, uint8_t, uint8_t) {
      return ++sent < 3;
    });
    check(!ok && sent == 3, "queue full: stop at the failing packet, report false");
  }

  // Short messages: the packets the old builders wrote (midiSendCC, the MTC quarter frame,
  // sendRealtime, midi_event's playRecord), for every status they used and every data value.
  for (int status = 0x80; status <= 0xEF; status++) {
    for (int d = 0; d < 256; d += 5) {
      const uint8_t kind = static_cast<uint8_t>(status & 0xF0);
      UsbMidiPacket m;
      check(usbMidiMessagePacket(static_cast<uint8_t>(status), static_cast<uint8_t>(d),
                                 static_cast<uint8_t>(255 - d), &m), "channel voice is a message");
      // playRecord / midiSendCC
      check(m.cin == (kind >> 4) && m.b1 == status && m.b2 == (d & 0x7F)
                && m.b3 == ((kind == 0xC0 || kind == 0xD0) ? 0 : ((255 - d) & 0x7F)),
            "channel voice: the playRecord / midiSendCC packet");
    }
  }
  for (int piece = 0; piece < 8; piece++) {
    for (int nibble = 0; nibble < 16; nibble++) {
      UsbMidiPacket m;
      const uint8_t qf = static_cast<uint8_t>((piece << 4) | nibble);
      check(usbMidiMessagePacket(0xF1, qf, 0, &m) && m.cin == 0x02 && m.b1 == 0xF1 && m.b2 == qf
                && m.b3 == 0, "MTC quarter frame: CIN 2, F1, the piece");
    }
  }
  for (int rt = 0xF8; rt <= 0xFF; rt++) {
    UsbMidiPacket m;
    check(usbMidiMessagePacket(static_cast<uint8_t>(rt), 0x55, 0x66, &m) && m.cin == 0x0F && m.b1 == rt
              && m.b2 == 0 && m.b3 == 0, "real-time: CIN F, one byte");
  }
  {
    UsbMidiPacket m;
    check(!usbMidiMessagePacket(0xF0, 0, 0, &m) && !usbMidiMessagePacket(0xF7, 0, 0, &m)
              && !usbMidiMessagePacket(0x40, 0, 0, &m), "SysEx bytes and data bytes are not messages");
  }

  std::printf("usb_midi_packets host test OK (frames 2..512 B, short messages)\n");
  return 0;
}
