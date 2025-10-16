#include "sender_mode.h"

#include <esp_now.h>
#include <cstring>

#include "midi.h"
#include "nowde_config.h"
#include "nowde_state.h"

void cleanupReceiverTable() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active && receiverTable[i].connected &&
        (now - receiverTable[i].lastSeen > RECEIVER_TIMEOUT_MS)) {
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

  uint8_t msg[256];
  int idx = 0;

  msg[idx++] = SYSEX_START;
  msg[idx++] = SYSEX_MANUFACTURER_ID;
  msg[idx++] = SYSEX_CMD_RECEIVER_TABLE;

  uint8_t count = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active) {
      count++;
    }
  }
  msg[idx++] = count;

  static uint8_t lastCount = 255;
  static bool lastConnectedStates[MAX_RECEIVERS] = {false};
  bool countChanged = (count != lastCount);
  bool statusChanged = false;

  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active && receiverTable[i].connected != lastConnectedStates[i]) {
      statusChanged = true;
      lastConnectedStates[i] = receiverTable[i].connected;
    }
  }

  if (countChanged || statusChanged) {
    DEBUG_SERIAL.println("\n[BRIDGE REPORT] Receiver table update");
    DEBUG_SERIAL.printf("  Receivers: %d\r\n", count);
    for (int i = 0; i < MAX_RECEIVERS; i++) {
      if (receiverTable[i].active) {
        DEBUG_SERIAL.print("    - ");
        for (int j = 0; j < 6; j++) {
          DEBUG_SERIAL.printf("%02X", receiverTable[i].mac[j]);
          if (j < 5) {
            DEBUG_SERIAL.print(":");
          }
        }
        DEBUG_SERIAL.print(" v");
        DEBUG_SERIAL.print(receiverTable[i].version);
        DEBUG_SERIAL.print(" (");
        DEBUG_SERIAL.print(receiverTable[i].layer);
        DEBUG_SERIAL.print(") - ");
        DEBUG_SERIAL.println(receiverTable[i].connected ? "ACTIVE" : "MISSING");
      }
    }
    DEBUG_SERIAL.println();
    lastCount = count;
  }

  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active) {
      for (int j = 0; j < 6; j++) {
        msg[idx++] = receiverTable[i].mac[j];
      }
      for (int j = 0; j < MAX_LAYER_LENGTH; j++) {
        msg[idx++] = receiverTable[i].layer[j];
      }
      for (int j = 0; j < MAX_VERSION_LENGTH; j++) {
        msg[idx++] = receiverTable[i].version[j];
      }
      msg[idx++] = receiverTable[i].connected ? 1 : 0;
    }
  }

  msg[idx++] = SYSEX_END;

  if (countChanged || statusChanged) {
    DEBUG_SERIAL.print("[BRIDGE TX] SysEx (");
    DEBUG_SERIAL.print(idx);
    DEBUG_SERIAL.print(" bytes): ");
    for (int i = 0; i < idx; i++) {
      DEBUG_SERIAL.printf("%02X ", msg[i]);
    }
    DEBUG_SERIAL.println();
  }

  int msgIdx = 0;
  while (msgIdx < idx) {
    midiEventPacket_t packet;
    int remaining = idx - msgIdx;

    if (msgIdx == 0) {
      packet.header = 0x04;
      packet.byte1 = msg[msgIdx++];
      packet.byte2 = (remaining > 1) ? msg[msgIdx++] : 0;
      packet.byte3 = (remaining > 2) ? msg[msgIdx++] : 0;
    } else if (remaining <= 3 && msg[idx - 1] == SYSEX_END) {
      if (remaining == 1) {
        packet.header = 0x05;
        packet.byte1 = msg[msgIdx++];
        packet.byte2 = 0;
        packet.byte3 = 0;
      } else if (remaining == 2) {
        packet.header = 0x06;
        packet.byte1 = msg[msgIdx++];
        packet.byte2 = msg[msgIdx++];
        packet.byte3 = 0;
      } else {
        packet.header = 0x07;
        packet.byte1 = msg[msgIdx++];
        packet.byte2 = msg[msgIdx++];
        packet.byte3 = msg[msgIdx++];
      }
    } else {
      packet.header = 0x04;
      packet.byte1 = msg[msgIdx++];
      packet.byte2 = (remaining > 1) ? msg[msgIdx++] : 0;
      packet.byte3 = (remaining > 2) ? msg[msgIdx++] : 0;
    }

    midiWritePacket(packet);
  }
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
