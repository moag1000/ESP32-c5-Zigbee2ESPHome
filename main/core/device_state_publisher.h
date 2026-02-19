/**
 * @file device_state_publisher.h
 * @brief Device State Publisher for Zigbee to MQTT Translation
 *
 * Publishes device states and availability to MQTT topics
 * compatible with Zigbee2MQTT and Home Assistant.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef DEVICE_STATE_PUBLISHER_H
#define DEVICE_STATE_PUBLISHER_H

#include <stdint.h>
#include "esp_err.h"
#include "core/device/unified_device.h"
#include "zigbee/zb_diagnostics.h"
#include "zigbee/zb_device_handler_types.h"
#include "zigbee/zb_cluster_electrical.h"
#include "zigbee/zb_cluster_multistate.h"
#include "zigbee/zb_cluster_security.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize device state publisher
 *
 * Must be called before publishing any device states.
 *
 * @return ESP_OK on success
 */
esp_err_t device_state_publisher_init(void);

/**
 * @brief Publish full device state to MQTT
 *
 * Publishes the complete state of a device to MQTT topic:
 * "zigbee2mqtt/[friendly_name]"
 *
 * Thread-safe and non-blocking (queues MQTT message internally).
 *
 * @param[in] device Pointer to unified device structure
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if device is NULL
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t device_state_publish(const device_t *device);

/**
 * @brief Publish device state by short address
 *
 * Convenience function that looks up device by short address and publishes state.
 *
 * @param[in] short_addr Device short address
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 */
esp_err_t device_state_publish_by_addr(uint16_t short_addr);

/**
 * @brief Publish device availability
 *
 * Publishes device availability status to MQTT topic:
 * "zigbee2mqtt/[friendly_name]/availability"
 *
 * @param[in] device Pointer to unified device structure
 * @param[in] available true for "online", false for "offline"
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if device is NULL
 */
esp_err_t device_state_publish_availability(const device_t *device, bool available);

/**
 * @brief Publish availability by short address
 *
 * Convenience function that looks up device by short address and publishes availability.
 *
 * @param[in] short_addr Device short address
 * @param[in] available true for "online", false for "offline"
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 */
esp_err_t device_state_publish_availability_by_addr(uint16_t short_addr, bool available);

/**
 * @brief Publish attribute update
 *
 * Publishes a specific attribute change to MQTT. This is more efficient
 * than publishing the full device state.
 *
 * @param[in] short_addr Device short address
 * @param[in] cluster_id Cluster ID
 * @param[in] attr_id Attribute ID
 * @param[in] value Attribute value
 * @param[in] value_len Value length in bytes
 * @return ESP_OK on success
 */
esp_err_t device_state_publish_attribute(uint16_t short_addr,
                                        uint16_t cluster_id,
                                        uint16_t attr_id,
                                        const void *value,
                                        size_t value_len);

/**
 * @brief Publish all devices availability
 *
 * Publishes availability status for all registered devices.
 * Useful during startup or bridge state changes.
 *
 * @param[in] available Availability status to set for all devices
 * @return ESP_OK on success
 */
esp_err_t device_state_publish_all_availability(bool available);

/**
 * @brief Build device state JSON string
 *
 * Creates a JSON string representation of the device state.
 * Caller must free the returned string.
 *
 * @param[in] device Pointer to unified device structure
 * @return JSON string (must be freed by caller) or NULL on error
 */
char* device_state_to_json(const device_t *device);

