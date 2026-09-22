#include "nowde_state.h"

#include <cstring>
#include <esp_now.h>

#if defined(NOWDE_BOARD_ATOMS3)
USBCDC USBSerial(0);
#endif

USBMIDI MIDI;
Preferences preferences;
ESPNowMeshClock meshClock(1000, 0.25, 10000, 5000, 10);

bool senderModeEnabled = false;
bool receiverModeEnabled = false;
char subscribedLayer[MAX_LAYER_LENGTH] = "";

uint8_t storedRole = NOWDE_ROLE_AUTO;
uint8_t nodeRole = NOWDE_ROLE_LEGACY;
uint8_t boardId = NOWDE_BOARDID_UNKNOWN;
bool lrEnabled = NOWDE_WIFI_LR != 0;   // overwritten from NVS in setup()

SenderEntry senderTable[MAX_SENDERS] = {};
ReceiverEntry receiverTable[MAX_RECEIVERS] = {};

uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

unsigned long lastSenderBeacon = 0;
unsigned long lastBridgeReport = 0;

MediaSyncState mediaSyncState;
MasterRelayState masterRelay;
OriginLock originLock;

unsigned long lastHostRxTime = 0;
bool hostResumed = false;
bool otaInProgress = false;

// RF Simulation state
bool rfSimulationEnabled = false;
unsigned long rfSimMaxDelayMs = 400; // Default max delay 400ms

DelayedMediaSyncPacket delayedPackets[MAX_DELAYED_PACKETS] = {};

bool macEqual(const uint8_t* mac1, const uint8_t* mac2) {
  for (int i = 0; i < 6; i++) {
    if (mac1[i] != mac2[i]) {
      return false;
    }
  }
  return true;
}

int countActiveSenders() {
  int count = 0;
  for (int i = 0; i < MAX_SENDERS; i++) {
    if (senderTable[i].active) {
      count++;
    }
  }
  return count;
}

int countActiveReceivers() {
  int count = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active) {
      count++;
    }
  }
  return count;
}

int countConnectedReceivers() {
  int count = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active && receiverTable[i].connected) {
      count++;
    }
  }
  return count;
}

int countLockedReceivers() {
  int count = 0;
  for (int i = 0; i < MAX_RECEIVERS; i++) {
    if (receiverTable[i].active && receiverTable[i].connected &&
        receiverTable[i].syncQuality == NOWDE_SYNC_LOCKED) {
      count++;
    }
  }
  return count;
}

bool hostLinked() {
  return lastHostRxTime != 0 && (millis() - lastHostRxTime) < HOST_LINK_TIMEOUT_MS;
}

uint8_t nodeSyncQuality() {
  // The master is the clock reference -- it does not "follow", so it is trivially locked.
  if (nodeRole == NOWDE_ROLE_MASTER) {
    return NOWDE_SYNC_LOCKED;
  }
  // Slave / legacy: quality = how well we are following the master right now.
  if (mediaSyncState.currentState != 1) {
    return NOWDE_SYNC_NONE;                       // stopped: not following anything
  }
  if (mediaSyncState.linkLost) {
    return NOWDE_SYNC_NONE;                       // freewheeling on link loss, master gone
  }
  if ((millis() - mediaSyncState.lastSyncTime) > LINK_LOST_TIMEOUT_MS) {
    return NOWDE_SYNC_NONE;                       // no fresh accepted sync
  }
  // Following the master. Precise only if the mesh clock agrees (compensation was trustworthy).
  if (mediaSyncState.coarse || meshClock.getSyncState() != SyncState::SYNCED) {
    return NOWDE_SYNC_COARSE;
  }
  return NOWDE_SYNC_LOCKED;
}

bool masterRelayPlaying() {
  // The host streams at 10 Hz while playing and 1 Hz while stopped; treat a gap as idle so a
  // master whose host went away stops claiming to play.
  return masterRelay.state == 1 && masterRelay.updatedAt != 0 &&
         (millis() - masterRelay.updatedAt) < 3000;
}

bool layerMatches(const char* subscribed, const char* packetLayer) {
  if (strncmp(subscribed, NOWDE_LAYER_WILDCARD, MAX_LAYER_LENGTH) == 0) {
    return true;
  }
  return strncmp(subscribed, packetLayer, MAX_LAYER_LENGTH) == 0;
}

static void logOriginLock(const char* what, const uint8_t* mac) {
  DEBUG_SERIAL.printf("[ORIGIN] %s %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                      what, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void originRelease() {
  originLock.valid = false;
  originLock.pinned = false;
  memset(originLock.mac, 0, sizeof(originLock.mac));
  // The next master's seq starts wherever it starts: diffing against the old one's would
  // charge the difference to the counter as thousands of lost frames.
  mediaSyncState.haveSeq = false;
}

bool originAccepts(const uint8_t* origin) {
  unsigned long now = millis();

  if (!originLock.valid) {
    memcpy(originLock.mac, origin, 6);
    originLock.valid = true;
    originLock.lastHeard = now;
    mediaSyncState.haveSeq = false;
    logOriginLock("locked to", origin);
    return true;
  }

  if (macEqual(originLock.mac, origin)) {
    originLock.lastHeard = now;
    return true;
  }

  // A different master. Under unicast the receiver table kept installations apart; under
  // broadcast nothing does, so a `*` slave in earshot of two of them would alternate. Hold
  // the one we have until it has actually gone quiet -- and forever if a host pinned it.
  if (!originLock.pinned && (now - originLock.lastHeard) > ORIGIN_LOCK_RELEASE_MS) {
    logOriginLock("released, switching to", origin);
    memcpy(originLock.mac, origin, 6);
    originLock.lastHeard = now;
    mediaSyncState.haveSeq = false;
    return true;
  }

  static unsigned long lastIgnoreLog = 0;
  if (now - lastIgnoreLog > 5000) {
    lastIgnoreLog = now;
    logOriginLock("ignoring foreign master", origin);
  }
  return false;
}

void applyRole(uint8_t resolvedRole) {
  nodeRole = resolvedRole;
  switch (resolvedRole) {
    case NOWDE_ROLE_MASTER:
      // Sender from boot: beacon right away so slaves lock the mesh before the host speaks.
      senderModeEnabled = true;
      break;
    case NOWDE_ROLE_SLAVE:
      // Never a sender, whatever the host sends (QUERY_CONFIG only answers HELLO).
      if (senderModeEnabled) {
        for (int i = 0; i < MAX_RECEIVERS; i++) {
          if (receiverTable[i].active) {
            esp_now_del_peer(receiverTable[i].mac);
          }
          receiverTable[i] = ReceiverEntry{};
        }
      }
      senderModeEnabled = false;
      break;
    case NOWDE_ROLE_LEGACY:
    default:
      // v1.2 behaviour: receiver now, sender when the host handshakes.
      break;
  }
  receiverModeEnabled = true;
}

uint32_t meshMillisStable() {
  uint32_t a = meshClock.meshMillis();
  for (int i = 0; i < 4; i++) {
    uint32_t b = meshClock.meshMillis();
    if (static_cast<int32_t>(b - a) < 100) {   // 32-bit ms wrap-safe; a torn read is off by ~4295 s
      return b;
    }
    a = b;
  }
  return a;
}
