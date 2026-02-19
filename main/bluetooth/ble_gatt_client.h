/**
 * @file ble_gatt_client.h
 * @brief BLE GATT Client API for ESP32-C5 Zigbee2MQTT Gateway
 *
 * This module provides BLE GATT client functionality for connecting to
 * BLE peripherals, discovering services/characteristics, and performing
 * read/write/notify operations.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_GATT_CLIENT_H
#define BLE_GATT_CLIENT_H

#include "esp_err.h"
#include "ble_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/**
 * @brief Maximum number of simultaneous BLE connections
 */
#define BLE_GATT_MAX_CONNECTIONS        3

/**
 * @brief Maximum number of services per connection
 */
#define BLE_GATT_MAX_SERVICES           16

/**
 * @brief Maximum number of characteristics per service
 */
#define BLE_GATT_MAX_CHARACTERISTICS    32

/**
 * @brief Maximum number of notification subscriptions per connection
 */
#define BLE_GATT_MAX_SUBSCRIPTIONS      16

/**
 * @brief Default connection timeout in milliseconds
 */
#define BLE_GATT_DEFAULT_CONN_TIMEOUT_MS    10000

/**
 * @brief Default GATT operation timeout in milliseconds
 */
#define BLE_GATT_DEFAULT_OP_TIMEOUT_MS      5000

/**
 * @brief Default MTU size
 */
#define BLE_GATT_DEFAULT_MTU            23

/**
 * @brief Preferred MTU size for ESP32-C5
 */
#define BLE_GATT_PREFERRED_MTU          256

/**
 * @brief Invalid connection handle value
 */
#define BLE_GATT_INVALID_CONN_HANDLE    0xFFFF

/**
 * @brief Invalid attribute handle value
 */
#define BLE_GATT_INVALID_ATTR_HANDLE    0x0000

/* ============================================================================
 * Standard Service UUIDs
 * ============================================================================ */

#define BLE_UUID_GENERIC_ACCESS         0x1800  /**< Generic Access Service */
#define BLE_UUID_GENERIC_ATTRIBUTE      0x1801  /**< Generic Attribute Service */
#define BLE_UUID_IMMEDIATE_ALERT        0x1802  /**< Immediate Alert Service */
#define BLE_UUID_LINK_LOSS              0x1803  /**< Link Loss Service */
#define BLE_UUID_TX_POWER               0x1804  /**< TX Power Service */
#define BLE_UUID_DEVICE_INFO            0x180A  /**< Device Information Service */
#define BLE_UUID_BATTERY_SERVICE        0x180F  /**< Battery Service */
#define BLE_UUID_ENVIRONMENTAL_SENSING  0x181A  /**< Environmental Sensing Service */

/* ============================================================================
 * Standard Characteristic UUIDs
 * ============================================================================ */

#define BLE_UUID_DEVICE_NAME            0x2A00  /**< Device Name */
#define BLE_UUID_APPEARANCE             0x2A01  /**< Appearance */
#define BLE_UUID_MANUFACTURER_NAME      0x2A29  /**< Manufacturer Name String */
#define BLE_UUID_MODEL_NUMBER           0x2A24  /**< Model Number String */
#define BLE_UUID_SERIAL_NUMBER          0x2A25  /**< Serial Number String */
#define BLE_UUID_FIRMWARE_REV           0x2A26  /**< Firmware Revision String */
#define BLE_UUID_HARDWARE_REV           0x2A27  /**< Hardware Revision String */
#define BLE_UUID_SOFTWARE_REV           0x2A28  /**< Software Revision String */
#define BLE_UUID_BATTERY_LEVEL          0x2A19  /**< Battery Level */
#define BLE_UUID_TEMPERATURE            0x2A6E  /**< Temperature */
#define BLE_UUID_HUMIDITY               0x2A6F  /**< Humidity */
#define BLE_UUID_PRESSURE               0x2A6D  /**< Pressure */

