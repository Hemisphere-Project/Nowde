#pragma once

#include <Arduino.h>
#include <cstddef>   // offsetof

// ============= VERSION & CONSTANTS =============
// 2.0.2: a relaying master now displays what it relays (its LCD/LED used to read idle).
// 2.0.1: sync hardening — media delivery decoupled from the mesh-clock gate (a stuck clock
// no longer freezes playback), active mesh re-sync + reboot self-heal, honest per-node lock
// signal. On-wire via HELLO so a flashed unit is identifiable. See docs/BENCH.md / the pass notes.
#define NOWDE_VERSION "2.0.2"
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
// Relay MEDIA_SYNC as ONE broadcast frame instead of one unicast per slave. Receivers already
// filter on the packet's layer, so this needs no receiver change. Removes the fan-out burst
// (and the class of bug where one peer silently gets nothing), and cuts master airtime ~N-fold
// -- the thing that makes LR affordable. Costs the MAC-layer ACK/retry that unicast gives:
// frame loss is then covered by the 10 Hz repeat, not by the radio. Off by default until the
// bench says otherwise; env `atoms3-bcast` builds it on.
#ifndef NOWDE_MEDIASYNC_BROADCAST
  #define NOWDE_MEDIASYNC_BROADCAST 0
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

// Per-node sync quality (2.0.1). Reported by a slave about itself; also the master's own.
//   0 = not following (stopped, freewheeling on link loss, or no fresh accepted sync)
//   1 = following-coarse (following the master's position, but the mesh clock disagrees ->
//       sub-frame precision is off; audio is still correct)
//   2 = locked (following AND mesh clock SYNCED AND compensation trustworthy)
//   0xFF = unknown (a pre-2.0.1 node that does not report it)
#define NOWDE_SYNC_NONE    0
#define NOWDE_SYNC_COARSE  1
#define NOWDE_SYNC_LOCKED  2
#define NOWDE_SYNC_UNKNOWN 0xFF

struct ReceiverInfo {
  uint8_t type = ESPNOW_MSG_RECEIVER_INFO;
  char layer[MAX_LAYER_LENGTH];
  char version[MAX_VERSION_LENGTH];
  uint8_t mediaIndex;   // Current playing media index (0 = stopped)
  uint8_t syncQuality;  // 2.0.1 trailer: NOWDE_SYNC_* — the slave's own lock, so the master
                        // can count who is actually delivering, not just who is alive.
} __attribute__((packed));
// Wire-compat: the byte is appended. A pre-2.0.1 slave sends the shorter packet; accept it
// (see handleReceiverInfo) and treat syncQuality as UNKNOWN.
#define RECEIVER_INFO_MIN_LEN ((int)offsetof(ReceiverInfo, syncQuality))

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
  uint8_t mediaIndex;   // Current playing media index (0 = stopped)
  uint8_t syncQuality = NOWDE_SYNC_UNKNOWN;  // 2.0.1: last reported by the slave (NOWDE_SYNC_*)
};

// 2.0.2: what a MASTER last relayed, for the UI only. The relay path never touches
// mediaSyncState (that is the RECEIVE state), so without this a master that is actively
// driving the mesh displays itself as idle -- "# 0 00:00" on a cyan banner. Kept separate on
// purpose: writing mediaSyncState from the relay would arm the link-loss detector against a
// lastSyncTime the master never updates, and make the master emit MTC back to its own host.
struct MasterRelayState {
  uint8_t index = 0;
  uint32_t positionMs = 0;
  uint8_t state = 0;                // 1 = playing
  unsigned long updatedAt = 0;      // millis() of the last relayed frame (0 = never)
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
  // 2.0.1: set when the last accepted MEDIA_SYNC was accepted WITHOUT mesh-clock compensation
  // (|meshNow - meshTimestamp| > CLOCK_DESYNC_THRESHOLD_MS). Position still follows the master;
  // only sub-frame precision is dropped. Drives the sync-quality signal and the re-sync self-heal.
  bool coarse = false;
};

constexpr uint8_t MTC_FRAMERATE = 30;
constexpr uint32_t LINK_LOST_TIMEOUT_MS = 10000;  // 10 seconds - increased tolerance for temporary sync gaps
// Overridable at build time so a bench build (env atoms3-coarsetest, -D...=0) can force every
// packet into the coarse path to exercise Fix A + the mesh-resync self-heal without real desync.
#ifndef CLOCK_DESYNC_THRESHOLD_MS_VAL
#define CLOCK_DESYNC_THRESHOLD_MS_VAL 200
#endif
constexpr uint32_t CLOCK_DESYNC_THRESHOLD_MS = CLOCK_DESYNC_THRESHOLD_MS_VAL;
constexpr uint32_t JUMP_FULLFRAME_THRESHOLD_MS = 1000;  // v2: full-frame when the position jumps this much

// ============= MESH RE-SYNC SELF-HEAL (2.0.1) =============
// A slave that IS receiving the master (fresh MEDIA_SYNC) but stays coarse / not mesh-locked is
// stuck on a stranded mesh-clock offset. Escalate: soft meshClock.reset() first, then a reboot.
// Only fires while packets are arriving — a plain RF outage (no packets) is freewheel, not this.
constexpr uint32_t MESH_RESYNC_GRACE_MS  = 15000;  // continuous coarse-while-receiving before a soft reset
constexpr uint8_t  MESH_MAX_SOFT_RESETS  = 2;      // soft meshClock.reset() attempts before escalating
constexpr uint32_t MESH_REBOOT_GRACE_MS  = 60000;  // continuous stuck (after soft resets) before ESP.restart()
constexpr uint8_t  MESH_MAX_AUTO_REBOOTS = 2;      // NVS-bounded auto-reboots before giving up (stay coarse+flagged)
