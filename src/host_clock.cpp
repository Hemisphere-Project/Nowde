#include "host_clock.h"

#include <cstring>

#include "nowde_config.h"
#include "nowde_state.h"
#include "sender_mode.h"

namespace {

// MTC quarter-frame accumulator
uint8_t qf[8] = {0};
uint8_t qfMask = 0;
unsigned long lastQfTime = 0;

// Decoded host clock
uint8_t hostIndex = 0;
bool hostRunning = false;
uint32_t hostPositionMs = 0;       // position at hostPositionAt
unsigned long hostPositionAt = 0;  // millis() when hostPositionMs was true
bool hostDirty = false;            // something changed since the last packet

unsigned long lastSysExSync = 0;   // SysEx MEDIA_SYNC seen (priority source)
unsigned long lastPacketSent = 0;
unsigned long lastMidiIn = 0;

float fpsFromRateCode(uint8_t rate) {
  switch (rate & 0x03) {
    case 0: return 24.0f;
    case 1: return 25.0f;
    case 2: return 29.97f;
    default: return 30.0f;
  }
}

void setPosition(uint32_t ms) {
  hostPositionMs = ms;
  hostPositionAt = millis();
  hostDirty = true;
}

void setRunning(bool running) {
  if (hostRunning != running) {
    hostRunning = running;
    hostDirty = true;
    DEBUG_SERIAL.printf("[HOST MIDI] %s\r\n", running ? "running" : "stopped");
  }
}

bool sysexHasPriority() {
  return lastSysExSync != 0 && (millis() - lastSysExSync) < HOST_MIDI_SYSEX_PRIORITY_MS;
}

}  // namespace

void hostClockNoteSysExSync() {
  lastSysExSync = millis();
}

bool hostClockActive() {
  return !sysexHasPriority() && lastMidiIn != 0 && (millis() - lastMidiIn) < HOST_LINK_TIMEOUT_MS;
}

uint8_t hostClockIndex() { return hostIndex; }
bool hostClockRunning() { return hostRunning; }

void hostClockOnQuarterFrame(uint8_t data) {
  lastMidiIn = millis();
  uint8_t piece = (data >> 4) & 0x07;
  qf[piece] = data & 0x0F;
  qfMask |= (1 << piece);
  lastQfTime = millis();
  if (piece == 7 && qfMask == 0xFF) {
    uint8_t frames = qf[0] | ((qf[1] & 0x01) << 4);
    uint8_t seconds = qf[2] | ((qf[3] & 0x03) << 4);
    uint8_t minutes = qf[4] | ((qf[5] & 0x03) << 4);
    uint8_t hours = qf[6] | ((qf[7] & 0x01) << 4);
    float fps = fpsFromRateCode((qf[7] >> 1) & 0x03);
    // The 8-piece sequence spans 2 frames: the timecode is the time at piece 0.
    uint32_t totalFrames = ((hours * 3600UL + minutes * 60UL + seconds) * (uint32_t)(fps + 0.5f)) + frames + MTC_QF_LAG_FRAMES;
    setPosition((uint32_t)(totalFrames * 1000.0f / fps));
    setRunning(true);
    qfMask = 0;
  }
}

void hostClockOnFullFrame(uint8_t hh, uint8_t mm, uint8_t ss, uint8_t ff) {
  lastMidiIn = millis();
  float fps = fpsFromRateCode((hh >> 5) & 0x03);
  uint32_t totalFrames = (((hh & 0x1F) * 3600UL + mm * 60UL + ss) * (uint32_t)(fps + 0.5f)) + ff;
  setPosition((uint32_t)(totalFrames * 1000.0f / fps));
  qfMask = 0;
  DEBUG_SERIAL.printf("[HOST MIDI] full-frame %02u:%02u:%02u:%02u\r\n", hh & 0x1F, mm, ss, ff);
}

void hostClockOnControlChange(uint8_t channel, uint8_t cc, uint8_t value) {
  (void)channel;
  lastMidiIn = millis();
  if (cc != 100) {
    return;  // v2.1: other CCs are not relayed yet (MIDI_EVENT is reserved)
  }
  if (value != hostIndex) {
    hostIndex = value;
    hostDirty = true;
    DEBUG_SERIAL.printf("[HOST MIDI] CC#100 = %u\r\n", value);
  }
  if (value == 0) {
    setRunning(false);
  }
}

void hostClockOnRealtime(uint8_t status) {
  lastMidiIn = millis();
  switch (status) {
    case 0xFA:  // Start: from the top
      setPosition(0);
      setRunning(true);
      break;
    case 0xFB:  // Continue
      setRunning(true);
      break;
    case 0xFC:  // Stop
      setRunning(false);
      break;
    default:
      break;
  }
}

void hostClockTick() {
  if (!senderModeEnabled || !hostClockActive()) {
    return;
  }
  unsigned long now = millis();

  // MTC stopped flowing: the host paused or stopped without a transport message
  if (hostRunning && lastQfTime != 0 && (now - lastQfTime) > HOST_MIDI_MTC_TIMEOUT_MS && lastQfTime > hostPositionAt - 1) {
    // only when MTC was the running source (a Start with no MTC keeps running)
    if (qfMask != 0 || (now - lastQfTime) < 2 * HOST_MIDI_MTC_TIMEOUT_MS) {
      setRunning(false);
    }
  }

  unsigned long interval = hostRunning ? HOST_MIDI_SYNC_INTERVAL_MS : HOST_MIDI_IDLE_INTERVAL_MS;
  if (!hostDirty && (now - lastPacketSent) < interval) {
    return;
  }
  hostDirty = false;
  lastPacketSent = now;

  MediaSyncPacket packet;
  strncpy(packet.layer, subscribedLayer, MAX_LAYER_LENGTH);
  packet.layer[MAX_LAYER_LENGTH - 1] = '\0';
  packet.mediaIndex = hostIndex;
  packet.positionMs = hostPositionMs + (hostRunning ? (now - hostPositionAt) : 0);
  packet.state = (hostRunning && hostIndex != 0) ? 1 : 0;
  packet.meshTimestamp = meshClock.meshMillis();
  sendMediaSyncToReceivers(packet);
}
