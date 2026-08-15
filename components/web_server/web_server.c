/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "web_server.h"

#include "log_ring.h"

#include "app_config.h"
#include "matter_lock.h"
#include "net_manager.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_check.h>
#include <esp_chip_info.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static const char *const k_tag = "aliro/web";
static const size_t k_max_body = 4096;
static const size_t k_max_ws_payload = 8192;

/* Embedded gzipped by the component's CMakeLists: ~98 KB of UI becomes ~23 KB
 * of application partition, and the browser inflates it. */
extern const uint8_t index_html_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_gz_end");
extern const uint8_t setup_html_start[] asm("_binary_setup_html_gz_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_gz_end");

static httpd_handle_t s_server;
static web_server_hooks_t s_hooks;

/* --- Session and Authentication ----------------------------------------- */

typedef struct {
    uint32_t id;
    uint64_t created_at;
    bool authenticated;
} session_t;

#define SESSION_TIMEOUT_MS (15 * 60 * 1000)  /* 15 minutes */
#define MAX_SESSIONS 8
static session_t s_sessions[MAX_SESSIONS];
static SemaphoreHandle_t s_sessions_mutex = NULL;

static uint32_t session_create(void)
{
    if (xSemaphoreTake(s_sessions_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return 0;
    }

    uint32_t now_ms = esp_timer_get_time() / 1000;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].created_at == 0 || (now_ms - s_sessions[i].created_at) > SESSION_TIMEOUT_MS) {
            s_sessions[i].id = (i + 1) * 12345 + (now_ms & 0xFFFF);
            s_sessions[i].created_at = now_ms;
            s_sessions[i].authenticated = false;
            xSemaphoreGive(s_sessions_mutex);
            return s_sessions[i].id;
        }
    }

    xSemaphoreGive(s_sessions_mutex);
    return 0;
}

static bool session_is_valid(uint32_t session_id)
{
    if (session_id == 0) {
        return false;
    }

    if (xSemaphoreTake(s_sessions_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    uint32_t now_ms = esp_timer_get_time() / 1000;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].id == session_id && (now_ms - s_sessions[i].created_at) <= SESSION_TIMEOUT_MS) {
            s_sessions[i].created_at = now_ms; /* Refresh expiry */
            xSemaphoreGive(s_sessions_mutex);
            return true;
        }
    }

    xSemaphoreGive(s_sessions_mutex);
    return false;
}

/* --- Event Publishing ---------------------------------------------------- */

typedef web_server_event_cb_t event_handler_t;
static event_handler_t s_event_observers[4] = {NULL};

static void publish_event(const char *event_type, const cJSON *data)
{
    for (size_t i = 0; i < sizeof(s_event_observers) / sizeof(s_event_observers[0]); i++) {
        if (s_event_observers[i]) {
            s_event_observers[i](event_type, data);
        }
    }
}

void web_server_register_event_observer(web_server_event_cb_t observer)
{
    for (size_t i = 0; i < sizeof(s_event_observers) / sizeof(s_event_observers[0]); i++) {
        if (!s_event_observers[i]) {
            s_event_observers[i] = observer;
            ESP_LOGI(k_tag, "Event observer registered at index %zu", i);
            return;
        }
    }
    ESP_LOGW(k_tag, "No space for more event observers");
}

/* --- Validation & Response Helpers ---------------------------------------- */

/** Standard HomeKey-style JSON response format */
static esp_err_t send_json_response(httpd_req_t *req, bool success, const char *message, 
                                     const char *error, cJSON *data)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *res = cJSON_CreateObject();
    
    cJSON_AddBoolToObject(res, "success", success);
    if (message) cJSON_AddStringToObject(res, "message", message);
    if (error) cJSON_AddStringToObject(res, "error", error);
    if (data) cJSON_AddItemToObject(res, "data", data);
    
    char *json_str = cJSON_PrintUnformatted(res);
    esp_err_t err = httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(res);
    
    return err;
}

/** Validate setup code: 8 digits, not all same, not sequential patterns */
static bool is_valid_setup_code(const char *code)
{
    if (!code || strlen(code) != 8) return false;
    
    // Check all digits
    for (int i = 0; i < 8; i++) {
        if (code[i] < '0' || code[i] > '9') return false;
    }
    
    // Reject all same digit (00000000-99999999)
    bool all_same = true;
    for (int i = 1; i < 8; i++) {
        if (code[i] != code[0]) {
            all_same = false;
            break;
        }
    }
    if (all_same) return false;
    
    // Reject sequential patterns
    const char *bad_patterns[] = {"12345678", "87654321", NULL};
    for (int i = 0; bad_patterns[i]; i++) {
        if (strcmp(code, bad_patterns[i]) == 0) return false;
    }
    
    return true;
}

/** Validate WiFi credentials */
static bool is_valid_wifi_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password) return false;
    
    size_t ssid_len = strlen(ssid);
    size_t pwd_len = strlen(password);
    
    // SSID: 1-32 chars, password: 8-63 chars (WPA2 requirement)
    return (ssid_len > 0 && ssid_len <= 32) && (pwd_len >= 8 && pwd_len <= 63);
}

/** Check heap memory is sufficient for requested feature */
static bool check_heap_available(size_t required_bytes, const char *feature)
{
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < required_bytes) {
        ESP_LOGW(k_tag, "%s requires %zu bytes, only %zu available", 
                 feature, required_bytes, free_heap);
        return false;
    }
    return true;
}

