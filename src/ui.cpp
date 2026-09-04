#include "ui.h"

#include <WiFi.h>
#include <esp_system.h>

#include "nowde_config.h"
#include "nowde_state.h"
#include "storage.h"

#if NOWDE_HAS_UI
#include <M5Unified.h>

namespace {

constexpr uint8_t LED_PIN = 35;            // AtomS3 Lite WS2812
constexpr unsigned long REFRESH_MS = 200;  // 5 Hz
constexpr unsigned long HOLD_RESET_MS = 3000;

bool hasDisplay = false;
uint8_t page = 0;
unsigned long lastRefresh = 0;
bool holdArmed = true;
char notice[24] = "";
unsigned long noticeUntil = 0;

struct Rgb { uint8_t r, g, b; };

// What the node is doing, as one colour. Same code on the LED and on the LCD banner.
Rgb statusColor(bool& blink) {
  blink = false;
  if (otaInProgress) return {80, 0, 80};                       // magenta: OTA
  SyncState sync = meshClock.getSyncState();
  if (nodeRole == NOWDE_ROLE_MASTER) {
    if (!hostLinked()) return {40, 0, 60};                     // purple: master, no host yet
    if (countConnectedReceivers() == 0) { blink = true; return {60, 60, 0}; }  // yellow blink: no slave
    return mediaSyncState.currentState == 1 ? Rgb{0, 70, 0} : Rgb{0, 40, 60};  // green play / cyan idle
  }
  // slave
  if (mediaSyncState.linkLost) return {80, 0, 0};               // red: lost the master while playing
  if (sync == SyncState::LOST) return {80, 20, 0};              // orange: mesh clock lost
  if (sync == SyncState::ALONE || countActiveSenders() == 0) { blink = true; return {60, 60, 0}; }  // yellow blink: alone
  return mediaSyncState.currentState == 1 ? Rgb{0, 70, 0} : Rgb{0, 40, 60};
}

const char* roleName() {
  switch (nodeRole) {
    case NOWDE_ROLE_MASTER: return "MASTER";
    case NOWDE_ROLE_SLAVE: return "SLAVE";
    default: return "LEGACY";
  }
}

const char* syncName(SyncState s) {
  switch (s) {
    case SyncState::SYNCED: return "SYNCED";
    case SyncState::LOST: return "LOST";
    default: return "ALONE";
  }
}

void drawStatusPage() {
  auto& d = M5.Display;
  bool blink;
  Rgb c = statusColor(blink);
  uint16_t banner = d.color565(c.r * 3, c.g * 3, c.b * 3);

  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.fillRect(0, 0, d.width(), 22, banner);
  d.setTextColor(TFT_BLACK, banner);
  d.setTextSize(2);
  d.setCursor(4, 4);
  d.print(roleName());

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextSize(1);
  int y = 30;
  auto line = [&](const char* k, const String& v) {
    d.setCursor(4, y);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.print(k);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(46, y);
    d.print(v);
    y += 11;
  };
  line("mesh", syncName(meshClock.getSyncState()));
  if (nodeRole == NOWDE_ROLE_MASTER) {
    line("slaves", String(countConnectedReceivers()));
  } else {
    line("master", String(countActiveSenders()));
  }
  line("host", hostLinked() ? "LINK" : "---");
  line("layer", String(subscribedLayer));

  // media line: index + position
  uint32_t pos = mediaSyncState.currentPositionMs;
  if (mediaSyncState.currentState == 1) {
    pos += millis() - mediaSyncState.localClockStartTime;
  }
  char media[24];
  snprintf(media, sizeof(media), "%s %u  %02lu:%02lu",
           mediaSyncState.currentState == 1 ? ">" : "#",
           mediaSyncState.currentIndex,
           (unsigned long)(pos / 60000), (unsigned long)((pos / 1000) % 60));
  line("media", media);

  if (millis() < noticeUntil) {
    d.setCursor(4, d.height() - 12);
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.print(notice);
  } else {
    d.setCursor(4, d.height() - 12);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    // the LR marker is the one build difference that must be readable at a glance on site
    d.printf("v%s  ch%d%s", NOWDE_VERSION, NOWDE_WIFI_CHANNEL, NOWDE_WIFI_LR ? " LR" : "");
  }
  d.endWrite();
}

void drawInfoPage() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  int y = 6;
  auto line = [&](const String& s) { d.setCursor(4, y); d.print(s); y += 11; };
  line("NOWDE v" NOWDE_VERSION);
  line(String("board ") + NOWDE_BOARD_NAME + (boardId == NOWDE_BOARDID_ATOMS3 ? " lcd" : " lite"));
  line(String("role  ") + roleName() + (storedRole == NOWDE_ROLE_AUTO ? " (auto)" : " (nvs)"));
  line("mac   " + WiFi.macAddress().substring(9));
  line(String("up    ") + String(millis() / 1000) + "s");
  line(String("mesh  ") + String(meshClock.msSinceLastSync()) + "ms ago");
  line(String("free  ") + String(ESP.getFreeHeap() / 1024) + "k");
  line("");
  line("hold 3s: reset NVS");
  d.endWrite();
}

