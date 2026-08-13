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
 * @brief Interactive debug console on the serial port.
 *
 * `idf.py monitor` already shows the log stream. This adds the other half:
 * being able to ask the reader questions and poke it, on a bench where the
 * web UI may not be reachable yet — which is exactly the situation during
 * first bring-up, before Wi-Fi credentials exist.
 *
 * Commands:
 *
 *   help                    list commands
 *   status                  device, network, reader and MQTT state
 *   config                  the running configuration, passwords masked
 *   wifi <ssid> [password]  set Wi-Fi credentials and persist them
 *   unlock                  drive the lock output, as a granted tap would
 *   nfc                     NFC wiring, and what the driver situation is
 *   identity                reader group id, sub-id and public key
 *   factory-reset           erase the stored configuration
 *   restart                 reboot
 */

/** @brief Reported by `status`, so the console does not depend on everything. */
typedef struct {
    size_t (*credential_count)(void);
    const char *(*transport_name)(void);
    bool (*mqtt_enabled)(void);
    bool (*mqtt_connected)(void);
} serial_console_hooks_t;

/**
 * @brief Start the REPL on the console UART.
 *
 * Safe to call before Wi-Fi is up; it does not depend on the network.
 */
esp_err_t serial_console_start(const serial_console_hooks_t *hooks);

#ifdef __cplusplus
}
#endif
