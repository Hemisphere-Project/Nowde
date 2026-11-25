#include "host_link.h"

#include "host_log.h"
#include "midi_rx.h"
#include "nowde_config.h"
#include "nowde_state.h"   // hostResumed
#include "usb_midi_packets.h"   // the short-message validator, shared by every link

namespace {
constexpr size_t SYSEX_BUFFER_SIZE = 512;  // Large enough for RUNNING_STATE payloads (~350B)
MidiRxParser<SYSEX_BUFFER_SIZE> rx;
unsigned long lastHostRxTime = 0;
}  // namespace

#if NOWDE_HOST_LINK == NOWDE_LINK_USB_MIDI
#include "usb_out.h"

// The USB-MIDI backend: usb_out.* queues, pumps and reads TinyUSB; this file sees CIN + bytes only.

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

#else  // ===== serial backends: Uart (HardwareSerial) / UsbSerial (the S3's hardware CDC) =====
// The same bytes as USB-MIDI's payload (the rail's wire contract): SysEx frames F0..F7 and short
// messages, status first, no running status out. Every sender only writes a byte ring (never
// blocks: a full ring drops the whole message); the MIDI task's pump moves it to the port.
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>

namespace {
#if NOWDE_HOST_LINK == NOWDE_LINK_UART
HardwareSerial& port = Serial0;   // UART0: the board's USB-UART bridge (DevKitC UART port, Atom)
#elif NOWDE_HOST_LINK == NOWDE_LINK_USB_CDC
HWCDC port;   // the S3's native port as hardware CDC, no TinyUSB (the core declares
             // HWCDCSerial only with CDC-on-boot, which would make it Serial: ours instead)
#else
#error "NOWDE_HOST_LINK: unknown link"
#endif
constexpr size_t TX_RING_BYTES = 8192;           // RUNNING_STATE bursts stay whole
constexpr size_t PORT_RX_BYTES = 2048;           // HWCDC drops bursted RX from its default 256 B ring
constexpr unsigned long TX_STALL_DROP_MS = 200;  // the USB-MIDI rule: a port nobody drains goes stale
RingbufHandle_t txRing = nullptr;
SemaphoreHandle_t frameLock = nullptr;

bool ringSend(const uint8_t* bytes, size_t len) {
  if (!txRing) {
    return false;
  }
  if (xPortInIsrContext()) {
    BaseType_t woken = pdFALSE;
    return xRingbufferSendFromISR(txRing, bytes, len, &woken) == pdTRUE;
  }
  return xRingbufferSend(txRing, bytes, len, 0) == pdTRUE;   // all or nothing, never waits
}
}  // namespace

void hostLinkInit() {
  if (!txRing) {
    txRing = xRingbufferCreate(TX_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
  }
  if (!frameLock) {
    frameLock = xSemaphoreCreateMutex();
  }
  port.setRxBufferSize(PORT_RX_BYTES);   // before begin(), or it does not take
#if NOWDE_HOST_LINK == NOWDE_LINK_UART
  port.setTxBufferSize(1024);
  port.begin(NOWDE_HOST_BAUD);           // 8N1
#else
  port.setTxTimeoutMs(0);                // never wait on a host that is not reading
  port.begin();
#endif
  hostLogInit();
}

bool hostLinkSend(const uint8_t* bytes, size_t len) {
  if (!bytes || len == 0) {
    return false;
  }
  if (bytes[0] == 0xF0) {
    hostLinkLock();
    bool ok = ringSend(bytes, len);
    hostLinkUnlock();
    return ok;
  }
  return hostLinkSendMessage(bytes[0], len > 1 ? bytes[1] : 0, len > 2 ? bytes[2] : 0);
}

bool hostLinkSendMessage(uint8_t status, uint8_t d1, uint8_t d2) {
  UsbMidiPacket m;   // the same validation and masking as the USB-MIDI link
  if (!usbMidiMessagePacket(status, d1, d2, &m)) {
    return false;
  }
  const size_t len = (m.cin == 0x0F || m.cin == 0x05) ? 1
                   : (m.cin == 0x02 || m.cin == 0x0C || m.cin == 0x0D) ? 2 : 3;
  const uint8_t bytes[3] = {m.b1, m.b2, m.b3};
  return ringSend(bytes, len);
}

void hostLinkLock() {
  if (frameLock && !xPortInIsrContext()) {
    xSemaphoreTake(frameLock, portMAX_DELAY);
  }
}

void hostLinkUnlock() {
  if (frameLock && !xPortInIsrContext()) {
    xSemaphoreGive(frameLock);
  }
}

void hostLinkPump() {
  static unsigned long stalledSince = 0;
  if (txRing) {
    int room = port.availableForWrite();
    if (room <= 0) {
      unsigned long now = millis();
      if (stalledSince == 0) {
        stalledSince = now;
      } else if (now - stalledSince > TX_STALL_DROP_MS) {
        size_t n = 0;           // stale: drop what is queued (no ghost stream when a host opens)
        void* item;
        while ((item = xRingbufferReceiveUpTo(txRing, &n, 0, TX_RING_BYTES)) != nullptr) {
          vRingbufferReturnItem(txRing, item);
        }
      }
    } else {
      stalledSince = 0;
      while (room > 0) {
        size_t n = 0;
        void* item = xRingbufferReceiveUpTo(txRing, &n, 0, static_cast<size_t>(room));
        if (!item) {
          break;
        }
        port.write(static_cast<const uint8_t*>(item), n);
        vRingbufferReturnItem(txRing, item);
        room -= static_cast<int>(n);
      }
    }
  }
  hostLogPump();
}

void hostLinkPoll(HostLinkSink& sink) {
  int budget = 1024;   // per 1 ms call: 921600 baud is ~92 bytes/ms
  while (budget-- > 0 && port.available() > 0) {
    int c = port.read();
    if (c < 0) {
      break;
    }
    if (!hostLinked()) {
      hostResumed = true;       // first byte after a silence: the host may be probing our role
    }
    lastHostRxTime = millis();
    rx.streamByte(static_cast<uint8_t>(c), sink);
  }
}
#endif

bool hostLinked() {
  return lastHostRxTime != 0 && (millis() - lastHostRxTime) < HOST_LINK_TIMEOUT_MS;
}
