#pragma once

#include <Arduino.h>

// ============= HOST LINK (Nowde 3, #t-050 ①) =============
// The one door between the node and its host. Every link carries the same bytes (the rail's wire
// contract): complete SysEx frames F0..F7 and short MIDI messages, status first, never running
// status out (accepted in). Backend today: USB-MIDI (usb_out.* + TinyUSB). The serial links
// (Uart, UsbSerial, #t-050 ②) slot in behind these same calls; nothing above them changes.

// What the host sent, whatever the link: one call per complete message.
struct HostLinkSink {
  virtual void message(uint8_t status, uint8_t d1, uint8_t d2) = 0;   // short message
  virtual void sysex(const uint8_t* frame, size_t len) = 0;           // complete F0..F7 frame
  virtual void sysexOverflow(size_t capacity) = 0;                    // frame dropped: too long

 protected:
  ~HostLinkSink() = default;
};

void hostLinkInit();     // before anything logs or sends (setup)
// send(bytes): one complete message to the host -- an F0..F7 frame, or a short message. false =
// dropped (queue full, or not a message).
bool hostLinkSend(const uint8_t* bytes, size_t len);
bool hostLinkSendMessage(uint8_t status, uint8_t d1 = 0, uint8_t d2 = 0);   // short message
void hostLinkLock();     // hold while sending a group (MTC's 8 quarter frames) so it stays
void hostLinkUnlock();   //   contiguous on the wire
void hostLinkPump();     // MIDI task only: what was sent goes out on the link
// poll(): MIDI task only. Reads what the host sent and hands every complete message to `sink`;
// any byte from the host is a heartbeat for hostLinked().
void hostLinkPoll(HostLinkSink& sink);
bool hostLinked();       // linked(): the host spoke within HOST_LINK_TIMEOUT_MS
