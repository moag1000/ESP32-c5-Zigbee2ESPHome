/**
 * @file cluster_state_ng.h
 * @brief NG Cluster State Management for Unified Device Model
 *
 * Provides cluster state storage using device_registry's cJSON state
 * instead of separate static arrays. This enables:
 *   - Single source of truth (device_registry)
 *   - Unlimited devices (no fixed array limits)
 *   - Event-driven state change notifications
 *   - JSON-based state for MQTT/HA discovery
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef CLUSTER_STATE_NG_H
#define CLUSTER_STATE_NG_H

#include "esp_err.h"
#include "core/device/unified_device.h"
#include "zb_device_handler_types.h"
#include "zb_cluster_hvac.h"
#include "zb_cluster_electrical.h"
#include "zb_cluster_security.h"
#include "zb_cluster_closures.h"
#include "zb_cluster_measurement.h"
#include "zb_cluster_multistate.h"
#include "cJSON.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * JSON State Keys
 *
 * Standard keys used in device state JSON objects.
 * These match zigbee2mqtt conventions for compatibility.
 * ============================================================================ */

/* Common keys */
#define STATE_KEY_STATE             "state"             /**< ON/OFF state */
#define STATE_KEY_BRIGHTNESS        "brightness"        /**< 0-255 brightness */
#define STATE_KEY_TEMPERATURE       "temperature"       /**< Temperature in C */
#define STATE_KEY_HUMIDITY          "humidity"          /**< Humidity in % */
#define STATE_KEY_BATTERY           "battery"           /**< Battery percentage */

/* Thermostat keys */
#define STATE_KEY_LOCAL_TEMP        "local_temperature"
#define STATE_KEY_HEATING_SETPOINT  "occupied_heating_setpoint"
#define STATE_KEY_COOLING_SETPOINT  "occupied_cooling_setpoint"
#define STATE_KEY_SYSTEM_MODE       "system_mode"
#define STATE_KEY_RUNNING_MODE      "running_mode"
#define STATE_KEY_PI_HEATING        "pi_heating_demand"
#define STATE_KEY_PI_COOLING        "pi_cooling_demand"

/* Door lock keys */
#define STATE_KEY_LOCK_STATE        "lock_state"
#define STATE_KEY_LOCK_TYPE         "lock_type"
#define STATE_KEY_ACTUATOR_ENABLED  "actuator_enabled"

/* Window covering keys */
#define STATE_KEY_POSITION          "position"
#define STATE_KEY_TILT              "tilt"
#define STATE_KEY_MOVING            "moving"
#define STATE_KEY_COVERING_TYPE     "covering_type"

/* Fan control keys */
#define STATE_KEY_FAN_MODE          "fan_mode"
#define STATE_KEY_FAN_MODE_SEQUENCE "fan_mode_sequence"

/* Electrical measurement keys */
#define STATE_KEY_VOLTAGE           "voltage"
#define STATE_KEY_CURRENT           "current"
#define STATE_KEY_POWER             "power"
#define STATE_KEY_ENERGY            "energy"
#define STATE_KEY_POWER_FACTOR      "power_factor"
#define STATE_KEY_FREQUENCY         "frequency"

/* Metering keys */
#define STATE_KEY_TOTAL_ENERGY      "total_energy"
#define STATE_KEY_INSTANT_DEMAND    "instantaneous_demand"
#define STATE_KEY_CURRENT_DAY       "current_day_consumption"
#define STATE_KEY_PREVIOUS_DAY      "previous_day_consumption"
#define STATE_KEY_METER_TYPE        "meter_type"
#define STATE_KEY_UNIT_OF_MEASURE   "unit_of_measure"