/* ============================================================================
 * Descriptor UUIDs
 * ============================================================================ */

#define BLE_UUID_CCCD                   0x2902  /**< Client Characteristic Configuration Descriptor */
#define BLE_UUID_CHAR_USER_DESC         0x2901  /**< Characteristic User Description */

/* ============================================================================
 * CCCD Values
 * ============================================================================ */

#define BLE_GATT_CCCD_NOTIFY            0x0001  /**< Enable notifications */
#define BLE_GATT_CCCD_INDICATE          0x0002  /**< Enable indications */

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief GATT client state enumeration
 */
typedef enum {
    BLE_GATT_STATE_UNINITIALIZED = 0,   /**< GATT client not initialized */
    BLE_GATT_STATE_INITIALIZED,          /**< GATT client initialized and ready */
    BLE_GATT_STATE_CONNECTING,           /**< Connection in progress */
    BLE_GATT_STATE_CONNECTED,            /**< At least one connection active */
    BLE_GATT_STATE_ERROR                 /**< GATT client in error state */
} ble_gatt_state_t;

/**
 * @brief Connection state enumeration
 */
typedef enum {
    BLE_CONN_STATE_DISCONNECTED = 0,     /**< Not connected */
    BLE_CONN_STATE_CONNECTING,           /**< Connection in progress */
    BLE_CONN_STATE_CONNECTED,            /**< Connected */
    BLE_CONN_STATE_DISCOVERING,          /**< Service discovery in progress */
    BLE_CONN_STATE_READY,                /**< Connected and services discovered */
    BLE_CONN_STATE_DISCONNECTING         /**< Disconnect in progress */
} ble_conn_state_t;

/**
 * @brief Characteristic properties flags
 */
typedef enum {
    BLE_GATT_CHR_PROP_BROADCAST         = 0x01,  /**< Broadcast supported */
    BLE_GATT_CHR_PROP_READ              = 0x02,  /**< Read supported */
    BLE_GATT_CHR_PROP_WRITE_NO_RSP      = 0x04,  /**< Write without response */
    BLE_GATT_CHR_PROP_WRITE             = 0x08,  /**< Write with response */
    BLE_GATT_CHR_PROP_NOTIFY            = 0x10,  /**< Notifications supported */
    BLE_GATT_CHR_PROP_INDICATE          = 0x20,  /**< Indications supported */
    BLE_GATT_CHR_PROP_AUTH_WRITE        = 0x40,  /**< Authenticated write */
    BLE_GATT_CHR_PROP_EXT_PROP          = 0x80   /**< Extended properties */
} ble_gatt_chr_prop_t;

/**
 * @brief UUID type (16-bit or 128-bit)
 *
 * Note: Named ble_gatt_uuid_type_t to avoid conflict with NimBLE's ble_gatt_uuid_type_t
 */
typedef enum {
    BLE_GATT_UUID_TYPE_16 = 16,          /**< 16-bit UUID */
    BLE_GATT_UUID_TYPE_128 = 128         /**< 128-bit UUID */
} ble_gatt_uuid_type_t;

/**
 * @brief UUID structure
 *
 * Note: Named ble_gatt_uuid_t to avoid conflict with NimBLE's ble_uuid_t
 */
typedef struct {
    ble_gatt_uuid_type_t type;           /**< UUID type (16 or 128 bit) */
    union {
        uint16_t uuid16;                 /**< 16-bit UUID value */
        uint8_t uuid128[16];             /**< 128-bit UUID value */
    } value;
} ble_gatt_uuid_t;

/**
 * @brief GATT service structure
 */
typedef struct {
    ble_gatt_uuid_t uuid;                     /**< Service UUID */
    uint16_t start_handle;               /**< Start attribute handle */
    uint16_t end_handle;                 /**< End attribute handle */
    bool primary;                        /**< True if primary service */
} ble_gatt_service_t;

