/**
 * @file bridge_request_handler.h
 * @brief Bridge Request Handler for Control Commands
 *
 * Handles bridge control requests received via MQTT topics like:
 * - zigbee2mqtt/bridge/request/permit_join
 * - zigbee2mqtt/bridge/request/device/remove
 * - zigbee2mqtt/bridge/request/device/rename
 * - zigbee2mqtt/bridge/request/health_check
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BRIDGE_REQUEST_HANDLER_H
#define BRIDGE_REQUEST_HANDLER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "utils/json_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize bridge request handler
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_request_handler_init(void);

/**
 * @brief Process bridge request
 *
 * Main entry point for handling bridge MQTT requests.
 *
 * @param[in] topic MQTT topic
 * @param[in] payload JSON payload
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t bridge_request_process(const char *topic, const char *payload, size_t len);

/**
 * @brief Handle permit join request
 *
 * Opens/closes Zigbee network for device joining.
 *
 * @param[in] duration Join duration in seconds (0=close, 254=open, 255=forever)
 * @return ESP_OK on success
 */
esp_err_t bridge_request_permit_join(uint8_t duration);

/**
 * @brief Handle permit join request with extended options
 *
 * Opens/closes Zigbee network for device joining with time and target device support.
 * Supports payloads:
 * - {"value": true}                           Enable with default 254s
 * - {"value": true, "time": 60}               Enable for 60 seconds
 * - {"value": true, "device": "0x00124b..."}  Enable for specific device only
 * - {"value": false}                          Disable permit join
 *
 * @param[in] options Parsed permit join options
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if options is NULL
 * @return ESP_ERR_INVALID_STATE if coordinator not running
 */
esp_err_t bridge_request_permit_join_extended(const json_permit_join_options_t *options);

/**
 * @brief Handle device remove request
 *
 * Removes device from Zigbee network.
 *
 * @param[in] friendly_name Device friendly name
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 */
esp_err_t bridge_request_device_remove(const char *friendly_name);

/**
 * @brief Handle device rename request
 *
 * Renames a device (changes friendly name).
 *
 * @param[in] old_name Current friendly name
 * @param[in] new_name New friendly name
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 */
esp_err_t bridge_request_device_rename(const char *old_name, const char *new_name);

/**
 * @brief Handle health check request
 *
 * Publishes system health information.
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_request_health_check(void);

/**
 * @brief Handle restart request
 *
 * Restarts the ESP32.
 *
 * @return Does not return
 */
esp_err_t bridge_request_restart(void);

/**
 * @brief Handle factory reset request
 *
 * Erases ALL NVS data (WiFi, Zigbee devices, config) and restarts the gateway.
 * WARNING: This will erase all settings including WiFi credentials!
 *
 * @return Does not return
 */
esp_err_t bridge_request_factory_reset(void);

/**
 * @brief Wipe the Zigbee network, keeping Wi-Fi and gateway settings
 *
 * Erases every Zigbee NVS namespace and the Zigbee storage partitions, then
 * restarts. Wi-Fi credentials and gateway configuration survive.
 *
 * @return does not return on success
 */
esp_err_t bridge_request_network_reset(void);

/**
 * @brief Erase Wi-Fi and gateway settings, keeping the Zigbee network
 *
 * The counterpart to bridge_request_network_reset(): paired devices stay,
 * the device forgets how to reach the network it was on.
 *
 * @return does not return on success
 */
esp_err_t bridge_request_config_reset(void);

/**
 * @brief Handle network map request
 *
 * Generates and publishes a network topology map with all devices.
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_request_networkmap(void);

/**
 * @brief Handle configuration get request
 *
 * Returns current gateway configuration via MQTT.
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_request_config_get(void);

/**
 * @brief Handle configuration set request
 *
 * Updates gateway configuration from JSON payload.
 *
 * @param[in] payload JSON configuration payload
 * @return ESP_OK on success
 */
esp_err_t bridge_request_config_set(const char *payload);

/**
 * @brief Handle OTA update check request
 *
 * Checks for available firmware updates.
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_request_ota_check(void);

/**
 * @brief Handle OTA update install request
 *
 * Starts firmware update installation.
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_request_ota_install(void);

/**
 * @brief Handle device availability check request
 *
 * Triggers an immediate availability check for a specific device.
 * Topic: zigbee2mqtt/bridge/request/device/availability/check
 * Payload: {"id": "friendly_name"}
 *
 * @param[in] payload JSON payload with device ID
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_STATE if check already pending
 */
