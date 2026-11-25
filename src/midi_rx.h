#pragma once

#include <stddef.h>
#include <stdint.h>

// Nowde 3 seam (nowde#t-050 ①): the receive side of the host link, as pure logic -- no Arduino,
// no TinyUSB -- so tools/midi_rx_test.cpp runs this same header on the host.
//
// What the host sends reaches the firmware as two things only: a short message (channel voice,
// MTC quarter frame, a real-time byte) and a complete SysEx frame (F0 .. F7). Two entry points
// produce them, one per kind of link; a link uses ONE parser instance and ONE entry point:
//
//   usbPacket(cin, b1, b2, b3)  USB-MIDI: one 4-byte event packet. The CIN says what the bytes
//                               are. Exactly the parse midiProcess() did inline before Nowde 3.
//   streamByte(b)               a serial link (UART, USB-CDC -- nowde#t-050 ②): plain MIDI bytes,
//                               running status, real-time bytes anywhere (inside SysEx too).
//
// Sink (any type with these three members):
//   void message(uint8_t status, uint8_t d1, uint8_t d2);  // real-time: d1 = d2 = 0
//   void sysex(const uint8_t* frame, size_t len);          // F0 .. F7, both included
//   void sysexOverflow(size_t capacity);                   // a frame longer than N, dropped at F7
//
// An unterminated frame is dropped at the next F0 (both links) or at the next non-real-time
// status byte (stream only: in USB-MIDI the CIN keeps a short message apart from SysEx data).
template <size_t N>
class MidiRxParser {
 public:
  template <typename Sink>
  void usbPacket(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3, Sink& sink) {
    if (cin == 0x2 && b1 == 0xF1) {                      // MTC quarter frame
      sink.message(0xF1, b2, 0);
      return;
    }
    if ((cin == 0xF || cin == 0x5) && b1 >= 0xF8) {      // single-byte real-time
      sink.message(b1, 0, 0);
      return;
    }
    if (cin >= 0x8 && cin <= 0xE) {                      // channel voice
      sink.message(b1, b2, b3);
      return;
    }
    if (cin < 0x4 || cin > 0x7) {
      return;
    }
    const size_t count = cin == 0x5 ? 1 : (cin == 0x6 ? 2 : 3);
    if (b1 == 0xF0) {
      startFrame();
    }
    if (!inSysEx_) {
      return;
    }
    const uint8_t bytes[3] = {b1, b2, b3};
    for (size_t i = 0; i < count; i++) {
      if (frameByte(bytes[i], sink)) {
        break;                                           // F7: the rest of the packet is padding
      }
    }
  }

  template <typename Sink>
  void streamByte(uint8_t b, Sink& sink) {
    if (b >= 0xF8) {                                     // real-time: never touches any state
      sink.message(b, 0, 0);
      return;
    }
    if (b == 0xF0) {
      status_ = 0;                                       // SysEx cancels running status
      startFrame();
      frameByte(b, sink);
      return;
    }
    if (inSysEx_) {
      if (b < 0x80 || b == 0xF7) {
        frameByte(b, sink);
        return;
      }
      inSysEx_ = false;                                  // a status byte: the frame is dropped
      len_ = 0;
      overflow_ = false;
    }
    if (b == 0xF7) {
      return;                                            // an end with no frame open
    }
    if (b >= 0x80) {
      status_ = b;
      got_ = 0;
      if (b < 0xF0) {
        const uint8_t kind = b & 0xF0;
        need_ = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
      } else if (b == 0xF1 || b == 0xF3) {
        need_ = 1;
      } else if (b == 0xF2) {
        need_ = 2;
      } else {
        status_ = 0;                                     // F6 tune request, or undefined F4/F5
        if (b == 0xF6) {
          sink.message(0xF6, 0, 0);
        }
      }
      return;
    }
    if (status_ == 0) {
      return;                                            // data with no status: dropped
    }
    data_[got_++] = b;
    if (got_ == need_) {
      sink.message(status_, data_[0], need_ == 2 ? data_[1] : 0);
      got_ = 0;
      if (status_ >= 0xF0) {
        status_ = 0;                                     // system common: no running status
      }
    }
  }

  bool inSysEx() const { return inSysEx_; }

 private:
  void startFrame() {
    inSysEx_ = true;
    len_ = 0;
    overflow_ = false;
  }

  // One byte of an open frame. True when it was the F7 that closed it.
  template <typename Sink>
  bool frameByte(uint8_t b, Sink& sink) {
    if (len_ < N) {
      buf_[len_++] = b;
    } else {
      overflow_ = true;                                  // keep reading so we resync at F7
    }
    if (b != 0xF7) {
      return false;
    }
    if (overflow_) {
      sink.sysexOverflow(N);
    } else {
      sink.sysex(buf_, len_);
    }
    inSysEx_ = false;
    len_ = 0;
    overflow_ = false;
    return true;
  }

  uint8_t buf_[N];
  size_t len_ = 0;
  bool inSysEx_ = false;
  bool overflow_ = false;
  uint8_t status_ = 0;   // stream: running status (0 = none)
  uint8_t need_ = 0;
  uint8_t got_ = 0;
  uint8_t data_[2] = {0, 0};
};
