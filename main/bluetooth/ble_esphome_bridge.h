/**
 * @file ble_esphome_bridge.h
 * @brief BLE to ESPHome Bridge for Home Assistant Integration
 *
 * Bridges BLE device data from the device registry to ESPHome entities,
 * enabling automatic discovery and state updates in Home Assistant.
 *
 * Supported BLE device types:
 * - Temperature/Humidity sensors (Xiaomi, Govee) -> ESPHome Sensor entities
 * - Beacons (iBeacon, Eddystone) -> ESPHome Binary Sensor entities (presence)
 * - Generic devices -> RSSI/battery sensors
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_ESPHOME_BRIDGE_H
#define BLE_ESPHOME_BRIDGE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** @brief Maximum BLE devices to bridge */
#ifndef BLE_ESPHOME_MAX_DEVICES
#define BLE_ESPHOME_MAX_DEVICES     30
#endif

/** @brief Entity key base for BLE devices (offset from other entities) */
#define BLE_ESPHOME_ENTITY_KEY_BASE    0x1000

/** @brief Presence timeout for beacons in seconds */
#define BLE_ESPHOME_PRESENCE_TIMEOUT_S  300

/* ============================================================================
 * Entity Key Offsets
 * Per-device entity key calculation:
 *   base = BLE_ESPHOME_ENTITY_KEY_BASE + (device_index * 10)
 *   temperature = base + 0
 *   humidity    = base + 1
 *   battery     = base + 2
 *   rssi        = base + 3
 *   presence    = base + 4
 * ============================================================================ */

#define BLE_ENTITY_OFFSET_TEMPERATURE   0
#define BLE_ENTITY_OFFSET_HUMIDITY      1
#define BLE_ENTITY_OFFSET_BATTERY       2
#define BLE_ENTITY_OFFSET_RSSI          3
#define BLE_ENTITY_OFFSET_PRESENCE      4
#define BLE_ENTITY_SLOTS_PER_DEVICE     10

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief BLE bridge device registration status
 */
typedef enum {
    BLE_BRIDGE_STATUS_UNREGISTERED = 0,
    BLE_BRIDGE_STATUS_REGISTERED,
    BLE_BRIDGE_STATUS_ACTIVE,
    BLE_BRIDGE_STATUS_STALE
} ble_bridge_status_t;

/**
 * @brief BLE bridge configuration
 */
typedef struct {
    bool auto_register;              /**< Auto-register new BLE devices */
    bool register_sensors;           /**< Register temperature/humidity sensors */
    bool register_beacons;           /**< Register beacon presence sensors */
    bool register_battery;           /**< Register battery level sensors */
    bool register_rssi;              /**< Register RSSI sensors */
    uint32_t presence_timeout_s;     /**< Beacon presence timeout */
    uint32_t update_interval_ms;     /**< Minimum update interval */
} ble_esphome_bridge_config_t;

/**
 * @brief Default bridge configuration
 */
#define BLE_ESPHOME_BRIDGE_CONFIG_DEFAULT() { \
    .auto_register = true, \
    .register_sensors = true, \
    .register_beacons = true, \
    .register_battery = true, \
    .register_rssi = false, \
    .presence_timeout_s = BLE_ESPHOME_PRESENCE_TIMEOUT_S, \
    .update_interval_ms = 1000 \
}

/**
 * @brief Bridged device information
 */
typedef struct {
    uint8_t mac[6];                  /**< Device MAC address */
    char name[32];                   /**< Device display name */
    ble_bridge_status_t status;      /**< Registration status */
    uint32_t entity_key_base;        /**< Base entity key for this device */
    bool has_temperature;            /**< Device reports temperature */
    bool has_humidity;               /**< Device reports humidity */
    bool has_battery;                /**< Device reports battery */
    bool is_beacon;                  /**< Device is a beacon */
    uint32_t last_update;            /**< Last update timestamp */
} ble_bridged_device_t;

/**
 * @brief Bridge statistics
 */
typedef struct {
    uint32_t devices_registered;     /**< Number of registered devices */
    uint32_t entities_created;       /**< Number of entities created */
    uint32_t state_updates;          /**< Total state updates sent */
    uint32_t presence_changes;       /**< Presence state changes */
} ble_esphome_bridge_stats_t;

/* ============================================================================
 * Initialization and Control
 * ============================================================================ */

