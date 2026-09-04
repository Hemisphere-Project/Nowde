#pragma once

#include <esp_now.h>

#include "nowde_config.h"

// ESP-NOW only actually transmits on the long-range PHY once each PEER carries an explicit
// LoRa rate config. esp_wifi_set_protocol(LR) alone merely advertises the capability: the
// frames keep going out at a legacy rate and a non-LR node decodes them perfectly (measured
// on the bench 2026-09-04, which is how the first LR build was caught doing nothing).
// Every peer we add therefore has to be tagged. Compiles to nothing on non-LR builds.
// inline on purpose: main.cpp keeps its file-local helpers in an anonymous namespace, so a
// definition living there would have internal linkage and never reach sender_mode.cpp.
#if NOWDE_WIFI_LR
inline void nowdeApplyPeerRate(const uint8_t* peer) {
  esp_now_rate_config_t rc = {};
  rc.phymode = WIFI_PHY_MODE_LR;
  rc.rate = NOWDE_LR_RATE;
  rc.ersu = false;
  rc.dcm = false;
  esp_err_t r = esp_now_set_peer_rate_config(peer, &rc);
  if (r != ESP_OK) {
    DEBUG_SERIAL.printf("[LR] peer rate config FAILED: %s\r\n", esp_err_to_name(r));
  }
}
#else
inline void nowdeApplyPeerRate(const uint8_t*) {}
#endif

// ESP-NOW delivery tallies, filled by onDataSent(). A rising espnowTxFail is the only
// warning that a slave is being talked to and not answering.
extern volatile uint32_t espnowTxOk;
extern volatile uint32_t espnowTxFail;
extern volatile uint32_t espnowRelayDropped;

void onDataSent(const esp_now_send_info_t* info, esp_now_send_status_t status);
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
