/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "m5nfc.h"
#include "pn532.h"

#include <esp_log.h>
#include <sdkconfig.h>

static const char *const k_tag = "nfc/m5compat";

esp_err_t m5nfc_init(void)
{
    /*
     * Only the selected bus's pins are set. A Kconfig symbol behind
     * "depends on" does not exist at all when the dependency is false, so
     * naming the I2C pins in an SPI build is a compile error, not a default.
     */
    const pn532_config_t cfg = {
        .rst_pin = CONFIG_PN532_RST_PIN,
#if CONFIG_PN532_BUS_I2C
        .bus = PN532_BUS_I2C,
        .i2c_sda = CONFIG_PN532_I2C_SDA,
        .i2c_scl = CONFIG_PN532_I2C_SCL,
        .i2c_freq_hz = CONFIG_PN532_I2C_FREQ_HZ,
        .i2c_addr = CONFIG_PN532_I2C_ADDR,
#elif CONFIG_PN532_BUS_SPI
        .bus = PN532_BUS_SPI,
        .spi_host = CONFIG_PN532_SPI_HOST,
        .spi_sck = CONFIG_PN532_SPI_SCK,
        .spi_miso = CONFIG_PN532_SPI_MISO,
        .spi_mosi = CONFIG_PN532_SPI_MOSI,
        .spi_cs = CONFIG_PN532_SPI_CS,
        .spi_freq_hz = CONFIG_PN532_SPI_FREQ_HZ,
#else
#error "PN532: no bus selected in menuconfig"
#endif
    };

    ESP_LOGI(k_tag, "PN532 on %s", cfg.bus == PN532_BUS_I2C ? "I2C" : "SPI");
    return pn532_begin(&cfg);
}

void m5nfc_update(void)
{
    pn532_update();
}

bool m5nfc_activate(void)
{
    return pn532_activate();
}

void m5nfc_deactivate(void)
{
    pn532_deactivate();
}

esp_err_t m5nfc_message_exchange(const uint8_t *command, size_t command_len, uint8_t *response, size_t *response_len)
{
    return pn532_message_exchange(command, command_len, response, response_len);
}
