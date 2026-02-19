/**
 * @file bridge_events.h
 * @brief Bridge Event System for Zigbee2MQTT Compatibility
 *
 * This module provides centralized event publishing for all important bridge events.
 * Events are published to "zigbee2mqtt/bridge/event" topic in JSON format,
 * allowing Home Assistant and other frontends to subscribe to bridge activity.
 *
 * Event Types (Zigbee2MQTT compatible):
 * - device_joined: Device has joined the network
 * - device_leave: Device has left the network
 * - device_interview: Interview status (started/successful/failed)
 * - device_announce: Device has announced its presence
 * - device_renamed: Device friendly name was changed
 * - permit_join: Permit join status changed
 * - network_started: Zigbee network has started
 * - network_stopped: Zigbee network has stopped
 * - device_bind: Binding was created
 * - device_unbind: Binding was removed
 * - group_member_added: Device added to group
 * - group_member_removed: Device removed from group
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BRIDGE_EVENTS_H
#define BRIDGE_EVENTS_H

#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bridge event types enumeration
 */
typedef enum {
    BRIDGE_EVENT_DEVICE_JOINED = 0,     /**< Device joined the network */
    BRIDGE_EVENT_DEVICE_LEAVE,          /**< Device left the network */
    BRIDGE_EVENT_DEVICE_INTERVIEW,      /**< Device interview status changed */
    BRIDGE_EVENT_DEVICE_ANNOUNCE,       /**< Device announced itself */
    BRIDGE_EVENT_DEVICE_RENAMED,        /**< Device was renamed */
    BRIDGE_EVENT_PERMIT_JOIN,           /**< Permit join status changed */
    BRIDGE_EVENT_NETWORK_STARTED,       /**< Network started */
    BRIDGE_EVENT_NETWORK_STOPPED,       /**< Network stopped */
    BRIDGE_EVENT_DEVICE_BIND,           /**< Device binding created */
    BRIDGE_EVENT_DEVICE_UNBIND,         /**< Device binding removed */
    BRIDGE_EVENT_GROUP_MEMBER_ADDED,    /**< Member added to group */
    BRIDGE_EVENT_GROUP_MEMBER_REMOVED,  /**< Member removed from group */
    /* API-009: Enhanced signal events */
    BRIDGE_EVENT_NLME_STATUS,           /**< Network layer status indication */
    BRIDGE_EVENT_PARENT_ANNOUNCE,       /**< Parent announcement (mesh) */
    BRIDGE_EVENT_TC_REJOIN,             /**< Trust Center rejoin complete */
    BRIDGE_EVENT_MAX                    /**< Maximum event type (for bounds checking) */
} bridge_event_type_t;

/**
 * @brief Interview status strings
 */
#define BRIDGE_INTERVIEW_STARTED     "started"
#define BRIDGE_INTERVIEW_SUCCESSFUL  "successful"
#define BRIDGE_INTERVIEW_FAILED      "failed"

/**
 * @brief MQTT topic for bridge events
 */
#define BRIDGE_EVENT_TOPIC           "zigbee2mqtt/bridge/event"

/**
 * @brief Initialize the bridge events module
 *
 * Initializes internal state. Should be called after MQTT client is ready.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if already initialized
 */
esp_err_t bridge_events_init(void);

/**
 * @brief Deinitialize the bridge events module
 *
 * Frees any allocated resources.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t bridge_events_deinit(void);

/**
 * @brief Publish a bridge event with custom data
 *
 * Publishes an event to the bridge event topic with the specified type
 * and data payload. The event is formatted as JSON:
 * {"type": "<event_type>", "data": <data>}
 *
 * @param[in] type Event type
 * @param[in] data Event data as cJSON object (will be consumed/freed)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if type is invalid
 * @return ESP_ERR_INVALID_STATE if not initialized or MQTT not connected
 * @return ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t bridge_events_publish(bridge_event_type_t type, cJSON *data);

/**
 * @brief Get event type string
 *
 * Returns the Zigbee2MQTT compatible string name for an event type.
 *
 * @param[in] type Event type
 * @return Event type string or "unknown" if invalid
 */
const char* bridge_events_get_type_str(bridge_event_type_t type);

/* ============================================================================
 * Convenience Functions - Pre-formatted event publishers
 * ============================================================================ */

/**
 * @brief Publish device_joined event
 *
 * Published when a new device joins the Zigbee network.
 *
 * @param[in] ieee_addr Device IEEE address (64-bit)
 * @param[in] friendly_name Device friendly name (can be NULL for IEEE address)
 * @return ESP_OK on success
 */
esp_err_t bridge_event_device_joined(uint64_t ieee_addr, const char *friendly_name);

/**
 * @brief Publish device_leave event
 *
 * Published when a device leaves the Zigbee network.
 *
 * @param[in] ieee_addr Device IEEE address (64-bit)
 * @param[in] friendly_name Device friendly name (can be NULL for IEEE address)
 * @return ESP_OK on success
 */
esp_err_t bridge_event_device_leave(uint64_t ieee_addr, const char *friendly_name);

/**
 * @brief Publish device_interview event
 *
 * Published when device interview status changes.
 *
 * @param[in] ieee_addr Device IEEE address (64-bit)
 * @param[in] status Interview status ("started", "successful", "failed")
 * @return ESP_OK on success
 */
esp_err_t bridge_event_device_interview(uint64_t ieee_addr, const char *status);

