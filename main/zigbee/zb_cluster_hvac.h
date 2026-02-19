/**
 * @file zb_cluster_hvac.h
 * @brief Thermostat (0x0201) and Fan Control (0x0202) Cluster APIs
 *
 * Provides control for HVAC devices including thermostats and fans.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_CLUSTER_HVAC_H
#define ZB_CLUSTER_HVAC_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types moved from zb_device_handler_types.h
 * ============================================================================ */

/** @brief Maximum thermostat devices tracked */
#define ZB_STATE_MAX_THERMOSTAT             16

/** @brief Maximum fan control devices tracked */
#define ZB_STATE_MAX_FAN_CONTROL            16

/* ============================================================================
 * Thermostat Cluster (0x0201) Types
 * ============================================================================ */

/**
 * @brief Thermostat Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_THERMOSTAT                    0x0201

/**
 * @brief Thermostat Cluster Attribute IDs
 */
#define ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID             0x0000  /**< Local temperature (int16, 0.01C units) */
#define ZB_ZCL_ATTR_THERMOSTAT_OUTDOOR_TEMPERATURE_ID           0x0001  /**< Outdoor temperature (int16, 0.01C units) */
#define ZB_ZCL_ATTR_THERMOSTAT_OCCUPANCY_ID                     0x0002  /**< Occupancy (bitmap8) */
#define ZB_ZCL_ATTR_THERMOSTAT_ABS_MIN_HEAT_SETPOINT_ID         0x0003  /**< Abs min heat setpoint */
#define ZB_ZCL_ATTR_THERMOSTAT_ABS_MAX_HEAT_SETPOINT_ID         0x0004  /**< Abs max heat setpoint */
#define ZB_ZCL_ATTR_THERMOSTAT_ABS_MIN_COOL_SETPOINT_ID         0x0005  /**< Abs min cool setpoint */
#define ZB_ZCL_ATTR_THERMOSTAT_ABS_MAX_COOL_SETPOINT_ID         0x0006  /**< Abs max cool setpoint */
#define ZB_ZCL_ATTR_THERMOSTAT_PI_COOLING_DEMAND_ID             0x0007  /**< PI cooling demand */
#define ZB_ZCL_ATTR_THERMOSTAT_PI_HEATING_DEMAND_ID             0x0008  /**< PI heating demand */
#define ZB_ZCL_ATTR_THERMOSTAT_HVAC_SYSTEM_TYPE_ID              0x0009  /**< HVAC system type config */
#define ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMP_CALIBRATION_ID        0x0010  /**< Local temperature calibration */
#define ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID     0x0011  /**< Occupied cooling setpoint */
#define ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID     0x0012  /**< Occupied heating setpoint */
#define ZB_ZCL_ATTR_THERMOSTAT_UNOCCUPIED_COOLING_SETPOINT_ID   0x0013  /**< Unoccupied cooling setpoint */
#define ZB_ZCL_ATTR_THERMOSTAT_UNOCCUPIED_HEATING_SETPOINT_ID   0x0014  /**< Unoccupied heating setpoint */
#define ZB_ZCL_ATTR_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT_ID       0x0015  /**< Min heat setpoint limit */
#define ZB_ZCL_ATTR_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT_ID       0x0016  /**< Max heat setpoint limit */
#define ZB_ZCL_ATTR_THERMOSTAT_MIN_COOL_SETPOINT_LIMIT_ID       0x0017  /**< Min cool setpoint limit */
#define ZB_ZCL_ATTR_THERMOSTAT_MAX_COOL_SETPOINT_LIMIT_ID       0x0018  /**< Max cool setpoint limit */
#define ZB_ZCL_ATTR_THERMOSTAT_MIN_SETPOINT_DEAD_BAND_ID        0x0019  /**< Min setpoint dead band */
#define ZB_ZCL_ATTR_THERMOSTAT_REMOTE_SENSING_ID                0x001A  /**< Remote sensing */
#define ZB_ZCL_ATTR_THERMOSTAT_CONTROL_SEQUENCE_OF_OPERATION_ID 0x001B  /**< Control sequence of operation */
#define ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID                   0x001C  /**< System mode */
#define ZB_ZCL_ATTR_THERMOSTAT_ALARM_MASK_ID                    0x001D  /**< Alarm mask */
#define ZB_ZCL_ATTR_THERMOSTAT_RUNNING_MODE_ID                  0x001E  /**< Thermostat running mode */

