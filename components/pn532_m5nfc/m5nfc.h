/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drop-in replacement for Espressif's m5nfc component.
 *
 * Their aliro_reader example and their esp-matter door lock both drive an NFC
 * frontend through exactly these five functions, backed by an ST25R3916 on an
 * M5Unit-NFC. This header declares the same five, backed by a PN532 instead --
 * the part that is actually purchasable.
 *
 * Swap this component in for theirs and their app_main.c and Aliro door lock
 * delegate compile unchanged. Pins come from Kconfig, because a Matter device
 * has no web UI to configure them from.
 */

#pragma once

#include <esp_err.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t m5nfc_init(void);
void m5nfc_update(void);
bool m5nfc_activate(void);
void m5nfc_deactivate(void);

esp_err_t m5nfc_message_exchange(const uint8_t *command, size_t command_len, uint8_t *response, size_t *response_len);

#ifdef __cplusplus
}
#endif