/** Validate GPIO pin (chip-specific) */
static bool is_valid_gpio_pin(uint8_t pin)
{
    // Reject invalid pin numbers
    if (pin == 255) return true;  // -1 means "not used"
    
    // ESP32 valid GPIO ranges (chip-dependent)
    // For now, accept 0-39 as general range (actual varies by chip)
    return pin < 40;
}

/** Check GPIO is not reserved for flash or PSRAM */
static bool is_reserved_gpio(uint8_t pin)
{
    // Reserved pins for ESP32 (varies by chip)
    // GPIO 6-11: Flash (SPI0, SPI1)
    // GPIO 16-17: PSRAM (if used)
    if ((pin >= 6 && pin <= 11) || (pin >= 16 && pin <= 17)) {
        return true;
    }
    return false;
}

/* --- WebSocket and real-time support -------------------------------------- */

typedef struct {
    int fd;
    httpd_ws_type_t type;
    size_t len;
    uint8_t *payload;
    uint8_t inline_payload[256];
} ws_frame_t;

static QueueHandle_t s_ws_queue = NULL;
static TaskHandle_t s_ws_task_handle = NULL;
static esp_timer_handle_t s_status_timer = NULL;

/* WebSocket clients registry */
typedef struct {
    int fd;
} ws_client_t;

#define WS_MAX_CLIENTS 4
static ws_client_t s_ws_clients[WS_MAX_CLIENTS];
static size_t s_ws_client_count = 0;
static SemaphoreHandle_t s_ws_clients_mutex = NULL;

/* --- helpers ------------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/** @brief Send an owned JSON string and free it. */
static esp_err_t send_json_owned(httpd_req_t *req, char *json)
{
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    const esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, httpd_err_code_t code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_status(req, code == HTTPD_400_BAD_REQUEST ? "400 Bad Request" : "500 Internal Server Error");
    const esp_err_t err = send_json(req, json ? json : "{\"ok\":false}");
    free(json);
    return err;
}

/** @brief Read the whole request body into a NUL-terminated heap buffer. */
static char *read_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > k_max_body) {
        return NULL;
    }

    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        return NULL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            free(buf);
            return NULL;
        }
        received += ret;
    }
    buf[received] = '\0';
    return buf;
}

/* --- WebSocket support ---------------------------------------------------- */

static void ws_queue_frame(int fd, const uint8_t *payload, size_t len, httpd_ws_type_t type)
{
    ws_frame_t *frame = malloc(sizeof(ws_frame_t));
    if (!frame) {
        return;
    }

    frame->fd = fd;
    frame->type = type;
    frame->len = len;

    if (len <= sizeof(frame->inline_payload)) {
        memcpy(frame->inline_payload, payload, len);
        frame->payload = frame->inline_payload;
    } else {
        frame->payload = malloc(len);
        if (!frame->payload) {
            free(frame);
            return;
        }
        memcpy(frame->payload, payload, len);
    }

    if (xQueueSend(s_ws_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (frame->payload != frame->inline_payload) {
            free(frame->payload);
        }
        free(frame);
    }
}

static void ws_add_client(int fd)
{
    if (xSemaphoreTake(s_ws_clients_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    /*
     * A socket number is only unique while the socket is open. A browser that
     * reloads gets its descriptor closed and handed straight back out, so the
     * same fd arrives at a second handshake while the first entry is still in
     * the table -- which then queues every broadcast twice for one client and
     * fails the duplicate send with ESP_ERR_INVALID_ARG. Re-registering an fd
     * we already hold is a reconnection, not a new client.
     */
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd) {
            xSemaphoreGive(s_ws_clients_mutex);
            ESP_LOGD(k_tag, "WebSocket client fd=%d reconnected", fd);
            return;
        }
    }

    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == -1) {
            s_ws_clients[i].fd = fd;
            s_ws_client_count++;
            ESP_LOGI(k_tag, "WebSocket client added: fd=%d, total=%zu", fd, s_ws_client_count);
            break;
        }
    }

    xSemaphoreGive(s_ws_clients_mutex);
}

static void ws_remove_client(int fd)
{
    if (xSemaphoreTake(s_ws_clients_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd == fd) {
            s_ws_clients[i].fd = -1;
            s_ws_client_count--;
            ESP_LOGI(k_tag, "WebSocket client removed: fd=%d, remaining=%zu", fd, s_ws_client_count);
            break;
        }
    }

    xSemaphoreGive(s_ws_clients_mutex);
}

static void ws_broadcast(const uint8_t *payload, size_t len, httpd_ws_type_t type)
{
    if (xSemaphoreTake(s_ws_clients_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].fd != -1) {
            ws_queue_frame(s_ws_clients[i].fd, payload, len, type);
        }
    }

    xSemaphoreGive(s_ws_clients_mutex);
}

static void ws_send_task(void *arg)
{
    (void)arg;

    while (true) {
        ws_frame_t *frame = NULL;
        if (xQueueReceive(s_ws_queue, &frame, portMAX_DELAY) == pdTRUE && frame) {
            httpd_ws_frame_t ws_pkt = {
                .final = true,
                .fragmented = false,
                .type = frame->type,
                .len = frame->len,
                .payload = frame->payload,
            };

            esp_err_t ret = httpd_ws_send_frame_async(s_server, frame->fd, &ws_pkt);
            if (ret != ESP_OK) {
                ESP_LOGW(k_tag, "WebSocket send failed: %s", esp_err_to_name(ret));
                ws_remove_client(frame->fd);
            }

            if (frame->payload != frame->inline_payload) {
                free(frame->payload);
            }
            free(frame);
        }
    }
}

