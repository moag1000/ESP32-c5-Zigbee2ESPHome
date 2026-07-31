/**
 * @file mqtt_adapter.c
 * @brief MQTT Adapter Implementation - Event Bus to MQTT Bridge
 *
 * Bridges the event bus system with the MQTT client, enabling event-driven
 * device state publishing and command handling.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "mqtt_adapter.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "core/monitoring/perf_metrics.h"
#include "core/foundation_init.h"
#include "core/memory/memory_manager_ng.h"
#include "mqtt/gateway_mqtt.h"
#include "mqtt/mqtt_topics.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

#include "adapter_interface.h"

static const char *TAG = "MQTT_ADAPTER";

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** @brief MQTT pool buffer size (must match foundation_init.c) */
#define MQTT_POOL_BUFFER_SIZE   512

/* ============================================================================
 * Static State
 * ============================================================================ */

static bool s_initialized = false;
static bool s_mqtt_connected = false;

/* ============================================================================
 * Forward Declarations - Event Handlers
 * ============================================================================ */

static void handle_device_state_changed(event_type_t type, void *data, size_t data_size, void *ctx);
static void handle_device_joined(event_type_t type, void *data, size_t data_size, void *ctx);
static void handle_device_left(event_type_t type, void *data, size_t data_size, void *ctx);
static void handle_mqtt_connected(event_type_t type, void *data, size_t data_size, void *ctx);
static void handle_mqtt_disconnected(event_type_t type, void *data, size_t data_size, void *ctx);
static void handle_power_state_changed(event_type_t type, void *data, size_t data_size, void *ctx);
static void handle_tuya_datapoint(event_type_t type, void *data, size_t data_size, void *ctx);
static void handle_bindings_list(event_type_t type, void *data, size_t data_size, void *ctx);

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * @brief Get display name for a device (friendly_name or IEEE address)
 *
 * @param dev Pointer to device
 * @param buf Output buffer for the name
 * @param buf_size Size of output buffer
 */
static void get_device_display_name(const device_t *dev, char *buf, size_t buf_size)
{
    if (dev->friendly_name[0] != '\0') {
        strncpy(buf, dev->friendly_name, buf_size - 1);
        buf[buf_size - 1] = '\0';
    } else {
        /* Use IEEE address as fallback */
        device_id_to_str(dev->id, buf, buf_size);
    }
}

/**
 * @brief Publish device state JSON to MQTT
 *
 * Uses the MQTT buffer pool for message formatting when available,
 * falling back to cJSON_PrintUnformatted for larger messages or
 * when the pool is exhausted.
 *
 * @param dev Pointer to device
 * @param state_json State JSON object (not consumed)
 * @return ESP_OK on success, error otherwise
 */
