/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <app/clusters/door-lock-server/door-lock-server.h>

/**
 * @brief The Door Lock cluster's Aliro half, answered from our own reader.
 *
 * Espressif's example delegate owns an Aliro reader and an NFC polling task of
 * its own. Ours does not: this firmware already has a reader (components/
 * aliro_reader) with a credential store, key-slot lookup and an access
 * decision behind it, all of which work with no Matter at all. So this class
 * is only a translator -- it reads the identity out of the running reader for
 * the Aliro attributes, and turns SetAliroReaderConfig into "adopt this
 * identity and restart", which app_main implements through the hook table.
 */
class AliroReaderDelegate : public chip::app::Clusters::DoorLock::Delegate {
public:
    static AliroReaderDelegate &Instance()
    {
        static AliroReaderDelegate instance;
        return instance;
    }

    CHIP_ERROR GetAliroReaderVerificationKey(chip::MutableByteSpan &verificationKey) override;
    CHIP_ERROR GetAliroReaderGroupIdentifier(chip::MutableByteSpan &groupIdentifier) override;
    CHIP_ERROR GetAliroReaderGroupSubIdentifier(chip::MutableByteSpan &groupSubIdentifier) override;
    CHIP_ERROR GetAliroExpeditedTransactionSupportedProtocolVersionAtIndex(size_t index,
                                                                          chip::MutableByteSpan &protocolVersion) override;
    CHIP_ERROR GetAliroGroupResolvingKey(chip::MutableByteSpan &groupResolvingKey) override;
    CHIP_ERROR GetAliroSupportedBLEUWBProtocolVersionAtIndex(size_t index,
                                                            chip::MutableByteSpan &protocolVersion) override;
    uint8_t GetAliroBLEAdvertisingVersion() override;
    uint16_t GetNumberOfAliroCredentialIssuerKeysSupported() override;
    uint16_t GetNumberOfAliroEndpointKeysSupported() override;

    CHIP_ERROR SetAliroReaderConfig(const chip::ByteSpan &signingKey, const chip::ByteSpan &verificationKey,
                                    const chip::ByteSpan &groupIdentifier,
                                    const chip::Optional<chip::ByteSpan> &groupResolvingKey) override;
    CHIP_ERROR ClearAliroReaderConfig() override;

private:
    AliroReaderDelegate() = default;
};

/** @brief How many Aliro endpoint keys the credential store has room for. */
constexpr uint16_t kAliroEndpointKeysSupported = 8;

/** @brief How many Aliro credential issuer keys the credential store holds. */
constexpr uint16_t kAliroCredentialIssuerKeysSupported = 4;
