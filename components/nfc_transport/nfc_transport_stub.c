/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nfc_transport.h"

#include <esp_log.h>

static const char *const k_tag = "nfc/stub";

static esp_err_t stub_init(void *ctx)
{
    (void)ctx;
    ESP_LOGW(k_tag, "no NFC frontend configured; reader will never detect a device");
    return ESP_OK;
}

static void stub_poll(void *ctx)
{
    (void)ctx;
}

static bool stub_activate(void *ctx)
{
    (void)ctx;
    return false;
}

static void stub_deactivate(void *ctx)
{
    (void)ctx;
}

static esp_err_t stub_exchange(void *ctx, const uint8_t *command, size_t command_len, uint8_t *response,
                               size_t *response_len)
{
    (void)ctx;
    (void)command;
    (void)command_len;
    (void)response;
    (void)response_len;
    return ESP_ERR_NOT_SUPPORTED;
}

const nfc_transport_t *nfc_transport_stub(void)
{
    static const nfc_transport_t k_stub = {
        .init = stub_init,
        .poll = stub_poll,
        .activate = stub_activate,
        .deactivate = stub_deactivate,
        .exchange = stub_exchange,
        .ctx = NULL,
        .name = "stub",
    };
    return &k_stub;
}
