/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "nfc_transport.h"

#include <esp_aliro_types.h>
#include <esp_err.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Aliro key slots are 8 bytes. */
#define ALIRO_KEY_SLOT_MAX_LEN 8

/** @brief Reader group identifiers are 16 bytes. */
#define ALIRO_GROUP_IDENTIFIER_LEN 16

/**
 * @brief Outcome of one tap.
 *
 * `err == ESP_OK` means the Aliro transaction completed and the user device
 * authenticated. It does *not* mean the door should open: that is the access
 * decision, and it belongs to the access-control layer, not here.
 */
typedef struct {
    esp_err_t err;                             /*!< ESP_OK when the transaction completed */
    bool key_slot_valid;                       /*!< True when the device presented a key slot */
    uint8_t key_slot[ALIRO_KEY_SLOT_MAX_LEN];  /*!< Key slot of the presenting credential */
    size_t key_slot_len;                       /*!< Length of @c key_slot in bytes */
    bool credential_known;                     /*!< True when the key slot resolved to a stored credential */
    esp_aliro_transaction_type_t txn_type;     /*!< Standard or fast transaction */
    int64_t duration_ms;                       /*!< Wall time from device detection to teardown */
} aliro_reader_result_t;

/**
 * @brief Resolve a credential public key from a key slot.
 *
 * Called from the reader task in the middle of a transaction, so it must be
 * quick and must not block on the network.
 *
 * @param[in]    key_slot     Key slot presented by the user device
 * @param[in]    key_slot_len Length of @p key_slot in bytes
 * @param[out]   out_pubkey   Buffer for the X.509 PEM credential public key
 * @param[inout] out_pubkey_len Capacity on input, PEM length on output
 *
 * @return ESP_OK when found, ESP_ERR_NOT_FOUND when the slot is unknown
 */
typedef esp_err_t (*aliro_reader_lookup_cb_t)(const uint8_t *key_slot, size_t key_slot_len, char *out_pubkey,
                                              size_t *out_pubkey_len);

/** @brief Called once per tap, from the reader task, after the session ends. */
typedef void (*aliro_reader_result_cb_t)(const aliro_reader_result_t *result, void *user_ctx);

typedef struct {
    /** @brief Reader group identifier. Identifies this installation to user devices. */
    uint8_t group_identifier[ALIRO_GROUP_IDENTIFIER_LEN];

    /** @brief Reader key pair, X.509 PEM. Must outlive the reader. */
    const char *reader_pubkey_pem;
    const char *reader_privkey_pem;

    /** @brief NFC frontend to run transactions over. Must outlive the reader. */
    const nfc_transport_t *transport;

    /** @brief Credential lookup. Required: it is how the reader learns who tapped. */
    aliro_reader_lookup_cb_t lookup_credential;

    /** @brief Result sink and its context. */
    aliro_reader_result_cb_t on_result;
    void *user_ctx;

    /** @brief Fast-transaction key slots to persist. 0 disables fast transactions. */
    size_t fast_transaction_slots;
} aliro_reader_config_t;

/**
 * @brief Initialize the Aliro SDK.
 *
 * Everything else in this header, including
 * aliro_reader_key_slot_from_pubkey(), needs the SDK up first — the helpers
 * fail with ESP_FAIL otherwise, which reads like a bad key rather than a
 * missing init. Idempotent, and aliro_reader_start() calls it for you.
 *
 * @param[in] fast_transaction_slots Persistent fast-transaction key slots,
 *                                   0 to disable fast transactions
 */
esp_err_t aliro_reader_sdk_init(size_t fast_transaction_slots);

/**
 * @brief Initialize the Aliro SDK, create the reader and start the polling task.
 *
 * The SDK supports one reader at a time, so this module is a singleton.
 *
 * @param[in] cfg Reader configuration, copied by value
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the configuration is incomplete
 *      - ESP_ERR_INVALID_STATE if the reader is already running
 */
esp_err_t aliro_reader_start(const aliro_reader_config_t *cfg);

/** @brief Stop the polling task, disable and delete the reader. */
esp_err_t aliro_reader_stop(void);

/**
 * @brief Derive the Aliro key slot for a credential public key.
 *
 * Thin wrapper over the SDK helper, so credential storage does not have to
 * depend on esp_aliro_lib directly.
 */
esp_err_t aliro_reader_key_slot_from_pubkey(const char *cred_pubkey_pem, size_t cred_pubkey_len, uint8_t *key_slot,
                                            size_t *key_slot_len);

/**
 * @brief Log the reader identity (group ID, sub-ID, public key).
 *
 * These are the values a provisioning system needs in order to issue a
 * credential for this reader.
 */
esp_err_t aliro_reader_log_identity(void);

#ifdef __cplusplus
}
#endif