/**
 * @brief GATT characteristic structure
 */
typedef struct {
    ble_gatt_uuid_t uuid;                     /**< Characteristic UUID */
    uint16_t def_handle;                 /**< Characteristic definition handle */
    uint16_t value_handle;               /**< Characteristic value handle */
    uint16_t cccd_handle;                /**< CCCD handle (0 if not present) */
    uint8_t properties;                  /**< Characteristic properties */
} ble_gatt_characteristic_t;

/**
 * @brief GATT connection information structure
 */
typedef struct {
    uint8_t mac[BLE_MAC_ADDR_LEN];       /**< Device MAC address */
    ble_addr_type_t addr_type;           /**< Address type */
    uint16_t conn_handle;                /**< Connection handle */
    ble_conn_state_t state;              /**< Connection state */
    bool encrypted;                      /**< Connection is encrypted */
    uint16_t mtu;                        /**< Current MTU size */
    int8_t rssi;                         /**< Connection RSSI */
    uint8_t service_count;               /**< Number of discovered services */
    bool auto_reconnect;                 /**< Auto-reconnect enabled */
} ble_gatt_connection_t;

/**
 * @brief Service discovery result structure
 */
typedef struct {
    uint16_t conn_handle;                /**< Connection handle */
    ble_gatt_service_t *services;        /**< Array of discovered services */
    uint8_t service_count;               /**< Number of services */
    bool complete;                       /**< Discovery complete flag */
} ble_gatt_disc_result_t;

/* ============================================================================
 * Callback Type Definitions
 * ============================================================================ */

/**
 * @brief Connection callback function type
 *
 * @param mac Device MAC address
 * @param success Connection successful
 * @param conn_handle Connection handle (valid if success is true)
 */
typedef void (*ble_gatt_connect_cb_t)(const uint8_t *mac, bool success, uint16_t conn_handle);

/**
 * @brief Disconnection callback function type
 *
 * @param mac Device MAC address
 * @param reason Disconnection reason code
 */
typedef void (*ble_gatt_disconnect_cb_t)(const uint8_t *mac, uint8_t reason);

/**
 * @brief Notification/Indication callback function type
 *
 * @param conn_handle Connection handle
 * @param attr_handle Attribute handle
 * @param data Notification data
 * @param len Data length
 */
typedef void (*ble_gatt_notify_cb_t)(uint16_t conn_handle, uint16_t attr_handle,
                                      const uint8_t *data, uint16_t len);

/**
 * @brief Service discovery callback function type
 *
 * @param result Service discovery result
 */
typedef void (*ble_gatt_disc_cb_t)(const ble_gatt_disc_result_t *result);

/**
 * @brief MTU exchange callback function type
 *
 * @param conn_handle Connection handle
 * @param mtu Negotiated MTU value
 */
typedef void (*ble_gatt_mtu_cb_t)(uint16_t conn_handle, uint16_t mtu);

/* ============================================================================
 * GATT Client Configuration
 * ============================================================================ */

/**
 * @brief GATT client configuration structure
 */
typedef struct {
    bool auto_discover_services;         /**< Auto-discover services on connect */
    bool auto_mtu_exchange;              /**< Auto MTU exchange on connect */
    uint16_t preferred_mtu;              /**< Preferred MTU size */
    uint32_t conn_timeout_ms;            /**< Default connection timeout */
    uint32_t op_timeout_ms;              /**< Default operation timeout */
} ble_gatt_client_config_t;

/**
 * @brief Default GATT client configuration
 */
#define BLE_GATT_CLIENT_CONFIG_DEFAULT() { \
    .auto_discover_services = true, \
    .auto_mtu_exchange = true, \
    .preferred_mtu = BLE_GATT_PREFERRED_MTU, \
    .conn_timeout_ms = BLE_GATT_DEFAULT_CONN_TIMEOUT_MS, \
    .op_timeout_ms = BLE_GATT_DEFAULT_OP_TIMEOUT_MS \
}

