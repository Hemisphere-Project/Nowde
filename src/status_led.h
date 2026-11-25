#pragma once

#include <Arduino.h>

// Nowde 3 status LED + user button (nowde#t-050 ③, proposal §7): drives led_pattern.h's pure
// vocabulary from the node's state, on every board that has a pixel the firmware owns (DevKitC-1
// WS2812 on GPIO48 by default, AtomS3 Lite GPIO35, M5 Atom GPIO27; SET_LED moves it), and reads
// the button (BOOT GPIO0 on the DevKits / ESP32, GPIO39 on the Atom, BtnA on the AtomS3s):
// short press = identify, 3 s = pin or unpin this node as master, 10 s = clear NVS and reboot.
// The AtomS3 with an LCD has no pixel: its screen is the status, its short press flips the page.

void statusLedInit();                  // setup(), after the role is resolved: NVS, pins, boot flash
void statusLedTick(unsigned long now); // every espnowTask loop (10 ms), next to uiTick()
void statusLedIdentify();              // IDENTIFY 0x14: white strobe 3 s, then the hop count
// SET_LED 0x13: pin (NOWDE_LED_PIN_NONE = no pixel), brightness 0..127, show-dark. Stored in NVS,
// applied live. false = refused (not an output pin, or the button's).
bool statusLedConfigure(uint8_t pin, uint8_t brightness, bool dark);
