#include <Arduino.h>
#include <USB.h>
#include <USBMIDI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>  // For EEPROM-like storage

// ============= VERSION & CONSTANTS =============
#define NOWDE_VERSION "1.0"
#define MAX_LAYER_LENGTH 16
#define MAX_VERSION_LENGTH 8
#define RECEIVER_TIMEOUT_MS 5000
#define SENDER_TIMEOUT_MS 5000
#define RECEIVER_BEACON_INTERVAL_MS 1000
#define SENDER_BEACON_INTERVAL_MS 1000
#define BRIDGE_REPORT_INTERVAL_MS 500
#define DEFAULT_RECEIVER_LAYER "-"

// ============= LOGGING CONFIGURATION =============
#define DEBUG_SERIAL Serial



// ============= SYSEX PROTOCOL =============
// SysEx format: F0 7D <command> [data...] F7
// Using 7D as manufacturer ID (Educational/Development use)
#define SYSEX_START 0xF0
#define SYSEX_END 0xF7
#define SYSEX_MANUFACTURER_ID 0x7D
#define SYSEX_CMD_BRIDGE_CONNECTED 0x01
#define SYSEX_CMD_SUBSCRIBE_LAYER 0x02
#define SYSEX_CMD_RECEIVER_TABLE 0x03  // To Bridge: report receiver table
#define SYSEX_CMD_CHANGE_RECEIVER_LAYER 0x04  // From Bridge: change specific receiver's layer
                                               // Format: F0 7D 04 [mac(6)] [layer(16)] F7

// ============= ESP-NOW MESSAGE TYPES =============
#define ESPNOW_MSG_SENDER_BEACON 0x01
#define ESPNOW_MSG_RECEIVER_INFO 0x02

// ============= DATA STRUCTURES =============
// ESP-NOW Sender Beacon: 1 byte (type only)
struct SenderBeacon {
  uint8_t type = ESPNOW_MSG_SENDER_BEACON;
} __attribute__((packed));

// ESP-NOW Receiver Info: 1 byte type + 16 bytes layer + 8 bytes version
struct ReceiverInfo {
  uint8_t type = ESPNOW_MSG_RECEIVER_INFO;
  char layer[MAX_LAYER_LENGTH];
  char version[MAX_VERSION_LENGTH];
} __attribute__((packed));

// Sender entry (for receiver's table)
struct SenderEntry {
  uint8_t mac[6];
  unsigned long lastSeen;
  bool active;
};

// Receiver entry (for sender's table)
struct ReceiverEntry {
  uint8_t mac[6];
  char layer[MAX_LAYER_LENGTH];
  char version[MAX_VERSION_LENGTH];
  unsigned long lastSeen;
  bool active;  // true = ever registered
  bool connected;  // true = currently responding
};

// ============= GLOBAL STATE =============
// Create USB MIDI instance
USBMIDI MIDI;

// Preferences for persistent storage
Preferences preferences;

// Operating modes
bool senderModeEnabled = false;
bool receiverModeEnabled = false;
char subscribedLayer[MAX_LAYER_LENGTH] = "";

// Tables
#define MAX_SENDERS 10
#define MAX_RECEIVERS 10
SenderEntry senderTable[MAX_SENDERS];
ReceiverEntry receiverTable[MAX_RECEIVERS];

// Broadcast address
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Timing
unsigned long lastSenderBeacon = 0;
unsigned long lastReceiverBeacon = 0;
unsigned long lastBridgeReport = 0;

// SysEx buffer
uint8_t sysexBuffer[128];
uint8_t sysexIndex = 0;
bool inSysex = false;

// ============= HELPER FUNCTIONS =============

// Forward declarations
void reportReceiversToBridge();
void saveLayerToEEPROM(const char* layer);
String loadLayerFromEEPROM();
void sendReceiverInfo();

// Compare MAC addresses
bool macEqual(const uint8_t* mac1, const uint8_t* mac2) {
  for (int i = 0; i < 6; i++) {
    if (mac1[i] != mac2[i]) return false;
  }
  return true;
}

// Count active senders
int countActiveSenders() {
  int count = 0;
  for (int i = 0; i < MAX_SENDERS; i++) {
    if (senderTable[i].active) count++;
  }
  return count;
}

