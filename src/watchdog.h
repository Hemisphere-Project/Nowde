#pragma once

#include <Arduino.h>
#include <esp_err.h>

// 2.0.5 radio watchdog: restarts a node whose radio is provably dead, behind an RTC-backed backoff.
// Rules, field facts and log lines: src/watchdog.cpp.
void watchdogInit();                     // setup(), before the boot HELLO: read the RTC record, log the boot
void watchdogTick(unsigned long now);    // espnowTask, every loop: health + triggers for this node's role
void watchdogRelayResult(esp_err_t r);   // sysex.cpp relaySend(): the outcome of every relay esp_now_send()
uint8_t watchdogRestartCount();          // watchdog restarts since power-on, capped at 127 (HELLO trailer)
