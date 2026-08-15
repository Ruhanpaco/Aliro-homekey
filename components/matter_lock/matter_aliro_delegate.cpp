/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Compiled to nothing without CONFIG_ALIRO_MATTER_ENABLE. The component builds
 * the same file list either way -- see this component's CMakeLists for why the
 * switch cannot live there -- so the guard lives here instead.
 */
#include <sdkconfig.h>

#if CONFIG_ALIRO_MATTER_ENABLE

#include "matter_aliro_delegate.h"

#include "aliro_reader.h"
#include "matter_lock_priv.h"

#include <esp_log.h>
#include <lib/support/CodeUtils.h>
#include <platform/ESP32/ESP32Utils.h>

#include <string.h>

using chip::ByteSpan;
using chip::MutableByteSpan;
using chip::DeviceLayer::Internal::ESP32Utils;

static const char *const k_tag = "aliro/matter";

/*
 * Aliro protocol version 1.0, big-endian, as the Door Lock cluster's
 * AliroExpeditedTransactionSupportedProtocolVersions list wants it. The SDK
 * speaks exactly this one, so the list has exactly one entry.
 */
static constexpr uint8_t k_protocol_version[] = {0x01, 0x00};

/* Room for an X.509 PEM around a P-256 key, plus its NUL. */
static constexpr size_t k_pem_max = 256;

/* --- Attributes: read straight out of the running reader ----------------- */

CHIP_ERROR AliroReaderDelegate::GetAliroReaderVerificationKey(MutableByteSpan &verificationKey)
{
    /*
     * Null until a controller has provisioned one, and specifically NOT the
     * key of whatever reader happens to be running.
     *
     * The cluster server refuses SetAliroReaderConfig unless this attribute
     * reads null -- an existing configuration has to be cleared first. This
     * reader always has an identity loaded, the development one if nothing
     * else, so answering from the live reader made the device look already
     * provisioned and Apple's request was rejected on arrival:
     *
     *     E [SetAliroReaderConfig] Aliro reader verification key was not read
     *                              or is not null.
     *
     * The development identity is a local default, not something a controller
     * gave us, and reporting it as an Aliro reader configuration was simply
     * untrue.
     */
    if (!matter_lock_reader_configured() || !aliro_reader_is_running()) {
        verificationKey.reduce_size(0);
        return CHIP_NO_ERROR;
    }

    size_t len = verificationKey.size();
    const esp_err_t err = aliro_reader_get_public_key_raw(verificationKey.data(), &len);
    if (err != ESP_OK) {
        verificationKey.reduce_size(0);
        return ESP32Utils::MapError(err);
    }
    verificationKey.reduce_size(len);
    return CHIP_NO_ERROR;
}

CHIP_ERROR AliroReaderDelegate::GetAliroReaderGroupIdentifier(MutableByteSpan &groupIdentifier)
{
    /* Nullable, and null for the same reason as the verification key: the
     * group identifier of a locally-defaulted reader is not one a controller
     * set. */
    if (!matter_lock_reader_configured() || !aliro_reader_is_running()) {
        groupIdentifier.reduce_size(0);
        return CHIP_NO_ERROR;
    }

    size_t len = groupIdentifier.size();
    const esp_err_t err = aliro_reader_get_group_identifier(groupIdentifier.data(), &len);
    if (err != ESP_OK) {
        groupIdentifier.reduce_size(0);
        return ESP32Utils::MapError(err);
    }
    groupIdentifier.reduce_size(len);
    return CHIP_NO_ERROR;
}

CHIP_ERROR AliroReaderDelegate::GetAliroReaderGroupSubIdentifier(MutableByteSpan &groupSubIdentifier)
{
    /* Not nullable, unlike the two above: the spec says this attribute always
     * has a value, so it answers from the running reader whether or not a
     * controller has provisioned anything. */
    if (!aliro_reader_is_running()) {
        groupSubIdentifier.reduce_size(0);
        return CHIP_NO_ERROR;
    }

    size_t len = groupSubIdentifier.size();
    const esp_err_t err = aliro_reader_get_group_sub_identifier(groupSubIdentifier.data(), &len);
    if (err != ESP_OK) {
        groupSubIdentifier.reduce_size(0);
        return ESP32Utils::MapError(err);
    }
    groupSubIdentifier.reduce_size(len);
    return CHIP_NO_ERROR;
}

CHIP_ERROR AliroReaderDelegate::GetAliroExpeditedTransactionSupportedProtocolVersionAtIndex(
    size_t index, MutableByteSpan &protocolVersion)
{
    VerifyOrReturnError(index == 0, CHIP_ERROR_PROVIDER_LIST_EXHAUSTED);
    VerifyOrReturnError(protocolVersion.size() >= sizeof(k_protocol_version), CHIP_ERROR_BUFFER_TOO_SMALL);

    memcpy(protocolVersion.data(), k_protocol_version, sizeof(k_protocol_version));
    protocolVersion.reduce_size(sizeof(k_protocol_version));
    return CHIP_NO_ERROR;
}

