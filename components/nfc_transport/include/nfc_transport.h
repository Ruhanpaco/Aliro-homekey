/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "app_config.h"

#include <esp_err.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Hardware-independent interface to an NFC frontend.
 *
 * Aliro runs as ISO 7816 APDUs over an ISO 14443-4 link. Everything above that
 * link is the Aliro SDK's problem; everything below it is a chip driver's
 * problem. This struct is the seam between the two, so that swapping a PN532
 * for a PN7160 or an ST25R3916 touches exactly one file.
 *
 * Lifecycle, as driven by the reader task:
 *
 *      init() once
 *      loop:
 *          poll()                  service the driver / look for a device
 *          if activate():          an ISO 14443-4 device is selected
 *              exchange() * N      one Aliro transaction
 *              deactivate()
 */
typedef struct nfc_transport_s {
    /** @brief Bring up the frontend. Called once before any other call. */
    esp_err_t (*init)(void *ctx);

    /** @brief Service the driver. Called on every pass of the polling loop. */
    void (*poll)(void *ctx);

    /** @brief Return true when a device is selected and ready for APDUs. */
    bool (*activate)(void *ctx);

    /** @brief Release the currently selected device. */
    void (*deactivate)(void *ctx);

    /**
     * @brief Send one APDU and return its response.
     *
     * Signature deliberately mirrors esp_aliro_message_exchange_cb_t.
     *
     * @param[in]    ctx          Driver context
     * @param[in]    command      APDU to send
     * @param[in]    command_len  Length of @p command in bytes
     * @param[out]   response     Response buffer
     * @param[inout] response_len Capacity on input, response length on output
     */
    esp_err_t (*exchange)(void *ctx, const uint8_t *command, size_t command_len, uint8_t *response,
                          size_t *response_len);

    /** @brief Driver-private state, passed back to every callback. */
    void *ctx;

    /** @brief Human-readable driver name, for logs. */
    const char *name;
} nfc_transport_t;

/**
 * @brief A transport that never detects a device.
 *
 * Lets the firmware build, boot and run its full startup path on a board with
 * no NFC frontend wired yet. Replace with a real driver, not with a hack in
 * the reader.
 */
const nfc_transport_t *nfc_transport_stub(void);

/**
 * @brief PN532 frontend, over SPI or I2C.
 *
 * The only real driver here, because the PN532 is the frontend that is both
 * widely available and does ISO 14443-4 reader mode with the ISO-DEP layer
 * inside the chip -- so whole APDUs go down and whole APDUs come back.
 *
 * Returns a transport whose init() fails if no chip answers on the configured
 * pins; the caller keeps running without a reader rather than refusing to boot.
 */
const nfc_transport_t *nfc_transport_pn532(const nfc_hw_config_t *cfg);

/**
 * @brief Tell the frontend which reader it should announce itself as.
 *
 * The reader group identifier goes into the Apple ECP beacon, the frame that
 * lets a locked phone offer its credential without the wallet being opened.
 * Call it after nfc_transport_from_config() and before the transport is
 * started; a frontend that cannot emit a beacon ignores it.
 *
 * Identity, not wiring, which is why it is not in @ref nfc_hw_config_t: this
 * changes when a controller provisions the reader, and the pin numbers do not.
 */
void nfc_transport_set_reader_id(const uint8_t *id, size_t len);

/**
 * @brief Pick a driver for the configured chip.
 *
 * Falls back to the stub for any chip whose driver is not written yet, and
 * says so in the log rather than pretending to work.
 */
const nfc_transport_t *nfc_transport_from_config(const nfc_hw_config_t *cfg);

#ifdef __cplusplus
}
#endif
