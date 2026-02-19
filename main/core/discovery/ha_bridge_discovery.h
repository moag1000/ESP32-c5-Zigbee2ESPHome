/**
 * @file ha_bridge_discovery.h
 * @brief Home Assistant Bridge Discovery for Zigbee2MQTT Gateway
 *
 * Publishes Home Assistant MQTT discovery messages for the bridge device itself,
 * exposing diagnostic sensors (heap, uptime, WiFi RSSI) and control entities
 * (permit join switch, log level select, restart button).
 *
 * All sensors read from: zigbee2mqtt/bridge/state
 * Control entities publish to: zigbee2mqtt/bridge/request/[action]
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef HA_BRIDGE_DISCOVERY_H
#define HA_BRIDGE_DISCOVERY_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Bridge Discovery Constants
 * ============================================================================ */

/** @brief Bridge device name for Home Assistant */
#define HA_BRIDGE_DEVICE_NAME           "Zigbee2MQTT Bridge"

/** @brief Bridge device model for Home Assistant */
#define HA_BRIDGE_DEVICE_MODEL          "ESP32-C5 Gateway"

/** @brief Bridge device manufacturer for Home Assistant */
#define HA_BRIDGE_DEVICE_MANUFACTURER   "ESP32-C5 Zigbee2MQTT"

/** @brief Bridge device identifier prefix */
#define HA_BRIDGE_DEVICE_ID             "zigbee2mqtt_bridge"

/** @brief Bridge state topic */
#define HA_BRIDGE_STATE_TOPIC           "zigbee2mqtt/bridge/state"

/** @brief Bridge availability topic */
#define HA_BRIDGE_AVAILABILITY_TOPIC    "zigbee2mqtt/bridge/state"

/** @brief State publish interval in milliseconds */
#define HA_BRIDGE_STATE_PUBLISH_INTERVAL_MS     30000

/* ============================================================================
 * Bridge State Structure
 * ============================================================================ */

/**
 * @brief Bridge state structure for JSON serialization
 *
 * Contains all state values published to zigbee2mqtt/bridge/state
 */
typedef struct {
    const char *state;          /**< Bridge state: "online" or "offline" */
    uint16_t device_count;      /**< Number of paired Zigbee devices */
    uint8_t channel;            /**< Current Zigbee channel (11-26) */
    uint16_t pan_id;            /**< Network PAN ID */
    const char *coordinator_state; /**< Coordinator state string */
    bool permit_join;           /**< Permit join enabled flag */
    uint8_t permit_join_timeout;/**< Remaining permit join time (seconds) */
    const char *log_level;      /**< Current log level string */
    uint32_t free_heap_kb;      /**< Free heap memory in KB */
    uint32_t uptime_sec;        /**< System uptime in seconds */
    int8_t wifi_rssi;           /**< WiFi signal strength (dBm) */
    char wifi_ip[16];           /**< WiFi IP address string */
    uint8_t wifi_signal_quality;/**< WiFi signal quality (0-100%) */
    char wifi_band[8];          /**< WiFi frequency band ("2.4GHz" or "5GHz") */
    char wifi_hostname[32];     /**< mDNS hostname */
    uint32_t wifi_uptime_sec;   /**< WiFi connection uptime in seconds */
    uint32_t wifi_connect_count;/**< Total WiFi connection count */
    uint32_t wifi_disconnect_count; /**< Total WiFi disconnection count */
    bool mqtt_connected;        /**< MQTT connection status */
    uint32_t mqtt_latency_ms;   /**< MQTT broker round-trip latency in ms */
    bool time_synced;           /**< NTP time synchronized flag */
    char current_time[24];      /**< Current time in ISO 8601 format or "not_synced" */
    /* Crash reporter fields */
    const char *last_reset_reason;  /**< Last reset reason string */
    bool last_reset_was_crash;      /**< True if last reset was a crash */
    uint32_t boot_count;            /**< Total boot count since NVS cleared */
    /* BLE status fields */
#if CONFIG_BT_SCANNER_ENABLED
    const char *ble_status;             /**< BLE scanner status: "scanning" or "stopped" */
    uint16_t ble_device_count;          /**< Number of tracked BLE devices */
    bool ble_active_scan;               /**< Active scanning enabled flag */
#endif
    /* ESPHome status fields */
#if CONFIG_ESPHOME_API_ENABLE
    const char *esphome_status;         /**< ESPHome API status: "running" or "stopped" */
    uint8_t esphome_clients;            /**< Connected ESPHome client count */
    uint32_t ble_proxy_adverts;         /**< BLE proxy advertisements forwarded */
    uint8_t ble_proxy_connections;      /**< Active BLE proxy connections */
#endif
} ha_bridge_state_t;

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Initialize Home Assistant bridge discovery
 *
 * Must be called before publishing bridge discovery messages.
 * Initializes internal state and prepares discovery configurations.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t ha_bridge_discovery_init(void);

