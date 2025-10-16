#include "nowde_state.h"

USBMIDI MIDI;
Preferences preferences;
ESPNowMeshClock meshClock(1000, 0.25, 10000, 5000, 10);

bool senderModeEnabled = false;
bool receiverModeEnabled = false;
char subscribedLayer[MAX_LAYER_LENGTH] = "";

SenderEntry senderTable[MAX_SENDERS] = {};
ReceiverEntry receiverTable[MAX_RECEIVERS] = {};

uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

unsigned long lastSenderBeacon = 0;
unsigned long lastBridgeReport = 0;

MediaSyncState mediaSyncState;

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