/* ============================================================================
 * Initialization and Deinitialization
 * ============================================================================ */

/**
 * @brief Initialize the BLE GATT client
 *
 * Initializes the GATT client module. The BLE manager must be initialized
 * before calling this function.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if already initialized or BLE manager not ready
 * @return ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t ble_gatt_client_init(void);

/**
 * @brief Initialize the BLE GATT client with custom configuration
 *
 * @param config Pointer to configuration structure
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if config is NULL
 * @return ESP_ERR_INVALID_STATE if already initialized
 * @return ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t ble_gatt_client_init_with_config(const ble_gatt_client_config_t *config);

/**
 * @brief Deinitialize the BLE GATT client
 *
 * Disconnects all connections and releases resources.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ble_gatt_client_deinit(void);

/**
 * @brief Check if GATT client is initialized
 *
 * @return true if initialized
 * @return false if not initialized
 */
bool ble_gatt_client_is_initialized(void);

/**
 * @brief Get GATT client state
 *
 * @return Current GATT client state
 */
ble_gatt_state_t ble_gatt_client_get_state(void);

/**
 * @brief Get GATT client state as string
 *
 * @return String representation of current state
 */
const char *ble_gatt_client_get_state_str(void);

/* ============================================================================
 * Connection Management
 * ============================================================================ */

/**
 * @brief Connect to a BLE device
 *
 * Initiates a connection to the specified BLE device. The connection
 * callback will be invoked with the result.
 *
 * @param mac Device MAC address (6 bytes)
 * @param addr_type Address type
 * @param timeout_ms Connection timeout in milliseconds (0 for default)
 * @return ESP_OK if connection initiated successfully
 * @return ESP_ERR_INVALID_ARG if mac is NULL
 * @return ESP_ERR_INVALID_STATE if not initialized
 * @return ESP_ERR_NO_MEM if maximum connections reached
 */
esp_err_t ble_gatt_connect(const uint8_t *mac, ble_addr_type_t addr_type, uint32_t timeout_ms);

/**
 * @brief Disconnect from a BLE device
 *
 * Terminates the connection with the specified handle.
 *
 * @param conn_handle Connection handle
 * @return ESP_OK if disconnect initiated
 * @return ESP_ERR_INVALID_ARG if invalid connection handle
 * @return ESP_ERR_NOT_FOUND if connection not found
 */
esp_err_t ble_gatt_disconnect(uint16_t conn_handle);

/**
 * @brief Disconnect from a BLE device by MAC address
 *
 * @param mac Device MAC address
 * @return ESP_OK if disconnect initiated
 * @return ESP_ERR_INVALID_ARG if mac is NULL
 * @return ESP_ERR_NOT_FOUND if device not connected
 */
esp_err_t ble_gatt_disconnect_by_mac(const uint8_t *mac);

/**
 * @brief Disconnect all connected devices
 *
 * @return ESP_OK on success
 */
esp_err_t ble_gatt_disconnect_all(void);

/**
 * @brief Check if device is connected
 *
 * @param mac Device MAC address
 * @return true if device is connected
 * @return false if not connected
 */
bool ble_gatt_is_connected(const uint8_t *mac);

/**
 * @brief Check if connection handle is valid and connected
 *
 * @param conn_handle Connection handle
 * @return true if connected
 * @return false if not connected
 */
bool ble_gatt_is_handle_connected(uint16_t conn_handle);

/**
 * @brief Get connection handle by MAC address
 *
 * @param mac Device MAC address
 * @return Connection handle, or BLE_GATT_INVALID_CONN_HANDLE if not found
 */
uint16_t ble_gatt_get_conn_handle(const uint8_t *mac);

/**
 * @brief Get number of active connections
 *
 * @return Number of active connections
 */
uint8_t ble_gatt_get_connection_count(void);