esp_err_t bridge_request_availability_check(const char *payload);

/**
 * @brief Device options structure
 *
 * Per-device options stored in NVS for Zigbee2MQTT compatibility.
 */
typedef struct {
    bool retain;                    /**< MQTT retain flag for device messages */
    uint8_t qos;                    /**< MQTT QoS level (0-2) */
    uint16_t debounce;              /**< Debounce time in ms for state changes */
    uint16_t debounce_ignore;       /**< Ignore duplicate messages within time window */
    char filtered_attributes[256];  /**< Comma-separated list of filtered attributes */
    bool optimistic;                /**< Enable optimistic mode */
    bool force_enable_state;        /**< Force enable state reporting */
} device_options_t;

/**
 * @brief Default device options
 */
#define DEVICE_OPTIONS_DEFAULT { \
    .retain = false, \
    .qos = 0, \
    .debounce = 0, \
    .debounce_ignore = 0, \
    .filtered_attributes = {0}, \
    .optimistic = false, \
    .force_enable_state = false \
}

/**
 * @brief Handle device configure request
 *
 * Triggers re-configuration of a device (reporting, bindings).
 * Topic: zigbee2mqtt/bridge/request/device/configure
 * Payload: {"id": "friendly_name"}
 *
 * @param[in] payload JSON payload with device ID
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 */
esp_err_t bridge_request_device_configure(const char *payload);

/**
 * @brief Handle device options request
 *
 * Sets device-specific options (retain, qos, debounce, etc.).
 * Topic: zigbee2mqtt/bridge/request/device/options
 * Payload: {"id": "friendly_name", "options": {...}}
 *
 * @param[in] payload JSON payload with device ID and options
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_ARG if options invalid
 */
esp_err_t bridge_request_device_options(const char *payload);

/**
 * @brief Handle device get request
 *
 * Reads specific attributes from a device via ZCL Read Attribute.
 * Topic: zigbee2mqtt/bridge/request/device/get
 * Payload: {"id": "friendly_name", "cluster": "genOnOff", "attributes": ["onOff"]}
 *
 * @param[in] payload JSON payload with device ID, cluster, and attributes
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_ARG if cluster/attributes invalid
 */
esp_err_t bridge_request_device_get(const char *payload);

/**
 * @brief Handle device remove request with force option
 *
 * Removes device from Zigbee network with optional force flag.
 * With force=true, removes from registry without Leave request.
 *
 * @param[in] friendly_name Device friendly name
 * @param[in] force If true, skip Leave request and force remove
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 */
esp_err_t bridge_request_device_remove_force(const char *friendly_name, bool force);

/**
 * @brief Get device options from NVS
 *
 * Retrieves stored device options from NVS.
 *
 * @param[in] ieee_addr Device IEEE address (as 64-bit value)
 * @param[out] options Output options structure
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if no options stored
 * @return ESP_ERR_NVS_NOT_FOUND if NVS key not found
 */
esp_err_t device_options_get(uint64_t ieee_addr, device_options_t *options);

/**
 * @brief Set device options in NVS
 *
 * Stores device options to NVS.
 *
 * @param[in] ieee_addr Device IEEE address (as 64-bit value)
 * @param[in] options Options to store
 * @return ESP_OK on success
 * @return ESP_FAIL on NVS error
 */
esp_err_t device_options_set(uint64_t ieee_addr, const device_options_t *options);

/**
 * @brief Remove device options from NVS
 *
 * Removes stored device options from NVS.
 *
 * @param[in] ieee_addr Device IEEE address (as 64-bit value)
 * @return ESP_OK on success
 */
esp_err_t device_options_remove(uint64_t ieee_addr);

/* ============================================================================
 * Install Code Management Commands
 * ============================================================================ */

/**
 * @brief Handle install code add request
 *
 * Adds an install code for secure device joining.
 * Topic: zigbee2mqtt/bridge/request/install_code/add
 * Payload: {"ieee_address": "0x00124b001234abcd", "install_code": "83FED3407A939723A5C639B26916D505C3B5"}
 *
 * @param[in] payload JSON payload
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if payload is invalid
 * @return ESP_ERR_INVALID_CRC if install code CRC validation fails
 */