/**
 * @brief Initialize BLE-ESPHome bridge
 *
 * Initializes the bridge module. Both BLE device registry and ESPHome
 * entity manager must be initialized before calling this function.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if dependencies not initialized
 *      - ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t ble_esphome_bridge_init(void);

/**
 * @brief Initialize with custom configuration
 *
 * @param[in] config Bridge configuration
 * @return ESP_OK on success
 */
esp_err_t ble_esphome_bridge_init_with_config(const ble_esphome_bridge_config_t *config);

/**
 * @brief Deinitialize BLE-ESPHome bridge
 *
 * @return ESP_OK on success
 */
esp_err_t ble_esphome_bridge_deinit(void);

/**
 * @brief Check if bridge is initialized
 *
 * @return true if initialized
 */
bool ble_esphome_bridge_is_initialized(void);

/**
 * @brief Start the bridge
 *
 * Registers callback with BLE device registry and begins
 * forwarding device updates to ESPHome entities.
 *
 * @return ESP_OK on success
 */
esp_err_t ble_esphome_bridge_start(void);

/**
 * @brief Stop the bridge
 *
 * Stops forwarding device updates.
 *
 * @return ESP_OK on success
 */
esp_err_t ble_esphome_bridge_stop(void);

/* ============================================================================
 * Device Registration
 * ============================================================================ */

/**
 * @brief Manually register a BLE device with ESPHome
 *
 * Creates ESPHome entities for the specified BLE device.
 *
 * @param[in] mac Device MAC address
 * @param[in] name Display name for the device
 * @return ESP_OK on success
 */
esp_err_t ble_esphome_bridge_register_device(const uint8_t mac[6], const char *name);

/**
 * @brief Unregister a BLE device from ESPHome
 *
 * Marks device entities as unavailable.
 *
 * @param[in] mac Device MAC address
 * @return ESP_OK on success
 */
esp_err_t ble_esphome_bridge_unregister_device(const uint8_t mac[6]);

/**
 * @brief Check if a device is registered with the bridge
 *
 * @param[in] mac Device MAC address
 * @return true if registered
 */
bool ble_esphome_bridge_is_device_registered(const uint8_t mac[6]);

/**
 * @brief Get bridged device information
 *
 * @param[in] mac Device MAC address
 * @param[out] device Device information
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not registered
 */
esp_err_t ble_esphome_bridge_get_device(const uint8_t mac[6], ble_bridged_device_t *device);

/**
 * @brief Get all bridged devices
 *
 * @param[out] devices Array to fill
 * @param[in] max_count Maximum devices to return
 * @return Number of devices returned
 */
size_t ble_esphome_bridge_get_all_devices(ble_bridged_device_t *devices, size_t max_count);

/* ============================================================================
 * State Updates
 * ============================================================================ */

/**
 * @brief Update sensor values for a device
 *
 * @param[in] mac Device MAC address
 * @param[in] temperature Temperature in Celsius (NAN to skip)
 * @param[in] humidity Humidity in percent (NAN to skip)
 * @param[in] battery Battery level 0-100 (-1 to skip)
 * @param[in] rssi Signal strength (-127 to skip)
 * @return ESP_OK on success
 */
esp_err_t ble_esphome_bridge_update_sensor(const uint8_t mac[6],
                                            float temperature,
                                            float humidity,
                                            int8_t battery,
                                            int8_t rssi);

/**
 * @brief Update beacon presence state
 *
 * @param[in] mac Beacon MAC address
 * @param[in] present true if beacon is present
 * @param[in] rssi Current RSSI value
 * @return ESP_OK on success
 */
esp_err_t ble_esphome_bridge_update_beacon(const uint8_t mac[6],
                                            bool present,
                                            int8_t rssi);

/**
 * @brief Process presence timeout check
 *
 * Called periodically to mark beacons as absent if not seen recently.
 *
 * @return Number of devices marked as absent
 */
size_t ble_esphome_bridge_process_presence_timeout(void);

/* ============================================================================
 * Statistics and Debug
 * ============================================================================ */

/**
 * @brief Get bridge statistics
 *
 * @param[out] stats Statistics structure
 * @return ESP_OK on success
 */
esp_err_t ble_esphome_bridge_get_stats(ble_esphome_bridge_stats_t *stats);

/**
 * @brief Reset bridge statistics
 *
 * @return ESP_OK on success
 */
esp_err_t ble_esphome_bridge_reset_stats(void);

/**
 * @brief Self-test for bridge module
 *
 * @return ESP_OK if all tests pass
 */
esp_err_t ble_esphome_bridge_self_test(void);

/**
 * @brief Log current bridge status
 */
void ble_esphome_bridge_log_status(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_ESPHOME_BRIDGE_H */
