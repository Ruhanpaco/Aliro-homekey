/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Answer every DNS A query with @p ipv4_addr.
 *
 * The captive-portal trick: whatever a phone probes for after joining the
 * setup AP resolves to us, so the configuration page opens on its own.
 *
 * @param ipv4_addr Address to hand out, in network byte order
 */
esp_err_t dns_hijack_start(uint32_t ipv4_addr);

/** @brief Stop the DNS responder. */
void dns_hijack_stop(void);

#ifdef __cplusplus
}
#endif
