#pragma once

#include <Arduino.h>
#include <USBMIDI.h>

#include "nowde_config.h"   // NOWDE_LOG_SYSEX (it includes this header back; #pragma once on both)

// ============= SINGLE USB WRITER, SINGLE ACTIVE ENDPOINT =============
// TinyUSB is driven from ONE task only: the MIDI task (core 0) pumps the queue below into the
// MIDI IN endpoint. Every other context (the ESP-NOW receive callback in the WiFi task, the
// ESP-NOW task on core 1, setup()) only enqueues and never blocks.
//
// The composite CDC interface exists for flashing (PlatformIO's 1200-bps touch) but carries NO
// data: bench 2026-09-04 showed the prebuilt TinyUSB/DWC2 losing IN transfers as soon as the CDC
// and the MIDI endpoints were both active (log silent until reboot, or the whole device dropped
// by the hub). Logs therefore travel over MIDI as SysEx LOG frames (0x31), only while a host
// asked for them (SET_LOG 0x0A). Build with -DNOWDE_LOG_CDC to get the log on the CDC instead
// (bench debugging only).

void usbOutInit();                                 // before anything logs or sends
// Queue one complete F0..F7 frame as USB-MIDI packets (CIN 4, then 5/6/7 on the last), contiguous
// in the queue. The ONE SysEx packetizer: every frame to the host goes through here (Nowde 3's
// HostLink::send). Queue full mid-frame: stops there, the host parser drops the unterminated frame.
bool usbOutSysEx(const uint8_t* frame, size_t len);
// Queue one short MIDI message (channel voice, MTC quarter frame, real-time) -- plain MIDI bytes,
// status first; the packet is the backend's business (usb_midi_packets.h). false = queue full or
// not a short message.
bool usbOutMessage(uint8_t status, uint8_t d1 = 0, uint8_t d2 = 0);
void usbOutLock();                                 // hold while enqueueing a multi-packet message
void usbOutUnlock();                               //   so it stays contiguous in the queue
void usbOutPump();                                 // MIDI task only: drain to TinyUSB
void usbOutLogEnable(bool on);                     // SET_LOG from the host
bool usbOutLogEnabled();

#if NOWDE_LOG_SYSEX
// Print sink for DEBUG_SERIAL where the log rides the host link: bytes go to a ring buffer; the pump turns
// complete lines into LOG frames. Until a host enables the log, the ring keeps the first 8 KB
// since boot (the boot banner is delivered late, not lost).
class UsbLog : public Print {
 public:
  void begin();
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  int availableForWrite() override;
  void flush() override {}
};
extern UsbLog usbLog;
#endif
