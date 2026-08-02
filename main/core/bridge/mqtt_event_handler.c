/**
 * @file mqtt_event_handler.c
 * @brief Centralized MQTT Event Handler Implementation
 *
 * Subscribes to all Zigbee-related events and publishes them to MQTT.
 * This decouples Zigbee modules from direct MQTT dependencies.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "mqtt_event_handler.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/device/device_registry.h"
#include "mqtt/gateway_mqtt.h"
#include "mqtt/mqtt_topics.h"
#include "core/memory/memory_manager_ng.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "MQTT_EVT_HANDLER";

/* ============================================================================
 * MQTT Topics for Events
 * ============================================================================ */

#define TOPIC_ZB_ROUTER_STATUS      "zigbee2mqtt/bridge/router/state"
#define TOPIC_ZB_ROUTER_PARENT      "zigbee2mqtt/bridge/router/parent"
#define TOPIC_ZB_ROUTER_NEIGHBORS   "zigbee2mqtt/bridge/router/neighbors"
#define TOPIC_ZB_TOPOLOGY           "zigbee2mqtt/bridge/response/networkmap"
#define TOPIC_ZB_TOUCHLINK          "zigbee2mqtt/bridge/response/touchlink"
#define TOPIC_ZB_ALARM              "zigbee2mqtt/bridge/event/alarm"
#define TOPIC_ZB_REPORTING          "zigbee2mqtt/bridge/response/configure_reporting"
#define TOPIC_BACKUP                "zigbee2mqtt/bridge/response/backup"
#define TOPIC_RESTORE               "zigbee2mqtt/bridge/response/restore"
#define TOPIC_BACKUP_LIST           "zigbee2mqtt/bridge/response/backup/list"
#define TOPIC_DRLC                  "zigbee2mqtt/bridge/drlc/event"
#define TOPIC_BATTERY               "zigbee2mqtt/bridge/event/battery"
#define TOPIC_OTA_PROGRESS          "zigbee2mqtt/bridge/ota/progress"
#define TOPIC_ZB_OTA_IMAGES         "zigbee2mqtt/bridge/ota/images"
#define TOPIC_ZB_OTA_STATUS_FMT     "zigbee2mqtt/bridge/ota/status/%s"

/* ============================================================================
 * Module State
 * ============================================================================ */

static bool s_initialized = false;
static uint32_t s_events_handled = 0;
static uint32_t s_publish_errors = 0;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * @brief Format IEEE address as hex string
 */
static void format_ieee_addr(uint64_t ieee_addr, char *buffer, size_t size)
{
    snprintf(buffer, size, "0x%016" PRIx64, ieee_addr);
}

/**
 * @brief Publish JSON to MQTT with error tracking
 */
static esp_err_t publish_json(const char *topic, cJSON *json, int qos, bool retain)
{
    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected, skipping publish to %s", topic);
        return ESP_ERR_INVALID_STATE;
    }

    char *json_str = cJSON_PrintUnformatted(json);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize JSON for %s", topic);
        s_publish_errors++;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = mqtt_client_publish(topic, json_str, strlen(json_str), qos, retain);
    cJSON_free(json_str);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish to %s: %s", topic, esp_err_to_name(ret));
        s_publish_errors++;
    }

    return ret;
}

/* ============================================================================
 * Event Handlers - Router Status
 * ============================================================================ */

static void publish_router_status(const evt_zb_router_status_t *evt)
{
    /* Publish basic router status */
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    char ieee_str[19];
    format_ieee_addr(evt->ieee_addr, ieee_str, sizeof(ieee_str));

    cJSON_AddStringToObject(json, "ieee_address", ieee_str);
    cJSON_AddNumberToObject(json, "network_address", evt->short_addr);
    cJSON_AddBoolToObject(json, "online", evt->online);
    cJSON_AddNumberToObject(json, "lqi", evt->lqi);
    cJSON_AddNumberToObject(json, "rssi", evt->rssi);

    publish_json(TOPIC_ZB_ROUTER_STATUS, json, 0, false);
    cJSON_Delete(json);

    /* Publish parent info if provided */
    if (evt->json_parent) {
        mqtt_client_publish(TOPIC_ZB_ROUTER_PARENT, evt->json_parent, 0, 1, false);
    }

    /* Publish neighbors if provided */
    if (evt->json_neighbors) {
        mqtt_client_publish(TOPIC_ZB_ROUTER_NEIGHBORS, evt->json_neighbors, 0, 1, false);
    }
}

