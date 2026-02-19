/**
 * @file ble_device_registry.h
 * @brief BLE Device Registry and Parser Dispatch
 *
 * Central registry for BLE devices with automatic parser dispatch.
 * Supports device caching with configurable timeout and tracking
 * up to BLE_MAX_TRACKED_DEVICES devices.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_DEVICE_REGISTRY_H
#define BLE_DEVICE_REGISTRY_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/** @brief Maximum number of tracked BLE devices (registry-specific)
 *
 * Wired to CONFIG_BT_MAX_TRACKED_DEVICES from Kconfig (menuconfig).
 */
#ifndef BLE_REGISTRY_MAX_TRACKED_DEVICES
#ifdef CONFIG_BT_MAX_TRACKED_DEVICES
#define BLE_REGISTRY_MAX_TRACKED_DEVICES     CONFIG_BT_MAX_TRACKED_DEVICES
#else
#define BLE_REGISTRY_MAX_TRACKED_DEVICES     500
#endif
#endif

/** @brief Default device cache timeout in seconds (5 minutes) */
#define BLE_DEVICE_CACHE_TIMEOUT_S  300

/** @brief Maximum advertisement data length for registry */
#define BLE_REGISTRY_MAX_ADV_DATA_LEN        62

/** @brief Maximum friendly name length for registry */
#define BLE_REGISTRY_DEVICE_NAME_MAX_LEN     32

/* ============================================================================
 * Type Definitions
 *
 * Note: These registry-specific types extend the generic types in ble_common.h
 * with specific device model support for the device registry module.
 * ============================================================================ */

/**
 * @brief BLE registry device type enumeration
 *
 * Specific device model types used by the device registry for parser dispatch.
 * These are more specific than the generic types in ble_common.h.
 */
typedef enum {
    BLE_REG_DEVICE_TYPE_UNKNOWN = 0,        /**< Unknown device type */
    BLE_REG_DEVICE_TYPE_XIAOMI_LYWSD03MMC,  /**< Xiaomi LYWSD03MMC (ATC/pvvx firmware) */
    BLE_REG_DEVICE_TYPE_GOVEE_H5075,        /**< Govee H5075 thermometer */
    BLE_REG_DEVICE_TYPE_IBEACON,            /**< Apple iBeacon */
    BLE_REG_DEVICE_TYPE_EDDYSTONE_UID,      /**< Eddystone-UID beacon */
    BLE_REG_DEVICE_TYPE_EDDYSTONE_TLM,      /**< Eddystone-TLM beacon */
    BLE_REG_DEVICE_TYPE_RUUVI_TAG,          /**< Ruuvi Tag environmental sensor */
    BLE_REG_DEVICE_TYPE_QINGPING,           /**< Qingping CGG1/CGP1S sensor */
    BLE_REG_DEVICE_TYPE_SWITCHBOT_METER,    /**< SwitchBot Meter temperature sensor */
    BLE_REG_DEVICE_TYPE_SWITCHBOT_BOT,      /**< SwitchBot Bot switch */
    BLE_REG_DEVICE_TYPE_SWITCHBOT_CURTAIN,  /**< SwitchBot Curtain motor */
    BLE_REG_DEVICE_TYPE_INKBIRD,            /**< Inkbird IBS-TH1/TH2 sensor */
    BLE_REG_DEVICE_TYPE_GENERIC_SENSOR,     /**< Generic BLE sensor */
    BLE_REG_DEVICE_TYPE_MAX
} ble_reg_device_type_t;

/**
 * @brief Raw BLE advertisement data structure for registry
 *
 * Note: This is a simplified structure compared to ble_adv_data_t in ble_common.h.
 * Use this for registry operations; convert from ble_adv_data_t as needed.
 */
typedef struct {
    uint8_t mac[6];         /**< Device MAC address */
    int8_t rssi;            /**< Received signal strength indicator (dBm) */
    uint8_t *adv_data;      /**< Advertisement data pointer */
    size_t adv_len;         /**< Advertisement data length */
    uint32_t timestamp;     /**< Reception timestamp (epoch seconds) */
} ble_registry_adv_data_t;

/**
 * @brief Parsed sensor data (temperature/humidity sensors)
 */
typedef struct {
    float temperature;      /**< Temperature in Celsius */
    float humidity;         /**< Relative humidity in percent */
    uint8_t battery;        /**< Battery level in percent (0-100) */
    int8_t rssi;            /**< Signal strength at time of reading */
    uint32_t last_seen;     /**< Last update timestamp (epoch seconds) */
    bool valid;             /**< Data validity flag */
} ble_sensor_data_t;

/**
 * @brief Parsed beacon data (iBeacon/Eddystone)
 */
typedef struct {
    uint8_t uuid[16];       /**< Beacon UUID (iBeacon) or Namespace (Eddystone) */
    uint16_t major;         /**< iBeacon Major value */
    uint16_t minor;         /**< iBeacon Minor value / Eddystone Instance */
    int8_t tx_power;        /**< Calibrated TX power at 1m */
    float distance;         /**< Estimated distance in meters */
    int8_t rssi;            /**< Current RSSI */
    uint32_t last_seen;     /**< Last update timestamp */
    bool present;           /**< Presence detection flag */
} ble_beacon_data_t;

/**
 * @brief Eddystone TLM telemetry data
 */
