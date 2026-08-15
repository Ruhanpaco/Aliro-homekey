/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration UI and REST API.
 *
 * Routes, trimmed from HomeKey-ESP32's set to what this project actually has:
 *
 *   GET  /                 the UI
 *   GET  /api/status       chip, uptime, heap, network, reader state
 *   GET  /api/hardware     which pins this chip allows, for the pin pickers
 *   GET  /api/config       running configuration, passwords masked
 *   POST /api/config       validate and persist a configuration
 *   POST /api/config/reset erase the stored configuration
 *   POST /api/reboot       restart, so a new configuration takes effect
 *   POST /api/unlock       drive the lock output now
 *
 * Deliberately absent, unlike the project this borrows from: HomeKit pairing,
 * Ethernet, NeoPixel, OTA upload, certificate management and HTTPS.
 */

/** @brief Reported through /api/status so the UI can show the reader state. */
typedef struct {
    size_t (*credential_count)(void);
    const char *(*transport_name)(void);
    bool (*lock_is_locked)(void);
    bool (*mqtt_enabled)(void);
    bool (*mqtt_connected)(void);
    esp_err_t (*unlock)(void);
} web_server_hooks_t;

/** @brief Event observer callback for web server events (config changes, lock events, etc.) */
typedef void (*web_server_event_observer_t)(const char *event_type, const void *data);

esp_err_t web_server_start(const web_server_hooks_t *hooks);

esp_err_t web_server_stop(void);

/** @brief Register an event observer to receive notifications of config changes and lock events. */
/**
 * @brief Watch events the web server publishes.
 *
 * @c data is a @c cJSON object. It is typed void here so that the public
 * header does not drag cJSON into every consumer.
 */
typedef void (*web_server_event_cb_t)(const char *event_type, const void *data);

void web_server_register_event_observer(web_server_event_cb_t observer);

#ifdef __cplusplus
}
#endif
