/**
 * @file ble_battery_service.h
 * @brief BLE Battery Service (0x180F) Support
 *
 * Provides automatic battery level reading from BLE devices that
 * implement the standard Battery Service (UUID 0x180F) with
 * Battery Level characteristic (UUID 0x2A19).
 *
 * Features:
 * - Automatic battery reading on device connect
 * - Periodic polling with configurable interval
 * - MQTT publishing of battery levels
 * - Home Assistant discovery integration
 * - Low battery alerts
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_BATTERY_SERVICE_H
#define BLE_BATTERY_SERVICE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Battery Service UUID */
#define BLE_BATTERY_SERVICE_UUID            0x180F

/** @brief Battery Level Characteristic UUID */
#define BLE_BATTERY_LEVEL_CHAR_UUID         0x2A19

/** @brief Default battery poll interval in milliseconds (5 minutes) */
#define BLE_BATTERY_DEFAULT_POLL_INTERVAL_MS    300000

/** @brief Minimum poll interval in milliseconds (1 minute) */
#define BLE_BATTERY_MIN_POLL_INTERVAL_MS        60000

/** @brief Maximum poll interval in milliseconds (1 hour) */
#define BLE_BATTERY_MAX_POLL_INTERVAL_MS        3600000

/** @brief Low battery threshold percentage */
#define BLE_BATTERY_LOW_THRESHOLD           20

/** @brief Critical battery threshold percentage */
#define BLE_BATTERY_CRITICAL_THRESHOLD      10

/** @brief Maximum number of monitored devices */
#define BLE_BATTERY_MAX_MONITORED_DEVICES   16

/** @brief Battery read timeout in milliseconds */
#define BLE_BATTERY_READ_TIMEOUT_MS         5000

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief Battery level status enumeration
 */
typedef enum {
    BLE_BATTERY_STATUS_UNKNOWN = 0,     /**< Battery level unknown */
    BLE_BATTERY_STATUS_NORMAL,          /**< Battery level normal (>20%) */
    BLE_BATTERY_STATUS_LOW,             /**< Battery level low (10-20%) */
    BLE_BATTERY_STATUS_CRITICAL,        /**< Battery level critical (<10%) */
} ble_battery_status_t;

/**
 * @brief Battery service configuration
 */
typedef struct {
    uint32_t poll_interval_ms;          /**< Polling interval in milliseconds */
    bool auto_read_on_connect;          /**< Auto-read battery on device connect */
    bool publish_to_mqtt;               /**< Publish battery levels to MQTT */
    bool enable_ha_discovery;           /**< Enable Home Assistant discovery */
    uint8_t low_threshold;              /**< Low battery threshold percentage */
    uint8_t critical_threshold;         /**< Critical battery threshold percentage */
} ble_battery_config_t;

/**
 * @brief Default battery service configuration
 */
#define BLE_BATTERY_CONFIG_DEFAULT() { \
    .poll_interval_ms = BLE_BATTERY_DEFAULT_POLL_INTERVAL_MS, \
    .auto_read_on_connect = true, \
    .publish_to_mqtt = true, \
    .enable_ha_discovery = true, \
    .low_threshold = BLE_BATTERY_LOW_THRESHOLD, \
    .critical_threshold = BLE_BATTERY_CRITICAL_THRESHOLD, \
}

/**
 * @brief Battery level information
 */
typedef struct {
    uint8_t mac[6];                     /**< Device MAC address */
    uint8_t level;                      /**< Battery level 0-100% */
    ble_battery_status_t status;        /**< Battery status */
    uint32_t last_read;                 /**< Last read timestamp */
    bool supported;                     /**< Device supports battery service */
    bool monitoring;                    /**< Device is being monitored */
} ble_battery_info_t;

/**
 * @brief Battery level change callback
 *
 * @param mac Device MAC address
 * @param level Battery level 0-100%
 * @param status Battery status (normal, low, critical)
 */
typedef void (*ble_battery_cb_t)(const uint8_t *mac, uint8_t level, ble_battery_status_t status);

/* ============================================================================
 * Initialization and Configuration
 * ============================================================================ */

/**
 * @brief Initialize the BLE battery service
 *
 * Initializes the battery service module. The GATT client must be
 * initialized before calling this function.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized or GATT not ready
 *      - ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t ble_battery_service_init(void);

/**
 * @brief Initialize with custom configuration
 *
 * @param config Pointer to configuration structure
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config is NULL
 *      - ESP_ERR_INVALID_STATE if already initialized
 */
esp_err_t ble_battery_service_init_with_config(const ble_battery_config_t *config);

/**
 * @brief Deinitialize the battery service
 *
 * Stops all monitoring and releases resources.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ble_battery_service_deinit(void);

/**
 * @brief Check if battery service is initialized
 *
 * @return true if initialized
 */
bool ble_battery_service_is_initialized(void);

/**
 * @brief Update configuration
 *
 * @param config New configuration
 * @return ESP_OK on success
 */
esp_err_t ble_battery_service_set_config(const ble_battery_config_t *config);

