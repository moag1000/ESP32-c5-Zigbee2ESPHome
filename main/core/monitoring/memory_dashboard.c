/**
 * @file memory_dashboard.c
 * @brief Memory Dashboard Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "memory_dashboard.h"
#include "memory_manager_ng.h"
#include "memory_pressure.h"
#include "module_manager.h"
#include "device_registry.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "gateway_mqtt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "mem_dash";

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Timer handle for periodic publishing */
static esp_timer_handle_t s_timer_handle = NULL;

/** @brief Initialization state */
static bool s_initialized = false;

/** @brief Timer running state */
static bool s_running = false;

/** @brief Current publish interval in milliseconds */
static uint32_t s_interval_ms = MEM_DASHBOARD_DEFAULT_INTERVAL_MS;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void timer_callback(void *arg);
static void memory_pressure_event_handler(event_type_t type, void *data,
                                          size_t data_size, void *user_ctx);
static const char *pressure_level_to_string(memory_pressure_t level);
static cJSON *build_memory_json(void);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t memory_dashboard_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing memory dashboard...");

    /* Create periodic timer */
    esp_timer_create_args_t timer_args = {
        .callback = timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mem_dash_timer",
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Subscribe to memory pressure events for instant updates */
    ret = event_subscribe(EVT_MEMORY_PRESSURE, memory_pressure_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to memory pressure events: %s",
                 esp_err_to_name(ret));
        /* Non-fatal - continue without event-driven updates */
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Memory dashboard initialized");
    return ESP_OK;
}

esp_err_t memory_dashboard_start(uint32_t interval_ms)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        ESP_LOGW(TAG, "Already running, stopping first");
        memory_dashboard_stop();
    }

    /* Clamp interval to valid range */
    if (interval_ms < MEM_DASHBOARD_MIN_INTERVAL_MS) {
        ESP_LOGW(TAG, "Interval %lu ms too small, using minimum %d ms",
                 (unsigned long)interval_ms, MEM_DASHBOARD_MIN_INTERVAL_MS);
        interval_ms = MEM_DASHBOARD_MIN_INTERVAL_MS;
    } else if (interval_ms > MEM_DASHBOARD_MAX_INTERVAL_MS) {
        ESP_LOGW(TAG, "Interval %lu ms too large, using maximum %d ms",
                 (unsigned long)interval_ms, MEM_DASHBOARD_MAX_INTERVAL_MS);
        interval_ms = MEM_DASHBOARD_MAX_INTERVAL_MS;
    }

    s_interval_ms = interval_ms;

    /* Start periodic timer */
    esp_err_t ret = esp_timer_start_periodic(s_timer_handle, interval_ms * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = true;
    ESP_LOGI(TAG, "Memory dashboard started (interval: %lu ms)",
             (unsigned long)interval_ms);

    /* Publish initial stats immediately */
    memory_dashboard_publish_now();

    return ESP_OK;
}

esp_err_t memory_dashboard_stop(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_running) {
        ESP_LOGW(TAG, "Not running");
        return ESP_OK;
    }

    esp_err_t ret = esp_timer_stop(s_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop timer: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = false;
    ESP_LOGI(TAG, "Memory dashboard stopped");
    return ESP_OK;
}

esp_err_t memory_dashboard_publish_now(void)
{
    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected, skipping publish");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build JSON payload */
    cJSON *json = build_memory_json();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to build JSON payload");
        return ESP_ERR_NO_MEM;
    }

    /* Convert JSON to string */
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return ESP_ERR_NO_MEM;
    }

    /* Ownership transfer: strdup topic (constant), json_str (heap) transfers directly */
    char *topic_heap = strdup(MEM_DASHBOARD_TOPIC);
    if (topic_heap == NULL) {
        free(json_str);
        ESP_LOGE(TAG, "Failed to strdup topic");
        return ESP_ERR_NO_MEM;
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
        ESP_LOGD(TAG, "Published memory statistics");
    } else {
        /* event_publish failed - ownership NOT transferred, free here */
        free(topic_heap);
        free(json_str);
        ESP_LOGW(TAG, "Failed to publish memory stats: %s", esp_err_to_name(ret));
    }

    return ret;
}

bool memory_dashboard_is_initialized(void)
{
    return s_initialized;
}

bool memory_dashboard_is_running(void)
{
    return s_running;
}

uint32_t memory_dashboard_get_interval(void)
{
    return s_initialized ? s_interval_ms : 0;
}

