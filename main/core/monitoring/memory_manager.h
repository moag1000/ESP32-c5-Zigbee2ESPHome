/**
 * @file memory_manager.h
 * @brief Memory Manager for ESP32-C5 Zigbee2MQTT Gateway
 *
 * Provides advanced memory monitoring and optimization for both
 * internal heap and PSRAM. Tracks memory usage, triggers warnings
 * on low memory conditions, and provides statistics for monitoring.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Memory statistics structure
 *
 * Contains detailed information about heap and PSRAM usage
 */
typedef struct {
    size_t total_heap;              /**< Total heap size in bytes */
    size_t free_heap;               /**< Current free heap in bytes */
    size_t min_free_heap;           /**< Minimum free heap since boot in bytes */
    size_t largest_free_block;      /**< Largest contiguous free block in bytes */
    size_t total_psram;             /**< Total PSRAM size in bytes */
    size_t free_psram;              /**< Current free PSRAM in bytes */
    size_t allocated_count;         /**< Number of allocated blocks (debug) */
    size_t free_count;              /**< Number of free blocks (debug) */
} memory_stats_t;

/**
 * @brief Memory threshold levels
 */
#define MEMORY_THRESHOLD_CRITICAL   (8 * 1024)    /**< Critical: 8KB (tri-radio minimum) */
#define MEMORY_THRESHOLD_WARNING    (15 * 1024)   /**< Warning: 15KB */
#define MEMORY_THRESHOLD_NORMAL     (30 * 1024)   /**< Normal: 30KB */

/**
 * @brief Memory status enumeration
 */
typedef enum {
    MEMORY_STATUS_NORMAL = 0,       /**< Memory usage is normal */
    MEMORY_STATUS_WARNING,          /**< Memory usage is high */
    MEMORY_STATUS_CRITICAL          /**< Memory usage is critical */
} memory_status_t;

/**
 * @brief Extended memory statistics with fragmentation details
 *
 * Provides additional information for advanced memory monitoring
 * including fragmentation analysis and PSRAM availability.
 */
typedef struct {
    size_t total_heap;              /**< Total heap size in bytes */
    size_t free_heap;               /**< Current free heap in bytes */
    size_t min_free_heap;           /**< Minimum free heap since boot in bytes */
    size_t largest_free_block;      /**< Largest contiguous free block in bytes */
    size_t total_psram;             /**< Total PSRAM size in bytes */
    size_t free_psram;              /**< Current free PSRAM in bytes */
    size_t largest_psram_block;     /**< Largest contiguous PSRAM block in bytes */
    uint8_t fragmentation_percent;  /**< Internal heap fragmentation 0-100% */
    uint8_t psram_fragmentation;    /**< PSRAM fragmentation 0-100% */
    bool psram_available;           /**< PSRAM availability flag */
    memory_status_t status;         /**< Current memory status */
} memory_stats_extended_t;

/**
 * @brief Initialize memory manager
 *
 * Sets up memory monitoring, initializes statistics tracking,
 * and configures PSRAM if available.
 *
 * @return ESP_OK on success
 *         ESP_FAIL on initialization failure
 */
esp_err_t memory_manager_init(void);

/**
 * @brief Get current memory statistics
 *
 * Retrieves detailed information about heap and PSRAM usage.
 *
 * @return memory_stats_t structure with current statistics
 */
memory_stats_t memory_manager_get_stats(void);

/**
 * @brief Check if free memory is above threshold
 *
 * @param[in] min_free Minimum free heap required in bytes
 * @return true if free heap >= min_free, false otherwise
 */
bool memory_manager_check_threshold(size_t min_free);

/**
 * @brief Get current memory status
 *
 * Evaluates current memory usage against thresholds.
 *
 * @return Memory status (NORMAL, WARNING, or CRITICAL)
 */
memory_status_t memory_manager_get_status(void);

/**
 * @brief Log current memory statistics
 *
 * Outputs detailed memory statistics to console via ESP_LOGI.
 * Includes heap, PSRAM, and fragmentation information.
 */
void memory_manager_log_stats(void);

/**
 * @brief Trigger memory optimization
 *
 * Attempts to optimize memory usage by:
 * - Triggering garbage collection if applicable
 * - Defragmenting heap if possible
 * - Clearing unused caches
 *
 * @return ESP_OK on success
 *         ESP_ERR_NOT_SUPPORTED if optimization not available
 */
esp_err_t memory_manager_optimize(void);

/**
 * @brief Get memory fragmentation percentage
 *
 * Calculates heap fragmentation as a percentage.
 * Higher values indicate more fragmentation.
 *
 * @return Fragmentation percentage (0-100)
 */
uint8_t memory_manager_get_fragmentation(void);

/**
 * @brief Enable/disable periodic memory logging
 *
 * @param[in] enable true to enable, false to disable
 * @param[in] interval_sec Logging interval in seconds (if enabled)
 * @return ESP_OK on success
 */
esp_err_t memory_manager_set_periodic_logging(bool enable, uint32_t interval_sec);

/**
 * @brief Register low memory callback
 *
 * Callback is invoked when free memory drops below warning threshold.
 *
 * @param[in] callback Function to call on low memory
 * @return ESP_OK on success
 */
typedef void (*memory_low_callback_t)(memory_status_t status);
esp_err_t memory_manager_register_callback(memory_low_callback_t callback);

/**
 * @brief Get extended memory statistics
 *
 * Retrieves detailed information including fragmentation analysis
 * and PSRAM statistics.
 *
 * @return memory_stats_extended_t structure with extended statistics
 */
memory_stats_extended_t memory_manager_get_stats_extended(void);

/**
 * @brief Check if largest free block is below threshold
 *
 * Useful for early fragmentation detection before allocations fail.
 *
 * @param[in] min_block_size Minimum required contiguous block size in bytes
 * @return true if largest free block >= min_block_size, false otherwise
 */
bool memory_manager_check_fragmentation_threshold(size_t min_block_size);

/**
 * @brief Trigger aggressive memory optimization
 *
 * Attempts more aggressive memory optimization:
 * - Multiple task yields to allow cleanup
 * - Extended delay to allow garbage collection
 * - Memory defragmentation attempt
 *
 * @return ESP_OK on success
 *         ESP_ERR_NOT_SUPPORTED if optimization not available
 */
esp_err_t memory_manager_optimize_aggressive(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_MANAGER_H */
