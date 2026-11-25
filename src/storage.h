#pragma once

#include <Arduino.h>

void saveLayerToEEPROM(const char* layer);
String loadLayerFromEEPROM();
void saveRoleToEEPROM(uint8_t role);
uint8_t loadRoleFromEEPROM();
// 2.0.3: the long-range PHY switch (SET_LR); absent key = the build default NOWDE_WIFI_LR.
void saveLrToEEPROM(uint8_t mode);     // 2.4: NOWDE_LR_* (0 off, 1 250k, 2 500k)
uint8_t loadLrFromEEPROM();
// v2.2: the link-loss policy (SET_LOSS_POLICY); absent key = the build default
// NOWDE_STOP_ON_LINK_LOST. true = stop, false = freewheel.
void saveLossPolicyToEEPROM(bool stop);
bool loadLossPolicyFromEEPROM();
// Nowde 3 (SET_LED 0x13): the status pixel's pin (NOWDE_LED_PIN_NONE = none), brightness 0..127
// and show-dark flag; absent keys leave the caller's defaults (the board profile).
void saveLedToEEPROM(uint8_t pin, uint8_t brightness, bool dark);
void loadLedFromEEPROM(int8_t& pin, uint8_t& brightness, bool& dark);
void clearEEPROM();

// 2.0.1 mesh-resync self-heal: a persisted count of consecutive auto-reboots, so a poisoned
// mesh cannot boot-loop. Read at boot, incremented just before an auto-reboot, cleared once
// the node regains a real lock. Survives the reboot (millis() does not).
uint8_t loadResyncRebootCount();
void saveResyncRebootCount(uint8_t count);
