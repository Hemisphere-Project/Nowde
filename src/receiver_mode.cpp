#include "receiver_mode.h"

#include <cstring>

#include <esp_now.h>

#include "midi.h"
#include "nowde_config.h"
#include "nowde_state.h"

void cleanupSenderTable() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_SENDERS; i++) {
    if (senderTable[i].active && (now - senderTable[i].lastSeen > SENDER_TIMEOUT_MS)) {
      DEBUG_SERIAL.println("\n[TIMEOUT] Sender removed");
      DEBUG_SERIAL.print("  MAC: ");
      for (int j = 0; j < 6; j++) {
        DEBUG_SERIAL.printf("%02X", senderTable[i].mac[j]);
        if (j < 5) {
          DEBUG_SERIAL.print(":");
        }
      }
      DEBUG_SERIAL.println();
      DEBUG_SERIAL.printf("  Remaining: %d\r\n\r\n", countActiveSenders() - 1);

      esp_now_del_peer(senderTable[i].mac);
      senderTable[i].active = false;
    }
  }
}

void sendReceiverInfo() {
  if (!receiverModeEnabled || strlen(subscribedLayer) == 0) {
    return;
  }

  ReceiverInfo info;
  strncpy(info.layer, subscribedLayer, MAX_LAYER_LENGTH);
  info.layer[MAX_LAYER_LENGTH - 1] = '\0';
  strncpy(info.version, NOWDE_VERSION, MAX_VERSION_LENGTH);
  info.version[MAX_VERSION_LENGTH - 1] = '\0';
  
  // Populate current playing media index (0 = stopped)
  info.mediaIndex = mediaSyncState.currentIndex;

  for (int i = 0; i < MAX_SENDERS; i++) {
    if (senderTable[i].active) {
      esp_now_send(senderTable[i].mac, reinterpret_cast<uint8_t*>(&info), sizeof(info));
    }
  }

  // Receiver info logging disabled for cleaner output
  // Info packets are sent every ~1s but not logged
}

void processMediaSyncPacket(const uint8_t* data, int len) {
  if (len < static_cast<int>(sizeof(MediaSyncPacket))) {
    return;
  }

  const MediaSyncPacket* syncPacket = reinterpret_cast<const MediaSyncPacket*>(data);

  if (strncmp(syncPacket->layer, subscribedLayer, MAX_LAYER_LENGTH) != 0) {
    return;
  }

  uint32_t currentMeshTime = meshClock.meshMillis();
  int32_t timeDelta = static_cast<int32_t>(currentMeshTime - syncPacket->meshTimestamp);

  if (abs(timeDelta) > static_cast<int32_t>(CLOCK_DESYNC_THRESHOLD_MS)) {
    // Log packet discard with details
    static unsigned long lastDiscardLog = 0;
    if (millis() - lastDiscardLog > 1000) {  // Log at most once per second
      DEBUG_SERIAL.printf("[MEDIA SYNC] PACKET DISCARDED - Clock desync! Delta=%ld ms (threshold=%lu ms)\r\n",
                         timeDelta, CLOCK_DESYNC_THRESHOLD_MS);
      lastDiscardLog = millis();
    }
    return;
  }

  unsigned long now = millis();
  uint32_t compensatedPositionMs = syncPacket->positionMs;
  if (syncPacket->state == 1 && timeDelta > 0) {
    compensatedPositionMs += timeDelta;
  }

  // Handle state change to stopped
  bool stateChangedToStopped = (mediaSyncState.currentState == 1 && syncPacket->state == 0);
  bool stateChangedToPlaying = (mediaSyncState.currentState == 0 && syncPacket->state == 1);
  
  // Update sync state
  mediaSyncState.currentIndex = syncPacket->mediaIndex;
  mediaSyncState.currentPositionMs = compensatedPositionMs;
  mediaSyncState.currentState = syncPacket->state;
  mediaSyncState.lastSyncTime = now;
  mediaSyncState.linkLost = false;
  
  // Reset local clock reference when receiving sync
  if (syncPacket->state == 1) {
    mediaSyncState.localClockStartTime = now;
  }

  // Handle media index changes (but not when stopping - that's handled by state transition)
  if (mediaSyncState.lastSentIndex != syncPacket->mediaIndex && syncPacket->mediaIndex != 0) {
    midiSendCC100(syncPacket->mediaIndex);
    mediaSyncState.lastSentIndex = syncPacket->mediaIndex;
    mediaSyncState.lastCC100SendTime = now;
  }

  // Handle state transitions
  if (stateChangedToPlaying) {
    DEBUG_SERIAL.println("[MEDIA SYNC] Media started playing");
  } else if (stateChangedToStopped) {
    // Just stopped: send CC#100 = 0 to signal stop (only place where CC#100=0 is sent)
    DEBUG_SERIAL.println("[MEDIA SYNC] Media stopped - sending CC#100=0");
    midiSendCC100(0);
    mediaSyncState.lastSentIndex = 0;
    mediaSyncState.lastCC100SendTime = now;
  }
  // Periodic CC#100 resend while playing (every CC100_REPEAT_INTERVAL_MS)
  // This allows late-started devices to catch up even if they missed initial CC
  // Set CC100_REPEAT_INTERVAL_MS to 0 in nowde_config.h to disable
  else if (CC100_REPEAT_INTERVAL_MS > 0 &&
           mediaSyncState.currentState == 1 && 
           mediaSyncState.currentIndex > 0 &&
           (now - mediaSyncState.lastCC100SendTime) >= CC100_REPEAT_INTERVAL_MS) {
    midiSendCC100(mediaSyncState.currentIndex);
    mediaSyncState.lastCC100SendTime = now;
  }

  static int syncCount = 0;
  syncCount++;
  if (syncCount % 50 == 0) {  // Every 5 seconds at 10Hz
    DEBUG_SERIAL.printf("[MEDIA SYNC RX] #%d Index=%d, Pos=%lu ms (compensated +%ld ms), State=%s\r\n",
                       syncCount, syncPacket->mediaIndex, compensatedPositionMs, timeDelta,
                       syncPacket->state == 1 ? "playing" : "stopped");
  }
}
