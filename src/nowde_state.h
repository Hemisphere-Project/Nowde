#pragma once

#include <Preferences.h>
#include <USBMIDI.h>
#include <ESPNowMeshClock.h>
#include "nowde_config.h"

#if defined(NOWDE_BOARD_ATOMS3)
#include <USBCDC.h>
extern USBCDC USBSerial;   // composite CDC next to MIDI on the single USB-C
#endif

extern USBMIDI MIDI;
extern Preferences preferences;
extern ESPNowMeshClock meshClock;

extern bool senderModeEnabled;
extern bool receiverModeEnabled;
extern char subscribedLayer[MAX_LAYER_LENGTH];

// Role: what NVS says (NOWDE_ROLE_SLAVE / MASTER / AUTO) and what it resolved to
// at boot (SLAVE / MASTER / LEGACY). See nowde_config.h.
extern uint8_t storedRole;
extern uint8_t nodeRole;
extern uint8_t boardId;
// 2.0.3: long-range PHY on/off, loaded from NVS at boot (SET_LR 0x0B stores it and restarts).
// All-or-nothing across the mesh, exactly like the former build flag.
extern bool lrEnabled;

extern SenderEntry senderTable[MAX_SENDERS];
extern ReceiverEntry receiverTable[MAX_RECEIVERS];

extern uint8_t broadcastAddress[6];

extern unsigned long lastSenderBeacon;
extern unsigned long lastBridgeReport;

extern MediaSyncState mediaSyncState;
// 2.0.2: master-side relay state, for the UI only (see MasterRelayState in nowde_config.h)
extern MasterRelayState masterRelay;
// v2.2: which master this slave follows (see OriginLock in nowde_config.h)
extern OriginLock originLock;

// Host link: last time any USB-MIDI packet came in from the host
extern unsigned long lastHostRxTime;
extern bool hostResumed;      // set when the host speaks after a silence; QUERY_RUNNING_STATE answers HELLO once
extern bool otaInProgress;

// RF Simulation for testing
extern bool rfSimulationEnabled;
extern unsigned long rfSimMaxDelayMs;

// Delayed packet structure for RF simulation
struct DelayedMediaSyncPacket {
  unsigned long sendTime;
  MediaSyncPacket packet;
  uint8_t receiverMac[6];
  bool active;
};

#define MAX_DELAYED_PACKETS 20
extern DelayedMediaSyncPacket delayedPackets[MAX_DELAYED_PACKETS];

// v2.2 controlled flooding (#t-024): last (origin, seq) already forwarded, scheduled or
// suppressed, per master this node has ever relayed for (see RelayOriginEntry, nowde_config.h).
extern RelayOriginEntry relayOrigins[MAX_RELAY_ORIGINS];

// One in-flight forward, mid its random 5-30 ms delay, waiting either to fire or to be
// suppressed if another node's copy of the same (origin, seq) is heard first (topology note
// §(2), "if you hear someone else forward it first, drop yours"). The payload is the inner
// packet, copied byte-verbatim -- never re-stamped, see the #t-020 note.
struct PendingRelay {
  bool active = false;
  unsigned long sendTime = 0;
  uint8_t origin[6] = {0};
  uint16_t seq = 0;
  uint8_t hop = 0;                                  // hop to STAMP on the outgoing envelope
  uint8_t payload[sizeof(MediaSyncPacket)] = {0};
  uint8_t payloadLen = 0;
};
extern PendingRelay pendingRelays[MAX_PENDING_RELAYS];

bool macEqual(const uint8_t* mac1, const uint8_t* mac2);
int countActiveSenders();
int countActiveReceivers();
int countConnectedReceivers();
// Connected receivers this master believes are LOCKED (syncQuality == NOWDE_SYNC_LOCKED),
// i.e. actually delivering in sync -- not merely alive. Feeds the "lock M/N" LCD line.
int countLockedReceivers();
bool hostLinked();
// This node's own sync quality (NOWDE_SYNC_*): master = LOCKED (it is the reference); a
// slave = how well it is following the master right now. The honest per-node lock signal.
uint8_t nodeSyncQuality();
// A master is "playing" for display purposes while it is still relaying fresh frames.
bool masterRelayPlaying();
// A slave subscribed to `subscribed` follows packets tagged `packetLayer`
bool layerMatches(const char* subscribed, const char* packetLayer);
// v2.2 ORIGIN LOCK. May this node act on a MEDIA_SYNC that originated at `origin`? Adopts an
// unheld lock, refreshes the held one, and refuses every other master until the held one has
// been silent for ORIGIN_LOCK_RELEASE_MS (or forever, if a host pinned it with SET_ORIGIN).
bool originAccepts(const uint8_t* origin);
// Release the lock and re-anchor the gap counter: the next MEDIA_SYNC heard adopts its origin.
void originRelease();
// Apply a role (NOWDE_ROLE_SLAVE / MASTER / LEGACY) to the running node
void applyRole(uint8_t resolvedRole);
// meshClock.meshMillis() with a guard against the library's torn 64-bit offset read
// (another task slewing _offset while we read: the value comes back off by 2^32 us, seen on
// the bench as "Delta=-4294966 ms"). Two consecutive reads must agree.
uint32_t meshMillisStable();