/**
 * @brief Publish all bridge discovery messages
 *
 * Publishes discovery configurations for all bridge entities:
 * - Diagnostic sensors (device_count, channel, pan_id, coordinator_state,
 *   free_heap, uptime, wifi_rssi, wifi_ip, current_time)
 * - Binary sensors (mqtt_connected, time_synced)
 * - Control entities (permit_join switch, log_level select, restart button)
 *
 * Should be called after MQTT connection is established and after
 * Home Assistant restarts (birth message received).
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 * @return ESP_FAIL if JSON creation or publish fails
 */
esp_err_t ha_bridge_discovery_publish_all(void);

/**
 * @brief Remove all bridge discovery messages
 *
 * Publishes empty payloads to remove all bridge entities from Home Assistant.
 * Used when disabling discovery or during shutdown.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t ha_bridge_discovery_remove(void);

/**
 * @brief Publish current bridge state
 *
 * Gathers current system state from various subsystems and publishes
 * a JSON state message to zigbee2mqtt/bridge/state.
 *
 * State JSON format:
 * @code
 * {
 *   "state": "online",
 *   "device_count": 5,
 *   "channel": 11,
 *   "pan_id": "0x1A62",
 *   "coordinator_state": "running",
 *   "permit_join": false,
 *   "permit_join_timeout": 0,
 *   "log_level": "info",
 *   "free_heap_kb": 52,
 *   "uptime_sec": 36000,
 *   "wifi_rssi": -61,
 *   "wifi_ip": "192.168.2.25",
 *   "wifi_signal_quality": 78,
 *   "wifi_band": "5GHz",
 *   "wifi_hostname": "zigbee-gateway",
 *   "wifi_uptime_sec": 35000,
 *   "wifi_connect_count": 3,
 *   "wifi_disconnect_count": 2,
 *   "mqtt_connected": true,
 *   "mqtt_latency_ms": 15,
 *   "time_synced": true,
 *   "current_time": "2026-01-25T14:30:00"
 * }
 * @endcode
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 * @return ESP_ERR_NO_MEM if JSON creation fails
 */
esp_err_t ha_bridge_publish_state(void);

/**
 * @brief Get current bridge state
 *
 * Populates a bridge state structure with current values from
 * all subsystems. Useful for external modules that need state info.
 *
 * @param[out] state Pointer to state structure to populate
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if state is NULL
 */
esp_err_t ha_bridge_get_state(ha_bridge_state_t *state);

/**
 * @brief Set bridge log level
 *
 * Changes the current log level and updates the bridge state.
 * Valid levels: "error", "warn", "info", "debug"
 *
 * @param[in] level Log level string
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if level is NULL or invalid
 */
esp_err_t ha_bridge_set_log_level(const char *level);

/**
 * @brief Get current log level string
 *
 * @return Current log level string (static, do not free)
 */
const char* ha_bridge_get_log_level(void);

/**
 * @brief Check if bridge discovery is initialized
 *
 * @return true if initialized, false otherwise
 */
bool ha_bridge_discovery_is_initialized(void);

/**
 * @brief Test bridge discovery functionality
 *
 * Creates and validates bridge discovery messages without publishing.
 * For debugging and verification purposes.
 *
 * @return ESP_OK if tests pass, ESP_FAIL otherwise
 */
esp_err_t ha_bridge_discovery_test(void);

#ifdef __cplusplus
}
#endif

#endif /* HA_BRIDGE_DISCOVERY_H */
