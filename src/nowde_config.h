#pragma once

#include <Arduino.h>

// ============= VERSION & CONSTANTS =============
#define NOWDE_VERSION "2.0"
#define MAX_LAYER_LENGTH 16
#define MAX_VERSION_LENGTH 8
#define MAX_SENDERS 10
#define MAX_RECEIVERS 10
#define RECEIVER_TIMEOUT_MS 5000
#define SENDER_TIMEOUT_MS 5000
#define RECEIVER_BEACON_INTERVAL_MS 1000
#define SENDER_BEACON_INTERVAL_MS 1000
#define BRIDGE_REPORT_INTERVAL_MS 500

// ============= BOARD =============
// Selected per PlatformIO env with -DNOWDE_BOARD_ATOMS3 (M5Stack AtomS3 / AtomS3 Lite,
// single native USB-C, composite MIDI + CDC) or nothing (ESP32-S3 DevKitC-1: native USB
// for MIDI, UART0 for logs — the MillluBridge baseline).
#if defined(NOWDE_BOARD_ATOMS3)
  #define NOWDE_BOARD_NAME "atoms3"
  #include "usb_out.h"
  #define DEBUG_SERIAL usbLog         // ring buffer -> USBCDC, pumped by the MIDI task (usb_out.h)
  #define NOWDE_HAS_UI 1
#else
  #define NOWDE_BOARD_NAME "devkit"
  #define DEBUG_SERIAL Serial         // UART0
  #define NOWDE_HAS_UI 0
#endif

// Board ids reported in HELLO / CONFIG_STATE
#define NOWDE_BOARDID_UNKNOWN     0
#define NOWDE_BOARDID_DEVKIT      1
#define NOWDE_BOARDID_ATOMS3      2
#define NOWDE_BOARDID_ATOMS3_LITE 3

// ============= ROLE =============
// Stored in NVS ("nowde"/"role"). AUTO = decide from the board at boot:
//   AtomS3 (LCD)      -> master
//   AtomS3 Lite (LED) -> slave
//   DevKit / unknown  -> legacy: receiver at boot, sender when the host handshakes (v1.2)
#define NOWDE_ROLE_SLAVE  0
#define NOWDE_ROLE_MASTER 1
#define NOWDE_ROLE_LEGACY 2         // resolved value only, never stored
#define NOWDE_ROLE_AUTO   0x7F
#ifndef NOWDE_ROLE_DEFAULT
  #define NOWDE_ROLE_DEFAULT NOWDE_ROLE_AUTO
#endif

// ============= LAYERS =============
// "-" = unassigned (MillluBridge convention, the GUI assigns one).
// "*" = wildcard: a slave on "*" follows any layer; a master sends to "*" slaves too.
#define NOWDE_LAYER_UNASSIGNED "-"
#define NOWDE_LAYER_WILDCARD   "*"
#ifndef NOWDE_DEFAULT_LAYER
  #if defined(NOWDE_BOARD_ATOMS3)
    #define NOWDE_DEFAULT_LAYER NOWDE_LAYER_WILDCARD
  #else
    #define NOWDE_DEFAULT_LAYER NOWDE_LAYER_UNASSIGNED
  #endif
#endif
#define DEFAULT_RECEIVER_LAYER NOWDE_DEFAULT_LAYER

// ============= RADIO =============
#ifndef NOWDE_WIFI_CHANNEL
  #define NOWDE_WIFI_CHANNEL 1        // every node must sit on the same channel
#endif
// Long-range PHY (ESP-NOW proprietary, 512/256 kbps): roughly +8-10 dB of receiver
// sensitivity, which is what buys a path through foliage or masonry. The catch is that
// it is LR-*only* -- a node without this build cannot demodulate LR frames at all, so
// either the whole mesh carries it or none of it does. A half-flashed fleet does not
// degrade, it silently loses the nodes you missed (watch the master's `slaves N`).
// Off by default; env `atoms3-lr` builds it on. See docs/BENCH.md section 6.
#ifndef NOWDE_WIFI_LR
  #define NOWDE_WIFI_LR 0
#endif
// 250K trades throughput we do not need for the most link budget; 500K is the milder step.
#ifndef NOWDE_LR_RATE
  #define NOWDE_LR_RATE WIFI_PHY_RATE_LORA_250K
#endif

