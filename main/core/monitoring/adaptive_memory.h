/**
 * @file adaptive_memory.h
 * @brief Adaptive Memory Response Manager
 *
 * Provides automatic system adjustments based on memory pressure:
 * - WARNING: Reduces BLE scan frequency, extends intervals
 * - CRITICAL: Pauses HA discovery, triggers aggressive optimization
 * - NORMAL: Restores normal operation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ADAPTIVE_MEMORY_H
#define ADAPTIVE_MEMORY_H

#include "esp_err.h"
#include "memory_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Reduced BLE scan interval during memory warning (ms) */
#define ADAPTIVE_BLE_SCAN_WARNING_MS        4000

/** @brief Normal BLE scan interval (from config) */
#define ADAPTIVE_BLE_SCAN_NORMAL_MS         2000

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * @brief Initialize adaptive memory management
 *
 * Registers the memory status callback with memory_manager.
 * Should be called after memory_manager_init() and before
 * BLE and HA discovery initialization.
 *
 * @return ESP_OK on success
 */
esp_err_t adaptive_memory_init(void);

/**
 * @brief Handle memory status change
 *
 * Called by memory manager when status changes.
 * Adjusts system behavior based on memory pressure.
 *
 * @param[in] status New memory status
 */
void adaptive_memory_on_status_change(memory_status_t status);

/**
 * @brief Get current adaptive mode
 *
 * @return Current memory status that adaptive mode is responding to
 */
memory_status_t adaptive_memory_get_mode(void);

/**
 * @brief Force restore normal operation
 *
 * Manually restores normal operation regardless of memory status.
 * Useful for testing or override.
 *
 * @return ESP_OK on success
 */
esp_err_t adaptive_memory_restore_normal(void);

#ifdef __cplusplus
}
#endif

#endif /* ADAPTIVE_MEMORY_H */
