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

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include <string.h>

/* Embedded by main/CMakeLists.txt from main/certs/ (TEXT: NUL-terminated). */
extern const char reader_pubkey_pem_start[] asm("_binary_reader_pubkey_pem_start");
extern const char reader_privkey_pem_start[] asm("_binary_reader_privkey_pem_start");
extern const char credential_pubkey_pem_start[] asm("_binary_credential_pubkey_pem_start");
extern const char credential_pubkey_pem_end[] asm("_binary_credential_pubkey_pem_end");

static const char *const k_tag = "aliro/app";

static const nfc_transport_t *s_transport;

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

static void start_reader(const app_config_t *cfg)
{
    s_transport = nfc_transport_from_config(&cfg->nfc);

    aliro_reader_config_t reader_cfg = {
        .reader_pubkey_pem = reader_pubkey_pem_start,
        .reader_privkey_pem = reader_privkey_pem_start,
        .transport = s_transport,
        .lookup_credential = access_control_lookup_credential,
        .on_result = access_control_on_reader_result,
        .user_ctx = NULL,
        .fast_transaction_slots = CONFIG_ALIRO_READER_FAST_TRANSACTION_SLOTS,
    };
    ESP_ERROR_CHECK(app_config_parse_group_id(cfg->group_id_hex, reader_cfg.group_identifier,
                                              sizeof(reader_cfg.group_identifier)));

    ESP_ERROR_CHECK(aliro_reader_start(&reader_cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(aliro_reader_log_identity());
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

    ESP_ERROR_CHECK(access_control_init(&cfg->lock));

    /* Before the credential store, because deriving a key slot is an SDK call
     * and the SDK reports "cannot be parsed" when it is simply not up yet. */
    ESP_ERROR_CHECK(aliro_reader_sdk_init(CONFIG_ALIRO_READER_FAST_TRANSACTION_SLOTS));

    /* Not fatal. A credential that will not parse means nobody can open this
     * door, which is bad -- but a reader stuck in a boot loop cannot even be
     * reconfigured to fix it, which is worse. */
    if (access_control_add_credential(credential_pubkey_pem_start,
                                      (size_t)(credential_pubkey_pem_end - credential_pubkey_pem_start),
                                      "dev-credential") != ESP_OK) {
        ESP_LOGE(k_tag, "development credential rejected; the reader will refuse every tap");
    }

    start_reader(cfg);

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
}
