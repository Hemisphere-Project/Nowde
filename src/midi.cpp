#include "midi.h"

#include "nowde_config.h"
#include "nowde_state.h"
#include "sysex.h"

namespace {
uint8_t sysexBuffer[128];
uint8_t sysexIndex = 0;
bool inSysex = false;
}

void midiInit() {
  MIDI.begin();
}

void midiSendCC100(uint8_t value) {
  MIDI.controlChange(100, value, 1);
  DEBUG_SERIAL.printf("[MIDI TX] CC#100 = %d (channel 1)\r\n", value);
}

void midiSendTimeCode(uint32_t positionMs) {
  uint32_t totalFrames = (positionMs * MTC_FRAMERATE) / 1000;
  uint8_t frames = totalFrames % MTC_FRAMERATE;
  uint8_t seconds = (totalFrames / MTC_FRAMERATE) % 60;
  uint8_t minutes = (totalFrames / (MTC_FRAMERATE * 60)) % 60;
  uint8_t hours = (totalFrames / (MTC_FRAMERATE * 3600)) % 24;

  auto sendQuarterFrame = [&](uint8_t piece, uint8_t nibble) {
    midiEventPacket_t packet;
    packet.header = 0x02;
    packet.byte1 = 0xF1;
    packet.byte2 = static_cast<uint8_t>((piece << 4) | (nibble & 0x0F));
    packet.byte3 = 0;
    midiWritePacket(packet);
  };

  sendQuarterFrame(0, frames & 0x0F);
  sendQuarterFrame(1, (frames >> 4) & 0x01);
  sendQuarterFrame(2, seconds & 0x0F);
  sendQuarterFrame(3, (seconds >> 4) & 0x03);
  sendQuarterFrame(4, minutes & 0x0F);
  sendQuarterFrame(5, (minutes >> 4) & 0x03);
  sendQuarterFrame(6, hours & 0x0F);

  uint8_t framerateCode = 3;
  sendQuarterFrame(7, ((hours >> 4) & 0x01) | (framerateCode << 1));

  static unsigned long lastMTCLog = 0;
  if (millis() - lastMTCLog > 5000) {
    DEBUG_SERIAL.printf("[MIDI TX] MTC: %02d:%02d:%02d:%02d (30fps)\r\n", hours, minutes, seconds, frames);
    lastMTCLog = millis();
  }
}

void midiWritePacket(midiEventPacket_t& packet) {
  MIDI.writePacket(&packet);
}

bool midiReadPacket(midiEventPacket_t* packet) {
  return MIDI.readPacket(packet);
}

void midiProcess() {
  midiEventPacket_t packet;

  while (midiReadPacket(&packet)) {
    uint8_t cin = packet.header & 0x0F;

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
            if (sysexIndex < sizeof(sysexBuffer)) {
              sysexBuffer[sysexIndex++] = bytes[i];
            }
            if (bytes[i] == SYSEX_END) {
              // Only log SysEx for non-repetitive messages (to reduce clutter)
              // Skip: MEDIA_SYNC (0x10) sent at 10Hz, QUERY_RUNNING_STATE (0x03) sent at 1Hz
              bool isRepetitive = (sysexIndex >= 3 && 
                                   (sysexBuffer[2] == SYSEX_CMD_MEDIA_SYNC || 
                                    sysexBuffer[2] == SYSEX_CMD_QUERY_RUNNING_STATE));
              if (!isRepetitive) {
                DEBUG_SERIAL.print("[SYSEX RX] ");
                for (int j = 0; j < sysexIndex; j++) {
                  DEBUG_SERIAL.printf("%02X ", sysexBuffer[j]);
                }
                DEBUG_SERIAL.printf("(%d bytes)\r\n", sysexIndex);
              }
              handleSysExMessage(sysexBuffer, sysexIndex);
              inSysex = false;
              sysexIndex = 0;
              break;
            }
          }
        }
      }
    }
  }
}
