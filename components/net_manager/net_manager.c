/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "net_manager.h"
#include "dns_hijack.h"

#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>

#include <stdio.h>
#include <string.h>

static const char *const k_tag = "aliro/net";
static const int k_max_join_attempts = 5;

static struct {
    net_config_t cfg;
    net_status_t status;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    int join_attempts;
} s_net;

static void start_setup_ap(void);

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        s_net.status.connected = false;
        s_net.status.ip[0] = '\0';
        if (s_net.status.mode != NET_MODE_STA) {
            break; /* already fell back */
        }
        if (++s_net.join_attempts <= k_max_join_attempts) {
            ESP_LOGW(k_tag, "join failed, retry %d/%d", s_net.join_attempts, k_max_join_attempts);
            esp_wifi_connect();
        } else {
            ESP_LOGW(k_tag, "cannot join '%s', falling back to setup AP", s_net.cfg.ssid);
            start_setup_ap();
        }
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(k_tag, "client joined the setup AP");
        break;

    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    snprintf(s_net.status.ip, sizeof(s_net.status.ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_net.status.connected = true;
    s_net.join_attempts = 0;

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_net.status.rssi = ap.rssi;
    }
    ESP_LOGI(k_tag, "connected as '%s', configuration UI at http://%s/", s_net.cfg.hostname, s_net.status.ip);
}

static void ap_ssid(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, out_len, "Aliro-Setup-%02X%02X", mac[4], mac[5]);
}

static void start_setup_ap(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());

    if (!s_net.ap_netif) {
        s_net.ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t wifi_cfg = {0};
    char ssid[33];
    ap_ssid(ssid, sizeof(ssid));
    strlcpy((char *)wifi_cfg.ap.ssid, ssid, sizeof(wifi_cfg.ap.ssid));
    wifi_cfg.ap.ssid_len = strlen(ssid);
    wifi_cfg.ap.max_connection = 4;
    wifi_cfg.ap.channel = 1;

    if (strlen(s_net.cfg.ap_password) >= 8) {
        strlcpy((char *)wifi_cfg.ap.password, s_net.cfg.ap_password, sizeof(wifi_cfg.ap.password));
        wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_get_ip_info(s_net.ap_netif, &ip_info);
    snprintf(s_net.status.ip, sizeof(s_net.status.ip), IPSTR, IP2STR(&ip_info.ip));
    snprintf(s_net.status.ssid, sizeof(s_net.status.ssid), "%s", ssid);
    s_net.status.mode = NET_MODE_SETUP_AP;
    s_net.status.connected = true;

    /* Answer every DNS query with our own address, so joining the AP pops the
     * configuration page up by itself. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(dns_hijack_start(ip_info.ip.addr));

    ESP_LOGW(k_tag, "setup AP '%s' is up, open http://%s/", ssid, s_net.status.ip);
}

static void start_sta(void)
{
    if (!s_net.sta_netif) {
        s_net.sta_netif = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(s_net.sta_netif, s_net.cfg.hostname);
    }

    wifi_config_t wifi_cfg = {0};
    /* strlcpy, not snprintf: an SSID is 32 bytes and our buffer is 33, so
     * GCC rightly warns that "%s" may truncate. Truncating is the intended
     * behaviour here, and strlcpy says so. */
    strlcpy((char *)wifi_cfg.sta.ssid, s_net.cfg.ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, s_net.cfg.password, sizeof(wifi_cfg.sta.password));

    snprintf(s_net.status.ssid, sizeof(s_net.status.ssid), "%s", s_net.cfg.ssid);
    s_net.status.mode = NET_MODE_STA;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());

    ESP_LOGI(k_tag, "joining '%s' as '%s'", s_net.cfg.ssid, s_net.cfg.hostname);
}

esp_err_t net_manager_start(const net_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, k_tag, "no network configuration");
    s_net.cfg = *cfg;

    ESP_RETURN_ON_ERROR(esp_netif_init(), k_tag, "esp_netif_init failed");
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(loop_err, k_tag, "event loop create failed");
    }

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), k_tag, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), k_tag, "wifi storage failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL),
                        k_tag, "wifi event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL),
                        k_tag, "ip event registration failed");

    if (s_net.cfg.ssid[0] == '\0') {
        ESP_LOGW(k_tag, "no Wi-Fi credentials stored");
        start_setup_ap();
    } else {
        start_sta();
    }

    return ESP_OK;
}

void net_manager_get_status(net_status_t *out)
{
    *out = s_net.status;
}

bool net_manager_is_up(void)
{
    return s_net.status.connected;
}
