/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "gpio_rules.h"

#include <cJSON.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#include <soc/soc_caps.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_tag = "aliro/config";
static const char *const k_nvs_namespace = "aliro";
static const char *const k_nvs_key = "config";

static app_config_t s_config;
static bool s_loaded;

/* --- enum <-> string ----------------------------------------------------- */

static const char *chip_name(nfc_chip_t c)
{
    switch (c) {
    case NFC_CHIP_PN532:
        return "pn532";
    case NFC_CHIP_PN7160:
        return "pn7160";
    case NFC_CHIP_ST25R3916:
        return "st25r3916";
    default:
        return "none";
    }
}

static nfc_chip_t chip_from_name(const char *s, nfc_chip_t fallback)
{
    if (!s) {
        return fallback;
    }
    if (strcmp(s, "pn532") == 0) {
        return NFC_CHIP_PN532;
    }
    if (strcmp(s, "pn7160") == 0) {
        return NFC_CHIP_PN7160;
    }
    if (strcmp(s, "st25r3916") == 0) {
        return NFC_CHIP_ST25R3916;
    }
    if (strcmp(s, "none") == 0) {
        return NFC_CHIP_NONE;
    }
    return fallback;
}

static const char *bus_name(nfc_bus_t b)
{
    switch (b) {
    case NFC_BUS_SPI:
        return "spi";
    case NFC_BUS_I2C:
        return "i2c";
    default:
        return "none";
    }
}

static nfc_bus_t bus_from_name(const char *s, nfc_bus_t fallback)
{
    if (!s) {
        return fallback;
    }
    if (strcmp(s, "spi") == 0) {
        return NFC_BUS_SPI;
    }
    if (strcmp(s, "i2c") == 0) {
        return NFC_BUS_I2C;
    }
    if (strcmp(s, "none") == 0) {
        return NFC_BUS_NONE;
    }
    return fallback;
}

/* --- defaults ------------------------------------------------------------ */

void app_config_defaults(app_config_t *out)
{
    memset(out, 0, sizeof(*out));

    snprintf(out->device_name, sizeof(out->device_name), "%s", CONFIG_ALIRO_DEFAULT_DEVICE_NAME);
    snprintf(out->group_id_hex, sizeof(out->group_id_hex), "%s", CONFIG_ALIRO_READER_GROUP_ID);

    out->nfc.chip = NFC_CHIP_NONE;
    out->nfc.bus = NFC_BUS_SPI;
    out->nfc.spi_host = (SOC_SPI_PERIPH_NUM > 2) ? 2 : 1;
    out->nfc.spi_sck = CONFIG_ALIRO_NFC_SPI_SCK;
    out->nfc.spi_miso = CONFIG_ALIRO_NFC_SPI_MISO;
    out->nfc.spi_mosi = CONFIG_ALIRO_NFC_SPI_MOSI;
    out->nfc.spi_cs = CONFIG_ALIRO_NFC_SPI_CS;
    out->nfc.spi_freq_hz = 1000000;
    out->nfc.i2c_sda = APP_CFG_PIN_UNSET;
    out->nfc.i2c_scl = APP_CFG_PIN_UNSET;
    out->nfc.i2c_freq_hz = 400000;
    out->nfc.i2c_addr = 0x24;
    out->nfc.irq_pin = CONFIG_ALIRO_NFC_IRQ;
    out->nfc.rst_pin = CONFIG_ALIRO_NFC_RST;

    out->lock.gpio = CONFIG_ALIRO_LOCK_GPIO;
#ifdef CONFIG_ALIRO_LOCK_ACTIVE_LOW
    /* A bool Kconfig that is 'n' is not defined at all, so it cannot be read
     * as a value -- it has to be tested with #ifdef. */
    out->lock.active_low = true;
#else
    out->lock.active_low = false;
#endif
    out->lock.unlock_ms = CONFIG_ALIRO_LOCK_UNLOCK_MS;

    snprintf(out->net.ap_password, sizeof(out->net.ap_password), "%s", CONFIG_ALIRO_AP_PASSWORD);
    snprintf(out->net.hostname, sizeof(out->net.hostname), "%s", CONFIG_ALIRO_DEFAULT_DEVICE_NAME);

    out->mqtt.enabled = false;
    out->mqtt.port = 1883;
    out->mqtt.publish_taps = true;
    out->mqtt.ha_discovery = true;
    snprintf(out->mqtt.client_id, sizeof(out->mqtt.client_id), "%s", CONFIG_ALIRO_DEFAULT_DEVICE_NAME);
    snprintf(out->mqtt.base_topic, sizeof(out->mqtt.base_topic), "aliro/%s", CONFIG_ALIRO_DEFAULT_DEVICE_NAME);
}

