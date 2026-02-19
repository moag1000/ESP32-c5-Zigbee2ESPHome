/**
 * @file event_bus.h
 * @brief Event bus interface for ESP32-C5 Zigbee2MQTT NG
 *
 * Provides a centralized event-driven architecture with FreeRTOS queue-based
 * asynchronous event dispatching. Supports multiple subscribers per event type
 * and thread-safe publish operations including ISR-safe publishing.
 *
 * @copyright Copyright (c) 2024-2025
 */

#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event types supported by the event bus
 */
typedef enum {
    /* Device Events */
    EVT_DEVICE_JOINED,        /**< A device joined the network */
    EVT_DEVICE_LEFT,          /**< A device left the network */
    EVT_DEVICE_STATE_CHANGED, /**< A device state changed */
    EVT_DEVICE_INTERVIEWED,   /**< A device interview completed */

    /* Network Events */
    EVT_NETWORK_READY,        /**< Zigbee network is ready */
    EVT_ZB_PERMIT_JOIN,       /**< Zigbee permit join state changed */
    EVT_ZB_ATTRIBUTE_REPORT,  /**< Zigbee attribute report received */
    EVT_ZB_COMMAND_RECEIVED,  /**< Zigbee ZCL command received */
    EVT_ZB_BIND_SUCCESS,      /**< Zigbee binding created successfully */
    EVT_ZB_BIND_FAILED,       /**< Zigbee binding creation failed */
    EVT_ZB_BINDINGS_LIST,     /**< Zigbee bindings list updated */
    EVT_ZB_GROUP_ADDED,       /**< Device added to Zigbee group */
    EVT_ZB_SCENE_ACTIVATED,   /**< Zigbee scene activated */

    /* WiFi Events */
    EVT_WIFI_CONNECTED,       /**< WiFi connected with IP address */
    EVT_WIFI_DISCONNECTED,    /**< WiFi disconnected */
    EVT_WIFI_SCAN_COMPLETE,   /**< WiFi network scan completed */
    EVT_WIFI_AP_STARTED,      /**< WiFi AP mode started */
    EVT_WIFI_AP_CLIENT_CONNECTED, /**< Client connected to AP */

    /* MQTT Events */
    EVT_MQTT_CONNECTED,       /**< MQTT client connected */
    EVT_MQTT_DISCONNECTED,    /**< MQTT client disconnected */
    EVT_MQTT_SUBSCRIBE_OK,    /**< MQTT subscription succeeded */
    EVT_MQTT_PUBLISH_OK,      /**< MQTT publish acknowledged */
    EVT_MQTT_ERROR,           /**< MQTT error occurred */

    /* HA Discovery Events */
    EVT_HA_DISCOVERY_PUBLISH, /**< HA discovery config to publish */

    /* BLE Events */
    EVT_BLE_DEVICE_FOUND,     /**< BLE device discovered */
    EVT_BLE_DEVICE_LOST,      /**< BLE device lost */

    /* System Events */
    EVT_SYSTEM_BOOT_COMPLETE, /**< System boot completed */
    EVT_SYSTEM_SHUTDOWN,      /**< System shutdown initiated */
    EVT_CONFIG_CHANGED,       /**< Configuration changed */
    EVT_LIFECYCLE_CHANGED,    /**< Lifecycle state changed */
    EVT_MEMORY_PRESSURE,      /**< Memory pressure detected */
    EVT_SYSTEM_MEMORY_WARNING,  /**< Memory warning threshold reached */
    EVT_SYSTEM_MEMORY_CRITICAL, /**< Memory critical threshold reached */

    /* Command Events */
    EVT_COMMAND_RECEIVED,     /**< Command received from MQTT/ESPHome */
    EVT_COMMAND_RESPONSE,     /**< Command completed with result */

    /* ESPHome Events */
    EVT_ESPHOME_CONNECTED,    /**< ESPHome client connected */
    EVT_ESPHOME_DISCONNECTED, /**< ESPHome client disconnected */

    /* Power Events */
    EVT_POWER_STATE_CHANGED,  /**< Power/electrical measurement changed */

    /* Tuya Events */
    EVT_TUYA_DATAPOINT,       /**< Tuya datapoint received */

    /* OTA Events (Gateway Firmware) */
    EVT_OTA_STARTED,          /**< OTA update started */
    EVT_OTA_PROGRESS,         /**< OTA update progress */
    EVT_OTA_COMPLETE,         /**< OTA update completed successfully */
    EVT_OTA_FAILED,           /**< OTA update failed */

    /* Zigbee Device OTA Events */
    EVT_ZB_OTA_PROGRESS,      /**< Zigbee device OTA progress update */
    EVT_ZB_OTA_COMPLETE,      /**< Zigbee device OTA completed */
    EVT_ZB_OTA_IMAGES_CHANGED,/**< OTA images list changed */

    /* Extended Zigbee Events */
    EVT_ZB_ROUTER_STATUS,         /**< Router state changed */
    EVT_ZB_TOPOLOGY_RESPONSE,     /**< Topology query result */
    EVT_ZB_TOUCHLINK_RESULT,      /**< Touchlink commissioning result */
    EVT_ZB_ALARM_TRIGGERED,       /**< Alarm cluster event */
    EVT_ZB_REPORTING_RESPONSE,    /**< Reporting config response */

    /* Backup/Restore Events */
    EVT_BACKUP_RESPONSE,          /**< Backup/Restore result */

    /* Demand-Response Events */
    EVT_DRLC_LOAD_CONTROL,        /**< Demand-Response load control event */

    /* Battery Events */
    EVT_DEVICE_BATTERY_CHANGED,   /**< Device battery level changed */

    /* Bridge Events (Layer-clean MQTT publishing from core) */
    EVT_BRIDGE_PUBLISH,           /**< Bridge data to publish via MQTT */

    /* Availability Events */
    EVT_DEVICE_AVAILABILITY_CHANGED, /**< Device online/offline status changed */

    /* Retained Topic Cleanup Events */
    EVT_DEVICE_TOPICS_CLEAR,      /**< Clear retained MQTT topics for a device */

    EVT_COUNT,                /**< Number of event types (must be last) */
} event_type_t;

