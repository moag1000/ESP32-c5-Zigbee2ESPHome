/**
 * @file zb_converter_std.c
 * @brief Standard Reusable Converter Function Implementations
 *
 * Provides standard fromZigbee (fz_*) and toZigbee (tz_*) converter
 * functions shared across multiple device definitions.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_converter_std.h"
#include "zigbee/zb_zcl_helpers.h"
#include "zigbee/zb_device_handler_types.h"
#include "zigbee/zb_cluster_closures.h"
#include "zigbee/zb_cluster_security.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "core/gateway_timeouts.h"
#include <string.h>
#include <math.h>
#include <inttypes.h>

static const char *TAG = "CONV_STD";

/* ============================================================================
 * Device Registry Helper Functions
 *
 * These helpers allow toZigbee converters to access device capabilities
 * and state from the device_registry when needed.
 * ============================================================================ */

/**
 * @brief Get device from device_registry by short address
 *
 * Helper function for toZigbee converters that need to access
 * device capabilities or state.
 *
 * @param short_addr Device short address
 * @return Pointer to device_t, or NULL if not found
 */
static inline device_t *conv_get_device(uint16_t short_addr)
{
    return device_registry_get_by_short_addr(short_addr);
}

/**
 * @brief Check if device has a specific capability
 *
 * @param short_addr Device short address
 * @param cap Capability to check
 * @return true if device has capability, false otherwise
 */
static inline bool conv_device_has_capability(uint16_t short_addr, device_capability_t cap)
{
    device_t *dev = conv_get_device(short_addr);
    if (dev == NULL) {
        return false;
    }
    return (dev->capabilities & cap) != 0;
}

/**
 * @brief Log device info for debugging
 *
 * @param short_addr Device short address
 * @param operation Description of the operation being performed
 */
static inline void conv_log_device_info(uint16_t short_addr, const char *operation)
{
    device_t *dev = conv_get_device(short_addr);
    if (dev != NULL) {
        ESP_LOGD(TAG, "%s: device 0x%04X caps=0x%08" PRIx32 " avail=%s",
                 operation, short_addr, dev->capabilities,
                 device_availability_to_str(dev->availability));
    }
}

/* ============================================================================
 * fromZigbee (fz_*) Converters
 * ============================================================================ */

