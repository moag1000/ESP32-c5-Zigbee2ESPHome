/**
 * @file mqtt_event_handler.h
 * @brief Centralized MQTT Event Handler for Zigbee Events
 *
 * This module subscribes to all Zigbee-related events from the Event Bus
 * and publishes them to MQTT. This decouples Zigbee modules from MQTT,
 * enabling:
 * - Centralized retry/throttle logic
 * - Event tracing for debugging
 * - Easy testing (mock events)
 * - Single point for MQTT topic management
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MQTT_EVENT_HANDLER_H
#define MQTT_EVENT_HANDLER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the MQTT event handler
 *
 * Subscribes to all Zigbee and system events that should be published
 * to MQTT. Must be called after event_bus_init() and mqtt_client_init().
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_event_handler_init(void);

/**
 * @brief Deinitialize the MQTT event handler
 *
 * Unsubscribes from all events.
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_event_handler_deinit(void);

/**
 * @brief Check if MQTT event handler is initialized
 *
 * @return true if initialized
 */
bool mqtt_event_handler_is_initialized(void);

/**
 * @brief Get event handler statistics
 *
 * @param[out] events_handled Total events handled
 * @param[out] publish_errors Total MQTT publish errors
 */
void mqtt_event_handler_get_stats(uint32_t *events_handled, uint32_t *publish_errors);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_EVENT_HANDLER_H */
