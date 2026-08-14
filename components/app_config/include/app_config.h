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
 * @brief Runtime configuration, owned here and edited over the web UI.
 *
 * Kconfig supplies the compile-time defaults; this is what the device is
 * actually running. It lives in NVS as a JSON document so that adding a field
 * does not invalidate a stored config.
 */

/** @brief Unset pin. */
#define APP_CFG_PIN_UNSET (-1)

typedef enum {
    NFC_CHIP_NONE = 0,
    NFC_CHIP_PN532,
    NFC_CHIP_PN7160,
    NFC_CHIP_ST25R3916,
} nfc_chip_t;

typedef enum {
    NFC_BUS_NONE = 0,
    NFC_BUS_SPI,
    NFC_BUS_I2C,
} nfc_bus_t;

typedef struct {
    nfc_chip_t chip;
    nfc_bus_t bus;

    /* SPI */
    uint8_t spi_host;     /*!< 1 = SPI2_HOST, 2 = SPI3_HOST */
    int8_t spi_sck;
    int8_t spi_miso;
    int8_t spi_mosi;
    int8_t spi_cs;
    uint32_t spi_freq_hz;

    /* I2C */
    int8_t i2c_sda;
    int8_t i2c_scl;
    uint32_t i2c_freq_hz;
    uint8_t i2c_addr;

    /* Both */
    int8_t irq_pin;
    int8_t rst_pin; /*!< RSTPD_N / VEN / reset, chip depending */
} nfc_hw_config_t;

typedef struct {
    int8_t gpio;
    bool active_low;
    /* Wider than the 60 s it accepts, so an out-of-range value from the API
     * is rejected by validation instead of silently wrapping into range. */
    uint32_t unlock_ms;
} lock_config_t;

typedef struct {
    char ssid[33];
    char password[65];
    char ap_password[65]; /*!< AP mode password, >= 8 chars or empty for open */
    char hostname[32];
} net_config_t;

/**
 * @brief MQTT, trimmed from HomeKey-ESP32's set to what this project has.
 *
 * Topics are derived from @c base_topic rather than configured one by one:
 * there is no HomeKit state machine here, so the per-topic overrides that
 * project needs (current/target state, battery, alt action) have nothing to
 * map onto.
 */
typedef struct {
    bool enabled;
    char broker[64]; /*!< Hostname or IP of the broker */
    uint16_t port;
    char username[33];
    char password[65];
    char client_id[33];
    char base_topic[49]; /*!< Everything is published under this prefix */
    bool use_ssl;
    bool allow_insecure; /*!< Skip broker certificate verification */
    bool ha_discovery;   /*!< Publish Home Assistant discovery documents */
    bool publish_taps;   /*!< Publish an event for every tap, granted or not */
} mqtt_config_t;

typedef struct {
    char device_name[32];
    char group_id_hex[33]; /*!< 32 hex chars, the 16-byte reader group identifier */
    nfc_hw_config_t nfc;
    lock_config_t lock;
    net_config_t net;
    mqtt_config_t mqtt;
} app_config_t;

/**
 * @brief Build a full topic from the configured base.
 *
 * @param[in]  cfg    MQTT configuration
 * @param[in]  suffix Topic suffix, e.g. "lock/set"
 * @param[out] out    Destination buffer
 * @param[in]  out_len Size of @p out
 */
void app_config_mqtt_topic(const mqtt_config_t *cfg, const char *suffix, char *out, size_t out_len);

/** @brief Load config from NVS, falling back to Kconfig defaults. */
esp_err_t app_config_init(void);

/** @brief The running configuration. Never NULL after app_config_init(). */
const app_config_t *app_config_get(void);

/** @brief Fill @p out with the compile-time defaults. */
void app_config_defaults(app_config_t *out);

/**
 * @brief Validate and persist a new configuration.
 *
 * Most fields only take effect after a restart; the caller decides when to
 * reboot.
 *
 * @param[in]  cfg     Candidate configuration
 * @param[out] err_msg Human-readable reason on failure, may be NULL
 * @param[in]  err_len Size of @p err_msg
 */
esp_err_t app_config_save(const app_config_t *cfg, char *err_msg, size_t err_len);

/** @brief Erase the stored config; defaults apply after restart. */
esp_err_t app_config_reset(void);

/**
 * @brief Check a configuration without storing it.
 *
 * Rejects pins that do not exist on this chip, pins reserved for flash/PSRAM,
 * pins used twice, and out-of-range values.
 */
esp_err_t app_config_validate(const app_config_t *cfg, char *err_msg, size_t err_len);

/**
 * @brief Serialize to JSON.
 *
 * @param[in] cfg             Configuration to serialize
 * @param[in] include_secrets When false, passwords are emitted as empty
 *                            strings. The web UI never receives them.
 * @return Heap-allocated JSON string the caller frees, or NULL.
 */
char *app_config_to_json(const app_config_t *cfg, bool include_secrets);

/**
 * @brief Merge a JSON document into @p cfg.
 *
 * Missing keys keep their current value, and an empty password string keeps
 * the stored password, so the UI can round-trip a masked config safely.
 */
esp_err_t app_config_from_json(const char *json, app_config_t *cfg, char *err_msg, size_t err_len);

/** @brief JSON description of this chip's usable pins, for the web UI. */
char *app_config_hardware_caps_json(void);

/**
 * @brief Read a provisioned PEM key out of NVS.
 *
 * The browser flasher in site/ writes a per-device reader identity into the
 * @c aliro namespace under @c rdr_pub, @c rdr_priv and @c cred_pub. When a key
 * is absent the caller falls back to the development identity compiled into
 * the application, so an unprovisioned board still boots and runs.
 *
 * @param[in] key NVS key name
 * @return Heap-allocated NUL-terminated PEM the caller frees, or NULL.
 */
char *app_config_load_pem(const char *key);

/** @brief Parse the 32-char hex group identifier into 16 bytes. */
esp_err_t app_config_parse_group_id(const char *hex, uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif
