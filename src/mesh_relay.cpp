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

// The relayed types, as an index into RelayOriginEntry::kinds; -1 = not a type this build relays.
int meshRelayKind(uint8_t type) {
  switch (type) {
    case ESPNOW_MSG_MEDIA_SYNC: return 0;
    case ESPNOW_MSG_MIDI_EVENT: return 1;
    default: return -1;
  }
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
  *freeSlot = RelayOriginEntry();
  memcpy(freeSlot->mac, origin, 6);
  freeSlot->valid = true;
  return freeSlot;
}

// True if `seq` was already handled in this window; records it either way. 32 frames back:
// 160 ms of 0x04 at one frame per 5 ms batch, past the 90 ms worst relay path (MIDI_EVENT_LEAD_MS).
// A seq further back than that is an origin that restarted its counter: start over from it.
bool seenBefore(RelaySeqWindow& w, uint16_t seq) {
  if (w.seen == 0) {
    w.top = seq;
    w.seen = 1u;
    return false;
  }
  int ahead = static_cast<int16_t>(static_cast<uint16_t>(seq - w.top));
  if (ahead > 0) {
    w.seen = (ahead >= 32) ? 1u : ((w.seen << ahead) | 1u);
    w.top = seq;
    return false;
  }
  int back = -ahead;
  if (back >= 32) {
    w.top = seq;
    w.seen = 1u;
    return false;
  }
  uint32_t bit = 1u << back;
  bool was = (w.seen & bit) != 0;
  w.seen |= bit;
  return was;
}

// Overhearing suppression (topology note §(2), "if you hear someone else forward it first,
// drop yours"): cancel any pending forward of ours for this exact (origin, type, seq).
void suppressPending(const uint8_t* origin, uint8_t type, uint16_t seq) {
  for (int i = 0; i < MAX_PENDING_RELAYS; i++) {
    if (pendingRelays[i].active && pendingRelays[i].seq == seq && pendingRelays[i].type == type &&
        macEqual(pendingRelays[i].origin, origin)) {
      pendingRelays[i].active = false;
      DEBUG_SERIAL.println("[RELAY] suppressed - already forwarded by another node");
    }
  }
}

}  // namespace

static_assert(sizeof(MeshRelayHeader) + MESH_RELAY_PAYLOAD_MAX <= ESP_NOW_MAX_DATA_LEN,
              "a full 0x04 frame must fit the 0x05 envelope");

void handleFloodCandidate(const uint8_t* origin, uint16_t seq, uint8_t receivedHop,
                          const uint8_t* inner, int innerLen, bool isRelay) {
  if (macEqual(origin, myMacAddress())) {
    return;  // never relay our own originated traffic
  }

  if (innerLen < 1) {
    return;
  }
  uint8_t type = inner[0];
  int kind = meshRelayKind(type);
  if (kind < 0) {
    return;  // not a type this build relays: it cannot tell a real frame from noise
  }

  if (isRelay) {
    suppressPending(origin, type, seq);
  }

  // Dedup on (origin, type, seq): mark it seen NOW, before scheduling, so a duplicate arriving
  // while ours is still pending (the case suppressPending() just handled) does not re-schedule.
  RelayOriginEntry* entry = findOrCreateOrigin(origin);
  if (seenBefore(entry->kinds[kind], seq)) {
    return;  // already forwarded, scheduled, or just suppressed for this exact packet
  }

  uint8_t newHop = receivedHop + 1;
  if (newHop > MAX_HOPS) {
    return;  // would exceed the hop budget -- flooding stops here by construction
  }

  if (innerLen > MESH_RELAY_PAYLOAD_MAX) {
    // Not clamped: a truncated 0x04 frame fails validation step 2 at every node downstream,
    // so a cut copy is airtime spent for nothing. Larger than any frame this build knows.
    static unsigned long lastBigLog = 0;
    if (millis() - lastBigLog > 2000) {
      lastBigLog = millis();
      DEBUG_SERIAL.printf("[RELAY] inner packet too large to forward (%d B)\r\n", innerLen);
    }
    return;
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
    pendingRelays[i].type = type;
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

bool midiEventFrameComplete(const uint8_t* frame, int len) {
  if (len < MIDI_EVENT_HEADER_LEN || frame[0] != ESPNOW_MSG_MIDI_EVENT) {
    return false;
  }
  int hdrLen = frame[offsetof(MidiEventHeader, hdrLen)];
  int recLen = frame[offsetof(MidiEventHeader, recLen)];
  if (hdrLen < MIDI_EVENT_HEADER_LEN || recLen < MIDI_EVENT_RECORD_LEN) {
    return false;
  }
  int records = frame[offsetof(MidiEventHeader, newCount)] +
                frame[offsetof(MidiEventHeader, carryCount)];
  return len >= hdrLen + records * recLen;
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

    uint8_t frame[sizeof(MeshRelayHeader) + MESH_RELAY_PAYLOAD_MAX];
    memcpy(frame, &header, sizeof(header));
    memcpy(frame + sizeof(header), pendingRelays[i].payload, pendingRelays[i].payloadLen);

    relaySend(broadcastAddress, frame, sizeof(header) + pendingRelays[i].payloadLen);
  }
}