void app_config_mqtt_topic(const mqtt_config_t *cfg, const char *suffix, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s", cfg->base_topic, suffix);
}

/* --- validation ---------------------------------------------------------- */

#define FAIL(fmt, ...)                                                                                                 \
    do {                                                                                                               \
        if (err_msg) {                                                                                                 \
            snprintf(err_msg, err_len, fmt, ##__VA_ARGS__);                                                            \
        }                                                                                                              \
        return ESP_ERR_INVALID_ARG;                                                                                    \
    } while (0)

typedef struct {
    int pin;
    const char *label;
} pin_use_t;

static esp_err_t check_pin(int pin, const char *label, bool output, char *err_msg, size_t err_len)
{
    if (pin == APP_CFG_PIN_UNSET) {
        return ESP_OK;
    }
    const char *reason = gpio_rules_reject_reason(pin, output);
    if (reason) {
        FAIL("%s: GPIO %d is %s", label, pin, reason);
    }
    return ESP_OK;
}

/** @brief Bail out of validate() when a pin check fails, without re-logging. */
#define CHECK_PIN(pin, label, output)                                                                                  \
    do {                                                                                                               \
        const esp_err_t _err = check_pin((pin), (label), (output), err_msg, err_len);                                   \
        if (_err != ESP_OK) {                                                                                          \
            return _err;                                                                                               \
        }                                                                                                              \
    } while (0)

esp_err_t app_config_validate(const app_config_t *cfg, char *err_msg, size_t err_len)
{
    if (!cfg) {
        FAIL("no configuration");
    }

    if (strlen(cfg->group_id_hex) != 32) {
        FAIL("reader group identifier must be exactly 32 hex characters");
    }
    uint8_t group_id[16];
    if (app_config_parse_group_id(cfg->group_id_hex, group_id, sizeof(group_id)) != ESP_OK) {
        FAIL("reader group identifier is not valid hex");
    }

    if (cfg->device_name[0] == '\0') {
        FAIL("device name must not be empty");
    }

    if (cfg->lock.unlock_ms < 100 || cfg->lock.unlock_ms > 60000) {
        FAIL("unlock duration must be between 100 and 60000 ms");
    }
    CHECK_PIN(cfg->lock.gpio, "lock output", true);
    if (cfg->lock.gpio == APP_CFG_PIN_UNSET) {
        FAIL("lock output pin must be set");
    }

    /* Collect every pin in use so two functions cannot claim the same one. */
    pin_use_t used[8];
    size_t used_count = 0;
    used[used_count++] = (pin_use_t){cfg->lock.gpio, "lock output"};

    if (cfg->nfc.bus == NFC_BUS_SPI) {
        if (cfg->nfc.spi_host != 1 && cfg->nfc.spi_host != 2) {
            FAIL("SPI host must be 1 (SPI2) or 2 (SPI3)");
        }
        if (cfg->nfc.spi_host == 2 && SOC_SPI_PERIPH_NUM <= 2) {
            FAIL("this chip has no SPI3 host");
        }
        if (cfg->nfc.spi_freq_hz < 100000 || cfg->nfc.spi_freq_hz > 20000000) {
            FAIL("SPI clock must be between 100 kHz and 20 MHz");
        }
        const struct {
            int pin;
            const char *label;
            bool output;
        } spi_pins[] = {
            {cfg->nfc.spi_sck, "SPI SCK", true},
            {cfg->nfc.spi_miso, "SPI MISO", false},
            {cfg->nfc.spi_mosi, "SPI MOSI", true},
            {cfg->nfc.spi_cs, "SPI CS", true},
        };
        for (size_t i = 0; i < sizeof(spi_pins) / sizeof(spi_pins[0]); i++) {
            if (spi_pins[i].pin == APP_CFG_PIN_UNSET) {
                FAIL("%s must be set when the NFC bus is SPI", spi_pins[i].label);
            }
            CHECK_PIN(spi_pins[i].pin, spi_pins[i].label, spi_pins[i].output);
            used[used_count++] = (pin_use_t){spi_pins[i].pin, spi_pins[i].label};
        }
    } else if (cfg->nfc.bus == NFC_BUS_I2C) {
        if (cfg->nfc.i2c_sda == APP_CFG_PIN_UNSET || cfg->nfc.i2c_scl == APP_CFG_PIN_UNSET) {
            FAIL("SDA and SCL must be set when the NFC bus is I2C");
        }
        if (cfg->nfc.i2c_freq_hz < 50000 || cfg->nfc.i2c_freq_hz > 1000000) {
            FAIL("I2C clock must be between 50 kHz and 1 MHz");
        }
        if (cfg->nfc.i2c_addr > 0x7F) {
            FAIL("I2C address must be a 7-bit value");
        }
        CHECK_PIN(cfg->nfc.i2c_sda, "I2C SDA", true);
        CHECK_PIN(cfg->nfc.i2c_scl, "I2C SCL", true);
        used[used_count++] = (pin_use_t){cfg->nfc.i2c_sda, "I2C SDA"};
        used[used_count++] = (pin_use_t){cfg->nfc.i2c_scl, "I2C SCL"};
    }

    CHECK_PIN(cfg->nfc.irq_pin, "NFC IRQ", false);
    CHECK_PIN(cfg->nfc.rst_pin, "NFC reset", true);
    if (cfg->nfc.irq_pin != APP_CFG_PIN_UNSET) {
        used[used_count++] = (pin_use_t){cfg->nfc.irq_pin, "NFC IRQ"};
    }
    if (cfg->nfc.rst_pin != APP_CFG_PIN_UNSET) {
        used[used_count++] = (pin_use_t){cfg->nfc.rst_pin, "NFC reset"};
    }

    for (size_t i = 0; i < used_count; i++) {
        for (size_t j = i + 1; j < used_count; j++) {
            if (used[i].pin == used[j].pin) {
                FAIL("GPIO %d is assigned to both %s and %s", used[i].pin, used[i].label, used[j].label);
            }
        }
    }

    const size_t ap_len = strlen(cfg->net.ap_password);
    if (ap_len > 0 && ap_len < 8) {
        FAIL("access point password must be at least 8 characters, or empty for an open network");
    }
    if (cfg->net.hostname[0] == '\0') {
        FAIL("hostname must not be empty");
    }

    if (cfg->mqtt.enabled) {
        if (cfg->mqtt.broker[0] == '\0') {
            FAIL("MQTT is enabled but no broker address is set");
        }
        if (cfg->mqtt.port == 0) {
            FAIL("MQTT port must be between 1 and 65535");
        }
        if (cfg->mqtt.client_id[0] == '\0') {
            FAIL("MQTT client ID must not be empty");
        }
        if (cfg->mqtt.base_topic[0] == '\0') {
            FAIL("MQTT base topic must not be empty");
        }
        /* A trailing slash would produce "base//lock/set", which some brokers
         * accept and no human expects. */
        if (cfg->mqtt.base_topic[strlen(cfg->mqtt.base_topic) - 1] == '/') {
            FAIL("MQTT base topic must not end with '/'");
        }
        if (strchr(cfg->mqtt.base_topic, '#') || strchr(cfg->mqtt.base_topic, '+')) {
            FAIL("MQTT base topic must not contain wildcards");
        }
    }

    return ESP_OK;
}

#undef FAIL
#undef CHECK_PIN

/* --- JSON ---------------------------------------------------------------- */

char *app_config_to_json(const app_config_t *cfg, bool include_secrets)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "name", cfg->device_name);
    cJSON_AddStringToObject(device, "group_id", cfg->group_id_hex);

    cJSON *nfc = cJSON_AddObjectToObject(root, "nfc");
    cJSON_AddStringToObject(nfc, "chip", chip_name(cfg->nfc.chip));
    cJSON_AddStringToObject(nfc, "bus", bus_name(cfg->nfc.bus));
    cJSON_AddNumberToObject(nfc, "spi_host", cfg->nfc.spi_host);
    cJSON_AddNumberToObject(nfc, "spi_sck", cfg->nfc.spi_sck);
    cJSON_AddNumberToObject(nfc, "spi_miso", cfg->nfc.spi_miso);
    cJSON_AddNumberToObject(nfc, "spi_mosi", cfg->nfc.spi_mosi);
    cJSON_AddNumberToObject(nfc, "spi_cs", cfg->nfc.spi_cs);
    cJSON_AddNumberToObject(nfc, "spi_freq_hz", cfg->nfc.spi_freq_hz);
    cJSON_AddNumberToObject(nfc, "i2c_sda", cfg->nfc.i2c_sda);
    cJSON_AddNumberToObject(nfc, "i2c_scl", cfg->nfc.i2c_scl);
    cJSON_AddNumberToObject(nfc, "i2c_freq_hz", cfg->nfc.i2c_freq_hz);
    cJSON_AddNumberToObject(nfc, "i2c_addr", cfg->nfc.i2c_addr);
    cJSON_AddNumberToObject(nfc, "irq_pin", cfg->nfc.irq_pin);
    cJSON_AddNumberToObject(nfc, "rst_pin", cfg->nfc.rst_pin);

    cJSON *lock = cJSON_AddObjectToObject(root, "lock");
    cJSON_AddNumberToObject(lock, "gpio", cfg->lock.gpio);
    cJSON_AddBoolToObject(lock, "active_low", cfg->lock.active_low);
    cJSON_AddNumberToObject(lock, "unlock_ms", cfg->lock.unlock_ms);

    cJSON *net = cJSON_AddObjectToObject(root, "net");
    cJSON_AddStringToObject(net, "ssid", cfg->net.ssid);
    cJSON_AddStringToObject(net, "hostname", cfg->net.hostname);
    /* Passwords leave the device only when it is talking to NVS, never to a
     * browser. The UI sends an empty string to mean "keep the stored one". */
    cJSON_AddStringToObject(net, "password", include_secrets ? cfg->net.password : "");
    cJSON_AddStringToObject(net, "ap_password", include_secrets ? cfg->net.ap_password : "");
    cJSON_AddBoolToObject(net, "password_set", cfg->net.password[0] != '\0');

    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", cfg->mqtt.enabled);
    cJSON_AddStringToObject(mqtt, "broker", cfg->mqtt.broker);
    cJSON_AddNumberToObject(mqtt, "port", cfg->mqtt.port);
    cJSON_AddStringToObject(mqtt, "username", cfg->mqtt.username);
    cJSON_AddStringToObject(mqtt, "client_id", cfg->mqtt.client_id);
    cJSON_AddStringToObject(mqtt, "base_topic", cfg->mqtt.base_topic);
    cJSON_AddBoolToObject(mqtt, "use_ssl", cfg->mqtt.use_ssl);
    cJSON_AddBoolToObject(mqtt, "allow_insecure", cfg->mqtt.allow_insecure);
    cJSON_AddBoolToObject(mqtt, "ha_discovery", cfg->mqtt.ha_discovery);
    cJSON_AddBoolToObject(mqtt, "publish_taps", cfg->mqtt.publish_taps);
    cJSON_AddStringToObject(mqtt, "password", include_secrets ? cfg->mqtt.password : "");
    cJSON_AddBoolToObject(mqtt, "password_set", cfg->mqtt.password[0] != '\0');

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static void json_get_string(const cJSON *obj, const char *key, char *dst, size_t dst_len, bool keep_when_empty)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return;
    }
    if (keep_when_empty && item->valuestring[0] == '\0') {
        return;
    }
    snprintf(dst, dst_len, "%s", item->valuestring);
}

