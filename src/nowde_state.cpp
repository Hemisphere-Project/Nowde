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

SenderEntry senderTable[MAX_SENDERS] = {};
ReceiverEntry receiverTable[MAX_RECEIVERS] = {};

uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

unsigned long lastSenderBeacon = 0;
unsigned long lastBridgeReport = 0;

MediaSyncState mediaSyncState;

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

bool hostLinked() {
  return lastHostRxTime != 0 && (millis() - lastHostRxTime) < HOST_LINK_TIMEOUT_MS;
}

bool layerMatches(const char* subscribed, const char* packetLayer) {
  if (strncmp(subscribed, NOWDE_LAYER_WILDCARD, MAX_LAYER_LENGTH) == 0) {
    return true;
  }
  return strncmp(subscribed, packetLayer, MAX_LAYER_LENGTH) == 0;
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
