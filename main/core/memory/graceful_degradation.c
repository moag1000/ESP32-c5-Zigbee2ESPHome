/**
 * @file graceful_degradation.c
 * @brief Graceful Degradation Implementation
 *
 * Implements automatic module unloading based on memory pressure levels
 * to maintain system stability under low memory conditions.
 *
 * @copyright Copyright (c) 2024-2026
 * @license Apache License 2.0
 */

#include "graceful_degradation.h"
#include "memory_pressure.h"
#include "module_manager.h"
#include "lifecycle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "GRACEFUL_DEG";

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Initialization flag */
static bool s_initialized = false;

/** @brief Current degradation state */
static bool s_is_degraded = false;

/** @brief Previous pressure level for tracking transitions */
static memory_pressure_t s_last_pressure = MEM_PRESSURE_NONE;

/** @brief Peak pressure level reached since init */
static memory_pressure_t s_peak_pressure = MEM_PRESSURE_NONE;

/** @brief Total modules unloaded count */
static size_t s_total_unload_count = 0;

/** @brief Timestamp when recovery started (for hysteresis) */
static int64_t s_recovery_start_time = 0;

/** @brief Flag indicating we're in recovery period */
static bool s_in_recovery = false;

/** @brief Mutex for thread safety */
static SemaphoreHandle_t s_mutex = NULL;

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static void pressure_callback(memory_pressure_t level, void *ctx);
static void handle_pressure_none(void);
static void handle_pressure_low(void);
static void handle_pressure_medium(void);
static void handle_pressure_high(void);
static void handle_pressure_critical(void);
static size_t unload_modules_up_to_priority(uint8_t max_priority);
static bool lock_acquire(void);
static void lock_release(void);
static int64_t get_time_ms(void);

/* ============================================================================
 * Private Functions
 * ============================================================================ */

/**
 * @brief Get current time in milliseconds
 */
static int64_t get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/**
 * @brief Acquire mutex lock
 */
