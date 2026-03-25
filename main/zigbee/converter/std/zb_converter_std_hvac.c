/**
 * @file zb_converter_std_hvac.c
 * @brief Standard HVAC Converter Functions (Thermostat Weekly Schedule, Setpoints)
 *
 * Provides fromZigbee and toZigbee converters for HVAC-specific attributes
 * beyond the basic thermostat converters in zb_converter_std.c.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/converter/zb_converter_std.h"
#include "zigbee/converter/zb_converter.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "core/gateway_timeouts.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "CONV_HVAC";

/* ============================================================================
 * fromZigbee (fz_*) Converters
 * ============================================================================ */

/**
 * @brief Parse thermostat setpoint attributes (heating and cooling)
 *
 * Cluster 0x0201, attributes:
 *   0x0012 = OccupiedHeatingSetpoint (int16, /100 -> degrees C)
 *   0x0013 = OccupiedCoolingSetpoint (int16, /100 -> degrees C)
 *
 * Uses dispatch context to determine which attribute was reported.
 * Outputs the value under the provided JSON key.
 */
esp_err_t fz_thermostat_setpoint(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    const zb_dispatch_ctx_t *ctx = zb_converter_get_dispatch_ctx();
    if (ctx == NULL) {
        ESP_LOGW(TAG, "fz_thermostat_setpoint: no dispatch context");
        return ESP_ERR_INVALID_STATE;
    }

    /* Verify this is the thermostat cluster */
    if (ctx->cluster_id != ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT) {
        ESP_LOGW(TAG, "fz_thermostat_setpoint: unexpected cluster 0x%04X", ctx->cluster_id);
        return ESP_ERR_INVALID_ARG;
    }

    /* Only handle heating (0x0012) and cooling (0x0013) setpoints */
    if (ctx->attr_id != 0x0012 && ctx->attr_id != 0x0013) {
        ESP_LOGD(TAG, "fz_thermostat_setpoint: ignoring attr 0x%04X", ctx->attr_id);
        return ESP_ERR_NOT_FOUND;
    }

    int16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double temp_c = (double)raw_val / 100.0;

    /* Use provided key, which typically maps to:
     * 0x0012 -> "occupied_heating_setpoint"
     * 0x0013 -> "occupied_cooling_setpoint" */
    cJSON_AddNumberToObject(json, key, temp_c);

    ESP_LOGD(TAG, "Thermostat setpoint attr 0x%04X: %.2f C (key=%s)",
             ctx->attr_id, temp_c, key);
    return ESP_OK;
}

/**
 * @brief Parse thermostat weekly schedule attribute
 *
 * Cluster 0x0201, attribute 0x0024 (WeeklySchedule).
 * The raw data is a ZCL weekly schedule payload:
 *   - Byte 0: number of transitions
 *   - Byte 1: day of week bitmap (bit0=Sun, bit1=Mon, ..., bit6=Sat)
 *   - Byte 2: mode (bit0=heat, bit1=cool)
 *   - For each transition (4 bytes):
 *     - uint16 transition_time (minutes since midnight)
 *     - int16 heat/cool setpoint (/100 -> degrees C)
 *     (if mode has both heat+cool, 6 bytes per transition:
 *       uint16 time + int16 heat_setpoint + int16 cool_setpoint)
 *
 * Output JSON:
 * {
 *   "<key>": {
 *     "day_of_week": 0x1F,
 *     "mode": "heat",
 *     "transitions": [
 *       { "time": 360, "heat_setpoint": 21.0 },
 *       ...
 *     ]
 *   }
 * }
 */
