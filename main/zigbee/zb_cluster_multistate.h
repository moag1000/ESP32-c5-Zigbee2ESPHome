/**
 * @file zb_cluster_multistate.h
 * @brief Multistate Input/Output/Value Cluster (0x0012, 0x0013, 0x0014) API
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_CLUSTER_MULTISTATE_H
#define ZB_CLUSTER_MULTISTATE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types moved from zb_device_handler_types.h
 * ============================================================================ */

/** @brief Maximum multistate (input/output/value) devices tracked */
#define ZB_STATE_MAX_MULTISTATE             32

/* ============================================================================
 * Multistate Input/Output/Value Clusters (0x0012, 0x0013, 0x0014) Types
 * ============================================================================ */

/**
 * @brief Multistate Input Cluster ID
 *
 * Used for multi-button switches (Aqara, IKEA), rotary switches, and
 * other devices that report discrete state values.
 */
#define ZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT      0x0012

/**
 * @brief Multistate Output Cluster ID
 *
 * Used for multi-mode outputs where PresentValue is writable.
 */
#define ZB_ZCL_CLUSTER_ID_MULTISTATE_OUTPUT     0x0013

/**
 * @brief Multistate Value Cluster ID
 *
 * Used for generic multi-state storage where PresentValue is writable.
 */
#define ZB_ZCL_CLUSTER_ID_MULTISTATE_VALUE      0x0014

/**
 * @brief Multistate Cluster Attribute IDs
 *
 * These attributes are common to all three multistate clusters.
 */
#define ZB_ZCL_ATTR_MULTISTATE_NUMBER_OF_STATES     0x004A  /**< NumberOfStates (uint16) */
#define ZB_ZCL_ATTR_MULTISTATE_OUT_OF_SERVICE       0x0051  /**< OutOfService (bool) */
#define ZB_ZCL_ATTR_MULTISTATE_PRESENT_VALUE        0x0055  /**< PresentValue (uint16) */
#define ZB_ZCL_ATTR_MULTISTATE_STATUS_FLAGS         0x006F  /**< StatusFlags (bitmap8) */

/**
 * @brief Multistate StatusFlags bits (bitmap8)
 *
 * Bit 0: InAlarm - Indicates an alarm condition
 * Bit 1: Fault - Indicates a fault condition
 * Bit 2: Overridden - Indicates value is overridden
 * Bit 3: OutOfService - Duplicate of OutOfService attribute
 */
#define ZB_MULTISTATE_STATUS_IN_ALARM       (1 << 0)
#define ZB_MULTISTATE_STATUS_FAULT          (1 << 1)
#define ZB_MULTISTATE_STATUS_OVERRIDDEN     (1 << 2)
#define ZB_MULTISTATE_STATUS_OUT_OF_SERVICE (1 << 3)

/**
 * @brief Multistate cluster type enumeration
 *
 * Identifies which type of multistate cluster is being used.
 */
typedef enum {
    ZB_MULTISTATE_TYPE_INPUT  = 0,  /**< Multistate Input (read-only) */
    ZB_MULTISTATE_TYPE_OUTPUT = 1,  /**< Multistate Output (writable) */
    ZB_MULTISTATE_TYPE_VALUE  = 2   /**< Multistate Value (writable) */
} zb_multistate_type_t;

/**
 * @brief Multistate state structure
 *
 * Stores the current state for a multistate input/output/value cluster.
 * This structure is shared by all three cluster types.
 */
typedef struct {
    uint16_t number_of_states;      /**< Number of valid states (1 to N) */
    uint16_t present_value;         /**< Current state value (1 to number_of_states) */
    bool out_of_service;            /**< True if device is out of service */
    uint8_t status_flags;           /**< Status flags bitmap */
    zb_multistate_type_t type;      /**< Cluster type (input/output/value) */
    uint8_t endpoint;               /**< Endpoint this state is associated with */
} zb_multistate_state_t;

/**
 * @brief Multistate state change callback type
 *
 * Called when a multistate cluster attribute changes.
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current multistate state
 */
typedef void (*zb_multistate_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                          const zb_multistate_state_t *state);

/**
 * @brief Initialize multistate cluster module
 * @return ESP_OK on success
 */
esp_err_t zb_cluster_multistate_init(void);

/**
 * @brief Deinitialize multistate cluster module
 */
void zb_cluster_multistate_deinit(void);

/**
 * @brief Register multistate state change callback
 */
esp_err_t zb_multistate_register_callback(zb_multistate_state_cb_t callback);

/**
 * @brief Check if device has Multistate Input cluster
 */
bool zb_device_has_multistate_input(uint16_t short_addr);

/**
 * @brief Check if device has Multistate Output cluster
 */
bool zb_device_has_multistate_output(uint16_t short_addr);

/**
 * @brief Check if device has Multistate Value cluster
 */
bool zb_device_has_multistate_value(uint16_t short_addr);

/**
 * @brief Check if device has any multistate cluster
 */
bool zb_device_has_multistate(uint16_t short_addr);

/**
 * @brief Read multistate attributes from device
 */
esp_err_t zb_multistate_read_state(uint16_t short_addr, uint8_t endpoint,
                                    uint16_t cluster_id);

/**
 * @brief Set multistate PresentValue (for output/value clusters)
 */
esp_err_t zb_multistate_set_value(uint16_t short_addr, uint8_t endpoint,
                                   uint16_t cluster_id, uint16_t value);

/**
 * @brief Handle multistate attribute report
 */
esp_err_t zb_multistate_handle_report(uint16_t short_addr, uint8_t endpoint,
                                       uint16_t cluster_id, uint16_t attr_id,
                                       void *value, size_t value_len);

/**
 * @brief Get current multistate state for a device
 */
esp_err_t zb_multistate_get_state(uint16_t short_addr, uint8_t endpoint,
                                   zb_multistate_state_t *state);

/**
 * @brief Get cluster type from cluster ID
 */
int zb_multistate_get_type_from_cluster(uint16_t cluster_id);

/**
 * @brief Get string representation of multistate type
 */
const char* zb_multistate_type_to_string(zb_multistate_type_t type);

/**
 * @brief Check if present value is in alarm state
 */
bool zb_multistate_is_in_alarm(const zb_multistate_state_t *state);

/**
 * @brief Check if device has a fault
 */
bool zb_multistate_has_fault(const zb_multistate_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CLUSTER_MULTISTATE_H */