/* --- Metrics broadcast ---------------------------------------------------- */

static char *format_metrics_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "metrics");

    uint64_t uptime_ms = esp_timer_get_time() / 1000;
    cJSON_AddNumberToObject(root, "uptime_ms", (double)uptime_ms);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());

    net_status_t net;
    net_manager_get_status(&net);
    cJSON_AddNumberToObject(root, "rssi", net.rssi);

    cJSON_AddBoolToObject(root, "mqtt_connected", s_hooks.mqtt_connected ? s_hooks.mqtt_connected() : false);
    cJSON_AddBoolToObject(root, "locked", s_hooks.lock_is_locked ? s_hooks.lock_is_locked() : true);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void status_timer_callback(void *arg)
{
    (void)arg;
    char *metrics = format_metrics_json();
    if (metrics) {
        ws_broadcast((const uint8_t *)metrics, strlen(metrics), HTTPD_WS_TYPE_TEXT);
        free(metrics);
    }
}

/* --- handlers ------------------------------------------------------------ */

/** @brief True while the reader is running its own AP with nowhere to go. */
static bool in_setup_mode(void)
{
    net_status_t net;
    net_manager_get_status(&net);
    return net.mode == NET_MODE_SETUP_AP;
}

static esp_err_t send_page(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    /* EMBED_FILES stores the file verbatim, with no terminator, so the whole
     * span between the symbols is page content -- here, gzip. */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t handle_setup(httpd_req_t *req)
{
    return send_page(req, setup_html_start, setup_html_end);
}

static esp_err_t handle_index(httpd_req_t *req)
{
    /*
     * A device still on its own access point has exactly one useful thing to
     * offer, so offer only that. The full configuration UI assumes a real
     * network -- it opens a WebSocket, polls status every five seconds and
     * loads five pages of hardware settings -- none of which a captive-portal
     * WebView handles well, and none of which can be acted on before the
     * reader is reachable.
     */
    if (in_setup_mode()) {
        return handle_setup(req);
    }
    return send_page(req, index_html_start, index_html_end);
}

static esp_err_t handle_get_status(httpd_req_t *req)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON *data = cJSON_CreateObject();
    cJSON *device = cJSON_AddObjectToObject(data, "device");
    cJSON_AddStringToObject(device, "name", app_config_get()->device_name);
    cJSON_AddStringToObject(device, "target", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(device, "cores", chip.cores);
    cJSON_AddNumberToObject(device, "revision", chip.revision);
    cJSON_AddStringToObject(device, "firmware", app ? app->version : "unknown");
    cJSON_AddStringToObject(device, "idf", app ? app->idf_ver : "unknown");
    cJSON_AddNumberToObject(device, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(device, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(device, "min_free_heap", esp_get_minimum_free_heap_size());

    net_status_t net;
    net_manager_get_status(&net);
    cJSON *network = cJSON_AddObjectToObject(data, "network");
    cJSON_AddStringToObject(network, "mode", net.mode == NET_MODE_STA          ? "sta"
                                             : net.mode == NET_MODE_SETUP_AP ? "setup_ap"
                                                                              : "offline");
    cJSON_AddBoolToObject(network, "connected", net.connected);
    cJSON_AddStringToObject(network, "ssid", net.ssid);
    cJSON_AddStringToObject(network, "ip", net.ip);
    cJSON_AddNumberToObject(network, "rssi", net.rssi);

    cJSON *reader = cJSON_AddObjectToObject(data, "reader");
    cJSON_AddNumberToObject(reader, "credentials", s_hooks.credential_count ? s_hooks.credential_count() : 0);
    cJSON_AddStringToObject(reader, "transport", s_hooks.transport_name ? s_hooks.transport_name() : "unknown");
    cJSON_AddBoolToObject(reader, "locked", s_hooks.lock_is_locked ? s_hooks.lock_is_locked() : true);

    cJSON *mqtt = cJSON_AddObjectToObject(data, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", s_hooks.mqtt_enabled ? s_hooks.mqtt_enabled() : false);
    cJSON_AddBoolToObject(mqtt, "connected", s_hooks.mqtt_connected ? s_hooks.mqtt_connected() : false);

    /* Called through the API rather than a hook: matter_lock stubs itself out
     * in a build without it, so this reports "unavailable" and costs a handful
     * of bytes rather than another five function pointers. */
    cJSON *matter = cJSON_AddObjectToObject(data, "matter");
    cJSON_AddBoolToObject(matter, "available", matter_lock_available());
    if (matter_lock_available()) {
        cJSON_AddBoolToObject(matter, "running", matter_lock_running());
        cJSON_AddNumberToObject(matter, "fabrics", matter_lock_fabric_count());
        cJSON_AddBoolToObject(matter, "reader_configured", matter_lock_reader_configured());
        cJSON_AddStringToObject(matter, "manual_code", matter_lock_manual_code());
        cJSON_AddStringToObject(matter, "qr_url", matter_lock_qr_url());
    }

    /* Which slot is running and which one an update would land in. Worth
     * showing: it is the difference between "the update took" and "the update
     * wrote somewhere and the device booted the old image anyway". */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    cJSON *ota = cJSON_AddObjectToObject(data, "ota");
    cJSON_AddStringToObject(ota, "running", running ? running->label : "unknown");
    cJSON_AddStringToObject(ota, "next", next ? next->label : "none");
    cJSON_AddNumberToObject(ota, "slot_size", next ? next->size : 0);
    esp_ota_img_states_t img_state;
    cJSON_AddBoolToObject(ota, "pending_verify",
                          running && esp_ota_get_state_partition(running, &img_state) == ESP_OK &&
                              img_state == ESP_OTA_IMG_PENDING_VERIFY);

    return send_json_response(req, true, "Device status", NULL, data);
}

static esp_err_t handle_get_hardware(httpd_req_t *req)
{
    char *hw_json = app_config_hardware_caps_json();
    cJSON *data = cJSON_Parse(hw_json);
    if (!data) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, "failed to parse hardware capabilities", NULL);
    }
    esp_err_t ret = send_json_response(req, true, "Hardware capabilities", NULL, data);
    free(hw_json);
    return ret;
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    char *cfg_json = app_config_to_json(app_config_get(), false);
    cJSON *data = cJSON_Parse(cfg_json);
    if (!data) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, "failed to parse configuration", NULL);
    }
    esp_err_t ret = send_json_response(req, true, "Current configuration", NULL, data);
    free(cfg_json);
    return ret;
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json_response(req, false, NULL, "empty or oversized request body", NULL);
    }

    /* Parse incoming config */
    cJSON *incoming = cJSON_Parse(body);
    free(body);
    if (!incoming) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json_response(req, false, NULL, "invalid JSON payload", NULL);
    }

    /* Start from the running config so a partial document is a patch */
    app_config_t candidate = *app_config_get();
    char reason[256] = {0};

    /* Validate incoming fields before applying */
    cJSON *field = incoming->child;
    while (field) {
        const char *key = field->string;

        /* Validate setup code if provided */
        if (strcmp(key, "group_id") == 0 && cJSON_IsString(field)) {
            if (!is_valid_setup_code(field->valuestring)) {
                snprintf(reason, sizeof(reason), 
                        "Invalid setup code: must be 8 digits, not all same, and not sequential");
                httpd_resp_set_status(req, "400 Bad Request");
                cJSON_Delete(incoming);
                return send_json_response(req, false, NULL, reason, NULL);
            }
        }

        /* Validate GPIO pin assignments */
        if (strstr(key, "_pin") && cJSON_IsNumber(field)) {
            uint8_t pin = (uint8_t)field->valueint;
            if (pin != 255 && !is_valid_gpio_pin(pin)) {
                snprintf(reason, sizeof(reason), "Invalid GPIO pin: %d", pin);
                httpd_resp_set_status(req, "400 Bad Request");
                cJSON_Delete(incoming);
                return send_json_response(req, false, NULL, reason, NULL);
            }
            if (is_reserved_gpio(pin)) {
                snprintf(reason, sizeof(reason), "GPIO %d is reserved for flash/PSRAM", pin);
                httpd_resp_set_status(req, "400 Bad Request");
                cJSON_Delete(incoming);
                return send_json_response(req, false, NULL, reason, NULL);
            }
        }

        field = field->next;
    }

    /* Apply validated JSON to config */
    char *json_str = cJSON_PrintUnformatted(incoming);
    esp_err_t err = app_config_from_json(json_str, &candidate, reason, sizeof(reason));
    free(json_str);
    cJSON_Delete(incoming);
    
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json_response(req, false, NULL, reason[0] ? reason : "config validation failed", NULL);
    }

    /* Save config */
    err = app_config_save(&candidate, reason, sizeof(reason));
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, reason[0] ? reason : "could not save configuration", NULL);
    }

    /* Publish config changed event */
    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "event", "config_changed");
    cJSON_AddStringToObject(event, "device_name", candidate.device_name);
    publish_event("config_changed", event);
    cJSON_Delete(event);

    ESP_LOGI(k_tag, "Configuration updated and saved");
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "restart_required", true);
    return send_json_response(req, true, "Configuration saved successfully", NULL, data);
}