/* IAS Zone keys */
#define STATE_KEY_ZONE_STATUS       "zone_status"
#define STATE_KEY_ZONE_TYPE         "zone_type"
#define STATE_KEY_ZONE_STATE        "zone_state"
#define STATE_KEY_ALARM1            "alarm_1"
#define STATE_KEY_ALARM2            "alarm_2"
#define STATE_KEY_TAMPER            "tamper"
#define STATE_KEY_LOW_BATTERY       "battery_low"
#define STATE_KEY_SUPERVISION       "supervision_reports"
#define STATE_KEY_RESTORE           "restore_reports"
#define STATE_KEY_TROUBLE           "trouble"
#define STATE_KEY_AC_MAINS          "ac_mains_fault"
#define STATE_KEY_OCCUPANCY         "occupancy"
#define STATE_KEY_CONTACT           "contact"

/* Illuminance keys */
#define STATE_KEY_ILLUMINANCE       "illuminance"
#define STATE_KEY_ILLUMINANCE_LUX   "illuminance_lux"

/* Pressure keys */
#define STATE_KEY_PRESSURE          "pressure"

/* PM2.5 keys */
#define STATE_KEY_PM25              "pm25"

/* Binary/Multistate keys */
#define STATE_KEY_PRESENT_VALUE     "present_value"
#define STATE_KEY_OUT_OF_SERVICE    "out_of_service"
#define STATE_KEY_STATUS_FLAGS      "status_flags"
#define STATE_KEY_IN_ALARM          "in_alarm"
#define STATE_KEY_FAULT             "fault"
#define STATE_KEY_OVERRIDDEN        "overridden"
#define STATE_KEY_NUMBER_OF_STATES  "number_of_states"
#define STATE_KEY_STATE_TEXT        "state_text"

/* ============================================================================
 * Thermostat State Functions
 * ============================================================================ */

/**
 * @brief Store thermostat state in device registry
 *
 * @param id Device ID
 * @param state Thermostat state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_thermostat(device_id_t id, const zb_thermostat_state_t *state);

/**
 * @brief Get thermostat state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_thermostat(device_id_t id, zb_thermostat_state_t *state);

/* ============================================================================
 * Door Lock State Functions
 * ============================================================================ */

/**
 * @brief Store door lock state in device registry
 *
 * @param id Device ID
 * @param state Door lock state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_door_lock(device_id_t id, const zb_door_lock_state_struct_t *state);

/**
 * @brief Get door lock state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_door_lock(device_id_t id, zb_door_lock_state_struct_t *state);

/* ============================================================================
 * Window Covering State Functions
 * ============================================================================ */

/**
 * @brief Store window covering state in device registry
 *
 * @param id Device ID
 * @param state Window covering state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_window_covering(device_id_t id, const zb_window_covering_state_t *state);

/**
 * @brief Get window covering state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_window_covering(device_id_t id, zb_window_covering_state_t *state);

/* ============================================================================
 * Fan Control State Functions
 * ============================================================================ */

/**
 * @brief Store fan control state in device registry
 *
 * @param id Device ID
 * @param state Fan control state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_fan_control(device_id_t id, const zb_fan_control_state_t *state);

/**
 * @brief Get fan control state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_fan_control(device_id_t id, zb_fan_control_state_t *state);

/* ============================================================================
 * Electrical Measurement State Functions
 * ============================================================================ */

/**
 * @brief Store electrical measurement state in device registry
 *
 * @param id Device ID
 * @param state Electrical measurement state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_electrical(device_id_t id, const zb_electrical_state_t *state);

/**
 * @brief Get electrical measurement state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_electrical(device_id_t id, zb_electrical_state_t *state);

/* ============================================================================
 * Metering State Functions
 * ============================================================================ */

/**
 * @brief Store metering state in device registry
 *
 * @param id Device ID
 * @param state Metering state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_metering(device_id_t id, const zb_metering_state_t *state);

/**
 * @brief Get metering state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_metering(device_id_t id, zb_metering_state_t *state);

/* ============================================================================
 * IAS Zone State Functions
 * ============================================================================ */

