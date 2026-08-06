/**
 * @file esphome_api.h
 * @brief ESPHome Native API Server
 *
 * Implements a TCP server on port 6053 for Home Assistant integration
 * using the ESPHome Native API protocol.
 *
 * Features:
 * - TCP server with configurable port
 * - Password-based authentication (optional)
 * - mDNS service announcement for auto-discovery
 * - Entity state subscription and updates
 * - Maximum 1-2 concurrent client connections
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_API_H
#define ESPHOME_API_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esphome_common.h"
#include "esphome_entities.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration Structures
 * ============================================================================ */

/**
 * @brief ESPHome API server configuration
 */
typedef struct {
    uint16_t port;                                  /**< TCP server port (default: 6053) */
    char password[64];                              /**< API password (empty for no auth) */
    char device_name[ESPHOME_MAX_NAME_LEN];         /**< Device name for discovery */
    char friendly_name[ESPHOME_MAX_NAME_LEN];       /**< Friendly name for display */
    char mac_address[18];                           /**< MAC address (auto-filled if empty) */
    uint8_t max_clients;                            /**< Max concurrent clients (see ESPHOME_MAX_CLIENTS) */
    bool use_mdns;                                  /**< Enable mDNS announcement */
    uint32_t keepalive_ms;                          /**< Keepalive interval in ms */
} esphome_api_config_t;

/**
 * @brief Default configuration initializer
 */
#define ESPHOME_API_CONFIG_DEFAULT() { \
    .port = ESPHOME_API_PORT, \
    .password = "", \
    .device_name = "esp32c5_gateway", \
    .friendly_name = "ESP32-C5 Zigbee Gateway", \
    .mac_address = "", \
    .max_clients = ESPHOME_MAX_CLIENTS, \
    .use_mdns = true, \
    .keepalive_ms = ESPHOME_KEEPALIVE_INTERVAL * 1000, \
}

/* ============================================================================
 * Statistics Structure
 * ============================================================================ */

/**
 * @brief ESPHome API server statistics
 */
typedef struct {
    uint32_t total_connections;         /**< Total client connections */
    uint32_t active_connections;        /**< Current active connections */
    uint32_t messages_received;         /**< Total messages received */
    uint32_t messages_sent;             /**< Total messages sent */
    uint32_t authentication_failures;   /**< Failed authentication attempts */
    uint32_t protocol_errors;           /**< Protocol/parse errors */
    uint64_t bytes_received;            /**< Total bytes received */
    uint64_t bytes_sent;                /**< Total bytes sent */
    uint32_t uptime_seconds;            /**< Server uptime in seconds */
} esphome_api_stats_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * @brief Initialize ESPHome API server
 *
 * Initializes the API server with the given configuration.
 * Does not start the server - call esphome_api_start() after.
 *
 * @param[in] config Server configuration (NULL for defaults)
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if memory allocation fails
 * @return ESP_ERR_INVALID_STATE if already initialized
 */
esp_err_t esphome_api_init(const esphome_api_config_t *config);

/**
 * @brief Start ESPHome API server
 *
 * Starts the TCP server and mDNS announcement (if enabled).
 * Requires WiFi to be connected.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 * @return ESP_FAIL if server start fails
 */
esp_err_t esphome_api_start(void);

/**
 * @brief Stop ESPHome API server
 *
 * Stops the TCP server, disconnects all clients, and removes
 * mDNS announcement.
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_api_stop(void);

/**
 * @brief Deinitialize ESPHome API server
 *
 * Stops the server (if running) and frees all resources.
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_api_deinit(void);

/**
 * @brief Check if API server is running
 *
 * @return true if server is running and accepting connections
 */
bool esphome_api_is_running(void);

/**
 * @brief Get number of connected clients
 *
 * @return Number of currently connected clients
 */
uint8_t esphome_api_get_client_count(void);

/**
 * @brief Get server statistics
 *
 * @param[out] stats Statistics structure to fill
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if stats is NULL
 */
esp_err_t esphome_api_get_stats(esphome_api_stats_t *stats);

/**
 * @brief Reset server statistics
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_api_reset_stats(void);

/* ============================================================================
 * Client Management
 * ============================================================================ */

/**
 * @brief Disconnect a specific client
 *
 * @param[in] client_id Client identifier (0-based index)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if client_id is invalid
 */
esp_err_t esphome_api_disconnect_client(uint8_t client_id);

/**
 * @brief Disconnect all clients
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_api_disconnect_all_clients(void);

/* ============================================================================
 * State Broadcast Functions
 * ============================================================================ */

/**
 * @brief Broadcast entity state to all subscribed clients
 *
 * Sends state update to all connected clients that have
 * subscribed to state updates.
 *
 * @param[in] entity_type Type of entity
 * @param[in] key Entity key
 * @param[in] state Pointer to state structure (type-specific)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if state is NULL
 */
esp_err_t esphome_api_broadcast_state(esphome_entity_type_t entity_type,
                                       esphome_entity_key_t key, const void *state);

