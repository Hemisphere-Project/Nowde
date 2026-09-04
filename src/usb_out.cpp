#include "usb_out.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>

#include "nowde_config.h"
#include "nowde_state.h"

namespace {
constexpr size_t MIDI_QUEUE_PACKETS = 1024;  // RUNNING_STATE bursts are ~120 packets per slave chunk
constexpr size_t LOG_RING_BYTES = 8192;
constexpr size_t LOG_LINE_MAX = 100;         // one LOG frame per line, capped
constexpr unsigned long USB_STALL_DROP_MS = 200;

QueueHandle_t midiQueue = nullptr;
SemaphoreHandle_t frameLock = nullptr;
#if defined(NOWDE_BOARD_ATOMS3)
RingbufHandle_t logRing = nullptr;
bool logEnabled = false;
bool logHistory = true;      // keep the backlog until the first host enables the log
char lineBuf[LOG_LINE_MAX];
size_t lineLen = 0;

// F0 7D 31 <ascii> F7 as USB-MIDI packets, contiguous in the queue
void enqueueLogFrame(const char* text, size_t len) {
  uint8_t msg[LOG_LINE_MAX + 4];
  size_t n = 0;
  msg[n++] = SYSEX_START;
  msg[n++] = SYSEX_MANUFACTURER_ID;
  msg[n++] = SYSEX_CMD_LOG;
  for (size_t i = 0; i < len; i++) {
    msg[n++] = static_cast<uint8_t>(text[i]) & 0x7F;
  }
  msg[n++] = SYSEX_END;

  usbOutLock();
  size_t pos = 0;
  while (pos < n) {
    midiEventPacket_t p = {};
    size_t remaining = n - pos;
    if (remaining > 3) {
      p.header = 0x04; p.byte1 = msg[pos]; p.byte2 = msg[pos + 1]; p.byte3 = msg[pos + 2]; pos += 3;
    } else if (remaining == 3) {
      p.header = 0x07; p.byte1 = msg[pos]; p.byte2 = msg[pos + 1]; p.byte3 = msg[pos + 2]; pos += 3;
    } else if (remaining == 2) {
      p.header = 0x06; p.byte1 = msg[pos]; p.byte2 = msg[pos + 1]; pos += 2;
    } else {
      p.header = 0x05; p.byte1 = msg[pos]; pos += 1;
    }
    if (!usbOutMidi(p)) {
      break;   // queue full: the line is lost, the host parser drops the truncated frame
    }
  }
  usbOutUnlock();
}

void pumpLog() {
  if (!logRing) {
    return;
  }
  if (!logEnabled && logHistory) {
    return;                      // nobody listens yet: keep the boot log for later
  }
  int chunks = 4;
  while (chunks-- > 0) {
    size_t n = 0;
    void* item = xRingbufferReceiveUpTo(logRing, &n, 0, 128);
    if (!item) {
      break;
    }
    if (logEnabled) {
      const uint8_t* b = static_cast<const uint8_t*>(item);
#if defined(NOWDE_LOG_CDC)
      if (static_cast<bool>(USBSerial)) USBSerial.write(b, n);
#else
      for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == '\n' || lineLen >= LOG_LINE_MAX) {
          if (lineLen > 0) enqueueLogFrame(lineBuf, lineLen);
          lineLen = 0;
          if (c == '\n') continue;
        }
        if (c == '\r' || c < 0x20 || c >= 0x80) continue;   // UTF-8 banner art is dropped
        lineBuf[lineLen++] = static_cast<char>(c);
      }
#endif
    }
    vRingbufferReturnItem(logRing, item);   // disabled after a first enable: discard
  }
}
#endif
}  // namespace

void usbOutInit() {
  if (!midiQueue) {
    midiQueue = xQueueCreate(MIDI_QUEUE_PACKETS, sizeof(midiEventPacket_t));
  }
  if (!frameLock) {
    frameLock = xSemaphoreCreateMutex();
  }
#if defined(NOWDE_BOARD_ATOMS3)
  if (!logRing) {
    logRing = xRingbufferCreate(LOG_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
  }
#endif
}

bool usbOutMidi(const midiEventPacket_t& packet) {
  if (!midiQueue) {
    return false;
  }
  if (xPortInIsrContext()) {
    BaseType_t woken = pdFALSE;
    return xQueueSendFromISR(midiQueue, &packet, &woken) == pdTRUE;
  }
  return xQueueSend(midiQueue, &packet, 0) == pdTRUE;
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
#if defined(NOWDE_BOARD_ATOMS3)
  pumpLog();
#endif
}

void usbOutLogEnable(bool on) {
#if defined(NOWDE_BOARD_ATOMS3)
  logEnabled = on;
  logHistory = false;   // from now on: live while enabled, discarded while disabled
#else
  (void)on;
#endif
}

bool usbOutLogEnabled() {
#if defined(NOWDE_BOARD_ATOMS3)
  return logEnabled;
#else
  return false;
#endif
}

#if defined(NOWDE_BOARD_ATOMS3)
UsbLog usbLog;

void UsbLog::begin() {
  USBSerial.setTxTimeoutMs(5);
  USBSerial.begin();     // the interface must exist for the 1200-bps flash touch
}

size_t UsbLog::write(uint8_t c) {
  return write(&c, 1);
}

size_t UsbLog::write(const uint8_t* buffer, size_t size) {
  if (!logRing || !buffer || size == 0) {
    return 0;
  }
  BaseType_t ok;
  if (xPortInIsrContext()) {
    BaseType_t woken = pdFALSE;
    ok = xRingbufferSendFromISR(logRing, buffer, size, &woken);
  } else {
    ok = xRingbufferSend(logRing, buffer, size, 0);   // full = drop, never wait
  }
  return ok == pdTRUE ? size : 0;
}

int UsbLog::availableForWrite() {
  return logRing ? static_cast<int>(xRingbufferGetCurFreeSize(logRing)) : 0;
}
#endif
