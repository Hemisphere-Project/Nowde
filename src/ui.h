#pragma once

#include <Arduino.h>

// Board UI: LCD on the AtomS3, RGB LED on the AtomS3 Lite, nothing on the DevKit.
// uiInit() must run before the role is resolved: it is what detects the board.
void uiInit();
uint8_t uiDetectBoard();     // NOWDE_BOARDID_*
void uiTick();               // call often (10 ms); refreshes at its own pace, handles the button
void uiLog(const char* line);// one-line notice on the LCD (no-op without one)
bool uiHasDisplay();         // the AtomS3's LCD: no status pixel, the screen is the status
bool uiButtonPressed();      // BtnA through M5Unified (false without the M5 module)
bool uiShortPress();         // a short press the UI takes (LCD: next page); false = identify
