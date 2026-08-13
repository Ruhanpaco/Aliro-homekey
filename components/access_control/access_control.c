/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "access_control.h"

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <stdio.h>
#include <string.h>

static const char *const k_tag = "aliro/access";

static lock_config_t s_lock;
static bool s_locked = true;
static access_observer_cb_t s_observer;
static void *s_observer_ctx;

static void notify(const access_event_t *event)
{
    if (s_observer) {
        s_observer(event, s_observer_ctx);
    }
}

void access_control_set_observer(access_observer_cb_t cb, void *ctx)
{
    s_observer = cb;
    s_observer_ctx = ctx;
}

bool access_control_is_locked(void)
{
    return s_locked;
}

#define LOCKED_LEVEL   (s_lock.active_low ? 1 : 0)
#define UNLOCKED_LEVEL (s_lock.active_low ? 0 : 1)

typedef struct {
    const char *pubkey_pem;
    size_t pubkey_len;
    uint8_t key_slot[ALIRO_KEY_SLOT_MAX_LEN];
    size_t key_slot_len;
    char label[24];
    bool used;
} credential_entry_t;

static credential_entry_t s_credentials[CONFIG_ALIRO_MAX_CREDENTIALS];
static esp_timer_handle_t s_relock_timer;

/* --- Credential store ---------------------------------------------------- */

esp_err_t access_control_add_credential(const char *cred_pubkey_pem, size_t cred_pubkey_len, const char *label)
{
    ESP_RETURN_ON_FALSE(cred_pubkey_pem && cred_pubkey_len > 0, ESP_ERR_INVALID_ARG, k_tag, "invalid credential");

    credential_entry_t *slot = NULL;
    for (size_t i = 0; i < CONFIG_ALIRO_MAX_CREDENTIALS; i++) {
        if (!s_credentials[i].used) {
            slot = &s_credentials[i];
            break;
        }
    }
    ESP_RETURN_ON_FALSE(slot, ESP_ERR_NO_MEM, k_tag, "credential table full");

    slot->key_slot_len = sizeof(slot->key_slot);
    ESP_RETURN_ON_ERROR(
        aliro_reader_key_slot_from_pubkey(cred_pubkey_pem, cred_pubkey_len, slot->key_slot, &slot->key_slot_len),
        k_tag, "failed to derive key slot");

    slot->pubkey_pem = cred_pubkey_pem;
    slot->pubkey_len = cred_pubkey_len;
    strlcpy(slot->label, label ? label : "unnamed", sizeof(slot->label));
    slot->used = true;

    ESP_LOGI(k_tag, "credential '%s' registered", slot->label);
    ESP_LOG_BUFFER_HEX_LEVEL(k_tag, slot->key_slot, slot->key_slot_len, ESP_LOG_DEBUG);
    return ESP_OK;
}

size_t access_control_credential_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < CONFIG_ALIRO_MAX_CREDENTIALS; i++) {
        count += s_credentials[i].used ? 1 : 0;
    }
    return count;
}

static const credential_entry_t *find_by_key_slot(const uint8_t *key_slot, size_t key_slot_len)
{
    for (size_t i = 0; i < CONFIG_ALIRO_MAX_CREDENTIALS; i++) {
        const credential_entry_t *c = &s_credentials[i];
        if (c->used && c->key_slot_len == key_slot_len && memcmp(c->key_slot, key_slot, key_slot_len) == 0) {
            return c;
        }
    }
    return NULL;
}

esp_err_t access_control_lookup_credential(const uint8_t *key_slot, size_t key_slot_len, char *out_pubkey,
                                           size_t *out_pubkey_len)
{
    ESP_RETURN_ON_FALSE(key_slot && out_pubkey && out_pubkey_len, ESP_ERR_INVALID_ARG, k_tag, "invalid lookup");

    const credential_entry_t *c = find_by_key_slot(key_slot, key_slot_len);
    if (!c) {
        return ESP_ERR_NOT_FOUND;
    }

    if (*out_pubkey_len < c->pubkey_len) {
        *out_pubkey_len = c->pubkey_len;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out_pubkey, c->pubkey_pem, c->pubkey_len);
    *out_pubkey_len = c->pubkey_len;
    return ESP_OK;
}

