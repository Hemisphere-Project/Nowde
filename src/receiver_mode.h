#pragma once

#include <Arduino.h>

void cleanupSenderTable();
void sendReceiverInfo();
// `origin` = the MAC of the node that MINTED this packet, which on the direct path is the
// recv callback's src_addr and on #t-024's relayed path will be the envelope's origin field.
// It is a parameter, never read from the callback here, for exactly that reason.
void processMediaSyncPacket(const uint8_t* origin, const uint8_t* data, int len);
