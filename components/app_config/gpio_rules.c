/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpio_rules.h"

#include <driver/gpio.h>
#include <sdkconfig.h>
#include <soc/gpio_num.h>
#include <soc/soc_caps.h>

/* Pin tables adapted from HomeKey-ESP32 (MIT, (c) rednblkx). */
#if CONFIG_IDF_TARGET_ESP32
static const uint8_t k_restricted[] = {6, 7, 8, 9, 10, 11, 16, 17};
static const uint8_t k_strapping[] = {0, 2, 4, 5, 12, 15};
#elif CONFIG_IDF_TARGET_ESP32S3
#if defined(CONFIG_ESPTOOLPY_OCT_FLASH) || defined(CONFIG_SPIRAM_MODE_OCT)
static const uint8_t k_restricted[] = {9, 10, 11, 12, 13, 14, 19, 20, 33, 34, 35, 36, 37, 38, 39};
#else
static const uint8_t k_restricted[] = {9, 10, 11, 12, 13, 14, 19, 20, 38, 39};
#endif
static const uint8_t k_strapping[] = {0, 3, 43, 44, 45, 46, 47, 48};
#elif CONFIG_IDF_TARGET_ESP32S2
static const uint8_t k_restricted[] = {22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
static const uint8_t k_strapping[] = {0, 45, 46};
#elif CONFIG_IDF_TARGET_ESP32C3
static const uint8_t k_restricted[] = {12, 13, 14, 15, 16, 17};
static const uint8_t k_strapping[] = {2, 8, 9};
#elif CONFIG_IDF_TARGET_ESP32C6
static const uint8_t k_restricted[] = {24, 25, 26, 27, 28, 29, 30};
static const uint8_t k_strapping[] = {4, 5, 8, 9, 15};
#elif CONFIG_IDF_TARGET_ESP32H2
static const uint8_t k_restricted[] = {6, 7, 15, 16, 17, 18, 19, 20, 21};
static const uint8_t k_strapping[] = {2, 3, 8, 9, 25};
#else
static const uint8_t k_restricted[] = {0};
static const uint8_t k_strapping[] = {0};
#endif

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2 || \
    CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2
#define HAVE_PIN_TABLES 1
#else
#define HAVE_PIN_TABLES 0
#endif

static bool in_table(const uint8_t *table, size_t count, int pin)
{
    for (size_t i = 0; i < count; i++) {
        if ((int)table[i] == pin) {
            return true;
        }
    }
    return false;
}

bool gpio_rules_is_restricted(int pin)
{
#if HAVE_PIN_TABLES
    return in_table(k_restricted, sizeof(k_restricted), pin);
#else
    (void)pin;
    (void)in_table;
    return false;
#endif
}

bool gpio_rules_is_strapping(int pin)
{
#if HAVE_PIN_TABLES
    return in_table(k_strapping, sizeof(k_strapping), pin);
#else
    (void)pin;
    return false;
#endif
}

bool gpio_rules_is_valid_output(int pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX && GPIO_IS_VALID_OUTPUT_GPIO(pin);
}

bool gpio_rules_is_valid_input(int pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX && GPIO_IS_VALID_GPIO(pin);
}

const char *gpio_rules_reject_reason(int pin, bool output)
{
    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        return "no such pin on this chip";
    }
    if (!GPIO_IS_VALID_GPIO(pin)) {
        return "not a usable GPIO on this chip";
    }
    if (output && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return "input-only pin, cannot drive an output";
    }
    if (gpio_rules_is_restricted(pin)) {
        return "reserved for flash or PSRAM";
    }
    return NULL;
}

const uint8_t *gpio_rules_restricted(size_t *count)
{
#if HAVE_PIN_TABLES
    *count = sizeof(k_restricted);
#else
    *count = 0;
#endif
    return k_restricted;
}

const uint8_t *gpio_rules_strapping(size_t *count)
{
#if HAVE_PIN_TABLES
    *count = sizeof(k_strapping);
#else
    *count = 0;
#endif
    return k_strapping;
}

int gpio_rules_max_pin(void)
{
    return GPIO_NUM_MAX - 1;
}