esp_err_t bridge_request_install_code_add(const char *payload);

/**
 * @brief Handle install code remove request
 *
 * Removes an install code for a device.
 * Topic: zigbee2mqtt/bridge/request/install_code/remove
 * Payload: {"ieee_address": "0x00124b001234abcd"}
 *
 * @param[in] payload JSON payload
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if no install code exists for device
 */
esp_err_t bridge_request_install_code_remove(const char *payload);

/**
 * @brief Handle install code list request
 *
 * Lists all configured install codes.
 * Topic: zigbee2mqtt/bridge/request/install_code/list
 * Payload: {} (empty object or any)
 *
 * @param[in] payload JSON payload (ignored)
 * @return ESP_OK on success
 */
esp_err_t bridge_request_install_code_list(const char *payload);

/* ============================================================================
 * Coordinator Settings and Info Commands (ZG-012)
 * ============================================================================ */

/**
 * @brief Handle coordinator health check request
 *
 * Performs coordinator health check and returns status.
 * Topic: zigbee2mqtt/bridge/request/coordinator/check
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_request_coordinator_check(void);

/**
 * @brief Handle coordinator version request
 *
 * Returns coordinator version and meta information.
 * Topic: zigbee2mqtt/bridge/request/coordinator/version
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_request_coordinator_version(void);

/**
 * @brief Handle coordinator settings request
 *
 * Returns coordinator settings (network config, PAN ID, channel, etc.).
 * Topic: zigbee2mqtt/bridge/request/coordinator/settings
 *
 * @param[in] payload JSON payload (currently unused)
 * @return ESP_OK on success
 */
esp_err_t bridge_request_coordinator_settings(const char *payload);

/* ============================================================================
 * Network Options and Channel Change (ZG-016)
 * ============================================================================ */

/**
 * @brief Handle network options request
 *
 * Handles network-wide options including channel change.
 * Topic: zigbee2mqtt/bridge/request/options
 * Payload: {"channel": 20}  - Change to channel 20
 * Payload: {}               - Get current network options
 *
 * WARNING: Channel change is disruptive and may cause temporary network outage.
 * All devices must support the new channel.
 *
 * @param[in] payload JSON payload with optional channel number
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if channel is invalid (must be 11-26)
 * @return ESP_ERR_INVALID_STATE if channel change already in progress
 */
esp_err_t bridge_request_network_options(const char *payload);

/* ============================================================================
 * Extended PAN ID Management (API-004)
 * ============================================================================ */

/**
 * @brief Handle network extended PAN ID request
 *
 * Gets or sets the network Extended PAN ID using ESP-Zigbee-SDK v1.6.8 APIs.
 * Topic: zigbee2mqtt/bridge/request/network/extended_pan_id
 *
 * GET: Empty payload or {} returns current Extended PAN ID
 * SET: {"value": "0x0123456789ABCDEF"}
 * SET from MAC: {"use_mac": true}
 *
 * Response includes: extended_pan_id, configured status, network info.
 *
 * WARNING: Changing Extended PAN ID on an active network will cause
 * all devices to lose connectivity and require rejoining.
 *
 * @param[in] payload JSON payload (empty for GET, value/use_mac for SET)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if network manager not initialized
 * @return ESP_ERR_INVALID_ARG if Extended PAN ID format is invalid
 */
esp_err_t bridge_request_network_extended_pan_id(const char *payload);

/* ============================================================================
 * Network Key Rotation (API-006)
 * ============================================================================ */

/**
 * @brief Handle network key rotation request
 *
 * Initiates a network-wide key rotation using ESP-Zigbee-SDK v1.6.8 API
 * esp_zb_secur_broadcast_network_key_switch().
 *
 * Topic: zigbee2mqtt/bridge/request/network/key_rotate
 * Payload: {} (empty object) or {"transaction": "optional-id"}
 *
 * Response format:
 * {
 *   "status": "ok",
 *   "data": {
 *     "key_rotation": "initiated",
 *     "sequence": 5,
 *     "device_count": 10,
 *     "channel": 11
 *   }
 * }
 *
 * The sequence number is incremented and stored in NVS with each rotation.
 *
 * WARNING: Key rotation is a sensitive security operation. All devices
 * must successfully receive the new key or they will lose network access.
 *
 * @param[in] payload JSON payload (optional, only for transaction ID)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if network not formed or not initialized
 * @return ESP_FAIL if key broadcast fails
 */
