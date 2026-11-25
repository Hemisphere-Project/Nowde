#pragma once

#include <Arduino.h>

#include "nowde_config.h"   // NOWDE_LOG_SYSEX (it includes this header back; #pragma once on both)

// ============= THE LOG ON THE HOST LINK (Nowde 3, #t-050 ②) =============
// Where a board has no spare port for its log (the AtomS3's single USB-C; a serial-linked node,
// whose one port IS the host link), the log travels as SysEx LOG frames (0x31) through hostLinkSend,
// only while a host asked for it (SET_LOG 0x0A). Link-agnostic: the same frames on USB-MIDI,
// a UART or a CDC.
//
// The AtomS3's composite CDC exists for flashing (PlatformIO's 1200-bps touch) but carries NO
// data: bench 2026-09-04 showed the prebuilt TinyUSB/DWC2 losing IN transfers as soon as the CDC
// and the MIDI endpoints were both active (log silent until reboot, or the whole device dropped
// by the hub). Build with -DNOWDE_LOG_CDC to get the log on the CDC instead (bench debugging only).

void hostLogInit();                // before anything logs (host_link's hostLinkInit calls it)
void hostLogPump();                // MIDI task only (hostLinkPump): complete lines -> LOG frames
void hostLogEnable(bool on);       // SET_LOG from the host
bool hostLogEnabled();

#if NOWDE_LOG_SYSEX
// Print sink for DEBUG_SERIAL: bytes go to a ring buffer; the pump turns complete lines into LOG
// frames. Until a host enables the log, the ring keeps the first 8 KB since boot (the boot banner
// is delivered late, not lost).
class HostLog : public Print {
 public:
  void begin();
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  int availableForWrite() override;
  void flush() override {}
};
extern HostLog hostLog;
#endif
