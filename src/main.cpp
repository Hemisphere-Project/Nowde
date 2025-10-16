/*
 * MilluBridge - Nowde ESP32 Firmware
 * Copyright (C) 2025 maigre - Hemisphere Project
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Arduino.h>
#include <USB.h>
#include <USBMIDI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <cstring>

#include "esp_now_handlers.h"
#include "midi.h"
#include "nowde_config.h"
#include "nowde_state.h"
#include "receiver_mode.h"
#include "sender_mode.h"
#include "storage.h"

namespace {

void printBanner() {
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("═══════════════════════════════════");
  DEBUG_SERIAL.println("      NOWDE ESP-NOW v1.0 Startup     ");
  DEBUG_SERIAL.println("        Hemisphere Project 2025      ");
  DEBUG_SERIAL.println("═══════════════════════════════════");
  DEBUG_SERIAL.println();
}

void configureUsbDescriptors() {
  USB.VID(0x303A);
  USB.PID(0x8000);
  USB.productName("Nowde 1.0");
  USB.manufacturerName("Hemisphere");
}

void addBroadcastPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, sizeof(broadcastAddress));
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(broadcastAddress)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      DEBUG_SERIAL.println("[ERROR] Failed to add broadcast peer!");
    } else {
      DEBUG_SERIAL.println("[INIT] Broadcast peer added");
    }
  } else {
    DEBUG_SERIAL.println("[INIT] Broadcast peer already exists");
  }
}

void logDeviceInfo() {
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("================================");
  DEBUG_SERIAL.print("Device MAC: ");
  DEBUG_SERIAL.println(WiFi.macAddress());
  DEBUG_SERIAL.print("Version: ");
  DEBUG_SERIAL.println(NOWDE_VERSION);
  DEBUG_SERIAL.println("================================");
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("Waiting for USB MIDI commands...");
  DEBUG_SERIAL.println("  - Bridge Connected: Activates sender mode");
  DEBUG_SERIAL.println("  - Subscribe Layer: Activates receiver mode");
  DEBUG_SERIAL.println();
}

}  // namespace

void setup() {
  DEBUG_SERIAL.begin(115200);
  delay(500);

  printBanner();

  configureUsbDescriptors();
  USB.begin();
  DEBUG_SERIAL.println("[INIT] USB initialized");

  midiInit();
  DEBUG_SERIAL.println("[INIT] USB MIDI initialized");

  meshClock.setDebugLog(0);  // LOG_ALL / LOG_SYNC / LOG_BCAST / LOG_RX / 0
  meshClock.begin(false);
  DEBUG_SERIAL.println("[INIT] Mesh Clock initialized");

  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  DEBUG_SERIAL.println("[INIT] WiFi STA mode configured");

  if (esp_now_init() != ESP_OK) {
    DEBUG_SERIAL.println("[ERROR] ESP-NOW init failed!");
    return;
  }
  DEBUG_SERIAL.println("[INIT] ESP-NOW initialized");

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  DEBUG_SERIAL.println("[INIT] ESP-NOW callbacks registered");

  addBroadcastPeer();
  logDeviceInfo();

  String savedLayer = loadLayerFromEEPROM();
  if (savedLayer.length() > 0) {
    receiverModeEnabled = true;
    strncpy(subscribedLayer, savedLayer.c_str(), MAX_LAYER_LENGTH);
    subscribedLayer[MAX_LAYER_LENGTH - 1] = '\0';
    DEBUG_SERIAL.println("[INIT] Auto-starting receiver mode");
    DEBUG_SERIAL.print("  Subscribed Layer: ");
    DEBUG_SERIAL.println(subscribedLayer);
    DEBUG_SERIAL.println();
  }
}

void loop() {
  unsigned long now = millis();

  midiProcess();

  if (senderModeEnabled) {
    if (now - lastSenderBeacon >= SENDER_BEACON_INTERVAL_MS) {
      lastSenderBeacon = now;
      sendSenderBeacon();
    }

    cleanupReceiverTable();

    if (now - lastBridgeReport >= BRIDGE_REPORT_INTERVAL_MS) {
      lastBridgeReport = now;
      reportReceiversToBridge();
    }
  }

  if (receiverModeEnabled) {
    static unsigned long nextBeacon = 0;
    if (now >= nextBeacon) {
      sendReceiverInfo();
      nextBeacon = now + RECEIVER_BEACON_INTERVAL_MS + random(0, 200);
    }

    cleanupSenderTable();

    // Check for link lost condition
    if (mediaSyncState.currentState == 1 && !mediaSyncState.linkLost) {
      if ((now - mediaSyncState.lastSyncTime) > LINK_LOST_TIMEOUT_MS) {
        mediaSyncState.linkLost = true;
        DEBUG_SERIAL.println("[MEDIA SYNC] LINK LOST - no sync packets received");
        
        if (mediaSyncState.stopOnLinkLost) {
          DEBUG_SERIAL.println("[MEDIA SYNC] Stopping MTC clock and sending CC#100=0");
          mediaSyncState.currentState = 0;
          midiSendCC100(0);
          mediaSyncState.lastSentIndex = 0;
        } else {
          DEBUG_SERIAL.println("[MEDIA SYNC] Continuing in freewheel mode indefinitely");
        }
      }
    }

    // Continuous MTC clock generation when playing
    if (mediaSyncState.currentState == 1) {
      // Calculate current position based on local clock
      unsigned long localElapsed = now - mediaSyncState.localClockStartTime;
      uint32_t currentPositionMs = mediaSyncState.currentPositionMs + localElapsed;

      // Send MTC at configured framerate
      if (now - mediaSyncState.lastMTCUpdateTime >= (1000 / MTC_FRAMERATE)) {
        midiSendTimeCode(currentPositionMs);
        mediaSyncState.lastMTCUpdateTime = now;
      }
    }
  }

  meshClock.loop();
  delay(10);
}
