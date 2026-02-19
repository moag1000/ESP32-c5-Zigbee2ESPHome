/**
 * @file ha_bridge_discovery.c
 * @brief Home Assistant Bridge Discovery Implementation
 *
 * Publishes Home Assistant MQTT discovery messages for the bridge device itself,
 * exposing diagnostic sensors and control entities.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ha_bridge_discovery.h"
#include "ha_constants.h"
#include "core/device/device_registry.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "gateway_defaults.h"
#include "gateway_mqtt.h"
#include "mqtt/mqtt_helpers.h"
#include "mqtt/mqtt_topics.h"
#include "wifi/wifi_manager.h"
#include "zigbee/zb_coordinator.h"
#include "zigbee/zb_device_handler_types.h"
#include "utils/version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_sntp.h"
#include "crash_reporter.h"
#if CONFIG_BT_SCANNER_ENABLED
#include "bluetooth/ble_scanner.h"
#endif
#if CONFIG_ESPHOME_API_ENABLE
#include "esphome/esphome_api.h"
#include "esphome/esphome_ble_proxy.h"
#endif

static const char *TAG = "HA_BRIDGE";

/* ============================================================================
 * Module State
 * ============================================================================ */

/** @brief Module initialized flag */
static bool s_initialized = false;

/** @brief Current log level */
static const char *s_log_level = "info";

/** @brief System start time in microseconds */
static int64_t s_start_time_us = 0;

/* ============================================================================
 * Helper Macros
 * ============================================================================ */

/**
 * @brief Check cJSON pointer and goto cleanup if NULL
 */
#define CHECK_CJSON_GOTO(ptr, label) do { \
    if ((ptr) == NULL) { \
        ESP_LOGE(TAG, "cJSON allocation failed at %s:%d", __FILE__, __LINE__); \
        goto label; \
    } \
} while(0)

/* ============================================================================
 * Bridge Device JSON Helper
 * ============================================================================ */

/**
 * @brief Create bridge device info JSON object
 *
 * Creates the "device" object for Home Assistant discovery messages.
 *
 * @return cJSON object pointer or NULL on error
 */
static cJSON* create_bridge_device_info(void)
{
    cJSON *device = cJSON_CreateObject();
    if (device == NULL) {
        return NULL;
    }

    /* Device identifiers array */
    cJSON *identifiers = cJSON_CreateArray();
    if (identifiers == NULL) {
        cJSON_Delete(device);
        return NULL;
    }
    cJSON *id_item = cJSON_CreateString(HA_BRIDGE_DEVICE_ID);
    if (id_item == NULL) {
        cJSON_Delete(identifiers);
        cJSON_Delete(device);
        return NULL;
    }
    cJSON_AddItemToArray(identifiers, id_item);
    cJSON_AddItemToObject(device, "identifiers", identifiers);

    /* Device info strings */
    cJSON_AddStringToObject(device, "name", HA_BRIDGE_DEVICE_NAME);
    cJSON_AddStringToObject(device, "model", HA_BRIDGE_DEVICE_MODEL);
    cJSON_AddStringToObject(device, "manufacturer", HA_BRIDGE_DEVICE_MANUFACTURER);
    cJSON_AddStringToObject(device, "sw_version", FIRMWARE_VERSION);

    return device;
}

/**
 * @brief Create base discovery config with common fields
 *
 * @param[in] name Entity name
 * @param[in] unique_id_suffix Suffix for unique ID (appended to bridge ID)
 * @return cJSON object pointer or NULL on error
 */
static cJSON* create_base_discovery_config(const char *name, const char *unique_id_suffix)
{
    cJSON *config = cJSON_CreateObject();
    if (config == NULL) {
        return NULL;
    }

    /* Name */
    cJSON_AddStringToObject(config, "name", name);

    /* Unique ID */
    char unique_id[GW_BUFFER_SIZE_SMALL];
    snprintf(unique_id, sizeof(unique_id), "%s_%s", HA_BRIDGE_DEVICE_ID, unique_id_suffix);
    cJSON_AddStringToObject(config, "unique_id", unique_id);

    /* State topic */
    cJSON_AddStringToObject(config, "state_topic", HA_BRIDGE_STATE_TOPIC);

    /* Availability */
    cJSON_AddStringToObject(config, "availability_topic", HA_BRIDGE_AVAILABILITY_TOPIC);
    cJSON_AddStringToObject(config, "availability_template",
                            "{{ 'online' if value_json.state == 'online' else 'offline' }}");

    /* Device info */
    cJSON *device = create_bridge_device_info();
    if (device == NULL) {
        cJSON_Delete(config);
        return NULL;
    }
    cJSON_AddItemToObject(config, "device", device);

    return config;
}

/**
 * @brief Publish discovery config to MQTT
 *
 * @param[in] component HA component type ("sensor", "binary_sensor", "switch", etc.)
 * @param[in] unique_id_suffix Suffix for topic and unique ID
 * @param[in] config cJSON config object (will be deleted on success)
 * @return ESP_OK on success
 */
static esp_err_t publish_discovery_config(const char *component,
                                          const char *unique_id_suffix,
                                          cJSON *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build discovery topic */
    char topic[MQTT_TOPIC_MAX_LEN];
    char entity_id[GW_BUFFER_SIZE_SMALL];
    snprintf(entity_id, sizeof(entity_id), "%s_%s", HA_BRIDGE_DEVICE_ID, unique_id_suffix);

    esp_err_t ret = mqtt_topic_ha_discovery(component, entity_id, topic, sizeof(topic));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build discovery topic for %s", unique_id_suffix);
        cJSON_Delete(config);
        return ret;
    }

    /* Convert to string and publish */
    char *json_str = cJSON_PrintUnformatted(config);
    cJSON_Delete(config);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize discovery config for %s", unique_id_suffix);
        return ESP_ERR_NO_MEM;
    }

    /* Ownership transfer: topic is stack-local, must strdup for handler.
     * json_str from cJSON_PrintUnformatted is heap-allocated, ownership
     * transfers directly to handler (do NOT free after event_publish). */
    char *topic_copy = strdup(topic);
    if (topic_copy == NULL) {
        ESP_LOGE(TAG, "Failed to strdup topic for %s", unique_id_suffix);
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_copy,
        .payload_json = json_str,
        .qos = 1,
        .retain = true
    };
    ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Published discovery: %s/%s", component, unique_id_suffix);
    } else {
        ESP_LOGE(TAG, "Failed to publish discovery: %s/%s", component, unique_id_suffix);
        /* On publish failure, handler won't run - we must free both */
        free(topic_copy);
        free(json_str);
    }

    return ret;
}

