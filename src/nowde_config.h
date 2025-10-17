#pragma once

#include <Arduino.h>

// ============= VERSION & CONSTANTS =============
#define NOWDE_VERSION "1.0"
#define MAX_LAYER_LENGTH 16
#define MAX_VERSION_LENGTH 8
#define MAX_SENDERS 10
#define MAX_RECEIVERS 10
#define RECEIVER_TIMEOUT_MS 5000
#define SENDER_TIMEOUT_MS 5000
#define RECEIVER_BEACON_INTERVAL_MS 1000
#define SENDER_BEACON_INTERVAL_MS 1000
#define BRIDGE_REPORT_INTERVAL_MS 500
#define DEFAULT_RECEIVER_LAYER "-"

// ============= LOGGING CONFIGURATION =============
#define DEBUG_SERIAL Serial

// ============= MESH CLOCK SYNC =============
#define TRANSMISSION_DELAY_US 1300

// ============= SYSEX PROTOCOL =============
#define SYSEX_START 0xF0
#define SYSEX_END 0xF7
#define SYSEX_MANUFACTURER_ID 0x7D

// Bridge → Nowde Direct (0x01-0x0F)
#define SYSEX_CMD_QUERY_CONFIG 0x01
#define SYSEX_CMD_PUSH_FULL_CONFIG 0x02
#define SYSEX_CMD_QUERY_RUNNING_STATE 0x03

// Bridge → Receivers via Sender (0x10-0x1F)
#define SYSEX_CMD_MEDIA_SYNC 0x10
#define SYSEX_CMD_CHANGE_RECEIVER_LAYER 0x11

// Nowde → Bridge Responses (0x20-0x3F)
#define SYSEX_CMD_HELLO 0x20
#define SYSEX_CMD_CONFIG_STATE 0x21
#define SYSEX_CMD_RUNNING_STATE 0x22
#define SYSEX_CMD_ERROR_REPORT 0x30

// Error codes for ERROR_REPORT
#define ERROR_CONFIG_INVALID 0x01
#define ERROR_SYSEX_PARSE_ERROR 0x02
#define ERROR_ESPNOW_SEND_FAILED 0x03
#define ERROR_MESH_CLOCK_LOST_SYNC 0x04
#define ERROR_RECEIVER_TIMEOUT 0x05
#define ERROR_UNKNOWN 0xFF

// ============= ESP-NOW MESSAGE TYPES =============
#define ESPNOW_MSG_SENDER_BEACON 0x01
#define ESPNOW_MSG_RECEIVER_INFO 0x02
#define ESPNOW_MSG_MEDIA_SYNC 0x03

// ============= DATA STRUCTURES =============
struct SenderBeacon {
  uint8_t type = ESPNOW_MSG_SENDER_BEACON;
} __attribute__((packed));

struct ReceiverInfo {
  uint8_t type = ESPNOW_MSG_RECEIVER_INFO;
  char layer[MAX_LAYER_LENGTH];
  char version[MAX_VERSION_LENGTH];
  uint8_t mediaIndex;  // Current playing media index (0 = stopped)
} __attribute__((packed));

struct MediaSyncPacket {
  uint8_t type = ESPNOW_MSG_MEDIA_SYNC;
  char layer[MAX_LAYER_LENGTH];
  uint8_t mediaIndex;
  uint32_t positionMs;
  uint8_t state;
  uint32_t meshTimestamp;
} __attribute__((packed));

struct SenderEntry {
  uint8_t mac[6];
  unsigned long lastSeen;
  bool active;
};

struct ReceiverEntry {
  uint8_t mac[6];
  char layer[MAX_LAYER_LENGTH];
  char version[MAX_VERSION_LENGTH];
  unsigned long lastSeen;
  bool active;
  bool connected;
  uint8_t mediaIndex;  // Current playing media index (0 = stopped)
};

struct MediaSyncState {
  uint8_t currentIndex = 0;
  uint32_t currentPositionMs = 0;
  uint8_t currentState = 0;
  unsigned long lastSyncTime = 0;
  unsigned long localClockStartTime = 0;  // When local clock started running
  unsigned long lastMTCUpdateTime = 0;    // Last MTC send time
  bool linkLost = false;
  bool stopOnLinkLost = true;  // Configurable: stop or continue on link lost
  uint8_t lastSentIndex = 255;
};

constexpr uint8_t MTC_FRAMERATE = 30;
constexpr uint32_t LINK_LOST_TIMEOUT_MS = 3000;
constexpr uint32_t CLOCK_DESYNC_THRESHOLD_MS = 200;
