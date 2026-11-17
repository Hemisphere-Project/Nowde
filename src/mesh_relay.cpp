#include "mesh_relay.h"

#include <cstring>

#include <esp_now.h>
#include <esp_wifi.h>

#include "esp_now_handlers.h"   // relaySend(), espnowRelayDropped
#include "nowde_config.h"
#include "nowde_state.h"

namespace {

// This node's own station MAC -- never relay a packet we originated ourselves (we would never
// actually receive our own broadcast, but a second master relaying OUR packet back is not
// something to re-relay either). Cached: esp_wifi_get_mac is cheap but no reason to repeat it.
const uint8_t* myMacAddress() {
  static uint8_t mac[6] = {0};
  static bool have = false;
  if (!have) {
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    have = true;
  }
  return mac;
}

RelayOriginEntry* findOrCreateOrigin(const uint8_t* origin) {
  RelayOriginEntry* freeSlot = nullptr;
  for (int i = 0; i < MAX_RELAY_ORIGINS; i++) {
    if (relayOrigins[i].valid && macEqual(relayOrigins[i].mac, origin)) {
      return &relayOrigins[i];
    }
    if (!relayOrigins[i].valid && freeSlot == nullptr) {
      freeSlot = &relayOrigins[i];
    }
  }
  if (freeSlot == nullptr) {
    // Table full -- more concurrent masters than MAX_RELAY_ORIGINS (== MAX_SENDERS) ever
    // anticipates. Reuse slot 0 rather than drop silently forever: the cost is one fresh
    // dedup window for that origin, invisible next to a table permanently stuck.
    freeSlot = &relayOrigins[0];
  }
  freeSlot->valid = false;
  memcpy(freeSlot->mac, origin, 6);
  return freeSlot;
}

// Overhearing suppression (topology note §(2), "if you hear someone else forward it first,
// drop yours"): cancel any pending forward of ours for this exact (origin, seq).
void suppressPending(const uint8_t* origin, uint16_t seq) {
  for (int i = 0; i < MAX_PENDING_RELAYS; i++) {
    if (pendingRelays[i].active && pendingRelays[i].seq == seq &&
        macEqual(pendingRelays[i].origin, origin)) {
      pendingRelays[i].active = false;
      DEBUG_SERIAL.println("[RELAY] suppressed - already forwarded by another node");
    }
  }
}

}  // namespace

void handleFloodCandidate(const uint8_t* origin, uint16_t seq, uint8_t receivedHop,
                          const uint8_t* inner, int innerLen, bool isRelay) {
  if (macEqual(origin, myMacAddress())) {
    return;  // never relay our own originated traffic
  }

  if (isRelay) {
    suppressPending(origin, seq);
  }

  // Dedup on (origin, seq): mark it seen NOW, before scheduling, so a duplicate arriving while
  // ours is still pending (the case suppressPending() just handled) does not also re-schedule.
  RelayOriginEntry* entry = findOrCreateOrigin(origin);
  bool alreadySeen = entry->valid && entry->lastSeq == seq;
  entry->valid = true;
  entry->lastSeq = seq;
  if (alreadySeen) {
    return;  // already forwarded, scheduled, or just suppressed for this exact packet
  }

  uint8_t newHop = receivedHop + 1;
  if (newHop > MAX_HOPS) {
    return;  // would exceed the hop budget -- flooding stops here by construction
  }

  if (innerLen > static_cast<int>(sizeof(MediaSyncPacket))) {
    innerLen = static_cast<int>(sizeof(MediaSyncPacket));  // defensive clamp, never expected
  }

  for (int i = 0; i < MAX_PENDING_RELAYS; i++) {
    if (pendingRelays[i].active) {
      continue;
    }
    pendingRelays[i].active = true;
    pendingRelays[i].sendTime =
        millis() + random(MESH_RELAY_DELAY_MIN_MS, MESH_RELAY_DELAY_MAX_MS + 1);
    pendingRelays[i].hop = newHop;
    memcpy(pendingRelays[i].origin, origin, 6);
    pendingRelays[i].seq = seq;
    memcpy(pendingRelays[i].payload, inner, innerLen);
    pendingRelays[i].payloadLen = static_cast<uint8_t>(innerLen);
    return;
  }

  static unsigned long lastDropLog = 0;
  if (millis() - lastDropLog > 2000) {
    lastDropLog = millis();
    DEBUG_SERIAL.println("[RELAY] pending table full - dropped a forward");
  }
}

void processPendingRelays() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_PENDING_RELAYS; i++) {
    if (!pendingRelays[i].active || now < pendingRelays[i].sendTime) {
      continue;
    }
    pendingRelays[i].active = false;

    MeshRelayHeader header;
    memcpy(header.origin, pendingRelays[i].origin, 6);
    header.seq = pendingRelays[i].seq;
    header.hop = pendingRelays[i].hop;

    uint8_t frame[sizeof(MeshRelayHeader) + sizeof(MediaSyncPacket)];
    memcpy(frame, &header, sizeof(header));
    memcpy(frame + sizeof(header), pendingRelays[i].payload, pendingRelays[i].payloadLen);

    relaySend(broadcastAddress, frame, sizeof(header) + pendingRelays[i].payloadLen);
  }
}