/* ============================================================================
 * Event Handlers - Topology
 * ============================================================================ */

static void publish_topology_response(const evt_zb_topology_response_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    char ieee_str[19];
    format_ieee_addr(evt->coordinator_ieee, ieee_str, sizeof(ieee_str));

    cJSON_AddStringToObject(json, "coordinator", ieee_str);
    cJSON_AddNumberToObject(json, "device_count", evt->device_count);

    if (evt->json_topology) {
        cJSON *topology = cJSON_Parse(evt->json_topology);
        if (topology) {
            cJSON_AddItemToObject(json, "topology", topology);
        }
    }

    publish_json(TOPIC_ZB_TOPOLOGY, json, 1, false);
    cJSON_Delete(json);
}

/* ============================================================================
 * Event Handlers - Touchlink
 * ============================================================================ */

static void publish_touchlink_result(const evt_zb_touchlink_result_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    char ieee_str[19];
    format_ieee_addr(evt->ieee_addr, ieee_str, sizeof(ieee_str));

    cJSON_AddStringToObject(json, "ieee_address", ieee_str);
    cJSON_AddNumberToObject(json, "network_address", evt->short_addr);
    cJSON_AddBoolToObject(json, "success", evt->success);
    if (!evt->success) {
        cJSON_AddNumberToObject(json, "error_code", evt->error_code);
    }

    publish_json(TOPIC_ZB_TOUCHLINK, json, 1, false);
    cJSON_Delete(json);
}

/* ============================================================================
 * Event Handlers - Alarms
 * ============================================================================ */

static void publish_alarm(const evt_zb_alarm_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    char ieee_str[19];
    format_ieee_addr(evt->ieee_addr, ieee_str, sizeof(ieee_str));

    cJSON_AddStringToObject(json, "ieee_address", ieee_str);
    cJSON_AddNumberToObject(json, "network_address", evt->short_addr);
    cJSON_AddNumberToObject(json, "endpoint", evt->endpoint);
    cJSON_AddNumberToObject(json, "cluster", evt->cluster_id);
    cJSON_AddNumberToObject(json, "alarm_code", evt->alarm_code);
    if (evt->description) {
        cJSON_AddStringToObject(json, "description", evt->description);
    }

    publish_json(TOPIC_ZB_ALARM, json, 0, false);
    cJSON_Delete(json);
}

/* ============================================================================
 * Event Handlers - Reporting Configuration
 * ============================================================================ */

static void publish_reporting_response(const evt_zb_reporting_response_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    char ieee_str[19];
    format_ieee_addr(evt->ieee_addr, ieee_str, sizeof(ieee_str));

    cJSON_AddStringToObject(json, "ieee_address", ieee_str);
    cJSON_AddNumberToObject(json, "network_address", evt->short_addr);
    cJSON_AddNumberToObject(json, "cluster", evt->cluster_id);
    cJSON_AddNumberToObject(json, "attribute", evt->attribute_id);
    cJSON_AddNumberToObject(json, "min_interval", evt->min_interval);
    cJSON_AddNumberToObject(json, "max_interval", evt->max_interval);
    cJSON_AddBoolToObject(json, "success", evt->success);
    if (!evt->success) {
        cJSON_AddNumberToObject(json, "error_code", evt->error_code);
    }

    publish_json(TOPIC_ZB_REPORTING, json, 1, false);
    cJSON_Delete(json);
}

/* ============================================================================
 * Event Handlers - Backup
 * ============================================================================ */