// Count active receivers
int countActiveReceivers() {
  int count = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active) count++;
  }
  return count;
}

// ============= SYSEX HANDLING =============

void handleSysExMessage(uint8_t* data, uint8_t length) {
  // Validate minimum length: F0 7D CMD F7 = 4 bytes
  if (length < 4) return;
  
  // Validate format
  if (data[0] != SYSEX_START || data[1] != SYSEX_MANUFACTURER_ID || data[length-1] != SYSEX_END) {
    return;
  }
  
  uint8_t command = data[2];
  
  DEBUG_SERIAL.print("[SYSEX] Command: 0x");
  DEBUG_SERIAL.print(command, HEX);
  DEBUG_SERIAL.print(" | Sender mode: ");
  DEBUG_SERIAL.print(senderModeEnabled ? "YES" : "NO");
  DEBUG_SERIAL.print(" | Receiver mode: ");
  DEBUG_SERIAL.println(receiverModeEnabled ? "YES" : "NO");
  
  switch (command) {
    case SYSEX_CMD_BRIDGE_CONNECTED:
      // Activate sender mode
      senderModeEnabled = true;
      DEBUG_SERIAL.println("\n=== SENDER MODE ACTIVATED ===");
      DEBUG_SERIAL.println("Received: Bridge Connected SysEx");
      DEBUG_SERIAL.println("Status: Broadcasting ESP-NOW beacons");
      DEBUG_SERIAL.println("=============================\n");
      
      // Immediately send current receiver table to Bridge
      reportReceiversToBridge();
      break;
      
    case SYSEX_CMD_SUBSCRIBE_LAYER:
      // Activate receiver mode with layer subscription
      if (length >= 5) {  // F0 7D 02 [layer...] F7
        receiverModeEnabled = true;
        
        // Extract layer name (max 16 chars)
        int layerLen = min(length - 4, MAX_LAYER_LENGTH - 1);
        memcpy(subscribedLayer, &data[3], layerLen);
        subscribedLayer[layerLen] = '\0';
        
        // Save to EEPROM
        saveLayerToEEPROM(subscribedLayer);
        
        DEBUG_SERIAL.println("\n=== RECEIVER LAYER CHANGED ===");
        DEBUG_SERIAL.print("New Layer: ");
        DEBUG_SERIAL.println(subscribedLayer);
        DEBUG_SERIAL.println("Status: Layer saved to EEPROM");
        DEBUG_SERIAL.println("==============================\n");
        
        // Immediately broadcast updated receiver info to sender(s)
        sendReceiverInfo();
      }
      break;
      
    case SYSEX_CMD_CHANGE_RECEIVER_LAYER:
      // Sender forwards layer change request to specific receiver
      // Format: F0 7D 04 [target_mac(6)] [layer(variable)] F7
      if (senderModeEnabled && length >= 11) {  // Minimum: F0 7D 04 + 6 MAC + at least 1 char + F7
        // Extract target MAC
        uint8_t targetMac[6];
        memcpy(targetMac, &data[3], 6);
        
        // Extract new layer (from byte 9 to before F7)
        int layerLen = min(length - 10, MAX_LAYER_LENGTH - 1);  // length-10 = skip header+MAC+end
        char newLayer[MAX_LAYER_LENGTH];
        memcpy(newLayer, &data[9], layerLen);
        newLayer[layerLen] = '\0';
        
        DEBUG_SERIAL.println("\n[SENDER] Received layer change request");
        DEBUG_SERIAL.print("  Target MAC: ");
        for (int j = 0; j < 6; j++) {
          DEBUG_SERIAL.printf("%02X", targetMac[j]);
          if (j < 5) DEBUG_SERIAL.print(":");
        }
        DEBUG_SERIAL.println();
        DEBUG_SERIAL.print("  New Layer: ");
        DEBUG_SERIAL.println(newLayer);
        DEBUG_SERIAL.print("  Searching in receiver table (");
        DEBUG_SERIAL.print(countActiveReceivers());
        DEBUG_SERIAL.println(" active receivers)...");
        
        // Find receiver in table
        bool found = false;
        for (int i = 0; i < MAX_RECEIVERS; i++) {
          if (receiverTable[i].active) {
            DEBUG_SERIAL.print("    [");
            DEBUG_SERIAL.print(i);
            DEBUG_SERIAL.print("] ");
            for (int j = 0; j < 6; j++) {
              DEBUG_SERIAL.printf("%02X", receiverTable[i].mac[j]);
              if (j < 5) DEBUG_SERIAL.print(":");
            }
            DEBUG_SERIAL.print(" - Layer: ");
            DEBUG_SERIAL.println(receiverTable[i].layer);
          }
          
          if (receiverTable[i].active && macEqual(receiverTable[i].mac, targetMac)) {
            found = true;
            DEBUG_SERIAL.println("  MATCH FOUND! Sending ESP-NOW message...");
            
            // Create layer change SysEx to send via ESP-NOW
            // Send SYSEX_CMD_SUBSCRIBE_LAYER to receiver
            uint8_t espnowMsg[32];
            int idx = 0;
            espnowMsg[idx++] = SYSEX_START;
            espnowMsg[idx++] = SYSEX_MANUFACTURER_ID;
            espnowMsg[idx++] = SYSEX_CMD_SUBSCRIBE_LAYER;
            memcpy(&espnowMsg[idx], newLayer, strlen(newLayer));
            idx += strlen(newLayer);
            espnowMsg[idx++] = SYSEX_END;
            
            esp_err_t result = esp_now_send(targetMac, espnowMsg, idx);
            
            DEBUG_SERIAL.print("  ESP-NOW Send: ");
            if (result == ESP_OK) {
              DEBUG_SERIAL.println("SUCCESS");
            } else {
              DEBUG_SERIAL.printf("FAILED (error %d)\n", result);
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
  }
}

// Callback for incoming MIDI messages - will be called manually in loop
void processMIDIData() {
  // The USBMIDI library doesn't have callbacks, we need to poll in loop
  // This will be handled differently
}

// ============= ESP-NOW CALLBACKS =============

void onDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  // Optional: handle send status if needed
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < 1) return;
  
  uint8_t msgType = data[0];
  
  // Check if this is a SysEx message (for layer changes sent via ESP-NOW)
  if (msgType == SYSEX_START) {
    // This is a SysEx message forwarded via ESP-NOW
    DEBUG_SERIAL.println("\n[ESP-NOW RX] SysEx message received");
    DEBUG_SERIAL.print("  From: ");
    for (int j = 0; j < 6; j++) {
      DEBUG_SERIAL.printf("%02X", info->src_addr[j]);
      if (j < 5) DEBUG_SERIAL.print(":");
    }
    DEBUG_SERIAL.println();
    DEBUG_SERIAL.print("  Data: ");
    for (int j = 0; j < len; j++) {
      DEBUG_SERIAL.printf("%02X ", data[j]);
    }
    DEBUG_SERIAL.printf("(%d bytes)\n", len);
    
    handleSysExMessage((uint8_t*)data, len);
    return;
  }
  
  if (msgType == ESPNOW_MSG_SENDER_BEACON) {
    bool found = false;
    int freeSlot = -1;
    
    for (int i = 0; i < MAX_SENDERS; i++) {
      if (senderTable[i].active && macEqual(senderTable[i].mac, info->src_addr)) {
        // Update existing entry
        senderTable[i].lastSeen = millis();
        found = true;
        break;
      }
      if (!senderTable[i].active && freeSlot == -1) {
        freeSlot = i;
      }
    }
    
    if (!found && freeSlot != -1) {
      // Add new sender to table
      memcpy(senderTable[freeSlot].mac, info->src_addr, 6);
      senderTable[freeSlot].lastSeen = millis();
      senderTable[freeSlot].active = true;
      
      // Add as ESP-NOW peer to send messages back
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, info->src_addr, 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;
      
      esp_err_t addResult = esp_now_add_peer(&peerInfo);
      
      DEBUG_SERIAL.println("\n[ESP-NOW RX] Sender Beacon");
      DEBUG_SERIAL.print("  From: ");
      for (int i = 0; i < 6; i++) {
        DEBUG_SERIAL.printf("%02X", info->src_addr[i]);
        if (i < 5) DEBUG_SERIAL.print(":");
      }
      DEBUG_SERIAL.println();
      DEBUG_SERIAL.println("  Action: Registered new sender");
      if (addResult == ESP_OK) {
        DEBUG_SERIAL.println("  Peer: Added to ESP-NOW");
      } else {
        DEBUG_SERIAL.printf("  Peer: Failed to add (error %d)\n", addResult);
      }
      DEBUG_SERIAL.printf("  Total Senders: %d\n\n", countActiveSenders());
    }
  }
  else if (msgType == ESPNOW_MSG_RECEIVER_INFO && senderModeEnabled) {
    // Sender mode: register/update receiver
    if (len >= sizeof(ReceiverInfo)) {
      ReceiverInfo* recvInfo = (ReceiverInfo*)data;
      
      bool found = false;
      int freeSlot = -1;
      bool changed = false;
      
      for (int i = 0; i < MAX_RECEIVERS; i++) {
        if (receiverTable[i].active && macEqual(receiverTable[i].mac, info->src_addr)) {
          // Update existing entry
          receiverTable[i].lastSeen = millis();
          
          // Check if it was missing and is now back
          if (!receiverTable[i].connected) {
            receiverTable[i].connected = true;
            changed = true;
            
            DEBUG_SERIAL.println("\n[ESP-NOW RX] Receiver RECONNECTED");
            DEBUG_SERIAL.print("  From: ");
            for (int j = 0; j < 6; j++) {
              DEBUG_SERIAL.printf("%02X", info->src_addr[j]);
              if (j < 5) DEBUG_SERIAL.print(":");
            }
            DEBUG_SERIAL.println();
            DEBUG_SERIAL.print("  Layer: ");
            DEBUG_SERIAL.println(recvInfo->layer);
            DEBUG_SERIAL.println("  Status: ACTIVE");
            DEBUG_SERIAL.println();
          }
          
          // Check if layer changed
          if (strncmp(receiverTable[i].layer, recvInfo->layer, MAX_LAYER_LENGTH) != 0) {
            strncpy(receiverTable[i].layer, recvInfo->layer, MAX_LAYER_LENGTH);
            receiverTable[i].layer[MAX_LAYER_LENGTH - 1] = '\0';
            changed = true;
            
            DEBUG_SERIAL.println("\n[ESP-NOW RX] Receiver Info Update");
            DEBUG_SERIAL.print("  From: ");
            for (int j = 0; j < 6; j++) {
              DEBUG_SERIAL.printf("%02X", info->src_addr[j]);
              if (j < 5) DEBUG_SERIAL.print(":");
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
        // Add new receiver to table
        memcpy(receiverTable[freeSlot].mac, info->src_addr, 6);
        strncpy(receiverTable[freeSlot].layer, recvInfo->layer, MAX_LAYER_LENGTH);
        receiverTable[freeSlot].layer[MAX_LAYER_LENGTH - 1] = '\0';
        strncpy(receiverTable[freeSlot].version, recvInfo->version, MAX_VERSION_LENGTH);
        receiverTable[freeSlot].version[MAX_VERSION_LENGTH - 1] = '\0';
        receiverTable[freeSlot].lastSeen = millis();
        receiverTable[freeSlot].active = true;
        receiverTable[freeSlot].connected = true;
        changed = true;
        
        // Add as ESP-NOW peer to receive future messages
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, info->src_addr, 6);
        peerInfo.channel = 0;
        peerInfo.encrypt = false;
        
        esp_err_t addResult = esp_now_add_peer(&peerInfo);
        
        DEBUG_SERIAL.println("\n[ESP-NOW RX] Receiver Info");
        DEBUG_SERIAL.print("  From: ");
        for (int i = 0; i < 6; i++) {
          DEBUG_SERIAL.printf("%02X", info->src_addr[i]);
          if (i < 5) DEBUG_SERIAL.print(":");
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
          DEBUG_SERIAL.printf("  Peer: Failed to add (error %d)\n", addResult);
        }
        DEBUG_SERIAL.printf("  Total Receivers: %d\n\n", countActiveReceivers());
      }
      
      // If changed, we'll report to bridge on next update cycle
    }
  }
}

// ============= SENDER MODE FUNCTIONS =============

void cleanupReceiverTable() {
  unsigned long now = millis();
  static uint8_t lastConnectedCount = 0;
  
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active && receiverTable[i].connected && 
        (now - receiverTable[i].lastSeen > RECEIVER_TIMEOUT_MS)) {
      // Mark as missing instead of removing
      receiverTable[i].connected = false;
      
      DEBUG_SERIAL.println("\n[TIMEOUT] Receiver marked as MISSING");
      DEBUG_SERIAL.print("  MAC: ");
      for (int j = 0; j < 6; j++) {
        DEBUG_SERIAL.printf("%02X", receiverTable[i].mac[j]);
        if (j < 5) DEBUG_SERIAL.print(":");
      }
      DEBUG_SERIAL.println();
      DEBUG_SERIAL.print("  Layer: ");
      DEBUG_SERIAL.println(receiverTable[i].layer);
      DEBUG_SERIAL.println("  Status: MISSING");
      DEBUG_SERIAL.println();
      
      // Note: Keep as ESP-NOW peer so it can reconnect automatically
    }
  }
}

void sendSenderBeacon() {
  if (!senderModeEnabled) return;
  
  static int beaconCount = 0;
  SenderBeacon beacon;
  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&beacon, sizeof(beacon));
  
  beaconCount++;
  if (beaconCount % 10 == 0) {  // Log every 10th beacon to avoid spam
    DEBUG_SERIAL.printf("[ESP-NOW TX] Sender Beacon #%d (every 10th logged)\n", beaconCount);
  }
}

void reportReceiversToBridge() {
  if (!senderModeEnabled) return;
  
  // Build SysEx message with receiver table
  // Format: F0 7D 03 [count] [mac1(6) layer1(16) version1(8) status1(1)] ... F7
  // status: 0=missing, 1=active
  
  uint8_t msg[256];
  int idx = 0;
  
  msg[idx++] = SYSEX_START;
  msg[idx++] = SYSEX_MANUFACTURER_ID;
  msg[idx++] = SYSEX_CMD_RECEIVER_TABLE;
  
  // Count active receivers (including missing ones)
  uint8_t count = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active) count++;
  }
  msg[idx++] = count;
  
  // Track if we need to log/send
  static uint8_t lastCount = 255;  // Initialize to 255 to force first send/log
  static bool lastConnectedStates[MAX_RECEIVERS] = {false};
  bool countChanged = (count != lastCount);
  bool statusChanged = false;
  
  // Check if any connection status changed
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active && receiverTable[i].connected != lastConnectedStates[i]) {
      statusChanged = true;
      lastConnectedStates[i] = receiverTable[i].connected;
    }
  }
  
  if (countChanged || statusChanged) {
    DEBUG_SERIAL.println("\n[BRIDGE REPORT] Receiver table update");
    DEBUG_SERIAL.printf("  Receivers: %d\n", count);
    for (int i = 0; i < MAX_RECEIVERS; i++) {
      if (receiverTable[i].active) {
        DEBUG_SERIAL.print("    - ");
        for (int j = 0; j < 6; j++) {
          DEBUG_SERIAL.printf("%02X", receiverTable[i].mac[j]);
          if (j < 5) DEBUG_SERIAL.print(":");
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
  
  // Add receiver data
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active) {
      // MAC address (6 bytes)
      for (int j = 0; j < 6; j++) {
        msg[idx++] = receiverTable[i].mac[j];
      }
      
      // Layer name (16 bytes)
      for (int j = 0; j < MAX_LAYER_LENGTH; j++) {
        msg[idx++] = receiverTable[i].layer[j];
      }
      
      // Version (8 bytes)
      for (int j = 0; j < MAX_VERSION_LENGTH; j++) {
        msg[idx++] = receiverTable[i].version[j];
      }
      
      // Status (1 byte): 1 = connected, 0 = missing
      msg[idx++] = receiverTable[i].connected ? 1 : 0;
    }
  }
  
  msg[idx++] = SYSEX_END;
  
  // Debug: Print the SysEx message being sent
  if (countChanged || statusChanged) {
    DEBUG_SERIAL.print("[BRIDGE TX] SysEx (");
    DEBUG_SERIAL.print(idx);
    DEBUG_SERIAL.print(" bytes): ");
    for (int i = 0; i < idx; i++) {
      DEBUG_SERIAL.printf("%02X ", msg[i]);
    }
    DEBUG_SERIAL.println();
  }
  
  // Always send the message (not just when count changes)
  // Send via MIDI using writePacket
  // Break into USB MIDI packets (3 bytes per packet + header)
  int msgIdx = 0;
  while (msgIdx < idx) {
    midiEventPacket_t packet;
    int remaining = idx - msgIdx;
    
    if (msgIdx == 0) {
      // First packet with SysEx start
      packet.header = 0x04;  // SysEx start or continue
      packet.byte1 = msg[msgIdx++];
      packet.byte2 = (remaining > 1) ? msg[msgIdx++] : 0;
      packet.byte3 = (remaining > 2) ? msg[msgIdx++] : 0;
    } else if (remaining <= 3 && msg[idx-1] == SYSEX_END) {
      // Last packet
      if (remaining == 1) {
        packet.header = 0x05;  // SysEx ends with 1 byte
        packet.byte1 = msg[msgIdx++];
        packet.byte2 = 0;
        packet.byte3 = 0;
      } else if (remaining == 2) {
        packet.header = 0x06;  // SysEx ends with 2 bytes
        packet.byte1 = msg[msgIdx++];
        packet.byte2 = msg[msgIdx++];
        packet.byte3 = 0;
      } else {
        packet.header = 0x07;  // SysEx ends with 3 bytes
        packet.byte1 = msg[msgIdx++];
        packet.byte2 = msg[msgIdx++];
        packet.byte3 = msg[msgIdx++];
      }
    } else {
      // Continue packet
      packet.header = 0x04;  // SysEx continue
      packet.byte1 = msg[msgIdx++];
      packet.byte2 = (remaining > 1) ? msg[msgIdx++] : 0;
      packet.byte3 = (remaining > 2) ? msg[msgIdx++] : 0;
    }
    
    MIDI.writePacket(&packet);
  }
}

// ============= RECEIVER MODE FUNCTIONS =============

void cleanupSenderTable() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_SENDERS; i++) {
    if (senderTable[i].active && (now - senderTable[i].lastSeen > SENDER_TIMEOUT_MS)) {
      DEBUG_SERIAL.println("\n[TIMEOUT] Sender removed");
      DEBUG_SERIAL.print("  MAC: ");
      for (int j = 0; j < 6; j++) {
        DEBUG_SERIAL.printf("%02X", senderTable[i].mac[j]);
        if (j < 5) DEBUG_SERIAL.print(":");
      }
      DEBUG_SERIAL.println();
      DEBUG_SERIAL.printf("  Remaining: %d\n\n", countActiveSenders() - 1);
      
      // Remove from ESP-NOW peers
      esp_now_del_peer(senderTable[i].mac);
      
      senderTable[i].active = false;
    }
  }
}