/**
 * @brief Get connection information
 *
 * @param conn_handle Connection handle
 * @param conn_info Pointer to connection info structure to fill
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if conn_info is NULL
 * @return ESP_ERR_NOT_FOUND if connection not found
 */
esp_err_t ble_gatt_get_connection_info(uint16_t conn_handle, ble_gatt_connection_t *conn_info);

/**
 * @brief Enable or disable auto-reconnect for a connection
 *
 * @param conn_handle Connection handle
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if connection not found
 */
esp_err_t ble_gatt_set_auto_reconnect(uint16_t conn_handle, bool enable);

/* ============================================================================
 * Service Discovery
 * ============================================================================ */

/**
 * @brief Discover all services on a device
 *
 * Initiates service discovery for all services. Results are delivered
 * via the discovery callback.
 *
 * @param conn_handle Connection handle
 * @return ESP_OK if discovery started
 * @return ESP_ERR_INVALID_STATE if not connected
 * @return ESP_ERR_NOT_FOUND if connection not found
 */
esp_err_t ble_gatt_discover_all_services(uint16_t conn_handle);

/**
 * @brief Discover a specific service by UUID
 *
 * @param conn_handle Connection handle
 * @param uuid Service UUID to discover
 * @return ESP_OK if discovery started
 * @return ESP_ERR_INVALID_ARG if uuid is NULL
 * @return ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t ble_gatt_discover_service_by_uuid(uint16_t conn_handle, const ble_gatt_uuid_t *uuid);

/**
 * @brief Discover characteristics of a service
 *
 * @param conn_handle Connection handle
 * @param start_handle Start attribute handle of service
 * @param end_handle End attribute handle of service
 * @return ESP_OK if discovery started
 * @return ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t ble_gatt_discover_characteristics(uint16_t conn_handle,
                                             uint16_t start_handle,
                                             uint16_t end_handle);

/**
 * @brief Discover descriptors of a characteristic
 *
 * @param conn_handle Connection handle
 * @param start_handle Start attribute handle (characteristic value handle + 1)
 * @param end_handle End attribute handle (next characteristic def handle - 1 or service end)
 * @return ESP_OK if discovery started
 * @return ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t ble_gatt_discover_descriptors(uint16_t conn_handle,
                                         uint16_t start_handle,
                                         uint16_t end_handle);

/**
 * @brief Get discovered services for a connection
 *
 * @param conn_handle Connection handle
 * @param services Buffer to store service array
 * @param max_services Maximum services to return
 * @param count Pointer to store actual count
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if services or count is NULL
 * @return ESP_ERR_NOT_FOUND if connection not found
 */
esp_err_t ble_gatt_get_services(uint16_t conn_handle,
                                 ble_gatt_service_t *services,
                                 uint8_t max_services,
                                 uint8_t *count);

/**
 * @brief Get discovered characteristics for a service
 *
 * @param conn_handle Connection handle
 * @param service_handle Service start handle
 * @param characteristics Buffer to store characteristic array
 * @param max_chars Maximum characteristics to return
 * @param count Pointer to store actual count
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if service not found
 */
esp_err_t ble_gatt_get_characteristics(uint16_t conn_handle,
                                        uint16_t service_handle,
                                        ble_gatt_characteristic_t *characteristics,
                                        uint8_t max_chars,
                                        uint8_t *count);

/* ============================================================================
 * Read Operations
 * ============================================================================ */

/**
 * @brief Read a characteristic value
 *
 * Performs a synchronous read operation with timeout.
 *
 * @param conn_handle Connection handle
 * @param attr_handle Attribute handle to read
 * @param buf Buffer to store read data
 * @param len Pointer to buffer length (in: max len, out: actual len)
 * @param timeout_ms Read timeout in milliseconds (0 for default)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if buf or len is NULL
 * @return ESP_ERR_TIMEOUT if operation timed out
 * @return ESP_FAIL on read error
 */
