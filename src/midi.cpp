#include "midi.h"

#include "midi_event.h"
#include "nowde_config.h"
#include "nowde_state.h"
#include "sysex.h"
#include "usb_out.h"
#include "host_clock.h"

namespace {
constexpr size_t SYSEX_BUFFER_SIZE = 512;  // Large enough for RUNNING_STATE payloads (~350B)
uint8_t sysexBuffer[SYSEX_BUFFER_SIZE];
size_t sysexIndex = 0;
bool inSysex = false;
bool sysexOverflow = false;

struct Smpte {
  uint8_t frames, seconds, minutes, hours;
};

Smpte toSmpte(uint32_t positionMs) {
  uint32_t totalFrames = (positionMs * MTC_FRAMERATE) / 1000;
  Smpte t;
  t.frames = totalFrames % MTC_FRAMERATE;
  t.seconds = (totalFrames / MTC_FRAMERATE) % 60;
  t.minutes = (totalFrames / (MTC_FRAMERATE * 60)) % 60;
  t.hours = (totalFrames / (MTC_FRAMERATE * 3600)) % 24;
  return t;
}
}

void midiInit() {
  MIDI.begin();
  midiEventInit();   // v2.3 0x04 (#t-038): the slave's hand-off queue, before ESP-NOW is up
}

void midiSendCC(uint8_t cc, uint8_t value) {
  usbOutMessage(0xB0, cc, value);   // control change, channel 1
  DEBUG_SERIAL.printf("[MIDI TX] CC#%d = %d (channel 1)\r\n", cc, value);
}

void midiSendCC100(uint8_t value) {
  midiSendCC(100, value);
}

void midiSendCC7(uint8_t value) {
  midiSendCC(7, value);
}

void midiSendTimeCode(uint32_t positionMs) {
  Smpte t = toSmpte(positionMs);

  auto sendQuarterFrame = [&](uint8_t piece, uint8_t nibble) {
    usbOutMessage(0xF1, static_cast<uint8_t>((piece << 4) | (nibble & 0x0F)));
  };

  usbOutLock();   // the 8 pieces stay contiguous in the queue
  sendQuarterFrame(0, t.frames & 0x0F);
  sendQuarterFrame(1, (t.frames >> 4) & 0x01);
  sendQuarterFrame(2, t.seconds & 0x0F);
  sendQuarterFrame(3, (t.seconds >> 4) & 0x03);
  sendQuarterFrame(4, t.minutes & 0x0F);
  sendQuarterFrame(5, (t.minutes >> 4) & 0x03);
  sendQuarterFrame(6, t.hours & 0x0F);

  uint8_t framerateCode = 3;  // 30 fps non-drop
  sendQuarterFrame(7, ((t.hours >> 4) & 0x01) | (framerateCode << 1));
  usbOutUnlock();

  static unsigned long lastMTCLog = 0;
  if (millis() - lastMTCLog > 5000) {
    DEBUG_SERIAL.printf("[MIDI TX] MTC: %02d:%02d:%02d:%02d (30fps)\r\n", t.hours, t.minutes, t.seconds, t.frames);
    lastMTCLog = millis();
  }
}

void midiSendFullFrame(uint32_t positionMs) {
  // Universal real-time SysEx: F0 7F 7F 01 01 hh mm ss ff F7 (hh carries the rate code in bits 5-6)
  Smpte t = toSmpte(positionMs);
  const uint8_t msg[10] = {0xF0, 0x7F, 0x7F, 0x01, 0x01,
                           static_cast<uint8_t>((3 << 5) | (t.hours & 0x1F)),
                           t.minutes, t.seconds, t.frames, 0xF7};
  usbOutSysEx(msg, sizeof(msg));
  DEBUG_SERIAL.printf("[MIDI TX] MTC full-frame %02d:%02d:%02d:%02d\r\n", t.hours, t.minutes, t.seconds, t.frames);
}

static void sendRealtime(uint8_t status) {
  usbOutMessage(status);   // single-byte system real-time
}

void midiSendStart() {
  sendRealtime(0xFA);
  DEBUG_SERIAL.println("[MIDI TX] Start");
}

void midiSendStop() {
  sendRealtime(0xFC);
  DEBUG_SERIAL.println("[MIDI TX] Stop");
}

bool midiReadPacket(midiEventPacket_t* packet) {
  return MIDI.readPacket(packet);
}

