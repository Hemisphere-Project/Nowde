#include "watchdog.h"

#include <esp_attr.h>
#include <esp_now.h>
#include <esp_system.h>

#include "nowde_config.h"
#include "nowde_state.h"

// ============= 2.0.5 RADIO WATCHDOG =============
// Field facts (Biennale MBA, 2026-09): twice the MASTER's radio wedged for good -- every
// esp_now_send() came back ESP_ERR_ESPNOW_NO_MEM ("[RELAY] enqueue FAILED ... 156221 dropped",
// every 2 s for 19 h), the receiver table drained to 0 (no RECEIVER_INFO reached us either) --
// while USB-MIDI and the main loop stayed perfectly healthy. An esp_restart() (a SysEx SET_LR,
// by luck) recovered it: the table was back to 5 within a minute. Meanwhile the slaves, built
// with NOWDE_STOP_ON_LINK_LOST=0, free-ran MTC for the whole outage.
//
// This module restarts a node when its radio is provably dead, and nothing else:
//   master (sender role)  a. every relay send failed NO_MEM for WD_NOMEM_MS while the host kept
//                            feeding us (one successful send ends the streak);
//                         b. the table held >= 1 connected slave this boot and has been empty
//                            for WD_TABLE_EMPTY_MS.
//   slave                 c. a master was heard this boot and none for WD_NO_SENDER_MS.
// Two rules bound it so it can never become a boot loop:
//   - nothing fires in the first WD_MIN_UPTIME_MS of a boot;
//   - a restart is allowed only once the boot has lived a backoff kept in RTC memory (kept
//     across esp_restart(), garbage after power-on -- hence the magic + check word): 5 min,
//     doubled at every watchdog restart of a boot that never had a healthy radio for
//     WD_HEALTHY_MS or that fires within WD_LOOP_WINDOW_MS of the previous one, capped at
//     60 min; back to 5 min as soon as the radio is healthy for WD_HEALTHY_MS. Until the backoff
//     is reached the trigger is "held" and says so once a minute.
// Every decision is a [WATCHDOG] log line; the restart count rides in HELLO (docs/PROTOCOL.md).
// "Healthy" = a master sees >= 1 connected slave (its beacons go out, their info comes back), a
// slave hears >= 1 master. A node that never had a peer this boot never fires: a fleet switched
// off around it costs it at most one restart, then it waits.

