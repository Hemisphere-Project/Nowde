#pragma once

#include <Arduino.h>
#include <USBMIDI.h>

void midiInit();
void midiSendCC100(uint8_t value);
void midiSendTimeCode(uint32_t positionMs);
void midiProcess();
void midiWritePacket(midiEventPacket_t& packet);
bool midiReadPacket(midiEventPacket_t* packet);
