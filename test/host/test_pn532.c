/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The PN532 driver against a simulated chip.
 *
 * None of this can be checked on a bench without a PN532 wired up, and a
 * framing or checksum mistake is invisible until then -- it looks exactly like
 * bad wiring. So the chip is simulated here: the fake parses the frames the
 * driver sends, validates both checksums the way real silicon does, and
 * answers with frames built independently of the driver's own code.
 *
 * The reference frames come from NXP UM0701-02 (PN532/C1 User Manual).
 */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "nfc_transport.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *fmt, ...)
{
    checks++;
    if (!ok) {
        failures++;
        va_list ap;
        va_start(ap, fmt);
        fputs("  FAIL: ", stdout);
        vprintf(fmt, ap);
        putchar('\n');
        va_end(ap);
    }
}

/* --- the simulated PN532 -------------------------------------------------- */

#define FAKE_MAX 512

static struct {
    uint8_t out[FAKE_MAX]; /* chip -> host, waiting to be read */
    size_t out_len;
    size_t out_pos;

    uint8_t last_cmd;
    uint8_t last_params[FAKE_MAX];
    size_t last_params_len;
    int commands_seen;

    bool bad_data_checksum; /* corrupt the next response */
    bool no_target;         /* answer InListPassiveTarget with NbTg = 0 */
    bool deaf;              /* an empty bus: accept writes, answer nothing */
    uint8_t sak;            /* SEL_RES reported for a found target */
    uint8_t exchange_status;

    uint8_t first_frame[32]; /* exactly as the driver emitted it */
    size_t first_frame_len;
} fake;

static void fake_reset(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.sak = 0x20; /* ISO 14443-4 capable */
}

static void queue(const uint8_t *data, size_t len)
{
    memcpy(fake.out + fake.out_len, data, len);
    fake.out_len += len;
}

/** Build a PN532 -> host response frame, independently of the driver. */
static void queue_response(uint8_t cmd, const uint8_t *body, size_t body_len)
{
    static const uint8_t ack[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    queue(ack, sizeof(ack));

    uint8_t f[FAKE_MAX];
    size_t n = 0;
    const size_t len = body_len + 2;
    f[n++] = 0x00;
    f[n++] = 0x00;
    f[n++] = 0xFF;
    f[n++] = (uint8_t)len;
    f[n++] = (uint8_t)(~len + 1);
    f[n++] = 0xD5;
    f[n++] = (uint8_t)(cmd + 1);

    uint8_t sum = 0xD5 + (uint8_t)(cmd + 1);
    for (size_t i = 0; i < body_len; i++) {
        f[n++] = body[i];
        sum += body[i];
    }
    f[n++] = (uint8_t)(~sum + 1) + (fake.bad_data_checksum ? 1 : 0);
    f[n++] = 0x00;
    queue(f, n);
}

/** Parse one host -> chip frame and decide what to answer with. */
static void fake_receive_frame(const uint8_t *f, size_t len)
{
    /* preamble, start code, LEN, LCS, TFI, cmd, DCS, postamble */
    check(len >= 9, "frame is only %zu bytes", len);
    check(f[0] == 0x00 && f[1] == 0x00 && f[2] == 0xFF, "frame does not start 00 00 FF");

    const uint8_t data_len = f[3];
    check((uint8_t)(data_len + f[4]) == 0, "length checksum is wrong");
    check(f[5] == 0xD4, "TFI is 0x%02X, expected 0xD4", f[5]);

    uint8_t sum = 0;
    for (size_t i = 0; i < data_len; i++) {
        sum += f[5 + i];
    }
    check((uint8_t)(sum + f[5 + data_len]) == 0, "data checksum is wrong");

    fake.last_cmd = f[6];
    fake.last_params_len = data_len - 2;
    memcpy(fake.last_params, f + 7, fake.last_params_len);
    fake.commands_seen++;

    switch (fake.last_cmd) {
    case 0x02: { /* GetFirmwareVersion */
        const uint8_t body[] = {0x32, 0x01, 0x06, 0x07}; /* PN532 v1.6 */
        queue_response(0x02, body, sizeof(body));
        break;
    }
    case 0x14: /* SAMConfiguration */
    case 0x32: /* RFConfiguration */
        queue_response(fake.last_cmd, NULL, 0);
        break;
    case 0x4A: { /* InListPassiveTarget */
        if (fake.no_target) {
            const uint8_t body[] = {0x00};
            queue_response(0x4A, body, sizeof(body));
        } else {
            /* NbTg, Tg, SENS_RES x2, SEL_RES, IDLen, ID... */
            const uint8_t body[] = {0x01, 0x01, 0x00, 0x44, fake.sak, 0x04, 0xDE, 0xAD, 0xBE, 0xEF};
            queue_response(0x4A, body, sizeof(body));
        }
        break;
    }
    case 0x40: { /* InDataExchange: echo the APDU back after a status byte */
        uint8_t body[FAKE_MAX];
        body[0] = fake.exchange_status;
        const size_t apdu_len = fake.last_params_len - 1;
        memcpy(body + 1, fake.last_params + 1, apdu_len);
        queue_response(0x40, body, apdu_len + 1);
        break;
    }
    case 0x52: /* InRelease */
        queue_response(0x52, (const uint8_t[]){0x00}, 1);
        break;
    default:
        check(false, "unexpected command 0x%02X", fake.last_cmd);
        break;
    }
}

static void fake_write(const uint8_t *data, size_t len)
{
    if (fake.first_frame_len == 0 && len <= sizeof(fake.first_frame)) {
        memcpy(fake.first_frame, data, len);
        fake.first_frame_len = len;
    }
    if (fake.deaf) {
        return; /* nothing on the bus to answer */
    }
    /* The whole frame arrives, preamble included -- indices below assume it. */
    fake_receive_frame(data, len);
}

static size_t fake_read(uint8_t *out, size_t len)
{
    size_t n = 0;
    while (n < len && fake.out_pos < fake.out_len) {
        out[n++] = fake.out[fake.out_pos++];
    }
    if (fake.out_pos >= fake.out_len) {
        fake.out_pos = fake.out_len = 0;
    }
    return n;
}

static bool fake_ready(void)
{
    return fake.out_pos < fake.out_len;
}

/* --- stubbed buses -------------------------------------------------------- */

int64_t esp_timer_get_time(void)
{
    static int64_t now;
    now += 1000; /* every call advances 1 ms, so timeouts terminate */
    return now;
}

void vTaskDelay(unsigned ticks)
{
    (void)ticks;
}

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

esp_err_t gpio_set_level(int pin, uint32_t level)
{
    (void)pin;
    (void)level;
    return ESP_OK;
}

esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t *c, int d)
{
    (void)h; (void)c; (void)d;
    return ESP_OK;
}

