/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PN532 frontend.
 *
 * The PN532 is the only frontend this project drives, because it is the one
 * that is actually available: every clone module on the market speaks the same
 * protocol, and it does ISO 14443-4 reader mode with the ISO-DEP layer handled
 * in the chip. InDataExchange performs RATS on the first call for a target
 * whose SAK advertises 14443-4 support, so this driver hands whole APDUs down
 * and gets whole APDUs back -- which is exactly the seam nfc_transport_t wants.
 *
 * Everything here is the NXP "normal information frame" protocol from
 * PN532/C1 User Manual (UM0701-02), section 6.2.1.
 */

#include "nfc_transport.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>

static const char *const k_tag = "nfc/pn532";

/* --- protocol constants -------------------------------------------------- */

#define PN532_PREAMBLE   0x00
#define PN532_STARTCODE1 0x00
#define PN532_STARTCODE2 0xFF
#define PN532_POSTAMBLE  0x00
#define PN532_HOSTTOPN   0xD4
#define PN532_PNTOHOST   0xD5

#define CMD_GET_FIRMWARE_VERSION   0x02
#define CMD_SAM_CONFIGURATION      0x14
#define CMD_RF_CONFIGURATION       0x32
#define CMD_IN_DATA_EXCHANGE       0x40
#define CMD_IN_LIST_PASSIVE_TARGET 0x4A
#define CMD_IN_RELEASE             0x52

/* SPI is byte-addressed by a leading operation code, and the bus runs
 * LSB-first -- the one detail that silently produces garbage if missed. */
#define SPI_OP_DATA_WRITE  0x01
#define SPI_OP_STATUS_READ 0x02
#define SPI_OP_DATA_READ   0x03
#define SPI_STATUS_READY   0x01

/* A PN532 frame carries at most 255 data bytes; the biggest thing this driver
 * moves is one Aliro APDU, which is far smaller. */
#define PN532_FRAME_MAX 264