esp_err_t fz_thermostat_weekly_schedule(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 3) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *data = (const uint8_t *)raw;
    uint8_t num_transitions = data[0];
    uint8_t day_of_week = data[1];
    uint8_t mode = data[2];

    bool has_heat = (mode & 0x01) != 0;
    bool has_cool = (mode & 0x02) != 0;

    /* Calculate expected bytes per transition */
    size_t bytes_per_transition = 2; /* transition_time (uint16) */
    if (has_heat) bytes_per_transition += 2; /* heat setpoint (int16) */
    if (has_cool) bytes_per_transition += 2; /* cool setpoint (int16) */

    size_t expected_len = 3 + (size_t)num_transitions * bytes_per_transition;
    if (len < expected_len) {
        ESP_LOGW(TAG, "Weekly schedule too short: got %zu, expected %zu",
                 len, expected_len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Build JSON schedule object */
    cJSON *schedule = cJSON_CreateObject();
    if (schedule == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(schedule, "day_of_week", day_of_week);

    const char *mode_str = "off";
    if (has_heat && has_cool) mode_str = "heat_cool";
    else if (has_heat) mode_str = "heat";
    else if (has_cool) mode_str = "cool";
    cJSON_AddStringToObject(schedule, "mode", mode_str);

    cJSON *transitions = cJSON_CreateArray();
    if (transitions == NULL) {
        cJSON_Delete(schedule);
        return ESP_ERR_NO_MEM;
    }

    const uint8_t *ptr = data + 3;
    for (uint8_t i = 0; i < num_transitions; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            break;
        }

        /* Transition time in minutes since midnight */
        uint16_t trans_time;
        memcpy(&trans_time, ptr, sizeof(trans_time));
        ptr += 2;
        cJSON_AddNumberToObject(entry, "time", trans_time);

        if (has_heat) {
            int16_t heat_sp;
            memcpy(&heat_sp, ptr, sizeof(heat_sp));
            ptr += 2;
            cJSON_AddNumberToObject(entry, "heat_setpoint", (double)heat_sp / 100.0);
        }

        if (has_cool) {
            int16_t cool_sp;
            memcpy(&cool_sp, ptr, sizeof(cool_sp));
            ptr += 2;
            cJSON_AddNumberToObject(entry, "cool_setpoint", (double)cool_sp / 100.0);
        }

        cJSON_AddItemToArray(transitions, entry);
    }

    cJSON_AddItemToObject(schedule, "transitions", transitions);
    cJSON_AddItemToObject(json, key, schedule);

    ESP_LOGD(TAG, "Weekly schedule: day=0x%02X mode=%s transitions=%d",
             day_of_week, mode_str, num_transitions);
    return ESP_OK;
}

/* ============================================================================
 * toZigbee (tz_*) Converters
 * ============================================================================ */

/**
 * @brief Send thermostat weekly schedule via Set Weekly Schedule command (0x01)
 *
 * Cluster 0x0201 command 0x01 (SetWeeklySchedule).
 *
 * Expected JSON input:
 * {
 *   "day_of_week": 0x1F,
 *   "mode": "heat",
 *   "transitions": [
 *     { "time": 360, "heat_setpoint": 21.0 },
 *     { "time": 480, "heat_setpoint": 22.5 },
 *     ...
 *   ]
 * }
 *
 * Payload format:
 *   - Byte 0: number of transitions
 *   - Byte 1: day of week bitmap
 *   - Byte 2: mode (0x01=heat, 0x02=cool, 0x03=heat+cool)
 *   - Per transition:
 *     - uint16 transition_time (minutes since midnight)
 *     - int16 heat_setpoint (if heat mode, *100)
 *     - int16 cool_setpoint (if cool mode, *100)
 */
esp_err_t tz_thermostat_weekly_schedule(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsObject(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse day_of_week */
    const cJSON *day_json = cJSON_GetObjectItem(value, "day_of_week");
    if (day_json == NULL || !cJSON_IsNumber(day_json)) {
        ESP_LOGW(TAG, "tz_weekly_schedule: missing day_of_week");
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t day_of_week = (uint8_t)cJSON_GetNumberValue(day_json);

    /* Parse mode */
    const cJSON *mode_json = cJSON_GetObjectItem(value, "mode");
    if (mode_json == NULL || !cJSON_IsString(mode_json)) {
        ESP_LOGW(TAG, "tz_weekly_schedule: missing mode");
        return ESP_ERR_INVALID_ARG;
    }
    const char *mode_str = cJSON_GetStringValue(mode_json);
    uint8_t mode = 0;
    bool has_heat = false;
    bool has_cool = false;

    if (strcmp(mode_str, "heat") == 0) {
        mode = 0x01;
        has_heat = true;
    } else if (strcmp(mode_str, "cool") == 0) {
        mode = 0x02;
        has_cool = true;
    } else if (strcmp(mode_str, "heat_cool") == 0) {
        mode = 0x03;
        has_heat = true;
        has_cool = true;
    } else {
        ESP_LOGW(TAG, "tz_weekly_schedule: unknown mode '%s'", mode_str);
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse transitions array */
    const cJSON *transitions = cJSON_GetObjectItem(value, "transitions");
    if (transitions == NULL || !cJSON_IsArray(transitions)) {
        ESP_LOGW(TAG, "tz_weekly_schedule: missing transitions array");
        return ESP_ERR_INVALID_ARG;
    }

    int num_transitions = cJSON_GetArraySize(transitions);
    if (num_transitions <= 0 || num_transitions > 10) {
        ESP_LOGW(TAG, "tz_weekly_schedule: invalid transition count %d", num_transitions);
        return ESP_ERR_INVALID_ARG;
    }

    /* Calculate payload size */
    size_t bytes_per_transition = 2; /* transition_time */
    if (has_heat) bytes_per_transition += 2;
    if (has_cool) bytes_per_transition += 2;

    size_t payload_len = 3 + (size_t)num_transitions * bytes_per_transition;
    /* Stack-allocate: max 10 transitions * 6 bytes + 3 header = 63 bytes */
    uint8_t payload[3 + 10 * 6];
    if (payload_len > sizeof(payload)) {
        return ESP_ERR_NO_MEM;
    }

    /* Build payload header */
    payload[0] = (uint8_t)num_transitions;
    payload[1] = day_of_week;
    payload[2] = mode;

    /* Build transitions */
    uint8_t *ptr = payload + 3;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, transitions) {
        /* Transition time */
        const cJSON *time_json = cJSON_GetObjectItem(entry, "time");
        if (time_json == NULL || !cJSON_IsNumber(time_json)) {
            ESP_LOGW(TAG, "tz_weekly_schedule: transition missing time");
            return ESP_ERR_INVALID_ARG;
        }
        uint16_t trans_time = (uint16_t)cJSON_GetNumberValue(time_json);
        memcpy(ptr, &trans_time, sizeof(trans_time));
        ptr += 2;

        /* Heat setpoint */
        if (has_heat) {
            const cJSON *heat_json = cJSON_GetObjectItem(entry, "heat_setpoint");
            if (heat_json == NULL || !cJSON_IsNumber(heat_json)) {
                ESP_LOGW(TAG, "tz_weekly_schedule: transition missing heat_setpoint");
                return ESP_ERR_INVALID_ARG;
            }
            int16_t heat_sp = (int16_t)(cJSON_GetNumberValue(heat_json) * 100.0);
            memcpy(ptr, &heat_sp, sizeof(heat_sp));
            ptr += 2;
        }

        /* Cool setpoint */
        if (has_cool) {
            const cJSON *cool_json = cJSON_GetObjectItem(entry, "cool_setpoint");
            if (cool_json == NULL || !cJSON_IsNumber(cool_json)) {
                ESP_LOGW(TAG, "tz_weekly_schedule: transition missing cool_setpoint");
                return ESP_ERR_INVALID_ARG;
            }
            int16_t cool_sp = (int16_t)(cJSON_GetNumberValue(cool_json) * 100.0);
            memcpy(ptr, &cool_sp, sizeof(cool_sp));
            ptr += 2;
        }
    }

    /* Send SetWeeklySchedule command (command ID 0x01) to thermostat cluster */
    esp_zb_zcl_custom_cluster_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .custom_cmd_id = 0x01, /* SetWeeklySchedule */
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
            .size = (uint16_t)payload_len,
            .value = payload,
        },
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "SetWeeklySchedule queued to 0x%04X EP%d: day=0x%02X mode=%s "
             "transitions=%d (TSN=%d)",
             short_addr, endpoint, day_of_week, mode_str, num_transitions, tsn);
    return ESP_OK;
}
