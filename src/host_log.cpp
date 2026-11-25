#include "host_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

#include "host_link.h"
#include "nowde_config.h"
#include "nowde_state.h"   // USBSerial (composite builds)

namespace {
#if NOWDE_LOG_SYSEX
constexpr size_t LOG_RING_BYTES = 8192;
constexpr size_t LOG_LINE_MAX = 100;         // one LOG frame per line, capped

RingbufHandle_t logRing = nullptr;
bool logEnabled = false;
bool logHistory = true;      // keep the backlog until the first host enables the log
char lineBuf[LOG_LINE_MAX];
size_t lineLen = 0;

// F0 7D 31 <ascii> F7, one message on the host link
void sendLogFrame(const char* text, size_t len) {
  uint8_t msg[LOG_LINE_MAX + 4];
  size_t n = 0;
  msg[n++] = SYSEX_START;
  msg[n++] = SYSEX_MANUFACTURER_ID;
  msg[n++] = SYSEX_CMD_LOG;
  for (size_t i = 0; i < len; i++) {
    msg[n++] = static_cast<uint8_t>(text[i]) & 0x7F;
  }
  msg[n++] = SYSEX_END;
  hostLinkSend(msg, n);   // link full: the line is lost (USB-MIDI: the host drops the truncated frame)
}
#endif
}  // namespace

void hostLogInit() {
#if NOWDE_LOG_SYSEX
  if (!logRing) {
    logRing = xRingbufferCreate(LOG_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
  }
#endif
}

void hostLogPump() {
#if NOWDE_LOG_SYSEX
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
#if defined(NOWDE_LOG_CDC) && NOWDE_USB_COMPOSITE
      if (static_cast<bool>(USBSerial)) USBSerial.write(b, n);
#else
      for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == '\n' || lineLen >= LOG_LINE_MAX) {
          if (lineLen > 0) sendLogFrame(lineBuf, lineLen);
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
#endif
}

void hostLogEnable(bool on) {
#if NOWDE_LOG_SYSEX
  logEnabled = on;
  logHistory = false;   // from now on: live while enabled, discarded while disabled
#else
  (void)on;
#endif
}

bool hostLogEnabled() {
#if NOWDE_LOG_SYSEX
  return logEnabled;
#else
  return false;
#endif
}

#if NOWDE_LOG_SYSEX
HostLog hostLog;

void HostLog::begin() {
#if NOWDE_USB_COMPOSITE
  USBSerial.setTxTimeoutMs(5);
  USBSerial.begin();     // the interface must exist for the 1200-bps flash touch
#endif
}

size_t HostLog::write(uint8_t c) {
  return write(&c, 1);
}

size_t HostLog::write(const uint8_t* buffer, size_t size) {
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

int HostLog::availableForWrite() {
  return logRing ? static_cast<int>(xRingbufferGetCurFreeSize(logRing)) : 0;
}
#endif
