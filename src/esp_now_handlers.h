#pragma once

#include <esp_now.h>

void onDataSent(const esp_now_send_info_t* info, esp_now_send_status_t status);
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