esp_err_t fz_on_off(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    bool value = *(const uint8_t *)raw != 0;
    cJSON_AddStringToObject(json, key, value ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t fz_brightness(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t level = *(const uint8_t *)raw;
    cJSON_AddNumberToObject(json, key, level);
    return ESP_OK;
}

esp_err_t fz_color_temp(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t mireds;
    memcpy(&mireds, raw, sizeof(mireds));
    cJSON_AddNumberToObject(json, key, mireds);
    return ESP_OK;
}

esp_err_t fz_color_xy(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Color X and Y are each uint16, normalized to 0.0-1.0 by dividing by 65535 */
    uint16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double normalized = (double)raw_val / 65535.0;

    /* Check if color object already exists */
    cJSON *color = cJSON_GetObjectItem(json, "color");
    if (color == NULL) {
        color = cJSON_CreateObject();
        if (color == NULL) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(json, "color", color);
    }

    /* Determine if this is x or y based on the key */
    if (strcmp(key, "color_x") == 0) {
        cJSON_AddNumberToObject(color, "x", normalized);
    } else if (strcmp(key, "color_y") == 0) {
        cJSON_AddNumberToObject(color, "y", normalized);
    }

    return ESP_OK;
}

esp_err_t fz_temperature(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    int16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double temp_c = (double)raw_val / 100.0;
    cJSON_AddNumberToObject(json, key, temp_c);
    return ESP_OK;
}

esp_err_t fz_humidity(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double humidity = (double)raw_val / 100.0;
    cJSON_AddNumberToObject(json, key, humidity);
    return ESP_OK;
}

esp_err_t fz_pressure(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    int16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double pressure_hpa = (double)raw_val / 10.0;
    cJSON_AddNumberToObject(json, key, pressure_hpa);
    return ESP_OK;
}

esp_err_t fz_illuminance(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double lux = 0.0;
    if (raw_val > 0) {
        lux = pow(10.0, ((double)(raw_val - 1)) / 10000.0);
    }
    cJSON_AddNumberToObject(json, key, lux);
    return ESP_OK;
}

esp_err_t fz_occupancy(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw_val = *(const uint8_t *)raw;
    bool occupied = (raw_val & 0x01) != 0;
    cJSON_AddBoolToObject(json, key, occupied);
    return ESP_OK;
}

esp_err_t fz_battery(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw_val = *(const uint8_t *)raw;
    /* ZCL battery percentage is in units of 0.5% */
    double percent = (double)raw_val / 2.0;
    if (percent > 100.0) {
        percent = 100.0;
    }
    cJSON_AddNumberToObject(json, key, percent);
    return ESP_OK;
}

esp_err_t fz_ias_zone_status(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t zone_status;
    memcpy(&zone_status, raw, sizeof(zone_status));
    bool alarm1 = (zone_status & 0x0001) != 0;
    cJSON_AddBoolToObject(json, key, alarm1);

    /* Also add tamper and battery_low */
    bool tamper = (zone_status & 0x0004) != 0;
    bool battery_low = (zone_status & 0x0008) != 0;
    cJSON_AddBoolToObject(json, "tamper", tamper);
    cJSON_AddBoolToObject(json, "battery_low", battery_low);

    return ESP_OK;
}

esp_err_t fz_vibration_action(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t status_type;
    memcpy(&status_type, raw, sizeof(status_type));

    const char *action;
    switch (status_type) {
        case 1:  action = "vibration"; break;
        case 2:  action = "tilt";      break;
        case 3:  action = "drop";      break;
        default: action = "unknown";   break;
    }
    cJSON_AddStringToObject(json, key, action);
    /* Use string "true"/"false" for HA compatibility (Jinja template expects string) */
    bool detected = (status_type >= 1 && status_type <= 3);
    cJSON_AddStringToObject(json, "vibration", detected ? "true" : "false");
    return ESP_OK;
}

esp_err_t fz_vibration_strength(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 4) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    /* Discard LSB then byte-swap the remaining two bytes */
    uint16_t shifted = (uint16_t)(raw_val >> 8);
    uint16_t strength = ((shifted & 0xFF) << 8) | ((shifted >> 8) & 0xFF);
    cJSON_AddNumberToObject(json, key, strength);
    return ESP_OK;
}

esp_err_t fz_vibration_angle(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 6) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Parse uint48 little-endian into a 64-bit value */
    const uint8_t *data = (const uint8_t *)raw;
    uint64_t value = 0;
    for (int i = 5; i >= 0; i--) {
        value = (value << 8) | data[i];
    }
    double x = (double)(int16_t)(value & 0xFFFF);
    double y = (double)(int16_t)((value >> 16) & 0xFFFF);
    double z = (double)(int16_t)((value >> 32) & 0xFFFF);

    double denom_x = sqrt(z * z + y * y);
    double denom_y = sqrt(x * x + z * z);
    double denom_z = sqrt(x * x + y * y);

    int angle_x = (denom_x > 0.0) ? (int)round(atan(x / denom_x) * 180.0 / M_PI) : 0;
    int angle_y = (denom_y > 0.0) ? (int)round(atan(y / denom_y) * 180.0 / M_PI) : 0;
    int angle_z = (denom_z > 0.0) ? (int)round(atan(z / denom_z) * 180.0 / M_PI) : 0;

    cJSON_AddNumberToObject(json, "angle_x", angle_x);
    cJSON_AddNumberToObject(json, "angle_y", angle_y);
    cJSON_AddNumberToObject(json, "angle_z", angle_z);

    /* Raw accelerometer values as int16 */
    cJSON_AddNumberToObject(json, "x_axis", (int16_t)(value & 0xFFFF));
    cJSON_AddNumberToObject(json, "y_axis", (int16_t)((value >> 16) & 0xFFFF));
    cJSON_AddNumberToObject(json, "z_axis", (int16_t)((value >> 32) & 0xFFFF));

    return ESP_OK;
}

/**
 * @brief Get data size for a ZCL attribute data type
 * @return Size in bytes, or 0 for variable/unknown types
 */