/**
 * @brief Store IAS Zone state in device registry
 *
 * @param id Device ID
 * @param state IAS Zone state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_ias_zone(device_id_t id, const zb_ias_zone_state_struct_t *state);

/**
 * @brief Get IAS Zone state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_ias_zone(device_id_t id, zb_ias_zone_state_struct_t *state);

/* ============================================================================
 * Illuminance State Functions
 * ============================================================================ */

/**
 * @brief Store illuminance state in device registry
 *
 * @param id Device ID
 * @param state Illuminance state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_illuminance(device_id_t id, const zb_illuminance_state_t *state);

/**
 * @brief Get illuminance state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_illuminance(device_id_t id, zb_illuminance_state_t *state);

/* ============================================================================
 * Pressure State Functions
 * ============================================================================ */

/**
 * @brief Store pressure state in device registry
 *
 * @param id Device ID
 * @param state Pressure state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_pressure(device_id_t id, const zb_pressure_state_t *state);

/**
 * @brief Get pressure state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_pressure(device_id_t id, zb_pressure_state_t *state);

/* ============================================================================
 * PM2.5 State Functions
 * ============================================================================ */

/**
 * @brief Store PM2.5 state in device registry
 *
 * @param id Device ID
 * @param state PM2.5 state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_pm25(device_id_t id, const zb_pm25_state_t *state);

/**
 * @brief Get PM2.5 state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_pm25(device_id_t id, zb_pm25_state_t *state);

/* ============================================================================
 * Binary Output State Functions
 * ============================================================================ */

/**
 * @brief Store binary output state in device registry
 *
 * @param id Device ID
 * @param state Binary output state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_binary_output(device_id_t id, const zb_binary_output_state_t *state);

/**
 * @brief Get binary output state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_binary_output(device_id_t id, zb_binary_output_state_t *state);

/* ============================================================================
 * Binary Value State Functions
 * ============================================================================ */

/**
 * @brief Store binary value state in device registry
 *
 * @param id Device ID
 * @param state Binary value state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_binary_value(device_id_t id, const zb_binary_value_state_t *state);

/**
 * @brief Get binary value state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_binary_value(device_id_t id, zb_binary_value_state_t *state);

/* ============================================================================
 * Multistate State Functions
 * ============================================================================ */

/**
 * @brief Store multistate state in device registry
 *
 * @param id Device ID
 * @param state Multistate state structure
 * @return ESP_OK on success
 */
esp_err_t cluster_state_set_multistate(device_id_t id, const zb_multistate_state_t *state);

/**
 * @brief Get multistate state from device registry
 *
 * @param id Device ID
 * @param state Output state structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not set
 */
esp_err_t cluster_state_get_multistate(device_id_t id, zb_multistate_state_t *state);

/* ============================================================================
 * Generic State Update Functions
 * ============================================================================ */

/**
 * @brief Update a single numeric value in device state
 *
 * @param id Device ID
 * @param key JSON key name
 * @param value Numeric value to set
 * @return ESP_OK on success
 */
esp_err_t cluster_state_update_number(device_id_t id, const char *key, double value);

/**
 * @brief Update a single boolean value in device state
 *
 * @param id Device ID
 * @param key JSON key name
 * @param value Boolean value to set
 * @return ESP_OK on success
 */
esp_err_t cluster_state_update_bool(device_id_t id, const char *key, bool value);

/**
 * @brief Update a single string value in device state
 *
 * @param id Device ID
 * @param key JSON key name
 * @param value String value to set
 * @return ESP_OK on success
 */
esp_err_t cluster_state_update_string(device_id_t id, const char *key, const char *value);

/* ============================================================================
 * Event Publishing
 * ============================================================================ */

/**
 * @brief Publish state change event for a device
 *
 * Should be called after updating device state to notify subscribers.
 *
 * @param id Device ID
 * @return ESP_OK on success
 */
esp_err_t cluster_state_publish_change(device_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* CLUSTER_STATE_NG_H */
