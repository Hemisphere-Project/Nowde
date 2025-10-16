#pragma once

#include <Arduino.h>
#include <esp_now.h>

void cleanupReceiverTable();
void sendSenderBeacon();
void reportReceiversToBridge();
void handleSenderBeacon(const esp_now_recv_info_t* info);
void handleReceiverInfo(const esp_now_recv_info_t* info, const uint8_t* data, int len);
