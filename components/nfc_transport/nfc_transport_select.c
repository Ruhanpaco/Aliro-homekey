/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nfc_transport.h"

#include <esp_log.h>

static const char *const k_tag = "nfc";

const nfc_transport_t *nfc_transport_from_config(const nfc_hw_config_t *cfg)
{
    if (!cfg || cfg->chip == NFC_CHIP_NONE) {
        return nfc_transport_stub();
    }

    if (cfg->bus == NFC_BUS_SPI) {
        ESP_LOGI(k_tag, "configured: SPI%d sck=%d miso=%d mosi=%d cs=%d @ %lu Hz, irq=%d rst=%d",
                 cfg->spi_host + 1, cfg->spi_sck, cfg->spi_miso, cfg->spi_mosi, cfg->spi_cs,
                 (unsigned long)cfg->spi_freq_hz, cfg->irq_pin, cfg->rst_pin);
    } else if (cfg->bus == NFC_BUS_I2C) {
        ESP_LOGI(k_tag, "configured: I2C sda=%d scl=%d addr=0x%02X @ %lu Hz, irq=%d rst=%d", cfg->i2c_sda,
                 cfg->i2c_scl, cfg->i2c_addr, (unsigned long)cfg->i2c_freq_hz, cfg->irq_pin, cfg->rst_pin);
    }

    if (cfg->chip == NFC_CHIP_PN532) {
        if (cfg->bus != NFC_BUS_SPI && cfg->bus != NFC_BUS_I2C) {
            ESP_LOGE(k_tag, "the PN532 needs a bus; pick SPI or I2C in the configuration");
            return nfc_transport_stub();
        }
        return nfc_transport_pn532(cfg);
    }

    /* The PN7160 and ST25R3916 are offered by the configuration because the
     * pin validation understands them, but neither has a driver. Saying so is
     * better than a transport that silently never sees a card. */
    ESP_LOGW(k_tag, "no driver implemented for the selected chip, using the stub transport");
    return nfc_transport_stub();
}
