/**
 * @file esphome_ble_proxy.h
 * @brief ESPHome Bluetooth Proxy Protocol (MSG 66-93, 126-127)
 *
 * Implements the Bluetooth Proxy protocol for Home Assistant integration,
 * enabling HA to receive BLE advertisements and perform GATT operations
 * through this device as a proxy.
 *
 * Protocol Messages (ESPHome API spec):
 * - MSG 66: Subscribe BLE Advertisements
 * - MSG 67: BLE Advertisement Response
 * - MSG 68: BLE Device Request (connect/disconnect/pair)
 * - MSG 69: BLE Device Connection Response
 * - MSG 73: BLE GATT Read Request
 * - MSG 74: BLE GATT Read Response
 * - MSG 75: BLE GATT Write Request
 * - MSG 76: BLE GATT Write Response
 * - MSG 78: BLE GATT Notify Request
 * - MSG 79: BLE GATT Notify Data Response
 * - MSG 80: Subscribe BLE Connections Free
 * - MSG 81: BLE Connections Free Response
 * - MSG 85: BLE Device Pairing Response
 * - MSG 87: Unsubscribe BLE Advertisements
 * - MSG 126: BLE Scanner State Response
 * - MSG 127: BLE Scanner Set Mode Request
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_BLE_PROXY_H
#define ESPHOME_BLE_PROXY_H

#include "esp_err.h"
#include "esphome_common.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/** @brief Maximum clients that can subscribe to BLE advertisements */
#define ESPHOME_BLE_PROXY_MAX_SUBSCRIBED_CLIENTS    ESPHOME_MAX_CLIENTS

/** @brief Maximum simultaneous GATT connections */
#define ESPHOME_BLE_PROXY_MAX_CONNECTIONS           3

/** @brief Queue size for advertisement forwarding */
#define ESPHOME_BLE_PROXY_ADV_QUEUE_SIZE            16

/** @brief Default RSSI filter threshold */
#define ESPHOME_BLE_PROXY_DEFAULT_RSSI_FILTER       (-90)

/** @brief Task stack size for advertisement processing */
#define ESPHOME_BLE_PROXY_TASK_STACK                4096

/** @brief Task priority for advertisement processing */
#define ESPHOME_BLE_PROXY_TASK_PRIO                 3

/* ============================================================================
 * BLE Device Request Types
 * ============================================================================ */

/**
 * @brief BLE device request type enumeration
 */
typedef enum {
    ESPHOME_BLE_REQUEST_CONNECT = 0,        /**< Connect to device */
    ESPHOME_BLE_REQUEST_DISCONNECT = 1,     /**< Disconnect from device */
    ESPHOME_BLE_REQUEST_PAIR = 2,           /**< Pair with device */
    ESPHOME_BLE_REQUEST_UNPAIR = 3,         /**< Unpair device */
    ESPHOME_BLE_REQUEST_CLEAR_CACHE = 4,    /**< Clear GATT cache */
} esphome_ble_request_type_t;

/**
 * @brief BLE GATT error codes
 */
typedef enum {
    ESPHOME_BLE_GATT_ERROR_NONE = 0,
    ESPHOME_BLE_GATT_ERROR_FAILED = 1,
    ESPHOME_BLE_GATT_ERROR_NOT_CONNECTED = 2,
    ESPHOME_BLE_GATT_ERROR_NOT_FOUND = 3,
    ESPHOME_BLE_GATT_ERROR_TIMEOUT = 4,
    ESPHOME_BLE_GATT_ERROR_NOT_PAIRED = 5,
} esphome_ble_gatt_error_t;

/* ============================================================================
 * Configuration Structures
 * ============================================================================ */

/**
 * @brief BLE Proxy configuration
 */
typedef struct {
    bool enable_advertisements;         /**< Forward BLE advertisements */
    bool enable_connections;            /**< Allow GATT connections */
    int8_t min_rssi;                    /**< Minimum RSSI filter (-127 to 0) */
    uint16_t scan_interval_ms;          /**< Scan interval in ms */
    uint16_t scan_window_ms;            /**< Scan window in ms */
} esphome_ble_proxy_config_t;