static bool lock_acquire(void)
{
    if (s_mutex == NULL) {
        return false;
    }
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

/**
 * @brief Release mutex lock
 */
static void lock_release(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

/**
 * @brief Unload modules up to specified priority level
 *
 * Iterates through registered modules and unloads those with
 * priority <= max_priority, starting from lowest priority.
 *
 * @param max_priority Maximum priority to unload (inclusive)
 * @return Number of modules unloaded
 */
static size_t unload_modules_up_to_priority(uint8_t max_priority)
{
    size_t unloaded = 0;
    const char *names[MODULE_MAX_COUNT];

    /* Get list of unloadable modules sorted by priority */
    size_t count = module_find_unloadable(names, MODULE_MAX_COUNT);
    if (count == 0) {
        ESP_LOGD(TAG, "No modules available to unload");
        return 0;
    }

    /* Unload modules up to max_priority */
    for (size_t i = 0; i < count; i++) {
        const module_def_t *def = module_get_def(names[i]);
        if (def == NULL) {
            continue;
        }

        /* Check priority threshold */
        if (def->priority > max_priority) {
            ESP_LOGD(TAG, "Skipping %s (priority %u > max %u)",
                     names[i], def->priority, max_priority);
            continue;
        }

        /* Attempt unload */
        esp_err_t ret = module_unload(names[i]);
        if (ret == ESP_OK) {
            unloaded++;
            ESP_LOGI(TAG, "Unloaded module: %s (priority %u)", names[i], def->priority);
        } else {
            ESP_LOGW(TAG, "Failed to unload %s: 0x%x", names[i], ret);
        }
    }

    return unloaded;
}

/**
 * @brief Handle NONE pressure level
 *
 * System has recovered to normal memory levels.
 * After hysteresis period, transition back to RUNNING state if degraded.
 */
static void handle_pressure_none(void)
{
    if (!s_is_degraded) {
        /* Not degraded, nothing to do */
        s_in_recovery = false;
        return;
    }

    int64_t now = get_time_ms();

    if (!s_in_recovery) {
        /* Start recovery timer */
        s_recovery_start_time = now;
        s_in_recovery = true;
        ESP_LOGI(TAG, "Memory recovered, starting hysteresis period (%d ms)",
                 GRACEFUL_DEG_HYSTERESIS_MS);
        return;
    }

    /* Check if hysteresis period has elapsed */
    if ((now - s_recovery_start_time) < GRACEFUL_DEG_HYSTERESIS_MS) {
        ESP_LOGD(TAG, "Hysteresis: %lld ms remaining",
                 (long long)(GRACEFUL_DEG_HYSTERESIS_MS - (now - s_recovery_start_time)));
        return;
    }

    /* Hysteresis complete, recover from degradation */
    ESP_LOGI(TAG, "Hysteresis complete, recovering from degraded state");

    /* Transition back to RUNNING if in LOW_MEMORY state */
    lifecycle_state_t current_state = lifecycle_get_state();
    if (current_state == LIFECYCLE_LOW_MEMORY) {
        esp_err_t ret = lifecycle_transition(LIFECYCLE_RUNNING);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Transitioned to RUNNING state");
        } else {
            ESP_LOGW(TAG, "Failed to transition to RUNNING: 0x%x", ret);
        }
    }

    s_is_degraded = false;
    s_in_recovery = false;
}

/**
 * @brief Handle LOW pressure level
 *
 * Warning level - consider cleaning caches but no module unloading.
 * Same recovery logic as NONE if previously degraded.
 */
static void handle_pressure_low(void)
{
    /* Reset recovery timer if coming from higher pressure */
    if (s_last_pressure > MEM_PRESSURE_LOW && !s_in_recovery) {
        ESP_LOGI(TAG, "Pressure reduced to LOW, starting recovery check");
    }

    /* Use same recovery logic as NONE */
    handle_pressure_none();
}

/**
 * @brief Handle MEDIUM pressure level
 *
 * Unload priority 0-2 modules.
 */
static void handle_pressure_medium(void)
{
    /* Cancel any recovery in progress */
    s_in_recovery = false;
    s_is_degraded = true;

    ESP_LOGW(TAG, "MEDIUM pressure - unloading priority 0-%d modules",
             GRACEFUL_DEG_MEDIUM_MAX_PRIORITY);

    size_t unloaded = unload_modules_up_to_priority(GRACEFUL_DEG_MEDIUM_MAX_PRIORITY);
    s_total_unload_count += unloaded;

    if (unloaded > 0) {
        ESP_LOGI(TAG, "Unloaded %zu modules at MEDIUM pressure", unloaded);
    }
}

/**
 * @brief Handle HIGH pressure level
 *
 * Aggressively unload priority 0-4 modules and transition to LOW_MEMORY state.
 */
static void handle_pressure_high(void)
{
    /* Cancel any recovery in progress */
    s_in_recovery = false;
    s_is_degraded = true;

    ESP_LOGW(TAG, "HIGH pressure - unloading priority 0-%d modules",
             GRACEFUL_DEG_HIGH_MAX_PRIORITY);

    size_t unloaded = unload_modules_up_to_priority(GRACEFUL_DEG_HIGH_MAX_PRIORITY);
    s_total_unload_count += unloaded;

    if (unloaded > 0) {
        ESP_LOGI(TAG, "Unloaded %zu modules at HIGH pressure", unloaded);
    }

    /* Transition to LOW_MEMORY state */
    lifecycle_state_t current_state = lifecycle_get_state();
    if (current_state == LIFECYCLE_RUNNING) {
        esp_err_t ret = lifecycle_transition(LIFECYCLE_LOW_MEMORY);
        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "Transitioned to LOW_MEMORY state");
        } else {
            ESP_LOGE(TAG, "Failed to transition to LOW_MEMORY: 0x%x", ret);
        }
    }
}

/**
 * @brief Handle CRITICAL pressure level
 *
 * Emergency mode - use module_unload_by_priority to unload all non-essential
 * modules until memory threshold is met.
 */
