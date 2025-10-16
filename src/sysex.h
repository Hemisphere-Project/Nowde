#pragma once

#include <Arduino.h>

void sendHello();
void handleSysExMessage(const uint8_t* data, uint8_t length);