namespace {

constexpr uint32_t WD_MAGIC              = 0x4E57443A;     // 'NWD:'
constexpr uint32_t WD_MIN_UPTIME_MS      = 120000;         // never in the first 120 s of a boot
constexpr uint32_t WD_BACKOFF_MIN_MS     = 5UL * 60000;    // 5 min
constexpr uint32_t WD_BACKOFF_MAX_MS     = 60UL * 60000;   // 60 min cap
constexpr uint32_t WD_LOOP_WINDOW_MS     = 10UL * 60000;   // fired < 10 min after the previous restart = looping
constexpr uint32_t WD_HEALTHY_MS         = 60000;          // radio healthy this long -> backoff back to 5 min
constexpr uint32_t WD_NOMEM_MS           = 30000;          // master: every relay send NO_MEM for 30 s ...
constexpr uint32_t WD_ATTEMPT_MAX_AGE_MS = 5000;           //   ... with attempts still coming (host feeding us)
constexpr uint32_t WD_TABLE_EMPTY_MS     = 120000;         // master: had slaves, none connected for 120 s
constexpr uint32_t WD_NO_SENDER_MS       = 300000;         // slave: had a master, none heard for 300 s

enum : uint32_t { WD_REASON_NONE = 0, WD_REASON_NOMEM = 1, WD_REASON_TABLE_EMPTY = 2, WD_REASON_NO_SENDER = 3 };

// Survives esp_restart() (RTC slow memory is not touched by a software reset); random after a
// power-on, which the magic + check word catch. Written only from the ESP-NOW task / setup().
struct WdRtc {
  uint32_t magic;
  uint32_t count;       // watchdog restarts since power-on
  uint32_t backoffMs;   // minimum uptime before the next watchdog restart may fire
  uint32_t armed;       // 1 = the boot reading this record follows a watchdog restart
  uint32_t reason;      // WD_REASON_* of that restart
  uint32_t check;       // ~(count ^ backoffMs ^ armed ^ reason)
};
RTC_NOINIT_ATTR WdRtc wdRtc;

bool bootAfterWatchdog = false;   // this boot follows a watchdog restart
bool healthyLatched = false;      // the radio was healthy for WD_HEALTHY_MS at some point this boot
unsigned long healthySince = 0;   // 0 = not healthy right now

// master triggers
bool hadReceiver = false;
unsigned long tableEmptySince = 0;                // 0 = not empty (or never had one)
volatile unsigned long relayNoMemSince = 0;       // 0 = the last relay send did not fail NO_MEM
volatile unsigned long relayLastAttempt = 0;
// slave trigger
bool hadSender = false;
unsigned long noSenderSince = 0;                  // 0 = a master is heard (or never was)

uint32_t wdCheck(const WdRtc& r) { return ~(r.count ^ r.backoffMs ^ r.armed ^ r.reason); }
void wdCommit() { wdRtc.check = wdCheck(wdRtc); }

const char* reasonName(uint32_t r) {
  switch (r) {
    case WD_REASON_NOMEM:       return "relay NO_MEM";
    case WD_REASON_TABLE_EMPTY: return "no slave";
    case WD_REASON_NO_SENDER:   return "no sender";
    default:                    return "none";
  }
}

// The trigger `reason` holds and has held for `forS` seconds: restart if the backoff allows it.
void fire(uint32_t reason, unsigned long now, unsigned long forS) {
  if (now < WD_MIN_UPTIME_MS) return;   // also true after a millis() wrap (49.7 d): harmless, it only holds
  if (now < wdRtc.backoffMs) {
    static unsigned long lastHeldLog = 0;
    if (lastHeldLog == 0 || now - lastHeldLog >= 60000) {
      lastHeldLog = now;
      DEBUG_SERIAL.printf("[WATCHDOG] %s for %lu s -> held (backoff %lu min, uptime %lu s)\r\n",
                          reasonName(reason), forS, (unsigned long)(wdRtc.backoffMs / 60000), now / 1000);
    }
    return;
  }
  uint32_t next = WD_BACKOFF_MIN_MS;
  if (bootAfterWatchdog && (!healthyLatched || now < WD_LOOP_WINDOW_MS)) {
    next = wdRtc.backoffMs * 2;
    if (next > WD_BACKOFF_MAX_MS) next = WD_BACKOFF_MAX_MS;
  }
  wdRtc.count++;
  wdRtc.backoffMs = next;
  wdRtc.armed = 1;
  wdRtc.reason = reason;
  wdCommit();
  DEBUG_SERIAL.printf("[WATCHDOG] %s for %lu s -> esp_restart (#%lu, next backoff %lu min)\r\n",
                      reasonName(reason), forS, (unsigned long)wdRtc.count, (unsigned long)(next / 60000));
  delay(300);        // let the LOG frame reach the host (SET_LR restarts the same way)
  esp_restart();
}

}  // namespace

void watchdogInit() {
  if (wdRtc.magic != WD_MAGIC || wdRtc.check != wdCheck(wdRtc) ||
      wdRtc.backoffMs < WD_BACKOFF_MIN_MS || wdRtc.backoffMs > WD_BACKOFF_MAX_MS) {
    // power-on (RTC memory is random) or a record we cannot trust: start clean
    wdRtc = WdRtc{};
    wdRtc.magic = WD_MAGIC;
    wdRtc.backoffMs = WD_BACKOFF_MIN_MS;
    wdCommit();
  }
  bootAfterWatchdog = (wdRtc.armed == 1);
  if (bootAfterWatchdog) {
    wdRtc.armed = 0;   // a later SET_LR / OTA restart must not read as ours
    wdCommit();
    DEBUG_SERIAL.printf("[WATCHDOG] boot after watchdog restart #%lu: %s (backoff %lu min)\r\n",
                        (unsigned long)wdRtc.count, reasonName(wdRtc.reason),
                        (unsigned long)(wdRtc.backoffMs / 60000));
  } else if (wdRtc.count != 0) {
    DEBUG_SERIAL.printf("[WATCHDOG] %lu watchdog restart(s) since power-on, last: %s\r\n",
                        (unsigned long)wdRtc.count, reasonName(wdRtc.reason));
  }
  // Printed on every boot so the policy is checkable on site without reading the binary.
  DEBUG_SERIAL.printf("[WATCHDOG] armed: master NO_MEM %lu s / no slave %lu s, slave no sender %lu s; "
                      "min uptime %lu s, backoff %lu min\r\n",
                      (unsigned long)(WD_NOMEM_MS / 1000), (unsigned long)(WD_TABLE_EMPTY_MS / 1000),
                      (unsigned long)(WD_NO_SENDER_MS / 1000), (unsigned long)(WD_MIN_UPTIME_MS / 1000),
                      (unsigned long)(wdRtc.backoffMs / 60000));
}