esp_err_t bridge_request_network_key_rotate(const char *payload);

/* ============================================================================
 * Time Server Commands
 * ============================================================================ */

/**
 * @brief Handle time get request
 *
 * Returns current time server state.
 * Topic: zigbee2mqtt/bridge/request/time
 *
 * Response includes: unix timestamp, zigbee time, synchronization status,
 *                    UTC string, timezone offset, DST status.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if time server not initialized
 */
esp_err_t bridge_request_time_get(void);

/**
 * @brief Handle time set request
 *
 * Sets the current time on the time server.
 * Topic: zigbee2mqtt/bridge/request/time/set
 * Payload: {"time": 1737734400}  (Unix timestamp)
 *
 * @param[in] payload JSON payload with unix timestamp
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if time server not initialized
 * @return ESP_ERR_INVALID_ARG if time value is invalid
 */
esp_err_t bridge_request_time_set(const char *payload);

/**
 * @brief Handle time config request
 *
 * Gets or sets time server configuration (timezone, DST settings).
 * Topic: zigbee2mqtt/bridge/request/time/config
 *
 * GET: Empty payload or {} returns current config
 * SET: Payload with config values to update:
 *   {"timezone_offset": 3600}           (UTC+1 in seconds)
 *   {"dst_shift": 3600, "dst_start": x, "dst_end": y}
 *   {"is_master": true, "is_synchronized": true}
 *
 * @param[in] payload JSON payload (empty for GET, config for SET)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if time server not initialized
 */
esp_err_t bridge_request_time_config(const char *payload);

/* ============================================================================
 * Network Backup and Restore Commands
 * ============================================================================ */

/**
 * @brief Handle network backup request
 *
 * Creates a full or partial network backup including coordinator state,
 * device database, groups, and bindings. The backup is stored to LittleFS
 * and published via MQTT.
 *
 * Topics:
 * - zigbee2mqtt/bridge/request/backup - Full backup
 * - zigbee2mqtt/bridge/request/backup/state - Coordinator state only
 * - zigbee2mqtt/bridge/request/backup/devices - Device database only
 *
 * Payload: {"transaction": "optional-id"} (optional)
 *
 * Response topic: zigbee2mqtt/bridge/response/backup
 * Response format: Zigbee2MQTT compatible JSON with coordinator, devices,
 *                  groups, and bindings arrays.
 *
 * @param[in] topic MQTT topic (determines backup type)
 * @param[in] payload JSON payload (optional transaction ID)
 * @param[in] len Payload length
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t bridge_request_backup(const char *topic, const char *payload, size_t len);

/**
 * @brief Handle network restore request
 *
 * Restores network configuration, device database, groups, and bindings
 * from a backup file. Network should be restarted after restore.
 *
 * Topic: zigbee2mqtt/bridge/request/restore
 * Payload: {"filename": "backup_20260101_120000.zbk", "transaction": "optional-id"}
 *          If filename is omitted, restores from most recent backup.
 *
 * Response topic: zigbee2mqtt/bridge/response/restore
 *
 * @param[in] payload JSON payload with optional filename
 * @param[in] len Payload length
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if backup file not found
 * @return ESP_ERR_INVALID_ARG if backup validation fails
 */
esp_err_t bridge_request_restore(const char *payload, size_t len);

/**
 * @brief Handle backup list request
 *
 * Returns list of available backup files with metadata.
 *
 * Topic: zigbee2mqtt/bridge/request/backup/list
 * Payload: {"transaction": "optional-id"} (optional)
 *
 * Response topic: zigbee2mqtt/bridge/response/backup/list
 * Response includes: filename, timestamp, device_count, group_count,
 *                    binding_count, file_size for each backup.
 *
 * @param[in] payload JSON payload (optional transaction ID)
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t bridge_request_backup_list(const char *payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_REQUEST_HANDLER_H */
