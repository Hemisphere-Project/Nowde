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

extern SenderEntry senderTable[MAX_SENDERS];
extern ReceiverEntry receiverTable[MAX_RECEIVERS];

extern uint8_t broadcastAddress[6];

extern unsigned long lastSenderBeacon;
extern unsigned long lastBridgeReport;

extern MediaSyncState mediaSyncState;

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

bool macEqual(const uint8_t* mac1, const uint8_t* mac2);
int countActiveSenders();
int countActiveReceivers();
int countConnectedReceivers();
bool hostLinked();
// A slave subscribed to `subscribed` follows packets tagged `packetLayer`
bool layerMatches(const char* subscribed, const char* packetLayer);
// Apply a role (NOWDE_ROLE_SLAVE / MASTER / LEGACY) to the running node
void applyRole(uint8_t resolvedRole);
// meshClock.meshMillis() with a guard against the library's torn 64-bit offset read
// (another task slewing _offset while we read: the value comes back off by 2^32 us, seen on
// the bench as "Delta=-4294966 ms"). Two consecutive reads must agree.
uint32_t meshMillisStable();