static esp_err_t handle_post_config_reset(httpd_req_t *req)
{
    if (app_config_reset() != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, "could not erase configuration", NULL);
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "restart_required", true);
    return send_json_response(req, true, "Configuration reset, device rebooting", NULL, data);
}

static esp_err_t handle_get_logs(httpd_req_t *req)
{
    uint32_t since = 0;
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[16];
        if (httpd_query_key_value(query, "since", value, sizeof(value)) == ESP_OK) {
            since = (uint32_t)strtoul(value, NULL, 10);
        }
    }

    static log_line_t lines[32];
    const size_t count = log_ring_copy_since(since, lines, sizeof(lines) / sizeof(lines[0]));

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "next", log_ring_next_id());
    cJSON *array = cJSON_AddArrayToObject(data, "lines");
    for (size_t i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "id", lines[i].id);
        cJSON_AddStringToObject(entry, "text", lines[i].text);
        cJSON_AddItemToArray(array, entry);
    }

    return send_json_response(req, true, "Logs retrieved", NULL, data);
}

static esp_err_t handle_post_unlock(httpd_req_t *req)
{
    if (!s_hooks.unlock) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, "no lock output configured", NULL);
    }
    if (s_hooks.unlock() != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, "could not drive the lock output", NULL);
    }
    
    /* Publish lock event */
    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "event", "unlock_triggered");
    cJSON_AddNumberToObject(event, "timestamp", esp_timer_get_time() / 1000000);
    publish_event("lock_event", event);
    cJSON_Delete(event);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "unlocked", true);
    return send_json_response(req, true, "Lock activated", NULL, data);
}

