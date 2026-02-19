/**
 * @file memory_manager.c
 * @brief Memory Manager Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "memory_manager.h"
#include "gateway_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "core/gateway_timeouts.h"

static const char *TAG = "MEM_MGR";

/* Periodic logging configuration */
static bool s_periodic_logging_enabled = false;
static uint32_t s_logging_interval_sec = 60;
static esp_timer_handle_t s_logging_timer = NULL;

/* Low memory callback */
static memory_low_callback_t s_low_memory_callback = NULL;
static memory_status_t s_last_status = MEMORY_STATUS_NORMAL;

/* PSRAM availability flag */
static bool s_psram_available = false;

/**
 * @brief Periodic logging timer callback
 */
static void periodic_logging_timer_callback(void *arg)
{
    memory_manager_log_stats();

    /* Check status and trigger callback if status changed */
    memory_status_t current_status = memory_manager_get_status();
    if (s_low_memory_callback && current_status != s_last_status) {
        s_low_memory_callback(current_status);
        s_last_status = current_status;
    }
}

/**
 * @brief Initialize memory manager
 */
esp_err_t memory_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing memory manager...");

    /* Check for PSRAM availability */
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_size > 0) {
        s_psram_available = true;
        ESP_LOGI(TAG, "PSRAM detected: %u bytes (%.2f MB)",
                 psram_size, psram_size / (1024.0 * 1024.0));
    } else {
        s_psram_available = false;
        ESP_LOGI(TAG, "No PSRAM detected");
    }

    /* Log initial memory statistics */
    memory_manager_log_stats();

    /* Initialize status */
    s_last_status = memory_manager_get_status();

    ESP_LOGI(TAG, "Memory manager initialized successfully");
    return ESP_OK;
}

/**
 * @brief Get current memory statistics
 */
memory_stats_t memory_manager_get_stats(void)
{
    memory_stats_t stats;
    memset(&stats, 0, sizeof(memory_stats_t));

    /* Internal heap statistics */
    stats.total_heap = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    stats.free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    stats.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    stats.largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    /* PSRAM statistics if available */
    if (s_psram_available) {
        stats.total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        stats.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }

    /* Get heap info for allocation counts */
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_INTERNAL);
    stats.allocated_count = heap_info.allocated_blocks;
    stats.free_count = heap_info.free_blocks;

    return stats;
}

/**
 * @brief Check if free memory is above threshold
 */
bool memory_manager_check_threshold(size_t min_free)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    return (free_heap >= min_free);
}

/**
 * @brief Get current memory status
 */
memory_status_t memory_manager_get_status(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    if (free_heap < MEMORY_THRESHOLD_CRITICAL) {
        return MEMORY_STATUS_CRITICAL;
    } else if (free_heap < MEMORY_THRESHOLD_WARNING) {
        return MEMORY_STATUS_WARNING;
    } else {
        return MEMORY_STATUS_NORMAL;
    }
}

/**
 * @brief Log current memory statistics
 */
