/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Matter stack bring-up and the door lock endpoint.
 */

/*
 * Compiled to nothing without CONFIG_ALIRO_MATTER_ENABLE. The component builds
 * the same file list either way -- see this component's CMakeLists for why the
 * switch cannot live there -- so the guard lives here instead.
 */
#include <sdkconfig.h>

#if CONFIG_ALIRO_MATTER_ENABLE

#include "matter_lock.h"
#include "matter_lock_priv.h"

#include "matter_aliro_delegate.h"

#include "access_control.h"
#include "app_config.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <nvs.h>

#include <app/AppConfig.h>
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <lib/support/TypeTraits.h>
#include <platform/ConnectivityManager.h>
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
static bool s_hooks_set;
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
    /*
     * Keyed on the hooks being set, not on the stack running, and that
     * distinction matters: emberAfDoorLockClusterInitCallback fires from
     * inside esp_matter::start(), which is before this file gets to call
     * itself running. Testing s_running here made the credential store reload
     * its endpoint keys into a null hook table and drop every one of them --
     * silently, with the cluster still reporting them as enrolled.
     */
    return s_hooks_set ? &s_hooks : nullptr;
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

/* --- Releasing a reader identity nobody owns ----------------------------- */

/*
 * The cluster server accepts SetAliroReaderConfig only while the reader's
 * verification key attribute reads null, so a stored configuration is not just
 * a setting -- it is a lock on the one command a phone ecosystem needs in order
 * to set this device up. Nothing used to clear it: not removing the fabric that
 * sent it, not removing every fabric on the device. It survived to the point
 * where Apple, re-adding a lock it had provisioned once before, was refused
 * twice in the same setup flow:
 *
 *     E [SetAliroReaderConfig] Aliro reader verification key was not read or
 *                              is not null.
 *
 * and the Home Key row in "Unlock your door" stayed greyed out, because a home
 * key cannot be issued against a reader identity the controller could not set.
 */

/** @brief Drop the reader identity, whatever the reason. */
static esp_err_t release_reader_config(const char *why)
{
    if (!s_hooks_set || !s_hooks.clear_reader_identity) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(k_tag, "%s: releasing the provisioned reader identity so a controller can provision this reader again",
             why);
    const esp_err_t err = s_hooks.clear_reader_identity();
    if (err == ESP_OK) {
        matter_lock_set_reader_configured(false);
    }
    return err;
}

/**
 * @brief Drop the reader identity only if it is serving nobody.
 *
 * A reader identity exists for the Aliro credentials enrolled against it. When
 * the last one is gone -- because the fabric that owned them left, or because
 * the device was provisioned before any phone was enrolled -- it belongs to
 * nobody, and keeping it only blocks the next controller. Letting go costs
 * nothing: the controller provisions again.
 *
 * The converse is deliberate. While credentials are enrolled the identity stays
 * put even if a second controller asks for its own, because the first one to
 * configure an Aliro reader owns it and the phones already carrying keys for it
 * would stop working.
 */
static void release_orphaned_reader_config(const char *why)
{
    if (!s_reader_configured || matter_lock_store_aliro_credential_count() != 0) {
        return;
    }
    (void)release_reader_config(why);
}

/**
 * @brief Watches the fabric table so a departing controller takes its own
 *        users, credentials and reader identity with it.
 *
 * esp-matter runs a fabric delegate of its own, but the device event it posts
 * carries no fabric index -- and the index is the whole point here. The
 * delegate list is intrusive and holds both without complaint.
 */
class LockFabricDelegate : public chip::FabricTable::Delegate {
public:
    void OnFabricRemoved(const chip::FabricTable &fabricTable, chip::FabricIndex fabricIndex) override
    {
        matter_lock_store_forget_fabric(fabricIndex);
        release_orphaned_reader_config("a fabric was removed");
    }
};

