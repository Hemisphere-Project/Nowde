#include "esp_now_handlers.h"

#include <cstring>

#include "mesh_relay.h"
#include "nowde_config.h"
#include "nowde_state.h"
#include "receiver_mode.h"
#include "sender_mode.h"
#include "sysex.h"

// Unicast ESP-NOW frames are ACKed at the MAC layer, so a FAIL here means the peer never
// answered. This callback used to discard that, and the relay discarded esp_now_send()'s
// result too -- between them a slave could sit in the master's table looking perfectly
// healthy (layer, version, seen < 1 s) while receiving nothing at all. Measured on the bench
// 2026-09-04: the LAST peer of every fan-out burst, silently, for as long as you left it.
volatile uint32_t espnowTxOk = 0;
volatile uint32_t espnowTxFail = 0;

void onDataSent(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    espnowTxOk++;
    return;
  }
  espnowTxFail++;

  static unsigned long lastLog = 0;
  if (millis() - lastLog > 2000) {
    lastLog = millis();
    if (info != nullptr) {
      const uint8_t* m = info->des_addr;
      DEBUG_SERIAL.printf("[ESP-NOW TX] delivery FAILED to %02X:%02X:%02X:%02X:%02X:%02X (%lu ok / %lu fail)\r\n",
                          m[0], m[1], m[2], m[3], m[4], m[5],
                          (unsigned long)espnowTxOk, (unsigned long)espnowTxFail);
    } else {
      DEBUG_SERIAL.printf("[ESP-NOW TX] delivery FAILED (%lu ok / %lu fail)\r\n",
                          (unsigned long)espnowTxOk, (unsigned long)espnowTxFail);
    }
  }
}

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < 1) {
    return;
  }

  if (meshClock.handleReceive(info->src_addr, data, len)) {
    return;
  }

  uint8_t msgType = data[0];

  if (msgType == SYSEX_START) {
    DEBUG_SERIAL.println("\n[ESP-NOW RX] SysEx message received");
    DEBUG_SERIAL.print("  From: ");
    for (int j = 0; j < 6; j++) {
      DEBUG_SERIAL.printf("%02X", info->src_addr[j]);
      if (j < 5) {
        DEBUG_SERIAL.print(":");
      }
    }
    DEBUG_SERIAL.println();
    DEBUG_SERIAL.print("  Data: ");
    for (int j = 0; j < len; j++) {
      DEBUG_SERIAL.printf("%02X ", data[j]);
    }
    DEBUG_SERIAL.printf("(%d bytes)\r\n", len);

    handleSysExMessage(data, static_cast<uint8_t>(len));
    return;
  }

  switch (msgType) {
    case ESPNOW_MSG_SENDER_BEACON:
      handleSenderBeacon(info);
      break;

    case ESPNOW_MSG_RECEIVER_INFO:
      if (senderModeEnabled) {
        handleReceiverInfo(info, data, len);
      }
      break;

    case ESPNOW_MSG_MEDIA_SYNC:
      // v2.2: `!senderModeEnabled` is new, and it is not a tidy-up. Receiver mode is ALWAYS on,
      // master included; under unicast that was harmless because nobody addressed a frame to a
      // master. Broadcast delivers to everyone in earshot, so without this a second master in
      // the room would be followed by the first -- origin-locking onto it and emitting its
      // position as MTC to its own host. A node that drives the mesh does not follow it.
      if (receiverModeEnabled && !senderModeEnabled) {
        // Direct path: the sender IS the origin. The ESPNOW_MSG_MESH_RELAY case below passes
        // the envelope's origin instead -- src_addr there is the relayer, not the master.
        processMediaSyncPacket(info->src_addr, data, len);
      }
      // v2.2 controlled flooding (#t-024): ANY node, regardless of role, may extend this
      // packet's reach for a moving/out-of-range slave (topology note §(2)(3)) -- relaying is
      // a mesh-layer service, orthogonal to who consumes the packet above. Dedup needs the
      // origin's own `seq`, only present once the frame is the full v2.2 size; a shorter frame
      // from an older master is still consumed above, it simply gets no relay coverage.
      if (len >= static_cast<int>(sizeof(MediaSyncPacket))) {
        const MediaSyncPacket* directPacket = reinterpret_cast<const MediaSyncPacket*>(data);
        handleFloodCandidate(info->src_addr, directPacket->seq, 0, data, len, false);
      }
      break;

    case ESPNOW_MSG_MESH_RELAY: {
      // v2.2 controlled flooding (#t-024). New type, never a trailer on 0x03 -- see the
      // #t-020 note "Why a trailer on 0x03 is the wrong spelling": a v1.2 slave ignores an
      // unknown type outright, where a trailer it would have ACTED on, origin-lock and all.
      if (len < static_cast<int>(sizeof(MeshRelayHeader) + MEDIA_SYNC_MIN_LEN)) {
        break;  // too short to be a real relay frame -- malformed or truncated, drop silently
      }
      const MeshRelayHeader* header = reinterpret_cast<const MeshRelayHeader*>(data);
      const uint8_t* inner = data + sizeof(MeshRelayHeader);
      int innerLen = len - static_cast<int>(sizeof(MeshRelayHeader));

      if (receiverModeEnabled && !senderModeEnabled) {
        // Relayed path: the envelope's origin, never info->src_addr (that is the relayer's
        // MAC, not the master's) -- see OriginLock's comment, nowde_config.h.
        processMediaSyncPacket(header->origin, inner, innerLen);
      }
      handleFloodCandidate(header->origin, header->seq, header->hop, inner, innerLen, true);
      break;
    }

    default:
      break;
  }
}
