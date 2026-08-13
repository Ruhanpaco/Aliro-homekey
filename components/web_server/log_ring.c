/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "log_ring.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>

#include <stdio.h>
#include <string.h>

#define RING_SLOTS 64

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static log_line_t s_ring[RING_SLOTS];
static uint32_t s_next_id = 1;
static vprintf_like_t s_previous;

/*
 * Called from whatever task is logging, so it must be short and must never
 * log anything itself.
 */
static int capture(const char *format, va_list args)
{
    char line[LOG_RING_LINE_MAX];

    va_list copy;
    va_copy(copy, args);
    const int written = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);

    if (written > 0) {
        /* Strip the trailing newline and the ANSI colour codes ESP-IDF adds;
         * the browser wants the text, not the terminal escape sequence. */
        size_t len = strnlen(line, sizeof(line) - 1);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len > 0 && line[len - 1] == 'm' && strchr(line, '\033')) {
            char *esc = strrchr(line, '\033');
            if (esc) {
                *esc = '\0';
                len = (size_t)(esc - line);
            }
        }
        const char *text = line;
        if (line[0] == '\033') {
            const char *m = strchr(line, 'm');
            if (m) {
                text = m + 1;
            }
        }

        if (*text) {
            portENTER_CRITICAL(&s_lock);
            log_line_t *slot = &s_ring[s_next_id % RING_SLOTS];
            slot->id = s_next_id++;
            strlcpy(slot->text, text, sizeof(slot->text));
            portEXIT_CRITICAL(&s_lock);
        }
    }

    return s_previous ? s_previous(format, args) : written;
}

esp_err_t log_ring_init(void)
{
    if (s_previous) {
        return ESP_OK;
    }
    s_previous = esp_log_set_vprintf(capture);
    return ESP_OK;
}

uint32_t log_ring_next_id(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint32_t id = s_next_id;
    portEXIT_CRITICAL(&s_lock);
    return id;
}

size_t log_ring_copy_since(uint32_t since, log_line_t *out, size_t max)
{
    size_t count = 0;

    portENTER_CRITICAL(&s_lock);
    const uint32_t newest = s_next_id;
    /* Anything older than a full lap round the ring is already overwritten. */
    uint32_t first = (newest > RING_SLOTS) ? newest - RING_SLOTS : 1;
    if (since + 1 > first) {
        first = since + 1;
    }
    if (newest > first && newest - first > max) {
        first = newest - max;
    }
    for (uint32_t id = first; id < newest && count < max; id++) {
        const log_line_t *slot = &s_ring[id % RING_SLOTS];
        if (slot->id == id) {
            out[count++] = *slot;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    return count;
}