/* ============================================================================
 * Diagnostic Sensor Publishers
 * ============================================================================ */

/**
 * @brief Publish zigbee_device_count sensor discovery
 */
static esp_err_t publish_sensor_device_count(void)
{
    cJSON *config = create_base_discovery_config("Zigbee Device Count", "device_count");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.device_count }}");
    cJSON_AddStringToObject(config, "icon", "mdi:devices");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "device_count", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish zigbee_channel sensor discovery
 */
static esp_err_t publish_sensor_channel(void)
{
    cJSON *config = create_base_discovery_config("Zigbee Channel", "channel");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.channel }}");
    cJSON_AddStringToObject(config, "icon", "mdi:radio-tower");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "channel", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish zigbee_pan_id sensor discovery
 */
static esp_err_t publish_sensor_pan_id(void)
{
    cJSON *config = create_base_discovery_config("Zigbee PAN ID", "pan_id");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.pan_id }}");
    cJSON_AddStringToObject(config, "icon", "mdi:identifier");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "pan_id", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish coordinator_state sensor discovery
 */
static esp_err_t publish_sensor_coordinator_state(void)
{
    cJSON *config = create_base_discovery_config("Coordinator State", "coordinator_state");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.coordinator_state }}");
    cJSON_AddStringToObject(config, "icon", "mdi:chip");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "coordinator_state", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish free_heap sensor discovery
 */
static esp_err_t publish_sensor_free_heap(void)
{
    cJSON *config = create_base_discovery_config("Free Heap", "free_heap");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "device_class", "data_size");
    cJSON_AddStringToObject(config, "unit_of_measurement", "kB");
    cJSON_AddStringToObject(config, "value_template", "{{ value_json.free_heap_kb }}");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_MEASUREMENT);
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "free_heap", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish uptime sensor discovery
 */
static esp_err_t publish_sensor_uptime(void)
{
    cJSON *config = create_base_discovery_config("Uptime", "uptime");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "device_class", "duration");
    cJSON_AddStringToObject(config, "unit_of_measurement", "s");
    cJSON_AddStringToObject(config, "value_template", "{{ value_json.uptime_sec }}");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_MEASUREMENT);
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);
    cJSON_AddStringToObject(config, "icon", "mdi:clock-outline");

    return publish_discovery_config("sensor", "uptime", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish wifi_rssi sensor discovery
 */
static esp_err_t publish_sensor_wifi_rssi(void)
{
    cJSON *config = create_base_discovery_config("WiFi Signal", "wifi_rssi");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "device_class", HA_DEVICE_CLASS_SIGNAL_STRENGTH);
    cJSON_AddStringToObject(config, "unit_of_measurement", HA_UNIT_DBM);
    cJSON_AddStringToObject(config, "value_template", "{{ value_json.wifi_rssi }}");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_MEASUREMENT);
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "wifi_rssi", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish wifi_ip sensor discovery
 */
static esp_err_t publish_sensor_wifi_ip(void)
{
    cJSON *config = create_base_discovery_config("WiFi IP Address", "wifi_ip");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.wifi_ip }}");
    cJSON_AddStringToObject(config, "icon", "mdi:ip-network");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "wifi_ip", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish wifi_signal_quality sensor discovery
 */
static esp_err_t publish_sensor_wifi_signal_quality(void)
{
    cJSON *config = create_base_discovery_config("WiFi Signal Quality", "wifi_signal_quality");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "unit_of_measurement", HA_UNIT_PERCENT);
    cJSON_AddStringToObject(config, "value_template", "{{ value_json.wifi_signal_quality }}");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_MEASUREMENT);
    cJSON_AddStringToObject(config, "icon", "mdi:wifi-strength-3");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "wifi_signal_quality", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish wifi_band sensor discovery
 */
static esp_err_t publish_sensor_wifi_band(void)
{
    cJSON *config = create_base_discovery_config("WiFi Band", "wifi_band");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.wifi_band }}");
    cJSON_AddStringToObject(config, "icon", "mdi:wifi");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "wifi_band", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish wifi_hostname sensor discovery
 */
static esp_err_t publish_sensor_wifi_hostname(void)
{
    cJSON *config = create_base_discovery_config("WiFi Hostname", "wifi_hostname");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.wifi_hostname }}");
    cJSON_AddStringToObject(config, "icon", "mdi:web");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "wifi_hostname", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish wifi_uptime sensor discovery
 */
static esp_err_t publish_sensor_wifi_uptime(void)
{
    cJSON *config = create_base_discovery_config("WiFi Uptime", "wifi_uptime");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "device_class", "duration");
    cJSON_AddStringToObject(config, "unit_of_measurement", "s");
    cJSON_AddStringToObject(config, "value_template", "{{ value_json.wifi_uptime_sec }}");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_MEASUREMENT);
    cJSON_AddStringToObject(config, "icon", "mdi:timer-outline");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "wifi_uptime", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish wifi_connect_count sensor discovery
 */