static esp_err_t publish_device_state(const device_t *dev, cJSON *state_json)
{
    if (!s_mqtt_connected) {
        ESP_LOGD(TAG, "MQTT not connected, skipping state publish");
        return ESP_ERR_INVALID_STATE;
    }

    if (!dev || !state_json) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build topic */
    char display_name[MQTT_MEDIUM_STR_MAX_LEN];
    get_device_display_name(dev, display_name, sizeof(display_name));

    char topic[MQTT_TOPIC_MAX_LEN];
    esp_err_t ret = mqtt_topic_device_state(display_name, topic, sizeof(topic));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build topic for device %s", display_name);
        return ret;
    }

    /* Try to use MQTT pool buffer for serialization */
    buffer_pool_t mqtt_pool = foundation_get_mqtt_pool();
    char *pool_buffer = NULL;
    char *json_str = NULL;
    bool used_pool = false;

    if (mqtt_pool != NULL) {
        pool_buffer = (char *)mem_pool_acquire(mqtt_pool);
        if (pool_buffer != NULL) {
            /* Try to print JSON directly into pool buffer */
            if (cJSON_PrintPreallocated(state_json, pool_buffer,
                                         MQTT_POOL_BUFFER_SIZE, false)) {
                json_str = pool_buffer;
                used_pool = true;
                ESP_LOGD(TAG, "Using pool buffer for state publish");
            } else {
                /* Buffer too small, release and fall back to malloc */
                mem_pool_release(mqtt_pool, pool_buffer);
                pool_buffer = NULL;
                ESP_LOGD(TAG, "Pool buffer too small, falling back to malloc");
            }
        }
    }

    /* Fall back to regular allocation if pool not available or too small */
    if (!used_pool) {
        json_str = cJSON_PrintUnformatted(state_json);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to serialize state JSON");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Record publish start time for latency measurement */
    int64_t publish_start_us = esp_timer_get_time();

    /* Publish with retain */
    ret = mqtt_client_publish(topic, json_str, strlen(json_str), 1, true);

    /* Release buffer appropriately */
    if (used_pool) {
        mem_pool_release(mqtt_pool, pool_buffer);
    } else {
        cJSON_free(json_str);
    }

    /* Record MQTT publish performance metrics */
    int64_t publish_end_us = esp_timer_get_time();
    uint32_t publish_latency_us = (uint32_t)(publish_end_us - publish_start_us);

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Published state to %s", topic);
        perf_metrics_record(PERF_METRIC_MQTT_PUBLISHED, 1);
        perf_metrics_record(PERF_METRIC_MQTT_PUBLISH_LATENCY_US, publish_latency_us);
    } else {
        ESP_LOGW(TAG, "Failed to publish state to %s: %s", topic, esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Publish device availability to MQTT
 *
 * Uses the MQTT buffer pool when available to format the availability
 * message, falling back to static strings when the pool is exhausted.
 *
 * @param dev Pointer to device
 * @param online true for online, false for offline
 * @return ESP_OK on success, error otherwise
 */
static esp_err_t publish_device_availability(const device_t *dev, bool online)
{
    if (!s_mqtt_connected) {
        ESP_LOGD(TAG, "MQTT not connected, skipping availability publish");
        return ESP_ERR_INVALID_STATE;
    }

    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build topic */
    char display_name[MQTT_MEDIUM_STR_MAX_LEN];
    get_device_display_name(dev, display_name, sizeof(display_name));

    char topic[MQTT_TOPIC_MAX_LEN];
    esp_err_t ret = mqtt_topic_device_availability(display_name, topic, sizeof(topic));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build availability topic for %s", display_name);
        return ret;
    }

    /* Record publish start time for latency measurement */
    int64_t publish_start_us = esp_timer_get_time();

    /* Availability payloads are small, use static strings (no allocation needed) */
    const char *payload = online ? "{\"state\":\"online\"}" : "{\"state\":\"offline\"}";
    ret = mqtt_client_publish(topic, payload, strlen(payload), 1, true);

    /* Record MQTT publish performance metrics */
    int64_t publish_end_us = esp_timer_get_time();
    uint32_t publish_latency_us = (uint32_t)(publish_end_us - publish_start_us);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Published availability %s for %s", online ? "online" : "offline", display_name);
        perf_metrics_record(PERF_METRIC_MQTT_PUBLISHED, 1);
        perf_metrics_record(PERF_METRIC_MQTT_PUBLISH_LATENCY_US, publish_latency_us);
    } else {
        ESP_LOGW(TAG, "Failed to publish availability for %s: %s", display_name, esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Iterator callback to republish all device states
 */
/**
 * @brief Republish all device states (called on MQTT reconnect)
 *
 * Uses collect-then-iterate pattern to avoid holding registry mutex
 * during MQTT publishing (deadlock risk per AP-1.3).
 */
static void republish_all_device_states(void)
{
    ESP_LOGI(TAG, "Republishing all device states...");

    device_id_t ids[DEVICE_REGISTRY_MAX_DEVICES];
    size_t count;
    if (device_registry_collect_ids(ids, DEVICE_REGISTRY_MAX_DEVICES, &count) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to collect device IDs for republish");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        device_t *dev = device_registry_get(ids[i]);
        if (dev == NULL || dev->protocol == DEV_PROTOCOL_VIRTUAL) {
            continue;
        }

        cJSON *state = device_registry_state_dup(dev->id);
        if (state) {
            publish_device_state(dev, state);
            cJSON_Delete(state);
        }

        bool online = (dev->availability == DEV_AVAIL_ONLINE);
        publish_device_availability(dev, online);
    }

    ESP_LOGI(TAG, "Device state republish complete (%zu devices)", count);
}

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

/**
 * @brief Handle EVT_DEVICE_STATE_CHANGED event
 *
 * Publishes device state to MQTT when state changes.
 */
static void handle_device_state_changed(event_type_t type, void *data, size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

#ifdef CONFIG_ESPHOME_PRIMARY_INTEGRATION
    /* State updates handled by ESPHome adapter, skip MQTT */
    return;
#endif

    /* Validate event data - must match evt_device_state_t structure */
    /* Note: ESPHome uses a different structure for state events, ignore those */
    if (!data || data_size != sizeof(evt_device_state_t)) {
        /* Silently ignore - likely ESPHome event with different structure */
        return;
    }

    const evt_device_state_t *evt = (const evt_device_state_t *)data;

    /* Look up device in registry */
    device_t *dev = device_registry_get(evt->ieee_addr);
    if (!dev) {
        ESP_LOGD(TAG, "Device 0x%016llX not in registry, skipping state publish",
                 (unsigned long long)evt->ieee_addr);
        return;
    }

    /*
     * Get state from device registry.
     *
     * Note: We always use the registry state instead of evt->json_state because
     * the event bus is asynchronous - by the time this handler runs, the original
     * json_state string may have been freed by the publisher (use-after-free).
     * The converter already merges state into device_registry before publishing
     * the event, so the registry always has the latest state.
     */
    cJSON *state = device_registry_state_dup(evt->ieee_addr);
    if (state) {
        publish_device_state(dev, state);
        cJSON_Delete(state);
    } else {
        ESP_LOGD(TAG, "No state in registry for device 0x%016llX",
                 (unsigned long long)evt->ieee_addr);
    }
}

/**
 * @brief Handle EVT_DEVICE_JOINED event
 *
 * Publishes device availability as online and logs the join.
 */
static void handle_device_joined(event_type_t type, void *data, size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

#ifdef CONFIG_ESPHOME_PRIMARY_INTEGRATION
    /* Device availability/state handled by ESPHome adapter, skip MQTT */
    return;
#endif

    if (!data || data_size < sizeof(evt_device_joined_t)) {
        ESP_LOGW(TAG, "Invalid device joined event data");
        return;
    }

    const evt_device_joined_t *evt = (const evt_device_joined_t *)data;

    ESP_LOGI(TAG, "Device joined: IEEE=0x%016llX, short=0x%04X",
             (unsigned long long)evt->ieee_addr, evt->short_addr);

    /* Look up device in registry */
    device_t *dev = device_registry_get(evt->ieee_addr);
    if (!dev) {
        ESP_LOGD(TAG, "Joined device not yet in registry, availability will be published later");
        return;
    }

    /* Publish availability online */
    publish_device_availability(dev, true);

    /* Optionally publish initial state if available */
    cJSON *state = device_registry_state_dup(evt->ieee_addr);
    if (state) {
        publish_device_state(dev, state);
        cJSON_Delete(state);
    }
}

/**
 * @brief Handle EVT_DEVICE_LEFT event
 *
 * Publishes device availability as offline.
 */
static void handle_device_left(event_type_t type, void *data, size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

#ifdef CONFIG_ESPHOME_PRIMARY_INTEGRATION
    /* Device availability handled by ESPHome adapter, skip MQTT */
    return;
#endif

    if (!data || data_size < sizeof(evt_device_left_t)) {
        ESP_LOGW(TAG, "Invalid device left event data");
        return;
    }

    const evt_device_left_t *evt = (const evt_device_left_t *)data;

    ESP_LOGI(TAG, "Device left: IEEE=0x%016llX, short=0x%04X, rejoin_expected=%d",
             (unsigned long long)evt->ieee_addr, evt->short_addr, evt->rejoin_expected);

    /* Look up device in registry */
    device_t *dev = device_registry_get(evt->ieee_addr);
    if (!dev) {
        ESP_LOGD(TAG, "Left device not in registry");
        return;
    }

    /* Publish availability offline */
    publish_device_availability(dev, false);
}

/**
 * @brief Handle EVT_MQTT_CONNECTED event
 *
 * Republishes all device states when MQTT connection is established.
 */
static void handle_mqtt_connected(event_type_t type, void *data, size_t data_size, void *ctx)
{
    (void)type;
    (void)data;
    (void)data_size;
    (void)ctx;

    ESP_LOGI(TAG, "MQTT connected event received");
    s_mqtt_connected = true;

    /* Republish all device states */
    republish_all_device_states();
}

/**
 * @brief Handle EVT_MQTT_DISCONNECTED event
 *
 * Updates internal state to prevent publish attempts while disconnected.
 */
static void handle_mqtt_disconnected(event_type_t type, void *data, size_t data_size, void *ctx)
{
    (void)type;
    (void)data;
    (void)data_size;
    (void)ctx;

    ESP_LOGI(TAG, "MQTT disconnected event received");
    s_mqtt_connected = false;
}

/**
 * @brief Handle EVT_POWER_STATE_CHANGED event
 *
 * Publishes electrical measurement data (power, voltage, current) to MQTT.
 */
static void handle_power_state_changed(event_type_t type, void *data, size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

#ifdef CONFIG_ESPHOME_PRIMARY_INTEGRATION
    return;
#endif

    if (!data || data_size < sizeof(evt_power_state_t)) {
        ESP_LOGW(TAG, "Invalid power state event data");
        return;
    }

    if (!s_mqtt_connected) {
        ESP_LOGD(TAG, "MQTT not connected, skipping power state publish");
        return;
    }

    const evt_power_state_t *evt = (const evt_power_state_t *)data;

    /* Look up device in registry */
    device_t *dev = device_registry_get(evt->ieee_addr);
    if (!dev) {
        ESP_LOGD(TAG, "Device 0x%016llX not in registry, skipping power state publish",
                 (unsigned long long)evt->ieee_addr);
        return;
    }

    /* Build power state JSON */
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        ESP_LOGE(TAG, "Failed to create power state JSON");
        return;
    }

    cJSON_AddNumberToObject(json, "power", evt->power_w);
    cJSON_AddNumberToObject(json, "voltage", evt->voltage_v);
    cJSON_AddNumberToObject(json, "current", evt->current_ma / 1000.0);  /* Convert to Amps */
    if (evt->power_factor > 0) {
        cJSON_AddNumberToObject(json, "power_factor", evt->power_factor);
    }
    if (evt->energy_kwh > 0) {
        cJSON_AddNumberToObject(json, "energy", evt->energy_kwh);
    }

    publish_device_state(dev, json);
    cJSON_Delete(json);
}

/**
 * @brief Handle EVT_TUYA_DATAPOINT event
 *
 * Publishes Tuya datapoint state to MQTT.
 */
static void handle_tuya_datapoint(event_type_t type, void *data, size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

#ifdef CONFIG_ESPHOME_PRIMARY_INTEGRATION
    return;
#endif

    if (!data || data_size < sizeof(evt_tuya_dp_t)) {
        ESP_LOGW(TAG, "Invalid Tuya datapoint event data");
        return;
    }

    if (!s_mqtt_connected) {
        ESP_LOGD(TAG, "MQTT not connected, skipping Tuya datapoint publish");
        return;
    }

    const evt_tuya_dp_t *evt = (const evt_tuya_dp_t *)data;

    /* Look up device in registry */
    device_t *dev = device_registry_get(evt->ieee_addr);
    if (!dev) {
        ESP_LOGD(TAG, "Device 0x%016llX not in registry, skipping Tuya datapoint publish",
                 (unsigned long long)evt->ieee_addr);
        return;
    }

    /* Parse and publish the JSON state from the event */
    if (evt->json_state) {
        cJSON *json = cJSON_Parse(evt->json_state);
        if (json) {
            publish_device_state(dev, json);
            cJSON_Delete(json);
        } else {
            ESP_LOGW(TAG, "Failed to parse Tuya datapoint JSON for device 0x%016llX",
                     (unsigned long long)evt->ieee_addr);
        }
    }
}

/**
 * @brief Handle EVT_ZB_BINDINGS_LIST event
 *
 * Publishes the Zigbee bindings list to MQTT.
 */
static void handle_bindings_list(event_type_t type, void *data, size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

    if (!data || data_size < sizeof(evt_zb_bindings_list_t)) {
        ESP_LOGW(TAG, "Invalid bindings list event data");
        return;
    }

    if (!s_mqtt_connected) {
        ESP_LOGD(TAG, "MQTT not connected, skipping bindings list publish");
        return;
    }

    const evt_zb_bindings_list_t *evt = (const evt_zb_bindings_list_t *)data;

    if (!evt->json_bindings) {
        ESP_LOGW(TAG, "Bindings list event has no JSON data");
        return;
    }

    /* Publish bindings list to bridge/bindings topic */
    static const char *TOPIC_BINDINGS_LIST = "zigbee2mqtt/bridge/bindings";

    esp_err_t ret = mqtt_client_publish(TOPIC_BINDINGS_LIST, evt->json_bindings,
                                        strlen(evt->json_bindings), 1, true);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Published bindings list (%zu bindings) to %s",
                 evt->binding_count, TOPIC_BINDINGS_LIST);
    } else {
        ESP_LOGW(TAG, "Failed to publish bindings list: %s", esp_err_to_name(ret));
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t mqtt_adapter_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "MQTT adapter already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing MQTT adapter...");

    esp_err_t ret;

    /* Subscribe to device state changed events */
    ret = event_subscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_DEVICE_STATE_CHANGED: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Subscribe to device joined events */
    ret = event_subscribe(EVT_DEVICE_JOINED, handle_device_joined, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_DEVICE_JOINED: %s", esp_err_to_name(ret));
        event_unsubscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed);
        return ret;
    }

    /* Subscribe to device left events */
    ret = event_subscribe(EVT_DEVICE_LEFT, handle_device_left, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_DEVICE_LEFT: %s", esp_err_to_name(ret));
        event_unsubscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed);
        event_unsubscribe(EVT_DEVICE_JOINED, handle_device_joined);
        return ret;
    }

    /* Subscribe to MQTT connected events */
    ret = event_subscribe(EVT_MQTT_CONNECTED, handle_mqtt_connected, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_MQTT_CONNECTED: %s", esp_err_to_name(ret));
        event_unsubscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed);
        event_unsubscribe(EVT_DEVICE_JOINED, handle_device_joined);
        event_unsubscribe(EVT_DEVICE_LEFT, handle_device_left);
        return ret;
    }

    /* Subscribe to MQTT disconnected events */
    ret = event_subscribe(EVT_MQTT_DISCONNECTED, handle_mqtt_disconnected, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_MQTT_DISCONNECTED: %s", esp_err_to_name(ret));
        event_unsubscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed);
        event_unsubscribe(EVT_DEVICE_JOINED, handle_device_joined);
        event_unsubscribe(EVT_DEVICE_LEFT, handle_device_left);
        event_unsubscribe(EVT_MQTT_CONNECTED, handle_mqtt_connected);
        return ret;
    }

    /* Subscribe to power state changed events */
    ret = event_subscribe(EVT_POWER_STATE_CHANGED, handle_power_state_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_POWER_STATE_CHANGED: %s", esp_err_to_name(ret));
        /* Non-critical, continue without power events */
    }

    /* Subscribe to Tuya datapoint events */
    ret = event_subscribe(EVT_TUYA_DATAPOINT, handle_tuya_datapoint, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_TUYA_DATAPOINT: %s", esp_err_to_name(ret));
        /* Non-critical, continue without Tuya events */
    }

    /* Subscribe to bindings list events */
    ret = event_subscribe(EVT_ZB_BINDINGS_LIST, handle_bindings_list, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_ZB_BINDINGS_LIST: %s", esp_err_to_name(ret));
        /* Non-critical, continue without bindings list events */
    }

    /* Check if MQTT is already connected */
    s_mqtt_connected = mqtt_client_is_connected();

    s_initialized = true;
    ESP_LOGI(TAG, "MQTT adapter initialized successfully");
    return ESP_OK;
}