/**
 * @brief Default configuration macro
 */
#define ESPHOME_BLE_PROXY_CONFIG_DEFAULT() { \
    .enable_advertisements = true, \
    .enable_connections = true, \
    .min_rssi = ESPHOME_BLE_PROXY_DEFAULT_RSSI_FILTER, \
    .scan_interval_ms = 50, \
    .scan_window_ms = 30, \
}

/* ============================================================================
 * Statistics Structure
 * ============================================================================ */

/**
 * @brief BLE Proxy statistics
 */
typedef struct {
    uint32_t advertisements_received;   /**< Total advertisements received */
    uint32_t advertisements_forwarded;  /**< Advertisements sent to clients */
    uint32_t advertisements_filtered;   /**< Advertisements filtered by RSSI */
    uint32_t advertisements_dropped;    /**< BF-026: Advertisements dropped (queue full) */
    uint32_t gatt_reads;                /**< GATT read operations */
    uint32_t gatt_writes;               /**< GATT write operations */
    uint32_t gatt_errors;               /**< GATT operation errors */
    uint32_t connections;               /**< Total connections made */
    uint32_t disconnections;            /**< Total disconnections */
    uint8_t active_connections;         /**< Current active connections */
    uint8_t subscribed_clients;         /**< Clients subscribed to advertisements */
} esphome_ble_proxy_stats_t;

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================ */

/**
 * @brief Initialize BLE Proxy module
 *
 * Initializes the BLE proxy with the given configuration.
 * Requires BLE manager to be initialized first.
 *
 * @param[in] config Proxy configuration (NULL for defaults)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if already initialized
 * @return ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t esphome_ble_proxy_init(const esphome_ble_proxy_config_t *config);

/**
 * @brief Deinitialize BLE Proxy module
 *
 * Stops the proxy, disconnects all connections, and frees resources.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t esphome_ble_proxy_deinit(void);

/**
 * @brief Start BLE Proxy
 *
 * Starts scanning and advertisement forwarding.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized or already running
 */
esp_err_t esphome_ble_proxy_start(void);

/**
 * @brief Stop BLE Proxy
 *
 * Stops scanning and advertisement forwarding.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not running
 */
esp_err_t esphome_ble_proxy_stop(void);

/**
 * @brief Check if BLE Proxy is running
 *
 * @return true if proxy is running and forwarding advertisements
 */
bool esphome_ble_proxy_is_running(void);

/**
 * @brief Get BLE Proxy statistics
 *
 * @param[out] stats Statistics structure to fill
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if stats is NULL
 */
esp_err_t esphome_ble_proxy_get_stats(esphome_ble_proxy_stats_t *stats);

/**
 * @brief Reset BLE Proxy statistics
 */
void esphome_ble_proxy_reset_stats(void);

/* ============================================================================
 * Message Handlers (called from esphome_api.c)
 * ============================================================================ */