static esp_err_t publish_sensor_wifi_connect_count(void)
{
    cJSON *config = create_base_discovery_config("WiFi Connect Count", "wifi_connect_count");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.wifi_connect_count }}");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_TOTAL_INCREASING);
    cJSON_AddStringToObject(config, "icon", "mdi:connection");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "wifi_connect_count", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish wifi_disconnect_count sensor discovery
 */
static esp_err_t publish_sensor_wifi_disconnect_count(void)
{
    cJSON *config = create_base_discovery_config("WiFi Disconnect Count", "wifi_disconnect_count");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.wifi_disconnect_count }}");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_TOTAL_INCREASING);
    cJSON_AddStringToObject(config, "icon", "mdi:wifi-off");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "wifi_disconnect_count", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish mqtt_connected binary sensor discovery
 */
static esp_err_t publish_binary_sensor_mqtt_connected(void)
{
    cJSON *config = create_base_discovery_config("MQTT Connected", "mqtt_connected");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "device_class", "connectivity");
    cJSON_AddStringToObject(config, "value_template",
                            "{{ 'ON' if value_json.mqtt_connected else 'OFF' }}");
    cJSON_AddStringToObject(config, "payload_on", "ON");
    cJSON_AddStringToObject(config, "payload_off", "OFF");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("binary_sensor", "mqtt_connected", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish time_synced binary sensor discovery
 */
static esp_err_t publish_binary_sensor_time_synced(void)
{
    cJSON *config = create_base_discovery_config("Time Synchronized", "time_synced");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "device_class", "connectivity");
    cJSON_AddStringToObject(config, "value_template",
                            "{{ 'ON' if value_json.time_synced else 'OFF' }}");
    cJSON_AddStringToObject(config, "payload_on", "ON");
    cJSON_AddStringToObject(config, "payload_off", "OFF");
    cJSON_AddStringToObject(config, "icon", "mdi:clock-check");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("binary_sensor", "time_synced", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish current_time sensor discovery
 */
static esp_err_t publish_sensor_current_time(void)
{
    cJSON *config = create_base_discovery_config("Current Time", "current_time");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "device_class", "timestamp");
    cJSON_AddStringToObject(config, "value_template",
        "{{ value_json.current_time if value_json.current_time != 'not_synced' else None }}");
    cJSON_AddStringToObject(config, "icon", "mdi:clock-outline");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "current_time", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish last_reset_reason sensor discovery
 */
static esp_err_t publish_sensor_last_reset_reason(void)
{
    cJSON *config = create_base_discovery_config("Last Reset Reason", "last_reset_reason");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.last_reset_reason }}");
    cJSON_AddStringToObject(config, "icon", "mdi:restart-alert");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "last_reset_reason", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish last_reset_was_crash binary sensor discovery
 */
static esp_err_t publish_binary_sensor_last_reset_was_crash(void)
{
    cJSON *config = create_base_discovery_config("Last Reset Was Crash", "last_reset_was_crash");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "device_class", "problem");
    cJSON_AddStringToObject(config, "value_template",
                            "{{ 'ON' if value_json.last_reset_was_crash else 'OFF' }}");
    cJSON_AddStringToObject(config, "payload_on", "ON");
    cJSON_AddStringToObject(config, "payload_off", "OFF");
    cJSON_AddStringToObject(config, "icon", "mdi:alert-circle");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("binary_sensor", "last_reset_was_crash", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish boot_count sensor discovery
 */
static esp_err_t publish_sensor_boot_count(void)
{
    cJSON *config = create_base_discovery_config("Boot Count", "boot_count");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.boot_count }}");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_TOTAL_INCREASING);
    cJSON_AddStringToObject(config, "icon", "mdi:counter");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "boot_count", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish mqtt_latency sensor discovery
 *
 * Exposes the MQTT broker round-trip latency as a diagnostic sensor.
 * Uses the PUBACK timing from QoS 1 publishes to measure latency.
 */
static esp_err_t publish_sensor_mqtt_latency(void)
{
    cJSON *config = create_base_discovery_config("MQTT Latency", "mqtt_latency");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "unit_of_measurement", "ms");
    cJSON_AddStringToObject(config, "value_template",
        "{{ value_json.mqtt_latency_ms if value_json.mqtt_latency_ms != 4294967295 else None }}");
    cJSON_AddStringToObject(config, "icon", "mdi:timer-outline");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_MEASUREMENT);
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "mqtt_latency", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/* ============================================================================
 * Control Entity Publishers
 * ============================================================================ */

/**
 * @brief Publish permit_join switch discovery
 */