esp_err_t ble_gatt_read_characteristic(uint16_t conn_handle,
                                        uint16_t attr_handle,
                                        uint8_t *buf,
                                        uint16_t *len,
                                        uint32_t timeout_ms);

/**
 * @brief Read a characteristic value by UUID
 *
 * @param conn_handle Connection handle
 * @param start_handle Start of range to search
 * @param end_handle End of range to search
 * @param uuid Characteristic UUID
 * @param buf Buffer to store read data
 * @param len Pointer to buffer length
 * @param timeout_ms Read timeout
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if characteristic not found
 */
esp_err_t ble_gatt_read_by_uuid(uint16_t conn_handle,
                                 uint16_t start_handle,
                                 uint16_t end_handle,
                                 const ble_gatt_uuid_t *uuid,
                                 uint8_t *buf,
                                 uint16_t *len,
                                 uint32_t timeout_ms);

/**
 * @brief Read a descriptor value
 *
 * @param conn_handle Connection handle
 * @param desc_handle Descriptor handle
 * @param buf Buffer to store read data
 * @param len Pointer to buffer length
 * @param timeout_ms Read timeout
 * @return ESP_OK on success
 */
esp_err_t ble_gatt_read_descriptor(uint16_t conn_handle,
                                    uint16_t desc_handle,
                                    uint8_t *buf,
                                    uint16_t *len,
                                    uint32_t timeout_ms);

/* ============================================================================
 * Write Operations
 * ============================================================================ */

/**
 * @brief Write a characteristic value with response
 *
 * Performs a synchronous write operation with acknowledgment.
 *
 * @param conn_handle Connection handle
 * @param attr_handle Attribute handle to write
 * @param data Data to write
 * @param len Data length
 * @param timeout_ms Write timeout in milliseconds (0 for default)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if data is NULL
 * @return ESP_ERR_TIMEOUT if operation timed out
 * @return ESP_FAIL on write error
 */
esp_err_t ble_gatt_write_characteristic(uint16_t conn_handle,
                                         uint16_t attr_handle,
                                         const uint8_t *data,
                                         uint16_t len,
                                         uint32_t timeout_ms);

/**
 * @brief Write a characteristic value without response
 *
 * Performs a write without waiting for acknowledgment.
 *
 * @param conn_handle Connection handle
 * @param attr_handle Attribute handle to write
 * @param data Data to write
 * @param len Data length
 * @return ESP_OK if write queued successfully
 * @return ESP_ERR_INVALID_ARG if data is NULL
 */
esp_err_t ble_gatt_write_no_response(uint16_t conn_handle,
                                      uint16_t attr_handle,
                                      const uint8_t *data,
                                      uint16_t len);

/**
 * @brief Write a descriptor value
 *
 * @param conn_handle Connection handle
 * @param desc_handle Descriptor handle
 * @param data Data to write
 * @param len Data length
 * @param timeout_ms Write timeout
 * @return ESP_OK on success
 */
esp_err_t ble_gatt_write_descriptor(uint16_t conn_handle,
                                     uint16_t desc_handle,
                                     const uint8_t *data,
                                     uint16_t len,
                                     uint32_t timeout_ms);

/* ============================================================================
 * Notification/Indication Subscription
 * ============================================================================ */

/**
 * @brief Subscribe to characteristic notifications
 *
 * Enables notifications for a characteristic by writing to its CCCD.
 *
 * @param conn_handle Connection handle
 * @param attr_handle Characteristic value handle
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if CCCD not found
 */
esp_err_t ble_gatt_subscribe_notify(uint16_t conn_handle,
                                     uint16_t attr_handle,
                                     bool enable);

/**
 * @brief Subscribe to characteristic indications
 *
 * Enables indications for a characteristic by writing to its CCCD.
 *
 * @param conn_handle Connection handle
 * @param attr_handle Characteristic value handle
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if CCCD not found
 */
