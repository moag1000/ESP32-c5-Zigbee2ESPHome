/**
 * @file ble_gatt_discovery.h
 * @brief Generic GATT Service Discovery for Unknown BLE Devices
 *
 * Provides automatic discovery and mapping of all GATT services and
 * characteristics for unknown BLE devices. Discovered services are
 * cached and can be published to MQTT for external tools.
 *
 * Features:
 * - Full service discovery with characteristic enumeration
 * - UUID-to-name mapping for standard BLE services
 * - Discovery result caching per device
 * - MQTT publishing of discovery results
 * - Characteristic properties parsing
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_GATT_DISCOVERY_H
#define BLE_GATT_DISCOVERY_H

#include "esp_err.h"
#include "ble_gatt_client.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Maximum services per discovery result */
#define BLE_DISCOVERY_MAX_SERVICES          16

/** @brief Maximum characteristics per service */
#define BLE_DISCOVERY_MAX_CHARS_PER_SVC     16

/** @brief Maximum cached discovery results */
#define BLE_DISCOVERY_MAX_CACHED_DEVICES    8

/** @brief Discovery timeout in milliseconds */
#define BLE_DISCOVERY_TIMEOUT_MS            30000

/** @brief Maximum UUID name length */
#define BLE_DISCOVERY_UUID_NAME_MAX_LEN     32

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief Discovered characteristic information
 */
typedef struct {
    ble_gatt_uuid_t uuid;                               /**< Characteristic UUID */
    uint16_t value_handle;                              /**< Value attribute handle */
    uint16_t cccd_handle;                               /**< CCCD handle (0 if none) */
    uint8_t properties;                                 /**< Characteristic properties */
    char name[BLE_DISCOVERY_UUID_NAME_MAX_LEN];         /**< Human-readable name */
} ble_discovery_characteristic_t;

/**
 * @brief Discovered service information
 */
typedef struct {
    ble_gatt_uuid_t uuid;                               /**< Service UUID */
    uint16_t start_handle;                              /**< Start attribute handle */
    uint16_t end_handle;                                /**< End attribute handle */
    bool primary;                                       /**< Is primary service */
    char name[BLE_DISCOVERY_UUID_NAME_MAX_LEN];         /**< Human-readable name */
    ble_discovery_characteristic_t characteristics[BLE_DISCOVERY_MAX_CHARS_PER_SVC];
    uint8_t char_count;                                 /**< Number of characteristics */
} ble_discovery_service_t;

/**
 * @brief Complete discovery result for a device
 */
typedef struct {
    uint8_t mac[6];                                     /**< Device MAC address */
    uint16_t conn_handle;                               /**< Connection handle used */
    ble_discovery_service_t services[BLE_DISCOVERY_MAX_SERVICES];
    uint8_t service_count;                              /**< Number of services */
    uint32_t discovery_time;                            /**< Discovery timestamp */
    bool complete;                                      /**< Discovery completed flag */
} ble_discovery_result_t;

/**
 * @brief Discovery completion callback
 *
 * @param mac Device MAC address
 * @param result Discovery result (NULL on failure)
 * @param success true if discovery succeeded
 */
typedef void (*ble_discovery_complete_cb_t)(const uint8_t *mac,
                                             const ble_discovery_result_t *result,
                                             bool success);

/**
 * @brief Discovery configuration
 */
typedef struct {
    bool auto_discover_on_connect;      /**< Auto-discover on new connections */
    bool cache_results;                 /**< Cache discovery results */
    bool publish_to_mqtt;               /**< Publish results to MQTT */
    uint32_t timeout_ms;                /**< Discovery timeout */
} ble_discovery_config_t;

/**
 * @brief Default discovery configuration
 */
#define BLE_DISCOVERY_CONFIG_DEFAULT() { \
    .auto_discover_on_connect = false, \
    .cache_results = true, \
    .publish_to_mqtt = true, \
    .timeout_ms = BLE_DISCOVERY_TIMEOUT_MS, \
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * @brief Initialize the GATT discovery module
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized
 *      - ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t ble_gatt_discovery_init(void);

/**
 * @brief Initialize with custom configuration
 *
 * @param config Discovery configuration
 * @return ESP_OK on success
 */
esp_err_t ble_gatt_discovery_init_with_config(const ble_discovery_config_t *config);

/**
 * @brief Deinitialize the discovery module
 *
 * @return ESP_OK on success
 */
esp_err_t ble_gatt_discovery_deinit(void);

/**
 * @brief Check if module is initialized
 *
 * @return true if initialized
 */
bool ble_gatt_discovery_is_initialized(void);

