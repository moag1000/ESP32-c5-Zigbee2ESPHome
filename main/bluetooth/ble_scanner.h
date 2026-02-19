/**
 * @file ble_scanner.h
 * @brief BLE Passive Scanner API for ESP32-C5 Zigbee2MQTT Gateway
 *
 * This module provides passive BLE scanning functionality for detecting
 * and tracking BLE devices and their advertisements.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_SCANNER_H
#define BLE_SCANNER_H

#include "esp_err.h"
#include "ble_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BLE scanner state enumeration
 */
typedef enum {
    BLE_SCANNER_STATE_UNINITIALIZED = 0,    /**< Scanner not initialized */
    BLE_SCANNER_STATE_INITIALIZED,          /**< Scanner initialized but not running */
    BLE_SCANNER_STATE_STARTING,             /**< Scanner starting up */
    BLE_SCANNER_STATE_RUNNING,              /**< Scanner actively scanning */
    BLE_SCANNER_STATE_STOPPING,             /**< Scanner stopping */
    BLE_SCANNER_STATE_ERROR                 /**< Scanner in error state */
} ble_scanner_state_t;

/**
 * @brief Advertisement callback function type
 *
 * Called when a BLE advertisement is received. The callback should
 * process the advertisement quickly and return; do not perform
 * blocking operations in the callback.
 *
 * @param adv Pointer to advertisement data (valid only during callback)
 */
typedef void (*ble_adv_callback_t)(const ble_adv_data_t *adv);

/**
 * @brief Scanner state change callback function type
 *
 * Called when the BLE scanner state changes (e.g., STARTING → RUNNING).
 * Used by ESPHome BLE proxy to push state updates to Home Assistant.
 *
 * @param new_state The new scanner state
 * @param old_state The previous scanner state
 */
typedef void (*ble_scanner_state_callback_t)(ble_scanner_state_t new_state,
                                              ble_scanner_state_t old_state);

/**
 * @brief Extended advertisement callback function type (BLE 5.0)
 *
 * Called when an extended BLE advertisement with large payload is received.
 * The ext_adv_data pointer is only valid during the callback.
 *
 * @param ext_adv Pointer to extended advertisement data
 */
typedef void (*ble_ext_adv_callback_t)(const ble_ext_adv_data_t *ext_adv);

/**
 * @brief Scanner configuration for BLE 5.0 features
 */
typedef struct {
    bool enable_extended_scanning;       /**< Enable BLE 5.0 extended scanning */
    bool enable_coded_phy;               /**< Enable Coded PHY (long range) */
    bool enable_2m_phy;                  /**< Enable 2M PHY (high speed) */
    bool filter_duplicates;              /**< Filter duplicate advertisements */
    uint16_t scan_interval_ms;           /**< Scan interval in ms */
    uint16_t scan_window_ms;             /**< Scan window in ms */
} ble_scanner_config_t;

/**
 * @brief Default scanner configuration macro
 */
#define BLE_SCANNER_CONFIG_DEFAULT() { \
    .enable_extended_scanning = true, \
    .enable_coded_phy = false, \
    .enable_2m_phy = true, \
    .filter_duplicates = false, \
    .scan_interval_ms = BLE_DEFAULT_SCAN_INTERVAL_MS, \
    .scan_window_ms = BLE_DEFAULT_SCAN_WINDOW_MS, \
}

/**
 * @brief Initialize the BLE scanner module
 *
 * Prepares the scanner for operation. The BLE manager must be
 * initialized before calling this function.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if memory allocation fails
 * @return ESP_ERR_INVALID_STATE if already initialized
 * @return ESP_FAIL on other errors
 */
esp_err_t ble_scanner_init(void);

/**
 * @brief Deinitialize the BLE scanner module
 *
 * Stops scanning and releases all resources. Call this before
 * ble_manager_deinit() when shutting down.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ble_scanner_deinit(void);

/**
 * @brief Start passive BLE scanning
 *
 * Begins scanning for BLE advertisements. Advertisements are
 * delivered to registered callbacks.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized or already running
 * @return ESP_FAIL on NimBLE scanning error
 */
esp_err_t ble_scanner_start(void);

/**
 * @brief Stop BLE scanning
 *
 * Stops the active scan operation. Can be restarted with
 * ble_scanner_start().
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not running
 * @return ESP_FAIL on NimBLE error
 */
esp_err_t ble_scanner_stop(void);

/**
 * @brief Check if scanner is currently running
 *
 * @return true if scanner is in RUNNING state
 * @return false otherwise
 */
bool ble_scanner_is_running(void);

/**
 * @brief Get current scanner state
 *
 * @return Current scanner state
 */
ble_scanner_state_t ble_scanner_get_state(void);

/**
 * @brief Get scanner state as string
 *
 * @return String representation of current state
 */
const char *ble_scanner_get_state_str(void);

/**
 * @brief Set scan interval
 *
 * Sets the scan cycle interval. The scanner will scan for a window
 * period within each interval. Can be called while scanning is active.
 *
 * @param interval_ms Scan interval in milliseconds (100-10000)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if interval is out of range
 */
esp_err_t ble_scanner_set_interval(uint32_t interval_ms);

/**
 * @brief Get current scan interval
 *
 * @return Current scan interval in milliseconds
 */
