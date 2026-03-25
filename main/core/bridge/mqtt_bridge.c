/**
 * @file mqtt_bridge.c
 * @brief MQTT Bridge Core Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "mqtt_bridge.h"
#include "device_state_publisher.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "zigbee/zb_command_handler.h"
#include "ha_discovery_ng.h"
#include "ha_bridge_discovery.h"
#include "bridge_request_handler.h"
#include "bridge_events.h"
#include "mqtt_event_handler.h"
#include "gateway_mqtt.h"
#include "gateway_defaults.h"
#include "mqtt/mqtt_topics.h"
#include "utils/json_utils.h"
#include "core/device/device_registry.h"
#include "zigbee/zb_coordinator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include <string.h>

static const char *TAG = "MQTT_BRIDGE";

/* Bridge state mutex for start/stop transitions */
static SemaphoreHandle_t s_bridge_mutex = NULL;

/* Bridge state */
static bool s_bridge_initialized = false;
static bool s_bridge_starting = false;  /* Prevents re-entry during long startup */
static bool s_bridge_enabled = false;
static bool s_initial_discovery_complete = false;
static bridge_stats_t s_stats = {0};

/* Forward declarations */
static void mqtt_message_callback(const char *topic, const char *data, size_t data_len);

esp_err_t mqtt_bridge_init(void)
{
    ESP_LOGI(TAG, "Initializing MQTT bridge...");

    /* Create bridge state mutex */
    if (s_bridge_mutex == NULL) {
        s_bridge_mutex = xSemaphoreCreateMutex();
        if (s_bridge_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create bridge mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Check prerequisites */
    if (!mqtt_client_is_connected()) {
        ESP_LOGW(TAG, "MQTT not connected - bridge will start when connection is established");
    }

    if (!zb_coordinator_is_running()) {
        ESP_LOGW(TAG, "Zigbee coordinator not running - bridge functionality limited");
    }

    /* Initialize sub-modules */
    esp_err_t ret;

    ret = device_state_publisher_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize device state publisher: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = command_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize command handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ha_discovery_ng_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means already initialized (from foundation_init) - that's OK */
        ESP_LOGE(TAG, "Failed to initialize HA discovery: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ha_bridge_discovery_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize HA bridge discovery: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bridge_request_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bridge request handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bridge_events_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bridge events: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mqtt_event_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MQTT event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Reset statistics */
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.enabled = false;

    s_bridge_initialized = true;
    ESP_LOGI(TAG, "MQTT bridge initialized successfully");
    return ESP_OK;
}

void mqtt_bridge_deinit(void)
{
    if (!s_bridge_initialized) {
        return;
    }

    /* Stop if running */
    if (s_bridge_enabled) {
        mqtt_bridge_stop();
    }

    /* Deinit sub-modules to unsubscribe event handlers */
    bridge_events_deinit();
    device_state_publisher_deinit();
    mqtt_event_handler_deinit();

    if (s_bridge_mutex != NULL) {
        vSemaphoreDelete(s_bridge_mutex);
        s_bridge_mutex = NULL;
    }

    s_bridge_initialized = false;
    ESP_LOGI(TAG, "MQTT bridge deinitialized");
}

esp_err_t mqtt_bridge_start(void)
{
    if (!s_bridge_initialized) {
        ESP_LOGW(TAG, "Bridge not initialized yet - skipping start");
        return ESP_ERR_INVALID_STATE;
    }

    /* Synchronize state transitions */
    if (xSemaphoreTake(s_bridge_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGE(TAG, "Bridge mutex timeout in start");
        return ESP_ERR_TIMEOUT;
    }

    if (s_bridge_enabled) {
        ESP_LOGD(TAG, "Bridge already started");
        xSemaphoreGive(s_bridge_mutex);
        return ESP_OK;
    }

    /* Prevent re-entry during long startup (discovery takes ~7 seconds) */
    if (s_bridge_starting) {
        ESP_LOGD(TAG, "Bridge startup already in progress");
        xSemaphoreGive(s_bridge_mutex);
        return ESP_OK;
    }
    s_bridge_starting = true;
    xSemaphoreGive(s_bridge_mutex);

    ESP_LOGI(TAG, "Starting MQTT bridge...");

    /* Register MQTT message callback */
    esp_err_t ret = mqtt_client_register_callback(mqtt_message_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT callback: %s", esp_err_to_name(ret));
        xSemaphoreTake(s_bridge_mutex, GW_TIMEOUT_LONG_TICKS);
        s_bridge_starting = false;
        xSemaphoreGive(s_bridge_mutex);
        return ret;
    }

    /* Subscribe to command topics */
    mqtt_client_subscribe(TOPIC_DEVICE_SET_PATTERN, 0);
    mqtt_client_subscribe(TOPIC_DEVICE_GET_PATTERN, 0);
    mqtt_client_subscribe(TOPIC_BRIDGE_REQUEST, 0);

    /* Publish bridge info */
    mqtt_bridge_publish_bridge_info();

    /* Publish device list */
    mqtt_bridge_publish_device_list();

    /* Availability for all devices is published by mqtt_adapter.c
     * (republish_all_device_states) on EVT_MQTT_CONNECTED, which
     * correctly uses the /availability topic with retained flag. */

    /* Publish Home Assistant discovery for all devices */
    vTaskDelay(pdMS_TO_TICKS(GW_WIFI_STABILIZE_DELAY_MS)); /* Small delay before discovery flood */
    ha_discovery_ng_publish_all();

    /* Publish Home Assistant discovery for bridge entities (sensors, buttons, etc.) */
    ha_bridge_discovery_publish_all();

    /* Publish FULL bridge state for HA sensors (contains all fields like device_count, channel, etc.)
     * IMPORTANT: This must be the ONLY retained publish to zigbee2mqtt/bridge/state
     * The discovery templates extract values from this JSON payload */
    ha_bridge_publish_state();

    xSemaphoreTake(s_bridge_mutex, GW_TIMEOUT_LONG_TICKS);
    s_bridge_enabled = true;
    s_bridge_starting = false;
    s_stats.enabled = true;
    s_initial_discovery_complete = true;
    xSemaphoreGive(s_bridge_mutex);

    ESP_LOGI(TAG, "MQTT bridge started successfully");
    return ESP_OK;
}

esp_err_t mqtt_bridge_stop(void)
{
    /* Synchronize state transitions */
    if (xSemaphoreTake(s_bridge_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGE(TAG, "Bridge mutex timeout in stop");
        return ESP_ERR_TIMEOUT;
    }

    if (!s_bridge_enabled) {
        ESP_LOGW(TAG, "Bridge not running");
        xSemaphoreGive(s_bridge_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_bridge_mutex);

    ESP_LOGI(TAG, "Stopping MQTT bridge...");

    /* Device availability is handled by mqtt_adapter.c on disconnect.
     * Bridge-level availability is handled by LWT. */

    /* Publish bridge state: offline */
    mqtt_bridge_publish_bridge_state("offline");

    /* Unsubscribe from topics */
    mqtt_client_unsubscribe(TOPIC_DEVICE_SET_PATTERN);
    mqtt_client_unsubscribe(TOPIC_DEVICE_GET_PATTERN);
    mqtt_client_unsubscribe(TOPIC_BRIDGE_REQUEST);

    xSemaphoreTake(s_bridge_mutex, GW_TIMEOUT_LONG_TICKS);
    s_bridge_enabled = false;
    s_stats.enabled = false;
    xSemaphoreGive(s_bridge_mutex);

    ESP_LOGI(TAG, "MQTT bridge stopped");
    return ESP_OK;
}

/**
 * @brief Republish all discovery messages after reconnect
 *
 * Staggered publishing to avoid queue overflow.
 * Called from mqtt_bridge_resubscribe() after initial discovery is complete.
 */
static void mqtt_bridge_republish_discovery(void)
{
    if (!s_initial_discovery_complete) {
        ESP_LOGD(TAG, "Initial discovery not complete - skipping republish");
        return;
    }

    ESP_LOGI(TAG, "Republishing discovery after reconnect...");

    /* Initial delay to let state messages drain */
    vTaskDelay(pdMS_TO_TICKS(GW_RECONNECT_DISCOVERY_DELAY_MS));

    /* Device availability is republished by mqtt_adapter.c
     * (republish_all_device_states) on EVT_MQTT_CONNECTED. */

    /* Republish bridge entity discovery */
    ha_bridge_discovery_publish_all();
    vTaskDelay(pdMS_TO_TICKS(GW_RECONNECT_DISCOVERY_DELAY_MS));

    /* Invalidate cache so ALL devices get re-published.
     * If broker restarted, retained messages are gone. */
    ha_discovery_invalidate_cache();

    /* Republish device discovery */
    ha_discovery_ng_publish_all();

    /* Republish bridge state for HA sensors */
    ha_bridge_publish_state();

    ESP_LOGI(TAG, "Discovery republish complete");
}

esp_err_t mqtt_bridge_resubscribe(void)
{
    if (!s_bridge_initialized || !s_bridge_enabled) {
        ESP_LOGW(TAG, "Bridge not initialized/enabled - cannot resubscribe");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Re-subscribing to MQTT topics after reconnect...");

    /* Re-subscribe to command topics */
    mqtt_client_subscribe(TOPIC_DEVICE_SET_PATTERN, 0);
    mqtt_client_subscribe(TOPIC_DEVICE_GET_PATTERN, 0);
    mqtt_client_subscribe(TOPIC_BRIDGE_REQUEST, 0);

    /* Re-publish bridge info */
    mqtt_bridge_publish_bridge_info();

    /* Re-publish device list */
    mqtt_bridge_publish_device_list();

    /* Re-publish all discovery (staggered to avoid queue overflow)
     * This also republishes the full bridge state at the end */
    mqtt_bridge_republish_discovery();

    ESP_LOGI(TAG, "MQTT topics re-subscribed successfully");
    return ESP_OK;
}

esp_err_t mqtt_bridge_publish_device_state(uint16_t short_addr)
{
    if (!s_bridge_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Publish device state via event bus */
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        s_stats.error_count++;
        return ESP_ERR_NOT_FOUND;
    }

    evt_device_state_t evt = {
        .ieee_addr = device->id,
        .short_addr = short_addr,
        .endpoint = device->proto.zigbee.endpoint,
        .cluster_id = 0,
        .attr_id = 0,
        .json_state = NULL,
    };
    esp_err_t ret = event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));

    if (ret == ESP_OK) {
        s_stats.publish_count++;
    } else {
        s_stats.error_count++;
    }

    return ret;
}

esp_err_t mqtt_bridge_publish_bridge_state(const char *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json_str = json_create_bridge_state(state);
    if (json_str == NULL) {
        return ESP_FAIL;
    }

    /* Publish via Event Bus (TRANSFER ownership: handler frees topic + payload) */
    char *topic = strdup(TOPIC_BRIDGE_STATE);
    if (topic == NULL) {
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    evt_bridge_publish_t evt = {
        .topic = topic,
        .payload_json = json_str,
        .qos = 1,
        .retain = true,
    };
    esp_err_t ret = event_publish(EVT_BRIDGE_PUBLISH, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        /* Handler won't run — caller must free */
        free(topic);
        free(json_str);
        s_stats.error_count++;
    } else {
        ESP_LOGI(TAG, "Published bridge state: %s", state);
        s_stats.publish_count++;
    }

    return ret;
}

esp_err_t mqtt_bridge_publish_bridge_info(void)
{
    cJSON *info = json_create_bridge_info();
    if (info == NULL) {
        return ESP_FAIL;
    }

    char *json_str = json_to_string_and_delete(info);
    if (json_str == NULL) {
        return ESP_FAIL;
    }

    /* Publish via Event Bus (TRANSFER ownership: handler frees topic + payload) */
    char *topic = strdup(TOPIC_BRIDGE_INFO);
    if (topic == NULL) {
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    evt_bridge_publish_t evt = {
        .topic = topic,
        .payload_json = json_str,
        .qos = 1,
        .retain = true,
    };
    esp_err_t ret = event_publish(EVT_BRIDGE_PUBLISH, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        free(topic);
        free(json_str);
        s_stats.error_count++;
    } else {
        ESP_LOGI(TAG, "Published bridge info");
        s_stats.publish_count++;
    }

    return ret;
}

esp_err_t mqtt_bridge_publish_device_list(void)
{
    cJSON *list = json_create_device_list();
    if (list == NULL) {
        return ESP_FAIL;
    }

    char *json_str = json_to_string_and_delete(list);
    if (json_str == NULL) {
        return ESP_FAIL;
    }

    /* Publish via Event Bus (TRANSFER ownership: handler frees topic + payload) */
    char *topic = strdup(TOPIC_BRIDGE_DEVICES);
    if (topic == NULL) {
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    evt_bridge_publish_t evt = {
        .topic = topic,
        .payload_json = json_str,
        .qos = 1,
        .retain = true,
    };
    esp_err_t ret = event_publish(EVT_BRIDGE_PUBLISH, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        free(topic);
        free(json_str);
        s_stats.error_count++;
    } else {
        ESP_LOGI(TAG, "Published device list");
        s_stats.publish_count++;
    }

    return ret;
}

esp_err_t mqtt_bridge_handle_command(const char *topic, const char *payload, size_t len)
{
    if (topic == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_stats.command_count++;

    /* Check if it's a bridge request */
    if (strstr(topic, "bridge/request") != NULL) {
        esp_err_t ret = bridge_request_process(topic, payload, len);
        if (ret != ESP_OK) {
            s_stats.error_count++;
        }
        return ret;
    }

    /* Check if it's a device command (../set or ../get) */
    if (strstr(topic, "/set") != NULL) {
        esp_err_t ret = command_handler_process(topic, payload, len);
        if (ret != ESP_OK) {
            s_stats.error_count++;
        }
        return ret;
    }

    /* Check if it's a device get request */
    if (strstr(topic, "/get") != NULL) {
        /* Extract friendly name and publish current state via event bus */
        char friendly_name[MQTT_MEDIUM_STR_MAX_LEN];
        if (mqtt_topic_extract_friendly_name(topic, friendly_name, sizeof(friendly_name)) == ESP_OK) {
            /* Find device by name directly using NG registry */
            device_t *device = device_registry_get_by_name(friendly_name);
            if (device != NULL) {
                /* Publish via event bus - handler reads from device_registry_get_state() */
                evt_device_state_t evt = {
                    .ieee_addr = device->id,
                    .short_addr = device->proto.zigbee.short_addr,
                    .endpoint = device->proto.zigbee.endpoint,
                    .cluster_id = 0,
                    .attr_id = 0,
                    .json_state = NULL,
                };
                return event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));
            }
        }
        s_stats.error_count++;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGD(TAG, "Unhandled MQTT message: %s", topic);
    return ESP_OK;
}

esp_err_t mqtt_bridge_on_device_join(uint16_t short_addr)
{
    ESP_LOGI(TAG, "Device joined: 0x%04X", short_addr);
    s_stats.device_join_count++;

    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        ESP_LOGE(TAG, "Device 0x%04X not found in registry", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    /* Publish availability: online via event bus */
    evt_device_state_t avail_evt = {
        .ieee_addr = device->id,
        .short_addr = short_addr,
        .endpoint = device->proto.zigbee.endpoint,
        .cluster_id = 0,
        .attr_id = 0,
        .json_state = NULL,
    };
    event_publish(EVT_DEVICE_STATE_CHANGED, &avail_evt, sizeof(avail_evt));

    /* Publish initial state via event bus - handler reads from device_registry_get_state() */
    evt_device_state_t state_evt = {
        .ieee_addr = device->id,
        .short_addr = short_addr,
        .endpoint = device->proto.zigbee.endpoint,
        .cluster_id = 0,
        .attr_id = 0,
        .json_state = NULL,
    };
    event_publish(EVT_DEVICE_STATE_CHANGED, &state_evt, sizeof(state_evt));

    /* Publish device list update */
    mqtt_bridge_publish_device_list();

    /* NOTE: HA discovery is NOT published here - it's published AFTER the interview
     * completes and device type is correctly determined. Publishing discovery here
     * with UNKNOWN device type causes duplicate entities in Home Assistant when
     * the interview later publishes the correct type. See zb_interview.c line ~1273 */

    ESP_LOGI(TAG, "Published join notifications for device: %s", device->friendly_name);
    return ESP_OK;
}

esp_err_t mqtt_bridge_on_device_leave(uint16_t short_addr)
{
    ESP_LOGI(TAG, "Device left: 0x%04X", short_addr);
    s_stats.device_leave_count++;

    /* Publish availability: offline via event bus */
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device != NULL) {
        evt_device_state_t evt = {
            .ieee_addr = device->id,
            .short_addr = short_addr,
            .endpoint = device->proto.zigbee.endpoint,
            .cluster_id = 0,
            .attr_id = 0,
            .json_state = NULL,
        };
        event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));
    }

    /* Update device list */
    mqtt_bridge_publish_device_list();

    return ESP_OK;
}

esp_err_t mqtt_bridge_on_attribute_change(uint16_t short_addr,
                                         uint16_t cluster_id,
                                         uint16_t attr_id,
                                         const void *value,
                                         size_t value_len)
{
    if (!s_bridge_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Attribute change: 0x%04X cluster=0x%04X attr=0x%04X",
             short_addr, cluster_id, attr_id);

    /* Publish attribute update */
    esp_err_t ret = device_state_publish_attribute(short_addr, cluster_id, attr_id, value, value_len);
    if (ret == ESP_OK) {
        s_stats.publish_count++;
    } else {
        s_stats.error_count++;
    }

    return ret;
}

bridge_stats_t mqtt_bridge_get_stats(void)
{
    return s_stats;
}

bool mqtt_bridge_is_enabled(void)
{
    return s_bridge_enabled;
}

bool mqtt_bridge_is_discovery_complete(void)
{
    return s_initial_discovery_complete;
}

esp_err_t mqtt_bridge_test(void)
{
    ESP_LOGI(TAG, "Running MQTT bridge test...");

    /* Test 1: Initialize */
    esp_err_t ret = mqtt_bridge_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bridge initialization failed");
        return ESP_FAIL;
    }

    /* Test 2: Check statistics */
    bridge_stats_t stats = mqtt_bridge_get_stats();
    ESP_LOGI(TAG, "Bridge stats: enabled=%d, publishes=%lu, commands=%lu, errors=%lu",
             stats.enabled, stats.publish_count, stats.command_count, stats.error_count);

    ESP_LOGI(TAG, "MQTT bridge test PASSED");
    return ESP_OK;
}

/* Internal MQTT message callback */
static void mqtt_message_callback(const char *topic, const char *data, size_t data_len)
{
    if (topic == NULL || data == NULL) {
        return;
    }

    ESP_LOGD(TAG, "MQTT message received: %s", topic);

    /* Handle message */
    mqtt_bridge_handle_command(topic, data, data_len);
}