static void publish_backup_response(const evt_backup_response_t *evt)
{
    /* Determine topic based on operation type */
    const char *topic = TOPIC_BACKUP;
    switch (evt->operation) {
        case BACKUP_OP_RESTORE:
            topic = TOPIC_RESTORE;
            break;
        case BACKUP_OP_LIST:
            topic = TOPIC_BACKUP_LIST;
            break;
        default:
            topic = TOPIC_BACKUP;
            break;
    }

    /* If json_payload is provided, publish it directly (for list/restore responses) */
    if (evt->json_payload) {
        mqtt_client_publish(topic, evt->json_payload, 0, 1, false);
        return;
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    static const char *op_names[] = {"save", "restore", "delete", "list"};
    const char *op_name = (evt->operation < 4) ? op_names[evt->operation] : "unknown";

    cJSON_AddStringToObject(json, "operation", op_name);
    cJSON_AddBoolToObject(json, "success", evt->success);

    if (evt->filename) {
        cJSON_AddStringToObject(json, "filename", evt->filename);
    }
    if (evt->error_msg) {
        cJSON_AddStringToObject(json, "error", evt->error_msg);
    }
    if (evt->file_size > 0) {
        cJSON_AddNumberToObject(json, "size", evt->file_size);
    }

    publish_json(topic, json, 1, false);
    cJSON_Delete(json);
}

/* ============================================================================
 * Event Handlers - Demand Response
 * ============================================================================ */

static void publish_drlc_event(const evt_drlc_load_control_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    char ieee_str[19];
    format_ieee_addr(evt->ieee_addr, ieee_str, sizeof(ieee_str));

    cJSON_AddStringToObject(json, "ieee_address", ieee_str);
    cJSON_AddNumberToObject(json, "network_address", evt->short_addr);
    cJSON_AddNumberToObject(json, "event_id", evt->event_id);
    cJSON_AddNumberToObject(json, "criticality", evt->criticality_level);
    cJSON_AddNumberToObject(json, "duration_minutes", evt->duration_min);
    cJSON_AddBoolToObject(json, "opt_out_available", evt->opt_out_available);

    if (evt->json_payload) {
        cJSON *payload = cJSON_Parse(evt->json_payload);
        if (payload) {
            cJSON_AddItemToObject(json, "payload", payload);
        }
    }

    publish_json(TOPIC_DRLC, json, 0, false);
    cJSON_Delete(json);
}

/* ============================================================================
 * Event Handlers - Battery
 * ============================================================================ */

static void publish_battery_changed(const evt_device_battery_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    char ieee_str[19];
    format_ieee_addr(evt->ieee_addr, ieee_str, sizeof(ieee_str));

    cJSON_AddStringToObject(json, "ieee_address", ieee_str);
    cJSON_AddNumberToObject(json, "network_address", evt->short_addr);

    if (evt->percentage != 255) {
        cJSON_AddNumberToObject(json, "percentage", evt->percentage);
    }
    if (evt->voltage_mv > 0) {
        cJSON_AddNumberToObject(json, "voltage_mv", evt->voltage_mv);
    }
    cJSON_AddBoolToObject(json, "low_battery", evt->low_battery);

    publish_json(TOPIC_BATTERY, json, 0, false);
    cJSON_Delete(json);
}

/* ============================================================================
 * Event Handlers - Binding
 * ============================================================================ */

static void publish_bind_response(const evt_zb_bind_t *evt, bool success)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    char ieee_str[19];
    format_ieee_addr(evt->src_ieee, ieee_str, sizeof(ieee_str));

    cJSON_AddStringToObject(json, "source", ieee_str);
    cJSON_AddNumberToObject(json, "source_endpoint", evt->src_endpoint);
    cJSON_AddNumberToObject(json, "cluster", evt->cluster_id);

    if (evt->dst_ieee != 0) {
        char dst_ieee_str[19];
        format_ieee_addr(evt->dst_ieee, dst_ieee_str, sizeof(dst_ieee_str));
        cJSON_AddStringToObject(json, "target", dst_ieee_str);
        cJSON_AddNumberToObject(json, "target_endpoint", evt->dst_endpoint);
    } else {
        cJSON_AddNumberToObject(json, "target_group", evt->dst_group);
    }

    cJSON_AddBoolToObject(json, "success", success);
    if (!success) {
        cJSON_AddNumberToObject(json, "error_code", evt->error_code);
    }

    publish_json("zigbee2mqtt/bridge/response/bind", json, 1, false);
    cJSON_Delete(json);
}

/* ============================================================================
 * Event Handlers - Scenes
 * ============================================================================ */

static void publish_scene_activated(const evt_zb_scene_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    cJSON_AddNumberToObject(json, "group_id", evt->group_id);
    cJSON_AddNumberToObject(json, "scene_id", evt->scene_id);
    if (evt->scene_name) {
        cJSON_AddStringToObject(json, "scene_name", evt->scene_name);
    }

    publish_json("zigbee2mqtt/bridge/event/scene", json, 0, false);
    cJSON_Delete(json);
}

/* ============================================================================
 * Event Handlers - OTA Progress
 * ============================================================================ */

