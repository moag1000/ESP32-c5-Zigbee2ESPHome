/**
 * @file cluster_state_ng.c
 * @brief NG Cluster State Management Implementation
 *
 * Implements cluster state storage using device_registry's cJSON state.
 * All state is stored in a single JSON object per device, avoiding the
 * need for separate static arrays per cluster type.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "cluster_state_ng.h"
#include "zb_constants.h"
#include "core/device/device_registry.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include <string.h>
#include <math.h>

/* ============================================================================
 * Generic State Update Functions
 * ============================================================================ */

esp_err_t cluster_state_update_number(device_id_t id, const char *key, double value)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json_value = cJSON_CreateNumber(value);
    if (!json_value) {
        return ESP_ERR_NO_MEM;
    }

    return device_registry_update_state(id, key, json_value);
}

esp_err_t cluster_state_update_bool(device_id_t id, const char *key, bool value)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json_value = cJSON_CreateBool(value);
    if (!json_value) {
        return ESP_ERR_NO_MEM;
    }

    return device_registry_update_state(id, key, json_value);
}

esp_err_t cluster_state_update_string(device_id_t id, const char *key, const char *value)
{
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json_value = cJSON_CreateString(value);
    if (!json_value) {
        return ESP_ERR_NO_MEM;
    }

    return device_registry_update_state(id, key, json_value);
}

/* ============================================================================
 * Event Publishing
 * ============================================================================ */

esp_err_t cluster_state_publish_change(device_id_t id)
{
    device_t *dev = device_registry_get(id);
    if (!dev) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Publish the documented payload type. Two things must not be done here:
     *
     * 1. Do NOT publish an ad-hoc struct. Subscribers cast the payload to
     *    evt_device_state_t (24 bytes on RV32); event_publish() allocates
     *    exactly data_size bytes, so a smaller struct makes the handler read
     *    json_state from past the end of the allocation.
     *
     * 2. Do NOT put the registry-owned cJSON pointer in the event. Dispatch is
     *    asynchronous, and the next report can cJSON_Delete() that object via
     *    device_registry_set_state() before the handler runs.
     *
     * json_state stays NULL; handlers read the current state from
     * device_registry_get_state() themselves, the same way ble_adapter.c,
     * mqtt_bridge.c and device_state_publisher.c already do. */
    /* proto is a union — only read the Zigbee arm for Zigbee devices. */
    evt_device_state_t evt = {
        .ieee_addr  = id,
        .short_addr = 0,
        .endpoint   = 0,
        .cluster_id = 0,
        .attr_id    = 0,
        .json_state = NULL,
    };

    if (dev->protocol == DEV_PROTOCOL_ZIGBEE) {
        evt.short_addr = dev->proto.zigbee.short_addr;
        evt.endpoint   = dev->proto.zigbee.endpoint;
    }

    return event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));
}

/* ============================================================================
 * Thermostat State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_thermostat(device_id_t id, const zb_thermostat_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    /* Temperature values stored in 0.01C units, convert to C for JSON */
    cJSON_AddNumberToObject(partial, STATE_KEY_LOCAL_TEMP,
                            state->local_temperature / 100.0);
    cJSON_AddNumberToObject(partial, STATE_KEY_HEATING_SETPOINT,
                            state->occupied_heating_setpoint / 100.0);
    cJSON_AddNumberToObject(partial, STATE_KEY_COOLING_SETPOINT,
                            state->occupied_cooling_setpoint / 100.0);
    cJSON_AddNumberToObject(partial, STATE_KEY_SYSTEM_MODE, state->system_mode);
    cJSON_AddNumberToObject(partial, STATE_KEY_RUNNING_MODE, state->running_mode);
    cJSON_AddNumberToObject(partial, STATE_KEY_PI_HEATING, state->pi_heating_demand);
    cJSON_AddNumberToObject(partial, STATE_KEY_PI_COOLING, state->pi_cooling_demand);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_thermostat(device_id_t id, zb_thermostat_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_thermostat_state_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_LOCAL_TEMP)) && cJSON_IsNumber(item)) {
        state->local_temperature = (int16_t)(item->valuedouble * 100);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_HEATING_SETPOINT)) && cJSON_IsNumber(item)) {
        state->occupied_heating_setpoint = (int16_t)(item->valuedouble * 100);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_COOLING_SETPOINT)) && cJSON_IsNumber(item)) {
        state->occupied_cooling_setpoint = (int16_t)(item->valuedouble * 100);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_SYSTEM_MODE)) && cJSON_IsNumber(item)) {
        state->system_mode = (zb_thermostat_system_mode_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_RUNNING_MODE)) && cJSON_IsNumber(item)) {
        state->running_mode = (zb_thermostat_running_mode_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_PI_HEATING)) && cJSON_IsNumber(item)) {
        state->pi_heating_demand = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_PI_COOLING)) && cJSON_IsNumber(item)) {
        state->pi_cooling_demand = (uint8_t)item->valueint;
    }

    return ESP_OK;
}

