/**
 * @file mqtt_adapter.h
 * @brief MQTT Adapter - Event Bus to MQTT Bridge
 *
 * Provides integration between the event bus and MQTT client. Subscribes to
 * internal events and publishes device states/availability to MQTT topics.
 * Also handles incoming MQTT commands and converts them to internal events.
 *
 * This adapter acts as a bridge layer:
 *   - EVT_DEVICE_STATE_CHANGED -> Publish to zigbee2mqtt/<friendly_name>
 *   - EVT_DEVICE_JOINED -> Publish availability online
 *   - EVT_DEVICE_LEFT -> Publish availability offline
 *   - EVT_MQTT_CONNECTED -> Republish all device states
 *   - MQTT commands -> Internal command events
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MQTT_ADAPTER_H
#define MQTT_ADAPTER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the MQTT adapter
 *
 * Subscribes to relevant event bus events:
 *   - EVT_DEVICE_STATE_CHANGED
 *   - EVT_DEVICE_JOINED
 *   - EVT_DEVICE_LEFT
 *   - EVT_MQTT_CONNECTED
 *   - EVT_MQTT_DISCONNECTED
 *
 * Must be called after event_bus_init() and mqtt_client_init().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized or prerequisites not met
 *      - ESP_ERR_NO_MEM if subscription fails
 */
esp_err_t mqtt_adapter_init(void);

/**
 * @brief Deinitialize the MQTT adapter
 *
 * Unsubscribes from all event bus events and cleans up resources.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t mqtt_adapter_deinit(void);

/**
 * @brief Notify adapter that MQTT is connected
 *
 * Called by the MQTT client connection callback when connected to broker.
 * Triggers republishing of all device states and availability.
 *
 * @note This function is safe to call from the MQTT event handler context.
 */
void mqtt_adapter_on_connected(void);

/**
 * @brief Notify adapter that MQTT is disconnected
 *
 * Called by the MQTT client connection callback when disconnected.
 * Updates internal state to prevent publishing attempts.
 *
 * @note This function is safe to call from the MQTT event handler context.
 */
void mqtt_adapter_on_disconnected(void);

/**
 * @brief Check if the MQTT adapter is initialized
 *
 * @return true if initialized, false otherwise
 */
bool mqtt_adapter_is_initialized(void);

/**
 * @brief Check if MQTT is currently connected (from adapter's perspective)
 *
 * @return true if connected, false otherwise
 */
bool mqtt_adapter_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_ADAPTER_H */