static esp_err_t publish_switch_permit_join(void)
{
    cJSON *config = create_base_discovery_config("Permit Join", "permit_join");
    CHECK_CJSON_GOTO(config, cleanup);

    /* Command topic */
    cJSON_AddStringToObject(config, "command_topic", TOPIC_BRIDGE_REQUEST_PERMIT_JOIN);

    /* State from bridge state topic */
    cJSON_AddStringToObject(config, "value_template",
                            "{{ 'ON' if value_json.permit_join else 'OFF' }}");
    cJSON_AddStringToObject(config, "state_on", "ON");
    cJSON_AddStringToObject(config, "state_off", "OFF");

    /* Payloads as JSON objects */
    cJSON_AddStringToObject(config, "payload_on", "{\"value\": true}");
    cJSON_AddStringToObject(config, "payload_off", "{\"value\": false}");

    cJSON_AddStringToObject(config, "icon", "mdi:lock-open-variant");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_CONFIG);

    return publish_discovery_config("switch", "permit_join", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish log_level select discovery
 */
static esp_err_t publish_select_log_level(void)
{
    cJSON *config = create_base_discovery_config("Log Level", "log_level");
    CHECK_CJSON_GOTO(config, cleanup);

    /* Command topic */
    cJSON_AddStringToObject(config, "command_topic", TOPIC_BRIDGE_REQUEST_LOGGING_LEVEL);

    /* State from bridge state topic */
    cJSON_AddStringToObject(config, "value_template", "{{ value_json.log_level }}");

    /* Options array */
    cJSON *options = cJSON_CreateArray();
    CHECK_CJSON_GOTO(options, cleanup_config);

    cJSON *opt_error = cJSON_CreateString("error");
    cJSON *opt_warn = cJSON_CreateString("warn");
    cJSON *opt_info = cJSON_CreateString("info");
    cJSON *opt_debug = cJSON_CreateString("debug");

    if (opt_error == NULL || opt_warn == NULL ||
        opt_info == NULL || opt_debug == NULL) {
        cJSON_Delete(opt_error);
        cJSON_Delete(opt_warn);
        cJSON_Delete(opt_info);
        cJSON_Delete(opt_debug);
        cJSON_Delete(options);
        goto cleanup_config;
    }

    cJSON_AddItemToArray(options, opt_error);
    cJSON_AddItemToArray(options, opt_warn);
    cJSON_AddItemToArray(options, opt_info);
    cJSON_AddItemToArray(options, opt_debug);
    cJSON_AddItemToObject(config, "options", options);

    /* Command template - wrap selection in JSON */
    cJSON_AddStringToObject(config, "command_template", "{\"level\": \"{{ value }}\"}");

    cJSON_AddStringToObject(config, "icon", "mdi:text-box-outline");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_CONFIG);

    return publish_discovery_config("select", "log_level", config);

cleanup_config:
    cJSON_Delete(config);
cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish restart button discovery
 */
static esp_err_t publish_button_restart(void)
{
    cJSON *config = create_base_discovery_config("Restart Gateway", "restart");
    CHECK_CJSON_GOTO(config, cleanup);

    /* Command topic */
    cJSON_AddStringToObject(config, "command_topic", TOPIC_BRIDGE_REQUEST_RESTART);

    /* Payload */
    cJSON_AddStringToObject(config, "payload_press", "{\"value\": true}");

    cJSON_AddStringToObject(config, "icon", "mdi:restart");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_CONFIG);

    /* Device class for restart button */
    cJSON_AddStringToObject(config, "device_class", "restart");

    return publish_discovery_config("button", "restart", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish permit join button discovery
 *
 * Creates a button that enables Zigbee pairing for 180 seconds when pressed.
 */
static esp_err_t publish_button_permit_join(void)
{
    cJSON *config = create_base_discovery_config("Permit Join", "permit_join_btn");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "command_topic", TOPIC_BRIDGE_REQUEST_PERMIT_JOIN);
    cJSON_AddStringToObject(config, "payload_press", "{\"value\": true, \"time\": 180}");
    cJSON_AddStringToObject(config, "icon", "mdi:plus-circle");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_CONFIG);

    return publish_discovery_config("button", "permit_join_btn", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish permit join stop button discovery
 *
 * Creates a button that stops Zigbee pairing when pressed.
 */
static esp_err_t publish_button_permit_join_stop(void)
{
    cJSON *config = create_base_discovery_config("Stop Permit Join", "permit_join_stop");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "command_topic", TOPIC_BRIDGE_REQUEST_PERMIT_JOIN);
    cJSON_AddStringToObject(config, "payload_press", "{\"value\": false}");
    cJSON_AddStringToObject(config, "icon", "mdi:close-circle");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_CONFIG);

    return publish_discovery_config("button", "permit_join_stop", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish network scan button discovery
 *
 * Creates a button that triggers a network topology scan when pressed.
 */
static esp_err_t publish_button_network_scan(void)
{
    cJSON *config = create_base_discovery_config("Scan Network", "network_scan");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "command_topic", TOPIC_BRIDGE_REQUEST_NETWORKMAP);
    cJSON_AddStringToObject(config, "payload_press", "{\"type\": \"raw\", \"routes\": true}");
    cJSON_AddStringToObject(config, "icon", "mdi:radar");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("button", "network_scan", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish factory reset button discovery
 *
 * Creates a button that erases ALL NVS data (WiFi, Zigbee devices, config)
 * and restarts the gateway. Use with caution!
 */
static esp_err_t publish_button_factory_reset(void)
{
    cJSON *config = create_base_discovery_config("Factory Reset", "factory_reset");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "command_topic", TOPIC_BRIDGE_REQUEST_FACTORY_RESET);
    cJSON_AddStringToObject(config, "payload_press", "{\"value\": true}");
    cJSON_AddStringToObject(config, "icon", "mdi:delete-forever");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_CONFIG);

    /* Device class for dangerous action - shows warning dialog in HA */
    cJSON_AddStringToObject(config, "device_class", "restart");

    return publish_discovery_config("button", "factory_reset", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish Network Reset button discovery
 *
 * Creates a button that erases Zigbee network data and devices but preserves
 * WiFi/MQTT configuration. The gateway restarts after the reset.
 */
static esp_err_t publish_button_network_reset(void)
{
    cJSON *config = create_base_discovery_config("Network Reset", "network_reset");
    CHECK_CJSON_GOTO(config, cleanup);

    /* Command topic */
    cJSON_AddStringToObject(config, "command_topic",
                            "zigbee2mqtt/bridge/request/reset/network");
    cJSON_AddStringToObject(config, "payload_press", "");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_CONFIG);
    cJSON_AddStringToObject(config, "device_class", "restart");
    cJSON_AddStringToObject(config, "icon", "mdi:zigbee");

    return publish_discovery_config("button", "network_reset", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish Config Reset button discovery
 *
 * Creates a button that erases WiFi and gateway configuration but preserves
 * Zigbee network and devices. The gateway restarts after the reset.
 */
static esp_err_t publish_button_config_reset(void)
{
    cJSON *config = create_base_discovery_config("Config Reset", "config_reset");
    CHECK_CJSON_GOTO(config, cleanup);

    /* Command topic */
    cJSON_AddStringToObject(config, "command_topic",
                            "zigbee2mqtt/bridge/request/reset/config");
    cJSON_AddStringToObject(config, "payload_press", "");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_CONFIG);
    cJSON_AddStringToObject(config, "device_class", "restart");
    cJSON_AddStringToObject(config, "icon", "mdi:cog-off");

    return publish_discovery_config("button", "config_reset", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/* ============================================================================
 * State Collection Helper
 * ============================================================================ */

/**
 * @brief Get coordinator state as string
 *
 * @param[in] state Coordinator state enum
 * @return State string
 */
static const char* coordinator_state_to_string(zb_coordinator_state_t state)
{
    switch (state) {
        case ZB_COORD_STATE_UNINITIALIZED: return "uninitialized";
        case ZB_COORD_STATE_INITIALIZED:   return "initialized";
        case ZB_COORD_STATE_STARTING:      return "starting";
        case ZB_COORD_STATE_RUNNING:       return "running";
        case ZB_COORD_STATE_ERROR:         return "error";
        case ZB_COORD_STATE_STOPPED:       return "stopped";
        default:                           return "unknown";
    }
}

/**
 * @brief Collect current bridge state from all subsystems
 *
 * @param[out] state State structure to populate
 */
static void collect_bridge_state(ha_bridge_state_t *state)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(ha_bridge_state_t));

    /* Bridge online state */
    state->state = mqtt_client_is_connected() ? "online" : "offline";

    /* Zigbee device count */
    state->device_count = (uint16_t)device_registry_count();

    /* Coordinator settings */
    zb_coordinator_settings_t coord_settings;
    if (zb_coordinator_get_settings(&coord_settings) == ESP_OK) {
        state->channel = coord_settings.channel;
        state->pan_id = coord_settings.pan_id;
    } else {
        state->channel = 0;
        state->pan_id = 0;
    }

    /* Coordinator state */
    state->coordinator_state = coordinator_state_to_string(zb_coordinator_get_state());

    /* Permit join state */
    state->permit_join = zb_coordinator_is_permit_join_enabled();
    state->permit_join_timeout = zb_coordinator_get_permit_join_remaining();

    /* Log level */
    state->log_level = s_log_level;

    /* Free heap in KB */
    state->free_heap_kb = esp_get_free_heap_size() / 1024;

    /* Uptime in seconds */
    int64_t now_us = esp_timer_get_time();
    state->uptime_sec = (uint32_t)((now_us - s_start_time_us) / GW_MICROSECONDS_PER_SECOND);

    /* WiFi RSSI */
    state->wifi_rssi = wifi_manager_get_rssi();

    /* WiFi IP */
    if (wifi_manager_get_ip(state->wifi_ip, sizeof(state->wifi_ip)) != ESP_OK) {
        strlcpy(state->wifi_ip, "0.0.0.0", sizeof(state->wifi_ip));
    }

    /* WiFi signal quality */
    state->wifi_signal_quality = wifi_manager_get_signal_quality();

    /* WiFi band - detect from RSSI update or get from stats */
    wifi_stats_t wifi_stats;
    if (wifi_manager_get_stats(&wifi_stats) == ESP_OK) {
        state->wifi_uptime_sec = wifi_stats.uptime_seconds;
        state->wifi_connect_count = wifi_stats.connect_count;
        state->wifi_disconnect_count = wifi_stats.disconnect_count;
    } else {
        state->wifi_uptime_sec = 0;
        state->wifi_connect_count = 0;
        state->wifi_disconnect_count = 0;
    }

    /* WiFi hostname */
    const char *hostname = wifi_manager_get_hostname();
    if (hostname != NULL) {
        strlcpy(state->wifi_hostname, hostname, sizeof(state->wifi_hostname));
    } else {
        strlcpy(state->wifi_hostname, "unknown", sizeof(state->wifi_hostname));
    }

    /* WiFi band - determine from channel if connected */
    if (wifi_manager_is_connected()) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            if (ap_info.primary >= WIFI_MGR_5GHZ_CHANNEL_MIN) {
                strlcpy(state->wifi_band, "5GHz", sizeof(state->wifi_band));
            } else {
                strlcpy(state->wifi_band, "2.4GHz", sizeof(state->wifi_band));
            }
        } else {
            strlcpy(state->wifi_band, "unknown", sizeof(state->wifi_band));
        }
    } else {
        strlcpy(state->wifi_band, "N/A", sizeof(state->wifi_band));
    }

    /* MQTT connected */
    state->mqtt_connected = mqtt_client_is_connected();

    /* MQTT latency */
    state->mqtt_latency_ms = mqtt_latency_get_ms();

    /* NTP synchronization status */
    state->time_synced = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);

    /* Get current time if synced (ISO 8601 with UTC timezone) */
    if (state->time_synced) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        gmtime_r(&now, &timeinfo);
        strftime(state->current_time, sizeof(state->current_time),
                 "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    } else {
        strlcpy(state->current_time, "not_synced", sizeof(state->current_time));
    }

    /* Crash reporter data */
    if (crash_reporter_is_initialized()) {
        state->last_reset_reason = crash_reporter_get_reason_str();
        state->last_reset_was_crash = crash_reporter_was_crash();
        state->boot_count = crash_reporter_get_boot_count();
    } else {
        state->last_reset_reason = "unknown";
        state->last_reset_was_crash = false;
        state->boot_count = 0;
    }

    /* BLE status */
#if CONFIG_BT_SCANNER_ENABLED
    state->ble_status = ble_scanner_is_running() ? "scanning" : "stopped";
    state->ble_device_count = (uint16_t)ble_scanner_get_device_count();
    state->ble_active_scan = ble_scanner_is_active_enabled();
#endif

    /* ESPHome status */
#if CONFIG_ESPHOME_API_ENABLE
    state->esphome_status = esphome_api_is_running() ? "running" : "stopped";
    state->esphome_clients = esphome_api_get_client_count();
    state->ble_proxy_adverts = 0;
    state->ble_proxy_connections = 0;
#if CONFIG_BT_SCANNER_ENABLED
    {
        esphome_ble_proxy_stats_t pstats;
        if (esphome_ble_proxy_get_stats(&pstats) == ESP_OK) {
            state->ble_proxy_adverts = pstats.advertisements_forwarded;
            state->ble_proxy_connections = pstats.active_connections;
        }
    }
#endif
#endif
}