/**
 * @brief Publish door lock state
 *
 * Publishes door lock state to MQTT topic.
 * Format: {"state": "LOCK"} or {"state": "UNLOCK"}
 *
 * @param[in] short_addr Device short address
 * @param[in] locked true if locked, false if unlocked
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t device_state_publish_lock_state(uint16_t short_addr, bool locked);

/**
 * @brief Publish electrical measurement state
 *
 * Publishes electrical measurement data to MQTT topic.
 * Format: {"voltage": 230.5, "current": 0.52, "power": 120, "power_factor": 0.98}
 *
 * @param[in] short_addr Device short address
 * @param[in] state Pointer to electrical measurement state
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t device_state_publish_electrical(uint16_t short_addr,
                                           const zb_electrical_state_t *state);

/**
 * @brief Publish metering (energy) state
 *
 * Publishes energy metering data to MQTT topic.
 * Format: {"energy": 1234.56, "power": 150, "current_day_consumption": 5.2,
 *          "previous_day_consumption": 12.3, "energy_produced": 0}
 *
 * Compatible with Home Assistant energy dashboard.
 *
 * @param[in] short_addr Device short address
 * @param[in] state Pointer to metering state
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t device_state_publish_metering(uint16_t short_addr,
                                         const zb_metering_state_t *state);

/**
 * @brief Publish device temperature state
 *
 * Publishes internal device temperature from Device Temperature
 * Configuration Cluster (0x0002) to MQTT topic.
 * Format: {"device_temperature": {"current": 45.5, "min_experienced": 20.0,
 *          "max_experienced": 55.0, "valid": true}}
 *
 * Used for diagnostics monitoring of device health.
 *
 * @param[in] short_addr Device short address
 * @param[in] temp Pointer to device temperature data
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_ARG if temp is NULL
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t device_state_publish_device_temperature(uint16_t short_addr,
                                                   const zb_device_temperature_t *temp);

/**
 * @brief Publish battery/power state for device (API-007)
 *
 * Publishes battery and power information to MQTT topic.
 * Format: {"battery": 66, "battery_low": false, "power_source": "Battery"}
 *
 * Only publishes if the device has valid power descriptor information.
 *
 * @param[in] short_addr Device short address
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_STATE if MQTT not connected or power info not available
 */
esp_err_t device_state_publish_battery(uint16_t short_addr);

/**
 * @brief Publish multistate (input/output/value) state
 *
 * Publishes multistate cluster data to MQTT topic.
 * Format: {"state": 2, "states_count": 3, "type": "input",
 *          "out_of_service": false, "in_alarm": false, "fault": false}
 *
 * Use case: Multi-button switches (Aqara, IKEA), rotary switches,
 * multi-mode outputs.
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint (0 to use first match)
 * @param[in] state Pointer to multistate state
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_ARG if state is NULL
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t device_state_publish_multistate(uint16_t short_addr, uint8_t endpoint,
                                           const zb_multistate_state_t *state);

/**
 * @brief Publish binary output state
 *
 * Publishes binary output cluster data to MQTT topic.
 * Format: {"state": "ON", "out_of_service": false, "in_alarm": false,
 *          "fault": false, "overridden": false}
 *
 * Use case: Zigbee relays, actuators (Schaltaktoren).
 *
 * @param[in] short_addr Device short address
 * @param[in] state Pointer to binary output state
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_ARG if state is NULL
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t device_state_publish_binary_output(uint16_t short_addr,
                                              const zb_binary_output_state_t *state);

/**
 * @brief Publish binary value state
 *
 * Publishes binary value cluster data to MQTT topic.
 * Format: {"state": "ON", "out_of_service": false, "in_alarm": false,
 *          "fault": false, "overridden": false}
 *
 * Use case: Generic binary state storage.
 *
 * @param[in] short_addr Device short address
 * @param[in] state Pointer to binary value state
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found
 * @return ESP_ERR_INVALID_ARG if state is NULL
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t device_state_publish_binary_value(uint16_t short_addr,
                                             const zb_binary_value_state_t *state);

/**
 * @brief Publish Tuya Fingerbot state
 *
 * Publishes Tuya Fingerbot datapoint state to MQTT topic.
 * Format: {"state": "ON", "mode": "push", "down_movement": 100,
 *          "sustain_time": 3, "reverse": false, "battery": 66}
 *
 * @param[in] short_addr Device short address
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if device not found or no Fingerbot state available
 * @return ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t device_state_publish_fingerbot(uint16_t short_addr);

/**
 * @brief Test device state publisher
 *
 * Performs basic publishing tests with mock data.
 *
 * @return ESP_OK if tests pass, ESP_FAIL otherwise
 */
esp_err_t device_state_publisher_test(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_STATE_PUBLISHER_H */
