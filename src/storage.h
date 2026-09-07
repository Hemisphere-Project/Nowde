#pragma once

#include <Arduino.h>

void saveLayerToEEPROM(const char* layer);
String loadLayerFromEEPROM();
void saveRoleToEEPROM(uint8_t role);
uint8_t loadRoleFromEEPROM();
void clearEEPROM();

// 2.0.1 mesh-resync self-heal: a persisted count of consecutive auto-reboots, so a poisoned
// mesh cannot boot-loop. Read at boot, incremented just before an auto-reboot, cleared once
// the node regains a real lock. Survives the reboot (millis() does not).
uint8_t loadResyncRebootCount();
void saveResyncRebootCount(uint8_t count);
