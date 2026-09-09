#pragma once

#include <Arduino.h>

void saveLayerToEEPROM(const char* layer);
String loadLayerFromEEPROM();
void saveRoleToEEPROM(uint8_t role);
uint8_t loadRoleFromEEPROM();
// 2.0.3: the long-range PHY switch (SET_LR); absent key = the build default NOWDE_WIFI_LR.
void saveLrToEEPROM(bool on);
bool loadLrFromEEPROM();
void clearEEPROM();

// 2.0.1 mesh-resync self-heal: a persisted count of consecutive auto-reboots, so a poisoned
// mesh cannot boot-loop. Read at boot, incremented just before an auto-reboot, cleared once
// the node regains a real lock. Survives the reboot (millis() does not).
uint8_t loadResyncRebootCount();
void saveResyncRebootCount(uint8_t count);
