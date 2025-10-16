#pragma once

#include <Arduino.h>

void cleanupSenderTable();
void sendReceiverInfo();
void processMediaSyncPacket(const uint8_t* data, int len);
