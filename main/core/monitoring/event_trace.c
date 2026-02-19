/**
 * @file event_trace.c
 * @brief Event Trace Module Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "event_trace.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/memory/memory_manager_ng.h"
#include "gateway_mqtt.h"
#include "mqtt/mqtt_helpers.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "evt_trace";

/* ============================================================================
 * Module State
 * ============================================================================ */

/** @brief Module initialized flag */
static bool s_initialized = false;

/** @brief Tracing enabled flag */
static bool s_enabled = true;

/** @brief MQTT publishing enabled flag */
static bool s_mqtt_enabled = false;

/** @brief Event type filter mask */
static uint32_t s_filter_mask = EVENT_TRACE_FILTER_ALL;

/** @brief Ring buffer for event entries */
static event_trace_entry_t *s_ring_buffer = NULL;

/** @brief Ring buffer write index (next write position) */
static size_t s_ring_write_idx = 0;

/** @brief Number of entries currently in ring buffer */
static size_t s_ring_count = 0;

/** @brief Statistics */
static event_trace_stats_t s_stats = {0};

/** @brief Mutex for ring buffer access */
static SemaphoreHandle_t s_mutex = NULL;

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * @brief Get current uptime in milliseconds
 */
static uint32_t get_timestamp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * @brief Check if event type passes the filter
 */
static bool event_passes_filter(event_type_t type)
{
    if (type >= EVT_COUNT) {
        return false;
    }
    return (s_filter_mask & (1U << type)) != 0;
}

/**
 * @brief Extract device ID from event data if applicable
 */
static void extract_device_id(event_type_t type, const void *data, trace_device_id_t *device_id)
{
    memset(device_id, 0, sizeof(trace_device_id_t));
    device_id->valid = false;

    if (data == NULL) {
        return;
    }

    switch (type) {
        case EVT_DEVICE_JOINED: {
            const evt_device_joined_t *evt = (const evt_device_joined_t *)data;
            device_id->ieee_addr = evt->ieee_addr;
            device_id->short_addr = evt->short_addr;
            device_id->valid = true;
            break;
        }
        case EVT_DEVICE_LEFT: {
            const evt_device_left_t *evt = (const evt_device_left_t *)data;
            device_id->ieee_addr = evt->ieee_addr;
            device_id->short_addr = evt->short_addr;
            device_id->valid = true;
            break;
        }
        case EVT_DEVICE_STATE_CHANGED: {
            const evt_device_state_t *evt = (const evt_device_state_t *)data;
            device_id->ieee_addr = evt->ieee_addr;
            device_id->short_addr = evt->short_addr;
            device_id->valid = true;
            break;
        }
        case EVT_DEVICE_INTERVIEWED: {
            const evt_device_interviewed_t *evt = (const evt_device_interviewed_t *)data;
            device_id->ieee_addr = evt->ieee_addr;
            device_id->short_addr = evt->short_addr;
            device_id->valid = true;
            break;
        }
        case EVT_BLE_DEVICE_FOUND: {
            const evt_ble_device_found_t *evt = (const evt_ble_device_found_t *)data;
            memcpy(device_id->mac, evt->mac, 6);
            device_id->valid = true;
            break;
        }
        case EVT_BLE_DEVICE_LOST: {
            const evt_ble_device_lost_t *evt = (const evt_ble_device_lost_t *)data;
            memcpy(device_id->mac, evt->mac, 6);
            device_id->valid = true;
            break;
        }
        default:
            /* No device ID for this event type */
            break;
    }
}

/**
 * @brief Generate a summary string for an event
 */
