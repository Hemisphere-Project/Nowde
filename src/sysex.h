#pragma once

#include <Arduino.h>

void sendHello();
// SET_ROLE 0x08 and the button's 3 s hold: store, resolve, apply, then HELLO + CONFIG_STATE.
void setStoredRole(uint8_t role);
// 2.4: length is uint16_t -- the USB-MIDI assembler holds up to 512 B (midi.cpp), and a uint8_t
// silently wrapped any frame over 255 B into a short, mis-parsed one.
void handleSysExMessage(const uint8_t* data, uint16_t length);
// 2.4: called every MIDI-task tick after the USB pump; restarts once a validated OTA_END's verdict is out.
void otaRestartPoll();
