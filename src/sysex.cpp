#include "sysex.h"

#include <cstring>

#include <esp_now.h>

#include "midi.h"
#include "nowde_config.h"
#include "nowde_state.h"
#include "receiver_mode.h"
#include "sender_mode.h"
#include "storage.h"

// Forward declarations for helper functions
void sendHello();
void sendConfigState();
void sendRunningState();
void sendErrorReport(uint8_t errorCode, const uint8_t* context, uint8_t contextLength);

// 7-bit encoding helpers
// Encodes 8-bit data to 7-bit MIDI-safe format
// Every 7 bytes of input becomes 8 bytes of output (MSBs packed into first byte)
int encode7bit(const uint8_t* input, int inputLen, uint8_t* output) {
  int outIdx = 0;
  int inIdx = 0;
  
  while (inIdx < inputLen) {
    // Pack MSBs of next 7 bytes into first output byte
    uint8_t msbByte = 0;
    int chunkSize = min(7, inputLen - inIdx);
    
    for (int i = 0; i < chunkSize; i++) {
      if (input[inIdx + i] & 0x80) {
        msbByte |= (1 << i);
      }
    }
    output[outIdx++] = msbByte;
    
    // Copy 7-bit data (clear MSB)
    for (int i = 0; i < chunkSize; i++) {
      output[outIdx++] = input[inIdx++] & 0x7F;
    }
  }
  
  return outIdx;
}

// Decodes 7-bit MIDI format back to 8-bit data
int decode7bit(const uint8_t* input, int inputLen, uint8_t* output) {
  int outIdx = 0;
  int inIdx = 0;
  
  while (inIdx < inputLen) {
    // Read MSB byte
    uint8_t msbByte = input[inIdx++];
    
    // Decode up to 7 data bytes
    int chunkSize = min(7, inputLen - inIdx);
    for (int i = 0; i < chunkSize && inIdx < inputLen; i++) {
      output[outIdx] = input[inIdx++];
      // Restore MSB if it was set
      if (msbByte & (1 << i)) {
        output[outIdx] |= 0x80;
      }
      outIdx++;
    }
  }
  
  return outIdx;
}