/**
 * @brief Broadcast all entity states
 *
 * Sends current state of all entities to all subscribed clients.
 * Useful after client subscription or reconnection.
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_api_broadcast_all_states(void);

/* ============================================================================
 * Device Information
 * ============================================================================ */

/**
 * @brief Set device information
 *
 * Updates device information sent in DeviceInfoResponse.
 *
 * @param[in] name Device name
 * @param[in] version Firmware version string
 * @param[in] model Device model string
 * @return ESP_OK on success
 */
esp_err_t esphome_api_set_device_info(const char *name, const char *version,
                                       const char *model);

/* ============================================================================
 * mDNS Functions
 * ============================================================================ */

/**
 * @brief Update mDNS service TXT records
 *
 * Updates the mDNS TXT records for the _esphomelib._tcp service.
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_api_update_mdns(void);

/**
 * @brief Remove mDNS service announcement
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_api_remove_mdns(void);

/* ============================================================================
 * Callback Registration
 * ============================================================================ */

/**
 * @brief Connection state callback type
 *
 * @param[in] client_id Client identifier
 * @param[in] connected true if connected, false if disconnected
 * @param[in] authenticated true if client is authenticated
 */
typedef void (*esphome_api_connection_cb_t)(uint8_t client_id, bool connected,
                                            bool authenticated);

/**
 * @brief Register connection callback
 *
 * @param[in] callback Callback function
 * @return ESP_OK on success
 */
esp_err_t esphome_api_register_connection_callback(esphome_api_connection_cb_t callback);

/* ============================================================================
 * Log Subscription (ES-005)
 * ============================================================================ */

/**
 * @brief Register log handler for streaming logs to subscribed clients
 *
 * Installs an ESP-IDF log hook to capture and forward logs to ESPHome API
 * clients that have subscribed to logs. Should be called after esphome_api_init().
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_api_register_log_handler(void);

/**
 * @brief Unregister log handler
 *
 * Removes the log hook and stops forwarding logs.
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_api_unregister_log_handler(void);

/**
 * @brief Send a log message to subscribed clients
 *
 * Manually sends a log message to all clients subscribed to logs.
 * This is called automatically by the log handler, but can also be
 * called directly if needed.
 *
 * @param[in] level Log level
 * @param[in] tag Log tag (component name)
 * @param[in] message Log message
 * @return ESP_OK on success
 */
esp_err_t esphome_api_send_log(esphome_log_level_t level, const char *tag,
                                const char *message);

/* ============================================================================
 * Home Assistant State Subscription
 * ============================================================================ */

/**
 * @brief HA state update callback type
 *
 * Called when Home Assistant sends a state update for a subscribed entity.
 *
 * @param[in] entity_id Home Assistant entity ID (e.g., "sensor.temperature")
 * @param[in] state Entity state value
 * @param[in] attribute Optional attribute name (may be NULL for state)
 */
typedef void (*esphome_ha_state_cb_t)(const char *entity_id, const char *state,
                                       const char *attribute);

/**
 * @brief Register Home Assistant state callback
 *
 * @param[in] callback Callback function to receive HA state updates
 * @return ESP_OK on success
 */
esp_err_t esphome_api_register_ha_state_callback(esphome_ha_state_cb_t callback);

/**
 * @brief Subscribe to Home Assistant entity state
 *
 * Requests state updates from Home Assistant for the specified entity.
 *
 * @param[in] entity_id Home Assistant entity ID (e.g., "sensor.temperature")
 * @return ESP_OK on success
 */
esp_err_t esphome_api_subscribe_ha_state(const char *entity_id);

/**
 * @brief Call Home Assistant service
 *
 * Sends a service call request to Home Assistant via connected clients.
 *
 * @param[in] domain Service domain (e.g., "light")
 * @param[in] service Service name (e.g., "turn_on")
 * @param[in] data_json Service data as JSON string (may be NULL)
 * @return ESP_OK on success
 */
esp_err_t esphome_api_call_ha_service(const char *domain, const char *service,
                                       const char *data_json);

/* ============================================================================
 * Internal API Notice
 *
 * The following internal functions are declared in esphome_api_internal.h
 * and should only be used within the esphome module (e.g., BLE Proxy):
 *
 * - esphome_api_send_to_client() - Send raw message to specific client
 * - esphome_api_get_client_id() - Get client ID from client context
 * - esphome_api_get_state() - Get global API server state
 * - esphome_api_send_message() - Send message to client with encryption
 * - And other internal helper functions
 * ============================================================================ */

/* ============================================================================
 * Testing and Diagnostics
 * ============================================================================ */

/**
 * @brief Run API server self-test
 *
 * Tests initialization, protocol handling, and entity integration.
 *
 * @return ESP_OK if all tests pass
 */
esp_err_t esphome_api_test(void);

/**
 * @brief Log server status and statistics
 *
 * Outputs detailed server status to serial console.
 */
void esphome_api_log_status(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_API_H */