void memory_manager_log_stats(void)
{
    memory_stats_t stats = memory_manager_get_stats();
    memory_status_t status = memory_manager_get_status();
    uint8_t fragmentation = memory_manager_get_fragmentation();

    /* Status string */
    const char *status_str;
    switch (status) {
        case MEMORY_STATUS_NORMAL:
            status_str = "NORMAL";
            break;
        case MEMORY_STATUS_WARNING:
            status_str = "WARNING";
            break;
        case MEMORY_STATUS_CRITICAL:
            status_str = "CRITICAL";
            break;
        default:
            status_str = "UNKNOWN";
    }

    ESP_LOGI(TAG, "=== Memory Statistics ===");
    ESP_LOGI(TAG, "Status: %s", status_str);
    ESP_LOGI(TAG, "Internal Heap:");
    ESP_LOGI(TAG, "  Total: %u bytes (%.2f KB)",
             stats.total_heap, stats.total_heap / 1024.0);
    ESP_LOGI(TAG, "  Free: %u bytes (%.2f KB)",
             stats.free_heap, stats.free_heap / 1024.0);
    ESP_LOGI(TAG, "  Min Free: %u bytes (%.2f KB)",
             stats.min_free_heap, stats.min_free_heap / 1024.0);
    ESP_LOGI(TAG, "  Largest Block: %u bytes (%.2f KB)",
             stats.largest_free_block, stats.largest_free_block / 1024.0);
    ESP_LOGI(TAG, "  Usage: %.1f%%",
             100.0 * (stats.total_heap - stats.free_heap) / stats.total_heap);
    ESP_LOGI(TAG, "  Fragmentation: %u%%", fragmentation);
    ESP_LOGI(TAG, "  Blocks: %u allocated, %u free",
             stats.allocated_count, stats.free_count);

    if (s_psram_available) {
        ESP_LOGI(TAG, "PSRAM:");
        ESP_LOGI(TAG, "  Total: %u bytes (%.2f MB)",
                 stats.total_psram, stats.total_psram / (1024.0 * 1024.0));
        ESP_LOGI(TAG, "  Free: %u bytes (%.2f MB)",
                 stats.free_psram, stats.free_psram / (1024.0 * 1024.0));
        ESP_LOGI(TAG, "  Usage: %.1f%%",
                 100.0 * (stats.total_psram - stats.free_psram) / stats.total_psram);
    }

    /* Warning/Error logs for low memory */
    if (status == MEMORY_STATUS_CRITICAL) {
        ESP_LOGE(TAG, "CRITICAL: Free heap is critically low! (%u bytes)", stats.free_heap);
    } else if (status == MEMORY_STATUS_WARNING) {
        ESP_LOGW(TAG, "WARNING: Free heap is below warning threshold (%u bytes)", stats.free_heap);
    }
}

/**
 * @brief Trigger memory optimization
 */
esp_err_t memory_manager_optimize(void)
{
    ESP_LOGI(TAG, "Attempting memory optimization...");

    /* Log before optimization */
    memory_stats_t before = memory_manager_get_stats();

    /* FreeRTOS doesn't have explicit garbage collection, but we can:
     * 1. Yield to allow idle task to run (which may free resources)
     * 2. Trigger heap defragmentation by freeing/reallocating if needed
     */
    taskYIELD();
    vTaskDelay(pdMS_TO_TICKS(GW_TIMEOUT_MINIMAL_MS));

    /* Log after optimization */
    memory_stats_t after = memory_manager_get_stats();

    int32_t freed = (int32_t)after.free_heap - (int32_t)before.free_heap;
    if (freed > 0) {
        ESP_LOGI(TAG, "Optimization freed %ld bytes", freed);
        return ESP_OK;
    } else {
        ESP_LOGD(TAG, "No additional memory freed by optimization");
        return ESP_OK; /* Still success, just nothing to free */
    }
}

/**
 * @brief Get memory fragmentation percentage
 */
uint8_t memory_manager_get_fragmentation(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (free_heap == 0) {
        return GW_MEMORY_FRAGMENTATION_MAX_PERCENT; /* Completely fragmented */
    }

    /* Fragmentation = (1 - largest_block / free_heap) * 100 */
    uint8_t fragmentation = GW_PERCENTAGE_MAX - (GW_PERCENTAGE_MAX * largest_block / free_heap);
    return fragmentation;
}

/**
 * @brief Enable/disable periodic memory logging
 */
esp_err_t memory_manager_set_periodic_logging(bool enable, uint32_t interval_sec)
{
    if (enable) {
        if (s_logging_timer != NULL) {
            /* Timer already exists, restart with new interval */
            esp_timer_stop(s_logging_timer);
            esp_timer_delete(s_logging_timer);
            s_logging_timer = NULL;
        }

        /* Create timer */
        esp_timer_create_args_t timer_args = {
            .callback = periodic_logging_timer_callback,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "mem_log_timer"
        };

        esp_err_t ret = esp_timer_create(&timer_args, &s_logging_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create logging timer: %s", esp_err_to_name(ret));
            return ret;
        }

        /* Start timer */
        ret = esp_timer_start_periodic(s_logging_timer, interval_sec * GW_TIMER_US_PER_SECOND);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start logging timer: %s", esp_err_to_name(ret));
            esp_timer_delete(s_logging_timer);
            s_logging_timer = NULL;
            return ret;
        }

        s_periodic_logging_enabled = true;
        s_logging_interval_sec = interval_sec;
        ESP_LOGI(TAG, "Periodic memory logging enabled (interval: %lu sec)", interval_sec);

    } else {
        /* Disable logging */
        if (s_logging_timer != NULL) {
            esp_timer_stop(s_logging_timer);
            esp_timer_delete(s_logging_timer);
            s_logging_timer = NULL;
        }
        s_periodic_logging_enabled = false;
        ESP_LOGI(TAG, "Periodic memory logging disabled");
    }

    return ESP_OK;
}