void handleSysExMessage(const uint8_t* data, uint8_t length) {
  // Basic validation
  if (length < 2) {
    return;  // Too short to be valid
  }
  
  if (data[0] != SYSEX_START || data[length - 1] != SYSEX_END) {
    return;  // Not a valid SysEx message
  }
  
  // Silently ignore SysEx messages not for us (e.g., Universal SysEx 0x7E, system messages)
  if (length < 3 || data[1] != SYSEX_MANUFACTURER_ID) {
    return;
  }
  
  // Now validate length for our messages
  if (length < 4) {
    // Our messages must have at least: F0 7D CMD F7
    uint8_t context[3] = {0};
    for (uint8_t i = 0; i < length && i < 3; i++) {
      context[i] = data[i];
    }
    sendErrorReport(ERROR_SYSEX_PARSE_ERROR, context, length);
    return;
  }

  uint8_t command = data[2];

  switch (command) {
    case SYSEX_CMD_QUERY_CONFIG:
      // Enable sender mode if not already active
      if (!senderModeEnabled) {
        senderModeEnabled = true;
        DEBUG_SERIAL.println("\n=== SENDER MODE ACTIVATED ===");
        DEBUG_SERIAL.println("Received: QUERY_CONFIG (0x01)");
        DEBUG_SERIAL.println("Status: Broadcasting ESP-NOW beacons");
        DEBUG_SERIAL.println("=============================\n");
      } else {
        DEBUG_SERIAL.println("[QUERY_CONFIG] Received from Bridge");
      }
      
      // Always send HELLO first (so Bridge knows we're alive/ready)
      // then send current config state
      sendHello();
      delay(50);  // Delay between messages to ensure Bridge processes them separately
      sendConfigState();
      break;

    case SYSEX_CMD_PUSH_FULL_CONFIG:
      // Format: F0 7D 02 [rfSimEnabled(1)] [rfSimMaxDelayHi(1)] [rfSimMaxDelayLo(1)] F7
      if (length >= 6) {
        // Enable sender mode if not already active
        if (!senderModeEnabled) {
          senderModeEnabled = true;
          DEBUG_SERIAL.println("\n=== SENDER MODE ACTIVATED ===");
          DEBUG_SERIAL.println("Received: PUSH_FULL_CONFIG (0x02)");
          DEBUG_SERIAL.println("=============================\n");
        }
        
        rfSimulationEnabled = data[3] != 0;
        // Decode from two 7-bit bytes (MIDI SysEx compatible)
        rfSimMaxDelayMs = (static_cast<uint16_t>(data[4] & 0x7F) << 7) | (data[5] & 0x7F);
        
        DEBUG_SERIAL.println("[PUSH_FULL_CONFIG] Configuration applied");
        DEBUG_SERIAL.print("  RF Simulation: ");
        DEBUG_SERIAL.println(rfSimulationEnabled ? "ENABLED" : "DISABLED");
        DEBUG_SERIAL.print("  Max Delay: ");
        DEBUG_SERIAL.print(rfSimMaxDelayMs);
        DEBUG_SERIAL.println(" ms\n");
        
        // Acknowledge with config state
        sendConfigState();
      } else {
        sendErrorReport(ERROR_CONFIG_INVALID, nullptr, 0);
      }
      break;

    case SYSEX_CMD_QUERY_RUNNING_STATE:
      if (senderModeEnabled) {
        // Silently send running state (queried every 1s by Bridge)
        sendRunningState();
      }
      break;

    case SYSEX_CMD_MEDIA_SYNC:
      if (senderModeEnabled && length >= 27) {
        char targetLayer[MAX_LAYER_LENGTH];
        memcpy(targetLayer, &data[3], MAX_LAYER_LENGTH);
        targetLayer[MAX_LAYER_LENGTH - 1] = '\0';

        uint8_t mediaIndex = data[19];
        
        // Decode 7-bit encoded position (5 bytes -> 4 bytes)
        // Format: [MSB byte][data1][data2][data3][data4]
        uint8_t msbByte = data[20];
        uint8_t positionBytes[4];
        for (int i = 0; i < 4; i++) {
          positionBytes[i] = data[21 + i];
          if (msbByte & (1 << i)) {
            positionBytes[i] |= 0x80;
          }
        }
        
        uint32_t positionMs = (static_cast<uint32_t>(positionBytes[0]) << 24) |
                              (static_cast<uint32_t>(positionBytes[1]) << 16) |
                              (static_cast<uint32_t>(positionBytes[2]) << 8) |
                              static_cast<uint32_t>(positionBytes[3]);
        uint8_t state = data[25];
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
        syncPacket.meshTimestamp = meshTimestamp;  // Set timestamp BEFORE any delay

        int sentCount = 0;
        for (int i = 0; i < MAX_RECEIVERS; i++) {
          // Send to ALL active receivers on matching layer, regardless of connected status
          // This ensures stopped state packets reach receivers even if they stopped sending info
          if (receiverTable[i].active &&
              strncmp(receiverTable[i].layer, targetLayer, MAX_LAYER_LENGTH) == 0) {
            
            if (!rfSimulationEnabled) {
              // Normal send - no delay
              esp_now_send(receiverTable[i].mac, reinterpret_cast<uint8_t*>(&syncPacket), sizeof(syncPacket));
            } else {
              // RF simulation - add random delay
              // Find free slot in delayed packets queue
              for (int j = 0; j < MAX_DELAYED_PACKETS; j++) {
                if (!delayedPackets[j].active) {
                  unsigned long delayMs = random(0, rfSimMaxDelayMs + 1);
                  delayedPackets[j].sendTime = millis() + delayMs;
                  delayedPackets[j].packet = syncPacket;
                  memcpy(delayedPackets[j].receiverMac, receiverTable[i].mac, 6);
                  delayedPackets[j].active = true;
                  break;
                }
              }
            }
            sentCount++;
          }
        }

        // ESP-NOW TX logging disabled for media sync to reduce clutter
        // Sync packets sent every ~100ms but not logged
      }
      break;
    case SYSEX_CMD_CHANGE_RECEIVER_LAYER:
      // When received by RECEIVER via ESP-NOW from sender
      if (receiverModeEnabled && length >= 4) {
        int layerLen = min<int>(length - 4, MAX_LAYER_LENGTH - 1);
        char newLayer[MAX_LAYER_LENGTH];
        memcpy(newLayer, &data[3], layerLen);
        newLayer[layerLen] = '\0';
        
        // Update subscribed layer
        strncpy(subscribedLayer, newLayer, MAX_LAYER_LENGTH);
        subscribedLayer[MAX_LAYER_LENGTH - 1] = '\0';
        
        // Save to NVS
        saveLayerToEEPROM(subscribedLayer);
        
        DEBUG_SERIAL.println("\n=== RECEIVER LAYER CHANGED ===");
        DEBUG_SERIAL.print("New Layer: ");
        DEBUG_SERIAL.println(subscribedLayer);
        DEBUG_SERIAL.println("Status: Layer saved to EEPROM");
        DEBUG_SERIAL.println("==============================\n");
        
        // Broadcast new layer info to senders
        sendReceiverInfo();
      }
      // When received by SENDER via USB MIDI from Bridge
      // Format: F0 7D 11 [mac_encoded(7)] [layer_encoded(19)] F7 = 29 bytes
      // MAC: 6 bytes raw -> 7 bytes encoded
      // Layer: 16 bytes raw -> 19 bytes encoded
      else if (senderModeEnabled && length >= 29) {
        // Decode MAC address (7 bytes encoded -> 6 bytes raw)
        uint8_t targetMac[6];
        decode7bit(&data[3], 7, targetMac);

        // Decode layer name (19 bytes encoded -> 16 bytes raw)
        uint8_t newLayerBytes[MAX_LAYER_LENGTH];
        decode7bit(&data[10], 19, newLayerBytes);
        
        char newLayer[MAX_LAYER_LENGTH];
        memcpy(newLayer, newLayerBytes, MAX_LAYER_LENGTH);
        newLayer[MAX_LAYER_LENGTH - 1] = '\0';
        
        // Remove padding nulls for display
        int layerLen = 0;
        for (int i = 0; i < MAX_LAYER_LENGTH && newLayer[i] != '\0'; i++) {
          layerLen = i + 1;
        }

        DEBUG_SERIAL.println("\n[CHANGE_RECEIVER_LAYER] Remote layer change request");
        DEBUG_SERIAL.print("  Target MAC: ");
        for (int j = 0; j < 6; j++) {
          DEBUG_SERIAL.printf("%02X", targetMac[j]);
          if (j < 5) DEBUG_SERIAL.print(":");
        }
        DEBUG_SERIAL.println();
        DEBUG_SERIAL.print("  New Layer: '");
        DEBUG_SERIAL.print(newLayer);
        DEBUG_SERIAL.println("'");

        bool found = false;
        for (int i = 0; i < MAX_RECEIVERS; i++) {
          if (receiverTable[i].active && macEqual(receiverTable[i].mac, targetMac)) {
            found = true;
            DEBUG_SERIAL.println("  Receiver found in table, sending ESP-NOW command...");

            // Build ESP-NOW SysEx message for receiver
            uint8_t espnowMsg[32];
            int idx = 0;
            espnowMsg[idx++] = SYSEX_START;
            espnowMsg[idx++] = SYSEX_MANUFACTURER_ID;
            espnowMsg[idx++] = SYSEX_CMD_CHANGE_RECEIVER_LAYER;  // Will be handled by receiver
            memcpy(&espnowMsg[idx], newLayer, layerLen);
            idx += layerLen;
            espnowMsg[idx++] = SYSEX_END;

            esp_err_t result = esp_now_send(targetMac, espnowMsg, idx);

            if (result == ESP_OK) {
              DEBUG_SERIAL.println("  ESP-NOW send: SUCCESS\n");
            } else {
              DEBUG_SERIAL.printf("  ESP-NOW send: FAILED (error %d)\r\n\r\n", result);
              sendErrorReport(ERROR_ESPNOW_SEND_FAILED, targetMac, 6);
            }
            break;
          }
        }

        if (!found) {
          DEBUG_SERIAL.println("  ERROR: Receiver not found in active table!\n");
          sendErrorReport(ERROR_RECEIVER_TIMEOUT, targetMac, 6);
        }
      }
      break;

    default:
      DEBUG_SERIAL.printf("[SYSEX] Unknown command: 0x%02X\r\n", command);
      sendErrorReport(ERROR_SYSEX_PARSE_ERROR, &command, 1);
      break;
  }
}