/**
 * @brief Handle Subscribe BLE Advertisements request (MSG 76)
 *
 * Subscribes/unsubscribes a client to BLE advertisement forwarding.
 *
 * @param[in] client_id Client identifier (0-based index)
 * @param[in] payload Message payload
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t esphome_ble_proxy_handle_subscribe_advertisements(
    uint8_t client_id, const uint8_t *payload, size_t len);

/**
 * @brief Handle BLE Device Request (MSG 66)
 *
 * Handles connect/disconnect/pair requests.
 *
 * @param[in] client_id Client identifier
 * @param[in] payload Message payload
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t esphome_ble_proxy_handle_device_request(
    uint8_t client_id, const uint8_t *payload, size_t len);

/**
 * @brief Handle BLE Connection Free request (MSG 67)
 *
 * Reports available connection slots.
 *
 * @param[in] client_id Client identifier
 * @param[in] payload Message payload
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t esphome_ble_proxy_handle_connection_free(
    uint8_t client_id, const uint8_t *payload, size_t len);

/**
 * @brief Handle BLE GATT Read Request (MSG 69)
 *
 * Performs a GATT characteristic read.
 *
 * @param[in] client_id Client identifier
 * @param[in] payload Message payload
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t esphome_ble_proxy_handle_gatt_read(
    uint8_t client_id, const uint8_t *payload, size_t len);

/**
 * @brief Handle BLE GATT Write Request (MSG 71)
 *
 * Performs a GATT characteristic write.
 *
 * @param[in] client_id Client identifier
 * @param[in] payload Message payload
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t esphome_ble_proxy_handle_gatt_write(
    uint8_t client_id, const uint8_t *payload, size_t len);

/**
 * @brief Handle BLE GATT Notify subscription request
 *
 * Subscribes/unsubscribes to GATT notifications.
 *
 * @param[in] client_id Client identifier
 * @param[in] payload Message payload
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t esphome_ble_proxy_handle_gatt_notify_request(
    uint8_t client_id, const uint8_t *payload, size_t len);

/**
 * @brief Handle Unsubscribe BLE Advertisements request (MSG 87)
 *
 * @param[in] client_id Client identifier
 * @return ESP_OK on success
 */
esp_err_t esphome_ble_proxy_handle_unsubscribe_advertisements(uint8_t client_id);

/**
 * @brief Handle BLE Scanner Set Mode request (MSG 127)
 *
 * @param[in] client_id Client identifier
 * @param[in] payload Message payload
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t esphome_ble_proxy_handle_scanner_set_mode(
    uint8_t client_id, const uint8_t *payload, size_t len);

/**
 * @brief Send BLE Scanner State response (MSG 126)
 *
 * Queries the actual BLE scanner state and sends it to the specified client.
 *
 * @param[in] client_id Client identifier
 * @return ESP_OK on success
 */
esp_err_t esphome_ble_proxy_send_scanner_state(uint8_t client_id);

/**
 * @brief Broadcast BLE Scanner State to all subscribed clients (MSG 126)
 *
 * Sends the current scanner state to all clients that are subscribed
 * to BLE advertisements. Called automatically when scanner state changes.
 */
void esphome_ble_proxy_broadcast_scanner_state(void);

/* ============================================================================
 * Client Cleanup
 * ============================================================================ */

/**
 * @brief Cleanup when client disconnects
 *
 * Unsubscribes the client from advertisements and cleans up any
 * GATT connections owned by this client.
 *
 * @param[in] client_id Client identifier
 */
void esphome_ble_proxy_client_disconnected(uint8_t client_id);

/* ============================================================================
 * Connection Management
 * ============================================================================ */

/**
 * @brief Get number of free GATT connection slots
 *
 * @return Number of available connection slots
 */
uint8_t esphome_ble_proxy_get_free_connections(void);

/**
 * @brief Check if device is connected
 *
 * @param[in] address Device address as uint64
 * @return true if device is connected
 */
bool esphome_ble_proxy_is_device_connected(uint64_t address);

/* ============================================================================
 * Address Conversion Utilities
 * ============================================================================ */

/**
 * @brief Convert 6-byte MAC address to uint64
 *
 * @param[in] mac 6-byte MAC address
 * @return uint64 representation (little-endian)
 */
static inline uint64_t esphome_mac_to_uint64(const uint8_t *mac)
{
    uint64_t result = 0;
    for (int i = 0; i < 6; i++) {
        result |= ((uint64_t)mac[i]) << (i * 8);
    }
    return result;
}

/**
 * @brief Convert uint64 address to 6-byte MAC
 *
 * @param[in] address uint64 address
 * @param[out] mac 6-byte MAC address output
 */
static inline void esphome_uint64_to_mac(uint64_t address, uint8_t *mac)
{
    for (int i = 0; i < 6; i++) {
        mac[i] = (address >> (i * 8)) & 0xFF;
    }
}

/* ============================================================================
 * Testing
 * ============================================================================ */

/**
 * @brief Run BLE Proxy self-tests
 *
 * @return ESP_OK if all tests pass
 */
esp_err_t esphome_ble_proxy_test(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_BLE_PROXY_H */
