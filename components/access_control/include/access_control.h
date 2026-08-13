/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "aliro_reader.h"
#include "app_config.h"

#include <esp_err.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Who is allowed in, and what happens when they are.
 *
 * Kept separate from the reader on purpose: the reader answers "is this tap
 * cryptographically genuine?", this layer answers "should the door open?".
 * Schedules, per-user rules and a persisted credential database all land here
 * later without touching the protocol code.
 */

typedef enum {
    ACCESS_EVENT_TAP,        /*!< A user device was presented */
    ACCESS_EVENT_LOCK_STATE, /*!< The lock output changed */
} access_event_type_t;

typedef struct {
    access_event_type_t type;
    bool granted;             /*!< TAP: the door was opened */
    bool locked;              /*!< LOCK_STATE: current state */
    const char *reason;       /*!< TAP: why it was refused, or "granted" */
    char credential[24];      /*!< TAP: label, or "" when unknown */
    char key_slot_hex[17];    /*!< TAP: key slot as hex, or "" */
} access_event_t;

/**
 * @brief Watch access events without access_control knowing who is watching.
 *
 * One observer, called from the reader task or a timer callback, so it must
 * not block. MQTT is the only consumer today; an event loop replaces this the
 * moment there is a second one.
 */
typedef void (*access_observer_cb_t)(const access_event_t *event, void *ctx);

void access_control_set_observer(access_observer_cb_t cb, void *ctx);

/** @brief True when the lock output is in its locked state. */
bool access_control_is_locked(void);

/** @brief Register a credential that may open this lock. */
esp_err_t access_control_add_credential(const char *cred_pubkey_pem, size_t cred_pubkey_len, const char *label);

/** @brief Number of credentials currently registered. */
size_t access_control_credential_count(void);

/**
 * @brief Bring up the lock output from the running configuration.
 *
 * @param[in] lock Pin, polarity and unlock duration, as edited in the web UI
 */
esp_err_t access_control_init(const lock_config_t *lock);

/** @brief Credential lookup, for aliro_reader_config_t::lookup_credential. */
esp_err_t access_control_lookup_credential(const uint8_t *key_slot, size_t key_slot_len, char *out_pubkey,
                                           size_t *out_pubkey_len);

/** @brief Access decision, for aliro_reader_config_t::on_result. */
void access_control_on_reader_result(const aliro_reader_result_t *result, void *user_ctx);

/** @brief Drive the lock output to its unlocked state for the configured time. */
esp_err_t access_control_unlock(void);

#ifdef __cplusplus
}
#endif