esp_err_t spi_bus_add_device(spi_host_device_t h, const spi_device_interface_config_t *c, spi_device_handle_t *out)
{
    (void)h;
    check((c->flags & SPI_DEVICE_BIT_LSBFIRST) != 0, "SPI device is not configured least-significant-bit first");
    *out = (spi_device_handle_t)1;
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t dev, spi_transaction_t *t)
{
    (void)dev;
    const uint8_t *tx = t->tx_buffer;
    uint8_t *rx = t->rx_buffer;
    const size_t bytes = t->length / 8;

    switch (tx[0]) {
    case 0x01: /* data write */
        fake_write(tx + 1, bytes - 1);
        break;
    case 0x02: /* status read */
        if (rx) {
            rx[1] = fake_ready() ? 0x01 : 0x00;
        }
        break;
    case 0x03: /* data read */
        if (rx) {
            memset(rx, 0, bytes);
            fake_read(rx + 1, bytes - 1);
        }
        break;
    default:
        check(false, "unknown SPI opcode 0x%02X", tx[0]);
        break;
    }
    return ESP_OK;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *c, i2c_master_bus_handle_t *out)
{
    (void)c;
    *out = (i2c_master_bus_handle_t)1;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *c,
                                    i2c_master_dev_handle_t *out)
{
    (void)b;
    check(c->device_address == 0x24, "I2C address is 0x%02X, expected 0x24", c->device_address);
    *out = (i2c_master_dev_handle_t)1;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *data, size_t len, int timeout)
{
    (void)d; (void)timeout;
    fake_write(data, len);
    return ESP_OK;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t d, uint8_t *data, size_t len, int timeout)
{
    (void)d; (void)timeout;
    /* Every I2C read is preceded by the ready byte. */
    data[0] = fake_ready() ? 0x01 : 0x00;
    if (len > 1) {
        fake_read(data + 1, len - 1);
    }
    return ESP_OK;
}

/* --- tests ---------------------------------------------------------------- */

static nfc_hw_config_t i2c_cfg(void)
{
    nfc_hw_config_t cfg = {0};
    cfg.chip = NFC_CHIP_PN532;
    cfg.bus = NFC_BUS_I2C;
    cfg.i2c_sda = 21;
    cfg.i2c_scl = 22;
    cfg.i2c_addr = 0x24;
    cfg.i2c_freq_hz = 100000;
    cfg.irq_pin = APP_CFG_PIN_UNSET;
    cfg.rst_pin = APP_CFG_PIN_UNSET;
    return cfg;
}

