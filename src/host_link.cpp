#include "host_link.h"

#include "host_log.h"
#include "midi_rx.h"
#include "nowde_config.h"
#include "nowde_state.h"   // hostResumed
#include "usb_out.h"

// The USB-MIDI backend: usb_out.* queues, pumps and reads TinyUSB; this file sees CIN + bytes only.
namespace {
constexpr size_t SYSEX_BUFFER_SIZE = 512;  // Large enough for RUNNING_STATE payloads (~350B)
MidiRxParser<SYSEX_BUFFER_SIZE> rx;
unsigned long lastHostRxTime = 0;
}  // namespace

void hostLinkInit() {
  usbOutInit();
  hostLogInit();
}

bool hostLinkSend(const uint8_t* bytes, size_t len) {
  if (!bytes || len == 0) {
    return false;
  }
  if (bytes[0] == 0xF0) {
    return usbOutSysEx(bytes, len);
  }
  return usbOutMessage(bytes[0], len > 1 ? bytes[1] : 0, len > 2 ? bytes[2] : 0);
}

bool hostLinkSendMessage(uint8_t status, uint8_t d1, uint8_t d2) {
  return usbOutMessage(status, d1, d2);
}

void hostLinkLock() {
  usbOutLock();
}

void hostLinkUnlock() {
  usbOutUnlock();
}

void hostLinkPump() {
  usbOutPump();
  hostLogPump();   // after the link's own pump, as before: its frames go out next pump
}

void hostLinkPoll(HostLinkSink& sink) {
  uint8_t cin, b1, b2, b3;
  while (usbInRead(&cin, &b1, &b2, &b3)) {
    if (!hostLinked()) {
      hostResumed = true;       // first packet after a silence: the host may be probing our role
    }
    lastHostRxTime = millis();  // anything from the host counts as a heartbeat
    rx.usbPacket(cin, b1, b2, b3, sink);
  }
}

bool hostLinked() {
  return lastHostRxTime != 0 && (millis() - lastHostRxTime) < HOST_LINK_TIMEOUT_MS;
}
