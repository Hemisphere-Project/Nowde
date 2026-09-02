#pragma once

#include <Arduino.h>

void saveLayerToEEPROM(const char* layer);
String loadLayerFromEEPROM();
void saveRoleToEEPROM(uint8_t role);
uint8_t loadRoleFromEEPROM();
void clearEEPROM();
