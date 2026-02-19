/**
 * @file zb_cluster_internal.h
 * @brief Internal header for Zigbee cluster implementations
 *
 * This header provides shared types, macros, and function declarations
 * used by all cluster-specific modules. It is not part of the public API.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_CLUSTER_INTERNAL_H
#define ZB_CLUSTER_INTERNAL_H

#include "esp_err.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "zb_device_handler_types.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "zb_constants.h"
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * ZCL Command Macro (moved from zb_device_handler.h)
 * ============================================================================ */

/**
 * @brief Initialize a ZCL basic command structure with short address
 *
 * @param addr Short address of the destination device
 * @param ep Endpoint on the destination device
 */
#define ZCL_BASIC_CMD_INIT(addr, ep) \
    .zcl_basic_cmd = { \
        .dst_addr_u = { \
            .addr_short = (addr), \
        }, \
        .dst_endpoint = (ep), \
        .src_endpoint = 1, \
    }

/* ============================================================================
 * Cluster Utility: check device has a specific cluster (NG-based)
 * ============================================================================ */

/**
 * @brief Check if a device supports a given cluster
 *
 * Uses the NG device registry for lookup.
 *
 * @param short_addr Device short address
 * @param cluster_id ZCL cluster ID
 * @return true if found
 */
static inline bool zb_device_has_cluster(uint16_t short_addr, uint16_t cluster_id)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (!dev) return false;
    return device_zigbee_has_cluster(dev, cluster_id);
}

/* ============================================================================
 * Common Cluster State Management Macros
 *
 * These macros reduce code duplication for the common pattern of
 * managing cluster-specific state arrays.
 * ============================================================================ */

/**
 * @brief Declare cluster state storage variables
 *
 * @param cluster_name Lower-case cluster name (e.g., thermostat)
 * @param state_type State structure type
 * @param max_count Maximum number of devices to track
 */
#define ZB_CLUSTER_STATE_DECLARE(cluster_name, state_type, max_count) \
    static state_type s_##cluster_name##_states[max_count]; \
    static uint16_t s_##cluster_name##_addrs[max_count]; \
    static size_t s_##cluster_name##_count = 0; \
    static zb_##cluster_name##_state_cb_t s_##cluster_name##_callback = NULL

/**
 * @brief Implement find state index function
 *
 * @param cluster_name Lower-case cluster name
 */
#define ZB_CLUSTER_FIND_STATE_INDEX_IMPL(cluster_name) \
    static int find_##cluster_name##_state_index(uint16_t short_addr) \
    { \
        for (size_t i = 0; i < s_##cluster_name##_count; i++) { \
            if (s_##cluster_name##_addrs[i] == short_addr) { \
                return (int)i; \
            } \
        } \
        return -1; \
    }

/**
 * @brief Macro to define a state lookup function (CQ-058)
 *
 * Simplified version for use with explicit array and count variables.
 *
 * @param name Function name suffix (e.g., thermostat)
 * @param addr_array Array of device addresses
 * @param count_var Count variable
 */
#define DEFINE_STATE_LOOKUP(name, addr_array, count_var) \
    static int find_##name##_state_index(uint16_t short_addr) { \
        for (size_t i = 0; i < count_var; i++) { \
            if (addr_array[i] == short_addr) return (int)i; \
        } \
        return -1; \
    }

/**
 * @brief Implement register callback function
 *
 * @param cluster_name Lower-case cluster name
 * @param tag_name TAG string for logging
 */
#define ZB_CLUSTER_REGISTER_CALLBACK_IMPL(cluster_name, tag_name) \
    esp_err_t zb_##cluster_name##_register_callback(zb_##cluster_name##_state_cb_t callback) \
    { \
        s_##cluster_name##_callback = callback; \
        ESP_LOGI(tag_name, #cluster_name " state callback registered"); \
        return ESP_OK; \
    }

/**
 * @brief Implement has cluster check function
 *
 * @param cluster_name Lower-case cluster name
 * @param cluster_id ZCL cluster ID constant
 */
#define ZB_CLUSTER_HAS_CHECK_IMPL(cluster_name, cluster_id) \
    bool zb_device_has_##cluster_name(uint16_t short_addr) \
    { \
        return zb_device_has_cluster(short_addr, cluster_id); \
    }

/**
 * @brief Implement get state function
 *
 * @param cluster_name Lower-case cluster name
 * @param state_type State structure type
 */
#define ZB_CLUSTER_GET_STATE_IMPL(cluster_name, state_type) \
    esp_err_t zb_##cluster_name##_get_state(uint16_t short_addr, state_type *state) \
    { \
        if (state == NULL) { \
            return ESP_ERR_INVALID_ARG; \
        } \
        int idx = find_##cluster_name##_state_index(short_addr); \
        if (idx < 0) { \
            return ESP_ERR_NOT_FOUND; \
        } \
        memcpy(state, &s_##cluster_name##_states[idx], sizeof(state_type)); \
        return ESP_OK; \
    }

/* ============================================================================
 * Common ZCL Command Building Helpers
 * ============================================================================ */

/**
 * @brief Build a standard read attributes command request
 */
static inline esp_zb_zcl_read_attr_cmd_t zb_build_read_attr_cmd(
    uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id)
{
    esp_zb_zcl_read_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
        .attr_number = 0,
        .attr_field = NULL,
    };
    return cmd;
}

/**
 * @brief Build a standard write attribute command request
 */
static inline esp_zb_zcl_write_attr_cmd_t zb_build_write_attr_cmd(
    uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id)
{
    esp_zb_zcl_write_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
        .attr_number = 1,
        .attr_field = NULL,
    };
    return cmd;
}

/**
 * @brief Build a custom cluster command request
 */
static inline esp_zb_zcl_custom_cluster_cmd_t zb_build_custom_cmd(
    uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id, uint8_t cmd_id)
{
    esp_zb_zcl_custom_cluster_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = cluster_id,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .custom_cmd_id = cmd_id,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_NULL,
            .size = 0,
            .value = NULL,
        },
    };
    return cmd;
}

/**
 * @brief Send a read attributes command (thread-safe with Zigbee lock)
 */
static inline esp_err_t zb_send_read_attr_cmd(esp_zb_zcl_read_attr_cmd_t *cmd_req,
                                               const char *tag, const char *context)
{
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_read_attr_cmd_req(cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(tag, "Sent %s read request (TSN=%d)", context, tsn);
    return ESP_OK;
}

/**
 * @brief Send a write attribute command (thread-safe with Zigbee lock)
 */
static inline esp_err_t zb_send_write_attr_cmd(esp_zb_zcl_write_attr_cmd_t *cmd_req,
                                                const char *tag, const char *context)
{
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_write_attr_cmd_req(cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(tag, "Sent %s write request (TSN=%d)", context, tsn);
    return ESP_OK;
}

/**
 * @brief Send a custom cluster command (thread-safe with Zigbee lock)
 */
static inline esp_err_t zb_send_custom_cmd(esp_zb_zcl_custom_cluster_cmd_t *cmd_req,
                                            const char *tag, const char *context)
{
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(tag, "Sent %s command (TSN=%d)", context, tsn);
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* ZB_CLUSTER_INTERNAL_H */