void sendReceiverInfo() {
  if (!receiverModeEnabled || strlen(subscribedLayer) == 0) return;
  
  ReceiverInfo info;
  strncpy(info.layer, subscribedLayer, MAX_LAYER_LENGTH);
  info.layer[MAX_LAYER_LENGTH - 1] = '\0';
  strncpy(info.version, NOWDE_VERSION, MAX_VERSION_LENGTH);
  info.version[MAX_VERSION_LENGTH - 1] = '\0';
  
  static int infoCount = 0;
  int sentCount = 0;
  
  // Send to all active senders
  for (int i = 0; i < MAX_SENDERS; i++) {
    if (senderTable[i].active) {
      esp_now_send(senderTable[i].mac, (uint8_t*)&info, sizeof(info));
      sentCount++;
    }
  }
  
  infoCount++;
  if (infoCount % 10 == 0 && sentCount > 0) {  // Log every 10th send to avoid spam
    DEBUG_SERIAL.printf("[ESP-NOW TX] Receiver Info #%d to %d sender(s) (every 10th logged)\n", 
                  infoCount, sentCount);
    DEBUG_SERIAL.printf("  Layer: %s\n", subscribedLayer);
    DEBUG_SERIAL.printf("  Version: %s\n", NOWDE_VERSION);
  }
}

