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

void clearEEPROM() {
  preferences.begin("nowde", false);
  preferences.clear();
  preferences.end();
  DEBUG_SERIAL.println("[EEPROM] All data cleared");
}