static esp_err_t handle_post_matter_pair(httpd_req_t *req)
{
    /*
     * Only useful on a device that has already been commissioned once: an
     * uncommissioned one advertises on its own from boot. This is how a second
     * ecosystem gets added, and how a lock is recovered when the controller
     * that owned it is gone.
     */
    if (!matter_lock_available()) {
        httpd_resp_set_status(req, "501 Not Implemented");
        return send_json_response(req, false, NULL, "this firmware was built without Matter", NULL);
    }
    if (matter_lock_open_commissioning_window() != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, "could not open the commissioning window", NULL);
    }
    return send_json_response(req, true, "Pairing open", NULL, NULL);
}

static void reboot_task(void *params)
{
    (void)params;
    /* Let the response reach the browser. A second, not the half we had: the
     * request that triggers this usually arrives over the setup access point,
     * which is the slowest and least reliable link this device ever serves. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t handle_post_reboot(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "reboot_delay_ms", 500);
    const esp_err_t err = send_json_response(req, true, "Device rebooting", NULL, data);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return err;
}

/* --- WebSocket handlers --------------------------------------------------- */

static esp_err_t handle_ws_post_handshake(httpd_req_t *req);

static esp_err_t handle_websocket(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(k_tag, "WebSocket handshake from fd=%d", httpd_req_to_sockfd(req));
        /* ESP-IDF has no post-handshake callback on httpd_uri_t (5.4 and 5.5
         * both lack ws_post_handshake_cb), but it does call the handler once
         * with HTTP_GET when the handshake completes. That is this branch, so
         * the new client is registered here. */
        return handle_ws_post_handshake(req);
    }

    /* Receive WebSocket frame */
    httpd_ws_frame_t ws_pkt = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ws_pkt.len > k_max_ws_payload) {
        ESP_LOGE(k_tag, "WebSocket payload too large: %zu", ws_pkt.len);
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(ws_pkt.len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        /* Echo back the message for keepalive, or handle specific commands */
        cJSON *msg = cJSON_ParseWithLength((const char *)ws_pkt.payload, ws_pkt.len);
        if (msg) {
            cJSON *type_item = cJSON_GetObjectItem(msg, "type");
            if (type_item && type_item->valuestring) {
                if (strcmp(type_item->valuestring, "ping") == 0) {
                    cJSON *pong = cJSON_CreateObject();
                    cJSON_AddStringToObject(pong, "type", "pong");
                    char *resp = cJSON_PrintUnformatted(pong);
                    cJSON_Delete(pong);
                    if (resp) {
                        ws_queue_frame(httpd_req_to_sockfd(req), (uint8_t *)resp, strlen(resp), HTTPD_WS_TYPE_TEXT);
                        free(resp);
                    }
                }
            }
            cJSON_Delete(msg);
        }
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ws_remove_client(httpd_req_to_sockfd(req));
    }

    free(buf);
    return ESP_OK;
}

static esp_err_t handle_ws_post_handshake(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    ws_add_client(fd);

    /* Send initial status to new client */
    char *status = format_metrics_json();
    if (status) {
        ws_queue_frame(fd, (uint8_t *)status, strlen(status), HTTPD_WS_TYPE_TEXT);
        free(status);
    }

    /* Start status timer if needed */
    if (s_status_timer && !esp_timer_is_active(s_status_timer)) {
        esp_timer_start_periodic(s_status_timer, 5 * 1000 * 1000); /* 5 seconds */
    }

    return ESP_OK;
}

/* --- OTA handlers --------------------------------------------------------- */

typedef struct {
    uint32_t total_bytes;
    uint32_t written_bytes;
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    char error[128];
} ota_state_t;

static void ota_task(void *arg)
{
    ota_state_t *state = (ota_state_t *)arg;
    ESP_LOGI(k_tag, "OTA task completed: %"PRIu32" bytes written", state->written_bytes);
    free(state);
    vTaskDelete(NULL);
}