esp_err_t mqtt_adapter_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "MQTT adapter not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing MQTT adapter...");

    /* Unsubscribe from all events */
    event_unsubscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed);
    event_unsubscribe(EVT_DEVICE_JOINED, handle_device_joined);
    event_unsubscribe(EVT_DEVICE_LEFT, handle_device_left);
    event_unsubscribe(EVT_MQTT_CONNECTED, handle_mqtt_connected);
    event_unsubscribe(EVT_MQTT_DISCONNECTED, handle_mqtt_disconnected);
    event_unsubscribe(EVT_POWER_STATE_CHANGED, handle_power_state_changed);
    event_unsubscribe(EVT_TUYA_DATAPOINT, handle_tuya_datapoint);
    event_unsubscribe(EVT_ZB_BINDINGS_LIST, handle_bindings_list);

    s_mqtt_connected = false;
    s_initialized = false;

    ESP_LOGI(TAG, "MQTT adapter deinitialized");
    return ESP_OK;
}

void mqtt_adapter_on_connected(void)
{
    if (!s_initialized) {
        return;
    }

    s_mqtt_connected = true;
    ESP_LOGI(TAG, "MQTT adapter notified: connected");

    /* Publish EVT_MQTT_CONNECTED event for other subscribers */
    evt_mqtt_connected_t evt = {
        .broker_uri = NULL, /* Not available in this context */
        .session_present = false,
    };
    event_publish(EVT_MQTT_CONNECTED, &evt, sizeof(evt));
}

void mqtt_adapter_on_disconnected(void)
{
    if (!s_initialized) {
        return;
    }

    s_mqtt_connected = false;
    ESP_LOGI(TAG, "MQTT adapter notified: disconnected");

    /* Publish EVT_MQTT_DISCONNECTED event for other subscribers */
    evt_mqtt_disconnected_t evt = {
        .error_code = 0,
        .reconnecting = true, /* Assume auto-reconnect is enabled */
    };
    event_publish(EVT_MQTT_DISCONNECTED, &evt, sizeof(evt));
}

bool mqtt_adapter_is_initialized(void)
{
    return s_initialized;
}

bool mqtt_adapter_is_connected(void)
{
    return s_mqtt_connected;
}

/* Adapter interface vtable */
const adapter_ops_t mqtt_adapter_ops = {
    .init = mqtt_adapter_init,
    .start = NULL,
    .stop = NULL,
    .deinit = mqtt_adapter_deinit,
    .is_running = NULL,
};