// ============= HELPER FUNCTIONS =============

void sendHello() {
  // Format: F0 7D 20 [version(8,encoded:10)] [uptimeMs(4,encoded:5)] [bootReason(1)] F7
  // Sent on boot to signal sender restart
  
  uint8_t rawData[16];
  uint8_t message[64];
  int rawIdx = 0;
  int msgIdx = 0;
  
  message[msgIdx++] = SYSEX_START;
  message[msgIdx++] = SYSEX_MANUFACTURER_ID;
  message[msgIdx++] = SYSEX_CMD_HELLO;
  
  // Version string (8 bytes, padded with nulls, then encoded)
  memset(rawData, 0, 8);
  memcpy(rawData, NOWDE_VERSION, min(8, (int)strlen(NOWDE_VERSION)));
  msgIdx += encode7bit(rawData, 8, &message[msgIdx]);  // 8 bytes -> 10 bytes encoded
  rawIdx = 0;
  
  // Uptime (4 bytes raw -> 5 bytes encoded)
  unsigned long uptime = millis();
  rawData[rawIdx++] = (uptime >> 24) & 0xFF;
  rawData[rawIdx++] = (uptime >> 16) & 0xFF;
  rawData[rawIdx++] = (uptime >> 8) & 0xFF;
  rawData[rawIdx++] = uptime & 0xFF;
  msgIdx += encode7bit(rawData, rawIdx, &message[msgIdx]);
  
  // Boot reason (1 byte - ESP32 reset reason, already 7-bit safe)
  message[msgIdx++] = esp_reset_reason() & 0x7F;
  
  message[msgIdx++] = SYSEX_END;
  int idx = msgIdx;  // Total message length
  
  DEBUG_SERIAL.printf("[HELLO] Sending %d bytes\\r\\n", idx);
  
  // Send via USB MIDI
  int pos = 0;
  while (pos < idx) {
    midiEventPacket_t packet;
    memset(&packet, 0, sizeof(packet));
    
    bool hasEnd = false;
    int endPos = -1;
    for (int i = 0; i < 3 && (pos + i) < idx; i++) {
      if (message[pos + i] == SYSEX_END) {
        hasEnd = true;
        endPos = i;
        break;
      }
    }
    
    if (hasEnd) {
      if (endPos == 0) {
        packet.header = 0x05;
        packet.byte1 = message[pos];
        packet.byte2 = 0;
        packet.byte3 = 0;
      } else if (endPos == 1) {
        packet.header = 0x06;
        packet.byte1 = message[pos];
        packet.byte2 = message[pos + 1];
        packet.byte3 = 0;
      } else {
        packet.header = 0x07;
        packet.byte1 = message[pos];
        packet.byte2 = message[pos + 1];
        packet.byte3 = message[pos + 2];
      }
      pos += endPos + 1;
    } else {
      packet.header = 0x04;
      packet.byte1 = message[pos];
      packet.byte2 = (pos + 1 < idx) ? message[pos + 1] : 0;
      packet.byte3 = (pos + 2 < idx) ? message[pos + 2] : 0;
      pos += 3;
    }
    
    midiWritePacket(packet);
  }
  
  DEBUG_SERIAL.println("[HELLO] Sent to Bridge");
}