/**
 * @brief Register low memory callback
 */
esp_err_t memory_manager_register_callback(memory_low_callback_t callback)
{
    s_low_memory_callback = callback;
    ESP_LOGI(TAG, "Low memory callback registered");
    return ESP_OK;
}

/**
 * @brief Get extended memory statistics
 */
memory_stats_extended_t memory_manager_get_stats_extended(void)
{
    memory_stats_extended_t stats;
    memset(&stats, 0, sizeof(memory_stats_extended_t));

    /* Internal heap statistics */
    stats.total_heap = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    stats.free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    stats.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    stats.largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    /* Calculate internal heap fragmentation */
    if (stats.free_heap > 0) {
        stats.fragmentation_percent = GW_PERCENTAGE_MAX -
            (GW_PERCENTAGE_MAX * stats.largest_free_block / stats.free_heap);
    } else {
        stats.fragmentation_percent = GW_MEMORY_FRAGMENTATION_MAX_PERCENT;
    }

    /* PSRAM statistics if available */
    stats.psram_available = s_psram_available;
    if (s_psram_available) {
        stats.total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        stats.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        stats.largest_psram_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        /* Calculate PSRAM fragmentation */
        if (stats.free_psram > 0) {
            stats.psram_fragmentation = GW_PERCENTAGE_MAX -
                (GW_PERCENTAGE_MAX * stats.largest_psram_block / stats.free_psram);
        } else {
            stats.psram_fragmentation = GW_MEMORY_FRAGMENTATION_MAX_PERCENT;
        }
    }

    /* Current status */
    stats.status = memory_manager_get_status();

    return stats;
}

/**
 * @brief Check if largest free block is below threshold
 */
bool memory_manager_check_fragmentation_threshold(size_t min_block_size)
{
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    return (largest_block >= min_block_size);
}

/**
 * @brief Trigger aggressive memory optimization
 */
esp_err_t memory_manager_optimize_aggressive(void)
{
    ESP_LOGI(TAG, "Attempting aggressive memory optimization...");

    /* Log before optimization */
    memory_stats_extended_t before = memory_manager_get_stats_extended();
    ESP_LOGI(TAG, "Before: free=%u, largest_block=%u, frag=%u%%",
             before.free_heap, before.largest_free_block, before.fragmentation_percent);

    /* Multiple yields to allow other tasks to release resources */
    for (int i = 0; i < 3; i++) {
        taskYIELD();
        vTaskDelay(pdMS_TO_TICKS(GW_TIMEOUT_MINIMAL_MS));
    }

    /* Extended delay for cleanup */
    vTaskDelay(pdMS_TO_TICKS(GW_TIMEOUT_SHORT_MS));

    /* Log after optimization */
    memory_stats_extended_t after = memory_manager_get_stats_extended();
    ESP_LOGI(TAG, "After: free=%u, largest_block=%u, frag=%u%%",
             after.free_heap, after.largest_free_block, after.fragmentation_percent);

    int32_t freed = (int32_t)after.free_heap - (int32_t)before.free_heap;
    int32_t defrag = (int32_t)after.largest_free_block - (int32_t)before.largest_free_block;

    if (freed > 0 || defrag > 0) {
        ESP_LOGI(TAG, "Optimization result: freed %ld bytes, defrag improvement %ld bytes",
                 freed, defrag);
    } else {
        ESP_LOGD(TAG, "No significant improvement from optimization");
    }

    return ESP_OK;
}