/* ============================================================================
 * BLE & ESPHome Diagnostic Sensor Publishers
 * ============================================================================ */

#if CONFIG_BT_SCANNER_ENABLED
/**
 * @brief Publish ble_status sensor discovery
 */
static esp_err_t publish_sensor_ble_status(void)
{
    cJSON *config = create_base_discovery_config("BLE Status", "ble_status");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.ble_status }}");
    cJSON_AddStringToObject(config, "icon", "mdi:bluetooth");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "ble_status", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish ble_device_count sensor discovery
 */
static esp_err_t publish_sensor_ble_device_count(void)
{
    cJSON *config = create_base_discovery_config("BLE Devices", "ble_device_count");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.ble_device_count }}");
    cJSON_AddStringToObject(config, "icon", "mdi:bluetooth-connect");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_MEASUREMENT);
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "ble_device_count", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish ble_scanner switch discovery
 */
static esp_err_t publish_switch_ble_scanner(void)
{
    cJSON *config = create_base_discovery_config("BLE Scanner", "ble_scanner");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "command_topic", TOPIC_BRIDGE_REQUEST_BLE_SCANNER);
    cJSON_AddStringToObject(config, "value_template",
                            "{{ 'ON' if value_json.ble_status == 'scanning' else 'OFF' }}");
    cJSON_AddStringToObject(config, "state_on", "ON");
    cJSON_AddStringToObject(config, "state_off", "OFF");
    cJSON_AddStringToObject(config, "payload_on", "{\"value\": true}");
    cJSON_AddStringToObject(config, "payload_off", "{\"value\": false}");
    cJSON_AddStringToObject(config, "icon", "mdi:bluetooth-settings");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_CONFIG);

    return publish_discovery_config("switch", "ble_scanner", config);