esp_err_t ble_gatt_subscribe_indicate(uint16_t conn_handle,
                                       uint16_t attr_handle,
                                       bool enable);

/**
 * @brief Get CCCD handle for a characteristic
 *
 * @param conn_handle Connection handle
 * @param chr_value_handle Characteristic value handle
 * @return CCCD handle, or BLE_GATT_INVALID_ATTR_HANDLE if not found
 */
uint16_t ble_gatt_get_cccd_handle(uint16_t conn_handle, uint16_t chr_value_handle);

/* ============================================================================
 * MTU Operations
 * ============================================================================ */

/**
 * @brief Exchange MTU with peer device
 *
 * Initiates MTU exchange to negotiate a larger MTU.
 *
 * @param conn_handle Connection handle
 * @param mtu Desired MTU size
 * @return ESP_OK if exchange initiated
 * @return ESP_ERR_INVALID_ARG if MTU out of range
 */
esp_err_t ble_gatt_exchange_mtu(uint16_t conn_handle, uint16_t mtu);

/**
 * @brief Get current MTU for a connection
 *
 * @param conn_handle Connection handle
 * @return Current MTU, or 0 if not connected
 */
uint16_t ble_gatt_get_mtu(uint16_t conn_handle);

/* ============================================================================
 * Callback Registration
 * ============================================================================ */

/**
 * @brief Set connection callback
 *
 * @param cb Connection callback function
 */
void ble_gatt_set_connect_cb(ble_gatt_connect_cb_t cb);

/**
 * @brief Set disconnection callback
 *
 * @param cb Disconnection callback function
 */
void ble_gatt_set_disconnect_cb(ble_gatt_disconnect_cb_t cb);

/**
 * @brief Set notification callback
 *
 * @param cb Notification callback function
 */
void ble_gatt_set_notify_cb(ble_gatt_notify_cb_t cb);

/**
 * @brief Set service discovery callback
 *
 * @param cb Service discovery callback function
 */
void ble_gatt_set_discovery_cb(ble_gatt_disc_cb_t cb);

/**
 * @brief Set MTU exchange callback
 *
 * @param cb MTU exchange callback function
 */
void ble_gatt_set_mtu_cb(ble_gatt_mtu_cb_t cb);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * @brief Create a 16-bit UUID
 *
 * @param uuid16 16-bit UUID value
 * @return ble_gatt_uuid_t structure
 */
ble_gatt_uuid_t ble_gatt_uuid16(uint16_t uuid16);

/**
 * @brief Create a 128-bit UUID
 *
 * @param uuid128 128-bit UUID array (16 bytes)
 * @return ble_gatt_uuid_t structure
 */
ble_gatt_uuid_t ble_gatt_uuid128(const uint8_t *uuid128);

/**
 * @brief Compare two UUIDs
 *
 * @param uuid1 First UUID
 * @param uuid2 Second UUID
 * @return true if UUIDs match
 */
bool ble_gatt_uuid_equal(const ble_gatt_uuid_t *uuid1, const ble_gatt_uuid_t *uuid2);

/**
 * @brief Convert UUID to string
 *
 * @param uuid UUID to convert
 * @param str Output buffer (at least 37 bytes for 128-bit UUID)
 * @return Pointer to str
 */
char *ble_gatt_uuid_to_str(const ble_gatt_uuid_t *uuid, char *str);

/**
 * @brief Get connection RSSI
 *
 * @param conn_handle Connection handle
 * @param rssi Pointer to store RSSI value
 * @return ESP_OK on success
 */
esp_err_t ble_gatt_get_rssi(uint16_t conn_handle, int8_t *rssi);

/**
 * @brief Self-test function for GATT client
 *
 * @return ESP_OK if all checks pass
 * @return ESP_FAIL if any check fails
 */
esp_err_t ble_gatt_client_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_GATT_CLIENT_H */