static size_t zcl_type_size(uint8_t type)
{
    switch (type) {
        case 0x10: return 1;  /* bool */
        case 0x20: return 1;  /* uint8 */
        case 0x21: return 2;  /* uint16 */
        case 0x22: return 3;  /* uint24 */
        case 0x23: return 4;  /* uint32 */
        case 0x24: return 5;  /* uint40 */
        case 0x25: return 6;  /* uint48 */
        case 0x28: return 1;  /* int8 */
        case 0x29: return 2;  /* int16 */
        case 0x2a: return 3;  /* int24 */
        case 0x2b: return 4;  /* int32 */
        case 0x39: return 4;  /* float */
        case 0x42: return 0;  /* string — length-prefixed, variable */
        default:   return 0;
    }
}

esp_err_t fz_xiaomi_ff01(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || len < 4) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *data = (const uint8_t *)raw;
    size_t pos = 0;

    while (pos + 2 < len) {
        uint8_t tag = data[pos++];
        uint8_t type = data[pos++];

        size_t val_size;
        if (type == 0x42) {
            /* Length-prefixed string */
            if (pos >= len) break;
            val_size = data[pos++];
        } else {
            val_size = zcl_type_size(type);
            if (val_size == 0) {
                ESP_LOGD(TAG, "Xiaomi 0xFF01: unknown type 0x%02X at tag 0x%02X", type, tag);
                break;
            }
        }

        if (pos + val_size > len) break;

        switch (tag) {
            case 0x01: {
                /* Battery voltage in mV (uint16) */
                if (val_size >= 2) {
                    uint16_t voltage_mv;
                    memcpy(&voltage_mv, &data[pos], sizeof(voltage_mv));
                    double voltage = (double)voltage_mv / 1000.0;
                    cJSON_AddNumberToObject(json, "voltage", voltage);

                    /* Convert voltage to percentage (CR2032: 2.7V=0%, 3.2V=100%) */
                    double pct = (voltage - 2.7) / (3.2 - 2.7) * 100.0;
                    if (pct < 0.0) pct = 0.0;
                    if (pct > 100.0) pct = 100.0;
                    cJSON_AddNumberToObject(json, key, (int)pct);
                }
                break;
            }
            case 0x03: {
                /* Device temperature (int8) */
                if (val_size >= 1) {
                    int8_t temp = (int8_t)data[pos];
                    cJSON_AddNumberToObject(json, "device_temperature", temp);
                }
                break;
            }
            case 0x05: {
                /* Power outage count (uint8 or uint16) */
                if (val_size >= 2) {
                    uint16_t count;
                    memcpy(&count, &data[pos], sizeof(count));
                    cJSON_AddNumberToObject(json, "power_outage_count", count);
                } else if (val_size >= 1) {
                    cJSON_AddNumberToObject(json, "power_outage_count", data[pos]);
                }
                break;
            }
            default:
                break;
        }

        pos += val_size;
    }

    return ESP_OK;
}

esp_err_t fz_uint16(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t value;
    memcpy(&value, raw, sizeof(value));
    cJSON_AddNumberToObject(json, key, value);
    return ESP_OK;
}

esp_err_t fz_electrical_power(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    int16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double power_w = (double)raw_val / 10.0;
    cJSON_AddNumberToObject(json, key, power_w);
    return ESP_OK;
}

esp_err_t fz_metering(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 6) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Parse uint48 (6 bytes, little-endian) */
    const uint8_t *data = (const uint8_t *)raw;
    uint64_t value = 0;
    for (int i = 5; i >= 0; i--) {
        value = (value << 8) | data[i];
    }
    /* Convert to kWh (divisor typically 1000) */
    double kwh = (double)value / 1000.0;
    cJSON_AddNumberToObject(json, key, kwh);
    return ESP_OK;
}

esp_err_t fz_thermostat_local_temp(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    int16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double temp_c = (double)raw_val / 100.0;
    cJSON_AddNumberToObject(json, key, temp_c);
    return ESP_OK;
}

esp_err_t fz_cover_position(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t position = *(const uint8_t *)raw;
    if (position > 100) {
        position = 100;
    }
    cJSON_AddNumberToObject(json, key, position);
    return ESP_OK;
}

