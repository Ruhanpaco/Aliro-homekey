/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The whole API, for builds without CONFIG_ALIRO_MATTER_ENABLE.
 *
 * Matter roughly doubles the size of the application, so a board that is only
 * ever going to be provisioned by the browser flasher should not have to carry
 * it. Stubbing the API here rather than sprinkling #if around app_main and the
 * web server means exactly one file differs between the two builds.
 */

#include <sdkconfig.h>

#if !CONFIG_ALIRO_MATTER_ENABLE

#include "matter_lock.h"

esp_err_t matter_lock_start(const matter_lock_hooks_t *hooks)
{
    (void)hooks;
    return ESP_ERR_NOT_SUPPORTED;
}

bool matter_lock_available(void)
{
    return false;
}

bool matter_lock_running(void)
{
    return false;
}

size_t matter_lock_fabric_count(void)
{
    return 0;
}

bool matter_lock_reader_configured(void)
{
    return false;
}

const char *matter_lock_qr_payload(void)
{
    return "";
}

const char *matter_lock_manual_code(void)
{
    return "";
}

const char *matter_lock_qr_url(void)
{
    return "";
}

esp_err_t matter_lock_open_commissioning_window(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t matter_lock_release_reader_config(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void matter_lock_report_lock_state(bool locked)
{
    (void)locked;
}

#endif /* !CONFIG_ALIRO_MATTER_ENABLE */