/**
 * @brief Thermostat Cluster Command IDs
 */
#define ZB_ZCL_CMD_THERMOSTAT_SETPOINT_RAISE_LOWER_ID           0x00    /**< Setpoint raise/lower */
#define ZB_ZCL_CMD_THERMOSTAT_SET_WEEKLY_SCHEDULE_ID            0x01    /**< Set weekly schedule */
#define ZB_ZCL_CMD_THERMOSTAT_GET_WEEKLY_SCHEDULE_ID            0x02    /**< Get weekly schedule */
#define ZB_ZCL_CMD_THERMOSTAT_CLEAR_WEEKLY_SCHEDULE_ID          0x03    /**< Clear weekly schedule */
#define ZB_ZCL_CMD_THERMOSTAT_GET_RELAY_STATUS_LOG_ID           0x04    /**< Get relay status log */

/**
 * @brief Thermostat System Mode values (enum8)
 */
typedef enum {
    ZB_THERMOSTAT_SYSTEM_MODE_OFF = 0x00,           /**< Off */
    ZB_THERMOSTAT_SYSTEM_MODE_AUTO = 0x01,          /**< Auto */
    ZB_THERMOSTAT_SYSTEM_MODE_COOL = 0x03,          /**< Cool */
    ZB_THERMOSTAT_SYSTEM_MODE_HEAT = 0x04,          /**< Heat */
    ZB_THERMOSTAT_SYSTEM_MODE_EMERGENCY_HEAT = 0x05, /**< Emergency heating */
    ZB_THERMOSTAT_SYSTEM_MODE_PRECOOLING = 0x06,    /**< Precooling */
    ZB_THERMOSTAT_SYSTEM_MODE_FAN_ONLY = 0x07,      /**< Fan only */
    ZB_THERMOSTAT_SYSTEM_MODE_DRY = 0x08,           /**< Dry */
    ZB_THERMOSTAT_SYSTEM_MODE_SLEEP = 0x09          /**< Sleep */
} zb_thermostat_system_mode_t;

/**
 * @brief Thermostat Running Mode values (enum8)
 */
typedef enum {
    ZB_THERMOSTAT_RUNNING_MODE_OFF = 0x00,          /**< Off */
    ZB_THERMOSTAT_RUNNING_MODE_COOL = 0x03,         /**< Cool */
    ZB_THERMOSTAT_RUNNING_MODE_HEAT = 0x04          /**< Heat */
} zb_thermostat_running_mode_t;

/**
 * @brief Thermostat state structure
 */
typedef struct {
    int16_t local_temperature;          /**< Local temperature (0.01C units) */
    int16_t occupied_cooling_setpoint;  /**< Occupied cooling setpoint (0.01C units) */
    int16_t occupied_heating_setpoint;  /**< Occupied heating setpoint (0.01C units) */
    int16_t min_heat_setpoint_limit;    /**< Min heat setpoint limit (0.01C units) */
    int16_t max_heat_setpoint_limit;    /**< Max heat setpoint limit (0.01C units) */
    int16_t min_cool_setpoint_limit;    /**< Min cool setpoint limit (0.01C units) */
    int16_t max_cool_setpoint_limit;    /**< Max cool setpoint limit (0.01C units) */
    zb_thermostat_system_mode_t system_mode;   /**< System mode */
    zb_thermostat_running_mode_t running_mode; /**< Running mode */
    uint8_t pi_heating_demand;          /**< PI heating demand (0-100%) */
    uint8_t pi_cooling_demand;          /**< PI cooling demand (0-100%) */
} zb_thermostat_state_t;

/**
 * @brief Thermostat state change callback type
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current thermostat state
 */
typedef void (*zb_thermostat_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                          const zb_thermostat_state_t *state);

/* ============================================================================
 * Fan Control Cluster (0x0202) Types
 * ============================================================================ */

