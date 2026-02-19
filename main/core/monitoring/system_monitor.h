/**
 * @file system_monitor.h
 * @brief System Monitor - Unified System Health Monitoring
 *
 * Integrates memory_manager and task_manager to provide comprehensive
 * system health monitoring. Includes watchdog feeding, uptime tracking,
 * and optional MQTT publishing of system statistics.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * System Monitor Constants
 * ============================================================================ */

/** @brief Monitoring task stack size (bytes) - needs extra for task_manager_log_stats() */
#define SYSMON_TASK_STACK_SIZE              4096  /* Reduced: uses buffer pools, not stack */

/** @brief Monitoring task priority */
#define SYSMON_TASK_PRIORITY                5

/** @brief Task stop timeout (ms) */
#define SYSMON_TASK_STOP_TIMEOUT_MS         5000

/** @brief Task stop polling interval (ms) */
#define SYSMON_TASK_STOP_POLL_MS            100

/** @brief Minimum monitoring interval (seconds) */
#define SYSMON_INTERVAL_MIN_SEC             10

/** @brief Maximum monitoring interval (seconds) */
#define SYSMON_INTERVAL_MAX_SEC             3600

/** @brief Default monitoring interval (seconds) */
#define SYSMON_INTERVAL_DEFAULT_SEC         60

/** @brief JSON buffer size for stats output */
#define SYSMON_JSON_BUFFER_SIZE             768

/** @brief System monitor buffer size */
#define SYSMON_BUFFER_SIZE                  256

/** @brief Version string length */
#define SYSMON_VERSION_STRING_LEN           32

/**
 * @brief System health status
 */
typedef enum {
    SYSTEM_HEALTH_GOOD = 0,      /**< All systems healthy */
    SYSTEM_HEALTH_WARNING,       /**< Minor issues detected */
    SYSTEM_HEALTH_CRITICAL       /**< Critical issues detected */
} system_health_t;

/**
 * @brief System statistics structure
 */
typedef struct {
    /* Uptime */
    uint64_t uptime_seconds;        /**< System uptime in seconds */

    /* Memory */
    size_t free_heap;               /**< Current free heap */
    size_t min_free_heap;           /**< Minimum free heap */
    size_t largest_free_block;      /**< Largest contiguous free block */
    size_t free_psram;              /**< Current free PSRAM */
    uint8_t heap_fragmentation;     /**< Heap fragmentation percentage */

    /* Tasks */
    size_t task_count;              /**< Number of running tasks */
    uint32_t cpu_usage;             /**< CPU usage percentage */
    bool stack_warning;             /**< Any task with high stack usage */

    /* Network */
    bool wifi_connected;            /**< WiFi connection status */
    bool mqtt_connected;            /**< MQTT connection status */
    int8_t wifi_rssi;               /**< WiFi RSSI in dBm */

    /* Health */
    system_health_t health;         /**< Overall system health */
} system_stats_t;

/**
 * @brief Initialize system monitor
 *
 * Initializes memory manager, task manager, and starts monitoring task.
 *
 * @param[in] enable_mqtt_publish Enable MQTT publishing of stats
 * @return ESP_OK on success
 */
esp_err_t system_monitor_init(bool enable_mqtt_publish);

/**
 * @brief Start system monitoring task
 *
 * Starts background task that periodically monitors system health.
 *
 * @param[in] interval_sec Monitoring interval in seconds
 * @return ESP_OK on success
 */
esp_err_t system_monitor_start(uint32_t interval_sec);

/**
 * @brief Stop system monitoring task
 *
 * @return ESP_OK on success
 */
esp_err_t system_monitor_stop(void);

/**
 * @brief Get current system statistics
 *
 * @return System statistics structure
 */
system_stats_t system_monitor_get_stats(void);

/**
 * @brief Get system uptime in seconds
 *
 * @return Uptime in seconds since boot
 */
uint64_t system_monitor_get_uptime(void);

/**
 * @brief Get overall system health status
 *
 * Evaluates memory, tasks, and connectivity to determine health.
 *
 * @return System health status
 */
system_health_t system_monitor_get_health(void);

/**
 * @brief Log all system statistics
 *
 * Outputs comprehensive system statistics to console.
 */
void system_monitor_log_all(void);

/**
 * @brief Feed watchdog timer
 *
 * Should be called periodically to prevent watchdog reset.
 * Automatically called by monitoring task.
 */
void system_monitor_feed_watchdog(void);

/**
 * @brief Trigger system health check
 *
 * Performs immediate health check and returns status.
 *
 * @return ESP_OK if healthy
 *         ESP_ERR_INVALID_STATE if critical issues detected
 */
esp_err_t system_monitor_health_check(void);

/**
 * @brief Publish system statistics to MQTT
 *
 * Publishes current system stats to zigbee2mqtt/bridge/system topic.
 * Only if MQTT publishing is enabled.
 *
 * @return ESP_OK on success
 */
esp_err_t system_monitor_publish_mqtt(void);

/**
 * @brief Enable/disable MQTT publishing
 *
 * @param[in] enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t system_monitor_set_mqtt_publishing(bool enable);

/**
 * @brief Set monitoring interval
 *
 * @param[in] interval_sec New interval in seconds
 * @return ESP_OK on success
 */
esp_err_t system_monitor_set_interval(uint32_t interval_sec);

/**
 * @brief Get formatted system stats as JSON
 *
 * @param[out] buffer Buffer to store JSON string
 * @param[in] buffer_size Size of buffer
 * @return ESP_OK on success
 */
esp_err_t system_monitor_get_json(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_MONITOR_H */
