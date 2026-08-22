/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Status LED and RTTTL buzzer.
 *
 * Both are observers of access_control's event stream, the same mechanism
 * MQTT and the Matter door lock endpoint already use -- no new plumbing back
 * into the reader task, and the observer contract ("must not block") is what
 * shapes this file: the buzzer's tune plays on its own task, note by note,
 * never inside the callback that access_control calls from the reader task.
 */

#include "feedback_io.h"

#include "access_control.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_tag = "aliro/feedback";

#define LEDC_TIMER    LEDC_TIMER_0
#define LEDC_CHANNEL  LEDC_CHANNEL_0
#define LEDC_MODE     LEDC_LOW_SPEED_MODE
#define LEDC_RES_BITS LEDC_TIMER_10_BIT
#define LEDC_MAX_DUTY ((1 << 10) - 1)

typedef enum {
    TUNE_GRANTED,
    TUNE_DENIED,
} tune_t;

static struct {
    feedback_config_t cfg;
    bool led_ready;
    bool buzzer_ready;
    QueueHandle_t tune_queue;
} s_feedback;

/* --- RTTTL -----------------------------------------------------------------
 *
 * "Name:d=4,o=6,b=100:c,8e,g#5,4p,2c6."  -- a name (ignored), a defaults
 * section, then a comma-separated list of notes. Each note is
 * [duration]pitch['#']['.'][octave], any part optional and falling back to
 * the defaults section. Layout and defaults per the format Nokia composers
 * popularized; this project cares only that it round-trips what a user pastes
 * from one of the many RTTTL libraries already out there.
 */

typedef struct {
    int duration; /*!< Denominator of a whole note: 4 = quarter, 8 = eighth */
    int octave;
    int bpm;
} rtttl_defaults_t;

static void parse_defaults(const char *section, rtttl_defaults_t *out)
{
    /* Sane fallbacks if a field is missing -- the classic RTTTL defaults. */
    out->duration = 4;
    out->octave = 6;
    out->bpm = 63;

    const char *p = section;
    while (p && *p) {
        char key = *p;
        const char *eq = strchr(p, '=');
        if (!eq) {
            break;
        }
        int value = atoi(eq + 1);
        switch (key) {
        case 'd':
            if (value > 0) {
                out->duration = value;
            }
            break;
        case 'o':
            out->octave = value;
            break;
        case 'b':
            if (value > 0) {
                out->bpm = value;
            }
            break;
        default:
            break;
        }
        const char *comma = strchr(eq, ',');
        p = comma ? comma + 1 : NULL;
    }
}

/** @brief Semitone offset from A4 for a natural note (c=0 .. b=11, a=9). */
static int note_semitone(char note)
{
    switch (note) {
    case 'c':
        return -9;
    case 'd':
        return -7;
    case 'e':
        return -5;
    case 'f':
        return -4;
    case 'g':
        return -2;
    case 'a':
        return 0;
    case 'b':
        return 2;
    default:
        return -9;
    }
}

