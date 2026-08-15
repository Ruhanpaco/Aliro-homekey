/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nfc_transport.h"

#include "pn532.h"

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

/* --- nfc_transport_t adapter -------------------------------------------- */

/*
 * The driver exposes five plain functions with no context pointer, because a
 * board has one NFC frontend. nfc_transport_t passes a context anyway, so
 * these wrappers bridge the two -- and keep the driver free of any knowledge
 * of app_config, which is what lets an esp-matter door lock use it too.
 */
static pn532_config_t s_cfg;

static esp_err_t transport_init(void *ctx)
{
    (void)ctx;
    return pn532_begin(&s_cfg);
}

static void transport_poll(void *ctx)
{
    (void)ctx;
    pn532_update();
}

static bool transport_activate(void *ctx)
{
    (void)ctx;
    return pn532_activate();
}

static void transport_deactivate(void *ctx)
{
    (void)ctx;
    pn532_deactivate();
}

static esp_err_t transport_exchange(void *ctx, const uint8_t *command, size_t command_len, uint8_t *response,
                                    size_t *response_len)
{
    (void)ctx;
    return pn532_message_exchange(command, command_len, response, response_len);
}

static const nfc_transport_t k_pn532 = {
    .init = transport_init,
    .poll = transport_poll,
    .activate = transport_activate,
    .deactivate = transport_deactivate,
    .exchange = transport_exchange,
    .ctx = NULL,
    .name = "pn532",
};

const nfc_transport_t *nfc_transport_pn532(const nfc_hw_config_t *cfg)
{
    /* app_config's view of the wiring, translated into the driver's own. */
    s_cfg = (pn532_config_t){
        .bus = cfg->bus == NFC_BUS_I2C ? PN532_BUS_I2C : PN532_BUS_SPI,
        .spi_host = cfg->spi_host,
        .spi_sck = cfg->spi_sck,
        .spi_miso = cfg->spi_miso,
        .spi_mosi = cfg->spi_mosi,
        .spi_cs = cfg->spi_cs,
        .spi_freq_hz = cfg->spi_freq_hz,
        .i2c_sda = cfg->i2c_sda,
        .i2c_scl = cfg->i2c_scl,
        .i2c_freq_hz = cfg->i2c_freq_hz,
        .i2c_addr = cfg->i2c_addr,
        .rst_pin = cfg->rst_pin == APP_CFG_PIN_UNSET ? PN532_PIN_UNSET : cfg->rst_pin,
    };
    return &k_pn532;
}