static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json_response(req, false, NULL, "empty request body", NULL);
    }

    /*
     * The upload itself needs very little: one 4 kB buffer, plus whatever
     * esp_ota_begin keeps while it erases the target partition. The 50 kB this
     * once demanded was a guess made on a build with 270 kB of heap free, and
     * on the Matter build -- which starts with about 170 kB and hands most of
     * it to chip, BLE and Wi-Fi -- that guess would refuse every update and
     * leave a USB cable as the only way back out. Guard against genuine
     * exhaustion, not against being busy.
     */
    if (!check_heap_available(16384, "OTA")) {
        httpd_resp_set_status(req, "507 Insufficient Storage");
        return send_json_response(req, false, NULL, "insufficient heap memory for OTA operation", NULL);
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, "no OTA partition found", NULL);
    }

    /* Refuse an image that cannot fit before erasing the slot that currently
     * holds a working one. esp_ota_write would otherwise fail somewhere in the
     * middle, leaving the spare partition half-written. */
    if (req->content_len > partition->size) {
        ESP_LOGE(k_tag, "image is %u bytes, partition '%s' holds %u", (unsigned)req->content_len, partition->label,
                 (unsigned)partition->size);
        httpd_resp_set_status(req, "413 Payload Too Large");
        return send_json_response(req, false, NULL, "firmware is larger than the OTA partition", NULL);
    }

    /* One at a time. Two uploads into the same partition interleave their
     * writes and produce an image that is neither. */
    static volatile bool s_ota_running;
    if (s_ota_running) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json_response(req, false, NULL, "an update is already in progress", NULL);
    }
    s_ota_running = true;

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(partition, req->content_len, &handle);
    if (err != ESP_OK) {
        s_ota_running = false;
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, "OTA begin failed", NULL);
    }

    ESP_LOGW(k_tag, "OTA started: %u bytes into '%s'", (unsigned)req->content_len, partition->label);

    /* From the heap and far larger than a TCP segment: at 1 kB this loop ran
     * thousands of times over a slow link, and every iteration is a write to
     * flash. */
    const size_t buf_size = 4096;
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        esp_ota_abort(handle);
        s_ota_running = false;
        httpd_resp_set_status(req, "507 Insufficient Storage");
        return send_json_response(req, false, NULL, "out of memory", NULL);
    }

    size_t remaining = req->content_len;
    size_t total_written = 0;
    size_t last_reported = 0;
    const char *failure = NULL;
    bool header_checked = false;

    while (remaining > 0) {
        const size_t want = remaining > buf_size ? buf_size : remaining;
        const int received = httpd_req_recv(req, (char *)buf, want);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            /* A stalled sender, not a failed one. Aborting here is what made
             * an upload over a weak Wi-Fi link fail at a random percentage. */
            continue;
        }
        if (received < 0) {
            failure = "connection lost during upload";
            break;
        }
        if (received == 0) {
            /* Nothing left, but content-length promised more: the client gave
             * up. Treating this as "keep going" spins forever. */
            failure = "upload ended early";
            break;
        }

        /*
         * Say which wrong file this is, before writing a byte of it.
         *
         * Every release ships two images whose names differ by one word, and
         * only one of them belongs here: <target>.firmware.bin is the app
         * alone, <target>.firmware.factory.bin is a full 4 MB flash dump for
         * offset 0x0. The factory image begins with 0xFF padding, so
         * esp_ota_write rejects it -- as "flash write failed", which sounds
         * like a hardware fault and sends people looking at the wrong thing.
         */
        if (!header_checked) {
            header_checked = true;

            if (buf[0] != ESP_IMAGE_HEADER_MAGIC) {
                ESP_LOGE(k_tag, "upload starts with 0x%02X, not an app image header", buf[0]);
                failure = "this is not an application image -- upload the firmware.bin, "
                          "not the firmware.factory.bin (that one is flashed over USB at 0x0)";
                break;
            }

            if ((size_t)received >= sizeof(esp_image_header_t)) {
                const esp_image_header_t *header = (const esp_image_header_t *)buf;
                if (header->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
                    ESP_LOGE(k_tag, "image is for chip id %u, this is %u", (unsigned)header->chip_id,
                             (unsigned)CONFIG_IDF_FIRMWARE_CHIP_ID);
                    failure = "this firmware was built for a different chip";
                    break;
                }
            }
        }

        if (esp_ota_write(handle, buf, received) != ESP_OK) {
            failure = "flash write failed";
            break;
        }
        total_written += received;
        remaining -= received;

        /* Progress in twentieths, over the WebSocket the UI already holds. The
         * send task owns the socket, so this does not block the upload. */
        if (total_written - last_reported >= req->content_len / 20 || remaining == 0) {
            last_reported = total_written;
            cJSON *progress = cJSON_CreateObject();
            cJSON_AddStringToObject(progress, "type", "ota");
            cJSON_AddNumberToObject(progress, "written", total_written);
            cJSON_AddNumberToObject(progress, "total", req->content_len);
            cJSON_AddNumberToObject(progress, "percent", (total_written * 100) / req->content_len);
            char *json = cJSON_PrintUnformatted(progress);
            cJSON_Delete(progress);
            if (json) {
                ws_broadcast((const uint8_t *)json, strlen(json), HTTPD_WS_TYPE_TEXT);
                free(json);
            }
        }
    }

    free(buf);

    if (!failure) {
        /* esp_ota_end is where a truncated or corrupt image is caught. Either
         * way it consumes the handle, so clear it before anything below can
         * reach the abort path -- aborting a handle esp_ota_end has already
         * released is a use-after-free, and set_boot_partition failing right
         * after a successful end is exactly how you get there. */
        const esp_err_t end_err = esp_ota_end(handle);
        handle = 0;
        if (end_err != ESP_OK) {
            failure = "the uploaded image failed validation";
        }
    }
    if (!failure && esp_ota_set_boot_partition(partition) != ESP_OK) {
        failure = "could not select the new image to boot";
    }

    if (failure) {
        if (handle) {
            esp_ota_abort(handle);
        }
        s_ota_running = false;
        ESP_LOGE(k_tag, "OTA failed after %u bytes: %s", (unsigned)total_written, failure);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, failure, NULL);
    }

    ESP_LOGW(k_tag, "OTA complete: %u bytes, booting '%s' next", (unsigned)total_written, partition->label);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "bytes_written", total_written);
    cJSON_AddNumberToObject(data, "reboot_delay_ms", 1000);
    const esp_err_t send_err = send_json_response(req, true, "Firmware updated, rebooting", NULL, data);

    /* Reboot from a task so the response is flushed first. s_ota_running is
     * deliberately left set: nothing else should start an upload into a
     * partition the device is about to boot from. */
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return send_err;
}

/* --- Captive Portal handlers ---------------------------------------------- */