static void publish_ota_progress(const evt_ota_progress_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    cJSON_AddNumberToObject(json, "bytes_written", evt->bytes_written);
    cJSON_AddNumberToObject(json, "total_size", evt->total_size);
    cJSON_AddNumberToObject(json, "percent", evt->percent);

    publish_json(TOPIC_OTA_PROGRESS, json, 0, false);
    cJSON_Delete(json);
}

static void publish_ota_complete(const evt_ota_complete_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    cJSON_AddStringToObject(json, "status", "complete");
    cJSON_AddNumberToObject(json, "total_size", evt->total_size);
    cJSON_AddNumberToObject(json, "duration_ms", evt->duration_ms);
    cJSON_AddBoolToObject(json, "restart_pending", evt->restart_pending);

    publish_json("zigbee2mqtt/bridge/ota/status", json, 1, false);
    cJSON_Delete(json);
}

/* ============================================================================
 * Event Handlers - Zigbee Device OTA
 * ============================================================================ */

/**
 * @brief Format IEEE address from uint8_t array as colon-separated hex string
 */
static void format_ieee_addr_bytes(const uint8_t *ieee_addr, char *buffer, size_t size)
{
    snprintf(buffer, size, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             ieee_addr[7], ieee_addr[6], ieee_addr[5], ieee_addr[4],
             ieee_addr[3], ieee_addr[2], ieee_addr[1], ieee_addr[0]);
}

static void publish_zb_ota_progress(const evt_zb_ota_progress_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    cJSON_AddStringToObject(json, "state", evt->state ? evt->state : "unknown");
    cJSON_AddNumberToObject(json, "progress", evt->percent);
    cJSON_AddNumberToObject(json, "remaining", evt->remaining);

    /* Build device-specific topic */
    char ieee_str[24];
    format_ieee_addr_bytes(evt->ieee_addr, ieee_str, sizeof(ieee_str));

    char topic[128];
    snprintf(topic, sizeof(topic), TOPIC_ZB_OTA_STATUS_FMT, ieee_str);

    publish_json(topic, json, 0, false);
    cJSON_Delete(json);
}

static void publish_zb_ota_complete(const evt_zb_ota_complete_t *evt)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return;

    cJSON_AddStringToObject(json, "state", evt->success ? "success" : "failed");
    cJSON_AddNumberToObject(json, "progress", evt->success ? 100 : 0);
    cJSON_AddNumberToObject(json, "remaining", 0);
    cJSON_AddNumberToObject(json, "duration_ms", evt->duration_ms);

    /* Build device-specific topic */
    char ieee_str[24];
    format_ieee_addr_bytes(evt->ieee_addr, ieee_str, sizeof(ieee_str));

    char topic[128];
    snprintf(topic, sizeof(topic), TOPIC_ZB_OTA_STATUS_FMT, ieee_str);

    publish_json(topic, json, 0, false);
    cJSON_Delete(json);
}

static void publish_zb_ota_images(const evt_zb_ota_images_t *evt)
{
    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected, skipping OTA images publish");
        return;
    }

    /* The json_images is already a formatted JSON string, publish directly */
    if (evt->json_images) {
        esp_err_t ret = mqtt_client_publish(TOPIC_ZB_OTA_IMAGES,
                                            evt->json_images,
                                            strlen(evt->json_images),
                                            0, false);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to publish OTA images: %s", esp_err_to_name(ret));
            s_publish_errors++;
        }
    }
}

/* ============================================================================
 * Event Handlers - Bridge Publish (Layer-clean core→MQTT)
 * ============================================================================ */

static void publish_bridge_data(const evt_bridge_publish_t *evt)
{
    /* Publishers transfer ownership of heap-allocated topic and payload_json
     * strings via the event bus. This handler MUST free them on ALL exit paths. */

    if (!evt->topic || !evt->payload_json) {
        ESP_LOGW(TAG, "Invalid bridge publish event: missing topic or payload");
        s_publish_errors++;
        goto cleanup;
    }

    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected, skipping bridge publish to %s", evt->topic);
        goto cleanup;
    }

    esp_err_t ret = mqtt_client_publish(evt->topic, evt->payload_json,
                                        strlen(evt->payload_json),
                                        evt->qos, evt->retain);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Published bridge data to %s", evt->topic);
    } else {
        ESP_LOGW(TAG, "Failed to publish bridge data to %s: %s",
                 evt->topic, esp_err_to_name(ret));
        s_publish_errors++;
    }