static void handle_pressure_critical(void)
{
    /* Cancel any recovery in progress */
    s_in_recovery = false;
    s_is_degraded = true;

    ESP_LOGE(TAG, "CRITICAL pressure - emergency mode, unloading all non-essential modules");

    /* Use module_unload_by_priority for aggressive unloading */
    size_t unloaded = module_unload_by_priority();
    s_total_unload_count += unloaded;

    ESP_LOGE(TAG, "Emergency unload complete: %zu modules unloaded", unloaded);

    /* Ensure we're in LOW_MEMORY state */
    lifecycle_state_t current_state = lifecycle_get_state();
    if (current_state == LIFECYCLE_RUNNING) {
        esp_err_t ret = lifecycle_transition(LIFECYCLE_LOW_MEMORY);
        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "Transitioned to LOW_MEMORY state");
        } else {
            ESP_LOGE(TAG, "Failed to transition to LOW_MEMORY: 0x%x", ret);
        }
    }
}

/**
 * @brief Memory pressure callback
 *
 * Called by memory_pressure module when pressure level changes.
 *
 * @param level New memory pressure level
 * @param ctx User context (unused)
 */
static void pressure_callback(memory_pressure_t level, void *ctx)
{
    (void)ctx;  /* Unused */

    if (!s_initialized) {
        return;
    }

    if (!lock_acquire()) {
        ESP_LOGW(TAG, "Failed to acquire lock in pressure callback");
        return;
    }

    /* Track peak pressure */
    if (level > s_peak_pressure) {
        s_peak_pressure = level;
    }

    /* Log level change */
    if (level != s_last_pressure) {
        ESP_LOGI(TAG, "Memory pressure changed: %s -> %s",
                 memory_pressure_level_to_str(s_last_pressure),
                 memory_pressure_level_to_str(level));
    }

    /* Handle based on pressure level */
    switch (level) {
        case MEM_PRESSURE_NONE:
            handle_pressure_none();
            break;

        case MEM_PRESSURE_LOW:
            handle_pressure_low();
            break;

        case MEM_PRESSURE_MEDIUM:
            handle_pressure_medium();
            break;

        case MEM_PRESSURE_HIGH:
            handle_pressure_high();
            break;

        case MEM_PRESSURE_CRITICAL:
            handle_pressure_critical();
            break;

        default:
            ESP_LOGW(TAG, "Unknown pressure level: %d", level);
            break;
    }

    s_last_pressure = level;
    lock_release();
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t graceful_degradation_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize state */
    s_is_degraded = false;
    s_last_pressure = memory_pressure_get_level();
    s_peak_pressure = s_last_pressure;
    s_total_unload_count = 0;
    s_recovery_start_time = 0;
    s_in_recovery = false;

    /* Register callback with memory pressure monitor */
    esp_err_t ret = memory_pressure_register_callback(pressure_callback, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register pressure callback: 0x%x", ret);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized (current pressure: %s)",
             memory_pressure_level_to_str(s_last_pressure));

    return ESP_OK;
}

void graceful_degradation_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    if (!lock_acquire()) {
        ESP_LOGW(TAG, "Failed to acquire lock for deinit");
        return;
    }

    /* Reset state */
    s_is_degraded = false;
    s_last_pressure = MEM_PRESSURE_NONE;
    s_peak_pressure = MEM_PRESSURE_NONE;
    s_total_unload_count = 0;
    s_recovery_start_time = 0;
    s_in_recovery = false;
    s_initialized = false;

    lock_release();

    /* Delete mutex */
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    ESP_LOGI(TAG, "Deinitialized");
}

bool graceful_degradation_is_degraded(void)
{
    if (!s_initialized) {
        return false;
    }

    if (!lock_acquire()) {
        return false;
    }

    bool degraded = s_is_degraded;
    lock_release();
    return degraded;
}

memory_pressure_t graceful_degradation_get_peak_pressure(void)
{
    if (!s_initialized) {
        return MEM_PRESSURE_NONE;
    }

    if (!lock_acquire()) {
        return MEM_PRESSURE_NONE;
    }

    memory_pressure_t peak = s_peak_pressure;
    lock_release();
    return peak;
}

size_t graceful_degradation_get_unload_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    if (!lock_acquire()) {
        return 0;
    }

    size_t count = s_total_unload_count;
    lock_release();
    return count;
}

void graceful_degradation_check_now(void)
{
    if (!s_initialized) {
        return;
    }

    /* Get current pressure level and invoke callback */
    memory_pressure_t level = memory_pressure_get_level();
    pressure_callback(level, NULL);
}