typedef struct {
    uint16_t battery_mv;    /**< Battery voltage in millivolts */
    float temperature;      /**< Beacon temperature in Celsius */
    uint32_t adv_count;     /**< Advertisement count since boot */
    uint32_t uptime_100ms;  /**< Uptime in 100ms units */
    uint32_t last_seen;     /**< Last update timestamp */
} ble_eddystone_tlm_t;

/**
 * @brief Unified parsed data union
 */
typedef union {
    ble_sensor_data_t sensor;       /**< Sensor data (Xiaomi, Govee) */
    ble_beacon_data_t beacon;       /**< Beacon data (iBeacon, Eddystone-UID) */
    ble_eddystone_tlm_t tlm;        /**< Eddystone TLM data */
} ble_parsed_data_t;

/**
 * @brief Tracked BLE device entry for registry
 */
typedef struct {
    uint8_t mac[6];                                    /**< Device MAC address */
    ble_reg_device_type_t type;                        /**< Detected device type */
    char name[BLE_REGISTRY_DEVICE_NAME_MAX_LEN];       /**< Friendly name */
    ble_parsed_data_t data;                            /**< Latest parsed data */
    uint32_t first_seen;                               /**< First detection timestamp */
    uint32_t last_seen;                                /**< Last detection timestamp */
    uint32_t update_count;                             /**< Number of updates received */
    bool active;                                       /**< Slot in use flag */
} ble_registry_tracked_device_t;

/**
 * @brief Device parser function prototype
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed data output
 * @return ESP_OK on successful parse, ESP_ERR_NOT_SUPPORTED if not parseable
 */
typedef esp_err_t (*ble_device_parser_fn_t)(const ble_registry_adv_data_t *adv,
                                             ble_parsed_data_t *out_data);

/**
 * @brief Device type detector function prototype
 *
 * @param[in] adv Advertisement data to analyze
 * @return Detected device type or BLE_REG_DEVICE_TYPE_UNKNOWN
 */
typedef ble_reg_device_type_t (*ble_device_detector_fn_t)(const ble_registry_adv_data_t *adv);

/**
 * @brief Device update callback function prototype
 *
 * @param[in] device Updated device entry
 * @param[in] is_new True if this is a newly discovered device
 */
typedef void (*ble_device_update_cb_t)(const ble_registry_tracked_device_t *device, bool is_new);

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Initialize the BLE device registry
 *
 * Allocates device registry storage and initializes internal state.
 * Must be called before any other registry functions.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - ESP_ERR_INVALID_STATE if already initialized
 */
esp_err_t ble_device_registry_init(void);

/**
 * @brief Deinitialize the BLE device registry
 *
 * Frees all allocated resources and clears device data.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ble_device_registry_deinit(void);

/**
 * @brief Check if the device registry is initialized
 *
 * @return true if initialized, false otherwise
 */
bool ble_device_registry_is_initialized(void);

/**
 * @brief Process incoming BLE advertisement
 *
 * Detects device type, dispatches to appropriate parser,
 * and updates the device cache.
 *
 * @param[in] adv Advertisement data to process
 * @return
 *      - ESP_OK on successful processing
 *      - ESP_ERR_INVALID_ARG if adv is NULL
 *      - ESP_ERR_NOT_SUPPORTED if device type not recognized
 */
esp_err_t ble_device_registry_process_adv(const ble_registry_adv_data_t *adv);

/**
 * @brief Get tracked device by MAC address
 *
 * @param[in] mac 6-byte MAC address
 * @return Pointer to device entry or NULL if not found
 */
const ble_registry_tracked_device_t *ble_device_registry_get_by_mac(const uint8_t mac[6]);

/**
 * @brief Get all tracked devices
 *
 * @param[out] devices Array to fill with device pointers
 * @param[in] max_count Maximum number of devices to return
 * @return Number of devices returned
 */
size_t ble_device_registry_get_all(const ble_registry_tracked_device_t **devices, size_t max_count);

/**
 * @brief Get count of currently tracked devices
 *
 * @return Number of active tracked devices
 */
size_t ble_device_registry_get_count(void);

/**
 * @brief Remove stale devices from registry
 *
 * Removes devices that haven't been seen within the timeout period.
 *
 * @param[in] timeout_s Timeout in seconds (0 uses default)
 * @return Number of devices removed
 */
size_t ble_device_registry_cleanup_stale(uint32_t timeout_s);

/**
 * @brief Set device friendly name
 *
 * @param[in] mac 6-byte MAC address
 * @param[in] name Friendly name (will be truncated if too long)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_INVALID_ARG if mac or name is NULL
 */
esp_err_t ble_device_registry_set_name(const uint8_t mac[6], const char *name);

/**
 * @brief Register device update callback
 *
 * @param[in] callback Function to call on device updates
 * @return ESP_OK on success
 */
esp_err_t ble_device_registry_register_callback(ble_device_update_cb_t callback);

/**
 * @brief Get device type name string
 *
 * @param[in] type Device type enumeration
 * @return Human-readable device type name
 */
const char *ble_reg_device_type_to_string(ble_reg_device_type_t type);

/**
 * @brief Format MAC address as string (registry version)
 *
 * @param[in] mac 6-byte MAC address
 * @param[out] buf Output buffer (minimum 18 bytes)
 * @param[in] buf_len Buffer length
 * @return Pointer to buf on success, NULL on error
 */
char *ble_registry_mac_to_string(const uint8_t mac[6], char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_DEVICE_REGISTRY_H */
