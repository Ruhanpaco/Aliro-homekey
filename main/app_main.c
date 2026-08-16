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
#include "matter_lock.h"
#include "mqtt_manager.h"
#include "net_manager.h"
#include "nfc_transport.h"
#include "serial_console.h"
#include "web_server.h"

#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include <stdio.h>
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
    bool owned; /*!< the key pair is on the heap and this file must free it */
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
        s_identity.owned = true;
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

/* --- Matter -------------------------------------------------------------- */

/*
 * The Matter side owns none of this; it asks through these. Everything below
 * is the same wiring the rest of app_main does, just triggered by a controller
 * instead of by boot.
 */

/**
 * @brief Adopt the reader identity a Matter controller just sent us.
 *
 * Persisted first, then applied, in that order on purpose: a device that
 * accepted SetAliroReaderConfig and then lost power must come back with the
 * identity the controller believes it has, or every phone enrolled against it
 * is silently dead.
 */
static esp_err_t reader_set_identity(const char *pubkey_pem, const char *privkey_pem, const uint8_t *group_id,
                                     size_t group_id_len)
{
    ESP_RETURN_ON_FALSE(pubkey_pem && privkey_pem && group_id && group_id_len == ALIRO_GROUP_IDENTIFIER_LEN,
                        ESP_ERR_INVALID_ARG, k_tag, "incomplete reader identity");

    char *pub = strdup(pubkey_pem);
    char *priv = strdup(privkey_pem);
    if (!pub || !priv) {
        free(pub);
        free(priv);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = app_config_save_pem("rdr_pub", pub);
    if (err == ESP_OK) {
        err = app_config_save_pem("rdr_priv", priv);
    }
    if (err == ESP_OK) {
        app_config_t cfg = *app_config_get();
        for (size_t i = 0; i < group_id_len; i++) {
            snprintf(cfg.group_id_hex + i * 2, 3, "%02X", group_id[i]);
        }
        char reason[96] = {0};
        err = app_config_save(&cfg, reason, sizeof(reason));
        if (err != ESP_OK) {
            ESP_LOGE(k_tag, "could not store the reader group identifier: %s", reason);
        }
    }
    if (err != ESP_OK) {
        free(pub);
        free(priv);
        return err;
    }

    /* Swap the live reader. Stopping first is required: the SDK allows one
     * reader at a time, and creating a second returns ESP_ERR_INVALID_STATE. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(aliro_reader_stop());

    if (s_identity.owned) {
        free((void *)s_identity.reader_pub);
        free((void *)s_identity.reader_priv);
    }
    s_identity.reader_pub = pub;
    s_identity.reader_priv = priv;
    s_identity.provisioned = true;
    s_identity.owned = true;

    err = start_reader(app_config_get());
    if (err != ESP_OK) {
        ESP_LOGE(k_tag, "the reader did not come back up with the provisioned identity");
        return err;
    }

    /* Key slots are derived against the reader's group sub-identifier, so
     * everything already enrolled has to be recomputed against the new one. */
    (void)access_control_refresh_key_slots();

    /* And the stored fast-transaction keys belong to the identity that just
     * went away. */
    aliro_reader_forget_fast_transactions();
    ESP_LOGI(k_tag, "reader now running the identity provisioned over Matter");
    return ESP_OK;
}

static esp_err_t reader_clear_identity(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_config_erase_pem("rdr_pub"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_config_erase_pem("rdr_priv"));
    aliro_reader_forget_fast_transactions();

    /* The reader keeps running on the identity it has until the next restart.
     * Tearing it down here would leave a door that cannot be opened by anyone,
     * including whoever is standing in front of it, and the controller has no
     * way to know that happened. */
    ESP_LOGW(k_tag, "reader identity erased; it takes effect at the next restart");
    return ESP_OK;
}

static void on_access_event_for_matter(const access_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->type == ACCESS_EVENT_LOCK_STATE) {
        matter_lock_report_lock_state(event->locked);
    }
}