cleanup:
    free((void *)evt->topic);
    free((void *)evt->payload_json);
}

/* ============================================================================
 * Event Handlers - HA Discovery
 * ============================================================================ */

static void publish_ha_discovery(const evt_ha_discovery_publish_t *evt)
{
    /* Publishers transfer ownership of heap-allocated topic and payload_json
     * strings via the event bus. This handler MUST free them on ALL exit paths. */

    if (!evt->topic || !evt->payload_json) {
        ESP_LOGW(TAG, "Invalid HA discovery event: missing topic or payload");
        s_publish_errors++;
        goto cleanup;
    }

    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected, skipping HA discovery publish");
        goto cleanup;
    }

    esp_err_t ret = mqtt_client_publish(evt->topic, evt->payload_json,
                                        strlen(evt->payload_json),
                                        evt->qos, evt->retain);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Published HA discovery to %s", evt->topic);
    } else {
        ESP_LOGW(TAG, "Failed to publish HA discovery to %s: %s",
                 evt->topic, esp_err_to_name(ret));
        s_publish_errors++;
    }

cleanup:
    free((void *)evt->topic);
    free((void *)evt->payload_json);
}

/* ============================================================================
 * Event Handlers - Device Availability
 * ============================================================================ */

static void publish_device_availability(const evt_device_availability_t *evt)
{
#ifdef CONFIG_ESPHOME_PRIMARY_INTEGRATION
    /* Device availability handled by ESPHome adapter, skip MQTT */
    return;
#endif

    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected, skipping availability publish");
        return;
    }

    /* Look up device to get friendly name for topic construction */
    device_t *dev = device_registry_get(evt->device_id);
    if (!dev) {
        ESP_LOGW(TAG, "Device 0x%016" PRIx64 " not found for availability publish",
                 evt->device_id);
        return;
    }

    /* Build availability topic */
    const char *display_name = (dev->friendly_name[0] != '\0')
        ? dev->friendly_name : NULL;
    char ieee_buf[24];
    if (display_name == NULL) {
        snprintf(ieee_buf, sizeof(ieee_buf), "0x%016llx", (unsigned long long)dev->id);
        display_name = ieee_buf;
    }

    char avail_topic[MQTT_TOPIC_MAX_LEN];
    esp_err_t ret = mqtt_topic_device_availability(display_name, avail_topic,
                                                    sizeof(avail_topic));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to build availability topic for %s", display_name);
        return;
    }

    const char *payload_str = evt->online
        ? "{\"state\":\"online\"}"
        : "{\"state\":\"offline\"}";

    ret = mqtt_client_publish(avail_topic, payload_str, strlen(payload_str), 1, true);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Published availability %s for %s",
                 evt->online ? "online" : "offline", display_name);
    } else {
        ESP_LOGW(TAG, "Failed to publish availability for %s: %s",
                 display_name, esp_err_to_name(ret));
        s_publish_errors++;
    }
}

/* ============================================================================
 * Event Handlers - Device Topics Clear
 * ============================================================================ */

static void clear_device_topics(const evt_device_topics_clear_t *evt)
{
    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected, skipping topic clear");
        return;
    }

    /* Look up device to get friendly name for topic construction */
    device_t *dev = device_registry_get(evt->ieee_addr);
    if (!dev) {
        ESP_LOGW(TAG, "Device 0x%016" PRIx64 " not found for topic clear",
                 evt->ieee_addr);
        return;
    }

    const char *display_name = (dev->friendly_name[0] != '\0')
        ? dev->friendly_name : NULL;
    char ieee_buf[24];
    if (display_name == NULL) {
        snprintf(ieee_buf, sizeof(ieee_buf), "0x%016llx", (unsigned long long)dev->id);
        display_name = ieee_buf;
    }

    /* Clear state topic */
    char topic_buf[MQTT_TOPIC_MAX_LEN];
    if (mqtt_topic_device_state(display_name, topic_buf, sizeof(topic_buf)) == ESP_OK) {
        mqtt_client_publish(topic_buf, "", 0, 1, true);
        ESP_LOGD(TAG, "Cleared retained state topic for %s", display_name);
    }

    /* Clear availability topic */
    if (mqtt_topic_device_availability(display_name, topic_buf, sizeof(topic_buf)) == ESP_OK) {
        mqtt_client_publish(topic_buf, "", 0, 1, true);
        ESP_LOGD(TAG, "Cleared retained availability topic for %s", display_name);
    }

    ESP_LOGI(TAG, "Cleared retained MQTT topics for %s", display_name);
}