/**
 * @brief Fan Control Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_FAN_CONTROL                   0x0202

/**
 * @brief Fan Control Cluster Attribute IDs
 */
#define ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID             0x0000  /**< Fan mode (enum8) */
#define ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_SEQUENCE_ID    0x0001  /**< Fan mode sequence (enum8) */

/**
 * @brief Fan Mode values (enum8)
 */
typedef enum {
    ZB_FAN_MODE_OFF = 0x00,         /**< Off */
    ZB_FAN_MODE_LOW = 0x01,         /**< Low */
    ZB_FAN_MODE_MEDIUM = 0x02,      /**< Medium */
    ZB_FAN_MODE_HIGH = 0x03,        /**< High */
    ZB_FAN_MODE_ON = 0x04,          /**< On */
    ZB_FAN_MODE_AUTO = 0x05,        /**< Auto */
    ZB_FAN_MODE_SMART = 0x06        /**< Smart */
} zb_fan_mode_t;

/**
 * @brief Fan Mode Sequence values (enum8)
 * Defines which fan modes are supported
 */
typedef enum {
    ZB_FAN_MODE_SEQ_LOW_MED_HIGH = 0x00,      /**< Low/Med/High */
    ZB_FAN_MODE_SEQ_LOW_HIGH = 0x01,          /**< Low/High */
    ZB_FAN_MODE_SEQ_LOW_MED_HIGH_AUTO = 0x02, /**< Low/Med/High/Auto */
    ZB_FAN_MODE_SEQ_LOW_HIGH_AUTO = 0x03,     /**< Low/High/Auto */
    ZB_FAN_MODE_SEQ_ON_AUTO = 0x04            /**< On/Auto */
} zb_fan_mode_sequence_t;

/**
 * @brief Fan Control state structure
 */
typedef struct {
    zb_fan_mode_t fan_mode;                 /**< Current fan mode */
    zb_fan_mode_sequence_t fan_mode_sequence; /**< Supported fan mode sequence */
} zb_fan_control_state_t;

/**
 * @brief Fan Control state change callback type
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current fan control state
 */
typedef void (*zb_fan_control_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                           const zb_fan_control_state_t *state);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

esp_err_t zb_cluster_hvac_init(void);
esp_err_t zb_cluster_hvac_deinit(void);
void zb_cluster_hvac_clear_all(void);
void zb_cluster_hvac_remove_device(uint16_t short_addr);

/* ============================================================================
 * Thermostat (0x0201) Public API
 * ============================================================================ */

esp_err_t zb_thermostat_register_callback(zb_thermostat_state_cb_t callback);
bool zb_device_has_thermostat(uint16_t short_addr);
esp_err_t zb_thermostat_read_state(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_thermostat_set_heating_setpoint(uint16_t short_addr, uint8_t endpoint,
                                              int16_t temperature);
esp_err_t zb_thermostat_set_cooling_setpoint(uint16_t short_addr, uint8_t endpoint,
                                              int16_t temperature);
esp_err_t zb_thermostat_set_system_mode(uint16_t short_addr, uint8_t endpoint,
                                         zb_thermostat_system_mode_t mode);
esp_err_t zb_thermostat_handle_report(uint16_t short_addr, uint8_t endpoint,
                                       uint16_t attr_id, void *value, size_t value_len);
esp_err_t zb_thermostat_get_state(uint16_t short_addr, zb_thermostat_state_t *state);

/* ============================================================================
 * Fan Control (0x0202) Public API
 * ============================================================================ */

esp_err_t zb_fan_control_register_callback(zb_fan_control_state_cb_t callback);
bool zb_device_has_fan_control(uint16_t short_addr);
esp_err_t zb_fan_control_read_state(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_fan_control_set_mode(uint16_t short_addr, uint8_t endpoint,
                                   zb_fan_mode_t mode);
esp_err_t zb_fan_control_handle_report(uint16_t short_addr, uint8_t endpoint,
                                        uint16_t attr_id, void *value, size_t value_len);
esp_err_t zb_fan_control_get_state(uint16_t short_addr, zb_fan_control_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CLUSTER_HVAC_H */
