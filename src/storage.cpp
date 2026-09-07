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