static esp_err_t handle_captive_portal_redirect(httpd_req_t *req)
{
    /* An absolute URL, not "/": some connectivity checkers treat a relative
     * redirect as "the network works" and never open the portal.
     *
     * It has to be the access point's address specifically. A probe only
     * arrives from a client attached to the AP, and after a successful join
     * the station address is on a network that client cannot reach yet. */
    net_status_t net;
    net_manager_get_status(&net);

    char location[32];
    snprintf(location, sizeof(location), "http://%s/", net.ap_ip[0] ? net.ap_ip : "192.168.4.1");

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

#define WIFI_SCAN_MAX 24

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    net_scan_result_t *found = calloc(WIFI_SCAN_MAX, sizeof(*found));
    if (!found) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, "out of memory", NULL);
    }

    size_t count = 0;
    const esp_err_t err = net_manager_scan(found, WIFI_SCAN_MAX, &count);
    if (err != ESP_OK) {
        free(found);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_response(req, false, NULL, esp_err_to_name(err), NULL);
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(data, "networks");
    for (size_t i = 0; i < count; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", found[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", found[i].rssi);
        cJSON_AddNumberToObject(net, "channel", found[i].channel);
        cJSON_AddBoolToObject(net, "open", found[i].open);
        cJSON_AddItemToArray(networks, net);
    }
    cJSON_AddNumberToObject(data, "count", count);

    free(found);
    return send_json_response(req, true, "Networks in range", NULL, data);
}

/*
 * "Try again" from the portal, using the credentials already stored. This is
 * the way back after the reader has given up on a network that was only
 * temporarily gone -- a rebooting router, a breaker, a neighbour's microwave.
 */
static esp_err_t handle_wifi_reconnect(httpd_req_t *req)
{
    char ip[16] = {0};
    const esp_err_t err = net_manager_reconnect(12000, ip, sizeof(ip));

    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json_response(req, false, NULL, "no network has been configured yet", NULL);
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        return send_json_response(req, false, NULL, "still cannot reach that network", NULL);
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "ssid", app_config_get()->net.ssid);
    cJSON_AddStringToObject(data, "ip", ip);
    cJSON_AddBoolToObject(data, "saved", true);
    return send_json_response(req, true, "Reconnected", NULL, data);
}

/*
 * Join a network from the setup portal. The access point stays up throughout,
 * so this can answer with either the new address or the reason it failed --
 * the alternative is saving a possibly-wrong password, rebooting, and leaving
 * the user to work out from a dark board that the passphrase had a typo.
 */
static esp_err_t handle_wifi_connect(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json_response(req, false, NULL, "could not read the request", NULL);
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json_response(req, false, NULL, "malformed JSON", NULL);
    }

    const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json_response(req, false, NULL, "an SSID is required", NULL);
    }

    char ssid[33];
    char password[65];
    strlcpy(ssid, ssid_item->valuestring, sizeof(ssid));
    strlcpy(password, cJSON_IsString(pass_item) ? pass_item->valuestring : "", sizeof(password));
    cJSON_Delete(root);

    char ip[16] = {0};
    /* Per attempt, and net_manager_join makes two: a successful join lands in
     * about three seconds, so this bounds the whole request at ~24 s. */
    const esp_err_t err = net_manager_join(ssid, password, 12000, ip, sizeof(ip));
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        return send_json_response(req, false, NULL,
                                  err == ESP_ERR_TIMEOUT ? "no address from that network -- is it in range?"
                                                         : "the network refused those credentials",
                                  NULL);
    }

    /* Only now, with a working connection proven, are the credentials worth
     * keeping. Anything else stores a password that has never worked. */
    app_config_t cfg = *app_config_get();
    strlcpy(cfg.net.ssid, ssid, sizeof(cfg.net.ssid));
    strlcpy(cfg.net.password, password, sizeof(cfg.net.password));

    char reason[128] = {0};
    const esp_err_t save_err = app_config_save(&cfg, reason, sizeof(reason));

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "ssid", ssid);
    cJSON_AddStringToObject(data, "ip", ip);
    cJSON_AddBoolToObject(data, "saved", save_err == ESP_OK);
    if (save_err != ESP_OK) {
        ESP_LOGE(k_tag, "joined '%s' but could not store the credentials: %s", ssid, reason);
    }
    return send_json_response(req, true, "Connected", NULL, data);
}

/* --- lifecycle ----------------------------------------------------------- */

