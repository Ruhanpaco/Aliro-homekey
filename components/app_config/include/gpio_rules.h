/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which pins a user is allowed to pick in the web UI.
 *
 * Pin tables adapted from HomeKey-ESP32 (MIT, (c) rednblkx), which learned
 * them the hard way. Two categories:
 *
 *   restricted - wired to flash or PSRAM inside the package. Choosing one
 *                bricks the boot. Never selectable.
 *   strapping  - sampled at reset to decide boot mode. Usable, but a pull-up
 *                or pull-down on them can stop the board from booting, so the
 *                UI warns.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief True when the pin is wired to flash/PSRAM and must never be used. */
bool gpio_rules_is_restricted(int pin);

/** @brief True when the pin is sampled at reset. Usable with care. */
bool gpio_rules_is_strapping(int pin);

/** @brief True when the pin exists on this chip and can be an output. */
bool gpio_rules_is_valid_output(int pin);

/** @brief True when the pin exists on this chip and can be an input. */
bool gpio_rules_is_valid_input(int pin);

/**
 * @brief Explain why a pin cannot be used, or return NULL when it can.
 *
 * @param pin    Pin under test
 * @param output True when the pin will be driven
 */
const char *gpio_rules_reject_reason(int pin, bool output);

/** @brief Restricted pins on this chip. */
const uint8_t *gpio_rules_restricted(size_t *count);

/** @brief Strapping pins on this chip. */
const uint8_t *gpio_rules_strapping(size_t *count);

/** @brief Highest GPIO number on this chip. */
int gpio_rules_max_pin(void);

#ifdef __cplusplus
}
#endif
