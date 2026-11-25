#include "status_led.h"

#include <driver/gpio.h>
#include <esp_system.h>

#include "host_link.h"
#include "led_pattern.h"
#include "nowde_config.h"
#include "nowde_state.h"
#include "storage.h"
#include "sysex.h"
#include "ui.h"

namespace {

int16_t ledPin = -1;                       // -1: no pixel driven
uint8_t brightness = led::DEFAULT_BRIGHTNESS;
bool dark = false;
unsigned long bootAt = 0;
bool identifying = false;
unsigned long identifyAt = 0;
led::ButtonState button = {false, false, 0};
led::Rgb shown = {1, 2, 3};                // never a real frame: the first tick writes
unsigned long lastFrame = 0;
constexpr unsigned long FRAME_MS = 20;     // 50 Hz: the strobe's 50 ms half-period is 2-3 frames

bool pixelDriven() { return ledPin >= 0 && !uiHasDisplay(); }

void writePixel(int16_t pin, led::Rgb c) {
  if (pin < 0) return;
  rgbLedWrite((uint8_t)pin, c.r, c.g, c.b);
}

bool buttonPressed() {
#if NOWDE_HAS_UI
  return uiButtonPressed();                // BtnA through M5Unified (uiTick runs M5.update())
#else
  return NOWDE_BUTTON_PIN >= 0 && digitalRead(NOWDE_BUTTON_PIN) == LOW;
#endif
}

// What led_pattern.h needs, from today's state. The election (④) fills role Standby and the pin
// conflict; the hop count and the watchdog's cause code have no carrier yet (#t-051).
led::Inputs inputs(unsigned long now) {
  led::Inputs in = {};
  in.now = now;
  in.booting = now - bootAt < led::BOOT_FLASH_MS;
  in.ota = otaInProgress;
  in.errorCode = 0;
  in.identify = identifying;
  in.identifyStart = identifyAt;
  in.hops = 0;
  in.dark = dark;
  in.pinConflict = false;
  in.role = nodeRole == NOWDE_ROLE_MASTER ? led::Role::Sender : led::Role::Slave;
  in.layerAssigned = strcmp(subscribedLayer, NOWDE_LAYER_UNASSIGNED) != 0;
  in.linkLost = mediaSyncState.linkLost;
  in.masterHeard = countActiveSenders() > 0;
  in.locked = meshClock.getSyncState() == SyncState::SYNCED;
  in.coarse = mediaSyncState.coarse;
  return in;
}

void clearAndReboot() {
  DEBUG_SERIAL.println("[BUTTON] 10 s hold: clearing NVS and rebooting");
  if (pixelDriven()) writePixel(ledPin, led::dimmed(led::WHITE, brightness, 127));
  uiLog("NVS RESET");
  clearEEPROM();
  delay(800);
  esp_restart();
}

}  // namespace

void statusLedInit() {
  bootAt = millis();
  int8_t pin = NOWDE_LED_PIN;
  loadLedFromEEPROM(pin, brightness, dark);
  ledPin = pin;
#if !NOWDE_HAS_UI
  if (NOWDE_BUTTON_PIN >= 0) {
    // GPIO34..39 of a classic ESP32 have no pull-up: the Atom's GPIO39 has its own on the board
    pinMode(NOWDE_BUTTON_PIN, NOWDE_BUTTON_PIN >= 34 ? INPUT : INPUT_PULLUP);
  }
#endif
  DEBUG_SERIAL.printf("[LED] pin %d, brightness %u, dark %u, button %d\r\n", (int)ledPin, brightness,
                      dark ? 1 : 0, (int)NOWDE_BUTTON_PIN);
}

void statusLedIdentify() {
  identifying = true;
  identifyAt = millis();
  DEBUG_SERIAL.println("[LED] identify");
}

bool statusLedConfigure(uint8_t pin, uint8_t brightnessIn, bool darkIn) {
  int16_t p = pin == NOWDE_LED_PIN_NONE ? -1 : pin;
  if (brightnessIn > 127) return false;
  if (p >= 0 && (!GPIO_IS_VALID_OUTPUT_GPIO(p) || p == NOWDE_BUTTON_PIN)) return false;
  if (p != ledPin && pixelDriven()) writePixel(ledPin, led::BLACK);  // the old pin goes dark
  ledPin = p;
  brightness = brightnessIn;
  dark = darkIn;
  saveLedToEEPROM(p < 0 ? NOWDE_LED_PIN_NONE : (uint8_t)p, brightness, dark);
  shown = {1, 2, 3};
  DEBUG_SERIAL.printf("[SET_LED] pin %d, brightness %u, dark %u\r\n", (int)ledPin, brightness,
                      dark ? 1 : 0);
  return true;
}

void statusLedTick(unsigned long now) {
  switch (led::buttonStep(button, buttonPressed(), now)) {
    case led::ButtonEvent::Identify:
      if (!uiShortPress()) statusLedIdentify();
      break;
    case led::ButtonEvent::PinToggle: {
      uint8_t role = storedRole == NOWDE_ROLE_MASTER ? NOWDE_ROLE_AUTO : NOWDE_ROLE_MASTER;
      DEBUG_SERIAL.printf("[BUTTON] 3 s hold: role %u -> %u\r\n", storedRole, role);
      setStoredRole(role);
      break;
    }
    case led::ButtonEvent::ClearNvs:
      clearAndReboot();
      break;
    case led::ButtonEvent::None:
      break;
  }

  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;
  led::Inputs in = inputs(now);
  if (identifying && led::identifyOver(in)) {
    identifying = false;
    in.identify = false;
  }
  if (!pixelDriven()) return;
  led::Rgb c = led::ledFrame(led::ledLook(in), now, brightness);
  if (c == shown) return;
  shown = c;
  writePixel(ledPin, c);
}
