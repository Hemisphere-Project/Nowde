#pragma once

#include <Preferences.h>
#include <USBMIDI.h>
#include <ESPNowMeshClock.h>
#include "nowde_config.h"

extern USBMIDI MIDI;
extern Preferences preferences;
extern ESPNowMeshClock meshClock;

extern bool senderModeEnabled;
extern bool receiverModeEnabled;
extern char subscribedLayer[MAX_LAYER_LENGTH];

extern SenderEntry senderTable[MAX_SENDERS];
extern ReceiverEntry receiverTable[MAX_RECEIVERS];

extern uint8_t broadcastAddress[6];

extern unsigned long lastSenderBeacon;
extern unsigned long lastBridgeReport;

extern MediaSyncState mediaSyncState;

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
