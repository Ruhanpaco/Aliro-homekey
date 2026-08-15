/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aliro_reader.h"

#include <esp_aliro.h>
#include <esp_aliro_utils.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <psa/crypto.h>
#include <sdkconfig.h>

#include <string.h>

static const char *const k_tag = "aliro/reader";

/*
 * The Aliro SDK allows one reader at a time and its callbacks carry no user
 * context, so this module is a singleton and the callbacks below trampoline
 * through this state.
 */
static struct {
    aliro_reader_config_t cfg;
    esp_aliro_reader_handle_t reader;
    TaskHandle_t task;
    volatile bool running;

    /* Per-transaction scratch, written by the lookup callback and read after
     * the session ends. Only the reader task touches it. */
    uint8_t key_slot[ALIRO_KEY_SLOT_MAX_LEN];
    size_t key_slot_len;
    bool key_slot_valid;
    bool credential_known;
} s_reader;

/* --- SDK callback trampolines ------------------------------------------- */

static esp_err_t on_message_exchange(const uint8_t *command, size_t command_len, uint8_t *response,
                                     size_t *response_len)
{
    const nfc_transport_t *t = s_reader.cfg.transport;
    return t->exchange(t->ctx, command, command_len, response, response_len);
}

static esp_err_t on_lookup_credential_pubkey(const uint8_t *key_slot, size_t key_slot_len, char *out_pubkey,
                                             size_t *out_pubkey_len)
{
    ESP_RETURN_ON_FALSE(key_slot && out_pubkey && out_pubkey_len, ESP_ERR_INVALID_ARG, k_tag,
                        "invalid key-slot lookup argument");

    /* Record who is asking, even when the lookup fails: an unknown credential
     * is exactly the event an operator wants to see in the log. */
    s_reader.key_slot_len = key_slot_len < sizeof(s_reader.key_slot) ? key_slot_len : sizeof(s_reader.key_slot);
    memcpy(s_reader.key_slot, key_slot, s_reader.key_slot_len);
    s_reader.key_slot_valid = true;

    const esp_err_t err =
        s_reader.cfg.lookup_credential(key_slot, key_slot_len, out_pubkey, out_pubkey_len);
    s_reader.credential_known = (err == ESP_OK);
    return err;
}

/* --- One transaction ----------------------------------------------------- */

static esp_err_t run_transaction(esp_aliro_transaction_type_t *out_txn_type)
{
    esp_aliro_session_config_t session_cfg = {
        .aid_type = ESP_ALIRO_NFC_AID_EXPEDITED_PHASE,
        .auth_policy = ESP_ALIRO_AUTH_POLICY_USER_DEVICE_SETTING_SECURE_ACTION,
    };

    esp_aliro_session_handle_t session = 0;
    esp_err_t err = esp_aliro_session_create(s_reader.reader, &session, &session_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* Expedited phase: mutual authentication. This is the phase that decides
     * whether the tap is genuine. */
    err = esp_aliro_session_run_expedited(session, on_message_exchange);

    if (err == ESP_OK) {
        (void)esp_aliro_session_get_transaction_type(session, out_txn_type);
        /* Exchange closes the transaction cleanly so the user device can show
         * its success feedback. Mailbox and step-up ride on this same call and
         * are left for a later milestone. */
        err = esp_aliro_session_run_exchange(session, on_message_exchange, ESP_ALIRO_CRYPTO_ENGINE_EXPEDITED);
    }

    (void)esp_aliro_session_delete(&session);
    return err;
}

static void reader_task(void *params)
{
    (void)params;
    const nfc_transport_t *t = s_reader.cfg.transport;

    while (s_reader.running) {
        t->poll(t->ctx);

        if (!t->activate(t->ctx)) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ALIRO_READER_POLL_INTERVAL_MS));
            continue;
        }

        const int64_t started_us = esp_timer_get_time();
        s_reader.key_slot_valid = false;
        s_reader.key_slot_len = 0;
        s_reader.credential_known = false;

        esp_aliro_transaction_type_t txn_type = ESP_ALIRO_TRANSACTION_STANDARD;
        const esp_err_t err = run_transaction(&txn_type);

        t->deactivate(t->ctx);

        aliro_reader_result_t result = {
            .err = err,
            .key_slot_valid = s_reader.key_slot_valid,
            .key_slot_len = s_reader.key_slot_len,
            .credential_known = s_reader.credential_known,
            .txn_type = txn_type,
            .duration_ms = (esp_timer_get_time() - started_us) / 1000,
        };
        memcpy(result.key_slot, s_reader.key_slot, s_reader.key_slot_len);

        if (err == ESP_OK) {
            ESP_LOGI(k_tag, "transaction ok (%s) in %lld ms",
                     txn_type == ESP_ALIRO_TRANSACTION_FAST ? "fast" : "standard", (long long)result.duration_ms);
        } else {
            ESP_LOGW(k_tag, "transaction failed after %lld ms: %s", (long long)result.duration_ms,
                     esp_err_to_name(err));
        }

        if (s_reader.cfg.on_result) {
            s_reader.cfg.on_result(&result, s_reader.cfg.user_ctx);
        }
    }

    s_reader.task = NULL;
    vTaskDelete(NULL);
}

