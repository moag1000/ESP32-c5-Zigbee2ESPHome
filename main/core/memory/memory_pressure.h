/**
 * @file memory_pressure.h
 * @brief Memory Pressure Monitoring for ESP32-C5 Zigbee2MQTT NG
 *
 * Provides periodic monitoring of heap memory and notifies registered
 * callbacks when memory pressure level changes. Used by the module manager
 * and other components to implement graceful degradation.
 *
 * Memory Pressure Levels:
 *   NONE     - >60KB free: Normal operation
 *   LOW      - 40-60KB free: Warning, consider GC caches
 *   MEDIUM   - 25-40KB free: Unload non-essential modules
 *   HIGH     - 15-25KB free: Aggressive unload, reduce buffers
 *   CRITICAL - <5KB free: Emergency mode, essential only
 *
 * @copyright Copyright (c) 2024-2025
 * @license Apache License 2.0
 */

#ifndef MEMORY_PRESSURE_H
#define MEMORY_PRESSURE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Memory Pressure Thresholds (bytes)
 *
 * Tuned for ESP32-C5 tri-radio (WiFi+Zigbee+BLE) which leaves only
 * ~10-15KB free internal RAM during normal operation. The BLE controller
 * alone consumes ~30KB internal RAM.
 * ============================================================================ */

/** @brief Threshold for MEM_PRESSURE_NONE (>20KB) */
#define MEM_PRESSURE_THRESHOLD_NONE     (20 * 1024)

/** @brief Threshold for MEM_PRESSURE_LOW (12-20KB) */
#define MEM_PRESSURE_THRESHOLD_LOW      (12 * 1024)

/** @brief Threshold for MEM_PRESSURE_MEDIUM (8-12KB) */
#define MEM_PRESSURE_THRESHOLD_MEDIUM   (8 * 1024)

/** @brief Threshold for MEM_PRESSURE_HIGH (5-8KB) */
#define MEM_PRESSURE_THRESHOLD_HIGH     (5 * 1024)

/** @brief Periodic check interval in microseconds (5 seconds) */
#define MEM_PRESSURE_CHECK_INTERVAL_US  (5 * 1000 * 1000)

/** @brief Maximum number of pressure callbacks */
#define MEM_PRESSURE_MAX_CALLBACKS      8

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * @brief Memory pressure levels
 *
 * Levels indicate increasing memory pressure:
 *   NONE     - Normal operation, ample free memory
 *   LOW      - Warning level, consider cleaning caches
 *   MEDIUM   - Unload non-essential modules (priority 0-2)
 *   HIGH     - Aggressive unload (priority 0-4), reduce buffers
 *   CRITICAL - Emergency mode, only essential modules
 */
typedef enum {
    MEM_PRESSURE_NONE,      /**< >20KB free - normal operation */
    MEM_PRESSURE_LOW,       /**< 12-20KB free - warning */
    MEM_PRESSURE_MEDIUM,    /**< 8-12KB free - unload non-essential */
    MEM_PRESSURE_HIGH,      /**< 5-8KB free - aggressive unload */
    MEM_PRESSURE_CRITICAL,  /**< <5KB free - emergency mode */
} memory_pressure_t;

/**
 * @brief Memory pressure callback function type
 *
 * Called when memory pressure level changes. Callbacks should be
 * quick and non-blocking. Heavy operations should be deferred.
 *
 * @param level New memory pressure level
 * @param ctx User context passed during registration
 */
typedef void (*pressure_callback_t)(memory_pressure_t level, void *ctx);

/* ============================================================================
 * Initialization & Control
 * ============================================================================ */

/**
 * @brief Initialize the memory pressure monitor
 *
 * Creates the periodic timer but does not start it.
 * Call memory_pressure_start() to begin monitoring.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if already initialized
 *     - ESP_ERR_NO_MEM if timer creation fails
 */
esp_err_t memory_pressure_init(void);

/**
 * @brief Start periodic memory pressure monitoring
 *
 * Starts the timer to check memory pressure every 5 seconds.
 * An initial check is performed immediately.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized or already running
 */
esp_err_t memory_pressure_start(void);

/**
 * @brief Stop periodic memory pressure monitoring
 *
 * Stops the periodic timer. Can be restarted with memory_pressure_start().
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized or not running
 */
esp_err_t memory_pressure_stop(void);

/* ============================================================================
 * Callback Registration
 * ============================================================================ */

/**
 * @brief Register a callback for memory pressure changes
 *
 * The callback will be called whenever the pressure level changes.
 * Maximum 8 callbacks can be registered.
 *
 * @param cb Callback function (must not be NULL)
 * @param ctx User context passed to callback
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if cb is NULL
 *     - ESP_ERR_NO_MEM if maximum callbacks reached
 */
esp_err_t memory_pressure_register_callback(pressure_callback_t cb, void *ctx);

/* ============================================================================
 * Query & Control
 * ============================================================================ */

/**
 * @brief Get current memory pressure level
 *
 * Returns the last calculated pressure level without performing
 * a new check. For an immediate check, use memory_pressure_check_now().
 *
 * @return Current memory pressure level
 */
memory_pressure_t memory_pressure_get_level(void);

/**
 * @brief Convert memory pressure level to string
 *
 * @param level Memory pressure level
 * @return String representation (e.g., "NONE", "LOW", "MEDIUM", etc.)
 */
const char *memory_pressure_level_to_str(memory_pressure_t level);

/**
 * @brief Perform an immediate memory pressure check
 *
 * Calculates the current pressure level and calls registered
 * callbacks if the level has changed since the last check.
 * This is also called periodically by the timer.
 */
void memory_pressure_check_now(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_PRESSURE_H */
