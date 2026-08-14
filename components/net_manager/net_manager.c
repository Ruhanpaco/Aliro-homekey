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
#include <freertos/event_groups.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_tag = "aliro/net";
static const int k_max_join_attempts = 5;

#define BIT_GOT_IP    BIT0
#define BIT_JOIN_FAIL BIT1

static struct {
    net_config_t cfg;
    net_status_t status;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    int join_attempts;
    EventGroupHandle_t events;
    /* A join requested from the configuration UI. While this is set the
     * disconnect handler reports the failure instead of retrying, so the
     * browser gets an answer rather than watching a silent retry loop. */
    bool joining;
    bool ap_up;
} s_net;

static void start_setup_ap(void);

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch (id) {
    case WIFI_EVENT_STA_START:
        /* Only chase a network we were actually given one to chase. In setup
         * mode the station interface exists purely so that scanning and a
         * live join are possible, and connecting to nothing would just spam
         * disconnect events. */
        if (s_net.cfg.ssid[0] != '\0') {
            esp_wifi_connect();
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        s_net.status.connected = s_net.ap_up;
        if (s_net.joining) {
            xEventGroupSetBits(s_net.events, BIT_JOIN_FAIL);
            break;
        }
        if (s_net.status.mode != NET_MODE_STA) {
            break; /* already fell back to the setup AP */
        }
        s_net.status.ip[0] = '\0';
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
    s_net.status.mode = NET_MODE_STA;
    s_net.join_attempts = 0;

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_net.status.rssi = ap.rssi;
        snprintf(s_net.status.ssid, sizeof(s_net.status.ssid), "%s", (const char *)ap.ssid);
    }

    ESP_LOGI(k_tag, "joined '%s' -- configuration UI at http://%s/", s_net.status.ssid, s_net.status.ip);
    xEventGroupSetBits(s_net.events, BIT_GOT_IP);
}

static void ap_ssid(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, out_len, "Aliro-Setup-%02X%02X", mac[4], mac[5]);
}

/*
 * RFC 8910: hand the portal's address to the client in the DHCP lease. A phone
 * that understands this opens the configuration page by itself instead of
 * showing "no internet" and leaving the user to find 192.168.4.1. The DNS
 * hijack below is the fallback for everything that does not.
 */
static void advertise_captive_portal(esp_netif_t *netif, const esp_netif_ip_info_t *ip_info)
{
    char uri[32];
    snprintf(uri, sizeof(uri), "http://" IPSTR, IP2STR(&ip_info->ip));

    /* The option can only be set while the DHCP server is stopped. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
    const esp_err_t err =
        esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    if (err != ESP_OK) {
        ESP_LOGW(k_tag, "captive portal DHCP option rejected: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
}

static void start_setup_ap(void)
{
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

    /* AP *and* station. The station half is what makes the portal useful: it
     * can list the networks in range and join one while the phone stays
     * connected to the AP, so the user never has to guess an address or
     * reboot the board to find out whether the password was right. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_get_ip_info(s_net.ap_netif, &ip_info);
    snprintf(s_net.status.ip, sizeof(s_net.status.ip), IPSTR, IP2STR(&ip_info.ip));
    snprintf(s_net.status.ap_ip, sizeof(s_net.status.ap_ip), IPSTR, IP2STR(&ip_info.ip));
    snprintf(s_net.status.ssid, sizeof(s_net.status.ssid), "%s", ssid);
    s_net.status.mode = NET_MODE_SETUP_AP;
    s_net.status.connected = true;
    s_net.ap_up = true;

    advertise_captive_portal(s_net.ap_netif, &ip_info);

    /* Answer every DNS query with our own address, for clients that ignore
     * the DHCP option above. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(dns_hijack_start(ip_info.ip.addr));

    ESP_LOGW(k_tag, "setup AP '%s' is up, open http://%s/", ssid, s_net.status.ip);
}

static void start_sta(void)
{
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

    s_net.events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_net.events, ESP_ERR_NO_MEM, k_tag, "event group allocation failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), k_tag, "esp_netif_init failed");
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(loop_err, k_tag, "event loop create failed");
    }

    /* Both interfaces exist from the start. Creating one later, while Wi-Fi is
     * running, is the kind of reordering that works on the bench and fails on
     * a cold boot. */
    s_net.sta_netif = esp_netif_create_default_wifi_sta();
    s_net.ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_net.sta_netif && s_net.ap_netif, ESP_ERR_NO_MEM, k_tag, "netif creation failed");
    esp_netif_set_hostname(s_net.sta_netif, s_net.cfg.hostname);

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

