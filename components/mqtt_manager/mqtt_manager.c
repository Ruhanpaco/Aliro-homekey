/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mqtt_manager.h"

#include "access_control.h"

#include <cJSON.h>
#include <esp_check.h>
#include <esp_log.h>
#include <mqtt_client.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const k_tag = "aliro/mqtt";

#define TOPIC_STATUS     "status"
#define TOPIC_LOCK_STATE "lock/state"
#define TOPIC_LOCK_SET   "lock/set"
#define TOPIC_AUTH       "auth"

static struct {
    mqtt_config_t cfg;
    char device_name[32];
    esp_mqtt_client_handle_t client;
    bool connected;
} s_mqtt;

static void topic(const char *suffix, char *out, size_t out_len)
{
    app_config_mqtt_topic(&s_mqtt.cfg, suffix, out, out_len);
}

static void publish(const char *suffix, const char *payload, int qos, int retain)
{
    if (!s_mqtt.client || !s_mqtt.connected) {
        return;
    }
    char full[96];
    topic(suffix, full, sizeof(full));
    esp_mqtt_client_publish(s_mqtt.client, full, payload, 0, qos, retain);
}

static void publish_lock_state(bool locked)
{
    publish(TOPIC_LOCK_STATE, locked ? "locked" : "unlocked", 1, 1);
}

/* --- Home Assistant discovery -------------------------------------------- */

static void publish_discovery(void)
{
    if (!s_mqtt.cfg.ha_discovery) {
        return;
    }

    char state_topic[96], command_topic[96], availability[96];
    topic(TOPIC_LOCK_STATE, state_topic, sizeof(state_topic));
    topic(TOPIC_LOCK_SET, command_topic, sizeof(command_topic));
    topic(TOPIC_STATUS, availability, sizeof(availability));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Lock");
    cJSON_AddStringToObject(root, "unique_id", s_mqtt.cfg.client_id);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "availability_topic", availability);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    cJSON_AddStringToObject(root, "state_locked", "locked");
    cJSON_AddStringToObject(root, "state_unlocked", "unlocked");
    cJSON_AddStringToObject(root, "payload_lock", "LOCK");
    cJSON_AddStringToObject(root, "payload_unlock", "UNLOCK");

    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON *ids = cJSON_AddArrayToObject(device, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_mqtt.cfg.client_id));
    cJSON_AddStringToObject(device, "name", s_mqtt.device_name);
    cJSON_AddStringToObject(device, "manufacturer", "Aliro HomeKey");
    cJSON_AddStringToObject(device, "model", "ESP32 Aliro Reader");

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return;
    }

    char discovery_topic[128];
    snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/lock/%s/config", s_mqtt.cfg.client_id);
    esp_mqtt_client_publish(s_mqtt.client, discovery_topic, payload, 0, 1, 1);
    free(payload);

    ESP_LOGI(k_tag, "published Home Assistant discovery for '%s'", s_mqtt.cfg.client_id);
}

/* --- access events ------------------------------------------------------- */

static void on_access_event(const access_event_t *event, void *ctx)
{
    (void)ctx;

    if (event->type == ACCESS_EVENT_LOCK_STATE) {
        publish_lock_state(event->locked);
        return;
    }

    if (!s_mqtt.cfg.publish_taps) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "granted", event->granted);
    cJSON_AddStringToObject(root, "reason", event->reason ? event->reason : "");
    cJSON_AddStringToObject(root, "credential", event->credential);
    cJSON_AddStringToObject(root, "key_slot", event->key_slot_hex);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload) {
        publish(TOPIC_AUTH, payload, 1, 0);
        free(payload);
    }
}

/* --- client -------------------------------------------------------------- */