cleanup:
    return ESP_ERR_NO_MEM;
}
#endif /* CONFIG_BT_SCANNER_ENABLED */

#if CONFIG_ESPHOME_API_ENABLE
/**
 * @brief Publish ble_proxy_connections sensor discovery
 */
static esp_err_t publish_sensor_ble_proxy_connections(void)
{
    cJSON *config = create_base_discovery_config("BLE Proxy Connections", "ble_proxy_connections");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.ble_proxy_connections }}");
    cJSON_AddStringToObject(config, "icon", "mdi:bluetooth-transfer");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_MEASUREMENT);
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "ble_proxy_connections", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish ble_proxy_adverts sensor discovery
 */
static esp_err_t publish_sensor_ble_proxy_adverts(void)
{
    cJSON *config = create_base_discovery_config("BLE Proxy Advertisements", "ble_proxy_adverts");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.ble_proxy_adverts }}");
    cJSON_AddStringToObject(config, "icon", "mdi:broadcast");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_TOTAL_INCREASING);
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "ble_proxy_adverts", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish esphome_status sensor discovery
 */
static esp_err_t publish_sensor_esphome_status(void)
{
    cJSON *config = create_base_discovery_config("ESPHome Status", "esphome_status");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.esphome_status }}");
    cJSON_AddStringToObject(config, "icon", "mdi:api");
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "esphome_status", config);

cleanup:
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Publish esphome_clients sensor discovery
 */
static esp_err_t publish_sensor_esphome_clients(void)
{
    cJSON *config = create_base_discovery_config("ESPHome Clients", "esphome_clients");
    CHECK_CJSON_GOTO(config, cleanup);

    cJSON_AddStringToObject(config, "value_template", "{{ value_json.esphome_clients }}");
    cJSON_AddStringToObject(config, "icon", "mdi:account-multiple");
    cJSON_AddStringToObject(config, "state_class", HA_STATE_CLASS_MEASUREMENT);
    cJSON_AddStringToObject(config, "entity_category", HA_ENTITY_CATEGORY_DIAGNOSTIC);

    return publish_discovery_config("sensor", "esphome_clients", config);

cleanup:
    return ESP_ERR_NO_MEM;
}
#endif /* CONFIG_ESPHOME_API_ENABLE */

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t ha_bridge_discovery_init(void)
{
    if (s_initialized) {
        ESP_LOGD(TAG, "Bridge discovery already initialized");
        return ESP_OK;
    }

    /* Record start time for uptime calculation */
    s_start_time_us = esp_timer_get_time();

    /* Initialize log level */
    s_log_level = "info";

    s_initialized = true;
    ESP_LOGI(TAG, "Bridge discovery initialized");

    return ESP_OK;
}

esp_err_t ha_bridge_discovery_publish_all(void)
{
    REQUIRE_MQTT_CONNECTED(ESP_ERR_INVALID_STATE);

    if (!s_initialized) {
        ESP_LOGW(TAG, "Bridge discovery not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Publishing bridge discovery messages");

    esp_err_t ret = ESP_OK;
    esp_err_t tmp_ret;

    /* Publish diagnostic sensors */
    tmp_ret = publish_sensor_device_count();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_channel();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_pan_id();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_coordinator_state();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_free_heap();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_uptime();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_wifi_rssi();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_wifi_ip();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_wifi_signal_quality();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_wifi_band();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_wifi_hostname();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_wifi_uptime();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_wifi_connect_count();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_wifi_disconnect_count();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    /* Publish binary sensors */
    tmp_ret = publish_binary_sensor_mqtt_connected();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_binary_sensor_time_synced();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_current_time();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    /* Crash reporter sensors */
    tmp_ret = publish_sensor_last_reset_reason();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_binary_sensor_last_reset_was_crash();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_boot_count();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_sensor_mqtt_latency();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    /* Publish control entities */
    tmp_ret = publish_switch_permit_join();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_select_log_level();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_button_restart();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_button_permit_join();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_button_permit_join_stop();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_button_network_scan();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));

    tmp_ret = publish_button_factory_reset();
    if (tmp_ret != ESP_OK) ret = tmp_ret;

    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));
    tmp_ret = publish_button_network_reset();
    if (tmp_ret != ESP_OK) ret = tmp_ret;

    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));
    tmp_ret = publish_button_config_reset();
    if (tmp_ret != ESP_OK) ret = tmp_ret;

    /* BLE diagnostic sensors */