/**
 * @brief Get current configuration
 *
 * @param config Pointer to configuration structure to fill
 * @return ESP_OK on success
 */
esp_err_t ble_battery_service_get_config(ble_battery_config_t *config);

/* ============================================================================
 * Battery Reading Operations
 * ============================================================================ */

/**
 * @brief Read battery level from a connected device
 *
 * Performs a synchronous battery level read from a connected device.
 * The device must have an active GATT connection.
 *
 * @param mac Device MAC address (6 bytes)
 * @param level Output: Battery level 0-100%
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if mac or level is NULL
 *      - ESP_ERR_NOT_FOUND if device not connected
 *      - ESP_ERR_NOT_SUPPORTED if device doesn't have battery service
 *      - ESP_ERR_TIMEOUT if read timed out
 */
esp_err_t ble_battery_read(const uint8_t *mac, uint8_t *level);

/**
 * @brief Read battery level by connection handle
 *
 * @param conn_handle GATT connection handle
 * @param level Output: Battery level 0-100%
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if level is NULL
 *      - ESP_ERR_NOT_FOUND if connection not found
 *      - ESP_ERR_NOT_SUPPORTED if no battery service
 */
esp_err_t ble_battery_read_by_handle(uint16_t conn_handle, uint8_t *level);

/**
 * @brief Check if device supports battery service
 *
 * Checks the discovered services for the Battery Service UUID.
 * Requires that service discovery has completed.
 *
 * @param mac Device MAC address
 * @return true if device has battery service
 */
bool ble_battery_device_supported(const uint8_t *mac);

/* ============================================================================
 * Monitoring Control
 * ============================================================================ */

/**
 * @brief Start periodic battery monitoring for a device
 *
 * Adds the device to the monitoring list and starts periodic
 * battery level polling at the configured interval.
 *
 * @param mac Device MAC address
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if mac is NULL
 *      - ESP_ERR_NO_MEM if max monitored devices reached
 *      - ESP_ERR_NOT_FOUND if device not connected
 */
esp_err_t ble_battery_monitor_start(const uint8_t *mac);

/**
 * @brief Stop battery monitoring for a device
 *
 * @param mac Device MAC address
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not monitored
 */
esp_err_t ble_battery_monitor_stop(const uint8_t *mac);

/**
 * @brief Stop all battery monitoring
 *
 * @return ESP_OK on success
 */
esp_err_t ble_battery_monitor_stop_all(void);

/**
 * @brief Check if device is being monitored
 *
 * @param mac Device MAC address
 * @return true if device is monitored
 */
bool ble_battery_is_monitored(const uint8_t *mac);

/**
 * @brief Get number of monitored devices
 *
 * @return Number of devices being monitored
 */
uint8_t ble_battery_get_monitored_count(void);

/* ============================================================================
 * Battery Information Retrieval
 * ============================================================================ */

/**
 * @brief Get cached battery information for a device
 *
 * Returns the last known battery level without performing a new read.
 *
 * @param mac Device MAC address
 * @param info Output: Battery information
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if no cached data for device
 */
esp_err_t ble_battery_get_info(const uint8_t *mac, ble_battery_info_t *info);

/**
 * @brief Get all monitored battery information
 *
 * @param info_array Array to fill with battery info
 * @param max_count Maximum entries to return
 * @return Number of entries returned
 */
uint8_t ble_battery_get_all_info(ble_battery_info_t *info_array, uint8_t max_count);

/**
 * @brief Get battery status from level
 *
 * @param level Battery level 0-100%
 * @return Battery status enumeration
 */
ble_battery_status_t ble_battery_get_status(uint8_t level);

/**
 * @brief Get battery status string
 *
 * @param status Battery status
 * @return Status string ("normal", "low", "critical", "unknown")
 */
const char *ble_battery_status_to_string(ble_battery_status_t status);

/* ============================================================================
 * Callback Registration
 * ============================================================================ */

/**
 * @brief Register battery level change callback
 *
 * The callback is invoked when a battery level is read and differs
 * from the last known value, or when battery status changes.
 *
 * @param callback Callback function (NULL to unregister)
 * @return ESP_OK on success
 */
esp_err_t ble_battery_register_callback(ble_battery_cb_t callback);

/* ============================================================================
 * MQTT Integration
 * ============================================================================ */

/**
 * @brief Publish battery level to MQTT
 *
 * Publishes battery information to:
 *   zigbee2mqtt/ble/<mac>/battery
 *
 * @param mac Device MAC address
 * @param level Battery level
 * @return ESP_OK on success
 */
esp_err_t ble_battery_publish_mqtt(const uint8_t *mac, uint8_t level);

/**
 * @brief Publish Home Assistant discovery for battery
 *
 * @param mac Device MAC address
 * @param device_name Device friendly name
 * @return ESP_OK on success
 */
esp_err_t ble_battery_publish_ha_discovery(const uint8_t *mac, const char *device_name);

#ifdef __cplusplus
}
#endif

#endif /* BLE_BATTERY_SERVICE_H */