static void json_get_int(const cJSON *obj, const char *key, int *dst)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        *dst = item->valueint;
    }
}

static void json_get_i8(const cJSON *obj, const char *key, int8_t *dst)
{
    int tmp = *dst;
    json_get_int(obj, key, &tmp);
    *dst = (int8_t)tmp;
}

static void json_get_bool(const cJSON *obj, const char *key, bool *dst)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
    }
}

esp_err_t app_config_from_json(const char *json, app_config_t *cfg, char *err_msg, size_t err_len)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        if (err_msg) {
            snprintf(err_msg, err_len, "malformed JSON");
        }
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (cJSON_IsObject(device)) {
        json_get_string(device, "name", cfg->device_name, sizeof(cfg->device_name), false);
        json_get_string(device, "group_id", cfg->group_id_hex, sizeof(cfg->group_id_hex), false);
    }

    const cJSON *nfc = cJSON_GetObjectItemCaseSensitive(root, "nfc");
    if (cJSON_IsObject(nfc)) {
        const cJSON *chip = cJSON_GetObjectItemCaseSensitive(nfc, "chip");
        if (cJSON_IsString(chip)) {
            cfg->nfc.chip = chip_from_name(chip->valuestring, cfg->nfc.chip);
        }
        const cJSON *bus = cJSON_GetObjectItemCaseSensitive(nfc, "bus");
        if (cJSON_IsString(bus)) {
            cfg->nfc.bus = bus_from_name(bus->valuestring, cfg->nfc.bus);
        }
        int host = cfg->nfc.spi_host;
        json_get_int(nfc, "spi_host", &host);
        cfg->nfc.spi_host = (uint8_t)host;
        json_get_i8(nfc, "spi_sck", &cfg->nfc.spi_sck);
        json_get_i8(nfc, "spi_miso", &cfg->nfc.spi_miso);
        json_get_i8(nfc, "spi_mosi", &cfg->nfc.spi_mosi);
        json_get_i8(nfc, "spi_cs", &cfg->nfc.spi_cs);
        json_get_i8(nfc, "irq_pin", &cfg->nfc.irq_pin);
        json_get_i8(nfc, "rst_pin", &cfg->nfc.rst_pin);
        json_get_i8(nfc, "i2c_sda", &cfg->nfc.i2c_sda);
        json_get_i8(nfc, "i2c_scl", &cfg->nfc.i2c_scl);
        int freq = (int)cfg->nfc.spi_freq_hz;
        json_get_int(nfc, "spi_freq_hz", &freq);
        cfg->nfc.spi_freq_hz = (uint32_t)freq;
        freq = (int)cfg->nfc.i2c_freq_hz;
        json_get_int(nfc, "i2c_freq_hz", &freq);
        cfg->nfc.i2c_freq_hz = (uint32_t)freq;
        int addr = cfg->nfc.i2c_addr;
        json_get_int(nfc, "i2c_addr", &addr);
        cfg->nfc.i2c_addr = (uint8_t)addr;
    }

    const cJSON *lock = cJSON_GetObjectItemCaseSensitive(root, "lock");
    if (cJSON_IsObject(lock)) {
        json_get_i8(lock, "gpio", &cfg->lock.gpio);
        json_get_bool(lock, "active_low", &cfg->lock.active_low);
        int unlock_ms = (int)cfg->lock.unlock_ms;
        json_get_int(lock, "unlock_ms", &unlock_ms);
        cfg->lock.unlock_ms = (uint32_t)(unlock_ms < 0 ? 0 : unlock_ms);
    }

    const cJSON *net = cJSON_GetObjectItemCaseSensitive(root, "net");
    if (cJSON_IsObject(net)) {
        json_get_string(net, "ssid", cfg->net.ssid, sizeof(cfg->net.ssid), false);
        json_get_string(net, "hostname", cfg->net.hostname, sizeof(cfg->net.hostname), false);
        /* Empty means "unchanged", so a UI that never saw the password cannot
         * accidentally erase it. */
        json_get_string(net, "password", cfg->net.password, sizeof(cfg->net.password), true);
        json_get_string(net, "ap_password", cfg->net.ap_password, sizeof(cfg->net.ap_password), true);
    }

    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        json_get_bool(mqtt, "enabled", &cfg->mqtt.enabled);
        json_get_bool(mqtt, "use_ssl", &cfg->mqtt.use_ssl);
        json_get_bool(mqtt, "allow_insecure", &cfg->mqtt.allow_insecure);
        json_get_bool(mqtt, "ha_discovery", &cfg->mqtt.ha_discovery);
        json_get_bool(mqtt, "publish_taps", &cfg->mqtt.publish_taps);
        json_get_string(mqtt, "broker", cfg->mqtt.broker, sizeof(cfg->mqtt.broker), false);
        json_get_string(mqtt, "username", cfg->mqtt.username, sizeof(cfg->mqtt.username), false);
        json_get_string(mqtt, "client_id", cfg->mqtt.client_id, sizeof(cfg->mqtt.client_id), false);
        json_get_string(mqtt, "base_topic", cfg->mqtt.base_topic, sizeof(cfg->mqtt.base_topic), false);
        json_get_string(mqtt, "password", cfg->mqtt.password, sizeof(cfg->mqtt.password), true);
        int port = cfg->mqtt.port;
        json_get_int(mqtt, "port", &port);
        cfg->mqtt.port = (port < 0 || port > 65535) ? 0 : (uint16_t)port;
    }

    cJSON_Delete(root);
    return app_config_validate(cfg, err_msg, err_len);
}