#if CONFIG_BT_SCANNER_ENABLED
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));
    tmp_ret = publish_sensor_ble_status();
    if (tmp_ret != ESP_OK) ret = tmp_ret;

    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));
    tmp_ret = publish_sensor_ble_device_count();
    if (tmp_ret != ESP_OK) ret = tmp_ret;

    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));
    tmp_ret = publish_switch_ble_scanner();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
#endif

    /* ESPHome diagnostic sensors */
#if CONFIG_ESPHOME_API_ENABLE
    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));
    tmp_ret = publish_sensor_ble_proxy_connections();
    if (tmp_ret != ESP_OK) ret = tmp_ret;

    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));
    tmp_ret = publish_sensor_ble_proxy_adverts();
    if (tmp_ret != ESP_OK) ret = tmp_ret;

    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));
    tmp_ret = publish_sensor_esphome_status();
    if (tmp_ret != ESP_OK) ret = tmp_ret;

    vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_PUBLISH_DELAY_MS));
    tmp_ret = publish_sensor_esphome_clients();
    if (tmp_ret != ESP_OK) ret = tmp_ret;
#endif

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "All bridge discovery messages published successfully");
    } else {
        ESP_LOGW(TAG, "Some bridge discovery messages failed to publish");
    }

    return ret;
}

esp_err_t ha_bridge_discovery_remove(void)
{
    REQUIRE_MQTT_CONNECTED(ESP_ERR_INVALID_STATE);

    ESP_LOGI(TAG, "Removing bridge discovery messages");

    /* Entity suffixes to remove */
    static const struct {
        const char *component;
        const char *suffix;
    } entities[] = {
        {"sensor", "device_count"},
        {"sensor", "channel"},
        {"sensor", "pan_id"},
        {"sensor", "coordinator_state"},
        {"sensor", "free_heap"},
        {"sensor", "uptime"},
        {"sensor", "wifi_rssi"},
        {"sensor", "wifi_ip"},
        {"sensor", "wifi_signal_quality"},
        {"sensor", "wifi_band"},
        {"sensor", "wifi_hostname"},
        {"sensor", "wifi_uptime"},
        {"sensor", "wifi_connect_count"},
        {"sensor", "wifi_disconnect_count"},
        {"binary_sensor", "mqtt_connected"},
        {"binary_sensor", "time_synced"},
        {"sensor", "current_time"},
        {"sensor", "last_reset_reason"},
        {"binary_sensor", "last_reset_was_crash"},
        {"sensor", "boot_count"},
        {"sensor", "mqtt_latency"},
        {"switch", "permit_join"},
        {"select", "log_level"},
        {"button", "restart"},
        {"button", "permit_join_btn"},
        {"button", "permit_join_stop"},
        {"button", "network_scan"},
        {"button", "factory_reset"},
        {"button", "network_reset"},
        {"button", "config_reset"},
#if CONFIG_BT_SCANNER_ENABLED
        {"sensor", "ble_status"},
        {"sensor", "ble_device_count"},
        {"switch", "ble_scanner"},
#endif
#if CONFIG_ESPHOME_API_ENABLE
        {"sensor", "ble_proxy_connections"},
        {"sensor", "ble_proxy_adverts"},
        {"sensor", "esphome_status"},
        {"sensor", "esphome_clients"},
#endif
    };

    const size_t entity_count = sizeof(entities) / sizeof(entities[0]);

    for (size_t i = 0; i < entity_count; i++) {
        char topic[MQTT_TOPIC_MAX_LEN];
        char entity_id[GW_BUFFER_SIZE_SMALL];

        snprintf(entity_id, sizeof(entity_id), "%s_%s",
                 HA_BRIDGE_DEVICE_ID, entities[i].suffix);

        if (mqtt_topic_ha_discovery(entities[i].component, entity_id,
                                    topic, sizeof(topic)) == ESP_OK) {
            /* Ownership transfer: strdup stack topic and empty payload.
             * Handler frees both after publishing. */
            char *topic_copy = strdup(topic);
            char *payload_copy = strdup("");
            if (topic_copy != NULL && payload_copy != NULL) {
                evt_ha_discovery_publish_t evt = {
                    .topic = topic_copy,
                    .payload_json = payload_copy,
                    .qos = 1,
                    .retain = true
                };
                if (event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt)) != ESP_OK) {
                    free(topic_copy);
                    free(payload_copy);
                }
            } else {
                free(topic_copy);
                free(payload_copy);
            }
        }
    }

    ESP_LOGI(TAG, "Bridge discovery messages removed");
    return ESP_OK;
}