static LockFabricDelegate s_fabric_delegate;

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
     * handled in matter_lock_store.cpp. AutoRelockTime is the one attribute a
     * controller can write that this device has to act on. */
    if (type == attribute::PRE_UPDATE && cluster_id == Id &&
        attribute_id == chip::app::Clusters::DoorLock::Attributes::AutoRelockTime::Id && val) {
        const uint32_t seconds = val->val.u32;
        if (seconds == 0) {
            /* Zero means "do not relock on your own". Nothing here can honour
             * that: the output is a strike or a relay, and leaving it energised
             * indefinitely is a door left open, not a setting. */
            ESP_LOGW(k_tag, "a controller asked for no auto-relock; keeping %u ms",
                     (unsigned)access_control_unlock_ms());
            return ESP_ERR_INVALID_ARG;
        }
        ESP_LOGI(k_tag, "a controller set AutoRelockTime to %u s", (unsigned)seconds);
        return access_control_set_unlock_ms(seconds * 1000);
    }
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
    s_hooks_set = true;

    load_reader_configured();

    /*
     * The device's own name, so a controller offers something better than
     * "Matter Accessory" when it is added. NodeLabel is the writable,
     * user-facing name in the Basic Information cluster; the vendor and
     * product names beside it are fixed at build time by the attestation
     * data, so this is the one a device gets to choose.
     *
     * A controller may overwrite it when the user renames the accessory in
     * their app, which is correct -- their name should win over ours.
     */
    node::config_t node_config;
    const app_config_t *app_cfg = app_config_get();
    if (app_cfg && app_cfg->device_name[0] != '\0') {
        strlcpy(node_config.root_node.basic_information.node_label, app_cfg->device_name,
                sizeof(node_config.root_node.basic_information.node_label));
        ESP_LOGI(k_tag, "advertising as '%s'", node_config.root_node.basic_information.node_label);
    }

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

    /*
     * Order is load-bearing, and it is not obvious. esp-matter refuses to add
     * the user feature unless a credential type is already present:
     *
     *     E esp_matter_feature: Should add at least one of PIN, RID, FGP and
     *                           FACE feature before add USR feature
     *
     * That refusal is a log line, not a return this code can trip over, so
     * with these three calls the other way round the lock came up looking
     * healthy while quietly missing USR -- and Aliro provisioning is defined
     * in terms of users, so it would have taken the whole feature down with
     * it. Credentials first, then users, then Aliro on top of both.
     *
     * The one change from esp-matter's defaults is that a remote unlock does
     * not demand a PIN: there is no keypad on this device, and a controller
     * that has been commissioned into the fabric has already proved more than
     * a PIN would.
     */
    cluster::door_lock::feature::pin_credential::config_t pin_config;
    pin_config.require_pin_for_remote_operation = false;
    if (cluster::door_lock::feature::pin_credential::add(lock_cluster, &pin_config) != ESP_OK) {
        ESP_LOGE(k_tag, "PIN credential feature refused");
    }

    cluster::door_lock::feature::credential_over_the_air_access::config_t cota_config;
    cota_config.require_pin_for_remote_operation = false;
    if (cluster::door_lock::feature::credential_over_the_air_access::add(lock_cluster, &cota_config) != ESP_OK) {
        ESP_LOGE(k_tag, "credential-over-the-air feature refused");
    }

    cluster::door_lock::feature::user::config_t user_config;
    if (cluster::door_lock::feature::user::add(lock_cluster, &user_config) != ESP_OK) {
        ESP_LOGE(k_tag, "user feature refused; Aliro provisioning will not work");
    }

    if (cluster::door_lock::feature::aliro_provisioning::add(lock_cluster) != ESP_OK) {
        ESP_LOGE(k_tag, "Aliro provisioning feature refused; no controller can provision this reader");
    }

    /*
     * AutoRelockTime, which esp-matter's own Aliro example creates and this did
     * not. The cluster server reads it on every remote unlock, and without it
     * every unlock logged
     *
     *     E chip[ZCL]: Failed to read DoorLock attribute: attribute=0x23,
     *                  status=0x86
     *
     * -- unsupported attribute. A lock that cannot say how long it stays open
     * is a lock that never relocks itself as far as a controller is concerned,
     * which is not what this one does: access_control has always driven the
     * output back after its configured time. Reporting that number is simply
     * the truth, and it is the one difference between this endpoint and the
     * reference one that Apple is known to issue a home key against.
     *
     * Seconds here, milliseconds in access_control, and the attribute is
     * writable -- a controller that changes it changes the GPIO timing too, in
     * on_attribute_update below. Two places holding the same number and only
     * one of them being obeyed is how this goes wrong later.
     */
    const uint32_t relock_s = (access_control_unlock_ms() + 999) / 1000;
    if (!cluster::door_lock::attribute::create_auto_relock_time(lock_cluster, relock_s)) {
        ESP_LOGE(k_tag, "could not publish AutoRelockTime");
    }

    s_endpoint_id = endpoint::get_id(endpoint);

    const esp_err_t err = esp_matter::start(on_matter_event);
    if (err != ESP_OK) {
        ESP_LOGE(k_tag, "Matter failed to start: %s", esp_err_to_name(err));
        return err;
    }

    s_running = true;

#if CHIP_CONFIG_PERSIST_SUBSCRIPTIONS
    ESP_LOGI(k_tag, "Matter subscriptions persist across restart");