void ledShow() {
  bool blink;
  Rgb c = statusColor(blink);
  if (blink && ((millis() / 400) & 1)) {
    rgbLedWrite(LED_PIN, 0, 0, 0);
  } else {
    rgbLedWrite(LED_PIN, c.r, c.g, c.b);
  }
}

}  // namespace

void uiInit() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 0;   // our log goes to the USB CDC, not UART0
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  cfg.clear_display = true;
  M5.begin(cfg);
  hasDisplay = M5.Display.width() > 0;
  if (hasDisplay) {
    M5.Display.setRotation(0);
    M5.Display.setBrightness(96);
    M5.Display.setTextWrap(false);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(4, 4);
    M5.Display.print("NOWDE");
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 30);
    M5.Display.print("booting...");
  } else {
    rgbLedWrite(LED_PIN, 0, 0, 60);  // blue: booting
  }
}

uint8_t uiDetectBoard() {
  switch (M5.getBoard()) {
    case m5::board_t::board_M5AtomS3:
      return NOWDE_BOARDID_ATOMS3;
    case m5::board_t::board_M5AtomS3Lite:
    case m5::board_t::board_M5AtomS3U:
      return NOWDE_BOARDID_ATOMS3_LITE;
    default:
      return hasDisplay ? NOWDE_BOARDID_ATOMS3 : NOWDE_BOARDID_UNKNOWN;
  }
}

void uiLog(const char* line) {
  strncpy(notice, line, sizeof(notice) - 1);
  notice[sizeof(notice) - 1] = '\0';
  noticeUntil = millis() + 3000;
}

void uiTick() {
  M5.update();

  // Button: click = next page, 3 s hold = clear NVS (layer + role back to defaults) and reboot
  if (M5.BtnA.wasClicked() && hasDisplay) {
    page = (page + 1) % 2;
    lastRefresh = 0;
  }
  if (M5.BtnA.pressedFor(HOLD_RESET_MS) && holdArmed) {
    holdArmed = false;
    DEBUG_SERIAL.println("[UI] Long press: clearing NVS and rebooting");
    if (hasDisplay) {
      M5.Display.fillScreen(TFT_RED);
      M5.Display.setCursor(4, 50);
      M5.Display.setTextColor(TFT_WHITE, TFT_RED);
      M5.Display.setTextSize(2);
      M5.Display.print("NVS RESET");
    } else {
      rgbLedWrite(LED_PIN, 80, 80, 80);
    }
    clearEEPROM();
    delay(800);
    esp_restart();
  }
  if (M5.BtnA.wasReleased()) {
    holdArmed = true;
  }

  unsigned long now = millis();
  if (now - lastRefresh < REFRESH_MS) {
    return;
  }
  lastRefresh = now;

  if (hasDisplay) {
    if (page == 0) drawStatusPage(); else drawInfoPage();
  } else {
    ledShow();
  }
}

#else  // no UI (DevKit)

void uiInit() {}
uint8_t uiDetectBoard() { return NOWDE_BOARDID_DEVKIT; }
void uiTick() {}
void uiLog(const char* line) { (void)line; }

#endif