static void start_matter(void)
{
    if (!matter_lock_available()) {
        return;
    }

    const matter_lock_hooks_t hooks = {
        .set_reader_identity = reader_set_identity,
        .clear_reader_identity = reader_clear_identity,
        .add_credential = access_control_add_credential,
        .remove_credential = access_control_remove_credential,
        .unlock = access_control_unlock,
        .lock = access_control_lock,
        .is_locked = access_control_is_locked,
    };

    if (matter_lock_start(&hooks) != ESP_OK) {
        ESP_LOGE(k_tag, "Matter did not start; the reader and the web UI are unaffected");
        return;
    }

    /* So a tap, or the relock timer behind it, moves LockState in whatever app
     * commissioned this device. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(access_control_add_observer(on_access_event_for_matter, NULL));
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

    if (aliro_reader_sdk_init(CONFIG_ALIRO_READER_FAST_TRANSACTION_SLOTS) != ESP_OK) {
        ESP_LOGE(k_tag, "Aliro SDK did not initialize; the reader is disabled this boot");
    } else if (start_reader(cfg) != ESP_OK) {
        ESP_LOGE(k_tag, "reader not running; configuration UI and console are still available");
    } else {
        /*
         * Credentials last, and specifically after the reader exists. An Aliro
         * key slot is derived from the credential public key together with the
         * reader's group sub-identifier, and that sub-identifier is only
         * generated by esp_aliro_reader_create(). Deriving before then fails
         * with the same ESP_FAIL the SDK uses for a malformed key, which cost
         * this project two hardware debugging sessions -- the giveaway was
         * "persistent_storage: Failed to get bytes" logged just above.
         */
        if (access_control_add_credential(s_identity.credential_pub, s_identity.credential_pub_len,
                                          s_identity.provisioned ? "provisioned" : "dev-credential") != ESP_OK) {
            ESP_LOGE(k_tag, "credential rejected; the reader will refuse every tap");
        }
    }

    /*
     * A board with no Wi-Fi credentials has exactly one job: raise its setup
     * access point and serve the page that takes credentials. Matter is not
     * merely useless there, it is what makes the job impossible -- on hardware
     * the HTTP server could not even start:
     *
     *     E aliro/web: httpd_start failed: ESP_ERR_HTTPD_TASK
     *
     * That is xTaskCreate failing for want of heap. This chip has about 45 KB
     * of plain DRAM, and in setup mode it is being asked to hold chip, BLE,
     * mDNS and CASE alongside a SoftAP, a DHCP server and a captive DNS
     * responder. The same firmware starts the web server perfectly once it has
     * credentials and drops the AP, which is what pins the cause on setup mode
     * rather than on Matter.
     *
     * So Matter waits for a network. Nothing is lost: a controller cannot
     * commission a device it has no route to, and the portal is how this
     * project gets credentials onto the board. Matter starts on the next boot,
     * once there is a network to be discovered on.
     *
     * When it does start, it goes first, and only because of who owns the
     * Wi-Fi driver: chip's ESP32 platform layer initializes esp_netif and
     * esp_wifi from inside InitChipStack, and doing that twice fails.
     * net_manager detects a driver that is already up and joins it.
     */
    const bool have_credentials = cfg->net.ssid[0] != '\0';
    if (have_credentials) {
        start_matter();
    } else if (matter_lock_available()) {
        ESP_LOGW(k_tag, "no Wi-Fi credentials: starting the setup portal only, Matter waits for the next boot");
    }

    /* Networking: a reader must keep working on a door whose Wi-Fi is down, so
     * nothing above this line depends on it. */
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

    /*
     * Getting this far is the definition of a good image. An app installed
     * over the air boots once as PENDING_VERIFY, and only this call makes it
     * permanent -- so a build that panics on the way up, or that cannot bring
     * up the console and the configuration UI, is reverted by the bootloader
     * at the next reset instead of needing a cable and an open enclosure.
     */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGW(k_tag, "new image on '%s' accepted; rollback cancelled", running->label);
        } else {
            ESP_LOGE(k_tag, "could not confirm this image; it will roll back on the next reset");
        }
    }
}