/**
 * @brief Publish device_announce event
 *
 * Published when a device announces itself (typically after power cycle).
 *
 * @param[in] ieee_addr Device IEEE address (64-bit)
 * @return ESP_OK on success
 */
esp_err_t bridge_event_device_announce(uint64_t ieee_addr);

/**
 * @brief Publish device_announce event with network address
 *
 * Published when a device announces itself, includes network address.
 *
 * @param[in] ieee_addr Device IEEE address (64-bit)
 * @param[in] nwk_addr Device network (short) address
 * @return ESP_OK on success
 */
esp_err_t bridge_event_device_announce_full(uint64_t ieee_addr, uint16_t nwk_addr);

/**
 * @brief Publish device_renamed event
 *
 * Published when a device's friendly name is changed.
 *
 * @param[in] ieee_addr Device IEEE address (64-bit)
 * @param[in] from Previous friendly name
 * @param[in] to New friendly name
 * @return ESP_OK on success
 */
esp_err_t bridge_event_device_renamed(uint64_t ieee_addr, const char *from, const char *to);

/**
 * @brief Publish permit_join event
 *
 * Published when permit join status changes.
 *
 * @param[in] enabled true if permit join is enabled
 * @param[in] time Duration in seconds (0 if disabled, 254/255 for unlimited)
 * @return ESP_OK on success
 */
esp_err_t bridge_event_permit_join(bool enabled, uint8_t time);

/**
 * @brief Publish network_started event
 *
 * Published when the Zigbee network starts successfully.
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_event_network_started(void);

/**
 * @brief Publish network_stopped event
 *
 * Published when the Zigbee network stops.
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_event_network_stopped(void);

/**
 * @brief Publish device_bind event
 *
 * Published when a binding is created between devices.
 *
 * @param[in] source_ieee Source device IEEE address
 * @param[in] source_name Source device friendly name (can be NULL)
 * @param[in] target_ieee Target device IEEE address
 * @param[in] target_name Target device friendly name (can be NULL)
 * @param[in] cluster_name Cluster name that was bound
 * @return ESP_OK on success
 */
esp_err_t bridge_event_device_bind(uint64_t source_ieee, const char *source_name,
                                    uint64_t target_ieee, const char *target_name,
                                    const char *cluster_name);

/**
 * @brief Publish device_unbind event
 *
 * Published when a binding is removed between devices.
 *
 * @param[in] source_ieee Source device IEEE address
 * @param[in] source_name Source device friendly name (can be NULL)
 * @param[in] target_ieee Target device IEEE address
 * @param[in] target_name Target device friendly name (can be NULL)
 * @param[in] cluster_name Cluster name that was unbound
 * @return ESP_OK on success
 */
esp_err_t bridge_event_device_unbind(uint64_t source_ieee, const char *source_name,
                                      uint64_t target_ieee, const char *target_name,
                                      const char *cluster_name);

/**
 * @brief Publish group_member_added event
 *
 * Published when a device is added to a group.
 *
 * @param[in] group_name Group name
 * @param[in] group_id Group ID
 * @param[in] ieee_addr Device IEEE address
 * @param[in] device_name Device friendly name (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t bridge_event_group_member_added(const char *group_name, uint16_t group_id,
                                           uint64_t ieee_addr, const char *device_name);

/**
 * @brief Publish group_member_removed event
 *
 * Published when a device is removed from a group.
 *
 * @param[in] group_name Group name
 * @param[in] group_id Group ID
 * @param[in] ieee_addr Device IEEE address
 * @param[in] device_name Device friendly name (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t bridge_event_group_member_removed(const char *group_name, uint16_t group_id,
                                             uint64_t ieee_addr, const char *device_name);

/* ============================================================================
 * API-009: Enhanced Signal Handler Events
 * ============================================================================ */

/**
 * @brief Publish NLME status event (API-009)
 *
 * Published when a significant network layer status indication occurs.
 * Used for network diagnostics and route error tracking.
 *
 * @param[in] nwk_addr Network address related to the status
 * @param[in] status NLME status code (0x00=success, 0xCC=route_discovery_failed, etc.)
 * @return ESP_OK on success
 */
esp_err_t bridge_event_nlme_status(uint16_t nwk_addr, uint8_t status);

/**
 * @brief Convert NLME status code to human-readable string
 */
const char* nlme_status_to_str(uint8_t status);

/**
 * @brief Publish parent announcement event (API-009)
 *
 * Published when a parent announcement is received in the mesh network.
 * Used for network topology tracking.
 *
 * @return ESP_OK on success
 */
esp_err_t bridge_event_parent_announce(void);

/**
 * @brief Publish Trust Center rejoin event (API-009)
 *
 * Published when a device completes a Trust Center rejoin.
 *
 * @param[in] success true if rejoin was successful, false if failed
 * @return ESP_OK on success
 */
esp_err_t bridge_event_tc_rejoin_done(bool success);

/**
 * @brief Get event statistics
 *
 * Returns the number of events published by type.
 *
 * @param[out] total_count Total events published (can be NULL)
 * @param[out] error_count Total publish errors (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t bridge_events_get_stats(uint32_t *total_count, uint32_t *error_count);

/**
 * @brief Test bridge events module
 *
 * Performs basic functionality tests.
 *
 * @return ESP_OK if all tests pass
 * @return ESP_FAIL if any test fails
 */
esp_err_t bridge_events_test(void);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_EVENTS_H */