uint32_t ble_scanner_get_interval(void);

/**
 * @brief Register advertisement callback
 *
 * Register a callback function to receive advertisement data.
 * Multiple callbacks can be registered (up to 8).
 *
 * @param callback Callback function pointer
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum callbacks reached
 * @return ESP_ERR_INVALID_ARG if callback is NULL
 */
esp_err_t ble_scanner_register_callback(ble_adv_callback_t callback);

/**
 * @brief Unregister advertisement callback
 *
 * Remove a previously registered callback.
 *
 * @param callback Callback function pointer to remove
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if callback not registered
 */
esp_err_t ble_scanner_unregister_callback(ble_adv_callback_t callback);

/**
 * @brief Register scanner state change callback
 *
 * Called whenever the scanner state changes. Only one state callback
 * is supported (used by ESPHome BLE proxy).
 *
 * @param callback State change callback function
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if callback is NULL
 */
esp_err_t ble_scanner_register_state_callback(ble_scanner_state_callback_t callback);

/**
 * @brief Get scanner statistics
 *
 * Retrieves current scanner statistics including advertisement
 * counts and scan cycle information.
 *
 * @param stats Pointer to statistics structure to fill
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if stats is NULL
 */
esp_err_t ble_scanner_get_stats(ble_scanner_stats_t *stats);

/**
 * @brief Reset scanner statistics
 *
 * Clears all accumulated statistics.
 */
void ble_scanner_reset_stats(void);

/**
 * @brief Get tracked device by index
 *
 * Retrieve information about a tracked device.
 *
 * @param index Device index (0 to BLE_MAX_TRACKED_DEVICES-1)
 * @param device Pointer to device structure to fill
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if device is NULL or index out of range
 * @return ESP_ERR_NOT_FOUND if slot is empty
 */
esp_err_t ble_scanner_get_tracked_device(size_t index, ble_tracked_device_t *device);

/**
 * @brief Get tracked device by MAC address
 *
 * Retrieve information about a tracked device by its MAC address.
 *
 * @param mac Device MAC address (6 bytes)
 * @param device Pointer to device structure to fill
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if mac or device is NULL
 * @return ESP_ERR_NOT_FOUND if device not found
 */
esp_err_t ble_scanner_get_device_by_mac(const uint8_t *mac, ble_tracked_device_t *device);

/**
 * @brief Get number of tracked devices
 *
 * @return Number of currently tracked devices
 */
size_t ble_scanner_get_device_count(void);

/**
 * @brief Clear all tracked devices
 *
 * Removes all devices from tracking. Statistics are not affected.
 */
void ble_scanner_clear_devices(void);

/**
 * @brief Remove stale devices
 *
 * Removes devices that have not been seen for the stale timeout period.
 *
 * @return Number of devices removed
 */
size_t ble_scanner_prune_stale_devices(void);

/**
 * @brief Self-test function for scanner
 *
 * Verifies that the scanner has been properly initialized.
 *
 * @return ESP_OK if all checks pass
 * @return ESP_FAIL if any check fails
 */
esp_err_t ble_scanner_self_test(void);

/**
 * @brief Enable or disable active scanning mode
 *
 * Active scanning sends scan request packets to obtain scan response data.
 * Note: Requires scanner restart to take effect if currently running.
 *
 * @param enable true to enable active scanning, false for passive
 */
void ble_scanner_set_active_mode(bool enable);

/**
 * @brief Check if active scanning is enabled
 *
 * @return true if active scanning mode is enabled
 * @return false if passive scanning mode is active (default)
 */
bool ble_scanner_is_active_enabled(void);

/* ============================================================================
 * BLE 5.0 Extended Scanning API
 * ============================================================================ */

/**
 * @brief Initialize scanner with BLE 5.0 configuration
 *
 * @param config Scanner configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t ble_scanner_init_with_config(const ble_scanner_config_t *config);

/**
 * @brief Check if extended scanning is enabled
 *
 * @return true if BLE 5.0 extended scanning is active
 */
bool ble_scanner_is_extended_enabled(void);

/**
 * @brief Enable or disable extended scanning
 *
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t ble_scanner_set_extended_mode(bool enable);

/**
 * @brief Set PHY preferences for scanning
 *
 * @param enable_1m Enable 1M PHY (required, always on)
 * @param enable_2m Enable 2M PHY
 * @param enable_coded Enable Coded PHY (long range)
 * @return ESP_OK on success
 */
esp_err_t ble_scanner_set_phy_config(bool enable_1m, bool enable_2m, bool enable_coded);

/**
 * @brief Register extended advertisement callback
 *
 * @param callback Callback for extended advertisements
 * @return ESP_OK on success
 */
esp_err_t ble_scanner_register_ext_callback(ble_ext_adv_callback_t callback);

/**
 * @brief Unregister extended advertisement callback
 *
 * @param callback Callback to remove
 * @return ESP_OK on success
 */
esp_err_t ble_scanner_unregister_ext_callback(ble_ext_adv_callback_t callback);

/**
 * @brief Get BLE version capabilities string
 *
 * @return String like "BLE 4.2 + 5.0 Extended"
 */
const char *ble_scanner_get_ble_version_str(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_SCANNER_H */
