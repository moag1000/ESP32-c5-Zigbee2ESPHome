/**
 * @file memory_dashboard.h
 * @brief Memory Dashboard - MQTT Publishing of Memory Statistics
 *
 * Publishes comprehensive memory statistics to MQTT for monitoring.
 * Supports periodic publishing via esp_timer and instant updates on
 * memory pressure events.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MEMORY_DASHBOARD_H
#define MEMORY_DASHBOARD_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/** @brief MQTT topic for memory statistics */
#define MEM_DASHBOARD_TOPIC             "zigbee2mqtt/bridge/memory"

/** @brief Default publish interval in milliseconds (30 seconds) */
#define MEM_DASHBOARD_DEFAULT_INTERVAL_MS   (30 * 1000)

/** @brief Minimum publish interval in milliseconds (5 seconds) */
#define MEM_DASHBOARD_MIN_INTERVAL_MS       (5 * 1000)

/** @brief Maximum publish interval in milliseconds (10 minutes) */
#define MEM_DASHBOARD_MAX_INTERVAL_MS       (10 * 60 * 1000)

/** @brief JSON buffer size for memory stats payload */
#define MEM_DASHBOARD_JSON_BUFFER_SIZE      512

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * @brief Initialize the memory dashboard
 *
 * Creates the esp_timer for periodic publishing and subscribes to
 * memory pressure events. Does not start periodic publishing.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if already initialized
 *     - ESP_ERR_NO_MEM if timer creation fails
 */
esp_err_t memory_dashboard_init(void);

/**
 * @brief Start periodic memory statistics publishing
 *
 * Starts the esp_timer to periodically publish memory statistics
 * to the MQTT broker at the specified interval.
 *
 * @param[in] interval_ms Publish interval in milliseconds
 *                        (clamped to MIN/MAX range)
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized
 *     - ESP_ERR_INVALID_ARG if interval is invalid
 */
esp_err_t memory_dashboard_start(uint32_t interval_ms);

/**
 * @brief Stop periodic memory statistics publishing
 *
 * Stops the esp_timer. The dashboard remains initialized and can
 * be restarted with memory_dashboard_start().
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized or not running
 */
esp_err_t memory_dashboard_stop(void);

/**
 * @brief Publish memory statistics immediately
 *
 * Publishes current memory statistics to MQTT without waiting
 * for the periodic timer. Can be called regardless of whether
 * periodic publishing is active.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if MQTT is not connected
 *     - ESP_ERR_NO_MEM if JSON buffer allocation fails
 *     - ESP_FAIL if MQTT publish fails
 */
esp_err_t memory_dashboard_publish_now(void);

/**
 * @brief Check if memory dashboard is initialized
 *
 * @return true if initialized, false otherwise
 */
bool memory_dashboard_is_initialized(void);

/**
 * @brief Check if periodic publishing is active
 *
 * @return true if timer is running, false otherwise
 */
bool memory_dashboard_is_running(void);

/**
 * @brief Get the current publish interval
 *
 * @return Current interval in milliseconds, or 0 if not initialized
 */
uint32_t memory_dashboard_get_interval(void);

/**
 * @brief Deinitialize the memory dashboard
 *
 * Stops publishing, deletes the timer, and unsubscribes from events.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t memory_dashboard_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DASHBOARD_H */
