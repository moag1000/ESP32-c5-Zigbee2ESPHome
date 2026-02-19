/**
 * @file mqtt_logger.h
 * @brief MQTT-based Logging System for ESP32-C5 Zigbee2MQTT Gateway
 *
 * Provides MQTT-based logging with:
 * - Configurable log levels (debug, info, warning, error)
 * - Namespace filtering (zigbee, mqtt, wifi, ble, system)
 * - Ring buffer for memory-safe log message queuing
 * - Integration with ESP_LOG system via esp_log_set_vprintf()
 * - MQTT commands for runtime configuration
 *
 * Topic: zigbee2mqtt/bridge/logging
 * Format: {"level": "info", "message": "...", "namespace": "zigbee"}
 *
 * Commands:
 * - bridge/request/logging/level: Set log level
 * - bridge/request/logging/namespaces: Configure namespace filtering
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MQTT_LOGGER_H
#define MQTT_LOGGER_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MQTT log levels
 *
 * Log levels from least to most verbose.
 * Setting a level enables that level and all less verbose levels.
 */
typedef enum {
    MQTT_LOG_ERROR = 0,     /**< Error conditions */
    MQTT_LOG_WARNING,       /**< Warning conditions */
    MQTT_LOG_INFO,          /**< Informational messages */
    MQTT_LOG_DEBUG          /**< Debug-level messages */
} mqtt_log_level_t;

/**
 * @brief Log namespace identifiers
 *
 * Used to categorize log messages by subsystem.
 */
typedef enum {
    MQTT_LOG_NS_SYSTEM = 0, /**< System/core messages */
    MQTT_LOG_NS_ZIGBEE,     /**< Zigbee stack messages */
    MQTT_LOG_NS_MQTT,       /**< MQTT client messages */
    MQTT_LOG_NS_WIFI,       /**< WiFi messages */
    MQTT_LOG_NS_BLE,        /**< Bluetooth messages */
    MQTT_LOG_NS_COUNT       /**< Total namespace count (internal use) */
} mqtt_log_namespace_t;

/**
 * @brief Log entry structure
 *
 * Contains all data for a single log entry.
 */
typedef struct {
    mqtt_log_level_t level;     /**< Log level */
    mqtt_log_namespace_t ns;    /**< Namespace identifier */
    char message[256];          /**< Log message text */
    uint32_t timestamp;         /**< Timestamp in milliseconds since boot */
} mqtt_log_entry_t;

/**
 * @brief MQTT logger configuration
 */
typedef struct {
    mqtt_log_level_t level;                     /**< Current log level threshold */
    bool namespaces_enabled[MQTT_LOG_NS_COUNT]; /**< Per-namespace enable flags */
    bool hook_esp_log;                          /**< Hook into ESP_LOG system */
    uint32_t ring_buffer_size;                  /**< Ring buffer size in entries */
} mqtt_logger_config_t;

/**
 * @brief Default configuration
 */
#define MQTT_LOGGER_CONFIG_DEFAULT { \
    .level = MQTT_LOG_INFO, \
    .namespaces_enabled = {true, true, true, true, true}, \
    .hook_esp_log = true, \
    .ring_buffer_size = 32 \
}

/**
 * @brief MQTT logger statistics
 */
typedef struct {
    uint32_t messages_logged;       /**< Total messages logged */
    uint32_t messages_published;    /**< Messages published to MQTT */
    uint32_t messages_dropped;      /**< Messages dropped (buffer full) */
    uint32_t messages_filtered;     /**< Messages filtered by level/namespace */
} mqtt_logger_stats_t;

/**
 * @brief Initialize MQTT logger
 *
 * Initializes the MQTT logging system with the specified log level.
 * Creates internal ring buffer and optionally hooks into ESP_LOG.
 *
 * @param[in] level Initial log level threshold
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_NO_MEM: Failed to allocate ring buffer
 *     - ESP_ERR_INVALID_STATE: Already initialized
 */