/* --- Public API ---------------------------------------------------------- */

esp_err_t aliro_reader_sdk_init(size_t fast_transaction_slots)
{
    static bool initialized;
    if (initialized) {
        return ESP_OK;
    }

    /*
     * PSA has to be up before anything touches a key.
     *
     * esp_aliro_init() does not do it -- it only registers an entropy source
     * -- and neither does the key-slot derivation path:
     *
     *     esp_aliro_get_key_slot_from_cred_pubkey()
     *       -> crypto::public_key(pem)
     *            -> mbedtls_pk_parse_public_key()
     *            -> mbedtls_pk_import_into_psa()      <-- needs PSA
     *
     * Inside the SDK psa_crypto_init() is only reached through private_key,
     * HKDF and the AES-GCM helpers, all of which run once a transaction is
     * under way. Espressif's own example never notices, because it derives key
     * slots exclusively from inside the lookup callback, by which point a
     * session has already warmed PSA up. Deriving one at boot instead lands on
     * a cold PSA and the parse fails with a bare ESP_FAIL -- which reads
     * exactly like a malformed key, and cost this project two wrong theories
     * before the library was disassembled.
     *
     * The call is idempotent, so doing it here is free insurance.
     */
    const psa_status_t psa = psa_crypto_init();
    ESP_RETURN_ON_FALSE(psa == PSA_SUCCESS, ESP_FAIL, k_tag, "psa_crypto_init failed: %d", (int)psa);

    const esp_aliro_config_t sdk_cfg = {
        .storage_partition_name = NULL, /* default "nvs" partition */
        .fast_transaction_storage_size = fast_transaction_slots,
    };
    ESP_RETURN_ON_ERROR(esp_aliro_init(&sdk_cfg), k_tag, "esp_aliro_init failed");

    initialized = true;
    ESP_LOGI(k_tag, "Aliro SDK initialized");
    return ESP_OK;
}

esp_err_t aliro_reader_start(const aliro_reader_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->transport && cfg->reader_pubkey_pem && cfg->reader_privkey_pem &&
                            cfg->lookup_credential,
                        ESP_ERR_INVALID_ARG, k_tag, "incomplete reader configuration");
    ESP_RETURN_ON_FALSE(cfg->transport->init && cfg->transport->poll && cfg->transport->activate &&
                            cfg->transport->deactivate && cfg->transport->exchange,
                        ESP_ERR_INVALID_ARG, k_tag, "incomplete NFC transport");
    ESP_RETURN_ON_FALSE(!s_reader.running, ESP_ERR_INVALID_STATE, k_tag, "reader already running");

    s_reader.cfg = *cfg;

    const nfc_transport_t *t = s_reader.cfg.transport;
    ESP_RETURN_ON_ERROR(t->init(t->ctx), k_tag, "NFC transport '%s' failed to initialize", t->name);
    ESP_LOGI(k_tag, "NFC transport: %s", t->name);

    ESP_RETURN_ON_ERROR(aliro_reader_sdk_init(cfg->fast_transaction_slots), k_tag, "SDK init failed");

    esp_aliro_reader_config_t reader_cfg = {
        .reader_pubkey = s_reader.cfg.reader_pubkey_pem,
        .reader_privkey = s_reader.cfg.reader_privkey_pem,
    };
    memcpy(reader_cfg.group_identifier, s_reader.cfg.group_identifier, sizeof(reader_cfg.group_identifier));
    ESP_RETURN_ON_ERROR(esp_aliro_reader_create(&s_reader.reader, &reader_cfg), k_tag, "reader create failed");

    /* Key-slot lookup is what turns "somebody authenticated" into "this
     * credential authenticated", so it is mandatory here even though the SDK
     * treats it as optional. It must be configured before enable. */
    ESP_RETURN_ON_ERROR(esp_aliro_reader_enable_key_slot(s_reader.reader, on_lookup_credential_pubkey), k_tag,
                        "key-slot lookup registration failed");

    ESP_RETURN_ON_ERROR(esp_aliro_reader_enable(s_reader.reader), k_tag, "reader enable failed");

    s_reader.running = true;

#if CONFIG_FREERTOS_UNICORE
    const BaseType_t core = 0;
