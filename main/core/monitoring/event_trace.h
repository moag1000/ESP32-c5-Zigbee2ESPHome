/**
 * @file event_trace.h
 * @brief Event Trace Module - Debug Event Flow Monitoring
 *
 * Subscribes to ALL events on the event bus and logs them with timestamps.
 * Supports optional MQTT publishing and configurable event filtering.
 * Maintains a ring buffer of recent events for debug dumps.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef EVENT_TRACE_H
#define EVENT_TRACE_H

#include <stdint.h>
#include "esp_err.h"
#include "core/events/event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Event Trace Constants
 * ============================================================================ */

/** @brief Maximum number of events stored in ring buffer */
#define EVENT_TRACE_RING_BUFFER_SIZE    32

/** @brief Maximum length of event summary string */
#define EVENT_TRACE_SUMMARY_LEN         64

/** @brief MQTT topic for event trace publishing */
#define EVENT_TRACE_MQTT_TOPIC          "zigbee2mqtt/bridge/events"

/* ============================================================================
 * Event Filter Masks
 * ============================================================================ */

/** @brief Filter mask: Device events (joined, left, state, interviewed) */
#define EVENT_TRACE_FILTER_DEVICE       ((1U << EVT_DEVICE_JOINED) | \
                                         (1U << EVT_DEVICE_LEFT) | \
                                         (1U << EVT_DEVICE_STATE_CHANGED) | \
                                         (1U << EVT_DEVICE_INTERVIEWED))

/** @brief Filter mask: Network events */
#define EVENT_TRACE_FILTER_NETWORK      (1U << EVT_NETWORK_READY)

/** @brief Filter mask: MQTT events (connected, disconnected) */
#define EVENT_TRACE_FILTER_MQTT         ((1U << EVT_MQTT_CONNECTED) | \
                                         (1U << EVT_MQTT_DISCONNECTED))

/** @brief Filter mask: BLE events (found, lost) */
#define EVENT_TRACE_FILTER_BLE          ((1U << EVT_BLE_DEVICE_FOUND) | \
                                         (1U << EVT_BLE_DEVICE_LOST))

/** @brief Filter mask: System events (config, lifecycle, memory) */
#define EVENT_TRACE_FILTER_SYSTEM       ((1U << EVT_CONFIG_CHANGED) | \
                                         (1U << EVT_LIFECYCLE_CHANGED) | \
                                         (1U << EVT_MEMORY_PRESSURE))

/** @brief Filter mask: All events (default) */
#define EVENT_TRACE_FILTER_ALL          0xFFFFFFFF

/* ============================================================================
 * Device ID Type (Protocol-agnostic, trace-specific)
 * ============================================================================ */

/**
 * @brief Device identifier for traces - can represent Zigbee, BLE, or virtual device
 *
 * @note Named trace_device_id_t to avoid conflict with event_data.h device_id_t
 */
typedef struct {
    uint64_t ieee_addr;         /**< IEEE address (Zigbee) or 0 */
    uint16_t short_addr;        /**< Short address (Zigbee) or 0 */
    uint8_t mac[6];             /**< MAC address (BLE) or zeros */
    bool valid;                 /**< True if device ID is set */
} trace_device_id_t;

/* ============================================================================
 * Event Trace Entry Structure
 * ============================================================================ */

/**
 * @brief Event trace ring buffer entry
 *
 * Captures essential information about each event for debug analysis.
 */
typedef struct {
    uint32_t timestamp_ms;              /**< Uptime timestamp in milliseconds */
    event_type_t type;                  /**< Event type enum */
    trace_device_id_t device_id;        /**< Associated device (if applicable) */
    char summary[EVENT_TRACE_SUMMARY_LEN]; /**< Human-readable event summary */
} event_trace_entry_t;

/* ============================================================================
 * Event Trace Statistics
 * ============================================================================ */

/**
 * @brief Event trace statistics
 */