esp_err_t mqtt_logger_init(mqtt_log_level_t level);

/**
 * @brief Initialize MQTT logger with full configuration
 *
 * Initializes with detailed configuration options.
 *
 * @param[in] config Pointer to configuration structure
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid configuration
 *     - ESP_ERR_NO_MEM: Failed to allocate ring buffer
 */
esp_err_t mqtt_logger_init_config(const mqtt_logger_config_t *config);

/**
 * @brief Deinitialize MQTT logger
 *
 * Stops the logger, unhooks from ESP_LOG, and frees resources.
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_STATE: Not initialized
 */
esp_err_t mqtt_logger_deinit(void);

/**
 * @brief Start MQTT logger publishing task
 *
 * Starts the background task that publishes queued log messages to MQTT.
 * Should be called after MQTT client is connected.
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_STATE: Not initialized or already started
 *     - ESP_FAIL: Failed to create task
 */
esp_err_t mqtt_logger_start(void);

/**
 * @brief Stop MQTT logger publishing task
 *
 * Stops the background publishing task. Messages will still be queued
 * but not published until start is called again.
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_STATE: Not running
 */
esp_err_t mqtt_logger_stop(void);

/**
 * @brief Log a message with printf-style formatting
 *
 * Queues a log message for MQTT publishing. This function is non-blocking
 * and safe to call from any context (including ISR with limitations).
 *
 * @param[in] level Log level for this message
 * @param[in] ns Namespace for this message
 * @param[in] format Printf-style format string
 * @param[in] ... Format arguments
 * @return
 *     - ESP_OK: Message queued successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NO_MEM: Ring buffer full, message dropped
 *     - ESP_ERR_INVALID_STATE: Logger not initialized
 */
esp_err_t mqtt_logger_log(mqtt_log_level_t level, mqtt_log_namespace_t ns,
                          const char *format, ...) __attribute__((format(printf, 3, 4)));

/**
 * @brief Log a message with va_list arguments
 *
 * Variant of mqtt_logger_log that takes a va_list for use with
 * wrapper functions.
 *
 * @param[in] level Log level for this message
 * @param[in] ns Namespace for this message
 * @param[in] format Printf-style format string
 * @param[in] args Variable argument list
 * @return
 *     - ESP_OK: Message queued successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NO_MEM: Ring buffer full, message dropped
 */
esp_err_t mqtt_logger_logv(mqtt_log_level_t level, mqtt_log_namespace_t ns,
                           const char *format, va_list args);

/**
 * @brief Set log level threshold
 *
 * Sets the minimum log level for messages to be published.
 * Can be called at runtime to change verbosity.
 *
 * @param[in] level New log level threshold
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid level
 */
esp_err_t mqtt_logger_set_level(mqtt_log_level_t level);

/**
 * @brief Get current log level threshold
 *
 * @return Current log level threshold
 */
mqtt_log_level_t mqtt_logger_get_level(void);

/**
 * @brief Enable or disable a namespace
 *
 * Controls whether messages from a specific namespace are published.
 *
 * @param[in] ns Namespace to configure
 * @param[in] enable true to enable, false to disable
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid namespace
 */
esp_err_t mqtt_logger_enable_namespace(mqtt_log_namespace_t ns, bool enable);

/**
 * @brief Check if namespace is enabled
 *
 * @param[in] ns Namespace to check
 * @return true if enabled, false if disabled
 */
bool mqtt_logger_is_namespace_enabled(mqtt_log_namespace_t ns);

/**
 * @brief Get logger statistics
 *
 * @param[out] stats Pointer to statistics structure to fill
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid stats pointer
 */
esp_err_t mqtt_logger_get_stats(mqtt_logger_stats_t *stats);

/**
 * @brief Reset logger statistics
 *
 * @return ESP_OK always
 */
esp_err_t mqtt_logger_reset_stats(void);