void sendConfigState() {
  // Format: F0 7D 21 [rfSimEnabled] [rfSimMaxDelayHi(7-bit)] [rfSimMaxDelayLo(7-bit)] F7
  uint8_t message[7];
  message[0] = SYSEX_START;
  message[1] = SYSEX_MANUFACTURER_ID;
  message[2] = SYSEX_CMD_CONFIG_STATE;
  message[3] = rfSimulationEnabled ? 1 : 0;
  // Encode as two 7-bit bytes (MIDI SysEx compatible, 14-bit range = 0-16383)
  message[4] = (rfSimMaxDelayMs >> 7) & 0x7F;  // Upper 7 bits
  message[5] = rfSimMaxDelayMs & 0x7F;         // Lower 7 bits
  message[6] = SYSEX_END;
  
  // Send via USB MIDI (requires chunking into MIDI packets)
  midiEventPacket_t packet;
  
  // First packet: F0 7D 20
  packet.header = 0x04;  // SysEx start
  packet.byte1 = message[0];
  packet.byte2 = message[1];
  packet.byte3 = message[2];
  midiWritePacket(packet);
  
  // Second packet: data bytes
  packet.header = 0x04;
  packet.byte1 = message[3];
  packet.byte2 = message[4];
  packet.byte3 = message[5];
  midiWritePacket(packet);
  
  // Third packet: F7 (end)
  packet.header = 0x05;  // SysEx end (single byte)
  packet.byte1 = message[6];
  packet.byte2 = 0;
  packet.byte3 = 0;
  midiWritePacket(packet);
  
  DEBUG_SERIAL.println("[CONFIG_STATE] Sent to Bridge");
}

