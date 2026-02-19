/**
 * @file zb_cluster_closures.h
 * @brief Window Covering Cluster (0x0102) API
 *
 * Provides control for blinds, shades, curtains, and other window
 * covering devices.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_CLUSTER_CLOSURES_H
#define ZB_CLUSTER_CLOSURES_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types moved from zb_device_handler_types.h
 * ============================================================================ */

/** @brief Maximum window covering devices tracked */
#define ZB_STATE_MAX_WINDOW_COVERING        16

/* ============================================================================
 * Window Covering Cluster (0x0102) Types
 * ============================================================================ */

/**
 * @brief Window Covering Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_WINDOW_COVERING           0x0102

/**
 * @brief Window Covering Cluster Attribute IDs
 */
#define ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_ID                     0x0000
#define ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_STATUS_ID            0x0007
#define ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POS_LIFT_PERCENT_ID 0x0008
#define ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POS_TILT_PERCENT_ID 0x0009
#define ZB_ZCL_ATTR_WINDOW_COVERING_INSTALLED_OPEN_LIMIT_LIFT   0x0010
#define ZB_ZCL_ATTR_WINDOW_COVERING_INSTALLED_CLOSED_LIMIT_LIFT 0x0011
#define ZB_ZCL_ATTR_WINDOW_COVERING_MODE_ID                     0x0017

/**
 * @brief Window Covering Cluster Command IDs
 */
#define ZB_ZCL_CMD_WINDOW_COVERING_UP_OPEN              0x00
#define ZB_ZCL_CMD_WINDOW_COVERING_DOWN_CLOSE           0x01
#define ZB_ZCL_CMD_WINDOW_COVERING_STOP                 0x02
#define ZB_ZCL_CMD_WINDOW_COVERING_GO_TO_LIFT_VALUE     0x04
#define ZB_ZCL_CMD_WINDOW_COVERING_GO_TO_LIFT_PERCENT   0x05
#define ZB_ZCL_CMD_WINDOW_COVERING_GO_TO_TILT_VALUE     0x07
#define ZB_ZCL_CMD_WINDOW_COVERING_GO_TO_TILT_PERCENT   0x08

/**
 * @brief Window Covering state structure
 */
typedef struct {
    uint8_t current_position_lift;  /**< Current lift position (0-100%) */
    uint8_t current_position_tilt;  /**< Current tilt position (0-100%) */
    uint8_t covering_type;          /**< Covering type (roller, venetian, etc.) */
    bool is_moving;                 /**< True if cover is currently moving */
} zb_window_covering_state_t;

/**
 * @brief Window Covering state change callback type
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current window covering state
 */
typedef void (*zb_window_covering_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                               const zb_window_covering_state_t *state);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

esp_err_t zb_cluster_closures_init(void);
esp_err_t zb_cluster_closures_deinit(void);
void zb_cluster_closures_clear_all(void);
void zb_cluster_closures_remove_device(uint16_t short_addr);

/* ============================================================================
 * Window Covering Cluster (0x0102) Public API
 * ============================================================================ */

esp_err_t zb_window_covering_register_callback(zb_window_covering_state_cb_t callback);
bool zb_device_has_window_covering(uint16_t short_addr);
esp_err_t zb_window_covering_read_position(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_window_covering_cmd_up(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_window_covering_cmd_down(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_window_covering_cmd_stop(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_window_covering_cmd_goto_lift_percent(uint16_t short_addr, uint8_t endpoint,
                                                    uint8_t percentage);
esp_err_t zb_window_covering_cmd_goto_tilt_percent(uint16_t short_addr, uint8_t endpoint,
                                                    uint8_t percentage);
esp_err_t zb_window_covering_handle_report(uint16_t short_addr, uint8_t endpoint,
                                            uint16_t attr_id, void *value, size_t value_len);
esp_err_t zb_window_covering_get_state(uint16_t short_addr, zb_window_covering_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CLUSTER_CLOSURES_H */