void watchdogRelayResult(esp_err_t r) {
  // MIDI task (core 0). Only NO_MEM extends a streak; a success -- or any other outcome, which is
  // not the field's signature -- ends it. A gap in the attempts (host away) starts a fresh streak.
  // Since v2.2 the ESP-NOW task calls this too (mesh_relay.cpp forwards through relaySend()): same
  // radio, same signature. Two writers can at worst restart a streak one send late -- a 30 s
  // streak still needs every send in it to fail.
  unsigned long now = millis();
  bool gap = (relayLastAttempt != 0) && (now - relayLastAttempt > WD_ATTEMPT_MAX_AGE_MS);
  relayLastAttempt = now;
  if (r == ESP_ERR_ESPNOW_NO_MEM) {
    if (relayNoMemSince == 0 || gap) relayNoMemSince = now;
  } else {
    relayNoMemSince = 0;
  }
}

uint8_t watchdogRestartCount() {
  return wdRtc.count > 127 ? 127 : static_cast<uint8_t>(wdRtc.count);
}

void watchdogTick(unsigned long now) {
  if (otaInProgress) return;   // someone is at the console: never restart under an OTA

  // ---- health: the radio demonstrably works -> backoff back to its floor (once per boot is enough)
  bool healthy = senderModeEnabled ? (countConnectedReceivers() > 0) : (countActiveSenders() > 0);
  if (healthy) {
    if (healthySince == 0) healthySince = now;
    if (!healthyLatched && now - healthySince >= WD_HEALTHY_MS) {
      healthyLatched = true;
      if (wdRtc.backoffMs != WD_BACKOFF_MIN_MS) {
        wdRtc.backoffMs = WD_BACKOFF_MIN_MS;
        wdCommit();
        DEBUG_SERIAL.printf("[WATCHDOG] radio healthy for %lu s -> backoff back to %lu min\r\n",
                            (unsigned long)(WD_HEALTHY_MS / 1000), (unsigned long)(WD_BACKOFF_MIN_MS / 60000));
      }
    }
  } else {
    healthySince = 0;
  }

  // ---- master (sender role): a. relay NO_MEM streak, b. table drained
  if (senderModeEnabled) {
    if (countConnectedReceivers() > 0) {
      hadReceiver = true;
      tableEmptySince = 0;
    } else if (hadReceiver && tableEmptySince == 0) {
      tableEmptySince = now;
    }
    unsigned long noMemSince = relayNoMemSince;       // one read each: written by the MIDI task
    unsigned long lastAttempt = relayLastAttempt;
    if (noMemSince != 0 && now - noMemSince >= WD_NOMEM_MS && now - lastAttempt <= WD_ATTEMPT_MAX_AGE_MS) {
      fire(WD_REASON_NOMEM, now, (now - noMemSince) / 1000);
    } else if (tableEmptySince != 0 && now - tableEmptySince >= WD_TABLE_EMPTY_MS) {
      fire(WD_REASON_TABLE_EMPTY, now, (now - tableEmptySince) / 1000);
    }
  }

  // ---- slave: c. isolated
  if (nodeRole == NOWDE_ROLE_SLAVE) {
    if (countActiveSenders() > 0) {
      hadSender = true;
      noSenderSince = 0;
    } else if (hadSender && noSenderSince == 0) {
      noSenderSince = now;
    }
    if (noSenderSince != 0 && now - noSenderSince >= WD_NO_SENDER_MS) {
      fire(WD_REASON_NO_SENDER, now, (now - noSenderSince) / 1000);
    }
  }
}