/** @brief One note: play at @p freq_hz for @p on_ms, then silence for @p off_ms. 0 Hz is a rest. */
static void play_note(double freq_hz, uint32_t on_ms, uint32_t off_ms, uint8_t gain_pct)
{
    if (freq_hz > 0 && on_ms > 0) {
        const uint32_t duty = (uint32_t)((LEDC_MAX_DUTY / 2) * (gain_pct > 100 ? 100 : gain_pct) / 100);
        ledc_set_freq(LEDC_MODE, LEDC_TIMER, (uint32_t)freq_hz);
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
    }
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    if (off_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

static void play_rtttl(const char *tune, uint8_t gain_pct)
{
    if (!tune || !tune[0]) {
        return;
    }

    /* Skip the name; RTTTL is name:defaults:notes and only the last two
     * sections matter here. */
    const char *first_colon = strchr(tune, ':');
    if (!first_colon) {
        ESP_LOGW(k_tag, "tune has no ':' separators, not RTTTL: '%s'", tune);
        return;
    }
    const char *second_colon = strchr(first_colon + 1, ':');
    if (!second_colon) {
        ESP_LOGW(k_tag, "tune is missing the notes section: '%s'", tune);
        return;
    }

    rtttl_defaults_t defaults;
    char defaults_buf[64];
    snprintf(defaults_buf, sizeof(defaults_buf), "%.*s", (int)(second_colon - first_colon - 1), first_colon + 1);
    parse_defaults(defaults_buf, &defaults);

    const char *p = second_colon + 1;
    while (p && *p) {
        const char *comma = strchr(p, ',');
        size_t tok_len = comma ? (size_t)(comma - p) : strlen(p);
        char tok[16];
        if (tok_len >= sizeof(tok)) {
            tok_len = sizeof(tok) - 1;
        }
        memcpy(tok, p, tok_len);
        tok[tok_len] = '\0';

        /* [duration digits] note-letter ['#'] ['.'] [octave digit] */
        const char *c = tok;
        int duration = 0;
        while (*c >= '0' && *c <= '9') {
            duration = duration * 10 + (*c - '0');
            c++;
        }
        if (duration == 0) {
            duration = defaults.duration;
        }

        double freq = 0;
        if (*c == 'p' || *c == 'P') {
            c++;
        } else if (*c) {
            char note = (char)tolower((unsigned char)*c);
            int semitone = note_semitone(note);
            c++;
            if (*c == '#') {
                semitone++;
                c++;
            }
            if (*c == '.') {
                c++;
            }
            int octave = defaults.octave;
            if (*c >= '0' && *c <= '9') {
                octave = *c - '0';
            }
            const int semitones_from_a4 = (octave - 4) * 12 + semitone;
            freq = 440.0 * pow(2.0, semitones_from_a4 / 12.0);
        }

        /* Whole note takes (60000 / bpm) * 4 ms; this note is 1/duration of that. */
        uint32_t note_ms = (uint32_t)((60000.0 / defaults.bpm) * 4.0 / duration);
        if (strchr(tok, '.')) {
            note_ms = note_ms + note_ms / 2;
        }
        /* A short gap between notes keeps them from blurring into one tone. */
        const uint32_t on_ms = note_ms > 20 ? note_ms - 15 : note_ms;
        play_note(freq, on_ms, note_ms - on_ms + 5, gain_pct);

        p = comma ? comma + 1 : NULL;
    }
}

/* --- task + observer -------------------------------------------------------- */

static void buzzer_task(void *arg)
{
    (void)arg;
    tune_t which;
    for (;;) {
        if (xQueueReceive(s_feedback.tune_queue, &which, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const char *tune = which == TUNE_GRANTED ? s_feedback.cfg.tune_granted : s_feedback.cfg.tune_denied;
        play_rtttl(tune, s_feedback.cfg.buzzer_gain);
    }
}

static void on_access_event(const access_event_t *event, void *ctx)
{
    (void)ctx;

    if (event->type == ACCESS_EVENT_LOCK_STATE) {
        if (s_feedback.led_ready) {
            const bool level = s_feedback.cfg.led_active_low ? !event->locked : event->locked;
            gpio_set_level(s_feedback.cfg.led_gpio, level);
        }
        return;
    }

    if (event->type == ACCESS_EVENT_TAP && s_feedback.buzzer_ready) {
        const tune_t which = event->granted ? TUNE_GRANTED : TUNE_DENIED;
        /* Never block the reader task: drop the request rather than wait for
         * queue space if a previous tune is somehow still draining. */
        (void)xQueueSend(s_feedback.tune_queue, &which, 0);
    }
}

esp_err_t feedback_io_start(const feedback_config_t *cfg)
{
    memset(&s_feedback, 0, sizeof(s_feedback));
    s_feedback.cfg = *cfg;

    if (cfg->led_enabled && cfg->led_gpio != APP_CFG_PIN_UNSET) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfg->led_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), k_tag, "LED GPIO config failed");
        gpio_set_level(cfg->led_gpio, cfg->led_active_low ? 1 : 0); /* starts locked */
        s_feedback.led_ready = true;
    }

    if (cfg->buzzer_enabled && cfg->buzzer_gpio != APP_CFG_PIN_UNSET) {
        const ledc_timer_config_t timer_cfg = {
            .speed_mode = LEDC_MODE,
            .timer_num = LEDC_TIMER,
            .duty_resolution = LEDC_RES_BITS,
            .freq_hz = 1000, /* placeholder; play_note() sets the real frequency per note */
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), k_tag, "LEDC timer config failed");

        const ledc_channel_config_t chan_cfg = {
            .gpio_num = cfg->buzzer_gpio,
            .speed_mode = LEDC_MODE,
            .channel = LEDC_CHANNEL,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&chan_cfg), k_tag, "LEDC channel config failed");

        s_feedback.tune_queue = xQueueCreate(2, sizeof(tune_t));
        ESP_RETURN_ON_FALSE(s_feedback.tune_queue, ESP_ERR_NO_MEM, k_tag, "tune queue alloc failed");

        /* A tune is a handful of tones over well under a second; this task
         * does nothing else and needs no more stack than that. */
        if (xTaskCreate(buzzer_task, "aliro_buzzer", 2560, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            ESP_LOGE(k_tag, "buzzer task creation failed; buzzer disabled this boot");
        } else {
            s_feedback.buzzer_ready = true;
        }
    }

    if (!s_feedback.led_ready && !s_feedback.buzzer_ready) {
        return ESP_OK; /* nothing configured, nothing to observe */
    }

    return access_control_add_observer(on_access_event, NULL);
}