esp_err_t memory_dashboard_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop if running */
    if (s_running) {
        memory_dashboard_stop();
    }

    /* Unsubscribe from events */
    event_unsubscribe(EVT_MEMORY_PRESSURE, memory_pressure_event_handler);

    /* Delete timer */
    if (s_timer_handle != NULL) {
        esp_timer_delete(s_timer_handle);
        s_timer_handle = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Memory dashboard deinitialized");
    return ESP_OK;
}

/* ============================================================================
 * Private Functions
 * ============================================================================ */

/**
 * @brief Timer callback for periodic publishing
 */
static void timer_callback(void *arg)
{
    (void)arg;
    memory_dashboard_publish_now();
}

/**
 * @brief Event handler for memory pressure changes
 *
 * Publishes memory statistics immediately when pressure level changes.
 */
static void memory_pressure_event_handler(event_type_t type, void *data,
                                          size_t data_size, void *user_ctx)
{
    (void)user_ctx;

    if (type != EVT_MEMORY_PRESSURE || data == NULL) {
        return;
    }

    evt_memory_pressure_t *pressure = (evt_memory_pressure_t *)data;
    ESP_LOGI(TAG, "Memory pressure changed to level %d, publishing stats",
             pressure->level);

    /* Publish updated stats immediately */
    memory_dashboard_publish_now();
}

/**
 * @brief Convert memory pressure level to string
 */
static const char *pressure_level_to_string(memory_pressure_t level)
{
    switch (level) {
        case MEM_PRESSURE_NONE:     return "none";
        case MEM_PRESSURE_LOW:      return "low";
        case MEM_PRESSURE_MEDIUM:   return "medium";
        case MEM_PRESSURE_HIGH:     return "high";
        case MEM_PRESSURE_CRITICAL: return "critical";
        default:                    return "unknown";
    }
}

/**
 * @brief Build JSON object with memory statistics
 *
 * Creates a cJSON object containing all memory statistics.
 * Caller must free the returned object with cJSON_Delete().
 *
 * @return cJSON object, or NULL on failure
 */
static cJSON *build_memory_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    /* Get memory statistics from memory_manager_ng */
    mem_stats_t stats;
    mem_get_stats(&stats);

    /* Get memory pressure level */
    memory_pressure_t pressure = memory_pressure_get_level();

    /* Basic heap statistics */
    cJSON_AddNumberToObject(root, "free_heap", stats.free_internal);
    cJSON_AddNumberToObject(root, "min_free_heap", stats.min_free_internal);
    cJSON_AddNumberToObject(root, "free_psram", stats.free_psram);
    cJSON_AddStringToObject(root, "pressure_level", pressure_level_to_string(pressure));
    cJSON_AddNumberToObject(root, "largest_block", stats.largest_free_block);

    /* Pool statistics */
    cJSON *pool_stats = cJSON_CreateObject();
    if (pool_stats != NULL) {
        /* Note: Buffer pools are typically named by the memory_manager_ng
         * implementation. We'll add placeholder pool stats that could be
         * populated if specific pools are exposed via the API.
         * For now, we show allocation statistics as a proxy. */
        cJSON *alloc_stats = cJSON_CreateObject();
        if (alloc_stats != NULL) {
            cJSON_AddNumberToObject(alloc_stats, "total", stats.alloc_count);
            cJSON_AddNumberToObject(alloc_stats, "freed", stats.free_count);
            cJSON_AddItemToObject(pool_stats, "allocations", alloc_stats);
        }

        cJSON *pool_perf = cJSON_CreateObject();
        if (pool_perf != NULL) {
            cJSON_AddNumberToObject(pool_perf, "hits", stats.pool_hits);
            cJSON_AddNumberToObject(pool_perf, "misses", stats.pool_misses);
            cJSON_AddItemToObject(pool_stats, "pool_perf", pool_perf);
        }

        cJSON_AddItemToObject(root, "pool_stats", pool_stats);
    }

    /* Module count from module_manager */
    size_t module_count = module_get_loaded_count();
    cJSON_AddNumberToObject(root, "module_count", module_count);

    /* Device count from device_registry */
    size_t device_count = device_registry_count();
    cJSON_AddNumberToObject(root, "device_count", device_count);

    /* Additional PSRAM statistics */
    if (stats.total_psram > 0) {
        cJSON_AddNumberToObject(root, "total_psram", stats.total_psram);
        cJSON_AddNumberToObject(root, "min_free_psram", stats.min_free_psram);
    }

    /* Total internal heap */
    cJSON_AddNumberToObject(root, "total_internal", stats.total_internal);

    return root;
}