esp_err_t fz_lock_state(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t state = *(const uint8_t *)raw;
    /* ZCL lock states: 0=not fully locked, 1=locked, 2=unlocked */
    const char *state_str = (state == 1) ? "LOCK" : "UNLOCK";
    cJSON_AddStringToObject(json, key, state_str);
    return ESP_OK;
}

esp_err_t fz_color_hs(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw_val = *(const uint8_t *)raw;
    /* Create or get color object */
    cJSON *color = cJSON_GetObjectItem(json, "color");
    if (color == NULL) {
        color = cJSON_CreateObject();
        if (color == NULL) return ESP_ERR_NO_MEM;
        cJSON_AddItemToObject(json, "color", color);
    }
    if (strcmp(key, "color_hue") == 0) {
        cJSON_AddNumberToObject(color, "hue", (double)raw_val * 360.0 / 254.0);
    } else if (strcmp(key, "color_saturation") == 0) {
        cJSON_AddNumberToObject(color, "saturation", (double)raw_val * 100.0 / 254.0);
    }
    return ESP_OK;
}

esp_err_t fz_thermostat_system_mode(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mode = *(const uint8_t *)raw;
    const char *mode_str;
    switch (mode) {
        case 0: mode_str = "off"; break;
        case 1: mode_str = "auto"; break;
        case 3: mode_str = "cool"; break;
        case 4: mode_str = "heat"; break;
        case 5: mode_str = "emergency_heating"; break;
        case 6: mode_str = "precooling"; break;
        case 7: mode_str = "fan_only"; break;
        case 8: mode_str = "dry"; break;
        case 9: mode_str = "sleep"; break;
        default: mode_str = "off"; break;
    }
    cJSON_AddStringToObject(json, key, mode_str);
    return ESP_OK;
}

esp_err_t fz_thermostat_running_state(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t state;
    memcpy(&state, raw, sizeof(state));
    const char *state_str = "idle";
    if (state & 0x0001) state_str = "heat";
    else if (state & 0x0002) state_str = "cool";
    else if (state & 0x0004) state_str = "fan_only";
    cJSON_AddStringToObject(json, key, state_str);
    return ESP_OK;
}

esp_err_t fz_fan_mode(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mode = *(const uint8_t *)raw;
    const char *mode_str;
    switch (mode) {
        case 0: mode_str = "off"; break;
        case 1: mode_str = "low"; break;
        case 2: mode_str = "medium"; break;
        case 3: mode_str = "high"; break;
        case 4: mode_str = "on"; break;
        case 5: mode_str = "auto"; break;
        case 6: mode_str = "smart"; break;
        default: mode_str = "off"; break;
    }
    cJSON_AddStringToObject(json, key, mode_str);
    return ESP_OK;
}

esp_err_t fz_current(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double amps = (double)raw_val / 1000.0;
    cJSON_AddNumberToObject(json, key, amps);
    return ESP_OK;
}

esp_err_t fz_voltage(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double volts = (double)raw_val / 10.0;
    cJSON_AddNumberToObject(json, key, volts);
    return ESP_OK;
}

esp_err_t fz_co2(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 4) {
        return ESP_ERR_INVALID_ARG;
    }
    /* CO2 concentration is float (IEEE 754) in ZCL */
    float co2_val;
    memcpy(&co2_val, raw, sizeof(co2_val));
    cJSON_AddNumberToObject(json, key, (double)co2_val);
    return ESP_OK;
}

esp_err_t fz_cover_tilt(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tilt = *(const uint8_t *)raw;
    if (tilt > 100) tilt = 100;
    cJSON_AddNumberToObject(json, key, tilt);
    return ESP_OK;
}

esp_err_t fz_voc(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 4) {
        return ESP_ERR_INVALID_ARG;
    }
    float voc_val;
    memcpy(&voc_val, raw, sizeof(voc_val));
    cJSON_AddNumberToObject(json, key, (double)voc_val);
    return ESP_OK;
}

esp_err_t fz_power_factor(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    int8_t pf = *(const int8_t *)raw;
    cJSON_AddNumberToObject(json, key, pf);
    return ESP_OK;
}

