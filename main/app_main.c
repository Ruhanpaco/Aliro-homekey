/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Aliro HomeKey - wiring only.
 *
 * Load the stored configuration, apply it to the lock and the NFC frontend,
 * start the reader, then bring up the network and the configuration UI.
 */

#include "access_control.h"
#include "aliro_reader.h"
#include "app_config.h"
#include "mqtt_manager.h"
#include "net_manager.h"
#include "nfc_transport.h"
#include "serial_console.h"
#include "web_server.h"

#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include <stdlib.h>
#include <string.h>

/* Embedded by main/CMakeLists.txt from main/certs/ (TEXT: NUL-terminated). */
extern const char reader_pubkey_pem_start[] asm("_binary_reader_pubkey_pem_start");
extern const char reader_privkey_pem_start[] asm("_binary_reader_privkey_pem_start");
extern const char credential_pubkey_pem_start[] asm("_binary_credential_pubkey_pem_start");
extern const char credential_pubkey_pem_end[] asm("_binary_credential_pubkey_pem_end");

static const char *const k_tag = "aliro/app";

static const nfc_transport_t *s_transport;

/*
 * Identity actually in use. A board provisioned by the browser flasher in
 * site/ has its own key pair in NVS; anything else falls back to the
 * development identity compiled into this image. Never freed: the SDK holds
 * these for as long as the reader exists.
 */
static struct {
    const char *reader_pub;
    const char *reader_priv;
    const char *credential_pub;
    size_t credential_pub_len;
    bool provisioned;
} s_identity;

static void load_identity(void)
{
    char *reader_pub = app_config_load_pem("rdr_pub");
    char *reader_priv = app_config_load_pem("rdr_priv");
    char *credential_pub = app_config_load_pem("cred_pub");

    /* Both halves or neither: a public key from NVS paired with the built-in
     * private key is an identity that cannot complete a transaction, and it
     * would fail in a way that looks like a protocol bug. */
    if (reader_pub && reader_priv) {
        s_identity.reader_pub = reader_pub;
        s_identity.reader_priv = reader_priv;
        s_identity.provisioned = true;
    } else {
        if (reader_pub || reader_priv) {
            ESP_LOGW(k_tag, "NVS holds only half a reader key pair; ignoring it");
        }
        free(reader_pub);
        free(reader_priv);
        s_identity.reader_pub = reader_pubkey_pem_start;
        s_identity.reader_priv = reader_privkey_pem_start;
    }

    if (credential_pub) {
        s_identity.credential_pub = credential_pub;
        s_identity.credential_pub_len = strlen(credential_pub) + 1;
    } else {
        s_identity.credential_pub = credential_pubkey_pem_start;
        s_identity.credential_pub_len = (size_t)(credential_pubkey_pem_end - credential_pubkey_pem_start);
    }

    ESP_LOGI(k_tag, "reader identity: %s", s_identity.provisioned ? "provisioned (NVS)" : "development (built in)");
}

static const char *transport_name(void)
{
    return s_transport ? s_transport->name : "none";
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t start_reader(const app_config_t *cfg)
{
    s_transport = nfc_transport_from_config(&cfg->nfc);

    aliro_reader_config_t reader_cfg = {
        .reader_pubkey_pem = s_identity.reader_pub,
        .reader_privkey_pem = s_identity.reader_priv,
        .transport = s_transport,
        .lookup_credential = access_control_lookup_credential,
        .on_result = access_control_on_reader_result,
        .user_ctx = NULL,
        .fast_transaction_slots = CONFIG_ALIRO_READER_FAST_TRANSACTION_SLOTS,
    };
    ESP_RETURN_ON_ERROR(app_config_parse_group_id(cfg->group_id_hex, reader_cfg.group_identifier,
                                                  sizeof(reader_cfg.group_identifier)),
                        k_tag, "reader group identifier '%s' is not valid hex", cfg->group_id_hex);

    ESP_RETURN_ON_ERROR(aliro_reader_start(&reader_cfg), k_tag, "reader failed to start");
    ESP_ERROR_CHECK_WITHOUT_ABORT(aliro_reader_log_identity());
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(k_tag, "Aliro HomeKey starting");

    /* The Aliro SDK persists its reader group sub-identifier and fast
     * transaction keys in NVS, and the configuration lives there too. */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());

    const app_config_t *cfg = app_config_get();
    ESP_LOGI(k_tag, "device '%s'", cfg->device_name);

    /* After app_config_init, so NVS is known good before it is read again. */
    load_identity();

    /*
     * Past this point nothing aborts the boot. A reader that cannot read is
     * useless, but a reader in a boot loop cannot be reconfigured to fix
     * itself -- and the serial console and web UI below are the only way to
     * fix anything. Every failure here is logged loudly and reported through
     * `status`, and the device still comes up.
     */
    ESP_ERROR_CHECK_WITHOUT_ABORT(access_control_init(&cfg->lock));

    /* Before the credential store, because deriving a key slot is an SDK call
     * and the SDK reports "cannot be parsed" when it is simply not up yet. */
    if (aliro_reader_sdk_init(CONFIG_ALIRO_READER_FAST_TRANSACTION_SLOTS) != ESP_OK) {
        ESP_LOGE(k_tag, "Aliro SDK did not initialize; the reader is disabled this boot");
    } else {
        if (access_control_add_credential(s_identity.credential_pub, s_identity.credential_pub_len,
                                          s_identity.provisioned ? "provisioned" : "dev-credential") != ESP_OK) {
            ESP_LOGE(k_tag, "development credential rejected; the reader will refuse every tap");
        }
        if (start_reader(cfg) != ESP_OK) {
            ESP_LOGE(k_tag, "reader not running; configuration UI and console are still available");
        }
    }

    /* Networking last: a reader must keep working on a door whose Wi-Fi is
     * down, so nothing above this line depends on it. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(net_manager_start(&cfg->net));

    ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_start(&cfg->mqtt, cfg->device_name));

    const web_server_hooks_t hooks = {
        .credential_count = access_control_credential_count,
        .transport_name = transport_name,
        .lock_is_locked = access_control_is_locked,
        .mqtt_enabled = mqtt_manager_is_enabled,
        .mqtt_connected = mqtt_manager_is_connected,
        .unlock = access_control_unlock,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(web_server_start(&hooks));

    /* Last, so its prompt lands after the boot log rather than in the middle
     * of it. Works with no network at all, which is the state a board is in
     * the first time it is powered up. */
    const serial_console_hooks_t console_hooks = {
        .credential_count = access_control_credential_count,
        .transport_name = transport_name,
        .mqtt_enabled = mqtt_manager_is_enabled,
        .mqtt_connected = mqtt_manager_is_connected,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(serial_console_start(&console_hooks));

    ESP_LOGI(k_tag, "ready: %u credential(s), transport '%s'", (unsigned)access_control_credential_count(),
             transport_name());

    /* How close the boot came to overflowing this task. A stack overflow here
     * is a reboot loop with a corrupted backtrace, so the margin is worth
     * printing rather than guessing at. */
    ESP_LOGI(k_tag, "main task stack headroom: %u bytes of %d",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL)), CONFIG_ESP_MAIN_TASK_STACK_SIZE);
}