char *app_config_hardware_caps_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "target", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(root, "max_pin", gpio_rules_max_pin());
    cJSON_AddNumberToObject(root, "spi_host_count", SOC_SPI_PERIPH_NUM);

    size_t count = 0;
    const uint8_t *restricted = gpio_rules_restricted(&count);
    cJSON *r = cJSON_AddArrayToObject(root, "restricted_pins");
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(r, cJSON_CreateNumber(restricted[i]));
    }

    const uint8_t *strapping = gpio_rules_strapping(&count);
    cJSON *s = cJSON_AddArrayToObject(root, "strapping_pins");
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(s, cJSON_CreateNumber(strapping[i]));
    }

    cJSON *inputs_only = cJSON_AddArrayToObject(root, "input_only_pins");
    for (int pin = 0; pin <= gpio_rules_max_pin(); pin++) {
        if (gpio_rules_is_valid_input(pin) && !gpio_rules_is_valid_output(pin)) {
            cJSON_AddItemToArray(inputs_only, cJSON_CreateNumber(pin));
        }
    }

    cJSON *usable = cJSON_AddArrayToObject(root, "usable_pins");
    for (int pin = 0; pin <= gpio_rules_max_pin(); pin++) {
        if (gpio_rules_is_valid_input(pin) && !gpio_rules_is_restricted(pin)) {
            cJSON_AddItemToArray(usable, cJSON_CreateNumber(pin));
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* --- persistence --------------------------------------------------------- */

static esp_err_t load_from_nvs(app_config_t *cfg)
{
    /* On a blank NVS the namespace does not exist yet. That is what a first
     * boot looks like, so it is reported quietly and the caller falls back to
     * defaults. */
    nvs_handle_t handle;
    const esp_err_t open_err = nvs_open(k_nvs_namespace, NVS_READONLY, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(open_err, k_tag, "nvs_open failed");

    size_t len = 0;
    esp_err_t err = nvs_get_str(handle, k_nvs_key, NULL, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    char *json = malloc(len);
    if (!json) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(handle, k_nvs_key, json, &len);
    nvs_close(handle);

    if (err == ESP_OK) {
        char reason[128] = {0};
        err = app_config_from_json(json, cfg, reason, sizeof(reason));
        if (err != ESP_OK) {
            ESP_LOGE(k_tag, "stored config rejected (%s); falling back to defaults", reason);
        }
    }
    free(json);
    return err;
}

static esp_err_t store_to_nvs(const app_config_t *cfg)
{
    char *json = app_config_to_json(cfg, true);
    ESP_RETURN_ON_FALSE(json, ESP_ERR_NO_MEM, k_tag, "config serialization failed");

    nvs_handle_t handle;
    esp_err_t err = nvs_open(k_nvs_namespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, k_nvs_key, json);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    free(json);
    return err;
}

esp_err_t app_config_init(void)
{
    app_config_defaults(&s_config);

    const esp_err_t err = load_from_nvs(&s_config);
    if (err == ESP_OK) {
        ESP_LOGI(k_tag, "configuration loaded from NVS");
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(k_tag, "no stored configuration, using defaults");
    } else {
        ESP_LOGW(k_tag, "config load failed (%s), using defaults", esp_err_to_name(err));
        app_config_defaults(&s_config);
    }

    /* A device with no hostname of its own is hard to find on a network with
     * several of them. */
    if (strcmp(s_config.net.hostname, CONFIG_ALIRO_DEFAULT_DEVICE_NAME) == 0) {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(s_config.net.hostname, sizeof(s_config.net.hostname), "%s-%02X%02X",
                     CONFIG_ALIRO_DEFAULT_DEVICE_NAME, mac[4], mac[5]);
        }
    }

    s_loaded = true;
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    if (!s_loaded) {
        app_config_defaults(&s_config);
        s_loaded = true;
    }
    return &s_config;
}

esp_err_t app_config_save(const app_config_t *cfg, char *err_msg, size_t err_len)
{
    ESP_RETURN_ON_ERROR(app_config_validate(cfg, err_msg, err_len), k_tag, "config rejected");
    ESP_RETURN_ON_ERROR(store_to_nvs(cfg), k_tag, "config store failed");

    s_config = *cfg;
    s_loaded = true;
    ESP_LOGI(k_tag, "configuration saved");
    return ESP_OK;
}

esp_err_t app_config_reset(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(k_nvs_namespace, NVS_READWRITE, &handle), k_tag, "nvs_open failed");

    esp_err_t err = nvs_erase_key(handle, k_nvs_key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        app_config_defaults(&s_config);
        s_loaded = true;
        ESP_LOGW(k_tag, "configuration reset to defaults");
    }
    return err;
}

esp_err_t app_config_parse_group_id(const char *hex, uint8_t *out, size_t out_len)
{
    if (!hex || !out || strlen(hex) != out_len * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < out_len * 2; i++) {
        if (!isxdigit((unsigned char)hex[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte = 0;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)byte;
    }
    return ESP_OK;
}