static void generate_summary(event_type_t type, const void *data, size_t data_size,
                             const trace_device_id_t *device_id, char *summary, size_t summary_len)
{
    if (data == NULL || data_size == 0) {
        snprintf(summary, summary_len, "%s (no data)", event_type_to_str(type));
        return;
    }

    switch (type) {
        case EVT_DEVICE_JOINED: {
            const evt_device_joined_t *evt = (const evt_device_joined_t *)data;
            snprintf(summary, summary_len, "Device joined: 0x%04X (EP%d)",
                     evt->short_addr, evt->endpoint);
            break;
        }
        case EVT_DEVICE_LEFT: {
            const evt_device_left_t *evt = (const evt_device_left_t *)data;
            snprintf(summary, summary_len, "Device left: 0x%04X (rejoin=%s)",
                     evt->short_addr, evt->rejoin_expected ? "yes" : "no");
            break;
        }
        case EVT_DEVICE_STATE_CHANGED: {
            const evt_device_state_t *evt = (const evt_device_state_t *)data;
            snprintf(summary, summary_len, "State: 0x%04X EP%d cluster=0x%04X attr=0x%04X",
                     evt->short_addr, evt->endpoint, evt->cluster_id, evt->attr_id);
            break;
        }
        case EVT_DEVICE_INTERVIEWED: {
            const evt_device_interviewed_t *evt = (const evt_device_interviewed_t *)data;
            snprintf(summary, summary_len, "Interview %s: 0x%04X (%d endpoints)",
                     evt->success ? "OK" : "FAIL", evt->short_addr, evt->endpoint_count);
            break;
        }
        case EVT_NETWORK_READY: {
            const evt_network_ready_t *evt = (const evt_network_ready_t *)data;
            snprintf(summary, summary_len, "Network ready: ch%d PAN=0x%04X",
                     evt->channel, evt->pan_id);
            break;
        }
        case EVT_MQTT_CONNECTED: {
            const evt_mqtt_connected_t *evt = (const evt_mqtt_connected_t *)data;
            snprintf(summary, summary_len, "MQTT connected (session=%s)",
                     evt->session_present ? "resumed" : "clean");
            break;
        }
        case EVT_MQTT_DISCONNECTED: {
            const evt_mqtt_disconnected_t *evt = (const evt_mqtt_disconnected_t *)data;
            snprintf(summary, summary_len, "MQTT disconnected (err=%d, reconnect=%s)",
                     evt->error_code, evt->reconnecting ? "yes" : "no");
            break;
        }
        case EVT_BLE_DEVICE_FOUND: {
            const evt_ble_device_found_t *evt = (const evt_ble_device_found_t *)data;
            snprintf(summary, summary_len, "BLE found: %02X:%02X:%02X:%02X:%02X:%02X rssi=%d",
                     evt->mac[0], evt->mac[1], evt->mac[2],
                     evt->mac[3], evt->mac[4], evt->mac[5], evt->rssi);
            break;
        }
        case EVT_BLE_DEVICE_LOST: {
            const evt_ble_device_lost_t *evt = (const evt_ble_device_lost_t *)data;
            snprintf(summary, summary_len, "BLE lost: %02X:%02X:%02X:%02X:%02X:%02X",
                     evt->mac[0], evt->mac[1], evt->mac[2],
                     evt->mac[3], evt->mac[4], evt->mac[5]);
            break;
        }
        case EVT_CONFIG_CHANGED: {
            const evt_config_changed_t *evt = (const evt_config_changed_t *)data;
            snprintf(summary, summary_len, "Config: %s changed",
                     evt->key ? evt->key : "unknown");
            break;
        }
        case EVT_LIFECYCLE_CHANGED: {
            const evt_lifecycle_changed_t *evt = (const evt_lifecycle_changed_t *)data;
            snprintf(summary, summary_len, "Lifecycle: %d -> %d",
                     evt->old_state, evt->new_state);
            break;
        }
        case EVT_MEMORY_PRESSURE: {
            const evt_memory_pressure_t *evt = (const evt_memory_pressure_t *)data;
            snprintf(summary, summary_len, "Memory pressure: level=%d free=%zu",
                     evt->level, evt->free_heap);
            break;
        }
        default:
            snprintf(summary, summary_len, "%s", event_type_to_str(type));
            break;
    }
}

/**
 * @brief Add entry to ring buffer
 */
