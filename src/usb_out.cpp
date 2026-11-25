#include "usb_out.h"

#include <USB.h>
#include <USBMIDI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "nowde_config.h"
#include "nowde_state.h"
#include "usb_midi_packets.h"

USBMIDI MIDI;   // the one TinyUSB MIDI interface: begun, pumped and read from this file only

namespace {
constexpr size_t MIDI_QUEUE_PACKETS = 1024;  // RUNNING_STATE bursts are ~120 packets per slave chunk
constexpr unsigned long USB_STALL_DROP_MS = 200;

QueueHandle_t midiQueue = nullptr;
SemaphoreHandle_t frameLock = nullptr;
}  // namespace

void usbOutInit() {
  if (!midiQueue) {
    midiQueue = xQueueCreate(MIDI_QUEUE_PACKETS, sizeof(midiEventPacket_t));
  }
  if (!frameLock) {
    frameLock = xSemaphoreCreateMutex();
  }
}

void usbOutDescriptors(const char* productName) {
  USB.VID(0x303A);
  USB.PID(0x8000);
  USB.productName(productName);
  USB.manufacturerName("Hemisphere");
}

void usbOutMidiBegin() {
  MIDI.begin();
}

void usbOutStart() {
  USB.begin();
}

bool usbInRead(uint8_t* cin, uint8_t* b1, uint8_t* b2, uint8_t* b3) {
  midiEventPacket_t packet;
  if (!MIDI.readPacket(&packet)) {
    return false;
  }
  *cin = packet.header & 0x0F;
  *b1 = packet.byte1;
  *b2 = packet.byte2;
  *b3 = packet.byte3;
  return true;
}

// One USB-MIDI packet into the queue (false = queue full). TinyUSB's packet type stays in this
// file: everything else hands plain MIDI bytes (Nowde 3, #t-050 ①).
static bool usbOutMidi(const midiEventPacket_t& packet) {
  if (!midiQueue) {
    return false;
  }
  if (xPortInIsrContext()) {
    BaseType_t woken = pdFALSE;
    return xQueueSendFromISR(midiQueue, &packet, &woken) == pdTRUE;
  }
  return xQueueSend(midiQueue, &packet, 0) == pdTRUE;
}

bool usbOutSysEx(const uint8_t* frame, size_t len) {
  usbOutLock();
  bool ok = usbMidiSysExPackets(frame, len, [](uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3) {
    midiEventPacket_t p = {};
    p.header = cin; p.byte1 = b1; p.byte2 = b2; p.byte3 = b3;
    return usbOutMidi(p);
  });
  usbOutUnlock();
  return ok;
}

bool usbOutMessage(uint8_t status, uint8_t d1, uint8_t d2) {
  UsbMidiPacket m;
  if (!usbMidiMessagePacket(status, d1, d2, &m)) {
    return false;
  }
  midiEventPacket_t p = {};
  p.header = m.cin; p.byte1 = m.b1; p.byte2 = m.b2; p.byte3 = m.b3;
  return usbOutMidi(p);
}

void usbOutLock() {
  if (frameLock && !xPortInIsrContext()) {
    xSemaphoreTake(frameLock, portMAX_DELAY);
  }
}

void usbOutUnlock() {
  if (frameLock && !xPortInIsrContext()) {
    xSemaphoreGive(frameLock);
  }
}

void usbOutPump() {
  static unsigned long stalledSince = 0;   // first failed write of the current stall, 0 = flowing
  midiEventPacket_t packet;
  int budget = 64;   // per call (the MIDI task runs every 1 ms): 64 packets = 256 B, plenty
  while (budget-- > 0 && midiQueue && xQueueReceive(midiQueue, &packet, 0) == pdTRUE) {
    if (MIDI.writePacket(&packet)) {
      stalledSince = 0;
      continue;
    }
    // TinyUSB FIFO full. Momentary (a RUNNING_STATE burst, the host draining): keep order and
    // retry next pump. Lasting (no host reads the port): everything queued is stale — MTC,
    // CC repeats — and would replay as a ghost stream when a host opens the port. Drop it.
    unsigned long now = millis();
    if (stalledSince == 0) {
      stalledSince = now;
      xQueueSendToFront(midiQueue, &packet, 0);
    } else if (now - stalledSince > USB_STALL_DROP_MS) {
      xQueueReset(midiQueue);
    } else {
      xQueueSendToFront(midiQueue, &packet, 0);
    }
    break;
  }
}