/* ============================================================================
 * Event Handlers - Tuya Datapoint
 * ============================================================================ */

static void publish_tuya_datapoint(const evt_tuya_dp_t *evt)
{
    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected, skipping Tuya datapoint publish");
        return;
    }

    /* Get device to find friendly_name for topic */
    device_t *dev = device_registry_get(evt->ieee_addr);
    if (!dev) {
        ESP_LOGW(TAG, "Device 0x%04X not found for Tuya publish", evt->short_addr);
        return;
    }

    /* Copy the name out before using it: this handler runs on the event
     * dispatcher task, and a remove from the MQTT or Zigbee task can zero the
     * registry slot while the string is being read. */
    char friendly_name[sizeof(dev->friendly_name)];
    snprintf(friendly_name, sizeof(friendly_name), "%s", dev->friendly_name);

    /* Build topic: zigbee2mqtt/<friendly_name> */
    char topic[MQTT_TOPIC_MAX_LEN];
    esp_err_t ret = mqtt_topic_device_state(friendly_name, topic, sizeof(topic));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to build topic for Tuya publish: %s", esp_err_to_name(ret));
        return;
    }

    /* If json_state contains complete state JSON, publish directly */
    if (evt->json_state) {
        ret = mqtt_client_publish(topic, evt->json_state, strlen(evt->json_state), 0, false);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to publish Tuya state to %s: %s", topic, esp_err_to_name(ret));
            s_publish_errors++;
        }
    }
}

/* ============================================================================
 * Central Event Handler
 * ============================================================================ */