static void add_to_ring_buffer(const event_trace_entry_t *entry)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for ring buffer");
        return;
    }

    /* Check for overwrite */
    if (s_ring_count >= EVENT_TRACE_RING_BUFFER_SIZE) {
        s_stats.buffer_overwrites++;
    }

    /* Copy entry to ring buffer */
    memcpy(&s_ring_buffer[s_ring_write_idx], entry, sizeof(event_trace_entry_t));

    /* Advance write index with wrap-around */
    s_ring_write_idx = (s_ring_write_idx + 1) % EVENT_TRACE_RING_BUFFER_SIZE;

    /* Update count (max is buffer size) */
    if (s_ring_count < EVENT_TRACE_RING_BUFFER_SIZE) {
        s_ring_count++;
    }

    xSemaphoreGive(s_mutex);
}

/**
 * @brief Publish event to MQTT as JSON
 */
static void publish_to_mqtt(const event_trace_entry_t *entry)
{
    if (!mqtt_client_is_connected()) {
        s_stats.mqtt_publish_errors++;
        return;
    }

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        s_stats.mqtt_publish_errors++;
        return;
    }

    cJSON_AddNumberToObject(json, "timestamp_ms", entry->timestamp_ms);
    cJSON_AddStringToObject(json, "type", event_type_to_str(entry->type));
    cJSON_AddStringToObject(json, "summary", entry->summary);

    if (entry->device_id.valid) {
        cJSON *device = cJSON_CreateObject();
        if (device != NULL) {
            if (entry->device_id.ieee_addr != 0) {
                char ieee_str[24];
                snprintf(ieee_str, sizeof(ieee_str), "0x%016llX",
                         (unsigned long long)entry->device_id.ieee_addr);
                cJSON_AddStringToObject(device, "ieee_addr", ieee_str);
                cJSON_AddNumberToObject(device, "short_addr", entry->device_id.short_addr);
            }
            /* Check if MAC is non-zero */
            bool has_mac = false;
            for (int i = 0; i < 6; i++) {
                if (entry->device_id.mac[i] != 0) {
                    has_mac = true;
                    break;
                }
            }
            if (has_mac) {
                char mac_str[18];
                snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                         entry->device_id.mac[0], entry->device_id.mac[1],
                         entry->device_id.mac[2], entry->device_id.mac[3],
                         entry->device_id.mac[4], entry->device_id.mac[5]);
                cJSON_AddStringToObject(device, "mac", mac_str);
            }
            cJSON_AddItemToObject(json, "device", device);
        }
    }

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str == NULL) {
        s_stats.mqtt_publish_errors++;
        return;
    }

    /* Ownership transfer: strdup topic (constant), json_str (heap) transfers directly */
    char *topic_heap = strdup(EVENT_TRACE_MQTT_TOPIC);
    if (topic_heap == NULL) {
        free(json_str);
        s_stats.mqtt_publish_errors++;
        return;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_heap,
        .payload_json = json_str,
        .qos = 0,
        .retain = false
    };
    esp_err_t ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret == ESP_OK) {
        /* Ownership transferred to handler - do NOT free */
        s_stats.mqtt_publish_count++;
    } else {
        /* event_publish failed - ownership NOT transferred, free here */
        free(topic_heap);
        free(json_str);
        s_stats.mqtt_publish_errors++;
    }
}

/**
 * @brief Event handler callback for all event types
 */
