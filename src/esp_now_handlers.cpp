#include "esp_now_handlers.h"

#include <cstring>

#include "nowde_config.h"
#include "nowde_state.h"
#include "receiver_mode.h"
#include "sender_mode.h"
#include "sysex.h"

void onDataSent(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  (void)info;
  (void)status;
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
      if (receiverModeEnabled) {
        processMediaSyncPacket(data, len);
      }
      break;

    default:
      break;
  }
}