/* --- Lock output --------------------------------------------------------- */

static void relock(void *arg)
{
    (void)arg;
    gpio_set_level(s_lock.gpio, LOCKED_LEVEL);
    s_locked = true;
    ESP_LOGI(k_tag, "locked");
    notify(&(access_event_t){.type = ACCESS_EVENT_LOCK_STATE, .locked = true});
}

esp_err_t access_control_init(const lock_config_t *lock)
{
    ESP_RETURN_ON_FALSE(lock && lock->gpio != APP_CFG_PIN_UNSET, ESP_ERR_INVALID_ARG, k_tag, "no lock output pin");
    s_lock = *lock;

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_lock.gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), k_tag, "lock GPIO %d config failed", s_lock.gpio);
    ESP_RETURN_ON_ERROR(gpio_set_level(s_lock.gpio, LOCKED_LEVEL), k_tag, "lock GPIO set failed");

    const esp_timer_create_args_t timer_args = {
        .callback = relock,
        .name = "relock",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_relock_timer), k_tag, "relock timer create failed");

    ESP_LOGI(k_tag, "lock output on GPIO %d (active %s), unlock %u ms", s_lock.gpio,
             s_lock.active_low ? "low" : "high", (unsigned)s_lock.unlock_ms);
    return ESP_OK;
}

esp_err_t access_control_unlock(void)
{
    ESP_RETURN_ON_FALSE(s_relock_timer, ESP_ERR_INVALID_STATE, k_tag, "access control not initialized");

    (void)esp_timer_stop(s_relock_timer); /* a second tap re-arms the full duration */
    ESP_RETURN_ON_ERROR(gpio_set_level(s_lock.gpio, UNLOCKED_LEVEL), k_tag, "lock GPIO set failed");
    s_locked = false;
    ESP_LOGI(k_tag, "unlocked for %u ms", (unsigned)s_lock.unlock_ms);
    notify(&(access_event_t){.type = ACCESS_EVENT_LOCK_STATE, .locked = false});
    return esp_timer_start_once(s_relock_timer, (uint64_t)s_lock.unlock_ms * 1000);
}

/* --- Decision ------------------------------------------------------------ */

static void tap_event(bool granted, const char *reason, const char *label, const aliro_reader_result_t *result)
{
    access_event_t event = {.type = ACCESS_EVENT_TAP, .granted = granted, .reason = reason};
    snprintf(event.credential, sizeof(event.credential), "%s", label ? label : "");
    for (size_t i = 0; i < result->key_slot_len && i * 2 + 2 < sizeof(event.key_slot_hex); i++) {
        snprintf(event.key_slot_hex + i * 2, 3, "%02X", result->key_slot[i]);
    }
    notify(&event);
}

void access_control_on_reader_result(const aliro_reader_result_t *result, void *user_ctx)
{
    (void)user_ctx;

    if (result->err != ESP_OK) {
        ESP_LOGW(k_tag, "denied: transaction failed (%s)", esp_err_to_name(result->err));
        tap_event(false, "transaction failed", NULL, result);
        return;
    }

    if (!result->key_slot_valid) {
        /* The device authenticated but never presented a key slot, so the
         * reader cannot say *which* credential this was. Refuse rather than
         * open a door for an unidentified holder. */
        ESP_LOGW(k_tag, "denied: authenticated device presented no key slot");
        tap_event(false, "no key slot", NULL, result);
        return;
    }

    const credential_entry_t *c = find_by_key_slot(result->key_slot, result->key_slot_len);
    if (!c || !result->credential_known) {
        ESP_LOGW(k_tag, "denied: unknown credential");
        ESP_LOG_BUFFER_HEX_LEVEL(k_tag, result->key_slot, result->key_slot_len, ESP_LOG_WARN);
        tap_event(false, "unknown credential", NULL, result);
        return;
    }

    ESP_LOGI(k_tag, "granted: '%s' (%s transaction, %lld ms)", c->label,
             result->txn_type == ESP_ALIRO_TRANSACTION_FAST ? "fast" : "standard", (long long)result->duration_ms);
    tap_event(true, "granted", c->label, result);
    ESP_ERROR_CHECK_WITHOUT_ABORT(access_control_unlock());
}
