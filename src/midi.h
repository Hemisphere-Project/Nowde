#pragma once

#include <Arduino.h>
#include <USBMIDI.h>

void midiInit();
void midiSendCC100(uint8_t value);
void midiSendTimeCode(uint32_t positionMs);       // MTC quarter-frames (8 pieces)
void midiSendFullFrame(uint32_t positionMs);      // MTC full-frame SysEx (v2)
void midiSendStart();                             // MIDI real-time Start 0xFA (v2)
void midiSendStop();                              // MIDI real-time Stop 0xFC (v2)
void midiProcess();
void midiWritePacket(midiEventPacket_t& packet);
bool midiReadPacket(midiEventPacket_t* packet);