static void handle_command(const char *data, int len)
{
    /* Payloads are the Home Assistant lock defaults. */
    if (len == 6 && strncasecmp(data, "UNLOCK", 6) == 0) {
        ESP_LOGI(k_tag, "unlock commanded over MQTT");
        ESP_ERROR_CHECK_WITHOUT_ABORT(access_control_unlock());
    } else if (len == 4 && strncasecmp(data, "LOCK", 4) == 0) {
        /* The lock relocks on its own timer; report the current state so a
         * broker that asked for LOCK sees an answer either way. */
        publish_lock_state(access_control_is_locked());
    } else {
        ESP_LOGW(k_tag, "ignoring unknown command (%d bytes)", len);
    }
}

static void on_mqtt_event(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args;
    (void)base;

    const esp_mqtt_event_handle_t event = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_mqtt.connected = true;
        ESP_LOGI(k_tag, "connected to %s:%u", s_mqtt.cfg.broker, (unsigned)s_mqtt.cfg.port);

        char full[96];
        topic(TOPIC_STATUS, full, sizeof(full));
        esp_mqtt_client_publish(s_mqtt.client, full, "online", 0, 1, 1);

        topic(TOPIC_LOCK_SET, full, sizeof(full));
        esp_mqtt_client_subscribe(s_mqtt.client, full, 1);

        publish_discovery();
        publish_lock_state(access_control_is_locked());
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt.connected = false;
        ESP_LOGW(k_tag, "disconnected");
        break;

    case MQTT_EVENT_DATA:
        handle_command(event->data, event->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(k_tag, "connection error");
        break;

    default:
        break;
    }
}

esp_err_t mqtt_manager_start(const mqtt_config_t *cfg, const char *device_name)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, k_tag, "no MQTT configuration");

    if (!cfg->enabled) {
        ESP_LOGI(k_tag, "disabled");
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(!s_mqtt.client, ESP_ERR_INVALID_STATE, k_tag, "already started");

    s_mqtt.cfg = *cfg;
    snprintf(s_mqtt.device_name, sizeof(s_mqtt.device_name), "%s", device_name ? device_name : "aliro-homekey");

    char uri[128];
    snprintf(uri, sizeof(uri), "%s://%s:%u", cfg->use_ssl ? "mqtts" : "mqtt", cfg->broker, (unsigned)cfg->port);

    char lwt_topic[96];
    topic(TOPIC_STATUS, lwt_topic, sizeof(lwt_topic));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = cfg->client_id,
        .session.last_will =
            {
                .topic = lwt_topic,
                .msg = "offline",
                .msg_len = 7,
                .qos = 1,
                .retain = 1,
            },
        .session.keepalive = 30,
    };

    if (cfg->username[0] != '\0') {
        mqtt_cfg.credentials.username = cfg->username;
        mqtt_cfg.credentials.authentication.password = cfg->password;
    }
    if (cfg->use_ssl && cfg->allow_insecure) {
        /* The user asked for this explicitly in the UI; it is the only way to
         * reach a broker with a self-signed certificate without a cert store. */
        mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
        mqtt_cfg.broker.verification.use_global_ca_store = false;
        ESP_LOGW(k_tag, "TLS certificate verification is disabled");
    }

    s_mqtt.client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_mqtt.client, ESP_FAIL, k_tag, "client init failed");

    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_mqtt.client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL), k_tag,
                        "event registration failed");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_mqtt.client), k_tag, "client start failed");

    (void)access_control_add_observer(on_access_event, NULL);

    ESP_LOGI(k_tag, "connecting to %s, base topic '%s'", uri, cfg->base_topic);
    return ESP_OK;
}

esp_err_t mqtt_manager_stop(void)
{
    if (!s_mqtt.client) {
        return ESP_OK;
    }
    access_control_remove_observer(on_access_event);
    esp_mqtt_client_stop(s_mqtt.client);
    esp_mqtt_client_destroy(s_mqtt.client);
    s_mqtt.client = NULL;
    s_mqtt.connected = false;
    return ESP_OK;
}

bool mqtt_manager_is_connected(void)
{
    return s_mqtt.connected;
}

bool mqtt_manager_is_enabled(void)
{
    return s_mqtt.cfg.enabled;
}