static nfc_hw_config_t spi_cfg(void)
{
    nfc_hw_config_t cfg = {0};
    cfg.chip = NFC_CHIP_PN532;
    cfg.bus = NFC_BUS_SPI;
    cfg.spi_host = 1;
    cfg.spi_sck = 18;
    cfg.spi_miso = 19;
    cfg.spi_mosi = 23;
    cfg.spi_cs = 5;
    cfg.spi_freq_hz = 1000000;
    cfg.irq_pin = APP_CFG_PIN_UNSET;
    cfg.rst_pin = APP_CFG_PIN_UNSET;
    return cfg;
}

/* The command frame for GetFirmwareVersion is printed in the user manual, so
 * the driver's framing can be compared against a known-good byte sequence
 * rather than against itself. */
static void test_frame_matches_datasheet(void)
{
    printf("frame construction against UM0701-02\n");
    fake_reset();

    nfc_hw_config_t cfg = i2c_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);
    t->init(t->ctx);

    /* The manual prints this exact byte sequence for GetFirmwareVersion, and
     * it is the first thing the driver sends. Comparing against it checks the
     * preamble, both checksums and the byte order in one shot. */
    static const uint8_t expected[] = {0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};
    check(fake.first_frame_len == sizeof(expected), "first frame is %zu bytes, expected %zu",
          fake.first_frame_len, sizeof(expected));
    if (fake.first_frame_len == sizeof(expected)) {
        const bool same = memcmp(fake.first_frame, expected, sizeof(expected)) == 0;
        check(same, "first frame does not match the manual");
        if (!same) {
            printf("    got     :");
            for (size_t i = 0; i < sizeof(expected); i++) printf(" %02X", fake.first_frame[i]);
            printf("\n    expected:");
            for (size_t i = 0; i < sizeof(expected); i++) printf(" %02X", expected[i]);
            printf("\n");
        }
    }
}

static void test_init_and_identity(void)
{
    printf("init: firmware query, SAM and RF configuration\n");
    fake_reset();
    nfc_hw_config_t cfg = i2c_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);

    check(t->init(t->ctx) == ESP_OK, "init failed against a chip that answers");
    /* GetFirmwareVersion, SAMConfiguration, and two RFConfiguration calls. */
    check(fake.commands_seen == 4, "init sent %d commands, expected 4", fake.commands_seen);
}

static void test_init_fails_when_absent(void)
{
    printf("init: nothing wired to the configured pins\n");
    fake_reset();
    fake.deaf = true;

    nfc_hw_config_t cfg = i2c_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);

    /* No chip means no reader, but it must not mean no boot: init reports the
     * failure and app_main carries on without a reader. */
    check(t->init(t->ctx) != ESP_OK, "init claimed success with nothing on the bus");
    check(t->activate(t->ctx) == false, "activate returned a target after a failed init");
}

static void test_activate_and_exchange(void)
{
    printf("activate and APDU exchange\n");
    fake_reset();
    nfc_hw_config_t cfg = i2c_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);
    check(t->init(t->ctx) == ESP_OK, "init failed");

    check(t->activate(t->ctx) == true, "activate found no target");
    check(fake.last_cmd == 0x4A, "activate sent 0x%02X, expected InListPassiveTarget", fake.last_cmd);
    check(fake.last_params[0] == 0x01 && fake.last_params[1] == 0x00,
          "activate asked for %u target(s) at BrTy 0x%02X, expected 1 at 0x00", fake.last_params[0],
          fake.last_params[1]);

    /* A SELECT AID, the first thing an Aliro transaction sends. */
    const uint8_t apdu[] = {0x00, 0xA4, 0x04, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x08, 0x58, 0x01, 0x01, 0x00};
    uint8_t response[64];
    size_t response_len = sizeof(response);

    const esp_err_t err = t->exchange(t->ctx, apdu, sizeof(apdu), response, &response_len);
    check(err == ESP_OK, "exchange failed: 0x%x", err);
    check(fake.last_cmd == 0x40, "exchange sent 0x%02X, expected InDataExchange", fake.last_cmd);
    check(fake.last_params[0] == 0x01, "exchange addressed target %u, expected 1", fake.last_params[0]);

    /* The fake echoes the APDU, so a correct driver strips the status byte and
     * returns exactly what was sent. */
    check(response_len == sizeof(apdu), "got %zu bytes back, sent %zu", response_len, sizeof(apdu));
    check(memcmp(response, apdu, sizeof(apdu)) == 0, "response payload does not match what was sent");

    t->deactivate(t->ctx);
    check(fake.last_cmd == 0x52, "deactivate sent 0x%02X, expected InRelease", fake.last_cmd);
}

