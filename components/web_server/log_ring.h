/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_RING_LINE_MAX 160

typedef struct {
    uint32_t id;
    char text[LOG_RING_LINE_MAX];
} log_line_t;

/**
 * @brief Capture the log stream into a ring buffer the web UI can read.
 *
 * HomeKey-ESP32 pushes logs to the browser over a WebSocket. This does the
 * same job with a ring buffer and polling: fewer moving parts, and no risk of
 * a socket write re-entering the logger that produced the line.
 */
esp_err_t log_ring_init(void);

/** @brief Id the next captured line will get. */
uint32_t log_ring_next_id(void);

/**
 * @brief Copy lines newer than @p since.
 *
 * @param[in]  since Highest id the caller already has, 0 for everything held
 * @param[out] out   Destination array
 * @param[in]  max   Capacity of @p out
 * @return number of lines written
 */
size_t log_ring_copy_since(uint32_t since, log_line_t *out, size_t max);

#ifdef __cplusplus
}
#endif
