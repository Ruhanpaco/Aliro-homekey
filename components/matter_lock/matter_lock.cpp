/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Matter stack bring-up and the door lock endpoint.
 */

#include "matter_lock.h"
#include "matter_lock_priv.h"

#include "matter_aliro_delegate.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <nvs.h>

#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/PlatformManager.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <string.h>

using namespace esp_matter;
using namespace esp_matter::endpoint;
using chip::app::Clusters::DoorLock::DlLockState;
using chip::app::Clusters::DoorLock::Id;

static const char *const k_tag = "aliro/matter";

static const char *const k_nvs_namespace = "mtr_lock";
static const char *const k_nvs_reader_configured = "aliro_cfg";

/** @brief How long a manually reopened commissioning window stays open. */
static constexpr uint16_t k_commissioning_window_s = 300;

static matter_lock_hooks_t s_hooks;
static uint16_t s_endpoint_id;
static bool s_running;
static bool s_reader_configured;

/* Captured once the stack is up; served to the web UI and the console. */
static char s_qr[96];
static char s_manual[32];
static char s_qr_url[192];

/* --- Accessors used by the other two translation units ------------------- */

extern "C" const matter_lock_hooks_t *matter_lock_hooks(void)
{
    return s_running ? &s_hooks : nullptr;
}

extern "C" uint16_t matter_lock_endpoint(void)
{
    return s_endpoint_id;
}

