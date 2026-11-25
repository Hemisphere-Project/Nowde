#pragma once

#include <stddef.h>
#include <stdint.h>

// Nowde 3 seam (nowde#t-050 ①): the USB-MIDI side of the host link, as pure logic -- no Arduino,
// no TinyUSB, no FreeRTOS -- so tools/usb_midi_packets_test.cpp runs this same header on the host.
//
// One complete SysEx frame (F0 .. F7) becomes 4-byte USB-MIDI event packets: CIN 0x4 (SysEx
// starts or continues, 3 bytes) while more than three bytes remain, then CIN 0x7 / 0x6 / 0x5
// (SysEx ends with 3 / 2 / 1 bytes) on the last one, unused bytes zero. Before 2.4-rc + Nowde 3
// the firmware carried six hand-rolled copies of this; usb_out's usbOutSysEx() is now the only
// caller, and the serial backends (UART, CDC) send the frame bytes as they are.
//
// `emit(cin, b1, b2, b3)` returns false when the packet could not be queued: the frame stops
// there, and the host parser drops the unterminated frame at the next F0.
template <typename Emit>
inline bool usbMidiSysExPackets(const uint8_t* frame, size_t len, Emit emit) {
  size_t pos = 0;
  while (pos < len) {
    size_t remaining = len - pos;
    uint8_t cin, b1 = frame[pos], b2 = 0, b3 = 0;
    if (remaining > 3) {
      cin = 0x04; b2 = frame[pos + 1]; b3 = frame[pos + 2]; pos += 3;
    } else if (remaining == 3) {
      cin = 0x07; b2 = frame[pos + 1]; b3 = frame[pos + 2]; pos += 3;
    } else if (remaining == 2) {
      cin = 0x06; b2 = frame[pos + 1]; pos += 2;
    } else {
      cin = 0x05; pos += 1;
    }
    if (!emit(cin, b1, b2, b3)) {
      return false;
    }
  }
  return true;
}