esp_err_t fz_smoke_alarm(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t zone_status;
    memcpy(&zone_status, raw, sizeof(zone_status));
    bool smoke = (zone_status & 0x0001) != 0;
    cJSON_AddBoolToObject(json, key, smoke);
    return ESP_OK;
}

esp_err_t fz_water_leak(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t zone_status;
    memcpy(&zone_status, raw, sizeof(zone_status));
    bool leak = (zone_status & 0x0001) != 0;
    cJSON_AddBoolToObject(json, key, leak);
    return ESP_OK;
}

esp_err_t fz_power_on_behavior(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t behavior = *(const uint8_t *)raw;
    const char *str;
    switch (behavior) {
        case 0x00: str = "off"; break;
        case 0x01: str = "on"; break;
        case 0x02: str = "toggle"; break;
        case 0xFF: str = "previous"; break;
        default: str = "off"; break;
    }
    cJSON_AddStringToObject(json, key, str);
    return ESP_OK;
}

esp_err_t fz_xiaomi_voltage(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t voltage_mv;
    memcpy(&voltage_mv, raw, sizeof(voltage_mv));
    double voltage = (double)voltage_mv / 1000.0;
    cJSON_AddNumberToObject(json, key, voltage);
    return ESP_OK;
}

/* ============================================================================
 * toZigbee (tz_*) Converters
 * ============================================================================ */

