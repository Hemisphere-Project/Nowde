/*
 * Nowde - ESP32-S3 sync node firmware
 * https://github.com/Hemisphere-Project/Nowde
 * Copyright (C) 2025-2026 maigre - Hemisphere Project
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
#include <esp_system.h>
#include <esp_mac.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_now_handlers.h"
#include "midi.h"
#include "nowde_config.h"
#include "nowde_state.h"
#include "receiver_mode.h"
#include "sender_mode.h"
#include "storage.h"
#include "sysex.h"
#include "ui.h"
#include "usb_out.h"

// Task handles for multi-core operation
TaskHandle_t midiTaskHandle = NULL;
TaskHandle_t espnowTaskHandle = NULL;

namespace {

void printBanner() {
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("═══════════════════════════════════");
  DEBUG_SERIAL.println("      NOWDE ESP-NOW v" NOWDE_VERSION "      ");
  DEBUG_SERIAL.println("        Hemisphere Project 2026      ");
  DEBUG_SERIAL.println("        board: " NOWDE_BOARD_NAME);
  DEBUG_SERIAL.println("═══════════════════════════════════");
  DEBUG_SERIAL.println();
}

void configureUsbDescriptors() {
  // Get MAC address for unique device identification using esp_efuse API
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  
  // Create product name: "Nowde - DDEEFF" (last 3 bytes of MAC)
  // Version is sent via HELLO SysEx message instead of USB descriptor
  // NOTE: macOS caches MIDI device names. To clear cache after firmware update:
  //   sudo rm -rf ~/Library/Preferences/com.apple.audio.midi*
  //   sudo killall coreaudiod
  //   (Then unplug/replug device or restart Mac)
  char productName[32];
  snprintf(productName, sizeof(productName), "Nowde - %02X%02X%02X", 
           mac[3], mac[4], mac[5]);
  
  DEBUG_SERIAL.print("[USB] Setting product name: ");
  DEBUG_SERIAL.println(productName);
  
  USB.VID(0x303A);
  USB.PID(0x8000);
  USB.productName(productName);
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
  // ESPNowMeshClock adds the same broadcast peer itself, so tag whichever entry won.
  nowdeApplyPeerRate(broadcastAddress);
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

// 2.0.1 mesh-resync self-heal. Persisted across reboots, read at boot (setup()).
uint8_t resyncRebootCount = 0;

// Called each receiver tick. Fires only while the node is COARSE -- following the master (fresh
// packets, playing) but with a mesh clock that disagrees, i.e. a stranded/forward-only-latched
// offset. A plain RF outage is NONE (freewheel), never COARSE, so it never triggers this. Audio
// stays correct throughout via Fix A; this only repairs sub-frame precision. Escalation:
//   coarse >= MESH_RESYNC_GRACE_MS   -> meshClock.reset() (soft), up to MESH_MAX_SOFT_RESETS
//   still coarse >= MESH_REBOOT_GRACE_MS after that -> ESP.restart(), bounded by an NVS counter.
void meshResyncTick() {
  static unsigned long stuckSince = 0;
  static unsigned long lastAction = 0;
  static uint8_t softResets = 0;
  static bool gaveUpLogged = false;

  uint8_t q = nodeSyncQuality();

  if (q != NOWDE_SYNC_COARSE) {
    stuckSince = 0;
    softResets = 0;
    gaveUpLogged = false;
    if (q == NOWDE_SYNC_LOCKED && resyncRebootCount != 0) {
      resyncRebootCount = 0;
      saveResyncRebootCount(0);          // a real lock re-arms the auto-reboot guard
      DEBUG_SERIAL.println("[RESYNC] locked again - auto-reboot guard cleared");
    }
    return;
  }

  unsigned long now = millis();
  if (stuckSince == 0) { stuckSince = now; lastAction = now; softResets = 0; }
  unsigned long stuckMs = now - stuckSince;

  if (softResets < MESH_MAX_SOFT_RESETS) {
    if (now - lastAction >= MESH_RESYNC_GRACE_MS) {
      meshClock.reset();
      softResets++;
      lastAction = now;
      DEBUG_SERIAL.printf("[RESYNC] mesh clock stranded (coarse %lus) - soft reset %u/%u\r\n",
                          stuckMs / 1000, softResets, MESH_MAX_SOFT_RESETS);
    }
    return;
  }

  // soft resets exhausted -> reboot after the longer grace, bounded so a poisoned mesh can't loop
  if (stuckMs >= MESH_REBOOT_GRACE_MS) {
    if (resyncRebootCount < MESH_MAX_AUTO_REBOOTS) {
      saveResyncRebootCount(resyncRebootCount + 1);
      DEBUG_SERIAL.printf("[RESYNC] soft resets failed - auto-reboot %u/%u to re-adopt the mesh\r\n",
                          resyncRebootCount + 1, MESH_MAX_AUTO_REBOOTS);
      delay(80);                 // best-effort: let a LOG frame drain. Emits NO MIDI Stop -- the
      ESP.restart();             // Pi rides the ~5-10 s USB gap on its mpv loop + Drifter freewheel.
    } else if (!gaveUpLogged) {
      gaveUpLogged = true;
      DEBUG_SERIAL.println("[RESYNC] auto-reboot cap reached - staying coarse+flagged (audio still follows master)");
    }
  }
}

}  // namespace

// ============= CORE 0 - MIDI/USB TASK (High Priority) =============
// Runs on Core 0 to ensure USB MIDI is never blocked by ESP-NOW traffic
void midiTask(void* parameter) {
  DEBUG_SERIAL.println("[TASK] MIDI task started on Core 0 (high priority)");
  
  for (;;) {
    midiProcess();  // Handle USB MIDI I/O
    vTaskDelay(pdMS_TO_TICKS(1));  // 1ms - very responsive for MIDI
  }
}

// ============= CORE 1 - ESP-NOW/APPLICATION TASK (Normal Priority) =============
// Runs on Core 1 for all ESP-NOW and application logic
void espnowTask(void* parameter) {
  DEBUG_SERIAL.println("[TASK] ESP-NOW task started on Core 1 (normal priority)");
  
  unsigned long lastSenderBeacon = 0;
  unsigned long lastBridgeReport = 0;
  unsigned long nextReceiverBeacon = 0;
  
  for (;;) {
    unsigned long now = millis();

    // Sender mode operations
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
      
      // Process delayed packets for RF simulation
      if (rfSimulationEnabled) {
        for (int i = 0; i < MAX_DELAYED_PACKETS; i++) {
          if (delayedPackets[i].active && now >= delayedPackets[i].sendTime) {
            esp_now_send(delayedPackets[i].receiverMac, 
                        reinterpret_cast<uint8_t*>(&delayedPackets[i].packet), 
                        sizeof(MediaSyncPacket));
            delayedPackets[i].active = false;
          }
        }
      }
    }

    // Receiver mode operations
    if (receiverModeEnabled) {
      if (now >= nextReceiverBeacon) {
        sendReceiverInfo();
        nextReceiverBeacon = now + RECEIVER_BEACON_INTERVAL_MS + random(0, 200);
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
            midiSendStop();
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

      // 2.0.1: repair a stranded mesh clock (soft reset, then bounded reboot) while COARSE.
      meshResyncTick();
    }

    meshClock.loop();
    uiTick();
    vTaskDelay(pdMS_TO_TICKS(10));  // 10ms - good for ESP-NOW operations
  }
}

void setup() {
#if defined(NOWDE_BOARD_ATOMS3)
  // Single USB-C: descriptors first, then CDC + MIDI as one composite device.
  uiInit();                       // M5Unified: board detection + LCD/LED
  usbOutInit();                   // queues first: logging/HELLO below only enqueue
  configureUsbDescriptors();
  midiInit();
  DEBUG_SERIAL.begin();           // USBCDC behind the log ring (usb_out.h)
  USB.begin();
  delay(1500);                    // let the host enumerate before we log or HELLO
#else
  usbOutInit();
  DEBUG_SERIAL.begin(115200);     // UART0
  delay(500);
#endif

  printBanner();

#if !defined(NOWDE_BOARD_ATOMS3)
  configureUsbDescriptors();
  USB.begin();
  DEBUG_SERIAL.println("[INIT] USB initialized");

  midiInit();
  DEBUG_SERIAL.println("[INIT] USB MIDI initialized");
  
  // Wait for USB to fully enumerate before sending HELLO
  delay(500);
#endif

  // Role: NVS override, else the board decides (see nowde_config.h)
  boardId = uiDetectBoard();
  storedRole = loadRoleFromEEPROM();
  uint8_t resolved = storedRole;
  if (storedRole == NOWDE_ROLE_AUTO) {
    resolved = (boardId == NOWDE_BOARDID_ATOMS3) ? NOWDE_ROLE_MASTER
             : (boardId == NOWDE_BOARDID_ATOMS3_LITE) ? NOWDE_ROLE_SLAVE
             : NOWDE_ROLE_LEGACY;
  }
  nodeRole = resolved;  // applyRole() runs after ESP-NOW is up
  DEBUG_SERIAL.printf("[INIT] Board id %u, role stored %u -> %s\r\n", boardId, storedRole,
                      resolved == NOWDE_ROLE_MASTER ? "MASTER" : resolved == NOWDE_ROLE_SLAVE ? "SLAVE" : "LEGACY");

  // 2.0.1: carry the mesh-resync auto-reboot count across the reboot so it cannot boot-loop.
  resyncRebootCount = loadResyncRebootCount();
  if (resyncRebootCount) {
    DEBUG_SERIAL.printf("[INIT] mesh-resync auto-reboot count = %u/%u (clears on next real lock)\r\n",
                        resyncRebootCount, MESH_MAX_AUTO_REBOOTS);
  }

  // Send HELLO to notify the host of boot/reboot
  sendHello();

  meshClock.setDebugLog(0);  // LOG_ALL / LOG_SYNC / LOG_BCAST / LOG_RX / 0
  meshClock.begin(false);
  DEBUG_SERIAL.println("[INIT] Mesh Clock initialized");

  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
#if NOWDE_WIFI_LR
  // LR-only PHY. Must be set after the interface is up and before the channel, and every
  // node on the mesh needs the same build or it simply will not see the others.
  esp_err_t lrErr = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
  if (lrErr != ESP_OK) DEBUG_SERIAL.printf("[LR] set_protocol FAILED: %s\r\n", esp_err_to_name(lrErr));
#endif
  esp_wifi_set_channel(NOWDE_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  DEBUG_SERIAL.printf("[INIT] WiFi STA mode configured, channel %d%s\r\n", NOWDE_WIFI_CHANNEL,
                      NOWDE_WIFI_LR ? " [LR]" : "");
  // Printed on every boot so the policy is checkable on site without reading the binary.
  DEBUG_SERIAL.printf("[INIT] Link loss: %s\r\n",
                      NOWDE_STOP_ON_LINK_LOST ? "STOP (CC#100=0 + MIDI Stop)" : "FREEWHEEL");

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
  // Always enable receiver mode with saved layer or default "-"
  if (savedLayer.length() == 0 || savedLayer == "") {
    savedLayer = DEFAULT_RECEIVER_LAYER;
  }
  
  strncpy(subscribedLayer, savedLayer.c_str(), MAX_LAYER_LENGTH);
  subscribedLayer[MAX_LAYER_LENGTH - 1] = '\0';
  applyRole(nodeRole);   // receiver always on; master = sender from boot
  DEBUG_SERIAL.println("[INIT] Receiver mode on");
  DEBUG_SERIAL.print("  Subscribed Layer: ");
  DEBUG_SERIAL.println(subscribedLayer);
  if (senderModeEnabled) {
    DEBUG_SERIAL.println("[INIT] Sender mode on (master role)");
  }
  DEBUG_SERIAL.println();
  
  // Create MIDI task on Core 0 with high priority (configMAX_PRIORITIES - 1)
  // Stack: 4096 bytes, Priority: 24 (high), Core: 0
  xTaskCreatePinnedToCore(
    midiTask,           // Task function
    "MIDI_Task",        // Task name
    4096,               // Stack size (bytes)
    NULL,               // Parameters
    configMAX_PRIORITIES - 1,  // High priority (24 on ESP32)
    &midiTaskHandle,    // Task handle
    0                   // Core 0 - dedicated to USB/MIDI
  );
  DEBUG_SERIAL.println("[INIT] MIDI task created on Core 0");
  
  // Create ESP-NOW task on Core 1 with normal priority
  // Stack: 8192 bytes, Priority: 10 (normal), Core: 1
  xTaskCreatePinnedToCore(
    espnowTask,         // Task function
    "ESPNOW_Task",      // Task name
    8192,               // Stack size (bytes) - larger for ESP-NOW operations
    NULL,               // Parameters
    10,                 // Normal priority
    &espnowTaskHandle,  // Task handle
    1                   // Core 1 - Arduino default core
  );
  DEBUG_SERIAL.println("[INIT] ESP-NOW task created on Core 1");
}

void loop() {
  // Empty - all work now done in FreeRTOS tasks
  // Arduino loop() runs on Core 1 but we don't need it
  vTaskDelay(portMAX_DELAY);  // Sleep forever
}
