/**
 * @file ble_adapter.h
 * @brief BLE Adapter - Bridges BLE Scanner to Event Bus and Device Registry
 *
 * This adapter provides a thin bridge layer between the existing BLE scanner
 * implementation and the new Phase 1 foundation (event_bus, device_registry,
 * unified_device).
 *
 * The adapter:
 *   - Hooks into existing BLE scanner callbacks without heavily modifying them
 *   - Translates BLE device events to event bus publications
 *   - Synchronizes BLE device state to the unified device registry
 *   - Maps BLE sensor data to device capabilities
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_ADAPTER_H
#define BLE_ADAPTER_H

#include "esp_err.h"
#include "core/device/unified_device.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/** @brief Minimum interval between state updates for same device (ms) */
#define BLE_ADAPTER_MIN_UPDATE_INTERVAL_MS      5000

/** @brief Device stale check interval (ms) */
#define BLE_ADAPTER_STALE_CHECK_INTERVAL_MS     60000

/* ============================================================================
 * Initialization / Lifecycle
 * ============================================================================ */

/**
 * @brief Initialize the BLE adapter
 *
 * Sets up internal state and prepares to register callbacks with the BLE scanner.
 * Call this after event_bus_init() and device_registry_init() but before
 * ble_adapter_start().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized or dependencies not ready
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - ESP_ERR_NOT_SUPPORTED if BLE is disabled in configuration
 */
esp_err_t ble_adapter_init(void);

/**
 * @brief Start the BLE adapter
 *
 * Registers callbacks with the BLE scanner and starts actively processing
 * BLE device advertisements. Also starts the BLE scanner if not already running.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized or already started
 *      - ESP_ERR_NOT_SUPPORTED if BLE is disabled in configuration
 */
esp_err_t ble_adapter_start(void);

/**
 * @brief Stop the BLE adapter
 *
 * Stops processing BLE events and unregisters callbacks from the scanner.
 * Optionally stops the BLE scanner as well. The adapter can be restarted
 * with ble_adapter_start().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not running
 *      - ESP_ERR_NOT_SUPPORTED if BLE is disabled in configuration
 */
esp_err_t ble_adapter_stop(void);

/**
 * @brief Deinitialize the BLE adapter
 *
 * Unregisters callbacks and frees all resources. The adapter must be
 * reinitialized to use again.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_NOT_SUPPORTED if BLE is disabled in configuration
 */
esp_err_t ble_adapter_deinit(void);

/**
 * @brief Check if the adapter is initialized
 *
 * @return true if initialized, false otherwise
 */
bool ble_adapter_is_initialized(void);

/**
 * @brief Check if the adapter is running
 *
 * @return true if started and processing events, false otherwise
 */
bool ble_adapter_is_running(void);

/* ============================================================================
 * Device Data Callback (for BLE device parsers)
 * ============================================================================ */

/**
 * @brief Parsed BLE sensor data structure
 *
 * Used to pass parsed data from BLE device parsers to the adapter.
 * All values are optional - check valid flags before use.
 */
typedef struct {
    /* Environmental sensors */
    bool has_temperature;       /**< Temperature value is valid */
    float temperature;          /**< Temperature in Celsius */

    bool has_humidity;          /**< Humidity value is valid */
    float humidity;             /**< Relative humidity percentage */

    bool has_pressure;          /**< Pressure value is valid */
    float pressure;             /**< Atmospheric pressure in hPa */

    /* Battery */
    bool has_battery;           /**< Battery value is valid */
    uint8_t battery;            /**< Battery level percentage (0-100) */

    /* Motion/presence */
    bool has_motion;            /**< Motion value is valid */
    bool motion;                /**< Motion detected */

    /* Device info */
    const char *device_type;    /**< Device type string (e.g., "xiaomi_lywsd03mmc") */
    const char *firmware;       /**< Firmware type if known (e.g., "ATC", "pvvx") */
} ble_adapter_parsed_data_t;

/**
 * @brief Callback for receiving parsed BLE device data
 *
 * Called by BLE device registry or parsers when advertisement data is parsed.
 * The adapter uses this to update the unified device registry and publish events.
 *
 * @param mac BLE MAC address (6 bytes)
 * @param name Device name (may be NULL)
 * @param rssi Signal strength (dBm)
 * @param data Parsed sensor data (may be NULL for presence-only updates)
 */
void ble_adapter_on_device_data(const uint8_t mac[6],
                                 const char *name,
                                 int8_t rssi,
                                 const ble_adapter_parsed_data_t *data);

/**
 * @brief Notify adapter of raw BLE advertisement
 *
 * Called by BLE scanner for every received advertisement. Used for
 * presence tracking when no parser is available for the device.
 *
 * @param mac BLE MAC address (6 bytes)
 * @param addr_type BLE address type (public/random)
 * @param rssi Signal strength (dBm)
 * @param adv_data Raw advertisement data
 * @param adv_len Advertisement data length
 */
void ble_adapter_on_raw_advertisement(const uint8_t mac[6],
                                       uint8_t addr_type,
                                       int8_t rssi,
                                       const uint8_t *adv_data,
                                       size_t adv_len);

/* ============================================================================
 * Device Management
 * ============================================================================ */

/**
 * @brief Manually prune stale BLE devices
 *
 * Removes devices that haven't been seen for the stale timeout period.
 * Publishes EVT_BLE_DEVICE_LOST for each removed device.
 *
 * @return Number of devices removed
 */
size_t ble_adapter_prune_stale_devices(void);

/**
 * @brief Get device ID from BLE MAC address
 *
 * Converts a 6-byte MAC address to device_id_t format.
 *
 * @param mac BLE MAC address (6 bytes)
 * @return Device ID
 */
device_id_t ble_adapter_mac_to_device_id(const uint8_t mac[6]);

/**
 * @brief Map BLE parsed data to device capabilities
 *
 * Analyzes parsed BLE data and returns a bitmask of device_capability_t flags.
 *
 * @param data Parsed BLE data
 * @return Capability bitmask
 */
uint32_t ble_adapter_get_capabilities(const ble_adapter_parsed_data_t *data);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * @brief BLE adapter statistics
 */
typedef struct {
    uint32_t devices_found;         /**< Total devices added to registry */
    uint32_t devices_lost;          /**< Total devices pruned as stale */
    uint32_t state_updates;         /**< Total state change events published */
    uint32_t advertisements;        /**< Total advertisements processed */
    uint32_t parse_errors;          /**< Total parse errors encountered */
} ble_adapter_stats_t;

/**
 * @brief Get adapter statistics
 *
 * @param stats Pointer to statistics structure to fill
 */
void ble_adapter_get_stats(ble_adapter_stats_t *stats);

/**
 * @brief Reset adapter statistics
 */
void ble_adapter_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_ADAPTER_H */
