#pragma once

#include <Arduino.h>

void saveLayerToEEPROM(const char* layer);
String loadLayerFromEEPROM();
void clearEEPROM();