/**
 * @brief Process logging configuration request
 *
 * Handles incoming MQTT requests for logging configuration:
 * - bridge/request/logging/level: {"level": "debug"}
 * - bridge/request/logging/namespaces: {"zigbee": true, "mqtt": false}
 *
 * @param[in] topic MQTT topic
 * @param[in] payload JSON payload
 * @param[in] len Payload length
 * @return
 *     - ESP_OK: Request processed successfully
 *     - ESP_ERR_INVALID_ARG: Invalid topic or payload
 */
esp_err_t mqtt_logger_process_request(const char *topic, const char *payload, size_t len);

/**
 * @brief Convert log level to string
 *
 * @param[in] level Log level
 * @return String representation ("debug", "info", "warning", "error")
 */
const char* mqtt_logger_level_to_str(mqtt_log_level_t level);

/**
 * @brief Convert string to log level
 *
 * @param[in] str Level string ("debug", "info", "warning", "error")
 * @return Log level, or MQTT_LOG_INFO if string not recognized
 */
mqtt_log_level_t mqtt_logger_str_to_level(const char *str);

/**
 * @brief Convert namespace to string
 *
 * @param[in] ns Namespace
 * @return String representation ("system", "zigbee", "mqtt", "wifi", "ble")
 */
const char* mqtt_logger_namespace_to_str(mqtt_log_namespace_t ns);

/**
 * @brief Convert string to namespace
 *
 * @param[in] str Namespace string
 * @return Namespace, or MQTT_LOG_NS_SYSTEM if string not recognized
 */
mqtt_log_namespace_t mqtt_logger_str_to_namespace(const char *str);

/**
 * @brief Convenience macros for logging
 *
 * These macros provide a simple interface for logging with automatic
 * namespace specification based on the calling module.
 */

/** @brief Log error message to system namespace */
#define MQTT_LOGE(format, ...) \
    mqtt_logger_log(MQTT_LOG_ERROR, MQTT_LOG_NS_SYSTEM, format, ##__VA_ARGS__)

/** @brief Log warning message to system namespace */
#define MQTT_LOGW(format, ...) \
    mqtt_logger_log(MQTT_LOG_WARNING, MQTT_LOG_NS_SYSTEM, format, ##__VA_ARGS__)

/** @brief Log info message to system namespace */
#define MQTT_LOGI(format, ...) \
    mqtt_logger_log(MQTT_LOG_INFO, MQTT_LOG_NS_SYSTEM, format, ##__VA_ARGS__)

/** @brief Log debug message to system namespace */
#define MQTT_LOGD(format, ...) \
    mqtt_logger_log(MQTT_LOG_DEBUG, MQTT_LOG_NS_SYSTEM, format, ##__VA_ARGS__)

/** @brief Log error message to Zigbee namespace */
#define MQTT_ZB_LOGE(format, ...) \
    mqtt_logger_log(MQTT_LOG_ERROR, MQTT_LOG_NS_ZIGBEE, format, ##__VA_ARGS__)

/** @brief Log warning message to Zigbee namespace */
#define MQTT_ZB_LOGW(format, ...) \
    mqtt_logger_log(MQTT_LOG_WARNING, MQTT_LOG_NS_ZIGBEE, format, ##__VA_ARGS__)

/** @brief Log info message to Zigbee namespace */
#define MQTT_ZB_LOGI(format, ...) \
    mqtt_logger_log(MQTT_LOG_INFO, MQTT_LOG_NS_ZIGBEE, format, ##__VA_ARGS__)

/** @brief Log debug message to Zigbee namespace */
#define MQTT_ZB_LOGD(format, ...) \
    mqtt_logger_log(MQTT_LOG_DEBUG, MQTT_LOG_NS_ZIGBEE, format, ##__VA_ARGS__)

/**
 * @brief Test MQTT logger functionality
 *
 * Performs basic logger functionality tests.
 * For debugging and verification purposes.
 *
 * @return
 *     - ESP_OK: Test passed
 *     - ESP_FAIL: Test failed
 */
esp_err_t mqtt_logger_test(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_LOGGER_H */
