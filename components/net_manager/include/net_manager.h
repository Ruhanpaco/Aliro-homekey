/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "app_config.h"

#include <esp_err.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_MODE_OFFLINE = 0,
    NET_MODE_STA,      /*!< Joined the configured network */
    NET_MODE_SETUP_AP, /*!< Running its own access point so it can be configured */
} net_mode_t;

typedef struct {
    net_mode_t mode;
    bool connected;
    char ssid[33];
    char ip[16];
    int8_t rssi;
} net_status_t;

/**
 * @brief Bring up networking.
 *
 * With stored credentials the device joins that network. Without them, or
 * after repeated join failures, it starts its own access point named after
 * the hostname so the configuration UI is always reachable — a reader that
 * cannot be configured because it cannot join Wi-Fi is a brick.
 */
esp_err_t net_manager_start(const net_config_t *cfg);

/** @brief Current link state. */
void net_manager_get_status(net_status_t *out);

/** @brief True once an IP address is assigned in either mode. */
bool net_manager_is_up(void);

#ifdef __cplusplus
}
#endif