typedef struct {
    uint32_t total_events;              /**< Total events traced */
    uint32_t events_per_type[EVT_COUNT]; /**< Count per event type */
    uint32_t filtered_events;           /**< Events filtered out */
    uint32_t mqtt_publish_count;        /**< Events published to MQTT */
    uint32_t mqtt_publish_errors;       /**< MQTT publish errors */
    uint32_t buffer_overwrites;         /**< Ring buffer overwrites */
} event_trace_stats_t;

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Initialize the event trace module
 *
 * Allocates ring buffer, subscribes to all event types, and initializes
 * internal state. Tracing is enabled by default after initialization.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if already initialized
 * @return ESP_ERR_NO_MEM if memory allocation failed
 */
esp_err_t event_trace_init(void);

/**
 * @brief Deinitialize the event trace module
 *
 * Unsubscribes from all events and frees the ring buffer.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t event_trace_deinit(void);

/**
 * @brief Enable or disable event tracing
 *
 * When disabled, events are not logged or stored in the ring buffer.
 * Subscriptions remain active to allow quick re-enable without resubscribing.
 *
 * @param enabled True to enable tracing, false to disable
 */
void event_trace_set_enabled(bool enabled);

/**
 * @brief Check if event tracing is enabled
 *
 * @return True if enabled, false otherwise
 */
bool event_trace_is_enabled(void);

/**
 * @brief Enable or disable MQTT publishing of events
 *
 * When enabled, traced events are published to EVENT_TRACE_MQTT_TOPIC
 * as JSON objects. MQTT must be connected for publishing to succeed.
 *
 * @param enabled True to enable MQTT publishing, false to disable
 */
void event_trace_set_mqtt_enabled(bool enabled);

/**
 * @brief Check if MQTT publishing is enabled
 *
 * @return True if enabled, false otherwise
 */
bool event_trace_is_mqtt_enabled(void);

/**
 * @brief Set event type filter mask
 *
 * Only events matching the filter mask will be traced. Use the
 * EVENT_TRACE_FILTER_* macros to construct the mask.
 *
 * Example: Trace only device events:
 *   event_trace_set_filter(EVENT_TRACE_FILTER_DEVICE);
 *
 * Example: Trace device and MQTT events:
 *   event_trace_set_filter(EVENT_TRACE_FILTER_DEVICE | EVENT_TRACE_FILTER_MQTT);
 *
 * @param event_type_mask Bitmask of event types to trace (1 << EVT_*)
 */
void event_trace_set_filter(uint32_t event_type_mask);

/**
 * @brief Get current filter mask
 *
 * @return Current event type filter mask
 */
uint32_t event_trace_get_filter(void);

/**
 * @brief Dump ring buffer contents to log
 *
 * Outputs all stored events from oldest to newest using ESP_LOGI.
 * Useful for post-mortem debugging.
 *
 * @return Number of events dumped
 */
size_t event_trace_dump(void);

/**
 * @brief Dump ring buffer contents as JSON to MQTT
 *
 * Publishes the entire ring buffer as a JSON array to MQTT.
 * Useful for remote debugging.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized or MQTT not connected
 * @return ESP_ERR_NO_MEM if JSON creation failed
 */
esp_err_t event_trace_dump_mqtt(void);

/**
 * @brief Get event trace statistics
 *
 * @param stats Pointer to statistics structure to fill
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if stats is NULL
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t event_trace_get_stats(event_trace_stats_t *stats);

/**
 * @brief Clear ring buffer and reset statistics
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t event_trace_clear(void);

/**
 * @brief Get the most recent trace entry
 *
 * @param entry Pointer to entry structure to fill
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if entry is NULL
 * @return ESP_ERR_NOT_FOUND if ring buffer is empty
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t event_trace_get_last(event_trace_entry_t *entry);

/**
 * @brief Get number of events currently in ring buffer
 *
 * @return Number of events (0 to EVENT_TRACE_RING_BUFFER_SIZE)
 */
size_t event_trace_get_count(void);

/**
 * @brief Check if event trace module is initialized
 *
 * @return True if initialized, false otherwise
 */
bool event_trace_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_TRACE_H */