/* ============================================================================
 * Door Lock State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_door_lock(device_id_t id, const zb_door_lock_state_struct_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    const char *lock_str = "unknown";
    switch (state->lock_state) {
        case ZB_DOOR_LOCK_STATE_LOCKED:
            lock_str = "LOCKED";
            break;
        case ZB_DOOR_LOCK_STATE_UNLOCKED:
            lock_str = "UNLOCKED";
            break;
        case ZB_DOOR_LOCK_STATE_NOT_FULLY_LOCKED:
            lock_str = "NOT_FULLY_LOCKED";
            break;
    }
    cJSON_AddStringToObject(partial, STATE_KEY_LOCK_STATE, lock_str);
    cJSON_AddNumberToObject(partial, STATE_KEY_LOCK_TYPE, state->lock_type);
    cJSON_AddBoolToObject(partial, STATE_KEY_ACTUATOR_ENABLED, state->actuator_enabled);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_door_lock(device_id_t id, zb_door_lock_state_struct_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_door_lock_state_struct_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_LOCK_STATE)) && cJSON_IsString(item)) {
        if (strcmp(item->valuestring, "LOCKED") == 0) {
            state->lock_state = ZB_DOOR_LOCK_STATE_LOCKED;
        } else if (strcmp(item->valuestring, "UNLOCKED") == 0) {
            state->lock_state = ZB_DOOR_LOCK_STATE_UNLOCKED;
        } else {
            state->lock_state = ZB_DOOR_LOCK_STATE_NOT_FULLY_LOCKED;
        }
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_LOCK_TYPE)) && cJSON_IsNumber(item)) {
        state->lock_type = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_ACTUATOR_ENABLED)) && cJSON_IsBool(item)) {
        state->actuator_enabled = cJSON_IsTrue(item);
    }

    return ESP_OK;
}

/* ============================================================================
 * Window Covering State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_window_covering(device_id_t id, const zb_window_covering_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(partial, STATE_KEY_POSITION, state->current_position_lift);
    cJSON_AddNumberToObject(partial, STATE_KEY_TILT, state->current_position_tilt);
    cJSON_AddNumberToObject(partial, STATE_KEY_COVERING_TYPE, state->covering_type);
    cJSON_AddBoolToObject(partial, STATE_KEY_MOVING, state->is_moving);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_window_covering(device_id_t id, zb_window_covering_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_window_covering_state_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_POSITION)) && cJSON_IsNumber(item)) {
        state->current_position_lift = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_TILT)) && cJSON_IsNumber(item)) {
        state->current_position_tilt = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_COVERING_TYPE)) && cJSON_IsNumber(item)) {
        state->covering_type = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_MOVING)) && cJSON_IsBool(item)) {
        state->is_moving = cJSON_IsTrue(item);
    }

    return ESP_OK;
}

/* ============================================================================
 * Fan Control State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_fan_control(device_id_t id, const zb_fan_control_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    const char *mode_str = "off";
    switch (state->fan_mode) {
        case ZB_FAN_MODE_OFF:   mode_str = "off"; break;
        case ZB_FAN_MODE_LOW:   mode_str = "low"; break;
        case ZB_FAN_MODE_MEDIUM: mode_str = "medium"; break;
        case ZB_FAN_MODE_HIGH:  mode_str = "high"; break;
        case ZB_FAN_MODE_ON:    mode_str = "on"; break;
        case ZB_FAN_MODE_AUTO:  mode_str = "auto"; break;
        case ZB_FAN_MODE_SMART: mode_str = "smart"; break;
    }
    cJSON_AddStringToObject(partial, STATE_KEY_FAN_MODE, mode_str);
    cJSON_AddNumberToObject(partial, STATE_KEY_FAN_MODE_SEQUENCE, state->fan_mode_sequence);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_fan_control(device_id_t id, zb_fan_control_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_fan_control_state_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_FAN_MODE)) && cJSON_IsString(item)) {
        const char *mode = item->valuestring;
        if (strcmp(mode, "off") == 0) state->fan_mode = ZB_FAN_MODE_OFF;
        else if (strcmp(mode, "low") == 0) state->fan_mode = ZB_FAN_MODE_LOW;
        else if (strcmp(mode, "medium") == 0) state->fan_mode = ZB_FAN_MODE_MEDIUM;
        else if (strcmp(mode, "high") == 0) state->fan_mode = ZB_FAN_MODE_HIGH;
        else if (strcmp(mode, "on") == 0) state->fan_mode = ZB_FAN_MODE_ON;
        else if (strcmp(mode, "auto") == 0) state->fan_mode = ZB_FAN_MODE_AUTO;
        else if (strcmp(mode, "smart") == 0) state->fan_mode = ZB_FAN_MODE_SMART;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_FAN_MODE_SEQUENCE)) && cJSON_IsNumber(item)) {
        state->fan_mode_sequence = (zb_fan_mode_sequence_t)item->valueint;
    }

    return ESP_OK;
}

/* ============================================================================
 * Electrical Measurement State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_electrical(device_id_t id, const zb_electrical_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    /* Calculate scaled values */
    double voltage = 0, current = 0, power = 0;

    if (state->voltage_divisor > 0) {
        voltage = (double)state->rms_voltage * state->voltage_multiplier / state->voltage_divisor;
    }
    if (state->current_divisor > 0) {
        current = (double)state->rms_current * state->current_multiplier / state->current_divisor;
    }
    if (state->power_divisor > 0) {
        power = (double)state->active_power * state->power_multiplier / state->power_divisor;
    }

    cJSON_AddNumberToObject(partial, STATE_KEY_VOLTAGE, voltage);
    cJSON_AddNumberToObject(partial, STATE_KEY_CURRENT, current);
    cJSON_AddNumberToObject(partial, STATE_KEY_POWER, power);
    cJSON_AddNumberToObject(partial, STATE_KEY_POWER_FACTOR, state->power_factor / 100.0);

    if (state->ac_frequency > 0 && state->power_divisor > 0) {
        cJSON_AddNumberToObject(partial, STATE_KEY_FREQUENCY, state->ac_frequency / 10.0);
    }

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_electrical(device_id_t id, zb_electrical_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Initialize with defaults */
    memset(state, 0, sizeof(zb_electrical_state_t));
    state->voltage_multiplier = 1;
    state->voltage_divisor = ZB_ELECTRICAL_VOLTAGE_DIVISOR;
    state->current_multiplier = 1;
    state->current_divisor = ZB_ELECTRICAL_CURRENT_DIVISOR_MA;
    state->power_multiplier = 1;
    state->power_divisor = ZB_ELECTRICAL_POWER_DIVISOR;

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_VOLTAGE)) && cJSON_IsNumber(item)) {
        state->rms_voltage = (uint16_t)(item->valuedouble * ZB_ELECTRICAL_VOLTAGE_DIVISOR);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_CURRENT)) && cJSON_IsNumber(item)) {
        state->rms_current = (uint16_t)(item->valuedouble * ZB_ELECTRICAL_CURRENT_DIVISOR_MA);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_POWER)) && cJSON_IsNumber(item)) {
        state->active_power = (int16_t)(item->valuedouble * ZB_ELECTRICAL_POWER_DIVISOR);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_POWER_FACTOR)) && cJSON_IsNumber(item)) {
        state->power_factor = (int8_t)(item->valuedouble * 100);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_FREQUENCY)) && cJSON_IsNumber(item)) {
        state->ac_frequency = (uint16_t)(item->valuedouble * 10);
    }

    state->is_valid = true;
    return ESP_OK;
}