/* --- configuration-time helpers ------------------------------------------ */

esp_err_t net_manager_scan(net_scan_result_t *out, size_t max, size_t *out_count)
{
    ESP_RETURN_ON_FALSE(out && out_count && max > 0, ESP_ERR_INVALID_ARG, k_tag, "invalid scan request");
    *out_count = 0;

    /* Scanning needs the station interface enabled. In setup mode it already
     * is; a device that has joined a network is also fine. Only a pure-AP
     * configuration needs a nudge, and it must keep serving the UI. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), k_tag, "cannot read Wi-Fi mode");
    if (mode == WIFI_MODE_AP) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), k_tag, "cannot enable the station interface");
    }

    const wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {.active = {.min = 100, .max = 300}},
    };
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_cfg, true), k_tag, "scan failed");

    uint16_t found = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&found), k_tag, "scan count failed");
    if (found == 0) {
        return ESP_OK;
    }

    wifi_ap_record_t *records = calloc(found, sizeof(*records));
    ESP_RETURN_ON_FALSE(records, ESP_ERR_NO_MEM, k_tag, "no memory for %u scan results", (unsigned)found);

    esp_err_t err = esp_wifi_scan_get_ap_records(&found, records);
    if (err == ESP_OK) {
        /* esp_wifi_scan_get_ap_records already sorts by descending RSSI, so
         * the first time an SSID is seen is its strongest radio. */
        for (uint16_t i = 0; i < found && *out_count < max; i++) {
            const char *ssid = (const char *)records[i].ssid;
            if (ssid[0] == '\0') {
                continue; /* hidden network: nothing to show and nothing to tap */
            }

            bool duplicate = false;
            for (size_t j = 0; j < *out_count; j++) {
                if (strcmp(out[j].ssid, ssid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }

            net_scan_result_t *entry = &out[(*out_count)++];
            strlcpy(entry->ssid, ssid, sizeof(entry->ssid));
            entry->rssi = records[i].rssi;
            entry->channel = records[i].primary;
            entry->open = records[i].authmode == WIFI_AUTH_OPEN;
        }
    }

    free(records);
    ESP_RETURN_ON_ERROR(err, k_tag, "cannot read scan results");
    ESP_LOGI(k_tag, "scan found %u networks, %u distinct", (unsigned)found, (unsigned)*out_count);
    return ESP_OK;
}

esp_err_t net_manager_join(const char *ssid, const char *password, uint32_t timeout_ms, char *out_ip,
                           size_t out_ip_len)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0] && password, ESP_ERR_INVALID_ARG, k_tag, "invalid join request");

    /* Keep the access point up: the browser asking for this is on it, and a
     * wrong password has to be reportable. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), k_tag, "cannot enable the station interface");
    }

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), k_tag, "cannot apply credentials");

    ESP_LOGI(k_tag, "trying to join '%s'", ssid);
    xEventGroupClearBits(s_net.events, BIT_GOT_IP | BIT_JOIN_FAIL);
    s_net.joining = true;

    (void)esp_wifi_disconnect();
    const esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK) {
        s_net.joining = false;
        ESP_RETURN_ON_ERROR(connect_err, k_tag, "esp_wifi_connect failed");
    }

    const EventBits_t bits = xEventGroupWaitBits(s_net.events, BIT_GOT_IP | BIT_JOIN_FAIL, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(timeout_ms));
    s_net.joining = false;

    if (bits & BIT_GOT_IP) {
        /* These are now the running credentials. Storing them is the caller's
         * decision: a successful test join is not automatically a commitment. */
        strlcpy(s_net.cfg.ssid, ssid, sizeof(s_net.cfg.ssid));
        strlcpy(s_net.cfg.password, password, sizeof(s_net.cfg.password));
        s_net.join_attempts = 0;
        if (out_ip && out_ip_len) {
            strlcpy(out_ip, s_net.status.ip, out_ip_len);
        }
        return ESP_OK;
    }

    ESP_LOGW(k_tag, "could not join '%s'", ssid);
    (void)esp_wifi_disconnect();
    return (bits & BIT_JOIN_FAIL) ? ESP_ERR_WIFI_NOT_CONNECT : ESP_ERR_TIMEOUT;
}