static void event_trace_handler(event_type_t type, void *data, size_t data_size, void *user_ctx)
{
    (void)user_ctx;

    /* Check if tracing is enabled */
    if (!s_enabled) {
        return;
    }

    /* Check filter */
    if (!event_passes_filter(type)) {
        s_stats.filtered_events++;
        return;
    }

    /* Create trace entry */
    event_trace_entry_t entry;
    entry.timestamp_ms = get_timestamp_ms();
    entry.type = type;

    /* Extract device ID */
    extract_device_id(type, data, &entry.device_id);

    /* Generate summary */
    generate_summary(type, data, data_size, &entry.device_id,
                     entry.summary, sizeof(entry.summary));

    /* Update statistics */
    s_stats.total_events++;
    if (type < EVT_COUNT) {
        s_stats.events_per_type[type]++;
    }

    /* Log the event */
    ESP_LOGI(TAG, "[%lu ms] %s: %s",
             (unsigned long)entry.timestamp_ms,
             event_type_to_str(type),
             entry.summary);

    /* Add to ring buffer */
    add_to_ring_buffer(&entry);

    /* Publish to MQTT if enabled */
    if (s_mqtt_enabled) {
        publish_to_mqtt(&entry);
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t event_trace_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate ring buffer - try PSRAM first */
    s_ring_buffer = mem_ng_calloc(EVENT_TRACE_RING_BUFFER_SIZE, sizeof(event_trace_entry_t),
                                   MEM_CAP_PSRAM);
    if (s_ring_buffer == NULL) {
        s_ring_buffer = mem_ng_calloc(EVENT_TRACE_RING_BUFFER_SIZE, sizeof(event_trace_entry_t),
                                       MEM_CAP_DEFAULT);
    }
    if (s_ring_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer");
        return ESP_ERR_NO_MEM;
    }

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        mem_ng_free(s_ring_buffer);
        s_ring_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Initialize state */
    s_ring_write_idx = 0;
    s_ring_count = 0;
    s_enabled = true;
    s_mqtt_enabled = false;
    s_filter_mask = EVENT_TRACE_FILTER_ALL;
    memset(&s_stats, 0, sizeof(s_stats));

    /* Subscribe to all event types */
    esp_err_t ret;
    for (int i = 0; i < EVT_COUNT; i++) {
        ret = event_subscribe((event_type_t)i, event_trace_handler, NULL);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to subscribe to %s: %s",
                     event_type_to_str((event_type_t)i), esp_err_to_name(ret));
            /* Continue subscribing to other events */
        }
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Initialized (buffer_size=%d, subscribed to %d event types)",
             EVENT_TRACE_RING_BUFFER_SIZE, EVT_COUNT);

    return ESP_OK;
}

esp_err_t event_trace_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Unsubscribe from all event types */
    for (int i = 0; i < EVT_COUNT; i++) {
        event_unsubscribe((event_type_t)i, event_trace_handler);
    }

    /* Free ring buffer */
    if (s_ring_buffer != NULL) {
        mem_ng_free(s_ring_buffer);
        s_ring_buffer = NULL;
    }

    /* Delete mutex */
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_initialized = false;

    ESP_LOGI(TAG, "Deinitialized");

    return ESP_OK;
}

void event_trace_set_enabled(bool enabled)
{
    s_enabled = enabled;
    ESP_LOGI(TAG, "Tracing %s", enabled ? "enabled" : "disabled");
}

bool event_trace_is_enabled(void)
{
    return s_enabled;
}

void event_trace_set_mqtt_enabled(bool enabled)
{
    s_mqtt_enabled = enabled;
    ESP_LOGI(TAG, "MQTT publishing %s", enabled ? "enabled" : "disabled");
}

bool event_trace_is_mqtt_enabled(void)
{
    return s_mqtt_enabled;
}

void event_trace_set_filter(uint32_t event_type_mask)
{
    s_filter_mask = event_type_mask;
    ESP_LOGI(TAG, "Filter mask set to 0x%08lX", (unsigned long)event_type_mask);
}

uint32_t event_trace_get_filter(void)
{
    return s_filter_mask;
}

size_t event_trace_dump(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return 0;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for dump");
        return 0;
    }

    size_t count = s_ring_count;

    if (count == 0) {
        ESP_LOGI(TAG, "Ring buffer is empty");
        xSemaphoreGive(s_mutex);
        return 0;
    }

    ESP_LOGI(TAG, "=== Event Trace Dump (%zu events) ===", count);

    /* Calculate starting index for oldest entry */
    size_t start_idx;
    if (count < EVENT_TRACE_RING_BUFFER_SIZE) {
        start_idx = 0;
    } else {
        start_idx = s_ring_write_idx;  /* Write index points to oldest when full */
    }

    /* Dump events from oldest to newest */
    for (size_t i = 0; i < count; i++) {
        size_t idx = (start_idx + i) % EVENT_TRACE_RING_BUFFER_SIZE;
        const event_trace_entry_t *entry = &s_ring_buffer[idx];

        ESP_LOGI(TAG, "[%3zu] %10lu ms | %-20s | %s",
                 i + 1,
                 (unsigned long)entry->timestamp_ms,
                 event_type_to_str(entry->type),
                 entry->summary);
    }

    ESP_LOGI(TAG, "=== End Dump ===");

    xSemaphoreGive(s_mutex);

    return count;
}

