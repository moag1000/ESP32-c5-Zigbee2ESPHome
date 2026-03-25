/**
 * @file zb_converter_std_vendor.c
 * @brief Vendor-Specific Converter Functions (Aqara, IKEA, Philips, Identify)
 *
 * Provides vendor-specific fromZigbee (fz_*) and toZigbee (tz_*) converter
 * functions for proprietary clusters and the standard Identify cluster.
 *
 * Supported vendors/clusters:
 * - Aqara Opple (cluster 0xFCC0, manufacturer 0x115F)
 * - IKEA Air Purifier / VOC (cluster 0xFC7D)
 * - Philips Hue Motion (cluster 0xFC00)
 * - ZCL Identify (cluster 0x0003)
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

static const char *TAG = "CONV_VENDOR";

/* ============================================================================
 * Aqara Opple (cluster 0xFCC0, manufacturer code 0x115F)
 * ============================================================================ */

/**
 * @brief fromZigbee: Aqara Opple manufacturer-specific attribute report
 *
 * Parses raw data from cluster 0xFCC0 as uint8/uint16/uint32 based on
 * the data length. Outputs the parsed value to the specified JSON key.
 *
 * @param raw  Raw attribute data
 * @param len  Length of raw data (1, 2, or 4 bytes)
 * @param json Output JSON object
 * @param key  JSON key to store the value under
 * @return ESP_OK on success
 */
esp_err_t fz_aqara_opple(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *data = (const uint8_t *)raw;

    if (len >= 4) {
        uint32_t val;
        memcpy(&val, data, sizeof(val));
        cJSON_AddNumberToObject(json, key, (double)val);
        ESP_LOGD(TAG, "fz_aqara_opple: key=%s uint32=%" PRIu32, key, val);
    } else if (len >= 2) {
        uint16_t val;
        memcpy(&val, data, sizeof(val));
        cJSON_AddNumberToObject(json, key, val);
        ESP_LOGD(TAG, "fz_aqara_opple: key=%s uint16=%" PRIu16, key, val);
    } else {
        uint8_t val = data[0];
        cJSON_AddNumberToObject(json, key, val);
        ESP_LOGD(TAG, "fz_aqara_opple: key=%s uint8=%" PRIu8, key, val);
    }

    return ESP_OK;
}

/**
 * @brief toZigbee: Write Aqara Opple manufacturer-specific attribute
 *
 * Writes a manufacturer-specific attribute on cluster 0xFCC0 with
 * manufacturer code 0x115F. Accepts a number or string JSON value.
 * Uses dispatch context to determine the attribute ID via cross-referencing
 * the converter's from_zigbee entries.
 *
 * @param short_addr  Device short address
 * @param endpoint    Target endpoint
 * @param value       JSON value (number or string)
 * @return ESP_OK on success
 */
esp_err_t tz_aqara_opple(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Determine attribute ID from dispatch context */
    const zb_dispatch_ctx_t *ctx = zb_converter_get_dispatch_ctx();
    uint16_t attr_id = 0x0000;
    if (ctx != NULL && ctx->conv != NULL) {
        /* Cross-reference from_zigbee entries to find matching attr_id */
        for (uint8_t i = 0; i < ctx->conv->from_zigbee_count; i++) {
            if (ctx->conv->from_zigbee[i].cluster_id == 0xFCC0) {
                attr_id = ctx->conv->from_zigbee[i].attr_id;
                break;
            }
        }
    }
    if (ctx != NULL && ctx->attr_id != 0) {
        attr_id = ctx->attr_id;
    }

    /* Prepare write buffer based on JSON value type */
    uint8_t write_buf[4];
    uint16_t write_size;
    uint8_t zcl_type;

    if (cJSON_IsNumber(value)) {
        double num = cJSON_GetNumberValue(value);
        if (num >= 0 && num <= 255 && num == (int)num) {
            /* uint8 */
            zcl_type = ESP_ZB_ZCL_ATTR_TYPE_U8;
            write_buf[0] = (uint8_t)num;
            write_size = 1;
        } else if (num >= 0 && num <= 65535 && num == (int)num) {
            /* uint16 */
            zcl_type = ESP_ZB_ZCL_ATTR_TYPE_U16;
            uint16_t raw_val = (uint16_t)num;
            memcpy(write_buf, &raw_val, sizeof(raw_val));
            write_size = 2;
        } else {
            /* uint32 */
            zcl_type = ESP_ZB_ZCL_ATTR_TYPE_U32;
            uint32_t raw_val = (uint32_t)num;
            memcpy(write_buf, &raw_val, sizeof(raw_val));
            write_size = 4;
        }
    } else if (cJSON_IsString(value)) {
        /* String value: try to interpret as a number */
        const char *str = cJSON_GetStringValue(value);
        if (str == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        char *endptr;
        long num = strtol(str, &endptr, 0);
        if (*endptr != '\0') {
            ESP_LOGW(TAG, "tz_aqara_opple: cannot parse string '%s' as number", str);
            return ESP_ERR_INVALID_ARG;
        }
        if (num >= 0 && num <= 255) {
            zcl_type = ESP_ZB_ZCL_ATTR_TYPE_U8;
            write_buf[0] = (uint8_t)num;
            write_size = 1;
        } else if (num >= 0 && num <= 65535) {
            zcl_type = ESP_ZB_ZCL_ATTR_TYPE_U16;
            uint16_t raw_val = (uint16_t)num;
            memcpy(write_buf, &raw_val, sizeof(raw_val));
            write_size = 2;
        } else {
            zcl_type = ESP_ZB_ZCL_ATTR_TYPE_U32;
            uint32_t raw_val = (uint32_t)num;
            memcpy(write_buf, &raw_val, sizeof(raw_val));
            write_size = 4;
        }
    } else {
        ESP_LOGW(TAG, "tz_aqara_opple: unsupported JSON value type");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "tz_aqara_opple: 0x%04X ep=%d attr=0x%04X type=0x%02X size=%d",
             short_addr, endpoint, attr_id, zcl_type, write_size);

    esp_zb_zcl_write_attr_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = 0xFCC0,
        .manuf_specific = 1,
        .manuf_code = 0x115F,
    };

    esp_zb_zcl_attribute_t attr = {
        .id = attr_id,
        .data = {
            .type = zcl_type,
            .size = write_size,
            .value = write_buf,
        },
    };
    cmd_req.attr_number = 1;
    cmd_req.attr_field = &attr;

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_write_attr_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Aqara Opple write queued (TSN=%d)", tsn);
    return ESP_OK;
}