static const uint8_t k_ack[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

/* --- driver state -------------------------------------------------------- */

typedef struct {
    nfc_hw_config_t cfg;

    spi_device_handle_t spi;
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t i2c_dev;

    bool ready;    /*!< The chip answered GetFirmwareVersion */
    bool selected; /*!< A target is currently activated */
    uint8_t fw_major;
    uint8_t fw_minor;

    /* One shared scratch buffer: the reader task is the only caller, and the
     * alternative is 264 bytes of stack in a task that already carries an
     * mbedTLS session. */
    uint8_t frame[PN532_FRAME_MAX];
} pn532_t;

static pn532_t s_pn532;

/* --- bus layer ----------------------------------------------------------- */

static bool is_spi(const pn532_t *dev)
{
    return dev->cfg.bus == NFC_BUS_SPI;
}

/*
 * Bus scratch lives here rather than on the stack. This is the deepest point
 * in the call chain, and the caller is the reader task -- which is already
 * running an ECDH and an AES-GCM from the Aliro SDK on the same 8 KB. Half a
 * kilobyte of permanent RAM is a better trade than half a kilobyte of stack
 * at the bottom of that. Safe because the driver is a singleton and only the
 * reader task ever calls it.
 */
static uint8_t s_bus_tx[PN532_FRAME_MAX + 1];
static uint8_t s_bus_rx[PN532_FRAME_MAX + 1];

static esp_err_t bus_write(pn532_t *dev, const uint8_t *data, size_t len)
{
    if (is_spi(dev)) {
        if (len + 1 > sizeof(s_bus_tx)) {
            return ESP_ERR_INVALID_SIZE;
        }
        s_bus_tx[0] = SPI_OP_DATA_WRITE;
        memcpy(s_bus_tx + 1, data, len);
        const spi_transaction_t t = {.length = (len + 1) * 8, .tx_buffer = s_bus_tx};
        return spi_device_polling_transmit(dev->spi, (spi_transaction_t *)&t);
    }
    return i2c_master_transmit(dev->i2c_dev, data, len, 100);
}

static esp_err_t bus_read(pn532_t *dev, uint8_t *data, size_t len)
{
    if (len + 1 > sizeof(s_bus_rx)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (is_spi(dev)) {
        memset(s_bus_tx, 0, len + 1);
        memset(s_bus_rx, 0, len + 1);
        s_bus_tx[0] = SPI_OP_DATA_READ;
        const spi_transaction_t t = {.length = (len + 1) * 8, .tx_buffer = s_bus_tx, .rx_buffer = s_bus_rx};
        const esp_err_t err = spi_device_polling_transmit(dev->spi, (spi_transaction_t *)&t);
        if (err == ESP_OK) {
            /* The first byte clocks out while the opcode goes in. */
            memcpy(data, s_bus_rx + 1, len);
        }
        return err;
    }

    /* I2C prepends a ready byte to every read. */
    const esp_err_t err = i2c_master_receive(dev->i2c_dev, s_bus_rx, len + 1, 100);
    if (err == ESP_OK) {
        memcpy(data, s_bus_rx + 1, len);
    }
    return err;
}

/** @brief True once the chip has a frame waiting. */
static bool bus_ready(pn532_t *dev)
{
    if (is_spi(dev)) {
        const uint8_t tx[2] = {SPI_OP_STATUS_READ, 0x00};
        uint8_t rx[2] = {0};
        const spi_transaction_t t = {.length = 16, .tx_buffer = tx, .rx_buffer = rx};
        if (spi_device_polling_transmit(dev->spi, (spi_transaction_t *)&t) != ESP_OK) {
            return false;
        }
        return (rx[1] & SPI_STATUS_READY) != 0;
    }

    uint8_t status = 0;
    if (i2c_master_receive(dev->i2c_dev, &status, 1, 100) != ESP_OK) {
        return false;
    }
    return (status & SPI_STATUS_READY) != 0;
}

static bool wait_ready(pn532_t *dev, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    do {
        if (bus_ready(dev)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (esp_timer_get_time() < deadline);
    return false;
}

/* --- framing ------------------------------------------------------------- */

static esp_err_t send_command(pn532_t *dev, uint8_t cmd, const uint8_t *params, size_t params_len)
{
    /* LEN counts TFI and the command byte along with the parameters. */
    const size_t data_len = params_len + 2;
    if (data_len > 255) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t f[PN532_FRAME_MAX];
    size_t n = 0;
    f[n++] = PN532_PREAMBLE;
    f[n++] = PN532_STARTCODE1;
    f[n++] = PN532_STARTCODE2;
    f[n++] = (uint8_t)data_len;
    f[n++] = (uint8_t)(~data_len + 1); /* length checksum */
    f[n++] = PN532_HOSTTOPN;
    f[n++] = cmd;

    uint8_t sum = PN532_HOSTTOPN + cmd;
    for (size_t i = 0; i < params_len; i++) {
        f[n++] = params[i];
        sum += params[i];
    }
    f[n++] = (uint8_t)(~sum + 1); /* data checksum */
    f[n++] = PN532_POSTAMBLE;

    return bus_write(dev, f, n);
}

static esp_err_t read_ack(pn532_t *dev, uint32_t timeout_ms)
{
    if (!wait_ready(dev, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t ack[sizeof(k_ack)] = {0};
    ESP_RETURN_ON_ERROR(bus_read(dev, ack, sizeof(ack)), k_tag, "ack read failed");
    if (memcmp(ack, k_ack, sizeof(k_ack)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/**
 * @brief Read one response frame and hand back everything after the echoed
 *        command byte.
 */
static esp_err_t read_response(pn532_t *dev, uint8_t cmd, uint8_t *out, size_t out_cap, size_t *out_len,
                               uint32_t timeout_ms)
{
    if (!wait_ready(dev, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }

    /*
     * One read for the whole frame. Not an optimisation: on I2C every read is
     * its own transaction that begins with a fresh status byte, so pulling a
     * frame out in pieces desynchronises the chip. Asking for more bytes than
     * the frame holds is harmless -- the trailing bytes are ignored.
     *
     * The caller's capacity bounds the request, since it already knows how
     * large a response it is prepared to accept. Nine bytes covers the frame
     * overhead: preamble, start code, LEN, LCS, TFI, command echo, DCS,
     * postamble, and one byte of slack for a module that pads.
     */
    size_t want = out_cap + 9;
    if (want > sizeof(dev->frame)) {
        want = sizeof(dev->frame);
    }
    memset(dev->frame, 0, want);
    ESP_RETURN_ON_ERROR(bus_read(dev, dev->frame, want), k_tag, "response read failed");

    /* Find 0x00 0xFF, leaving room for the six header bytes that follow it. */
    const uint8_t *f = dev->frame;
    size_t i = 0;
    while (i + 6 <= want && !(f[i] == 0x00 && f[i + 1] == 0xFF)) {
        i++;
    }
    if (i + 6 > want) {
        ESP_LOGE(k_tag, "no frame start in response to 0x%02X", cmd);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t len = f[i + 2];
    const uint8_t lcs = f[i + 3];
    if ((uint8_t)(len + lcs) != 0) {
        ESP_LOGE(k_tag, "bad length checksum in response to 0x%02X", cmd);
        return ESP_ERR_INVALID_CRC;
    }
    if (len < 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (f[i + 4] != PN532_PNTOHOST || f[i + 5] != (uint8_t)(cmd + 1)) {
        ESP_LOGE(k_tag, "response is for 0x%02X, expected 0x%02X", f[i + 5], cmd + 1);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t body_len = len - 2;
    if (body_len > out_cap) {
        ESP_LOGE(k_tag, "response body of %u bytes does not fit %u", (unsigned)body_len, (unsigned)out_cap);
        return ESP_ERR_INVALID_SIZE;
    }
    /* Body and the checksum byte after it must both be inside what was read. */
    if (i + 6 + body_len >= want) {
        ESP_LOGE(k_tag, "response frame is truncated");
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t sum = PN532_PNTOHOST + (uint8_t)(cmd + 1);
    for (size_t j = 0; j < body_len; j++) {
        sum += f[i + 6 + j];
    }
    if ((uint8_t)(sum + f[i + 6 + body_len]) != 0) {
        ESP_LOGE(k_tag, "bad data checksum in response to 0x%02X", cmd);
        return ESP_ERR_INVALID_CRC;
    }

    memcpy(out, f + i + 6, body_len);
    *out_len = body_len;
    return ESP_OK;
}

static esp_err_t pn532_command(pn532_t *dev, uint8_t cmd, const uint8_t *params, size_t params_len, uint8_t *out,
                               size_t out_cap, size_t *out_len, uint32_t timeout_ms)
{
    size_t discard = 0;
    if (!out_len) {
        out_len = &discard;
    }
    ESP_RETURN_ON_ERROR(send_command(dev, cmd, params, params_len), k_tag, "0x%02X: send failed", cmd);
    ESP_RETURN_ON_ERROR(read_ack(dev, 100), k_tag, "0x%02X: no ACK", cmd);
    return read_response(dev, cmd, out, out_cap, out_len, timeout_ms);
}

/* --- bring-up ------------------------------------------------------------ */

static esp_err_t bus_init(pn532_t *dev)
{
    if (is_spi(dev)) {
        const spi_host_device_t host = dev->cfg.spi_host == 2 ? SPI3_HOST : SPI2_HOST;
        const spi_bus_config_t bus = {
            .mosi_io_num = dev->cfg.spi_mosi,
            .miso_io_num = dev->cfg.spi_miso,
            .sclk_io_num = dev->cfg.spi_sck,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = PN532_FRAME_MAX + 1,
        };
        ESP_RETURN_ON_ERROR(spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO), k_tag, "SPI bus init failed");

        const spi_device_interface_config_t devcfg = {
            .clock_speed_hz = dev->cfg.spi_freq_hz ? (int)dev->cfg.spi_freq_hz : 1000000,
            .mode = 0,
            .spics_io_num = dev->cfg.spi_cs,
            .queue_size = 1,
            /* The PN532 clocks SPI least-significant bit first. Without both
             * of these every byte arrives bit-reversed and nothing matches. */
            .flags = SPI_DEVICE_BIT_LSBFIRST,
        };
        return spi_bus_add_device(host, &devcfg, &dev->spi);
    }

    const i2c_master_bus_config_t bus = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = dev->cfg.i2c_scl,
        .sda_io_num = dev->cfg.i2c_sda,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &dev->i2c_bus), k_tag, "I2C bus init failed");

    const i2c_device_config_t devcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev->cfg.i2c_addr ? dev->cfg.i2c_addr : 0x24,
        .scl_speed_hz = dev->cfg.i2c_freq_hz ? dev->cfg.i2c_freq_hz : 100000,
    };
    return i2c_master_bus_add_device(dev->i2c_bus, &devcfg, &dev->i2c_dev);
}

/** @brief Pulse RSTPD_N if the board wired it, so a wedged chip comes back. */
static void hardware_reset(const pn532_t *dev)
{
    if (dev->cfg.rst_pin == APP_CFG_PIN_UNSET) {
        return;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << dev->cfg.rst_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        return;
    }
    gpio_set_level(dev->cfg.rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(dev->cfg.rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static esp_err_t pn532_init(void *ctx)
{
    pn532_t *dev = ctx;

    ESP_RETURN_ON_ERROR(bus_init(dev), k_tag, "bus init failed");
    hardware_reset(dev);

    /* The chip may be asleep; the first command after power-up is routinely
     * lost, so ask twice before believing it is absent. */
    uint8_t version[4] = {0};
    size_t version_len = 0;
    esp_err_t err = pn532_command(dev, CMD_GET_FIRMWARE_VERSION, NULL, 0, version, sizeof(version), &version_len, 200);
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(50));
        err = pn532_command(dev, CMD_GET_FIRMWARE_VERSION, NULL, 0, version, sizeof(version), &version_len, 500);
    }
    if (err != ESP_OK || version_len < 4) {
        ESP_LOGE(k_tag, "no PN532 answered on the configured %s pins", is_spi(dev) ? "SPI" : "I2C");
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    dev->fw_major = version[1];
    dev->fw_minor = version[2];
    ESP_LOGI(k_tag, "PN532 found, firmware %u.%u", dev->fw_major, dev->fw_minor);

    /* Normal mode, no SAM, 1 s timeout, and leave the IRQ line alone -- this
     * driver polls the status byte rather than wiring an interrupt. */
    const uint8_t sam[] = {0x01, 0x14, 0x00};
    ESP_RETURN_ON_ERROR(pn532_command(dev, CMD_SAM_CONFIGURATION, sam, sizeof(sam), NULL, 0, NULL, 200), k_tag,
                        "SAMConfiguration failed");

    /* MxRtyPassiveActivation = 0: report an empty field immediately instead of
     * blocking the reader task on every pass of the polling loop. */
    const uint8_t retries[] = {0x05, 0xFF, 0x01, 0x00};
    ESP_RETURN_ON_ERROR(pn532_command(dev, CMD_RF_CONFIGURATION, retries, sizeof(retries), NULL, 0, NULL, 200), k_tag,
                        "RFConfiguration(retries) failed");

    /* Field on, with automatic RF collision avoidance. */
    const uint8_t field[] = {0x01, 0x03};
    ESP_RETURN_ON_ERROR(pn532_command(dev, CMD_RF_CONFIGURATION, field, sizeof(field), NULL, 0, NULL, 200), k_tag,
                        "RFConfiguration(field) failed");

    dev->ready = true;
    ESP_LOGI(k_tag, "reader ready, waiting for a device");
    return ESP_OK;
}

/* --- polling loop -------------------------------------------------------- */

static void pn532_poll(void *ctx)
{
    (void)ctx;
    /* Nothing to service: the PN532 does discovery inside InListPassiveTarget,
     * which activate() issues on every pass. */
}

static bool pn532_activate(void *ctx)
{
    pn532_t *dev = ctx;
    if (!dev->ready) {
        return false;
    }

    /*
     * A phone presenting an Aliro credential answers ISO 14443-4 like any
     * other card, so a plain type A poll finds it. Apple's ECP broadcast --
     * an unsolicited frame that makes a locked iPhone offer its credential
     * without the wallet being opened -- would go here, sent with
     * InCommunicateThru before this call. The payload is ecosystem-specific
     * and this project has no authoritative Aliro one, so it is left out
     * rather than guessed at.
     */
    const uint8_t params[] = {0x01, 0x00}; /* one target, 106 kbps type A */
    uint8_t found[64] = {0};
    size_t found_len = 0;

    const esp_err_t err =
        pn532_command(dev, CMD_IN_LIST_PASSIVE_TARGET, params, sizeof(params), found, sizeof(found), &found_len, 100);
    if (err != ESP_OK || found_len < 1 || found[0] == 0) {
        return false; /* empty field is the normal case, not an error */
    }

    /*
     * NbTg, Tg, SENS_RES (2), SEL_RES, NFCIDLength, NFCID...
     * Bit 5 of SEL_RES is the target's claim to speak ISO 14443-4. Without it
     * InDataExchange has no ISO-DEP layer to run and every APDU would fail, so
     * refuse here where the reason is still obvious.
     */
    if (found_len >= 5 && !(found[4] & 0x20)) {
        ESP_LOGW(k_tag, "device in field is not ISO 14443-4 (SAK 0x%02X); ignoring", found[4]);
        (void)pn532_command(dev, CMD_IN_RELEASE, (const uint8_t[]){0x01}, 1, NULL, 0, NULL, 100);
        return false;
    }

    if (found_len >= 6) {
        ESP_LOG_BUFFER_HEX_LEVEL(k_tag, found + 6, found[5] > found_len - 6 ? found_len - 6 : found[5], ESP_LOG_DEBUG);
    }

    dev->selected = true;
    return true;
}

static void pn532_deactivate(void *ctx)
{
    pn532_t *dev = ctx;
    if (!dev->selected) {
        return;
    }
    const uint8_t target[] = {0x01};
    (void)pn532_command(dev, CMD_IN_RELEASE, target, sizeof(target), NULL, 0, NULL, 100);
    dev->selected = false;
}

static esp_err_t pn532_exchange(void *ctx, const uint8_t *command, size_t command_len, uint8_t *response,
                                size_t *response_len)
{
    pn532_t *dev = ctx;
    ESP_RETURN_ON_FALSE(dev->selected, ESP_ERR_INVALID_STATE, k_tag, "no target selected");
    ESP_RETURN_ON_FALSE(command && response && response_len, ESP_ERR_INVALID_ARG, k_tag, "invalid exchange");
    /* One target byte and the APDU have to fit a single frame. */
    ESP_RETURN_ON_FALSE(command_len <= 253, ESP_ERR_INVALID_SIZE, k_tag, "APDU of %u bytes is too long",
                        (unsigned)command_len);

    uint8_t params[254];
    params[0] = 0x01; /* target 1 */
    memcpy(params + 1, command, command_len);

    /* The response is a status byte followed by the card's own reply. */
    uint8_t raw[PN532_FRAME_MAX];
    size_t raw_len = 0;
    ESP_RETURN_ON_ERROR(pn532_command(dev, CMD_IN_DATA_EXCHANGE, params, command_len + 1, raw, sizeof(raw), &raw_len,
                                      1000),
                        k_tag, "InDataExchange failed");

    ESP_RETURN_ON_FALSE(raw_len >= 1, ESP_ERR_INVALID_RESPONSE, k_tag, "empty exchange response");
    if ((raw[0] & 0x3F) != 0x00) {
        ESP_LOGW(k_tag, "target reported error 0x%02X", raw[0]);
        dev->selected = false; /* the link is gone; stop pretending otherwise */
        return ESP_FAIL;
    }

    const size_t payload = raw_len - 1;
    ESP_RETURN_ON_FALSE(payload <= *response_len, ESP_ERR_INVALID_SIZE, k_tag,
                        "response of %u bytes does not fit %u", (unsigned)payload, (unsigned)*response_len);
    memcpy(response, raw + 1, payload);
    *response_len = payload;
    return ESP_OK;
}

/* --- public -------------------------------------------------------------- */

static const nfc_transport_t k_pn532 = {
    .init = pn532_init,
    .poll = pn532_poll,
    .activate = pn532_activate,
    .deactivate = pn532_deactivate,
    .exchange = pn532_exchange,
    .ctx = &s_pn532,
    .name = "pn532",
};

const nfc_transport_t *nfc_transport_pn532(const nfc_hw_config_t *cfg)
{
    s_pn532 = (pn532_t){0};
    s_pn532.cfg = *cfg;
    return &k_pn532;
}