esp_err_t web_server_start(const web_server_hooks_t *hooks)
{
    ESP_RETURN_ON_FALSE(!s_server, ESP_ERR_INVALID_STATE, k_tag, "server already running");

    if (hooks) {
        s_hooks = *hooks;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 32; /* API, WebSocket, OTA, setup portal and OS connectivity probes */
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;

    /* Start capturing before the server does anything worth reading. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(log_ring_init());

    /* Initialize WebSocket infrastructure */
    s_ws_clients_mutex = xSemaphoreCreateMutex();
    if (!s_ws_clients_mutex) {
        ESP_LOGE(k_tag, "Failed to create WebSocket mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize session management */
    s_sessions_mutex = xSemaphoreCreateMutex();
    if (!s_sessions_mutex) {
        ESP_LOGE(k_tag, "Failed to create sessions mutex");
        vSemaphoreDelete(s_ws_clients_mutex);
        return ESP_ERR_NO_MEM;
    }
    memset(s_sessions, 0, sizeof(s_sessions));

    /* Initialize WebSocket client registry */
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        s_ws_clients[i].fd = -1;
    }
    s_ws_client_count = 0;

    /* Create WebSocket queue */
    s_ws_queue = xQueueCreate(16, sizeof(ws_frame_t *));
    if (!s_ws_queue) {
        ESP_LOGE(k_tag, "Failed to create WebSocket queue");
        vSemaphoreDelete(s_ws_clients_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Create WebSocket send task */
    if (xTaskCreate(ws_send_task, "ws_send", 4096, NULL, 3, &s_ws_task_handle) != pdPASS) {
        ESP_LOGE(k_tag, "Failed to create WebSocket send task");
        vQueueDelete(s_ws_queue);
        vSemaphoreDelete(s_ws_clients_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Create status timer for periodic metrics broadcast */
    esp_timer_create_args_t timer_args = {
        .callback = status_timer_callback,
        .arg = NULL,
        .name = "ws_status",
    };
    if (esp_timer_create(&timer_args, &s_status_timer) != ESP_OK) {
        ESP_LOGE(k_tag, "Failed to create status timer");
        vTaskDelete(s_ws_task_handle);
        vQueueDelete(s_ws_queue);
        vSemaphoreDelete(s_ws_clients_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), k_tag, "httpd_start failed");

    const httpd_uri_t routes[] = {
        /* API endpoints */
        {.uri = "/api/status", .method = HTTP_GET, .handler = handle_get_status, .is_websocket = false},
        {.uri = "/api/hardware", .method = HTTP_GET, .handler = handle_get_hardware, .is_websocket = false},
        {.uri = "/api/config", .method = HTTP_GET, .handler = handle_get_config, .is_websocket = false},
        {.uri = "/api/config", .method = HTTP_POST, .handler = handle_post_config, .is_websocket = false},
        {.uri = "/api/config/reset", .method = HTTP_POST, .handler = handle_post_config_reset, .is_websocket = false},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = handle_post_reboot, .is_websocket = false},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = handle_get_logs, .is_websocket = false},
        {.uri = "/api/unlock", .method = HTTP_POST, .handler = handle_post_unlock, .is_websocket = false},
        {.uri = "/api/matter/pair", .method = HTTP_POST, .handler = handle_post_matter_pair, .is_websocket = false},

        /* WebSocket endpoint */
        {.uri = "/api/ws", .method = HTTP_GET, .handler = handle_websocket, .is_websocket = true},

        /* OTA endpoint */
        {.uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota_upload, .is_websocket = false},

        /* Setup portal: list networks in range, then join one. /setup stays
         * reachable after the reader is on a network, so moving it to a
         * different one does not mean factory-resetting it first. */
        {.uri = "/setup", .method = HTTP_GET, .handler = handle_setup, .is_websocket = false},
        {.uri = "/api/wifi_scan", .method = HTTP_GET, .handler = handle_wifi_scan, .is_websocket = false},
        {.uri = "/api/wifi_connect", .method = HTTP_POST, .handler = handle_wifi_connect, .is_websocket = false},
        {.uri = "/api/wifi_reconnect", .method = HTTP_POST, .handler = handle_wifi_reconnect, .is_websocket = false},

        /*
         * Connectivity probes. Every OS fetches a known URL after joining a
         * network and decides from the answer whether to open its captive
         * portal window; a redirect is the answer that opens it. Serving the
         * catch-all UI instead technically works, but sends 40 kB to a probe
         * that only wanted a status line.
         */
        {.uri = "/generate_204", .method = HTTP_GET, .handler = handle_captive_portal_redirect, .is_websocket = false},
        {.uri = "/gen_204", .method = HTTP_GET, .handler = handle_captive_portal_redirect, .is_websocket = false},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handle_captive_portal_redirect, .is_websocket = false},
        {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = handle_captive_portal_redirect, .is_websocket = false},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = handle_captive_portal_redirect, .is_websocket = false},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = handle_captive_portal_redirect, .is_websocket = false},
        {.uri = "/canonical.html", .method = HTTP_GET, .handler = handle_captive_portal_redirect, .is_websocket = false},
        {.uri = "/success.txt", .method = HTTP_GET, .handler = handle_captive_portal_redirect, .is_websocket = false},
        {.uri = "/redirect", .method = HTTP_GET, .handler = handle_captive_portal_redirect, .is_websocket = false},

        /* Catch-all last: any other GET returns the UI */
        {.uri = "/*", .method = HTTP_GET, .handler = handle_index, .is_websocket = false},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const esp_err_t err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(k_tag, "route %s failed: %s", routes[i].uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(k_tag, "configuration UI listening on port %d (WebSocket enabled)", cfg.server_port);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }

    /* Stop status timer */
    if (s_status_timer) {
        esp_timer_stop(s_status_timer);
        esp_timer_delete(s_status_timer);
        s_status_timer = NULL;
    }

    /* Stop HTTP server */
    const esp_err_t err = httpd_stop(s_server);
    s_server = NULL;

    /* Stop WebSocket send task */
    if (s_ws_task_handle) {
        vTaskDelete(s_ws_task_handle);
        s_ws_task_handle = NULL;
    }

    /* Delete WebSocket queue */
    if (s_ws_queue) {
        vQueueDelete(s_ws_queue);
        s_ws_queue = NULL;
    }

    /* Delete WebSocket clients mutex */
    if (s_ws_clients_mutex) {
        vSemaphoreDelete(s_ws_clients_mutex);
        s_ws_clients_mutex = NULL;
    }

    /* Delete sessions mutex */
    if (s_sessions_mutex) {
        vSemaphoreDelete(s_sessions_mutex);
        s_sessions_mutex = NULL;
    }

    return err;
}
