/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Door Lock cluster's user and credential database.
 *
 * The cluster server keeps none of this itself: it validates and then calls
 * out to the application through the emberAfPluginDoorLock* functions below,
 * and the application is expected to own the storage and survive a reboot with
 * it. Everything here exists to serve one flow --
 *
 *     SetUser(userIndex=1, ...)                     "someone lives here"
 *     SetCredential(AliroEvictableEndpointKey, 1)   "and this is their phone"
 *
 * -- because that second command is how a phone's Aliro endpoint key reaches
 * the reader. When one arrives it is converted to PEM and handed to
 * access_control, which is what actually decides whether a tap opens the door.
 * PIN credentials are stored so the cluster behaves, but nothing here opens a
 * door for one; this is a reader, not a keypad.
 */

#include "matter_aliro_delegate.h"
#include "matter_lock_priv.h"

#include "aliro_reader.h"

#include <esp_log.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/TypeTraits.h>
#include <nvs.h>

#include <stdio.h>
#include <string.h>

using namespace chip;
using namespace chip::app::Clusters::DoorLock;

static const char *const k_tag = "aliro/matter-db";

static const char *const k_nvs_namespace = "mtr_lock";
static const char *const k_nvs_users = "users";
static const char *const k_nvs_credentials = "creds";

static constexpr uint16_t kMaxUsers = 5;
static constexpr uint8_t kMaxCredentialsPerUser = 3;
static constexpr uint16_t kMaxPinCredentials = 5;
static constexpr size_t kMaxCredentialSize = 65; /* an uncompressed P-256 point */

/* --- Storage ------------------------------------------------------------- */

/*
 * Plain data, no spans: these structs go into NVS as blobs, and a ByteSpan
 * written to flash is a pointer written to flash. The cluster's own structs
 * are rebuilt around this storage on the way out.
 */
struct StoredCredential {
    uint8_t status; /* DlCredentialStatus */
    uint8_t type;   /* CredentialTypeEnum */
    uint8_t createdBy;
    uint8_t modifiedBy;
    uint8_t len;
    uint8_t data[kMaxCredentialSize];
};

struct StoredUserCredential {
    uint8_t type;
    uint16_t index;
};

struct StoredUser {
    uint8_t status; /* UserStatusEnum */
    uint8_t type;   /* UserTypeEnum */
    uint8_t rule;   /* CredentialRuleEnum */
    uint8_t nameLen;
    uint8_t createdBy;
    uint8_t modifiedBy;
    uint8_t credentialCount;
    uint8_t reserved;
    uint32_t uniqueId;
    char name[DOOR_LOCK_MAX_USER_NAME_SIZE];
    StoredUserCredential credentials[kMaxCredentialsPerUser];
};

/* One blob each, so a write is one NVS transaction rather than 26. */
struct CredentialDb {
    StoredCredential programmingPin;
    StoredCredential pin[kMaxPinCredentials];
    StoredCredential issuer[kAliroCredentialIssuerKeysSupported];
    StoredCredential evictable[kAliroEndpointKeysSupported];
    StoredCredential nonEvictable[kAliroEndpointKeysSupported];
};

static StoredUser s_users[kMaxUsers];
static CredentialDb s_credentials;

/* The cluster hands out a Span over these, so they must outlive the call that
 * builds them. Rebuilt from s_users whenever a user changes. */
static CredentialStruct s_user_credentials[kMaxUsers][kMaxCredentialsPerUser];

static bool s_loaded;

/* --- NVS ----------------------------------------------------------------- */

static void persist(const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(k_nvs_namespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, key, data, len);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        /* Not fatal: the lock keeps working this boot, it just forgets after a
         * power cut. Worth shouting about, because that is a confusing way to
         * lose a phone that was working yesterday. */
        ESP_LOGE(k_tag, "could not persist '%s': %s", key, esp_err_to_name(err));
    }
}