/* ============================================================================
 * Metering State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_metering(device_id_t id, const zb_metering_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    /* Calculate scaled total energy */
    double total = 0;
    if (state->divisor > 0) {
        total = (double)state->current_summation_delivered * state->multiplier / state->divisor;
    }

    cJSON_AddNumberToObject(partial, STATE_KEY_TOTAL_ENERGY, total);

    /* Instantaneous demand (power) */
    if (state->divisor > 0) {
        double power = (double)state->instantaneous_demand * state->multiplier / state->divisor;
        /* Convert to watts if in kWh mode */
        if (state->unit_of_measure == ZB_METERING_UNIT_KWH) {
            power *= 1000.0;
        }
        cJSON_AddNumberToObject(partial, STATE_KEY_INSTANT_DEMAND, power);
    }

    cJSON_AddNumberToObject(partial, STATE_KEY_METER_TYPE, state->metering_device_type);
    cJSON_AddNumberToObject(partial, STATE_KEY_UNIT_OF_MEASURE, state->unit_of_measure);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_metering(device_id_t id, zb_metering_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_metering_state_t));
    state->multiplier = 1;
    state->divisor = 1;

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_TOTAL_ENERGY)) && cJSON_IsNumber(item)) {
        state->current_summation_delivered = (uint64_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_INSTANT_DEMAND)) && cJSON_IsNumber(item)) {
        state->instantaneous_demand = (int32_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_METER_TYPE)) && cJSON_IsNumber(item)) {
        state->metering_device_type = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_UNIT_OF_MEASURE)) && cJSON_IsNumber(item)) {
        state->unit_of_measure = (uint8_t)item->valueint;
    }

    return ESP_OK;
}