esp_err_t ha_bridge_publish_state(void)
{
    REQUIRE_MQTT_CONNECTED(ESP_ERR_INVALID_STATE);

    /* Collect current state */
    ha_bridge_state_t state;
    collect_bridge_state(&state);

    /* Create JSON object */
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create state JSON object");
        return ESP_ERR_NO_MEM;
    }

    /* Add all state fields */
    cJSON_AddStringToObject(json, "state", state.state);
    cJSON_AddNumberToObject(json, "device_count", state.device_count);
    cJSON_AddNumberToObject(json, "channel", state.channel);

    /* PAN ID as hex string */
    char pan_id_str[8];
    snprintf(pan_id_str, sizeof(pan_id_str), "0x%04X", state.pan_id);
    cJSON_AddStringToObject(json, "pan_id", pan_id_str);

    cJSON_AddStringToObject(json, "coordinator_state", state.coordinator_state);
    cJSON_AddBoolToObject(json, "permit_join", state.permit_join);
    cJSON_AddNumberToObject(json, "permit_join_timeout", state.permit_join_timeout);
    cJSON_AddStringToObject(json, "log_level", state.log_level);
    cJSON_AddNumberToObject(json, "free_heap_kb", state.free_heap_kb);
    cJSON_AddNumberToObject(json, "uptime_sec", state.uptime_sec);
    cJSON_AddNumberToObject(json, "wifi_rssi", state.wifi_rssi);
    cJSON_AddStringToObject(json, "wifi_ip", state.wifi_ip);
    cJSON_AddNumberToObject(json, "wifi_signal_quality", state.wifi_signal_quality);
    cJSON_AddStringToObject(json, "wifi_band", state.wifi_band);
    cJSON_AddStringToObject(json, "wifi_hostname", state.wifi_hostname);
    cJSON_AddNumberToObject(json, "wifi_uptime_sec", state.wifi_uptime_sec);
    cJSON_AddNumberToObject(json, "wifi_connect_count", state.wifi_connect_count);
    cJSON_AddNumberToObject(json, "wifi_disconnect_count", state.wifi_disconnect_count);
    cJSON_AddBoolToObject(json, "mqtt_connected", state.mqtt_connected);
    cJSON_AddNumberToObject(json, "mqtt_latency_ms", state.mqtt_latency_ms);
    cJSON_AddBoolToObject(json, "time_synced", state.time_synced);
    cJSON_AddStringToObject(json, "current_time", state.current_time);

    /* Crash reporter fields */
    cJSON_AddStringToObject(json, "last_reset_reason", state.last_reset_reason);
    cJSON_AddBoolToObject(json, "last_reset_was_crash", state.last_reset_was_crash);
    cJSON_AddNumberToObject(json, "boot_count", state.boot_count);

    /* BLE status fields */
#if CONFIG_BT_SCANNER_ENABLED
    cJSON_AddStringToObject(json, "ble_status", state.ble_status);
    cJSON_AddNumberToObject(json, "ble_device_count", state.ble_device_count);
    cJSON_AddBoolToObject(json, "ble_active_scan", state.ble_active_scan);
#endif

    /* ESPHome status fields */
#if CONFIG_ESPHOME_API_ENABLE
    cJSON_AddStringToObject(json, "esphome_status", state.esphome_status);
    cJSON_AddNumberToObject(json, "esphome_clients", state.esphome_clients);
    cJSON_AddNumberToObject(json, "ble_proxy_adverts", state.ble_proxy_adverts);
    cJSON_AddNumberToObject(json, "ble_proxy_connections", state.ble_proxy_connections);
#endif

    /* Serialize and publish */
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize state JSON");
        return ESP_ERR_NO_MEM;
    }

    /* Ownership transfer: HA_BRIDGE_STATE_TOPIC is a #define constant,
     * must strdup for handler. json_str from cJSON_PrintUnformatted is
     * heap-allocated, ownership transfers directly to handler. */
    char *topic_copy = strdup(HA_BRIDGE_STATE_TOPIC);
    if (topic_copy == NULL) {
        ESP_LOGE(TAG, "Failed to strdup state topic");
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_copy,
        .payload_json = json_str,
        .qos = 1,
        .retain = true
    };
    esp_err_t ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Published bridge state");
    } else {
        ESP_LOGE(TAG, "Failed to publish bridge state: %s", esp_err_to_name(ret));
        /* On publish failure, handler won't run - we must free both */
        free(topic_copy);
        free(json_str);
    }

    return ret;
}

esp_err_t ha_bridge_get_state(ha_bridge_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    collect_bridge_state(state);
    return ESP_OK;
}

esp_err_t ha_bridge_set_log_level(const char *level)
{
    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate level */
    if (strcmp(level, "error") == 0 ||
        strcmp(level, "warn") == 0 ||
        strcmp(level, "info") == 0 ||
        strcmp(level, "debug") == 0) {
        s_log_level = level;
        ESP_LOGI(TAG, "Log level set to: %s", level);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Invalid log level: %s", level);
    return ESP_ERR_INVALID_ARG;
}

const char* ha_bridge_get_log_level(void)
{
    return s_log_level;
}

bool ha_bridge_discovery_is_initialized(void)
{
    return s_initialized;
}

esp_err_t ha_bridge_discovery_test(void)
{
    ESP_LOGI(TAG, "Running bridge discovery test...");

    /* Test device info creation */
    cJSON *device = create_bridge_device_info();
    if (device == NULL) {
        ESP_LOGE(TAG, "Failed to create device info");
        return ESP_FAIL;
    }

    char *device_str = cJSON_PrintUnformatted(device);
    cJSON_Delete(device);

    if (device_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize device info");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Device info: %s", device_str);
    free(device_str);

    /* Test base config creation */
    cJSON *config = create_base_discovery_config("Test Sensor", "test");
    if (config == NULL) {
        ESP_LOGE(TAG, "Failed to create base config");
        return ESP_FAIL;
    }

    char *config_str = cJSON_PrintUnformatted(config);
    cJSON_Delete(config);

    if (config_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize config");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Base config: %s", config_str);
    free(config_str);

    /* Test state collection */
    ha_bridge_state_t state;
    collect_bridge_state(&state);

    ESP_LOGI(TAG, "State: device_count=%d, channel=%d, heap=%lu KB, uptime=%lu s",
             state.device_count, state.channel, state.free_heap_kb, state.uptime_sec);

    ESP_LOGI(TAG, "Bridge discovery test PASSED");
    return ESP_OK;
}
