/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The Matter face of this reader.
 *
 * Aliro never travels over Wi-Fi -- a tap is NFC from start to finish, and the
 * reader in components/aliro_reader is what handles it. What Matter provides is
 * the *provisioning* plane: the standard way a phone ecosystem hands a lock its
 * reader key pair and then enrolls endpoint keys for the phones that may open
 * it. Those two commands, SetAliroReaderConfig and SetCredential, only exist
 * inside the Door Lock cluster (0x0101), which is why this firmware presents
 * itself as a door lock even on a board with no motor attached.
 *
 * This module is the only place that knows about Matter. It is compiled out
 * entirely unless CONFIG_ALIRO_MATTER_ENABLE is set, and the stubs in
 * matter_lock_disabled.c keep every caller compiling either way.
 */

/** @brief An Aliro reader group identifier is 16 bytes. */
#define MATTER_LOCK_GROUP_ID_LEN 16

/**
 * @brief What Matter is allowed to do to the rest of the firmware.
 *
 * Deliberately a hook table rather than direct calls: the Matter side has no
 * business knowing about app_config, NVS or the credential store, and the
 * wiring that does live in app_main, the same as every other subsystem here.
 */
typedef struct {
    /**
     * @brief Adopt a reader identity sent by a Matter controller.
     *
     * Called from the Matter task when a controller runs SetAliroReaderConfig.
     * The implementation is expected to persist the identity and restart the
     * reader with it; the keys are PEM and are not retained after the call.
     */
    esp_err_t (*set_reader_identity)(const char *pubkey_pem, const char *privkey_pem, const uint8_t *group_id,
                                     size_t group_id_len);

    /** @brief Forget the reader identity Matter provisioned. */
    esp_err_t (*clear_reader_identity)(void);

    /** @brief Enroll a phone's endpoint key. @p pubkey_pem is not retained. */
    esp_err_t (*add_credential)(const char *pubkey_pem, size_t pubkey_len, const char *label);

    /** @brief Drop an endpoint key. */
    esp_err_t (*remove_credential)(const char *pubkey_pem, size_t pubkey_len);

    /** @brief Unlock now, for a remote Unlock command. */
    esp_err_t (*unlock)(void);

    /** @brief Lock now, for a remote Lock command. */
    esp_err_t (*lock)(void);

    /** @brief Current lock output state, reported as the LockState attribute. */
    bool (*is_locked)(void);
} matter_lock_hooks_t;

/**
 * @brief Bring up the Matter stack and publish the door lock endpoint.
 *
 * Call after the network is up. Returns ESP_ERR_NOT_SUPPORTED in a build
 * without CONFIG_ALIRO_MATTER_ENABLE.
 */
esp_err_t matter_lock_start(const matter_lock_hooks_t *hooks);

/** @brief True when this firmware was built with Matter support at all. */
bool matter_lock_available(void);

/** @brief True once the stack is up and the endpoint exists. */
bool matter_lock_running(void);

/** @brief Number of fabrics this device has been commissioned into. */
size_t matter_lock_fabric_count(void);

/** @brief True when a controller has given us an Aliro reader identity. */
bool matter_lock_reader_configured(void);

/**
 * @brief Onboarding payload, the "MT:..." string a commissioner scans.
 *
 * Stable for the life of the process. Empty string before the stack starts.
 */
const char *matter_lock_qr_payload(void);

/** @brief Manual pairing code, for entering by hand instead of scanning. */
const char *matter_lock_manual_code(void);

/** @brief URL that renders @ref matter_lock_qr_payload as a scannable code. */
const char *matter_lock_qr_url(void);

/**
 * @brief Reopen commissioning for 5 minutes.
 *
 * A device that has never been commissioned already advertises on boot; this
 * is for adding a second ecosystem, or recovering after a controller was lost.
 */
esp_err_t matter_lock_open_commissioning_window(void);

/**
 * @brief Publish a lock state change to Matter.
 *
 * Safe to call from any task, including the reader task straight after a tap.
 */
void matter_lock_report_lock_state(bool locked);

#ifdef __cplusplus
}
#endif