extern "C" void matter_lock_set_reader_configured(bool configured)
{
    s_reader_configured = configured;

    nvs_handle_t handle;
    if (nvs_open(k_nvs_namespace, NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_set_u8(handle, k_nvs_reader_configured, configured ? 1 : 0);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
}

static void load_reader_configured(void)
{
    nvs_handle_t handle;
    if (nvs_open(k_nvs_namespace, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t value = 0;
    if (nvs_get_u8(handle, k_nvs_reader_configured, &value) == ESP_OK) {
        s_reader_configured = value != 0;
    }
    nvs_close(handle);
}

/* --- Stack callbacks ----------------------------------------------------- */

static void on_matter_event(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(k_tag, "commissioned; this device is now in %u fabric(s)",
                 (unsigned)chip::Server::GetInstance().GetFabricTable().FabricCount());
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(k_tag, "a controller is commissioning this device");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(k_tag, "commissioning failed: the fail-safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        /*
         * The last controller went away. Reopen commissioning rather than
         * leaving a lock that can only be recovered by reflashing -- there is
         * no button on most of these boards, and the web UI is the only other
         * way in.
         */
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            ESP_LOGW(k_tag, "last fabric removed; reopening commissioning");
            chip::CommissioningWindowManager &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (!mgr.IsCommissioningWindowOpen()) {
                const CHIP_ERROR err = mgr.OpenBasicCommissioningWindow(
                    chip::System::Clock::Seconds16(k_commissioning_window_s),
                    chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(k_tag, "could not reopen commissioning: %" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
        break;
    }

    default:
        break;
    }
}

static esp_err_t on_identify(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                             uint8_t effect_variant, void *priv_data)
{
    /* "Which lock am I looking at?" There is no indicator to flash on a bare
     * DevKit, so say it in the log where the answer is at least visible. */
    ESP_LOGI(k_tag, "identify: endpoint %u, effect %u", (unsigned)endpoint_id, (unsigned)effect_id);
    return ESP_OK;
}

static esp_err_t on_attribute_update(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                     uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    /* Lock and unlock arrive as commands, not attribute writes, and are
     * handled in matter_lock_store.cpp. Nothing else here needs driving. */
    return ESP_OK;
}

/* --- Onboarding payload -------------------------------------------------- */

static void capture_onboarding_codes(void)
{
    /* Commissioning happens over BLE on a Wi-Fi ESP32: the commissioner has no
     * other way to reach a device that is not on the network yet. */
    const chip::RendezvousInformationFlags flags(chip::RendezvousInformationFlag::kBLE);

    chip::MutableCharSpan qr(s_qr, sizeof(s_qr) - 1);
    if (GetQRCode(qr, flags) == CHIP_NO_ERROR) {
        s_qr[qr.size() < sizeof(s_qr) ? qr.size() : sizeof(s_qr) - 1] = '\0';
        (void)GetQRCodeUrl(s_qr_url, sizeof(s_qr_url), chip::CharSpan(s_qr, strlen(s_qr)));
    } else {
        ESP_LOGE(k_tag, "could not build the onboarding payload");
    }

    chip::MutableCharSpan manual(s_manual, sizeof(s_manual) - 1);
    if (GetManualPairingCode(manual, flags) == CHIP_NO_ERROR) {
        s_manual[manual.size() < sizeof(s_manual) ? manual.size() : sizeof(s_manual) - 1] = '\0';
    }

    ESP_LOGI(k_tag, "commissioning payload: %s", s_qr);
    ESP_LOGI(k_tag, "manual pairing code:   %s", s_manual);
    ESP_LOGI(k_tag, "scannable code:        %s", s_qr_url);
}

/* --- Public API ---------------------------------------------------------- */

extern "C" esp_err_t matter_lock_start(const matter_lock_hooks_t *hooks)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hooks) {
        return ESP_ERR_INVALID_ARG;
    }
    s_hooks = *hooks;

    load_reader_configured();

    node::config_t node_config;
    node_t *node = node::create(&node_config, on_attribute_update, on_identify);
    if (!node) {
        ESP_LOGE(k_tag, "could not create the Matter node");
        return ESP_FAIL;
    }

    /*
     * Door Lock is not a costume. SetAliroReaderConfig and the Aliro
     * credential types live in this cluster and nowhere else, so a device that
     * wants a phone ecosystem to provision it has to be a door lock -- whether
     * the GPIO on the other end drives a strike, a relay or an LED.
     */
    door_lock::config_t lock_config;
    lock_config.door_lock.lock_state = chip::to_underlying(DlLockState::kLocked);
    lock_config.door_lock.delegate = &AliroReaderDelegate::Instance();

    endpoint_t *endpoint = door_lock::create(node, &lock_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!endpoint) {
        ESP_LOGE(k_tag, "could not create the door lock endpoint");
        return ESP_FAIL;
    }

    cluster_t *lock_cluster = cluster::get(endpoint, Id);

    /* Aliro provisioning depends on the user feature: an endpoint key is
     * always a credential belonging to a user. */
    cluster::door_lock::feature::user::config_t user_config;
    cluster::door_lock::feature::user::add(lock_cluster, &user_config);

    /*
     * PIN and credential-over-the-air are what every shipping lock advertises,
     * and controllers are noticeably happier with them present. The one change
     * from the defaults is that a remote unlock does not demand a PIN -- there
     * is no keypad on this device, and a controller that has been commissioned
     * into the fabric has already proved more than a PIN would.
     */
    cluster::door_lock::feature::pin_credential::config_t pin_config;
    pin_config.require_pin_for_remote_operation = false;
    cluster::door_lock::feature::pin_credential::add(lock_cluster, &pin_config);

    cluster::door_lock::feature::credential_over_the_air_access::config_t cota_config;
    cota_config.require_pin_for_remote_operation = false;
    cluster::door_lock::feature::credential_over_the_air_access::add(lock_cluster, &cota_config);

    cluster::door_lock::feature::aliro_provisioning::add(lock_cluster);

    s_endpoint_id = endpoint::get_id(endpoint);

    const esp_err_t err = esp_matter::start(on_matter_event);
    if (err != ESP_OK) {
        ESP_LOGE(k_tag, "Matter failed to start: %s", esp_err_to_name(err));
        return err;
    }

    s_running = true;
    capture_onboarding_codes();

    ESP_LOGI(k_tag, "door lock on endpoint %u, %u fabric(s)", (unsigned)s_endpoint_id,
             (unsigned)chip::Server::GetInstance().GetFabricTable().FabricCount());
    return ESP_OK;
}

extern "C" bool matter_lock_available(void)
{
    return true;
}

extern "C" bool matter_lock_running(void)
{
    return s_running;
}

extern "C" size_t matter_lock_fabric_count(void)
{
    return s_running ? chip::Server::GetInstance().GetFabricTable().FabricCount() : 0;
}

extern "C" bool matter_lock_reader_configured(void)
{
    return s_reader_configured;
}

extern "C" const char *matter_lock_qr_payload(void)
{
    return s_qr;
}

extern "C" const char *matter_lock_manual_code(void)
{
    return s_manual;
}

extern "C" const char *matter_lock_qr_url(void)
{
    return s_qr_url;
}

static void open_commissioning_window(intptr_t arg)
{
    chip::CommissioningWindowManager &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (mgr.IsCommissioningWindowOpen()) {
        return;
    }
    const CHIP_ERROR err =
        mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(k_commissioning_window_s),
                                         chip::CommissioningWindowAdvertisement::kAllSupported);
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(k_tag, "commissioning open for %u seconds", (unsigned)k_commissioning_window_s);
    } else {
        ESP_LOGE(k_tag, "could not open commissioning: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

extern "C" esp_err_t matter_lock_open_commissioning_window(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Everything inside the stack has to run on the Matter task. */
    return chip::DeviceLayer::PlatformMgr().ScheduleWork(open_commissioning_window, 0) == CHIP_NO_ERROR ? ESP_OK
                                                                                                       : ESP_FAIL;
}

static void report_lock_state(intptr_t locked)
{
    DoorLockServer::Instance().SetLockState(s_endpoint_id, locked ? DlLockState::kLocked : DlLockState::kUnlocked);
}

extern "C" void matter_lock_report_lock_state(bool locked)
{
    if (!s_running) {
        return;
    }
    /* Called from the reader task the instant a tap is granted, so it must not
     * touch the cluster directly. */
    (void)chip::DeviceLayer::PlatformMgr().ScheduleWork(report_lock_state, locked ? 1 : 0);
}