static void load_blob(const char *key, void *data, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open(k_nvs_namespace, NVS_READONLY, &handle) != ESP_OK) {
        return; /* first boot: the namespace does not exist yet */
    }

    size_t stored = 0;
    if (nvs_get_blob(handle, key, nullptr, &stored) == ESP_OK && stored == len) {
        (void)nvs_get_blob(handle, key, data, &stored);
    } else if (stored != 0) {
        /* A firmware update that changes the table sizes changes the blob
         * size. Starting empty is better than reading a table apart. */
        ESP_LOGW(k_tag, "stored '%s' is %u bytes, expected %u; ignoring it", key, (unsigned)stored, (unsigned)len);
    }
    nvs_close(handle);
}

/* --- Slot lookup --------------------------------------------------------- */

/*
 * Matter numbers credentials per type, one-based, except the programming PIN
 * which is the sole occupant of index 0.
 */
static StoredCredential *credential_slot(CredentialTypeEnum type, uint16_t index)
{
    switch (type) {
    case CredentialTypeEnum::kProgrammingPIN:
        return index == 0 ? &s_credentials.programmingPin : nullptr;
    case CredentialTypeEnum::kPin:
        return (index >= 1 && index <= kMaxPinCredentials) ? &s_credentials.pin[index - 1] : nullptr;
    case CredentialTypeEnum::kAliroCredentialIssuerKey:
        return (index >= 1 && index <= kAliroCredentialIssuerKeysSupported) ? &s_credentials.issuer[index - 1] : nullptr;
    case CredentialTypeEnum::kAliroEvictableEndpointKey:
        return (index >= 1 && index <= kAliroEndpointKeysSupported) ? &s_credentials.evictable[index - 1] : nullptr;
    case CredentialTypeEnum::kAliroNonEvictableEndpointKey:
        return (index >= 1 && index <= kAliroEndpointKeysSupported) ? &s_credentials.nonEvictable[index - 1] : nullptr;
    default:
        return nullptr;
    }
}

static bool is_aliro_endpoint_key(CredentialTypeEnum type)
{
    return type == CredentialTypeEnum::kAliroEvictableEndpointKey ||
           type == CredentialTypeEnum::kAliroNonEvictableEndpointKey;
}

/* --- Handing endpoint keys to the reader --------------------------------- */

static void credential_label(CredentialTypeEnum type, uint16_t index, char *out, size_t out_len)
{
    snprintf(out, out_len, "matter %s%u", type == CredentialTypeEnum::kAliroEvictableEndpointKey ? "ev" : "nev",
             (unsigned)index);
}

/**
 * @brief Mirror one Aliro endpoint key into the reader's credential store.
 *
 * @param add true to enroll, false to withdraw.
 */
static void apply_endpoint_key(CredentialTypeEnum type, uint16_t index, const uint8_t *raw, size_t raw_len, bool add)
{
    const matter_lock_hooks_t *hooks = matter_lock_hooks();
    if (!hooks || raw_len == 0) {
        return;
    }

    char pem[256];
    size_t pem_len = sizeof(pem) - 1;
    const esp_err_t err = aliro_reader_pubkey_pem_from_raw(raw, raw_len, pem, &pem_len);
    if (err != ESP_OK) {
        ESP_LOGE(k_tag, "endpoint key %u is not a key we can use: %s", (unsigned)index, esp_err_to_name(err));
        return;
    }
    pem[pem_len] = '\0';

    /* The reader wants the terminator counted, the same convention the
     * compiled-in development credential uses. */
    const size_t pem_size = pem_len + 1;

    if (add) {
        char label[24];
        credential_label(type, index, label, sizeof(label));
        if (hooks->add_credential && hooks->add_credential(pem, pem_size, label) == ESP_OK) {
            ESP_LOGI(k_tag, "enrolled '%s'", label);
        } else {
            ESP_LOGE(k_tag, "could not enroll endpoint key %u", (unsigned)index);
        }
    } else if (hooks->remove_credential) {
        (void)hooks->remove_credential(pem, pem_size);
        ESP_LOGI(k_tag, "withdrew endpoint key %u", (unsigned)index);
    }
}

