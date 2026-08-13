/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal captive-portal DNS responder. Adapted in spirit from the dns_server
 * component in HomeKey-ESP32 (MIT, (c) rednblkx) and ESP-IDF's captive portal
 * example.
 */

#include "dns_hijack.h"

#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const char *const k_tag = "aliro/dns";
static const int k_dns_port = 53;
static const size_t k_max_packet = 512;

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t rd_length;
    uint32_t addr;
} dns_answer_t;

static TaskHandle_t s_task;
static int s_sock = -1;
static uint32_t s_addr;

/** @brief Skip a QNAME, which is a series of length-prefixed labels. */
static const uint8_t *skip_qname(const uint8_t *p, const uint8_t *end)
{
    while (p < end && *p != 0) {
        p += (size_t)*p + 1;
    }
    return (p < end) ? p + 1 : NULL;
}

static void dns_task(void *params)
{
    (void)params;
    uint8_t buf[k_max_packet];

    while (s_sock >= 0) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        const int len = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len < (int)sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *header = (dns_header_t *)buf;
        if (ntohs(header->qd_count) != 1) {
            continue; /* one question per packet is all a captive portal sees */
        }

        const uint8_t *q = buf + sizeof(dns_header_t);
        const uint8_t *end = buf + len;
        const uint8_t *after_name = skip_qname(q, end);
        if (!after_name || after_name + 4 > end) {
            continue;
        }

        uint16_t qtype;
        memcpy(&qtype, after_name, sizeof(qtype));
        if (ntohs(qtype) != 1 /* A */) {
            continue;
        }

        const size_t question_len = (size_t)(after_name + 4 - q);
        const size_t reply_len = sizeof(dns_header_t) + question_len + 2 + sizeof(dns_answer_t);
        if (reply_len > sizeof(buf)) {
            continue;
        }

        header->flags = htons(0x8180); /* response, recursion available */
        header->an_count = htons(1);
        header->ns_count = 0;
        header->ar_count = 0;

        uint8_t *answer = buf + sizeof(dns_header_t) + question_len;
        answer[0] = 0xC0; /* pointer to the question's name */
        answer[1] = 0x0C;

        dns_answer_t record = {
            .type = htons(1),
            .class = htons(1),
            .ttl = htonl(60),
            .rd_length = htons(4),
            .addr = s_addr,
        };
        memcpy(answer + 2, &record, sizeof(record));

        sendto(s_sock, buf, reply_len, 0, (struct sockaddr *)&from, from_len);
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_hijack_start(uint32_t ipv4_addr)
{
    if (s_task) {
        return ESP_OK;
    }

    s_addr = ipv4_addr;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(s_sock >= 0, ESP_FAIL, k_tag, "socket failed: errno %d", errno);

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(k_dns_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(k_tag, "bind failed: errno %d", errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 5, &s_task) != pdPASS) {
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(k_tag, "captive DNS responder started");
    return ESP_OK;
}

void dns_hijack_stop(void)
{
    if (s_sock >= 0) {
        const int sock = s_sock;
        s_sock = -1;
        shutdown(sock, 0);
        close(sock);
    }
}