/* ============================================================================
 * Discovery Operations
 * ============================================================================ */

/**
 * @brief Start discovery for a connected device
 *
 * Initiates full service and characteristic discovery for the
 * specified device. Results are delivered via the completion callback.
 *
 * @param mac Device MAC address (must be connected)
 * @param callback Completion callback (can be NULL)
 * @return
 *      - ESP_OK if discovery started
 *      - ESP_ERR_NOT_FOUND if device not connected
 *      - ESP_ERR_INVALID_STATE if discovery already in progress
 */
esp_err_t ble_gatt_discovery_start(const uint8_t *mac, ble_discovery_complete_cb_t callback);

/**
 * @brief Start discovery by connection handle
 *
 * @param conn_handle GATT connection handle
 * @param callback Completion callback
 * @return ESP_OK if discovery started
 */
esp_err_t ble_gatt_discovery_start_by_handle(uint16_t conn_handle,
                                              ble_discovery_complete_cb_t callback);

/**
 * @brief Cancel ongoing discovery
 *
 * @param mac Device MAC address
 * @return ESP_OK if cancelled
 */
esp_err_t ble_gatt_discovery_cancel(const uint8_t *mac);

/**
 * @brief Check if discovery is in progress
 *
 * @param mac Device MAC address
 * @return true if discovery in progress
 */
bool ble_gatt_discovery_in_progress(const uint8_t *mac);

/* ============================================================================
 * Result Retrieval
 * ============================================================================ */

/**
 * @brief Get cached discovery result for a device
 *
 * @param mac Device MAC address
 * @param result Output: Discovery result
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if no cached result
 */
esp_err_t ble_gatt_discovery_get_cached(const uint8_t *mac, ble_discovery_result_t *result);

/**
 * @brief Check if device has cached discovery
 *
 * @param mac Device MAC address
 * @return true if cached result exists
 */
bool ble_gatt_discovery_has_cached(const uint8_t *mac);

/**
 * @brief Clear cached discovery result
 *
 * @param mac Device MAC address
 * @return ESP_OK on success
 */
esp_err_t ble_gatt_discovery_clear_cached(const uint8_t *mac);

/**
 * @brief Clear all cached discoveries
 *
 * @return ESP_OK on success
 */
esp_err_t ble_gatt_discovery_clear_all_cached(void);

/* ============================================================================
 * UUID Name Lookup
 * ============================================================================ */

/**
 * @brief Get human-readable name for a 16-bit UUID
 *
 * Returns the standard Bluetooth SIG name for known service and
 * characteristic UUIDs.
 *
 * @param uuid16 16-bit UUID value
 * @return Name string or "Unknown" if not recognized
 */
const char *ble_gatt_discovery_uuid16_name(uint16_t uuid16);

/**
 * @brief Format UUID as string
 *
 * @param uuid UUID to format
 * @param buf Output buffer (at least 37 bytes for 128-bit)
 * @param buf_len Buffer length
 * @return Pointer to buf
 */
char *ble_gatt_discovery_uuid_to_str(const ble_gatt_uuid_t *uuid, char *buf, size_t buf_len);

/**
 * @brief Format characteristic properties as string
 *
 * @param properties Property flags
 * @param buf Output buffer (at least 64 bytes recommended)
 * @param buf_len Buffer length
 * @return Pointer to buf
 */
char *ble_gatt_discovery_props_to_str(uint8_t properties, char *buf, size_t buf_len);

/* ============================================================================
 * MQTT Publishing
 * ============================================================================ */

/**
 * @brief Publish discovery results to MQTT
 *
 * Publishes to: zigbee2mqtt/ble/<mac>/services
 *
 * @param result Discovery result to publish
 * @return ESP_OK on success
 */
esp_err_t ble_gatt_discovery_publish_mqtt(const ble_discovery_result_t *result);

/**
 * @brief Publish discovery as JSON string
 *
 * @param result Discovery result
 * @param json_buf Output buffer for JSON
 * @param buf_len Buffer length
 * @return Number of bytes written, or -1 on error
 */
int ble_gatt_discovery_to_json(const ble_discovery_result_t *result,
                                char *json_buf, size_t buf_len);

/* ============================================================================
 * Callback Registration
 * ============================================================================ */

/**
 * @brief Set global discovery completion callback
 *
 * This callback is invoked for all discovery completions in
 * addition to any per-discovery callbacks.
 *
 * @param callback Global callback (NULL to unregister)
 */
void ble_gatt_discovery_set_callback(ble_discovery_complete_cb_t callback);

#ifdef __cplusplus
}
#endif

#endif /* BLE_GATT_DISCOVERY_H */
