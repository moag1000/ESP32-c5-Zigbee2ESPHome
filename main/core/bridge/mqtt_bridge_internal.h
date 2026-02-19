/**
 * @file mqtt_bridge_internal.h
 * @brief Internal API for MQTT Bridge - DO NOT USE OUTSIDE CORE/ZIGBEE MODULES
 *
 * This header contains internal callback functions that are used by the
 * zigbee module to notify the MQTT bridge of device events. These functions
 * are not part of the public API and should not be called by external modules.
 *
 * For public bridge API, use mqtt_bridge.h instead.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MQTT_BRIDGE_INTERNAL_H
#define MQTT_BRIDGE_INTERNAL_H

#include "mqtt_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Zigbee Event Callbacks
 *
 * These functions are called by zb_callbacks.c to notify the MQTT bridge
 * of device events. They handle publishing device state changes to MQTT
 * and triggering Home Assistant discovery.
 * ============================================================================ */

/**
 * @internal
 * @brief Handle Zigbee device join event
 *
 * Called by zb_callbacks.c when a new Zigbee device joins the network.
 * Publishes availability and triggers Home Assistant discovery.
 *
 * @param[in] short_addr Device short address
 * @return ESP_OK on success
 */
esp_err_t mqtt_bridge_on_device_join(uint16_t short_addr);

/**
 * @internal
 * @brief Handle Zigbee device leave event
 *
 * Called by zb_callbacks.c when a Zigbee device leaves the network.
 * Publishes offline availability.
 *
 * @param[in] short_addr Device short address
 * @return ESP_OK on success
 */
esp_err_t mqtt_bridge_on_device_leave(uint16_t short_addr);

/**
 * @internal
 * @brief Handle Zigbee attribute change event
 *
 * Called by zb_callbacks.c when a Zigbee device attribute changes.
 * Publishes updated state to MQTT.
 *
 * @param[in] short_addr Device short address
 * @param[in] cluster_id Cluster ID
 * @param[in] attr_id Attribute ID
 * @param[in] value Attribute value
 * @param[in] value_len Value length
 * @return ESP_OK on success
 */
esp_err_t mqtt_bridge_on_attribute_change(uint16_t short_addr,
                                          uint16_t cluster_id,
                                          uint16_t attr_id,
                                          const void *value,
                                          size_t value_len);

/* ============================================================================
 * MQTT Message Handling
 *
 * Internal callback for processing incoming MQTT messages. Called by the
 * MQTT client when messages are received on subscribed topics.
 * ============================================================================ */

/**
 * @internal
 * @brief Handle MQTT command message
 *
 * Internal callback for MQTT message handling. Routes messages to
 * appropriate handlers (commands, bridge requests, etc.)
 *
 * @param[in] topic MQTT topic
 * @param[in] payload Message payload
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t mqtt_bridge_handle_command(const char *topic, const char *payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_BRIDGE_INTERNAL_H */