// ============= MEDIA SYNC CONFIGURATION =============
// Interval for repeating CC#100 while playing (0 = disable auto-repeat)
#define CC100_REPEAT_INTERVAL_MS 1000

// What a slave does when the media-sync stream dies (LINK_LOST_TIMEOUT_MS with no packet
// while playing):
//   1 = STOP      -- stop the clock, send CC#100=0 + MIDI Stop. The v1.2 / MillluBridge
//                    behaviour: the host goes visibly idle rather than drifting unattended.
//   0 = FREEWHEEL -- keep regenerating MTC from the mesh clock and let the stream re-correct
//                    the position when it returns. A 10 s RF gap costs a few ms of drift
//                    instead of an audible stop.
// Freewheel is the right call for a long audio loop in the open (foliage fades come and go);
// stop is right for a video wall that must not run blind. Env `atoms3` ships FREEWHEEL, the
// DevKit / v1.2 baseline keeps STOP. Boot log prints which one is live.
#ifndef NOWDE_STOP_ON_LINK_LOST
  #define NOWDE_STOP_ON_LINK_LOST 1
#endif

// ============= HOST LINK =============
// A host is considered linked while it has sent us anything within this window
// (HPlayer2 polls QUERY_RUNNING_STATE every 2 s as a keepalive; the Bridge every 1 s).
#define HOST_LINK_TIMEOUT_MS 5000

// ============= MESH CLOCK SYNC =============
#ifndef TRANSMISSION_DELAY_US          // ESPNowMeshClock defines it too
#define TRANSMISSION_DELAY_US 1300
#endif

// ============= SYSEX PROTOCOL =============
#define SYSEX_START 0xF0
#define SYSEX_END 0xF7
#define SYSEX_MANUFACTURER_ID 0x7D

// Bridge → Nowde Direct (0x01-0x0F)
#define SYSEX_CMD_QUERY_CONFIG 0x01
#define SYSEX_CMD_PUSH_FULL_CONFIG 0x02
#define SYSEX_CMD_QUERY_RUNNING_STATE 0x03
#define SYSEX_CMD_ENTER_BOOTLOADER 0x04  // Deprecated - use OTA instead
#define SYSEX_CMD_OTA_BEGIN 0x05
#define SYSEX_CMD_OTA_DATA 0x06
#define SYSEX_CMD_OTA_END 0x07
#define SYSEX_CMD_SET_ROLE 0x08          // v2: F0 7D 08 role F7 (0 slave, 1 master, 7F auto)
#define SYSEX_CMD_SET_LOCAL_LAYER 0x09   // v2: F0 7D 09 layer(ascii) F7 — this node's own layer
#define SYSEX_CMD_SET_LOG 0x0A           // v2: F0 7D 0A on(1) F7 — stream the node log as LOG frames

// Bridge → Receivers via Sender (0x10-0x1F)
#define SYSEX_CMD_MEDIA_SYNC 0x10
#define SYSEX_CMD_CHANGE_RECEIVER_LAYER 0x11

// Nowde → Bridge Responses (0x20-0x3F)
#define SYSEX_CMD_HELLO 0x20
#define SYSEX_CMD_CONFIG_STATE 0x21
#define SYSEX_CMD_RUNNING_STATE 0x22
#define SYSEX_CMD_OTA_ACK 0x23
#define SYSEX_CMD_ERROR_REPORT 0x30
#define SYSEX_CMD_LOG 0x31               // v2: F0 7D 31 text(ascii) F7 — one log line, after SET_LOG 1

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
#define ESPNOW_MSG_MIDI_EVENT 0x04       // v2.1: reserved (Note/CC relay scheduled on mesh time)

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
  bool stopOnLinkLost = NOWDE_STOP_ON_LINK_LOST;  // build-time, see NOWDE_STOP_ON_LINK_LOST
  uint8_t lastSentIndex = 255;
  unsigned long lastCC100SendTime = 0;    // Last time CC#100 was sent
};

constexpr uint8_t MTC_FRAMERATE = 30;
constexpr uint32_t LINK_LOST_TIMEOUT_MS = 10000;  // 10 seconds - increased tolerance for temporary sync gaps
constexpr uint32_t CLOCK_DESYNC_THRESHOLD_MS = 200;
constexpr uint32_t JUMP_FULLFRAME_THRESHOLD_MS = 1000;  // v2: full-frame when the position jumps this much