void sendRunningState() {
  // Format: F0 7D 22 [uptimeMs(4,encoded:5)] [meshSynced(1)] [numReceivers(1)]
  //         For each receiver: [receiverData(36 bytes, encoded:42)]
  //         F7
  // All multi-byte fields are 7-bit encoded to prevent 0x80-0xFF bytes in data
  
  uint8_t rawData[256];  // Raw 8-bit data buffer
  uint8_t message[512];  // Encoded 7-bit message buffer (larger for encoded data)
  int rawIdx = 0;
  int msgIdx = 0;
  
  // Build header
  message[msgIdx++] = SYSEX_START;
  message[msgIdx++] = SYSEX_MANUFACTURER_ID;
  message[msgIdx++] = SYSEX_CMD_RUNNING_STATE;
  
  // Build raw uptime (4 bytes, big-endian)
  unsigned long uptime = millis();
  rawData[rawIdx++] = (uptime >> 24) & 0xFF;
  rawData[rawIdx++] = (uptime >> 16) & 0xFF;
  rawData[rawIdx++] = (uptime >> 8) & 0xFF;
  rawData[rawIdx++] = uptime & 0xFF;
  
  // Encode and append uptime (4 bytes -> 5 bytes encoded)
  msgIdx += encode7bit(rawData, rawIdx, &message[msgIdx]);
  rawIdx = 0;  // Reset for next field
  
  // Mesh clock synced (1 byte, safe as-is since 0 or 1)
  SyncState syncState = meshClock.getSyncState();
  bool synced = (syncState == SyncState::SYNCED);
  message[msgIdx++] = synced ? 1 : 0;
  
  // Count active receivers
  uint8_t numActive = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active) {
      numActive++;
    }
  }
  
  message[msgIdx++] = numActive;  // Safe as-is (typically small number)
  
  // Add each receiver's data (encoded)
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active) {
      rawIdx = 0;
      
      // MAC (6 bytes)
      memcpy(&rawData[rawIdx], receiverTable[i].mac, 6);
      rawIdx += 6;
      
      // Layer (16 bytes, padded with nulls)
      memcpy(&rawData[rawIdx], receiverTable[i].layer, MAX_LAYER_LENGTH);
      rawIdx += MAX_LAYER_LENGTH;
      
      // Version (8 bytes, padded with nulls)
      memcpy(&rawData[rawIdx], receiverTable[i].version, MAX_VERSION_LENGTH);
      rawIdx += MAX_VERSION_LENGTH;
      
      // Last seen (4 bytes, big-endian milliseconds ago)
      unsigned long lastSeenMs = millis() - receiverTable[i].lastSeen;
      rawData[rawIdx++] = (lastSeenMs >> 24) & 0xFF;
      rawData[rawIdx++] = (lastSeenMs >> 16) & 0xFF;
      rawData[rawIdx++] = (lastSeenMs >> 8) & 0xFF;
      rawData[rawIdx++] = lastSeenMs & 0xFF;
      
      // Active (1 byte)
      rawData[rawIdx++] = 1;
      
      // Media index (1 byte) - current playing media index (0 = stopped)
      rawData[rawIdx++] = receiverTable[i].mediaIndex;
      
      // Encode receiver data (36 bytes -> 42 bytes encoded)
      msgIdx += encode7bit(rawData, rawIdx, &message[msgIdx]);
    }
  }
  
  message[msgIdx++] = SYSEX_END;
  int idx = msgIdx;
  
  // Send via USB MIDI (chunk into packets)
  int pos = 0;
  while (pos < idx) {
    midiEventPacket_t packet;
    memset(&packet, 0, sizeof(packet));
    
    // Check if this packet will contain the end byte (0xF7)
    bool hasEnd = false;
    int endPos = -1;
    for (int i = 0; i < 3 && (pos + i) < idx; i++) {
      if (message[pos + i] == SYSEX_END) {
        hasEnd = true;
        endPos = i;
        break;
      }
    }
    
    if (hasEnd) {
      if (endPos == 0) {
        packet.header = 0x05;
        packet.byte1 = message[pos++];
        packet.byte2 = 0;
        packet.byte3 = 0;
      } else if (endPos == 1) {
        packet.header = 0x06;
        packet.byte1 = message[pos++];
        packet.byte2 = message[pos++];
        packet.byte3 = 0;
      } else {
        packet.header = 0x07;
        packet.byte1 = message[pos++];
        packet.byte2 = message[pos++];
        packet.byte3 = message[pos++];
      }
    } else {
      packet.header = 0x04;
      packet.byte1 = message[pos++];
      packet.byte2 = (pos < idx) ? message[pos++] : 0;
      packet.byte3 = (pos < idx) ? message[pos++] : 0;
    }
    
    midiWritePacket(packet);
  }
}