CHIP_ERROR AliroReaderDelegate::GetAliroGroupResolvingKey(MutableByteSpan &groupResolvingKey)
{
    /* Only meaningful with Aliro over BLE+UWB, which needs radio hardware an
     * ESP32 with a PN532 does not have. The feature is not advertised. */
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR AliroReaderDelegate::GetAliroSupportedBLEUWBProtocolVersionAtIndex(size_t index,
                                                                             MutableByteSpan &protocolVersion)
{
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

uint8_t AliroReaderDelegate::GetAliroBLEAdvertisingVersion()
{
    return 0;
}

uint16_t AliroReaderDelegate::GetNumberOfAliroCredentialIssuerKeysSupported()
{
    return kAliroCredentialIssuerKeysSupported;
}

uint16_t AliroReaderDelegate::GetNumberOfAliroEndpointKeysSupported()
{
    return kAliroEndpointKeysSupported;
}

/* --- SetAliroReaderConfig: the whole point of the Matter plane ----------- */

CHIP_ERROR AliroReaderDelegate::SetAliroReaderConfig(const ByteSpan &signingKey, const ByteSpan &verificationKey,
                                                     const ByteSpan &groupIdentifier,
                                                     const chip::Optional<ByteSpan> &groupResolvingKey)
{
    using namespace chip::app::Clusters::DoorLock;

    VerifyOrReturnError(signingKey.size() == kAliroSigningKeySize, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(verificationKey.size() == kAliroReaderVerificationKeySize, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(groupIdentifier.size() == kAliroReaderGroupIdentifierSize, CHIP_ERROR_INVALID_ARGUMENT);

    if (groupResolvingKey.HasValue()) {
        ESP_LOGW(k_tag, "ignoring the group resolving key: this reader is NFC only, not BLE+UWB");
    }

    const matter_lock_hooks_t *hooks = matter_lock_hooks();
    VerifyOrReturnError(hooks && hooks->set_reader_identity, CHIP_ERROR_INCORRECT_STATE);

    /*
     * The controller sends raw key material -- 32 bytes of private scalar and
     * an uncompressed 65-byte point -- while the Aliro SDK takes PEM. Both
     * buffers are on the stack and are gone by the time this returns, so the
     * hook is expected to copy anything it keeps.
     */
    char privkey_pem[k_pem_max];
    char pubkey_pem[k_pem_max];
    size_t privkey_len = sizeof(privkey_pem) - 1;
    size_t pubkey_len = sizeof(pubkey_pem) - 1;

    ReturnErrorOnFailure(ESP32Utils::MapError(
        aliro_reader_privkey_pem_from_raw(signingKey.data(), signingKey.size(), privkey_pem, &privkey_len)));
    privkey_pem[privkey_len] = '\0';

    ReturnErrorOnFailure(ESP32Utils::MapError(
        aliro_reader_pubkey_pem_from_raw(verificationKey.data(), verificationKey.size(), pubkey_pem, &pubkey_len)));
    pubkey_pem[pubkey_len] = '\0';

    ESP_LOGI(k_tag, "a controller provisioned a reader identity; restarting the reader with it");

    const esp_err_t err =
        hooks->set_reader_identity(pubkey_pem, privkey_pem, groupIdentifier.data(), groupIdentifier.size());

    /* Wipe the private key off this stack before returning. The compiler is
     * allowed to drop a plain memset on a dying frame; this one it cannot. */
    memset(privkey_pem, 0, sizeof(privkey_pem));
    __asm__ __volatile__("" ::"r"(privkey_pem) : "memory");

    ReturnErrorOnFailure(ESP32Utils::MapError(err));
    matter_lock_set_reader_configured(true);
    return CHIP_NO_ERROR;
}

CHIP_ERROR AliroReaderDelegate::ClearAliroReaderConfig()
{
    const matter_lock_hooks_t *hooks = matter_lock_hooks();
    VerifyOrReturnError(hooks && hooks->clear_reader_identity, CHIP_ERROR_INCORRECT_STATE);

    ESP_LOGW(k_tag, "clearing the reader identity; enrolled phones stop working until one is provisioned again");
    ReturnErrorOnFailure(ESP32Utils::MapError(hooks->clear_reader_identity()));
    matter_lock_set_reader_configured(false);
    return CHIP_NO_ERROR;
}

#endif /* CONFIG_ALIRO_MATTER_ENABLE */