/**
 * @brief Event priority levels
 *
 * Higher priority events are processed before lower priority events.
 * Critical and high priority events use a separate queue for faster processing.
 */
typedef enum {
    EVT_PRIORITY_LOW = 0,       /**< Low priority - processed last */
    EVT_PRIORITY_NORMAL = 1,    /**< Normal priority - default */
    EVT_PRIORITY_HIGH = 2,      /**< High priority - processed before normal */
    EVT_PRIORITY_CRITICAL = 3,  /**< Critical priority - processed first */
} event_priority_t;

/**
 * @brief Event handler callback function type
 *
 * @param type Event type that triggered the callback
 * @param data Pointer to event data (may be NULL)
 * @param data_size Size of event data in bytes
 * @param user_ctx User context passed during subscription
 */
typedef void (*event_handler_t)(event_type_t type, void *data, size_t data_size, void *user_ctx);

/**
 * @brief Event filter callback function type
 *
 * Called before event is dispatched to handler. Return true to receive the event,
 * false to skip this handler for this event.
 *
 * @param type Event type
 * @param data Pointer to event data (may be NULL)
 * @param filter_ctx Filter context passed during subscription
 * @return true to receive the event, false to skip
 */
typedef bool (*event_filter_t)(event_type_t type, void *data, void *filter_ctx);

/**
 * @brief Initialize the event bus
 *
 * Creates the event queue, dispatcher task, and initializes subscriber arrays.
 * Must be called before any other event bus functions.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized
 *      - ESP_ERR_NO_MEM if memory allocation failed
 */
esp_err_t event_bus_init(void);

/**
 * @brief Deinitialize the event bus
 *
 * Stops the dispatcher task, deletes the queue, and clears all subscribers.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t event_bus_deinit(void);

/**
 * @brief Subscribe to an event type
 *
 * Registers a handler callback for the specified event type.
 * The handler will be called when events of this type are published.
 *
 * @param type Event type to subscribe to
 * @param handler Callback function to invoke on event
 * @param ctx User context passed to handler (can be NULL)
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_INVALID_ARG if type is invalid or handler is NULL
 *      - ESP_ERR_NO_MEM if maximum subscribers reached for this event type
 */
esp_err_t event_subscribe(event_type_t type, event_handler_t handler, void *ctx);

