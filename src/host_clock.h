#pragma once

#include <Arduino.h>

// v2.1 — generic MIDI in on a master node.
//
// Any MIDI host (a DAW, QLab, Millumin without the Bridge, a keyboard) can drive the
// mesh with plain MIDI instead of the 0x7D SysEx: MTC quarter-frames / full-frame give
// the position, CC#100 the media index, Start / Continue / Stop the transport. The node
// normalises that into the same MediaSyncPacket stream the SysEx path produces, on its
// own layer (SET_LOCAL_LAYER; "*" = every slave). SysEx MEDIA_SYNC keeps priority: while
// a host sends it, MIDI in is ignored.

void hostClockOnQuarterFrame(uint8_t data);                  // 0xF1 nn
void hostClockOnFullFrame(uint8_t hh, uint8_t mm, uint8_t ss, uint8_t ff);
void hostClockOnControlChange(uint8_t channel, uint8_t cc, uint8_t value);
void hostClockOnRealtime(uint8_t status);                    // 0xFA / 0xFB / 0xFC
void hostClockNoteSysExSync();                               // called on every SysEx MEDIA_SYNC
void hostClockTick();                                        // from the ESP-NOW task, every 10 ms

bool hostClockActive();                                      // MIDI in is the current source
uint8_t hostClockIndex();
bool hostClockRunning();
