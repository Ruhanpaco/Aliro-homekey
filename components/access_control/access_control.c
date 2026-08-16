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
#include <stdlib.h>
#include <string.h>

static const char *const k_tag = "aliro/access";

#define ACCESS_MAX_OBSERVERS 3

static lock_config_t s_lock;
static bool s_locked = true;

static struct {
    access_observer_cb_t cb;
    void *ctx;
} s_observers[ACCESS_MAX_OBSERVERS];

static void notify(const access_event_t *event)
{
    for (size_t i = 0; i < ACCESS_MAX_OBSERVERS; i++) {
        if (s_observers[i].cb) {
            s_observers[i].cb(event, s_observers[i].ctx);
        }
    }
}

esp_err_t access_control_add_observer(access_observer_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, k_tag, "no observer");

    for (size_t i = 0; i < ACCESS_MAX_OBSERVERS; i++) {
        if (s_observers[i].cb == NULL || s_observers[i].cb == cb) {
            s_observers[i].cb = cb;
            s_observers[i].ctx = ctx;
            return ESP_OK;
        }
    }
    ESP_LOGE(k_tag, "no room for another access observer");
    return ESP_ERR_NO_MEM;
}

void access_control_remove_observer(access_observer_cb_t cb)
{
    for (size_t i = 0; i < ACCESS_MAX_OBSERVERS; i++) {
        if (s_observers[i].cb == cb) {
            s_observers[i].cb = NULL;
            s_observers[i].ctx = NULL;
        }
    }
}

bool access_control_is_locked(void)
{
    return s_locked;
}

#define LOCKED_LEVEL   (s_lock.active_low ? 1 : 0)
#define UNLOCKED_LEVEL (s_lock.active_low ? 0 : 1)

typedef struct {
    char *pubkey_pem; /*!< owned: credentials arrive on other people's stacks */
    size_t pubkey_len;
    uint8_t key_slot[ALIRO_KEY_SLOT_MAX_LEN];
    size_t key_slot_len;
    char label[24];
    bool used;
} credential_entry_t;

static credential_entry_t s_credentials[CONFIG_ALIRO_MAX_CREDENTIALS];
static esp_timer_handle_t s_relock_timer;

/* --- Credential store ---------------------------------------------------- */

static credential_entry_t *find_by_key_slot(const uint8_t *key_slot, size_t key_slot_len);

/**
 * @brief Derive a key slot, and settle which PEM length the SDK wanted.
 *
 * @param[inout] pem_len Length offered on input, length that actually worked
 *                       on output
 */
static esp_err_t derive_key_slot(const char *cred_pubkey_pem, size_t *pem_len, uint8_t *key_slot, size_t *key_slot_len,
                                 const char *label)
{
    *key_slot_len = ALIRO_KEY_SLOT_MAX_LEN;
    esp_err_t err = aliro_reader_key_slot_from_pubkey(cred_pubkey_pem, *pem_len, key_slot, key_slot_len);

    if (err != ESP_OK) {
        /*
         * The SDK reports ESP_FAIL for anything it cannot parse, which covers
         * a malformed key, the wrong length convention, and (as this project
         * found the hard way) simply running out of stack. Describe the input
         * and try the other length convention, so one boot log says which it
         * is instead of leaving it to guesswork.
         *
         * The embedded PEM is NUL-terminated by CMake's TEXT mode and the
         * length spans that NUL, which is what mbedTLS wants. A caller that
         * passes strlen() instead lands one byte short.
         */
        const bool nul_terminated = *pem_len > 0 && cred_pubkey_pem[*pem_len - 1] == '\0';
        ESP_LOGE(k_tag, "key slot derivation failed for '%s': %d bytes, %s, starts '%.26s'",
                 label ? label : "unnamed", (int)*pem_len,
                 nul_terminated ? "NUL-terminated" : "not NUL-terminated", cred_pubkey_pem);

        if (nul_terminated) {
            *key_slot_len = ALIRO_KEY_SLOT_MAX_LEN;
            err = aliro_reader_key_slot_from_pubkey(cred_pubkey_pem, *pem_len - 1, key_slot, key_slot_len);
            if (err == ESP_OK) {
                ESP_LOGW(k_tag, "the SDK wants the length WITHOUT the trailing NUL; using %d bytes",
                         (int)*pem_len - 1);
                *pem_len -= 1;
            }
        }
    }
    return err;
}

esp_err_t access_control_add_credential(const char *cred_pubkey_pem, size_t cred_pubkey_len, const char *label)
{
    ESP_RETURN_ON_FALSE(cred_pubkey_pem && cred_pubkey_len > 0, ESP_ERR_INVALID_ARG, k_tag, "invalid credential");

    uint8_t key_slot[ALIRO_KEY_SLOT_MAX_LEN];
    size_t key_slot_len = 0;
    size_t pem_len = cred_pubkey_len;
    ESP_RETURN_ON_ERROR(derive_key_slot(cred_pubkey_pem, &pem_len, key_slot, &key_slot_len, label), k_tag,
                        "failed to derive key slot");

    /* A controller re-sending a credential it already sent is normal -- it
     * happens on every re-commissioning -- and must not consume a second
     * slot. */
    credential_entry_t *slot = find_by_key_slot(key_slot, key_slot_len);
    const bool replacing = slot != NULL;

    if (!slot) {
        for (size_t i = 0; i < CONFIG_ALIRO_MAX_CREDENTIALS; i++) {
            if (!s_credentials[i].used) {
                slot = &s_credentials[i];
                break;
            }
        }
    }
    ESP_RETURN_ON_FALSE(slot, ESP_ERR_NO_MEM, k_tag, "credential table full");

    /* Copied, because a credential provisioned over Matter lives on the Matter
     * task's stack and is gone as soon as the command returns. */
    char *copy = malloc(pem_len);
    ESP_RETURN_ON_FALSE(copy, ESP_ERR_NO_MEM, k_tag, "out of memory storing credential '%s'",
                        label ? label : "unnamed");
    memcpy(copy, cred_pubkey_pem, pem_len);

    free(slot->pubkey_pem);
    slot->pubkey_pem = copy;
    slot->pubkey_len = pem_len;
    memcpy(slot->key_slot, key_slot, key_slot_len);
    slot->key_slot_len = key_slot_len;
    strlcpy(slot->label, label ? label : "unnamed", sizeof(slot->label));
    slot->used = true;

    ESP_LOGI(k_tag, "credential '%s' %s", slot->label, replacing ? "updated" : "registered");
    ESP_LOG_BUFFER_HEX_LEVEL(k_tag, slot->key_slot, slot->key_slot_len, ESP_LOG_DEBUG);
    return ESP_OK;
}