/* ============================================================================
 * IKEA Proprietary (cluster 0xFC7D)
 * ============================================================================ */

/**
 * @brief fromZigbee: IKEA Air Purifier proprietary cluster
 *
 * Reads PM2.5 measured value (attr 0x0001, uint16) and
 * filter runtime (attr 0x0002, uint32 in minutes).
 * Uses dispatch context to distinguish which attribute was reported.
 *
 * @param raw  Raw attribute data
 * @param len  Length of raw data
 * @param json Output JSON object
 * @param key  JSON key to store the value under
 * @return ESP_OK on success
 */
esp_err_t fz_ikea_air_purifier(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    const zb_dispatch_ctx_t *ctx = zb_converter_get_dispatch_ctx();
    uint16_t attr_id = ctx ? ctx->attr_id : 0x0001;

    if (attr_id == 0x0002 && len >= 4) {
        /* Filter runtime in minutes (uint32) */
        uint32_t runtime_min;
        memcpy(&runtime_min, raw, sizeof(runtime_min));
        cJSON_AddNumberToObject(json, key, (double)runtime_min);
        ESP_LOGD(TAG, "fz_ikea_air_purifier: filter_runtime=%" PRIu32 " min", runtime_min);
    } else {
        /* PM2.5 measured value (uint16) */
        uint16_t pm25;
        memcpy(&pm25, raw, sizeof(pm25));
        cJSON_AddNumberToObject(json, key, pm25);
        ESP_LOGD(TAG, "fz_ikea_air_purifier: pm25=%" PRIu16 " ug/m3", pm25);
    }

    return ESP_OK;
}

/**
 * @brief fromZigbee: IKEA VOC index from proprietary cluster 0xFC7D
 *
 * Reads VOC index (attr 0x0003, uint16, Sensirion SGP40 scale 0-500).
 *
 * @param raw  Raw attribute data
 * @param len  Length of raw data
 * @param json Output JSON object
 * @param key  JSON key to store the value under
 * @return ESP_OK on success
 */
esp_err_t fz_ikea_voc_index(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t voc_index;
    memcpy(&voc_index, raw, sizeof(voc_index));

    /* Clamp to valid SGP40 range 0-500 */
    if (voc_index > 500) {
        ESP_LOGW(TAG, "fz_ikea_voc_index: value %" PRIu16 " exceeds SGP40 range, clamping to 500",
                 voc_index);
        voc_index = 500;
    }

    cJSON_AddNumberToObject(json, key, voc_index);
    ESP_LOGD(TAG, "fz_ikea_voc_index: voc_index=%" PRIu16, voc_index);

    return ESP_OK;
}

/* ============================================================================
 * Philips Hue Proprietary (cluster 0xFC00)
 * ============================================================================ */