esp_err_t event_trace_dump_mqtt(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    REQUIRE_MQTT_CONNECTED(ESP_ERR_INVALID_STATE);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for MQTT dump");
        return ESP_ERR_TIMEOUT;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    cJSON *events = cJSON_CreateArray();
    if (events == NULL) {
        cJSON_Delete(root);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(root, "events", events);
    cJSON_AddNumberToObject(root, "count", s_ring_count);
    cJSON_AddNumberToObject(root, "total_traced", s_stats.total_events);

    /* Calculate starting index */
    size_t start_idx;
    if (s_ring_count < EVENT_TRACE_RING_BUFFER_SIZE) {
        start_idx = 0;
    } else {
        start_idx = s_ring_write_idx;
    }

    /* Add events to array */
    for (size_t i = 0; i < s_ring_count; i++) {
        size_t idx = (start_idx + i) % EVENT_TRACE_RING_BUFFER_SIZE;
        const event_trace_entry_t *entry = &s_ring_buffer[idx];

        cJSON *event = cJSON_CreateObject();
        if (event != NULL) {
            cJSON_AddNumberToObject(event, "timestamp_ms", entry->timestamp_ms);
            cJSON_AddStringToObject(event, "type", event_type_to_str(entry->type));
            cJSON_AddStringToObject(event, "summary", entry->summary);

            if (entry->device_id.valid && entry->device_id.ieee_addr != 0) {
                char ieee_str[24];
                snprintf(ieee_str, sizeof(ieee_str), "0x%016llX",
                         (unsigned long long)entry->device_id.ieee_addr);
                cJSON_AddStringToObject(event, "ieee_addr", ieee_str);
            }

            cJSON_AddItemToArray(events, event);
        }
    }

    xSemaphoreGive(s_mutex);

    /* Publish via event bus */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Ownership transfer: strdup topic (constant), json_str (heap) transfers directly */
    char *topic_heap = strdup(EVENT_TRACE_MQTT_TOPIC "/dump");
    if (topic_heap == NULL) {
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_heap,
        .payload_json = json_str,
        .qos = 1,
        .retain = false
    };
    esp_err_t ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret == ESP_OK) {
        /* Ownership transferred to handler - do NOT free */
        ESP_LOGI(TAG, "Published event dump to MQTT");
    } else {
        /* event_publish failed - ownership NOT transferred, free here */
        free(topic_heap);
        free(json_str);
        ESP_LOGE(TAG, "Failed to publish event dump: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t event_trace_get_stats(event_trace_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(stats, &s_stats, sizeof(event_trace_stats_t));

    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t event_trace_clear(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Clear ring buffer */
    memset(s_ring_buffer, 0, EVENT_TRACE_RING_BUFFER_SIZE * sizeof(event_trace_entry_t));
    s_ring_write_idx = 0;
    s_ring_count = 0;

    /* Clear statistics */
    memset(&s_stats, 0, sizeof(s_stats));

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Ring buffer and statistics cleared");

    return ESP_OK;
}

esp_err_t event_trace_get_last(event_trace_entry_t *entry)
{
    if (entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_ring_count == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Last written entry is at (write_idx - 1) with wrap-around */
    size_t last_idx = (s_ring_write_idx + EVENT_TRACE_RING_BUFFER_SIZE - 1)
                      % EVENT_TRACE_RING_BUFFER_SIZE;
    memcpy(entry, &s_ring_buffer[last_idx], sizeof(event_trace_entry_t));

    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

size_t event_trace_get_count(void)
{
    if (!s_initialized) {
        return 0;
    }
    return s_ring_count;
}

bool event_trace_is_initialized(void)
{
    return s_initialized;
}
