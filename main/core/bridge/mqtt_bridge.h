/**
 * @file mqtt_bridge.h
 * @brief MQTT Bridge Core - Zigbee to MQTT Translation Layer
 *
 * Main integration layer that connects Zigbee coordinator events
 * with MQTT messaging. Coordinates all bridge sub-modules.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bridge statistics structure
 */
typedef struct {
    bool enabled;                   /**< Bridge enabled status */
    uint32_t publish_count;         /**< Total messages published */
    uint32_t command_count;         /**< Total commands processed */
    uint32_t error_count;           /**< Total errors encountered */
    uint32_t device_join_count;     /**< Total devices joined */
    uint32_t device_leave_count;    /**< Total devices left */
} bridge_stats_t;

/**
 * @brief Initialize MQTT bridge
 *
 * Initializes all bridge sub-modules:
 * - Device state publisher
 * - Command handler
 * - Home Assistant discovery
 * - Bridge request handler
 * - Log bridge (optional)
 *
 * Must be called after both Zigbee coordinator and MQTT client are initialized.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if prerequisites not met
 */
esp_err_t mqtt_bridge_init(void);

/**
 * @brief Start MQTT bridge
 *
 * Registers MQTT message callback and subscribes to command topics.
 * Publishes initial bridge state and device list.
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_bridge_start(void);

/**
 * @brief Stop MQTT bridge
 *
 * Publishes offline state and unsubscribes from topics.
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_bridge_stop(void);

/**
 * @brief Re-subscribe to MQTT topics after reconnect
 *
 * Called when MQTT connection is re-established to restore subscriptions.
 * Also republishes bridge state.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if bridge not initialized/enabled
 */
esp_err_t mqtt_bridge_resubscribe(void);

/**
 * @brief Publish device state to MQTT
 *
 * Publishes current state of a Zigbee device. Thread-safe.
 *
 * @param[in] short_addr Device short address
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 */
esp_err_t mqtt_bridge_publish_device_state(uint16_t short_addr);

/**
 * @brief Publish bridge state
 *
 * Publishes bridge status to "zigbee2mqtt/bridge/state"
 *
 * @param[in] state State string ("online", "offline")
 * @return ESP_OK on success
 */
esp_err_t mqtt_bridge_publish_bridge_state(const char *state);

/**
 * @brief Publish bridge info
 *
 * Publishes bridge/coordinator information to "zigbee2mqtt/bridge/info"
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_bridge_publish_bridge_info(void);

/**
 * @brief Publish device list
 *
 * Publishes all registered devices to "zigbee2mqtt/bridge/devices"
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_bridge_publish_device_list(void);

/* ============================================================================
 * Internal API Notice
 *
 * The following internal callback functions are declared in
 * core/mqtt_bridge_internal.h and should only be used by the zigbee module:
 *
 * - mqtt_bridge_handle_command() - MQTT message routing
 * - mqtt_bridge_on_device_join() - Device join event handler
 * - mqtt_bridge_on_device_leave() - Device leave event handler
 * - mqtt_bridge_on_attribute_change() - Attribute change event handler
 * ============================================================================ */

/**
 * @brief Get bridge statistics
 *
 * Returns bridge operation statistics.
 *
 * @return Bridge statistics structure
 */
bridge_stats_t mqtt_bridge_get_stats(void);

/**
 * @brief Check if bridge is enabled
 *
 * @return true if bridge is enabled and running
 */
bool mqtt_bridge_is_enabled(void);

/**
 * @brief Check if initial discovery has completed
 *
 * Used to determine if reconnect should republish discovery.
 *
 * @return true if initial discovery has been published
 */
bool mqtt_bridge_is_discovery_complete(void);

/**
 * @brief Test MQTT bridge
 *
 * Performs basic bridge functionality tests.
 *
 * @return ESP_OK if tests pass, ESP_FAIL otherwise
 */
esp_err_t mqtt_bridge_test(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_BRIDGE_H */
