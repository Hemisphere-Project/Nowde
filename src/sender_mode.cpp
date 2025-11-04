#include "sender_mode.h"

#include <esp_now.h>
#include <cstring>

#include "midi.h"
#include "nowde_config.h"
#include "nowde_state.h"

void cleanupReceiverTable() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active) {
      unsigned long timeSinceLastSeen = now - receiverTable[i].lastSeen;
      
      // Mark as disconnected after RECEIVER_TIMEOUT_MS (5s)
      if (receiverTable[i].connected && timeSinceLastSeen > RECEIVER_TIMEOUT_MS) {
        receiverTable[i].connected = false;

        DEBUG_SERIAL.println("\n[TIMEOUT] Receiver marked as MISSING");
        DEBUG_SERIAL.print("  MAC: ");
        for (int j = 0; j < 6; j++) {
          DEBUG_SERIAL.printf("%02X", receiverTable[i].mac[j]);
          if (j < 5) {
            DEBUG_SERIAL.print(":");
          }
        }
        DEBUG_SERIAL.println();
        DEBUG_SERIAL.print("  Layer: ");
        DEBUG_SERIAL.println(receiverTable[i].layer);
        DEBUG_SERIAL.println("  Status: MISSING");
        DEBUG_SERIAL.println();
      }
      
      // Completely remove after 10 seconds (reduced from 30s to quickly free slots)
      if (timeSinceLastSeen > 10000) {
        DEBUG_SERIAL.println("\n[CLEANUP] Receiver removed from table");
        DEBUG_SERIAL.print("  MAC: ");
        for (int j = 0; j < 6; j++) {
          DEBUG_SERIAL.printf("%02X", receiverTable[i].mac[j]);
          if (j < 5) {
            DEBUG_SERIAL.print(":");
          }
        }
        DEBUG_SERIAL.println();
        DEBUG_SERIAL.printf("  Time inactive: %lu seconds\r\n", timeSinceLastSeen / 1000);
        DEBUG_SERIAL.println();
        
        // Remove from ESP-NOW peer list
        esp_now_del_peer(receiverTable[i].mac);
        
        // Mark slot as free
        receiverTable[i].active = false;
        receiverTable[i].connected = false;
      }
    }
  }
}

void sendSenderBeacon() {
  if (!senderModeEnabled) {
    return;
  }

  SenderBeacon beacon;
  esp_err_t result = esp_now_send(broadcastAddress, reinterpret_cast<uint8_t*>(&beacon), sizeof(beacon));
  (void)result;

  // Beacon logging disabled for cleaner output
  // Beacons are sent every 1s but not logged
}

void reportReceiversToBridge() {
  if (!senderModeEnabled) {
    return;
  }

  // This function is now deprecated - receiver table reporting moved to RUNNING_STATE (0x21)
  // The Bridge uses QUERY_RUNNING_STATE (0x03) to get receiver info
  // This function may still be called from legacy code paths but does nothing
  
    // Legacy reporting removed - use sendRunningState() in sysex.cpp instead
  return;
}

void handleSenderBeacon(const esp_now_recv_info_t* info) {
  bool found = false;
  int freeSlot = -1;

  for (int i = 0; i < MAX_SENDERS; i++) {
    if (senderTable[i].active && macEqual(senderTable[i].mac, info->src_addr)) {
      senderTable[i].lastSeen = millis();
      found = true;
      break;
    }
    if (!senderTable[i].active && freeSlot == -1) {
      freeSlot = i;
    }
  }

  // Only log when NEW sender is registered (not on every beacon)
  if (!found && freeSlot != -1) {
    memcpy(senderTable[freeSlot].mac, info->src_addr, 6);
    senderTable[freeSlot].lastSeen = millis();
    senderTable[freeSlot].active = true;

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, info->src_addr, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    esp_err_t addResult = esp_now_add_peer(&peerInfo);

    DEBUG_SERIAL.println("\n[ESP-NOW RX] Sender Beacon");
    DEBUG_SERIAL.print("  From: ");
    for (int i = 0; i < 6; i++) {
      DEBUG_SERIAL.printf("%02X", info->src_addr[i]);
      if (i < 5) {
        DEBUG_SERIAL.print(":");
      }
    }
    DEBUG_SERIAL.println();
    DEBUG_SERIAL.println("  Action: Registered new sender");
    if (addResult == ESP_OK) {
      DEBUG_SERIAL.println("  Peer: Added to ESP-NOW");
    } else {
      DEBUG_SERIAL.printf("  Peer: Failed to add (error %d)\r\n", addResult);
    }
    DEBUG_SERIAL.printf("  Total Senders: %d\r\n\r\n", countActiveSenders());
  }
}