void sendErrorReport(uint8_t errorCode, const uint8_t* context, uint8_t contextLength) {
  // Format: F0 7D 30 [errorCode(1)] [contextLength(1)] [context...] F7
  uint8_t message[64];
  int idx = 0;
  
  message[idx++] = SYSEX_START;
  message[idx++] = SYSEX_MANUFACTURER_ID;
  message[idx++] = SYSEX_CMD_ERROR_REPORT;
  message[idx++] = errorCode;
  message[idx++] = contextLength;
  
  if (context && contextLength > 0) {
    memcpy(&message[idx], context, min<uint8_t>(contextLength, 32));
    idx += min<uint8_t>(contextLength, 32);
  }
  
  message[idx++] = SYSEX_END;
  
  // Send via USB MIDI
  int pos = 0;
  while (pos < idx) {
    midiEventPacket_t packet;
    
    if (pos == 0) {
      packet.header = 0x04;
      packet.byte1 = message[pos++];
      packet.byte2 = (pos < idx) ? message[pos++] : 0;
      packet.byte3 = (pos < idx) ? message[pos++] : 0;
    } else if (pos >= idx - 1 && message[idx - 1] == SYSEX_END) {
      if (pos == idx - 1) {
        packet.header = 0x05;
        packet.byte1 = message[pos++];
        packet.byte2 = 0;
        packet.byte3 = 0;
      } else {
        packet.header = 0x06;
        packet.byte1 = message[pos++];
        packet.byte2 = (pos < idx) ? message[pos++] : 0;
        packet.byte3 = 0;
      }
    } else {
      packet.header = 0x04;
      packet.byte1 = message[pos++];
      packet.byte2 = (pos < idx) ? message[pos++] : 0;
      packet.byte3 = (pos < idx) ? message[pos++] : 0;
    }
    
    midiWritePacket(packet);
  }
  
  const char* errorName = "UNKNOWN";
  switch (errorCode) {
    case ERROR_CONFIG_INVALID: errorName = "CONFIG_INVALID"; break;
    case ERROR_SYSEX_PARSE_ERROR: errorName = "SYSEX_PARSE_ERROR"; break;
    case ERROR_ESPNOW_SEND_FAILED: errorName = "ESPNOW_SEND_FAILED"; break;
    case ERROR_MESH_CLOCK_LOST_SYNC: errorName = "MESH_CLOCK_LOST_SYNC"; break;
    case ERROR_RECEIVER_TIMEOUT: errorName = "RECEIVER_TIMEOUT"; break;
  }
  
  DEBUG_SERIAL.printf("[ERROR_REPORT] Sent: %s (0x%02X)\r\n", errorName, errorCode);
}