// ============= EEPROM / PREFERENCES FUNCTIONS =============

void saveLayerToEEPROM(const char* layer) {
  preferences.begin("nowde", false);  // Open in RW mode
  preferences.putString("layer", layer);
  preferences.end();
  
  DEBUG_SERIAL.println("[EEPROM] Layer saved");
}

String loadLayerFromEEPROM() {
  // Try to open preferences namespace (may not exist on first boot)
  if (!preferences.begin("nowde", true)) {
    // Namespace doesn't exist yet (first boot) - return default
    DEBUG_SERIAL.println("[EEPROM] No saved data found (first boot)");
    return String(DEFAULT_RECEIVER_LAYER);
  }
  
  String layer = preferences.getString("layer", DEFAULT_RECEIVER_LAYER);
  preferences.end();
  
  DEBUG_SERIAL.print("[EEPROM] Loaded layer: ");
  DEBUG_SERIAL.println(layer);
  
  return layer;
}

// ============= SETUP =============

void setup() {
  // Initialize Serial for debug
  DEBUG_SERIAL.begin(115200);
  delay(500);  // Wait for serial to stabilize
  
  // Print startup banner
  DEBUG_SERIAL.println("\n\n");
  DEBUG_SERIAL.println("╔════════════════════════════════════╗");
  DEBUG_SERIAL.println("║         NOWDE ESP-NOW v1.0         ║");
  DEBUG_SERIAL.println("║    Hemisphere Project © 2025       ║");
  DEBUG_SERIAL.println("╚════════════════════════════════════╝");
  DEBUG_SERIAL.println();
  
  // Set custom USB device names
  USB.VID(0x303A);  // Espressif VID
  USB.PID(0x8000);  // Custom PID
  USB.productName("Nowde 1.0");
  USB.manufacturerName("Hemisphere");
  
  // Initialize USB
  USB.begin();
  DEBUG_SERIAL.println("[INIT] USB initialized");
  
  // Initialize USB MIDI
  MIDI.begin();
  DEBUG_SERIAL.println("[INIT] USB MIDI initialized");
  
  // Wait for USB to be ready
  delay(1000);
  
  // Initialize WiFi in STA mode for ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  DEBUG_SERIAL.println("[INIT] WiFi STA mode configured");
  
  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    DEBUG_SERIAL.println("[ERROR] ESP-NOW init failed!");
    return;
  }
  DEBUG_SERIAL.println("[INIT] ESP-NOW initialized");
  
  // Register callbacks
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  DEBUG_SERIAL.println("[INIT] ESP-NOW callbacks registered");
  
  // Add broadcast peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    DEBUG_SERIAL.println("[ERROR] Failed to add broadcast peer!");
    return;
  }
  DEBUG_SERIAL.println("[INIT] Broadcast peer added");
  
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("═══════════════════════════════════");
  DEBUG_SERIAL.print("Device MAC: ");
  DEBUG_SERIAL.println(WiFi.macAddress());
  DEBUG_SERIAL.print("Version: ");
  DEBUG_SERIAL.println(NOWDE_VERSION);
  DEBUG_SERIAL.println("═══════════════════════════════════");
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("Waiting for USB MIDI commands...");
  DEBUG_SERIAL.println("  - Bridge Connected: Activates sender mode");
  DEBUG_SERIAL.println("  - Subscribe Layer: Activates receiver mode");
  DEBUG_SERIAL.println();

  // Load saved layer from EEPROM (or use default)
  String savedLayer = loadLayerFromEEPROM();
  
  // Check if we should auto-start receiver mode
  if (savedLayer.length() > 0) {
    receiverModeEnabled = true;
    strncpy(subscribedLayer, savedLayer.c_str(), MAX_LAYER_LENGTH);
    subscribedLayer[MAX_LAYER_LENGTH - 1] = '\0';
    DEBUG_SERIAL.println("[INIT] Auto-starting receiver mode");
    DEBUG_SERIAL.print("  Subscribed Layer: ");
    DEBUG_SERIAL.println(subscribedLayer);
    DEBUG_SERIAL.println("  Source: EEPROM");
    DEBUG_SERIAL.println();
  }
}

