#include "sysex.h"

#include <cstring>

#include <esp_now.h>

#include "midi.h"
#include "nowde_config.h"
#include "nowde_state.h"
#include "receiver_mode.h"
#include "sender_mode.h"
#include "storage.h"

void handleSysExMessage(const uint8_t* data, uint8_t length) {
  if (length < 4) {
    return;
  }

  if (data[0] != SYSEX_START || data[1] != SYSEX_MANUFACTURER_ID || data[length - 1] != SYSEX_END) {
    return;
  }

  uint8_t command = data[2];

  // SYSEX command logging hidden for cleaner output
  // Only specific command responses are logged below

  switch (command) {
    case SYSEX_CMD_BRIDGE_CONNECTED:
      senderModeEnabled = true;
      DEBUG_SERIAL.println("\n=== SENDER MODE ACTIVATED ===");
      DEBUG_SERIAL.println("Received: Bridge Connected SysEx");
      DEBUG_SERIAL.println("Status: Broadcasting ESP-NOW beacons");
      DEBUG_SERIAL.println("=============================\n");
      reportReceiversToBridge();
      break;

    case SYSEX_CMD_SUBSCRIBE_LAYER:
      if (length >= 5) {
        receiverModeEnabled = true;
        int layerLen = min<int>(length - 4, MAX_LAYER_LENGTH - 1);
        memcpy(subscribedLayer, &data[3], layerLen);
        subscribedLayer[layerLen] = '\0';
        saveLayerToEEPROM(subscribedLayer);

        DEBUG_SERIAL.println("\n=== RECEIVER LAYER CHANGED ===");
        DEBUG_SERIAL.print("New Layer: ");
        DEBUG_SERIAL.println(subscribedLayer);
        DEBUG_SERIAL.println("Status: Layer saved to EEPROM");
        DEBUG_SERIAL.println("==============================\n");

        sendReceiverInfo();
      }
      break;

    case SYSEX_CMD_CHANGE_RECEIVER_LAYER:
      if (senderModeEnabled && length >= 11) {
        uint8_t targetMac[6];
        memcpy(targetMac, &data[3], 6);

        int layerLen = min<int>(length - 10, MAX_LAYER_LENGTH - 1);
        char newLayer[MAX_LAYER_LENGTH];
        memcpy(newLayer, &data[9], layerLen);
        newLayer[layerLen] = '\0';

        DEBUG_SERIAL.println("\n[SENDER] Received layer change request");
        DEBUG_SERIAL.print("  Target MAC: ");
        for (int j = 0; j < 6; j++) {
          DEBUG_SERIAL.printf("%02X", targetMac[j]);
          if (j < 5) {
            DEBUG_SERIAL.print(":");
          }
        }
        DEBUG_SERIAL.println();
        DEBUG_SERIAL.print("  New Layer: ");
        DEBUG_SERIAL.println(newLayer);
        DEBUG_SERIAL.print("  Searching in receiver table (");
        DEBUG_SERIAL.print(countActiveReceivers());
        DEBUG_SERIAL.println(" active receivers)...");

        bool found = false;
        for (int i = 0; i < MAX_RECEIVERS; i++) {
          if (receiverTable[i].active) {
            DEBUG_SERIAL.print("    [");
            DEBUG_SERIAL.print(i);
            DEBUG_SERIAL.print("] ");
            for (int j = 0; j < 6; j++) {
              DEBUG_SERIAL.printf("%02X", receiverTable[i].mac[j]);
              if (j < 5) {
                DEBUG_SERIAL.print(":");
              }
            }
            DEBUG_SERIAL.print(" - Layer: ");
            DEBUG_SERIAL.println(receiverTable[i].layer);
          }

          if (receiverTable[i].active && macEqual(receiverTable[i].mac, targetMac)) {
            found = true;
            DEBUG_SERIAL.println("  MATCH FOUND! Sending ESP-NOW message...");

            uint8_t espnowMsg[32];
            int idx = 0;
            espnowMsg[idx++] = SYSEX_START;
            espnowMsg[idx++] = SYSEX_MANUFACTURER_ID;
            espnowMsg[idx++] = SYSEX_CMD_SUBSCRIBE_LAYER;
            size_t payloadLen = strlen(newLayer);
            memcpy(&espnowMsg[idx], newLayer, payloadLen);
            idx += payloadLen;
            espnowMsg[idx++] = SYSEX_END;

            esp_err_t result = esp_now_send(targetMac, espnowMsg, idx);

            DEBUG_SERIAL.print("  ESP-NOW Send: ");
            if (result == ESP_OK) {
              DEBUG_SERIAL.println("SUCCESS");
            } else {
              DEBUG_SERIAL.printf("FAILED (error %d)\r\n", result);
            }
            DEBUG_SERIAL.println();
            break;
          }
        }

        if (!found) {
          DEBUG_SERIAL.println("  ERROR: Receiver not found in table!");
          DEBUG_SERIAL.println();
        }
      }
      break;

    case SYSEX_CMD_MEDIA_SYNC:
      if (senderModeEnabled && length >= 26) {
        char targetLayer[MAX_LAYER_LENGTH];
        memcpy(targetLayer, &data[3], MAX_LAYER_LENGTH);
        targetLayer[MAX_LAYER_LENGTH - 1] = '\0';

        uint8_t mediaIndex = data[19];
        uint32_t positionMs = (static_cast<uint32_t>(data[20]) << 24) |
                              (static_cast<uint32_t>(data[21]) << 16) |
                              (static_cast<uint32_t>(data[22]) << 8) |
                              static_cast<uint32_t>(data[23]);
        uint8_t state = data[24];
        uint32_t meshTimestamp = meshClock.meshMillis();

        // Only log media sync on state changes or media index changes
        static uint8_t lastState = 255;
        static uint8_t lastIndex = 255;
        bool shouldLog = (state != lastState) || (mediaIndex != lastIndex);
        
        if (shouldLog) {
          DEBUG_SERIAL.printf("[MEDIA SYNC] Layer='%s', Index=%d, Pos=%lu ms, State=%s, MeshTime=%lu\r\n",
                             targetLayer, mediaIndex, positionMs,
                             state == 1 ? "playing" : "stopped", meshTimestamp);
          lastState = state;
          lastIndex = mediaIndex;
        }

        MediaSyncPacket syncPacket;
        strncpy(syncPacket.layer, targetLayer, MAX_LAYER_LENGTH);
        syncPacket.layer[MAX_LAYER_LENGTH - 1] = '\0';
        syncPacket.mediaIndex = mediaIndex;
        syncPacket.positionMs = positionMs;
        syncPacket.state = state;
        syncPacket.meshTimestamp = meshTimestamp;

        int sentCount = 0;
        for (int i = 0; i < MAX_RECEIVERS; i++) {
          // Send to ALL active receivers on matching layer, regardless of connected status
          // This ensures stopped state packets reach receivers even if they stopped sending info
          if (receiverTable[i].active &&
              strncmp(receiverTable[i].layer, targetLayer, MAX_LAYER_LENGTH) == 0) {
            esp_now_send(receiverTable[i].mac, reinterpret_cast<uint8_t*>(&syncPacket), sizeof(syncPacket));
            sentCount++;
          }
        }

        // ESP-NOW TX logging disabled for media sync to reduce clutter
        // Sync packets sent every ~100ms but not logged
      }
      break;
  }
}