esp_err_t tz_on_off(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_on_off");

    /* Verify device has ON_OFF capability */
    if (!conv_device_has_capability(short_addr, DEV_CAP_ON_OFF)) {
        ESP_LOGW(TAG, "Device 0x%04X lacks ON_OFF capability", short_addr);
        /* Continue anyway - capability may not be set yet */
    }

    bool on = false;
    bool toggle = false;

    if (cJSON_IsString(value)) {
        const char *str = cJSON_GetStringValue(value);
        if (str == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        if (strcasecmp(str, "ON") == 0) {
            on = true;
        } else if (strcasecmp(str, "OFF") == 0) {
            on = false;
        } else if (strcasecmp(str, "TOGGLE") == 0) {
            toggle = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (cJSON_IsBool(value)) {
        on = cJSON_IsTrue(value);
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (toggle) {
        /* No helper available for toggle, manual lock required */
        esp_zb_zcl_on_off_cmd_t cmd_req = {
            .zcl_basic_cmd = {
                .dst_addr_u = { .addr_short = short_addr },
                .dst_endpoint = endpoint,
                .src_endpoint = 1,
            },
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID,
        };
        esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
        uint8_t tsn = esp_zb_zcl_on_off_cmd_req(&cmd_req);
        esp_zb_lock_release();
        ESP_LOGD(TAG, "Toggle command queued to 0x%04X (TSN=%d)", short_addr, tsn);
        return ESP_OK;
    }

    return zb_zcl_send_on_off_cmd_with_lock(short_addr, endpoint, on);
}

esp_err_t tz_brightness(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsNumber(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_brightness");

    /* Verify device has BRIGHTNESS capability */
    if (!conv_device_has_capability(short_addr, DEV_CAP_BRIGHTNESS)) {
        ESP_LOGW(TAG, "Device 0x%04X lacks BRIGHTNESS capability", short_addr);
        /* Continue anyway - capability may not be set yet */
    }

    double raw_val = cJSON_GetNumberValue(value);
    if (raw_val < 0) raw_val = 0;
    if (raw_val > 254) raw_val = 254;
    uint8_t level = (uint8_t)raw_val;

    return zb_zcl_send_level_cmd_with_lock(short_addr, endpoint, level, 0);
}

esp_err_t tz_color_temp(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsNumber(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_color_temp");

    /* Verify device has COLOR_TEMP capability */
    if (!conv_device_has_capability(short_addr, DEV_CAP_COLOR_TEMP)) {
        ESP_LOGW(TAG, "Device 0x%04X lacks COLOR_TEMP capability", short_addr);
        /* Continue anyway - capability may not be set yet */
    }

    double val = cJSON_GetNumberValue(value);
    if (val < 0) val = 0;
    if (val > 65535) val = 65535;
    uint16_t mireds = (uint16_t)val;

    /* Use ZCL color temperature move to color temperature command */
    esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .color_temperature = mireds,
        .transition_time = 0,
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Color temp command queued to 0x%04X (TSN=%d)", short_addr, tsn);
    return ESP_OK;
}

esp_err_t tz_color_xy(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsObject(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_color_xy");

    /* Verify device has COLOR_XY capability */
    if (!conv_device_has_capability(short_addr, DEV_CAP_COLOR_XY)) {
        ESP_LOGW(TAG, "Device 0x%04X lacks COLOR_XY capability", short_addr);
        /* Continue anyway - capability may not be set yet */
    }

    cJSON *x_json = cJSON_GetObjectItem(value, "x");
    cJSON *y_json = cJSON_GetObjectItem(value, "y");
    if (x_json == NULL || y_json == NULL ||
        !cJSON_IsNumber(x_json) || !cJSON_IsNumber(y_json)) {
        return ESP_ERR_INVALID_ARG;
    }

    double x = cJSON_GetNumberValue(x_json);
    double y = cJSON_GetNumberValue(y_json);

    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    if (y < 0.0) y = 0.0;
    if (y > 1.0) y = 1.0;

    /* Convert from 0.0-1.0 to 0-65535 */
    uint16_t color_x = (uint16_t)(x * 65535.0);
    uint16_t color_y = (uint16_t)(y * 65535.0);

    return zb_zcl_send_color_cmd_with_lock(short_addr, endpoint, color_x, color_y, 0);
}

esp_err_t tz_thermostat_setpoint(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsNumber(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_thermostat_setpoint");

    /* Verify device has CLIMATE capability */
    if (!conv_device_has_capability(short_addr, DEV_CAP_CLIMATE)) {
        ESP_LOGW(TAG, "Device 0x%04X lacks CLIMATE capability", short_addr);
        /* Continue anyway - capability may not be set yet */
    }

    double temp_c = cJSON_GetNumberValue(value);
    if (temp_c < -327.0) temp_c = -327.0;
    if (temp_c > 327.0) temp_c = 327.0;
    int16_t raw_value = (int16_t)(temp_c * 100.0);

    /* Write occupied_heating_setpoint attribute (0x0012) */
    esp_zb_zcl_write_attr_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
    };

    esp_zb_zcl_attribute_t attr = {
        .id = 0x0012, /* OccupiedHeatingSetpoint */
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_S16,
            .size = sizeof(int16_t),
            .value = (void *)&raw_value,
        },
    };
    cmd_req.attr_number = 1;
    cmd_req.attr_field = &attr;

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_write_attr_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Thermostat setpoint command queued to 0x%04X (TSN=%d)", short_addr, tsn);
    return ESP_OK;
}

esp_err_t tz_cover_position(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsNumber(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_cover_position");

    /* Verify device has COVER capability */
    if (!conv_device_has_capability(short_addr, DEV_CAP_COVER)) {
        ESP_LOGW(TAG, "Device 0x%04X lacks COVER capability", short_addr);
        /* Continue anyway - capability may not be set yet */
    }

    double raw_val = cJSON_GetNumberValue(value);
    if (raw_val < 0) raw_val = 0;
    if (raw_val > 100) raw_val = 100;
    uint8_t position = (uint8_t)raw_val;

    return zb_window_covering_cmd_goto_lift_percent(short_addr, endpoint, position);
}

esp_err_t tz_cover_command(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsString(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_cover_command");

    /* Verify device has COVER capability */
    if (!conv_device_has_capability(short_addr, DEV_CAP_COVER)) {
        ESP_LOGW(TAG, "Device 0x%04X lacks COVER capability", short_addr);
        /* Continue anyway - capability may not be set yet */
    }

    const char *cmd = cJSON_GetStringValue(value);
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcasecmp(cmd, "OPEN") == 0) {
        return zb_window_covering_cmd_up(short_addr, endpoint);
    } else if (strcasecmp(cmd, "CLOSE") == 0) {
        return zb_window_covering_cmd_down(short_addr, endpoint);
    } else if (strcasecmp(cmd, "STOP") == 0) {
        return zb_window_covering_cmd_stop(short_addr, endpoint);
    }

    ESP_LOGW(TAG, "Unknown cover command: %s", cmd);
    return ESP_ERR_INVALID_ARG;
}

esp_err_t tz_lock_command(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsString(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_lock_command");

    /* Verify device has LOCK capability */
    if (!conv_device_has_capability(short_addr, DEV_CAP_LOCK)) {
        ESP_LOGW(TAG, "Device 0x%04X lacks LOCK capability", short_addr);
        /* Continue anyway - capability may not be set yet */
    }

    const char *cmd = cJSON_GetStringValue(value);
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcasecmp(cmd, "LOCK") == 0) {
        return zb_door_lock_cmd_lock(short_addr, endpoint);
    } else if (strcasecmp(cmd, "UNLOCK") == 0) {
        return zb_door_lock_cmd_unlock(short_addr, endpoint);
    }

    ESP_LOGW(TAG, "Unknown lock command: %s", cmd);
    return ESP_ERR_INVALID_ARG;
}

esp_err_t tz_xiaomi_sensitivity(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    uint8_t sensitivity_value;

    if (cJSON_IsString(value)) {
        const char *str = cJSON_GetStringValue(value);
        if (str == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        if (strcasecmp(str, "high") == 0) {
            sensitivity_value = 1;
        } else if (strcasecmp(str, "medium") == 0) {
            sensitivity_value = 11;
        } else if (strcasecmp(str, "low") == 0) {
            sensitivity_value = 21;
        } else {
            ESP_LOGW(TAG, "Unknown sensitivity: %s (use high/medium/low)", str);
            return ESP_ERR_INVALID_ARG;
        }
    } else if (cJSON_IsNumber(value)) {
        int num = (int)cJSON_GetNumberValue(value);
        if (num < 1) num = 1;
        if (num > 21) num = 21;
        sensitivity_value = (uint8_t)num;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Setting Xiaomi sensitivity to %d for 0x%04X", sensitivity_value, short_addr);

    /* Write to Basic cluster (0x0000), manufacturer-specific attr 0xFF0D
     * Manufacturer code: 0x115F (Xiaomi/Lumi) */
    esp_zb_zcl_write_attr_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        .manuf_specific = 1,   /* Enable manufacturer-specific ZCL frame header */
        .manuf_code = 0x115F,  /* Xiaomi manufacturer code */
    };

    esp_zb_zcl_attribute_t attr = {
        .id = 0xFF0D,  /* Xiaomi sensitivity attribute */
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_U8,
            .size = sizeof(uint8_t),
            .value = (void *)&sensitivity_value,
        },
    };
    cmd_req.attr_number = 1;
    cmd_req.attr_field = &attr;

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_write_attr_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGI(TAG, "Sensitivity command queued (TSN=%d) — device must be awake to receive",
             tsn);

    return ESP_OK;
}

esp_err_t tz_color_hs(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsObject(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_color_hs");

    cJSON *hue_json = cJSON_GetObjectItem(value, "hue");
    cJSON *sat_json = cJSON_GetObjectItem(value, "saturation");
    if (hue_json == NULL || sat_json == NULL ||
        !cJSON_IsNumber(hue_json) || !cJSON_IsNumber(sat_json)) {
        return ESP_ERR_INVALID_ARG;
    }
    double hue = cJSON_GetNumberValue(hue_json);
    double sat = cJSON_GetNumberValue(sat_json);
    uint8_t zcl_hue = (uint8_t)(hue * 254.0 / 360.0);
    uint8_t zcl_sat = (uint8_t)(sat * 254.0 / 100.0);

    esp_zb_color_move_to_hue_saturation_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .hue = zcl_hue,
        .saturation = zcl_sat,
        .transition_time = 0,
    };
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_color_move_to_hue_and_saturation_cmd_req(&cmd_req);
    esp_zb_lock_release();
    ESP_LOGD(TAG, "Color HS command queued to 0x%04X (TSN=%d)", short_addr, tsn);
    return ESP_OK;
}

esp_err_t tz_thermostat_system_mode(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsString(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_thermostat_system_mode");

    const char *mode = cJSON_GetStringValue(value);
    if (mode == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t zcl_mode;
    if (strcasecmp(mode, "off") == 0) zcl_mode = 0;
    else if (strcasecmp(mode, "auto") == 0) zcl_mode = 1;
    else if (strcasecmp(mode, "cool") == 0) zcl_mode = 3;
    else if (strcasecmp(mode, "heat") == 0) zcl_mode = 4;
    else if (strcasecmp(mode, "fan_only") == 0) zcl_mode = 7;
    else if (strcasecmp(mode, "dry") == 0) zcl_mode = 8;
    else return ESP_ERR_INVALID_ARG;

    esp_zb_zcl_write_attr_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
    };
    esp_zb_zcl_attribute_t attr = {
        .id = 0x001C,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
            .size = sizeof(uint8_t),
            .value = (void *)&zcl_mode,
        },
    };
    cmd_req.attr_number = 1;
    cmd_req.attr_field = &attr;

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_write_attr_cmd_req(&cmd_req);
    esp_zb_lock_release();
    ESP_LOGD(TAG, "Thermostat system mode command queued (TSN=%d)", tsn);
    return ESP_OK;
}

esp_err_t tz_fan_mode(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsString(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_fan_mode");

    const char *mode = cJSON_GetStringValue(value);
    if (mode == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t zcl_mode;
    if (strcasecmp(mode, "off") == 0) zcl_mode = 0;
    else if (strcasecmp(mode, "low") == 0) zcl_mode = 1;
    else if (strcasecmp(mode, "medium") == 0) zcl_mode = 2;
    else if (strcasecmp(mode, "high") == 0) zcl_mode = 3;
    else if (strcasecmp(mode, "on") == 0) zcl_mode = 4;
    else if (strcasecmp(mode, "auto") == 0) zcl_mode = 5;
    else return ESP_ERR_INVALID_ARG;

    esp_zb_zcl_write_attr_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL,
    };
    esp_zb_zcl_attribute_t attr = {
        .id = 0x0000,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
            .size = sizeof(uint8_t),
            .value = (void *)&zcl_mode,
        },
    };
    cmd_req.attr_number = 1;
    cmd_req.attr_field = &attr;

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_write_attr_cmd_req(&cmd_req);
    esp_zb_lock_release();
    ESP_LOGD(TAG, "Fan mode command queued (TSN=%d)", tsn);
    return ESP_OK;
}

esp_err_t tz_cover_tilt(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsNumber(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_cover_tilt");

    /* Verify device has COVER capability */
    if (!conv_device_has_capability(short_addr, DEV_CAP_COVER)) {
        ESP_LOGW(TAG, "Device 0x%04X lacks COVER capability", short_addr);
    }

    double raw_val = cJSON_GetNumberValue(value);
    if (raw_val < 0) raw_val = 0;
    if (raw_val > 100) raw_val = 100;
    uint8_t tilt = (uint8_t)raw_val;

    /* Use GoToTiltPercentage cluster command (0x08), NOT attribute write.
     * Attribute 0x0009 (CurrentPositionTiltPercentage) is read-only. */
    return zb_window_covering_cmd_goto_tilt_percent(short_addr, endpoint, tilt);
}

esp_err_t tz_power_on_behavior(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsString(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Log device info for debugging */
    conv_log_device_info(short_addr, "tz_power_on_behavior");

    const char *str = cJSON_GetStringValue(value);
    if (str == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t behavior;
    if (strcasecmp(str, "off") == 0) behavior = 0x00;
    else if (strcasecmp(str, "on") == 0) behavior = 0x01;
    else if (strcasecmp(str, "toggle") == 0) behavior = 0x02;
    else if (strcasecmp(str, "previous") == 0) behavior = 0xFF;
    else return ESP_ERR_INVALID_ARG;

    esp_zb_zcl_write_attr_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
    };
    esp_zb_zcl_attribute_t attr = {
        .id = 0x4003, /* StartUpOnOff */
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
            .size = sizeof(uint8_t),
            .value = (void *)&behavior,
        },
    };
    cmd_req.attr_number = 1;
    cmd_req.attr_field = &attr;

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_write_attr_cmd_req(&cmd_req);
    esp_zb_lock_release();
    ESP_LOGD(TAG, "Power-on behavior command queued (TSN=%d)", tsn);
    return ESP_OK;
}