// ============= MIDI PROCESSING =============

// Process incoming USB MIDI for SysEx messages
void processMIDI() {
  midiEventPacket_t packet;
  
  while (MIDI.readPacket(&packet)) {
    // Check if this is a SysEx message
    // USB MIDI SysEx packets have special CIN codes
    // The CIN is in the lower nibble of the header byte (bits 0-3)
    uint8_t cin = packet.header & 0x0F;
    
    // CIN 0x4 = SysEx starts or continues
    // CIN 0x5 = SysEx ends with 1 byte
    // CIN 0x6 = SysEx ends with 2 bytes  
    // CIN 0x7 = SysEx ends with 3 bytes
    if (cin >= 0x4 && cin <= 0x7) {
      
      // Process bytes from packet
      if (packet.byte1 != 0) {
        if (packet.byte1 == SYSEX_START) {
          inSysex = true;
          sysexIndex = 0;
          sysexBuffer[sysexIndex++] = packet.byte1;
        } else if (inSysex) {
          if (sysexIndex < sizeof(sysexBuffer)) {
            sysexBuffer[sysexIndex++] = packet.byte1;
          }
          if (packet.byte1 == SYSEX_END) {
            DEBUG_SERIAL.print("[SYSEX RX] ");
            for (int i = 0; i < sysexIndex; i++) {
              DEBUG_SERIAL.printf("%02X ", sysexBuffer[i]);
            }
            DEBUG_SERIAL.printf("(%d bytes)\n", sysexIndex);
            handleSysExMessage(sysexBuffer, sysexIndex);
            inSysex = false;
            sysexIndex = 0;
          }
        }
      }
      
      if (packet.byte2 != 0 && inSysex) {
        if (sysexIndex < sizeof(sysexBuffer)) {
          sysexBuffer[sysexIndex++] = packet.byte2;
        }
        if (packet.byte2 == SYSEX_END) {
          DEBUG_SERIAL.print("[SYSEX RX] ");
          for (int i = 0; i < sysexIndex; i++) {
            DEBUG_SERIAL.printf("%02X ", sysexBuffer[i]);
          }
          DEBUG_SERIAL.printf("(%d bytes)\n", sysexIndex);
          handleSysExMessage(sysexBuffer, sysexIndex);
          inSysex = false;
          sysexIndex = 0;
        }
      }
      
      if (packet.byte3 != 0 && inSysex) {
        if (sysexIndex < sizeof(sysexBuffer)) {
          sysexBuffer[sysexIndex++] = packet.byte3;
        }
        if (packet.byte3 == SYSEX_END) {
          DEBUG_SERIAL.print("[SYSEX RX] ");
          for (int i = 0; i < sysexIndex; i++) {
            DEBUG_SERIAL.printf("%02X ", sysexBuffer[i]);
          }
          DEBUG_SERIAL.printf("(%d bytes)\n", sysexIndex);
          handleSysExMessage(sysexBuffer, sysexIndex);
          inSysex = false;
          sysexIndex = 0;
        }
      }
    }
  }
}

// ============= LOOP =============

void loop() {
  unsigned long now = millis();
  
  // Process incoming MIDI messages
  processMIDI();
  
  // Sender mode tasks
  if (senderModeEnabled) {
    // Send beacon periodically
    if (now - lastSenderBeacon >= SENDER_BEACON_INTERVAL_MS) {
      lastSenderBeacon = now;
      sendSenderBeacon();
    }
    
    // Cleanup old receivers
    cleanupReceiverTable();
    
    // Report to Bridge periodically
    if (now - lastBridgeReport >= BRIDGE_REPORT_INTERVAL_MS) {
      lastBridgeReport = now;
      reportReceiversToBridge();
    }
  }
  
  // Receiver mode tasks
  if (receiverModeEnabled) {
    // Send info periodically with random offset to avoid collisions
    static unsigned long nextBeacon = 0;
    if (now >= nextBeacon) {
      sendReceiverInfo();
      nextBeacon = now + RECEIVER_BEACON_INTERVAL_MS + random(0, 200);
    }
    
    // Cleanup old senders
    cleanupSenderTable();
  }
  
  delay(10);
}
