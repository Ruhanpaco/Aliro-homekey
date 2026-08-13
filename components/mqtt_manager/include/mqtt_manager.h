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

/**
 * @brief MQTT bridge, modelled on HomeKey-ESP32's but cut to this project.
 *
 * Topics, all under the configured base:
 *
 *   <base>/status        online / offline    (retained, last will)
 *   <base>/lock/state    locked / unlocked   (retained)
 *   <base>/lock/set      LOCK | UNLOCK       (subscribed)
 *   <base>/auth          JSON, one per tap
 *
 * There is no HomeKit state machine here, so the current/target state pair,
 * jammed state, battery level and alt-action topics that project carries have
 * nothing to map onto and are not implemented.
 */
esp_err_t mqtt_manager_start(const mqtt_config_t *cfg, const char *device_name);

esp_err_t mqtt_manager_stop(void);

/** @brief True when the broker connection is up. */
bool mqtt_manager_is_connected(void);

/** @brief True when MQTT is enabled in the configuration. */
bool mqtt_manager_is_enabled(void);

#ifdef __cplusplus
}
#endif
