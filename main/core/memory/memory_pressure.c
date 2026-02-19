/**
 * @file memory_pressure.c
 * @brief Memory Pressure Monitoring Implementation for ESP32-C5 Zigbee2MQTT NG
 *
 * Implements periodic heap monitoring using esp_timer and notifies
 * registered callbacks when memory pressure level changes.
 *
 * @copyright Copyright (c) 2024-2025
 * @license Apache License 2.0
 */

#include "memory_pressure.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "MEM_PRESSURE";

/* ============================================================================
 * Internal Types
 * ============================================================================ */

/**
 * @brief Callback entry structure
 */
typedef struct {
    pressure_callback_t callback;
    void *ctx;
} pressure_callback_entry_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Whether module has been initialized */
static bool s_initialized = false;

/** @brief Whether monitoring is running */
static bool s_running = false;

/** @brief Current memory pressure level */
static memory_pressure_t s_current_level = MEM_PRESSURE_NONE;

/** @brief Periodic timer handle */
static esp_timer_handle_t s_timer_handle = NULL;

/** @brief Registered callbacks */
static pressure_callback_entry_t s_callbacks[MEM_PRESSURE_MAX_CALLBACKS];

/** @brief Number of registered callbacks */
static size_t s_callback_count = 0;

/** @brief Pressure level name strings */
static const char *s_level_names[] = {
    "NONE",
    "LOW",
    "MEDIUM",
    "HIGH",
    "CRITICAL",
};

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

/**
 * @brief Calculate memory pressure level from free heap
 *
 * @param free_heap Free internal heap in bytes
 * @return Calculated pressure level
 */
static memory_pressure_t calculate_pressure_level(size_t free_heap)
{
    if (free_heap > MEM_PRESSURE_THRESHOLD_NONE) {
        return MEM_PRESSURE_NONE;
    } else if (free_heap > MEM_PRESSURE_THRESHOLD_LOW) {
        return MEM_PRESSURE_LOW;
    } else if (free_heap > MEM_PRESSURE_THRESHOLD_MEDIUM) {
        return MEM_PRESSURE_MEDIUM;
    } else if (free_heap > MEM_PRESSURE_THRESHOLD_HIGH) {
        return MEM_PRESSURE_HIGH;
    } else {
        return MEM_PRESSURE_CRITICAL;
    }
}

/**
 * @brief Notify all registered callbacks of a level change
 *
 * @param level New pressure level
 */
static void notify_callbacks(memory_pressure_t level)
{
    for (size_t i = 0; i < s_callback_count; i++) {
        if (s_callbacks[i].callback != NULL) {
            s_callbacks[i].callback(level, s_callbacks[i].ctx);
        }
    }
}

/**
 * @brief Check memory pressure and notify if changed
 *
 * This function is called by the timer and by memory_pressure_check_now().
 */
static void check_memory_pressure(void)
{
    /* Get current free internal heap */
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    /* Calculate new pressure level */
    memory_pressure_t new_level = calculate_pressure_level(free_heap);

    /* Only notify if level changed */
    if (new_level != s_current_level) {
        memory_pressure_t old_level = s_current_level;
        s_current_level = new_level;

        /* Log the change - warning if pressure increased */
        if (new_level > old_level) {
            ESP_LOGW(TAG, "Pressure increased: %s -> %s (free: %u bytes)",
                     s_level_names[old_level],
                     s_level_names[new_level],
                     (unsigned int)free_heap);
        } else {
            ESP_LOGI(TAG, "Pressure decreased: %s -> %s (free: %u bytes)",
                     s_level_names[old_level],
                     s_level_names[new_level],
                     (unsigned int)free_heap);
        }

        /* Notify registered callbacks */
        notify_callbacks(new_level);

        /* Publish EVT_MEMORY_PRESSURE event */
        evt_memory_pressure_t evt = {
            .level = (int)new_level,
            .free_heap = free_heap,
        };
        event_publish(EVT_MEMORY_PRESSURE, &evt, sizeof(evt));
    }
}

/**
 * @brief Timer callback function
 *
 * Called periodically by esp_timer to check memory pressure.
 *
 * @param arg User argument (unused)
 */
static void timer_callback(void *arg)
{
    (void)arg;
    check_memory_pressure();
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t memory_pressure_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Initialize state */
    s_current_level = MEM_PRESSURE_NONE;
    s_callback_count = 0;
    s_running = false;
    memset(s_callbacks, 0, sizeof(s_callbacks));

    /* Create periodic timer */
    esp_timer_create_args_t timer_args = {
        .callback = timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mem_pressure",
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Calculate initial pressure level */
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_current_level = calculate_pressure_level(free_heap);

    s_initialized = true;

    ESP_LOGI(TAG, "Initialized, initial level: %s (free: %u bytes)",
             s_level_names[s_current_level],
             (unsigned int)free_heap);

    return ESP_OK;
}

esp_err_t memory_pressure_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_ERR_INVALID_STATE;
    }

    /* Start periodic timer */
    esp_err_t ret = esp_timer_start_periodic(s_timer_handle, MEM_PRESSURE_CHECK_INTERVAL_US);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = true;

    /* Perform an immediate check */
    check_memory_pressure();

    ESP_LOGI(TAG, "Started periodic monitoring (interval: %d ms)",
             MEM_PRESSURE_CHECK_INTERVAL_US / 1000);

    return ESP_OK;
}

esp_err_t memory_pressure_stop(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_running) {
        ESP_LOGW(TAG, "Not running");
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop periodic timer */
    esp_err_t ret = esp_timer_stop(s_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop timer: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = false;

    ESP_LOGI(TAG, "Stopped periodic monitoring");

    return ESP_OK;
}

esp_err_t memory_pressure_register_callback(pressure_callback_t cb, void *ctx)
{
    if (cb == NULL) {
        ESP_LOGE(TAG, "Callback is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_callback_count >= MEM_PRESSURE_MAX_CALLBACKS) {
        ESP_LOGE(TAG, "Maximum callbacks (%d) reached", MEM_PRESSURE_MAX_CALLBACKS);
        return ESP_ERR_NO_MEM;
    }

    s_callbacks[s_callback_count].callback = cb;
    s_callbacks[s_callback_count].ctx = ctx;
    s_callback_count++;

    ESP_LOGD(TAG, "Callback registered (%zu/%d)",
             s_callback_count, MEM_PRESSURE_MAX_CALLBACKS);

    return ESP_OK;
}

memory_pressure_t memory_pressure_get_level(void)
{
    return s_current_level;
}

const char *memory_pressure_level_to_str(memory_pressure_t level)
{
    if (level > MEM_PRESSURE_CRITICAL) {
        return "UNKNOWN";
    }
    return s_level_names[level];
}

void memory_pressure_check_now(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized, skipping check");
        return;
    }

    check_memory_pressure();
}