static void mqtt_event_handler_cb(event_type_t type, void *data,
                                  size_t data_size, void *user_ctx)
{
    (void)user_ctx;

    /* EVT_HA_DISCOVERY_PUBLISH and EVT_BRIDGE_PUBLISH carry ownership-transferred
     * heap strings that must be freed even when MQTT is disconnected.
     * Skip the early return for these event types to avoid memory leaks. */
    if (!mqtt_client_is_connected()
        && type != EVT_HA_DISCOVERY_PUBLISH
        && type != EVT_BRIDGE_PUBLISH) {
        ESP_LOGD(TAG, "MQTT not connected, skipping event %d", type);
        return;
    }

    s_events_handled++;

    switch (type) {
        case EVT_ZB_ROUTER_STATUS:
            if (data && data_size >= sizeof(evt_zb_router_status_t)) {
                publish_router_status((evt_zb_router_status_t *)data);
            }
            break;

        case EVT_ZB_TOPOLOGY_RESPONSE:
            if (data && data_size >= sizeof(evt_zb_topology_response_t)) {
                publish_topology_response((evt_zb_topology_response_t *)data);
            }
            break;

        case EVT_ZB_TOUCHLINK_RESULT:
            if (data && data_size >= sizeof(evt_zb_touchlink_result_t)) {
                publish_touchlink_result((evt_zb_touchlink_result_t *)data);
            }
            break;

        case EVT_ZB_ALARM_TRIGGERED:
            if (data && data_size >= sizeof(evt_zb_alarm_t)) {
                publish_alarm((evt_zb_alarm_t *)data);
            }
            break;

        case EVT_ZB_REPORTING_RESPONSE:
            if (data && data_size >= sizeof(evt_zb_reporting_response_t)) {
                publish_reporting_response((evt_zb_reporting_response_t *)data);
            }
            break;

        case EVT_BACKUP_RESPONSE:
            if (data && data_size >= sizeof(evt_backup_response_t)) {
                publish_backup_response((evt_backup_response_t *)data);
            }
            break;

        case EVT_DRLC_LOAD_CONTROL:
            if (data && data_size >= sizeof(evt_drlc_load_control_t)) {
                publish_drlc_event((evt_drlc_load_control_t *)data);
            }
            break;

        case EVT_DEVICE_BATTERY_CHANGED:
            if (data && data_size >= sizeof(evt_device_battery_t)) {
                publish_battery_changed((evt_device_battery_t *)data);
            }
            break;

        case EVT_ZB_BIND_SUCCESS:
            if (data && data_size >= sizeof(evt_zb_bind_t)) {
                publish_bind_response((evt_zb_bind_t *)data, true);
            }
            break;

        case EVT_ZB_BIND_FAILED:
            if (data && data_size >= sizeof(evt_zb_bind_t)) {
                publish_bind_response((evt_zb_bind_t *)data, false);
            }
            break;

        case EVT_ZB_SCENE_ACTIVATED:
            if (data && data_size >= sizeof(evt_zb_scene_t)) {
                publish_scene_activated((evt_zb_scene_t *)data);
            }
            break;

        case EVT_OTA_PROGRESS:
            if (data && data_size >= sizeof(evt_ota_progress_t)) {
                publish_ota_progress((evt_ota_progress_t *)data);
            }
            break;

        case EVT_OTA_COMPLETE:
            if (data && data_size >= sizeof(evt_ota_complete_t)) {
                publish_ota_complete((evt_ota_complete_t *)data);
            }
            break;

        case EVT_TUYA_DATAPOINT:
            if (data && data_size >= sizeof(evt_tuya_dp_t)) {
                publish_tuya_datapoint((evt_tuya_dp_t *)data);
            }
            break;

        case EVT_ZB_OTA_PROGRESS:
            if (data && data_size >= sizeof(evt_zb_ota_progress_t)) {
                publish_zb_ota_progress((evt_zb_ota_progress_t *)data);
            }
            break;

        case EVT_ZB_OTA_COMPLETE:
            if (data && data_size >= sizeof(evt_zb_ota_complete_t)) {
                publish_zb_ota_complete((evt_zb_ota_complete_t *)data);
            }
            break;

        case EVT_ZB_OTA_IMAGES_CHANGED:
            if (data && data_size >= sizeof(evt_zb_ota_images_t)) {
                publish_zb_ota_images((evt_zb_ota_images_t *)data);
            }
            break;

        case EVT_BRIDGE_PUBLISH:
            if (data && data_size >= sizeof(evt_bridge_publish_t)) {
                publish_bridge_data((evt_bridge_publish_t *)data);
            }
            break;

        case EVT_HA_DISCOVERY_PUBLISH:
            if (data && data_size >= sizeof(evt_ha_discovery_publish_t)) {
                publish_ha_discovery((evt_ha_discovery_publish_t *)data);
            }
            break;

        case EVT_DEVICE_AVAILABILITY_CHANGED:
            if (data && data_size >= sizeof(evt_device_availability_t)) {
                publish_device_availability((evt_device_availability_t *)data);
            }
            break;

        case EVT_DEVICE_TOPICS_CLEAR:
            if (data && data_size >= sizeof(evt_device_topics_clear_t)) {
                clear_device_topics((evt_device_topics_clear_t *)data);
            }
            break;

        default:
            ESP_LOGD(TAG, "Unhandled event type: %d", type);
            break;
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t mqtt_event_handler_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing MQTT event handler");

    /* Subscribe to all Zigbee events that should be published to MQTT */
    esp_err_t ret;

    ret = event_subscribe(EVT_ZB_ROUTER_STATUS, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_ROUTER_STATUS");
    }

    ret = event_subscribe(EVT_ZB_TOPOLOGY_RESPONSE, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_TOPOLOGY_RESPONSE");
    }

    ret = event_subscribe(EVT_ZB_TOUCHLINK_RESULT, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_TOUCHLINK_RESULT");
    }

    ret = event_subscribe(EVT_ZB_ALARM_TRIGGERED, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_ALARM_TRIGGERED");
    }

    ret = event_subscribe(EVT_ZB_REPORTING_RESPONSE, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_REPORTING_RESPONSE");
    }

    ret = event_subscribe(EVT_BACKUP_RESPONSE, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_BACKUP_RESPONSE");
    }

    ret = event_subscribe(EVT_DRLC_LOAD_CONTROL, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_DRLC_LOAD_CONTROL");
    }

    ret = event_subscribe(EVT_DEVICE_BATTERY_CHANGED, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_DEVICE_BATTERY_CHANGED");
    }

    ret = event_subscribe(EVT_ZB_BIND_SUCCESS, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_BIND_SUCCESS");
    }

    ret = event_subscribe(EVT_ZB_BIND_FAILED, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_BIND_FAILED");
    }

    ret = event_subscribe(EVT_ZB_SCENE_ACTIVATED, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_SCENE_ACTIVATED");
    }

    ret = event_subscribe(EVT_OTA_PROGRESS, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_OTA_PROGRESS");
    }

    ret = event_subscribe(EVT_OTA_COMPLETE, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_OTA_COMPLETE");
    }

    ret = event_subscribe(EVT_TUYA_DATAPOINT, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_TUYA_DATAPOINT");
    }

    ret = event_subscribe(EVT_ZB_OTA_PROGRESS, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_OTA_PROGRESS");
    }

    ret = event_subscribe(EVT_ZB_OTA_COMPLETE, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_OTA_COMPLETE");
    }

    ret = event_subscribe(EVT_ZB_OTA_IMAGES_CHANGED, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ZB_OTA_IMAGES_CHANGED");
    }

    ret = event_subscribe(EVT_BRIDGE_PUBLISH, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_BRIDGE_PUBLISH");
    }

    ret = event_subscribe(EVT_HA_DISCOVERY_PUBLISH, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_HA_DISCOVERY_PUBLISH");
    }

    ret = event_subscribe(EVT_DEVICE_AVAILABILITY_CHANGED, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_DEVICE_AVAILABILITY_CHANGED");
    }

    ret = event_subscribe(EVT_DEVICE_TOPICS_CLEAR, mqtt_event_handler_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_DEVICE_TOPICS_CLEAR");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "MQTT event handler initialized");

    return ESP_OK;
}

esp_err_t mqtt_event_handler_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* Unsubscribe from all events */
    event_unsubscribe(EVT_ZB_ROUTER_STATUS, mqtt_event_handler_cb);
    event_unsubscribe(EVT_ZB_TOPOLOGY_RESPONSE, mqtt_event_handler_cb);
    event_unsubscribe(EVT_ZB_TOUCHLINK_RESULT, mqtt_event_handler_cb);
    event_unsubscribe(EVT_ZB_ALARM_TRIGGERED, mqtt_event_handler_cb);
    event_unsubscribe(EVT_ZB_REPORTING_RESPONSE, mqtt_event_handler_cb);
    event_unsubscribe(EVT_BACKUP_RESPONSE, mqtt_event_handler_cb);
    event_unsubscribe(EVT_DRLC_LOAD_CONTROL, mqtt_event_handler_cb);
    event_unsubscribe(EVT_DEVICE_BATTERY_CHANGED, mqtt_event_handler_cb);
    event_unsubscribe(EVT_ZB_BIND_SUCCESS, mqtt_event_handler_cb);
    event_unsubscribe(EVT_ZB_BIND_FAILED, mqtt_event_handler_cb);
    event_unsubscribe(EVT_ZB_SCENE_ACTIVATED, mqtt_event_handler_cb);
    event_unsubscribe(EVT_OTA_PROGRESS, mqtt_event_handler_cb);
    event_unsubscribe(EVT_OTA_COMPLETE, mqtt_event_handler_cb);
    event_unsubscribe(EVT_TUYA_DATAPOINT, mqtt_event_handler_cb);
    event_unsubscribe(EVT_ZB_OTA_PROGRESS, mqtt_event_handler_cb);
    event_unsubscribe(EVT_ZB_OTA_COMPLETE, mqtt_event_handler_cb);
    event_unsubscribe(EVT_ZB_OTA_IMAGES_CHANGED, mqtt_event_handler_cb);
    event_unsubscribe(EVT_BRIDGE_PUBLISH, mqtt_event_handler_cb);
    event_unsubscribe(EVT_HA_DISCOVERY_PUBLISH, mqtt_event_handler_cb);
    event_unsubscribe(EVT_DEVICE_AVAILABILITY_CHANGED, mqtt_event_handler_cb);
    event_unsubscribe(EVT_DEVICE_TOPICS_CLEAR, mqtt_event_handler_cb);

    s_initialized = false;
    ESP_LOGI(TAG, "MQTT event handler deinitialized");

    return ESP_OK;
}

bool mqtt_event_handler_is_initialized(void)
{
    return s_initialized;
}

void mqtt_event_handler_get_stats(uint32_t *events_handled, uint32_t *publish_errors)
{
    if (events_handled) {
        *events_handled = s_events_handled;
    }
    if (publish_errors) {
        *publish_errors = s_publish_errors;
    }
}