/* --- Load ---------------------------------------------------------------- */

static void refresh_user_credentials(uint16_t userIndex)
{
    const StoredUser &user = s_users[userIndex];
    for (uint8_t i = 0; i < user.credentialCount && i < kMaxCredentialsPerUser; i++) {
        s_user_credentials[userIndex][i].credentialType = static_cast<CredentialTypeEnum>(user.credentials[i].type);
        s_user_credentials[userIndex][i].credentialIndex = user.credentials[i].index;
    }
}

/**
 * @brief Read the database back and re-enroll every key it holds.
 *
 * access_control keeps its credentials in RAM, so without this a reboot would
 * silently stop opening the door for every phone a controller ever added --
 * with the cluster still cheerfully reporting them as enrolled.
 */
static void store_load(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;

    load_blob(k_nvs_users, s_users, sizeof(s_users));
    load_blob(k_nvs_credentials, &s_credentials, sizeof(s_credentials));

    for (uint16_t i = 0; i < kMaxUsers; i++) {
        refresh_user_credentials(i);
    }

    unsigned restored = 0;
    for (uint16_t i = 0; i < kAliroEndpointKeysSupported; i++) {
        const StoredCredential *tables[] = {&s_credentials.evictable[i], &s_credentials.nonEvictable[i]};
        const CredentialTypeEnum types[] = {CredentialTypeEnum::kAliroEvictableEndpointKey,
                                            CredentialTypeEnum::kAliroNonEvictableEndpointKey};
        for (size_t t = 0; t < 2; t++) {
            if (tables[t]->status == static_cast<uint8_t>(DlCredentialStatus::kOccupied)) {
                apply_endpoint_key(types[t], i + 1, tables[t]->data, tables[t]->len, true);
                restored++;
            }
        }
    }

    if (restored) {
        ESP_LOGI(k_tag, "restored %u endpoint key(s) from the last commissioning", restored);
    }
}

/* --- Cluster callbacks --------------------------------------------------- */

void emberAfDoorLockClusterInitCallback(EndpointId endpoint)
{
    DoorLockServer::Instance().InitServer(endpoint);

    store_load();

    const matter_lock_hooks_t *hooks = matter_lock_hooks();
    const bool locked = !hooks || !hooks->is_locked || hooks->is_locked();
    DoorLockServer::Instance().SetLockState(endpoint, locked ? DlLockState::kLocked : DlLockState::kUnlocked);
}