/* ============================================================================
 * IAS Zone State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_ias_zone(device_id_t id, const zb_ias_zone_state_struct_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(partial, STATE_KEY_ZONE_STATUS, state->zone_status);
    cJSON_AddNumberToObject(partial, STATE_KEY_ZONE_TYPE, state->zone_type);
    cJSON_AddBoolToObject(partial, STATE_KEY_ALARM1, state->alarm1);
    cJSON_AddBoolToObject(partial, STATE_KEY_ALARM2, state->alarm2);
    cJSON_AddBoolToObject(partial, STATE_KEY_TAMPER, state->tamper);
    cJSON_AddBoolToObject(partial, STATE_KEY_LOW_BATTERY, state->battery_low);
    cJSON_AddBoolToObject(partial, STATE_KEY_TROUBLE, state->trouble);
    cJSON_AddBoolToObject(partial, STATE_KEY_AC_MAINS, state->ac_mains_fault);

    /* Map zone type to HA-compatible state */
    switch (state->zone_type) {
        case ZB_IAS_ZONE_TYPE_MOTION_SENSOR:
            cJSON_AddBoolToObject(partial, STATE_KEY_OCCUPANCY, state->alarm1);
            break;
        case ZB_IAS_ZONE_TYPE_CONTACT_SWITCH:
            cJSON_AddBoolToObject(partial, STATE_KEY_CONTACT, !state->alarm1);
            break;
        default:
            break;
    }

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_ias_zone(device_id_t id, zb_ias_zone_state_struct_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_ias_zone_state_struct_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_ZONE_STATUS)) && cJSON_IsNumber(item)) {
        state->zone_status = (uint16_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_ZONE_TYPE)) && cJSON_IsNumber(item)) {
        state->zone_type = (zb_ias_zone_type_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_ALARM1)) && cJSON_IsBool(item)) {
        state->alarm1 = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_ALARM2)) && cJSON_IsBool(item)) {
        state->alarm2 = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_TAMPER)) && cJSON_IsBool(item)) {
        state->tamper = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_LOW_BATTERY)) && cJSON_IsBool(item)) {
        state->battery_low = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_TROUBLE)) && cJSON_IsBool(item)) {
        state->trouble = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_AC_MAINS)) && cJSON_IsBool(item)) {
        state->ac_mains_fault = cJSON_IsTrue(item);
    }

    return ESP_OK;
}