static void test_spi_path(void)
{
    printf("the same exchange over SPI\n");
    fake_reset();
    nfc_hw_config_t cfg = spi_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);

    check(t->init(t->ctx) == ESP_OK, "SPI init failed");
    check(t->activate(t->ctx) == true, "SPI activate found no target");

    const uint8_t apdu[] = {0x00, 0xA4, 0x04, 0x00};
    uint8_t response[32];
    size_t response_len = sizeof(response);
    check(t->exchange(t->ctx, apdu, sizeof(apdu), response, &response_len) == ESP_OK, "SPI exchange failed");
    check(response_len == sizeof(apdu) && memcmp(response, apdu, sizeof(apdu)) == 0, "SPI payload round-trip failed");
}

static void test_no_target(void)
{
    printf("an empty field is not an error\n");
    fake_reset();
    nfc_hw_config_t cfg = i2c_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);
    check(t->init(t->ctx) == ESP_OK, "init failed");

    fake.no_target = true;
    check(t->activate(t->ctx) == false, "activate claimed a target in an empty field");
}

static void test_rejects_non_iso14443_4(void)
{
    printf("a card that cannot do ISO 14443-4 is refused\n");
    fake_reset();
    nfc_hw_config_t cfg = i2c_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);
    check(t->init(t->ctx) == ESP_OK, "init failed");

    fake.sak = 0x08; /* MIFARE Classic: no ISO-DEP */
    check(t->activate(t->ctx) == false, "activate accepted a target with SAK 0x08");
}

static void test_exchange_error_drops_target(void)
{
    printf("a target error ends the session rather than being ignored\n");
    fake_reset();
    nfc_hw_config_t cfg = i2c_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);
    check(t->init(t->ctx) == ESP_OK, "init failed");
    check(t->activate(t->ctx) == true, "activate found no target");

    fake.exchange_status = 0x01; /* time out */
    const uint8_t apdu[] = {0x00, 0xA4};
    uint8_t response[16];
    size_t response_len = sizeof(response);
    check(t->exchange(t->ctx, apdu, sizeof(apdu), response, &response_len) != ESP_OK,
          "a non-zero status byte was reported as success");

    /* The link is gone, so a further exchange must be refused outright. */
    response_len = sizeof(response);
    check(t->exchange(t->ctx, apdu, sizeof(apdu), response, &response_len) == ESP_ERR_INVALID_STATE,
          "driver kept using a target that had already failed");
}

static void test_corrupt_checksum_is_caught(void)
{
    printf("a corrupted response is rejected, not parsed\n");
    fake_reset();
    nfc_hw_config_t cfg = i2c_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);
    check(t->init(t->ctx) == ESP_OK, "init failed");

    fake.bad_data_checksum = true;
    check(t->activate(t->ctx) == false, "a frame with a bad data checksum was accepted");
}

static void test_oversized_apdu_refused(void)
{
    printf("an APDU too long for one frame is refused\n");
    fake_reset();
    nfc_hw_config_t cfg = i2c_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);
    check(t->init(t->ctx) == ESP_OK, "init failed");
    check(t->activate(t->ctx) == true, "activate found no target");

    static uint8_t huge[300];
    uint8_t response[16];
    size_t response_len = sizeof(response);
    check(t->exchange(t->ctx, huge, sizeof(huge), response, &response_len) == ESP_ERR_INVALID_SIZE,
          "a 300-byte APDU was not refused");
}

static void test_small_response_buffer(void)
{
    printf("a response that does not fit is reported, not truncated\n");
    fake_reset();
    nfc_hw_config_t cfg = i2c_cfg();
    const nfc_transport_t *t = nfc_transport_pn532(&cfg);
    check(t->init(t->ctx) == ESP_OK, "init failed");
    check(t->activate(t->ctx) == true, "activate found no target");

    const uint8_t apdu[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t response[4];
    size_t response_len = sizeof(response);
    check(t->exchange(t->ctx, apdu, sizeof(apdu), response, &response_len) == ESP_ERR_INVALID_SIZE,
          "an oversized response was written into a small buffer");
}

int main(void)
{
    test_frame_matches_datasheet();
    test_init_and_identity();
    test_init_fails_when_absent();
    test_activate_and_exchange();
    test_spi_path();
    test_no_target();
    test_rejects_non_iso14443_4();
    test_exchange_error_drops_target();
    test_corrupt_checksum_is_caught();
    test_oversized_apdu_refused();
    test_small_response_buffer();

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
