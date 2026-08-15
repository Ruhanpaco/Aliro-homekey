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
 * Called from the reader task or a timer callback, so an observer must not
 * block. There are two today -- MQTT, and the Matter door lock endpoint, which
 * has to report LockState the moment a tap opens the door.
 */
typedef void (*access_observer_cb_t)(const access_event_t *event, void *ctx);

/** @brief Register an observer. ESP_ERR_NO_MEM when the table is full. */
esp_err_t access_control_add_observer(access_observer_cb_t cb, void *ctx);

/** @brief Unregister an observer previously added. */
void access_control_remove_observer(access_observer_cb_t cb);

/** @brief True when the lock output is in its locked state. */
bool access_control_is_locked(void);

/**
 * @brief Register a credential that may open this lock.
 *
 * The key is copied, so the caller may pass a buffer that goes out of scope --
 * which a credential arriving over Matter always does. Registering a key that
 * is already present replaces it rather than filling a second slot.
 */
esp_err_t access_control_add_credential(const char *cred_pubkey_pem, size_t cred_pubkey_len, const char *label);

/** @brief Withdraw a credential. ESP_ERR_NOT_FOUND when it was not there. */
esp_err_t access_control_remove_credential(const char *cred_pubkey_pem, size_t cred_pubkey_len);

/**
 * @brief Recompute every stored key slot.
 *
 * A key slot is derived from the credential key *and* the reader's group
 * sub-identifier, so adopting a new reader identity invalidates every slot
 * derived under the old one. Without this the reader would answer every tap
 * with "unknown credential" and nothing would say why.
 */
esp_err_t access_control_refresh_key_slots(void);

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

/** @brief Drive the lock output back to locked now, cancelling any relock timer. */
esp_err_t access_control_lock(void);

#ifdef __cplusplus
}
#endif
