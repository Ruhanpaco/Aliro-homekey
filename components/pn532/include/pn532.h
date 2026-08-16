/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PN532 wiring, independent of where it came from.
 *
 * Deliberately not app_config's nfc_hw_config_t. The driver is useful in two
 * places that share no configuration system: this firmware, which stores its
 * wiring as JSON in NVS and edits it over a web UI, and an esp-matter door
 * lock, which has neither and takes its pins from Kconfig. Both fill this in.
 */
#define PN532_PIN_UNSET (-1)

typedef enum {
    PN532_BUS_SPI = 0,
    PN532_BUS_I2C,
} pn532_bus_t;

typedef struct {
    pn532_bus_t bus;

    /* SPI */
    uint8_t spi_host; /*!< 1 = SPI2_HOST, 2 = SPI3_HOST */
    int8_t spi_sck;
    int8_t spi_miso;
    int8_t spi_mosi;
    int8_t spi_cs;
    uint32_t spi_freq_hz; /*!< 0 selects 1 MHz */

    /* I2C */
    int8_t i2c_sda;
    int8_t i2c_scl;
    uint32_t i2c_freq_hz; /*!< 0 selects 100 kHz */
    uint8_t i2c_addr;     /*!< 0 selects 0x24 */

    int8_t rst_pin; /*!< RSTPD_N, or PN532_PIN_UNSET when not wired */

    /**
     * First 8 bytes of the reader group identifier, for the Apple ECP beacon.
     *
     * A locked phone answers an ordinary poll with nothing: the wallet has to
     * be opened and the key chosen first. ECP is the announcement that changes
     * that -- a frame naming the reader and the credential profile it wants,
     * which lets the phone offer the right applet without being unlocked.
     *
     * Leave it all-zero and the beacon is skipped entirely; the reader then
     * behaves exactly as it did before, reading a key the user has selected by
     * hand.
     */
    uint8_t reader_id[8];
} pn532_config_t;

/** @brief Bytes of reader group identifier that go into the ECP beacon. */
#define PN532_ECP_READER_ID_LEN 8

/** @brief Header, reader identifier and CRC_A. */
#define PN532_ECP_FRAME_LEN 18

/**
 * @brief The five operations an Aliro reader needs from an NFC frontend.
 *
 * The same five Espressif's own m5nfc component exposes, in the same order and
 * with the same signatures, so either can drive the SDK's session loop.
 *
 *      pn532_begin()    once
 *      loop:
 *          pn532_update()
 *          if pn532_activate():
 *              pn532_message_exchange() * N
 *              pn532_deactivate()
 */
esp_err_t pn532_begin(const pn532_config_t *cfg);
void pn532_update(void);
bool pn532_activate(void);
void pn532_deactivate(void);
esp_err_t pn532_message_exchange(const uint8_t *command, size_t command_len, uint8_t *response, size_t *response_len);

#ifdef __cplusplus
}
#endif