/* ============================================================================
 * Illuminance State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_illuminance(device_id_t id, const zb_illuminance_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(partial, STATE_KEY_ILLUMINANCE, state->measured_value);

    /* Convert to lux: lux = 10^((measured_value - 1) / 10000) */
    float lux = 0;
    if (state->measured_value != ZB_ZCL_ILLUMINANCE_MEASURED_VALUE_INVALID &&
        state->measured_value != ZB_ZCL_ILLUMINANCE_MEASURED_VALUE_TOO_LOW &&
        state->measured_value > 0) {
        lux = powf(10.0f, ((float)state->measured_value - 1.0f) / 10000.0f);
    }
    cJSON_AddNumberToObject(partial, STATE_KEY_ILLUMINANCE_LUX, lux);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_illuminance(device_id_t id, zb_illuminance_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_illuminance_state_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_ILLUMINANCE)) && cJSON_IsNumber(item)) {
        state->measured_value = (uint16_t)item->valueint;
    }

    return ESP_OK;
}

/* ============================================================================
 * Pressure State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_pressure(device_id_t id, const zb_pressure_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    /* Pressure in hPa (measured_value is in 10 Pa = 0.01 kPa = 0.1 hPa) */
    float pressure_hpa = state->measured_value / 10.0f;
    cJSON_AddNumberToObject(partial, STATE_KEY_PRESSURE, pressure_hpa);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_pressure(device_id_t id, zb_pressure_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_pressure_state_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_PRESSURE)) && cJSON_IsNumber(item)) {
        state->measured_value = (int16_t)(item->valuedouble * 10);
    }

    return ESP_OK;
}

