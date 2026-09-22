#include "storage.h"

#include "nowde_config.h"
#include "nowde_state.h"

void saveLayerToEEPROM(const char* layer) {
  preferences.begin("nowde", false);
  preferences.putString("layer", layer);
  preferences.end();
  DEBUG_SERIAL.println("[EEPROM] Layer saved");
}

String loadLayerFromEEPROM() {
  if (!preferences.begin("nowde", true)) {
    DEBUG_SERIAL.println("[EEPROM] No saved data found (first boot)");
    return String(DEFAULT_RECEIVER_LAYER);
  }

  String layer = preferences.getString("layer", DEFAULT_RECEIVER_LAYER);
  preferences.end();

  DEBUG_SERIAL.print("[EEPROM] Loaded layer: ");
  DEBUG_SERIAL.println(layer);

  return layer;
}

void saveRoleToEEPROM(uint8_t role) {
  preferences.begin("nowde", false);
  preferences.putUChar("role", role);
  preferences.end();
  DEBUG_SERIAL.printf("[EEPROM] Role saved: %u\r\n", role);
}

uint8_t loadRoleFromEEPROM() {
  if (!preferences.begin("nowde", true)) {
    return NOWDE_ROLE_DEFAULT;
  }
  uint8_t role = preferences.getUChar("role", NOWDE_ROLE_DEFAULT);
  preferences.end();
  if (role != NOWDE_ROLE_SLAVE && role != NOWDE_ROLE_MASTER) {
    role = NOWDE_ROLE_AUTO;
  }
  return role;
}

// 2.0.3: long-range PHY is a stored switch, not a build. Absent key = the build's default
// (NOWDE_WIFI_LR), so the `atoms3-lr` env still means "LR unless told otherwise".
void saveLrToEEPROM(bool on) {
  preferences.begin("nowde", false);
  preferences.putUChar("lr", on ? 1 : 0);
  preferences.end();
  DEBUG_SERIAL.printf("[EEPROM] LR saved: %u\r\n", on ? 1 : 0);
}

bool loadLrFromEEPROM() {
  if (!preferences.begin("nowde", true)) {
    return NOWDE_WIFI_LR != 0;
  }
  uint8_t v = preferences.getUChar("lr", NOWDE_WIFI_LR ? 1 : 0);
  preferences.end();
  return v != 0;
}

// v2.2: freewheel vs stop on link loss is a stored choice, not a build. Absent key = the build's
// default (NOWDE_STOP_ON_LINK_LOST), so `atoms3` still means "freewheel unless told otherwise".
void saveLossPolicyToEEPROM(bool stop) {
  preferences.begin("nowde", false);
  preferences.putUChar("loss", stop ? 1 : 0);
  preferences.end();
  DEBUG_SERIAL.printf("[EEPROM] Loss policy saved: %s\r\n", stop ? "STOP" : "FREEWHEEL");
}

bool loadLossPolicyFromEEPROM() {
  if (!preferences.begin("nowde", true)) {
    return NOWDE_STOP_ON_LINK_LOST != 0;
  }
  uint8_t v = preferences.getUChar("loss", NOWDE_STOP_ON_LINK_LOST ? 1 : 0);
  preferences.end();
  return v != 0;
}

void clearEEPROM() {
  preferences.begin("nowde", false);
  preferences.clear();
  preferences.end();
  DEBUG_SERIAL.println("[EEPROM] All data cleared");
}

uint8_t loadResyncRebootCount() {
  if (!preferences.begin("nowde", true)) {
    return 0;
  }
  uint8_t n = preferences.getUChar("resyncrb", 0);
  preferences.end();
  return n;
}

void saveResyncRebootCount(uint8_t count) {
  preferences.begin("nowde", false);
  preferences.putUChar("resyncrb", count);
  preferences.end();
}
