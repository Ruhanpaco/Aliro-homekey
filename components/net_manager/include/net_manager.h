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
    /* The setup AP's own address, while it is running. Once a join succeeds
     * `ip` becomes the address on the joined network, but clients are still
     * attached to the access point until they leave it -- anything answering
     * those clients has to point at this one. */
    char ap_ip[16];
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

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool open; /*!< No passphrase required */
} net_scan_result_t;

/**
 * @brief Scan for nearby networks, strongest first.
 *
 * Blocks for a second or two. Duplicate SSIDs are collapsed to the strongest
 * BSSID, because a mesh network appearing five times is noise to someone
 * picking their home network out of a list.
 *
 * @param[out] out       Destination array
 * @param[in]  max       Capacity of @p out
 * @param[out] out_count Networks written
 */
esp_err_t net_manager_scan(net_scan_result_t *out, size_t max, size_t *out_count);

/**
 * @brief Join a network now, without rebooting.
 *
 * The setup access point stays up for the duration, so the browser that asked
 * for this keeps its connection and can be told the new address. On success
 * the credentials become the running configuration; persisting them is the
 * caller's job.
 *
 * @param[in]  ssid       Network to join
 * @param[in]  password   Passphrase, or "" for an open network
 * @param[in]  timeout_ms How long to wait for an IP address, per attempt. Two
 *                        attempts are made: a WPA3-SAE association routinely
 *                        fails once and then succeeds unchanged.
 * @param[out] out_ip     Assigned address, may be NULL
 * @param[in]  out_ip_len Size of @p out_ip
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if no address arrived, or
 *         ESP_ERR_WIFI_NOT_CONNECT if the access point rejected us.
 */
esp_err_t net_manager_join(const char *ssid, const char *password, uint32_t timeout_ms, char *out_ip,
                           size_t out_ip_len);

/**
 * @brief Try the stored network again, and stand down the access point if it works.
 *
 * After three failed attempts the reader stops reconnecting on its own and
 * brings up its access point, so this is the deliberate "try again" a person
 * reaches for from the portal once the router is back. On success the access
 * point is stopped a few seconds later -- long enough for the answer to reach
 * the browser that asked -- leaving the reader on Wi-Fi only.
 *
 * @param[in]  timeout_ms How long to wait for an address, per attempt
 * @param[out] out_ip     Assigned address, may be NULL
 * @param[in]  out_ip_len Size of @p out_ip
 * @return ESP_ERR_INVALID_STATE when no network has ever been configured.
 */
esp_err_t net_manager_reconnect(uint32_t timeout_ms, char *out_ip, size_t out_ip_len);

#ifdef __cplusplus
}
#endif