bool emberAfPluginDoorLockOnDoorLockCommand(EndpointId endpointId, const Nullable<FabricIndex> &fabricIdx,
                                            const Nullable<NodeId> &nodeId, const Optional<ByteSpan> &pinCode,
                                            OperationErrorEnum &err)
{
    const matter_lock_hooks_t *hooks = matter_lock_hooks();
    if (!hooks || !hooks->lock || hooks->lock() != ESP_OK) {
        err = OperationErrorEnum::kUnspecified;
        return false;
    }
    ESP_LOGI(k_tag, "locked by a Matter controller");
    return true;
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(EndpointId endpointId, const Nullable<FabricIndex> &fabricIdx,
                                              const Nullable<NodeId> &nodeId, const Optional<ByteSpan> &pinCode,
                                              OperationErrorEnum &err)
{
    const matter_lock_hooks_t *hooks = matter_lock_hooks();
    if (!hooks || !hooks->unlock || hooks->unlock() != ESP_OK) {
        err = OperationErrorEnum::kUnspecified;
        return false;
    }
    ESP_LOGI(k_tag, "unlocked by a Matter controller");
    return true;
}

bool emberAfPluginDoorLockGetUser(EndpointId endpointId, uint16_t userIndex, EmberAfPluginDoorLockUserInfo &user)
{
    VerifyOrReturnValue(userIndex >= 1 && userIndex <= kMaxUsers, false); /* one-indexed */
    store_load();

    const StoredUser &stored = s_users[userIndex - 1];
    user.userStatus = static_cast<UserStatusEnum>(stored.status);
    if (user.userStatus == UserStatusEnum::kAvailable) {
        return true;
    }

    user.userName = CharSpan(stored.name, stored.nameLen);
    user.credentials = Span<const CredentialStruct>(s_user_credentials[userIndex - 1], stored.credentialCount);
    user.userUniqueId = stored.uniqueId;
    user.userType = static_cast<UserTypeEnum>(stored.type);
    user.credentialRule = static_cast<CredentialRuleEnum>(stored.rule);
    user.createdBy = stored.createdBy;
    user.lastModifiedBy = stored.modifiedBy;
    user.creationSource = DlAssetSource::kMatterIM;
    user.modificationSource = DlAssetSource::kMatterIM;
    return true;
}

bool emberAfPluginDoorLockSetUser(EndpointId endpointId, uint16_t userIndex, FabricIndex creator, FabricIndex modifier,
                                  const CharSpan &userName, uint32_t uniqueId, UserStatusEnum userStatus,
                                  UserTypeEnum usertype, CredentialRuleEnum credentialRule,
                                  const CredentialStruct *credentials, size_t totalCredentials)
{
    VerifyOrReturnValue(userIndex >= 1 && userIndex <= kMaxUsers, false);
    VerifyOrReturnValue(userName.size() <= DOOR_LOCK_MAX_USER_NAME_SIZE, false);
    VerifyOrReturnValue(totalCredentials <= kMaxCredentialsPerUser, false);
    store_load();

    StoredUser &stored = s_users[userIndex - 1];
    memset(&stored, 0, sizeof(stored));
    memcpy(stored.name, userName.data(), userName.size());
    stored.nameLen = static_cast<uint8_t>(userName.size());
    stored.uniqueId = uniqueId;
    stored.status = to_underlying(userStatus);
    stored.type = to_underlying(usertype);
    stored.rule = to_underlying(credentialRule);
    stored.createdBy = creator;
    stored.modifiedBy = modifier;
    stored.credentialCount = static_cast<uint8_t>(totalCredentials);

    for (size_t i = 0; i < totalCredentials; i++) {
        stored.credentials[i].type = to_underlying(credentials[i].credentialType);
        stored.credentials[i].index = credentials[i].credentialIndex;
    }
    refresh_user_credentials(userIndex - 1);

    persist(k_nvs_users, s_users, sizeof(s_users));
    ESP_LOGI(k_tag, "user %u set: '%.*s', %u credential(s)", (unsigned)userIndex, (int)userName.size(), userName.data(),
             (unsigned)totalCredentials);
    return true;
}

bool emberAfPluginDoorLockGetCredential(EndpointId endpointId, uint16_t credentialIndex,
                                        CredentialTypeEnum credentialType,
                                        EmberAfPluginDoorLockCredentialInfo &credential)
{
    store_load();

    const StoredCredential *slot = credential_slot(credentialType, credentialIndex);
    VerifyOrReturnValue(slot != nullptr, false);

    credential.status = static_cast<DlCredentialStatus>(slot->status);
    if (credential.status == DlCredentialStatus::kAvailable) {
        return true;
    }

    credential.credentialType = static_cast<CredentialTypeEnum>(slot->type);
    credential.credentialData = ByteSpan(slot->data, slot->len);
    credential.createdBy = slot->createdBy;
    credential.lastModifiedBy = slot->modifiedBy;
    credential.creationSource = DlAssetSource::kMatterIM;
    credential.modificationSource = DlAssetSource::kMatterIM;
    return true;
}

bool emberAfPluginDoorLockSetCredential(EndpointId endpointId, uint16_t credentialIndex, FabricIndex creator,
                                        FabricIndex modifier, DlCredentialStatus credentialStatus,
                                        CredentialTypeEnum credentialType, const ByteSpan &credentialData)
{
    store_load();

    StoredCredential *slot = credential_slot(credentialType, credentialIndex);
    VerifyOrReturnValue(slot != nullptr, false);
    VerifyOrReturnValue(credentialData.size() <= kMaxCredentialSize, false);

    const bool was_occupied = slot->status == to_underlying(DlCredentialStatus::kOccupied);
    const bool now_occupied = credentialStatus == DlCredentialStatus::kOccupied;

    /* Withdraw the old key before overwriting it, or the reader keeps opening
     * for a phone the controller believes it has just removed. */
    if (was_occupied && is_aliro_endpoint_key(static_cast<CredentialTypeEnum>(slot->type))) {
        apply_endpoint_key(static_cast<CredentialTypeEnum>(slot->type), credentialIndex, slot->data, slot->len, false);
    }

    slot->status = to_underlying(credentialStatus);
    slot->type = to_underlying(credentialType);
    slot->createdBy = creator;
    slot->modifiedBy = modifier;
    slot->len = static_cast<uint8_t>(credentialData.size());
    memset(slot->data, 0, sizeof(slot->data));
    memcpy(slot->data, credentialData.data(), credentialData.size());

    if (now_occupied && is_aliro_endpoint_key(credentialType)) {
        apply_endpoint_key(credentialType, credentialIndex, slot->data, slot->len, true);
    } else if (now_occupied && credentialType == CredentialTypeEnum::kAliroCredentialIssuerKey) {
        /* Stored so the attribute reads back, but unused: this reader
         * authenticates endpoint keys directly and does not verify device
         * certificates issued under an issuer key. */
        ESP_LOGI(k_tag, "stored Aliro credential issuer key %u (not used for access decisions)",
                 (unsigned)credentialIndex);
    }

    persist(k_nvs_credentials, &s_credentials, sizeof(s_credentials));
    return true;
}

/* --- Schedules: not supported -------------------------------------------- */

/*
 * None of the schedule features are advertised, so a spec-compliant controller
 * never calls these. They exist because the cluster server references them
 * unconditionally and the link fails without them.
 */

DlStatus emberAfPluginDoorLockGetSchedule(EndpointId endpointId, uint8_t weekdayIndex, uint16_t userIndex,
                                          EmberAfPluginDoorLockWeekDaySchedule &schedule)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockGetSchedule(EndpointId endpointId, uint8_t yearDayIndex, uint16_t userIndex,
                                          EmberAfPluginDoorLockYearDaySchedule &schedule)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockGetSchedule(EndpointId endpointId, uint8_t holidayIndex,
                                          EmberAfPluginDoorLockHolidaySchedule &holidaySchedule)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockSetSchedule(EndpointId endpointId, uint8_t weekdayIndex, uint16_t userIndex,
                                          DlScheduleStatus status, DaysMaskMap daysMask, uint8_t startHour,
                                          uint8_t startMinute, uint8_t endHour, uint8_t endMinute)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockSetSchedule(EndpointId endpointId, uint8_t yearDayIndex, uint16_t userIndex,
                                          DlScheduleStatus status, uint32_t localStartTime, uint32_t localEndTime)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockSetSchedule(EndpointId endpointId, uint8_t holidayIndex, DlScheduleStatus status,
                                          uint32_t localStartTime, uint32_t localEndTime,
                                          OperatingModeEnum operatingMode)
{
    return DlStatus::kFailure;
}

void emberAfPluginDoorLockOnAutoRelock(EndpointId endpointId)
{
    /* access_control runs its own relock timer off the configured unlock
     * duration, and reports the result back through the observer. */
}