void handleReceiverInfo(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < static_cast<int>(sizeof(ReceiverInfo))) {
    return;
  }

  const ReceiverInfo* recvInfo = reinterpret_cast<const ReceiverInfo*>(data);

  bool found = false;
  int freeSlot = -1;
  bool changed = false;

  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active && macEqual(receiverTable[i].mac, info->src_addr)) {
      receiverTable[i].lastSeen = millis();
      
      // Update media index silently (no logging)
      receiverTable[i].mediaIndex = recvInfo->mediaIndex;

      // Only log on RECONNECTION (was disconnected, now connected again)
      if (!receiverTable[i].connected) {
        receiverTable[i].connected = true;
        changed = true;

        DEBUG_SERIAL.println("\n[ESP-NOW RX] Receiver RECONNECTED");
        DEBUG_SERIAL.print("  From: ");
        for (int j = 0; j < 6; j++) {
          DEBUG_SERIAL.printf("%02X", info->src_addr[j]);
          if (j < 5) {
            DEBUG_SERIAL.print(":");
          }
        }
        DEBUG_SERIAL.println();
        DEBUG_SERIAL.print("  Layer: ");
        DEBUG_SERIAL.println(recvInfo->layer);
        DEBUG_SERIAL.println("  Status: ACTIVE");
        DEBUG_SERIAL.println();
      }

      // Only log on LAYER CHANGE (not on every info packet)
      if (strncmp(receiverTable[i].layer, recvInfo->layer, MAX_LAYER_LENGTH) != 0) {
        strncpy(receiverTable[i].layer, recvInfo->layer, MAX_LAYER_LENGTH);
        receiverTable[i].layer[MAX_LAYER_LENGTH - 1] = '\0';
        changed = true;

        DEBUG_SERIAL.println("\n[ESP-NOW RX] Receiver Info Update");
        DEBUG_SERIAL.print("  From: ");
        for (int j = 0; j < 6; j++) {
          DEBUG_SERIAL.printf("%02X", info->src_addr[j]);
          if (j < 5) {
            DEBUG_SERIAL.print(":");
          }
        }
        DEBUG_SERIAL.println();
        DEBUG_SERIAL.print("  Layer Changed: ");
        DEBUG_SERIAL.println(recvInfo->layer);
        DEBUG_SERIAL.println();
      }

      found = true;
      break;
    }
    if (!receiverTable[i].active && freeSlot == -1) {
      freeSlot = i;
    }
  }

  if (!found && freeSlot != -1) {
    memcpy(receiverTable[freeSlot].mac, info->src_addr, 6);
    strncpy(receiverTable[freeSlot].layer, recvInfo->layer, MAX_LAYER_LENGTH);
    receiverTable[freeSlot].layer[MAX_LAYER_LENGTH - 1] = '\0';
    strncpy(receiverTable[freeSlot].version, recvInfo->version, MAX_VERSION_LENGTH);
    receiverTable[freeSlot].version[MAX_VERSION_LENGTH - 1] = '\0';
    receiverTable[freeSlot].lastSeen = millis();
    receiverTable[freeSlot].active = true;
    receiverTable[freeSlot].connected = true;
    receiverTable[freeSlot].mediaIndex = recvInfo->mediaIndex;  // Initialize media index
    changed = true;

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, info->src_addr, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    esp_err_t addResult = esp_now_add_peer(&peerInfo);

    DEBUG_SERIAL.println("\n[ESP-NOW RX] Receiver Info");
    DEBUG_SERIAL.print("  From: ");
    for (int i = 0; i < 6; i++) {
      DEBUG_SERIAL.printf("%02X", info->src_addr[i]);
      if (i < 5) {
        DEBUG_SERIAL.print(":");
      }
    }
    DEBUG_SERIAL.println();
    DEBUG_SERIAL.print("  Layer: ");
    DEBUG_SERIAL.println(recvInfo->layer);
    DEBUG_SERIAL.print("  Version: ");
    DEBUG_SERIAL.println(recvInfo->version);
    DEBUG_SERIAL.println("  Action: Registered new receiver");
    if (addResult == ESP_OK) {
      DEBUG_SERIAL.println("  Peer: Added to ESP-NOW");
    } else {
      DEBUG_SERIAL.printf("  Peer: Failed to add (error %d)\r\n", addResult);
    }
    DEBUG_SERIAL.printf("  Total Receivers: %d\r\n\r\n", countActiveReceivers());
  }

  (void)changed;
}