/**
 * @brief Subscribe to an event type with optional filter
 *
 * Registers a handler callback for the specified event type with an optional
 * filter function. The filter is called before the handler; if it returns false,
 * the handler is skipped for that event.
 *
 * @param type Event type to subscribe to
 * @param handler Callback function to invoke on event
 * @param filter Optional filter function (can be NULL for no filtering)
 * @param ctx User context passed to both handler and filter (can be NULL)
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_INVALID_ARG if type is invalid or handler is NULL
 *      - ESP_ERR_NO_MEM if maximum subscribers reached for this event type
 */
esp_err_t event_subscribe_filtered(event_type_t type, event_handler_t handler,
                                   event_filter_t filter, void *ctx);

/**
 * @brief Unsubscribe from an event type
 *
 * Removes a previously registered handler from the specified event type.
 *
 * @param type Event type to unsubscribe from
 * @param handler Handler to remove
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_INVALID_ARG if type is invalid or handler is NULL
 *      - ESP_ERR_NOT_FOUND if handler not found for this event type
 */
esp_err_t event_unsubscribe(event_type_t type, event_handler_t handler);

/**
 * @brief Publish an event (asynchronous)
 *
 * Queues an event for dispatch to all subscribed handlers.
 * If data is provided, it will be copied and freed after all handlers are called.
 * Returns immediately; handlers are called from the dispatcher task.
 *
 * @param type Event type to publish
 * @param data Pointer to event data (can be NULL)
 * @param data_size Size of event data in bytes (0 if data is NULL)
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_INVALID_ARG if type is invalid
 *      - ESP_ERR_NO_MEM if data copy failed or queue is full
 */
esp_err_t event_publish(event_type_t type, void *data, size_t data_size);

/**
 * @brief Publish an event with priority (asynchronous)
 *
 * Queues an event with specified priority for dispatch to all subscribed handlers.
 * Higher priority events (CRITICAL, HIGH) are processed before lower priority events.
 * If data is provided, it will be copied and freed after all handlers are called.
 *
 * @param type Event type to publish
 * @param data Pointer to event data (can be NULL)
 * @param data_size Size of event data in bytes (0 if data is NULL)
 * @param priority Event priority level
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_INVALID_ARG if type is invalid
 *      - ESP_ERR_NO_MEM if data copy failed or queue is full
 */
esp_err_t event_publish_prio(event_type_t type, void *data, size_t data_size,
                             event_priority_t priority);

/**
 * @brief Publish an event from an ISR context
 *
 * Queues an event for dispatch from an interrupt service routine.
 * Data is NOT copied - it must be static or global and remain valid
 * until the event is processed.
 *
 * @note data_size should be set to 0 to indicate no-copy semantics
 *
 * @param type Event type to publish
 * @param data Pointer to static/global event data (can be NULL)
 * @param data_size Must be 0 (no-copy semantics for ISR)
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if type is invalid
 *      - ESP_FAIL if queue send failed
 */
esp_err_t event_publish_from_isr(event_type_t type, void *data, size_t data_size);

/**
 * @brief Publish an event with priority from an ISR context
 *
 * Queues an event with specified priority from an interrupt service routine.
 * Data is NOT copied - it must be static or global and remain valid
 * until the event is processed.
 *
 * @param type Event type to publish
 * @param data Pointer to static/global event data (can be NULL)
 * @param data_size Must be 0 (no-copy semantics for ISR)
 * @param priority Event priority level
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if type is invalid
 *      - ESP_FAIL if queue send failed
 */
esp_err_t event_publish_prio_from_isr(event_type_t type, void *data, size_t data_size,
                                      event_priority_t priority);

/**
 * @brief Get the string name of an event type
 *
 * @param type Event type
 *
 * @return String representation of the event type, or "UNKNOWN" if invalid
 */
const char *event_type_to_str(event_type_t type);

/**
 * @brief Get the number of subscribers for an event type
 *
 * @param type Event type to query
 *
 * @return Number of active subscribers, or 0 if type is invalid
 */
size_t event_get_subscriber_count(event_type_t type);

/**
 * @brief Get pending event counts in queues
 *
 * Returns the number of events waiting to be processed in high and normal
 * priority queues.
 *
 * @param high_prio_count Pointer to store high priority queue count (can be NULL)
 * @param normal_prio_count Pointer to store normal priority queue count (can be NULL)
 */
void event_get_queue_stats(size_t *high_prio_count, size_t *normal_prio_count);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