#else
    const BaseType_t core = CONFIG_ALIRO_READER_TASK_CORE;
#endif
    const BaseType_t ok =
        xTaskCreatePinnedToCore(reader_task, "aliro_reader", CONFIG_ALIRO_READER_TASK_STACK_SIZE, NULL,
                                CONFIG_ALIRO_READER_TASK_PRIORITY, &s_reader.task, core);
    if (ok != pdPASS) {
        s_reader.running = false;
        (void)esp_aliro_reader_delete(&s_reader.reader);
        ESP_LOGE(k_tag, "failed to create reader task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t aliro_reader_stop(void)
{
    if (!s_reader.running) {
        return ESP_OK;
    }

    s_reader.running = false;
    /* Let the task observe the flag and exit before tearing the reader down;
     * deleting a reader with a live session fails. */
    while (s_reader.task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ALIRO_READER_POLL_INTERVAL_MS));
    }

    ESP_RETURN_ON_ERROR(esp_aliro_reader_disable(s_reader.reader), k_tag, "reader disable failed");
    return esp_aliro_reader_delete(&s_reader.reader);
}

esp_err_t aliro_reader_key_slot_from_pubkey(const char *cred_pubkey_pem, size_t cred_pubkey_len, uint8_t *key_slot,
                                            size_t *key_slot_len)
{
    return esp_aliro_get_key_slot_from_cred_pubkey(cred_pubkey_pem, cred_pubkey_len, key_slot, key_slot_len);
}

bool aliro_reader_is_running(void)
{
    return s_reader.running && s_reader.reader != 0;
}

esp_err_t aliro_reader_get_public_key_raw(uint8_t *out, size_t *out_len)
{
    ESP_RETURN_ON_FALSE(s_reader.reader, ESP_ERR_INVALID_STATE, k_tag, "reader not created");
    return esp_aliro_reader_get_public_key_raw_data(s_reader.reader, out, out_len);
}

esp_err_t aliro_reader_get_group_identifier(uint8_t *out, size_t *out_len)
{
    ESP_RETURN_ON_FALSE(s_reader.reader, ESP_ERR_INVALID_STATE, k_tag, "reader not created");
    return esp_aliro_reader_get_group_identifier(s_reader.reader, out, out_len);
}

esp_err_t aliro_reader_get_group_sub_identifier(uint8_t *out, size_t *out_len)
{
    ESP_RETURN_ON_FALSE(s_reader.reader, ESP_ERR_INVALID_STATE, k_tag, "reader not created");
    return esp_aliro_reader_get_group_sub_identifier(s_reader.reader, out, out_len);
}

esp_err_t aliro_reader_pubkey_pem_from_raw(const uint8_t *raw, size_t raw_len, char *pem, size_t *pem_len)
{
    return esp_aliro_get_pubkey_pem_from_raw_data(raw, raw_len, pem, pem_len);
}

esp_err_t aliro_reader_privkey_pem_from_raw(const uint8_t *raw, size_t raw_len, char *pem, size_t *pem_len)
{
    return esp_aliro_get_privkey_pem_from_raw_data(raw, raw_len, pem, pem_len);
}

esp_err_t aliro_reader_log_identity(void)
{
    ESP_RETURN_ON_FALSE(s_reader.reader, ESP_ERR_INVALID_STATE, k_tag, "reader not created");

    uint8_t group_id[ALIRO_GROUP_IDENTIFIER_LEN];
    size_t group_id_len = sizeof(group_id);
    ESP_RETURN_ON_ERROR(esp_aliro_reader_get_group_identifier(s_reader.reader, group_id, &group_id_len), k_tag,
                        "group identifier read failed");
    ESP_LOG_BUFFER_HEX_LEVEL("aliro/group-id", group_id, group_id_len, ESP_LOG_INFO);

    uint8_t group_sub_id[ALIRO_GROUP_IDENTIFIER_LEN];
    size_t group_sub_id_len = sizeof(group_sub_id);
    if (esp_aliro_reader_get_group_sub_identifier(s_reader.reader, group_sub_id, &group_sub_id_len) == ESP_OK) {
        ESP_LOG_BUFFER_HEX_LEVEL("aliro/group-sub-id", group_sub_id, group_sub_id_len, ESP_LOG_INFO);
    }

    uint8_t pubkey[65];
    size_t pubkey_len = sizeof(pubkey);
    ESP_RETURN_ON_ERROR(esp_aliro_reader_get_public_key_raw_data(s_reader.reader, pubkey, &pubkey_len), k_tag,
                        "public key read failed");
    ESP_LOG_BUFFER_HEX_LEVEL("aliro/reader-pubkey", pubkey, pubkey_len, ESP_LOG_INFO);

    return ESP_OK;
}