/**
 * @brief fromZigbee: Philips Hue motion sensor proprietary attributes
 *
 * Reads sensitivity (attr 0x0030, uint8) and LED indication (attr 0x0033, bool)
 * from cluster 0xFC00. Uses dispatch context to distinguish attributes.
 *
 * @param raw  Raw attribute data
 * @param len  Length of raw data
 * @param json Output JSON object
 * @param key  JSON key to store the value under
 * @return ESP_OK on success
 */
esp_err_t fz_philips_hue_motion(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    const zb_dispatch_ctx_t *ctx = zb_converter_get_dispatch_ctx();
    uint16_t attr_id = ctx ? ctx->attr_id : 0x0030;

    if (attr_id == 0x0033) {
        /* LED indication (bool) */
        bool led_on = *(const uint8_t *)raw != 0;
        cJSON_AddBoolToObject(json, key, led_on);
        ESP_LOGD(TAG, "fz_philips_hue_motion: led_indication=%s", led_on ? "true" : "false");
    } else {
        /* Sensitivity (uint8) — attr 0x0030 */
        uint8_t sensitivity = *(const uint8_t *)raw;
        cJSON_AddNumberToObject(json, key, sensitivity);
        ESP_LOGD(TAG, "fz_philips_hue_motion: sensitivity=%" PRIu8, sensitivity);
    }

    return ESP_OK;
}

/* ============================================================================
 * ZCL Identify Cluster (0x0003)
 * ============================================================================ */

/**
 * @brief toZigbee: Send ZCL Identify command with duration
 *
 * Sends the standard Identify command (cluster 0x0003, cmd 0x00) to make
 * the target device identify itself for the specified number of seconds.
 *
 * @param short_addr  Device short address
 * @param endpoint    Target endpoint
 * @param value       JSON number — identify duration in seconds
 * @return ESP_OK on success
 */
esp_err_t tz_identify(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsNumber(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    double duration = cJSON_GetNumberValue(value);
    if (duration < 0) duration = 0;
    if (duration > 65534) duration = 65534;
    uint16_t identify_time = (uint16_t)duration;

    ESP_LOGI(TAG, "tz_identify: 0x%04X ep=%d duration=%" PRIu16 "s",
             short_addr, endpoint, identify_time);

    esp_zb_zcl_identify_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .identify_time = identify_time,
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_identify_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Identify command queued (TSN=%d)", tsn);
    return ESP_OK;
}

/**
 * @brief toZigbee: Send ZCL Identify Trigger Effect command
 *
 * Sends the Trigger Effect command (cluster 0x0003, cmd 0x40) with
 * a named effect. The effect variant is always 0x00 (default).
 *
 * Supported effect strings:
 *   - "blink"           -> 0x00 (on/off once)
 *   - "breathe"         -> 0x01 (on/off 15 times over 1s each)
 *   - "okay"            -> 0x02 (green flash or double blink)
 *   - "channel_change"  -> 0x0B (orange 8s or max/min brightness)
 *   - "finish_effect"   -> 0xFE (complete current effect then stop)
 *   - "stop_effect"     -> 0xFF (stop immediately)
 *
 * @param short_addr  Device short address
 * @param endpoint    Target endpoint
 * @param value       JSON string — effect name
 * @return ESP_OK on success
 */
esp_err_t tz_identify_effect(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsString(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *effect_str = cJSON_GetStringValue(value);
    if (effect_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t effect_id;
    if (strcasecmp(effect_str, "blink") == 0) {
        effect_id = ESP_ZB_ZCL_IDENTIFY_EFFECT_ID_BLINK;
    } else if (strcasecmp(effect_str, "breathe") == 0) {
        effect_id = ESP_ZB_ZCL_IDENTIFY_EFFECT_ID_BREATHE;
    } else if (strcasecmp(effect_str, "okay") == 0) {
        effect_id = ESP_ZB_ZCL_IDENTIFY_EFFECT_ID_OKAY;
    } else if (strcasecmp(effect_str, "channel_change") == 0) {
        effect_id = ESP_ZB_ZCL_IDENTIFY_EFFECT_ID_CHANNEL_CHANGE;
    } else if (strcasecmp(effect_str, "finish_effect") == 0) {
        effect_id = ESP_ZB_ZCL_IDENTIFY_EFFECT_ID_FINISH_EFFECT;
    } else if (strcasecmp(effect_str, "stop_effect") == 0) {
        effect_id = ESP_ZB_ZCL_IDENTIFY_EFFECT_ID_STOP;
    } else {
        ESP_LOGW(TAG, "tz_identify_effect: unknown effect '%s'", effect_str);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "tz_identify_effect: 0x%04X ep=%d effect=%s (0x%02X)",
             short_addr, endpoint, effect_str, effect_id);

    esp_zb_zcl_identify_trigger_effect_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .effect_id = effect_id,
        .effect_variant = 0x00,
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_identify_trigger_effect_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Identify trigger effect queued (TSN=%d)", tsn);
    return ESP_OK;
}