esp_err_t access_control_remove_credential(const char *cred_pubkey_pem, size_t cred_pubkey_len)
{
    ESP_RETURN_ON_FALSE(cred_pubkey_pem && cred_pubkey_len > 0, ESP_ERR_INVALID_ARG, k_tag, "invalid credential");

    uint8_t key_slot[ALIRO_KEY_SLOT_MAX_LEN];
    size_t key_slot_len = 0;
    size_t pem_len = cred_pubkey_len;
    ESP_RETURN_ON_ERROR(derive_key_slot(cred_pubkey_pem, &pem_len, key_slot, &key_slot_len, "removal"), k_tag,
                        "failed to derive key slot");

    credential_entry_t *slot = find_by_key_slot(key_slot, key_slot_len);
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(k_tag, "credential '%s' withdrawn", slot->label);
    free(slot->pubkey_pem);
    memset(slot, 0, sizeof(*slot));
    return ESP_OK;
}

esp_err_t access_control_refresh_key_slots(void)
{
    esp_err_t result = ESP_OK;

    for (size_t i = 0; i < CONFIG_ALIRO_MAX_CREDENTIALS; i++) {
        credential_entry_t *c = &s_credentials[i];
        if (!c->used) {
            continue;
        }

        size_t pem_len = c->pubkey_len;
        uint8_t key_slot[ALIRO_KEY_SLOT_MAX_LEN];
        size_t key_slot_len = 0;
        const esp_err_t err = derive_key_slot(c->pubkey_pem, &pem_len, key_slot, &key_slot_len, c->label);
        if (err != ESP_OK) {
            ESP_LOGE(k_tag, "could not re-derive the key slot for '%s'; it will not open the door", c->label);
            result = err;
            continue;
        }
        memcpy(c->key_slot, key_slot, key_slot_len);
        c->key_slot_len = key_slot_len;
        c->pubkey_len = pem_len;
    }
    return result;
}

size_t access_control_credential_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < CONFIG_ALIRO_MAX_CREDENTIALS; i++) {
        count += s_credentials[i].used ? 1 : 0;
    }
    return count;
}

static credential_entry_t *find_by_key_slot(const uint8_t *key_slot, size_t key_slot_len)
{
    for (size_t i = 0; i < CONFIG_ALIRO_MAX_CREDENTIALS; i++) {
        credential_entry_t *c = &s_credentials[i];
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

esp_err_t access_control_lock(void)
{
    ESP_RETURN_ON_FALSE(s_relock_timer, ESP_ERR_INVALID_STATE, k_tag, "access control not initialized");

    /* Stop the relock timer first: without this a Lock command issued during
     * an unlock window is undone when the timer fires and re-announces a state
     * that is already true. */
    (void)esp_timer_stop(s_relock_timer);
    ESP_RETURN_ON_ERROR(gpio_set_level(s_lock.gpio, LOCKED_LEVEL), k_tag, "lock GPIO set failed");

    if (!s_locked) {
        s_locked = true;
        ESP_LOGI(k_tag, "locked");
        notify(&(access_event_t){.type = ACCESS_EVENT_LOCK_STATE, .locked = true});
    }
    return ESP_OK;
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
        /*
         * A fast transaction never presents one, and that is by design: the
         * SDK matched a persistent key it stored during an earlier standard
         * transaction, which is the same device proving itself with material
         * only it could hold. The key-slot lookup is not called because there
         * is nothing to look up.
         *
         * Refusing here was wrong, and it refused the first real tap this
         * project ever completed:
         *
         *     session: Fast transaction matched persistent key index=0
         *     aliro/reader: transaction ok (fast) in 569 ms
         *     W aliro/access: denied: authenticated device presented no key slot
         *
         * What is genuinely lost is *which* credential it was -- the reader
         * knows the holder is legitimate, not which one. That is the trade
         * Aliro makes for a tap that feels instant, and the event says so
         * rather than naming a credential it cannot identify.
         */
        if (result->txn_type == ESP_ALIRO_TRANSACTION_FAST) {
            ESP_LOGI(k_tag, "granted: fast transaction against a stored key (%lld ms)",
                     (long long)result->duration_ms);
            tap_event(true, "granted", "fast transaction", result);
            ESP_ERROR_CHECK_WITHOUT_ABORT(access_control_unlock());
            return;
        }

        /* A standard transaction that presents no key slot is a different
         * thing: the reader cannot say who this is, and will not open. */
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