#else
    ESP_LOGW(k_tag, "Matter subscription persistence is disabled; controllers may stay on Updating after restart");
#endif

    /*
     * Both under the stack lock: the fabric table belongs to the Matter task,
     * which is running by now, and the credential store this reads was filled
     * in from that task during start().
     */
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    const CHIP_ERROR fabric_err = chip::Server::GetInstance().GetFabricTable().AddFabricDelegate(&s_fabric_delegate);
    if (fabric_err != CHIP_NO_ERROR) {
        ESP_LOGE(k_tag, "could not watch the fabric table: %" CHIP_ERROR_FORMAT, fabric_err.Format());
    }
    /* Fabrics can also be removed while this device is powered off, and a
     * reader identity provisioned by an older firmware has no owner recorded at
     * all. Both land here. */
    release_orphaned_reader_config("at boot");
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    /*
     * The station is ours, not chip's.
     *
     * Left to itself chip's ConnectivityManager drives the station on its own
     * schedule -- and it has no credentials, because in this project they
     * arrive through the setup portal and live in our own configuration, not
     * in chip's. On hardware that produced two state machines fighting over
     * one radio:
     *
     *     chip[DL]: Attempting to connect WiFi station interface
     *     E chip[DL]: Failed to get configured network ... 0x0500300F
     *     E wifi:sta is connecting, cannot set config    <- ours, refused
     *     aliro/net: lost 'iPhone-R', retry 1/3
     *
     * The last two lines are the damage: chip's connect attempt was in flight
     * when net_manager called esp_wifi_set_config, so the credentials the user
     * typed were never applied at all, and the reader sat in a reconnect loop
     * against a network it had been told about but could not be configured for.
     *
     * ApplicationControlled is chip's own term for "the application owns the
     * station". It keeps reporting connectivity from the events it observes,
     * which is all the Matter side actually needs.
     */
    /* Under the stack lock: this runs on the main task, the Matter task is
     * already up by now, and SetWiFiStationMode reaches into the same state
     * that task drives. */
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(
        chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_ApplicationControlled);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    ESP_LOGI(k_tag, "Wi-Fi station left under application control; net_manager owns the connection");
#endif

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

extern "C" esp_err_t matter_lock_get_fabrics(matter_lock_fabric_t *out, size_t max, size_t *count)
{
    if (!out || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    for (const chip::FabricInfo &fabric : chip::Server::GetInstance().GetFabricTable()) {
        if (*count >= max) {
            break;
        }
        matter_lock_fabric_t &entry = out[*count];
        entry.index = fabric.GetFabricIndex();
        entry.vendor_id = chip::to_underlying(fabric.GetVendorId());
        entry.fabric_id = fabric.GetFabricId();
        entry.node_id = fabric.GetNodeId();

        entry.label[0] = '\0';
        const chip::CharSpan label = fabric.GetFabricLabel();
        if (!label.empty()) {
            const size_t len = label.size() < sizeof(entry.label) - 1 ? label.size() : sizeof(entry.label) - 1;
            memcpy(entry.label, label.data(), len);
            entry.label[len] = '\0';
        }
        (*count)++;
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    return ESP_OK;
}

extern "C" esp_err_t matter_lock_remove_fabric(uint8_t fabric_index)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fabric_index == chip::kUndefinedFabricIndex) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Done here under the stack lock rather than handed to the Matter task.
     * ScheduleWork only reports whether the work was *queued*, so the first
     * version of this answered "Removing that controller" to the browser
     * whatever happened next and swallowed the actual result -- which is
     * exactly what a button that appears to do nothing looks like.
     *
     * Delete() tears down that fabric's sessions and runs the fabric
     * delegates, ours included, so the credentials and the reader identity are
     * gone by the time this returns and the page's next poll shows the truth.
     */
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    const CHIP_ERROR err = chip::Server::GetInstance().GetFabricTable().Delete(fabric_index);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(k_tag, "could not remove fabric %u: %" CHIP_ERROR_FORMAT, (unsigned)fabric_index, err.Format());
        return ESP_FAIL;
    }
    ESP_LOGW(k_tag, "fabric %u removed from the web UI; that controller can no longer reach this lock",
             (unsigned)fabric_index);
    return ESP_OK;
}

extern "C" esp_err_t matter_lock_release_reader_config(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_reader_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Runs on the caller's task, unlike the two above: this touches the reader
     * and NVS rather than the cluster, and the web server wants to report
     * whether it worked rather than that it was scheduled. */
    return release_reader_config("asked to over the web UI");
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

#endif /* CONFIG_ALIRO_MATTER_ENABLE */