/* ============================================================================
 * PM2.5 State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_pm25(device_id_t id, const zb_pm25_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(partial, STATE_KEY_PM25, state->measured_value);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_pm25(device_id_t id, zb_pm25_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_pm25_state_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_PM25)) && cJSON_IsNumber(item)) {
        state->measured_value = (float)item->valuedouble;
        state->is_valid = true;
    }

    return ESP_OK;
}

/* ============================================================================
 * Binary Output State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_binary_output(device_id_t id, const zb_binary_output_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(partial, STATE_KEY_STATE, state->present_value ? "ON" : "OFF");
    cJSON_AddBoolToObject(partial, STATE_KEY_PRESENT_VALUE, state->present_value);
    cJSON_AddBoolToObject(partial, STATE_KEY_OUT_OF_SERVICE, state->out_of_service);
    cJSON_AddNumberToObject(partial, STATE_KEY_STATUS_FLAGS, state->status_flags);
    cJSON_AddBoolToObject(partial, STATE_KEY_IN_ALARM, state->in_alarm);
    cJSON_AddBoolToObject(partial, STATE_KEY_FAULT, state->fault);
    cJSON_AddBoolToObject(partial, STATE_KEY_OVERRIDDEN, state->overridden);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_binary_output(device_id_t id, zb_binary_output_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_binary_output_state_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_PRESENT_VALUE)) && cJSON_IsBool(item)) {
        state->present_value = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_OUT_OF_SERVICE)) && cJSON_IsBool(item)) {
        state->out_of_service = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_STATUS_FLAGS)) && cJSON_IsNumber(item)) {
        state->status_flags = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_IN_ALARM)) && cJSON_IsBool(item)) {
        state->in_alarm = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_FAULT)) && cJSON_IsBool(item)) {
        state->fault = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_OVERRIDDEN)) && cJSON_IsBool(item)) {
        state->overridden = cJSON_IsTrue(item);
    }

    return ESP_OK;
}

/* ============================================================================
 * Binary Value State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_binary_value(device_id_t id, const zb_binary_value_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(partial, STATE_KEY_STATE, state->present_value ? "ON" : "OFF");
    cJSON_AddBoolToObject(partial, STATE_KEY_PRESENT_VALUE, state->present_value);
    cJSON_AddBoolToObject(partial, STATE_KEY_OUT_OF_SERVICE, state->out_of_service);
    cJSON_AddNumberToObject(partial, STATE_KEY_STATUS_FLAGS, state->status_flags);
    cJSON_AddBoolToObject(partial, STATE_KEY_IN_ALARM, state->in_alarm);
    cJSON_AddBoolToObject(partial, STATE_KEY_FAULT, state->fault);
    cJSON_AddBoolToObject(partial, STATE_KEY_OVERRIDDEN, state->overridden);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_binary_value(device_id_t id, zb_binary_value_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_binary_value_state_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_PRESENT_VALUE)) && cJSON_IsBool(item)) {
        state->present_value = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_OUT_OF_SERVICE)) && cJSON_IsBool(item)) {
        state->out_of_service = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_STATUS_FLAGS)) && cJSON_IsNumber(item)) {
        state->status_flags = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_IN_ALARM)) && cJSON_IsBool(item)) {
        state->in_alarm = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_FAULT)) && cJSON_IsBool(item)) {
        state->fault = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_OVERRIDDEN)) && cJSON_IsBool(item)) {
        state->overridden = cJSON_IsTrue(item);
    }

    return ESP_OK;
}

/* ============================================================================
 * Multistate State Functions
 * ============================================================================ */

esp_err_t cluster_state_set_multistate(device_id_t id, const zb_multistate_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *partial = cJSON_CreateObject();
    if (!partial) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(partial, STATE_KEY_PRESENT_VALUE, state->present_value);
    cJSON_AddNumberToObject(partial, STATE_KEY_NUMBER_OF_STATES, state->number_of_states);
    cJSON_AddBoolToObject(partial, STATE_KEY_OUT_OF_SERVICE, state->out_of_service);
    cJSON_AddNumberToObject(partial, STATE_KEY_STATUS_FLAGS, state->status_flags);

    /* Also store as "action" for button compatibility */
    char action_buf[16];
    snprintf(action_buf, sizeof(action_buf), "%u", state->present_value);
    cJSON_AddStringToObject(partial, "action", action_buf);

    esp_err_t ret = device_registry_merge_state(id, partial);
    cJSON_Delete(partial);

    if (ret == ESP_OK) {
        cluster_state_publish_change(id);
    }

    return ret;
}

esp_err_t cluster_state_get_multistate(device_id_t id, zb_multistate_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = device_registry_get_state(id);
    if (!json) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(state, 0, sizeof(zb_multistate_state_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_PRESENT_VALUE)) && cJSON_IsNumber(item)) {
        state->present_value = (uint16_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_NUMBER_OF_STATES)) && cJSON_IsNumber(item)) {
        state->number_of_states = (uint16_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_OUT_OF_SERVICE)) && cJSON_IsBool(item)) {
        state->out_of_service = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, STATE_KEY_STATUS_FLAGS)) && cJSON_IsNumber(item)) {
        state->status_flags = (uint8_t)item->valueint;
    }

    return ESP_OK;
}
