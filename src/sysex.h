#pragma once

#include <Arduino.h>

void sendHello();
void handleSysExMessage(const uint8_t* data, uint8_t length);
// 2.4: called every MIDI-task tick after the USB pump; restarts once a validated OTA_END's verdict is out.
void otaRestartPoll();