void midiProcess() {
  midiEventPacket_t packet;

  midiEventFire();   // v2.3 0x04 (#t-038): scheduled records whose mesh time has come
  usbOutPump();      // MIDI task: everything queued for the host goes out here
  otaRestartPoll();  // 2.4: a validated OTA_END restarts here, after its verdict went out above

  while (midiReadPacket(&packet)) {
    if (!hostLinked()) {
      hostResumed = true;       // first packet after a silence: the host may be probing our role
    }
    lastHostRxTime = millis();  // anything from the host counts as a heartbeat
    uint8_t cin = packet.header & 0x0F;

    // v2.1 — plain MIDI from the host (a master's DAW / QLab / keyboard)
    if (cin == 0x2 && packet.byte1 == 0xF1) {                 // MTC quarter-frame
      hostClockOnQuarterFrame(packet.byte2);
      continue;
    }
    if ((cin == 0xF || cin == 0x5) && packet.byte1 >= 0xF8) { // single-byte real-time
      hostClockOnRealtime(packet.byte1);
      continue;
    }
    if (cin >= 0x8 && cin <= 0xE) {
      // A host CC feeds both the v2.1 clock and the v2.3 frame -- except CC#100, the media index:
      // the slaves already get it inside MEDIA_SYNC and play it to their hosts from there (with
      // its 1 s repeat and the stop edge), so on the frame too a slave's host would see two CC#100
      // streams on their own timing, and the frame's 1 Hz snapshot would replay an index the
      // MEDIA_SYNC path had already stopped. CC#100 stays off the radio.
      if (cin == 0xB) {
        hostClockOnControlChange(packet.byte1 & 0x0F, packet.byte2, packet.byte3);
        if (packet.byte2 == 100) {
          continue;
        }
      }
      midiEventCapture(packet.byte1, packet.byte2, packet.byte3);   // v2.3 0x04 (#t-037): ONE call
      continue;
    }

    if (cin >= 0x4 && cin <= 0x7) {
      int dataBytes = 0;
      if (cin == 0x4) {
        dataBytes = 3;
      } else if (cin == 0x5) {
        dataBytes = 1;
      } else if (cin == 0x6) {
        dataBytes = 2;
      } else if (cin == 0x7) {
        dataBytes = 3;
      }

      if (dataBytes > 0) {
        if (packet.byte1 == SYSEX_START) {
          inSysex = true;
          sysexIndex = 0;
        }

        if (inSysex) {
          const uint8_t bytes[3] = {packet.byte1, packet.byte2, packet.byte3};
          for (int i = 0; i < dataBytes; i++) {
            if (sysexIndex < SYSEX_BUFFER_SIZE) {
              sysexBuffer[sysexIndex++] = bytes[i];
            } else {
              sysexOverflow = true;  // Keep reading so we can resync at F7
            }
            if (bytes[i] == SYSEX_END) {
              // Only log SysEx for non-repetitive messages (to reduce clutter)
              // Skip: MEDIA_SYNC (0x10) sent at 10Hz, QUERY_RUNNING_STATE (0x03) sent at 1Hz
              // Skip: OTA_DATA (0x06) during firmware updates (thousands of messages)
              bool isRepetitive = (sysexIndex >= 3 && 
                                   (sysexBuffer[2] == SYSEX_CMD_MEDIA_SYNC || 
                                    sysexBuffer[2] == SYSEX_CMD_QUERY_RUNNING_STATE ||
                                    sysexBuffer[2] == 0x06));  // OTA_DATA
              if (!isRepetitive && !sysexOverflow) {
                DEBUG_SERIAL.print("[SYSEX RX] ");
                for (int j = 0; j < sysexIndex; j++) {
                  DEBUG_SERIAL.printf("%02X ", sysexBuffer[j]);
                }
                DEBUG_SERIAL.printf("(%d bytes)\r\n", sysexIndex);
              }
              if (sysexOverflow) {
                DEBUG_SERIAL.printf("[SYSEX RX] WARNING: SysEx overflow (len>%d), discarding message\r\n", SYSEX_BUFFER_SIZE);
              } else {
                handleSysExMessage(sysexBuffer, sysexIndex);
              }
              inSysex = false;
              sysexIndex = 0;
              sysexOverflow = false;
              break;
            }
          }
        }
      }
    }
  }

  midiEventTick();   // v2.3 0x04 (#t-037): close a 5 ms batch, or send the owed carry-over
}
