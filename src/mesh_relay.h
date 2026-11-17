#pragma once

#include <Arduino.h>

// v2.2 controlled flooding (#t-024, topology note §(2)(3)). Called from onDataRecv
// (esp_now_handlers.cpp) for both paths that can carry a MediaSyncPacket:
//   - direct 0x03 MEDIA_SYNC: origin = info->src_addr, receivedHop = 0, isRelay = false
//   - relayed 0x05 MESH_RELAY: origin = the envelope's, receivedHop = the envelope's hop,
//     isRelay = true
// `inner` / `innerLen` is the wrapped packet, verbatim, in both cases: a MediaSyncPacket (0x03)
// or, since v2.3, a MIDI_EVENT frame (0x04, #t-037). Any other inner type is not relayed.
//
// Schedules a randomized MESH_RELAY_DELAY_MIN_MS..MESH_RELAY_DELAY_MAX_MS re-broadcast unless:
// this node originated the packet itself, this exact (origin, type, seq) has already been handled
// (forwarded, scheduled, or suppressed), or forwarding would exceed MAX_HOPS. If `isRelay` and
// another node's copy of the same (origin, type, seq) is heard while our own forward is still
// pending, that pending forward is cancelled (overhearing suppression).
void handleFloodCandidate(const uint8_t* origin, uint16_t seq, uint8_t receivedHop,
                          const uint8_t* inner, int innerLen, bool isRelay);

// docs/PROTOCOL.md "The v2.3 event frame", validation steps 1-2: a 0x04 frame whose header and
// declared records are all present. Longer is complete (step 3: the tail is ignored). Strides by
// the frame's own hdrLen/recLen, never by sizeof -- the slave leg (#t-037 slice 3) reuses it.
bool midiEventFrameComplete(const uint8_t* frame, int len);

// Polled every ESPNOW_Task tick (main.cpp), unconditionally -- relaying is a mesh-layer
// service, independent of this node's own sender/receiver role. Fires any pending relay
// whose randomized delay has elapsed and was not suppressed.
void processPendingRelays();
