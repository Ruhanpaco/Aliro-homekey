#pragma once
#include "esp_err.h"
#define ESP_MAC_WIFI_STA 0
static inline esp_err_t esp_read_mac(uint8_t *m, int t){ (void)t; for(int i=0;i<6;i++) m[i]=0xAA; return ESP_OK; }
