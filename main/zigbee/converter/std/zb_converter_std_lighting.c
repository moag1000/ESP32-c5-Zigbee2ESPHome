/**
 * @file zb_converter_std_lighting.c
 * @brief Standard Lighting Converter Functions
 *
 * Provides lighting-related fromZigbee and toZigbee converters:
 * - fz_color_enhance_hs: Enhanced Hue/Saturation from Color Control cluster
 * - tz_effect: Trigger Effect command on Identify cluster
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_converter_std.h"
#include "zb_converter.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "core/gateway_timeouts.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "CONV_LIGHT";

/* ============================================================================
 * fromZigbee (fz_*) Converters
 * ============================================================================ */

/**
 * @brief Enhanced Hue/Saturation from Color Control cluster (0x0300)
 *
 * Reads enhanced current hue (attr 0x4000, uint16 0-65535) and current
 * saturation (attr 0x0001, uint8 0-254). Enhanced hue maps to 0-360 degrees,
 * saturation maps to 0-100%.
 *
 * Output: "color" JSON object with "hue" and "saturation" keys.
 *
 * This is called once per attribute report. The "color" object accumulates
 * both hue and saturation across separate reports (same pattern as fz_color_xy).
 */
esp_err_t fz_color_enhance_hs(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Create or get existing color object */
    cJSON *color = cJSON_GetObjectItem(json, "color");
    if (color == NULL) {
        color = cJSON_CreateObject();
        if (color == NULL) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(json, "color", color);
    }

    if (strcmp(key, "color_hue") == 0) {
        /* Enhanced current hue: uint16 0-65535 -> 0-360 degrees */
        if (len < 2) {
            return ESP_ERR_INVALID_ARG;
        }
        uint16_t enhanced_hue;
        memcpy(&enhanced_hue, raw, sizeof(enhanced_hue));
        double hue_degrees = (double)enhanced_hue * 360.0 / 65535.0;
        cJSON_AddNumberToObject(color, "hue", hue_degrees);
        ESP_LOGD(TAG, "Enhanced hue: raw=%" PRIu16 " -> %.1f deg", enhanced_hue, hue_degrees);
    } else if (strcmp(key, "color_saturation") == 0) {
        /* Current saturation: uint8 0-254 -> 0-100% */
        if (len < 1) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t sat_raw = *(const uint8_t *)raw;
        double saturation_pct = (double)sat_raw * 100.0 / 254.0;
        cJSON_AddNumberToObject(color, "saturation", saturation_pct);
        ESP_LOGD(TAG, "Saturation: raw=%u -> %.1f%%", sat_raw, saturation_pct);
    }

    return ESP_OK;
}

/* ============================================================================
 * toZigbee (tz_*) Converters
 * ============================================================================ */

/**
 * @brief Trigger Effect command on Identify cluster (0x0003)
 *
 * Sends the Trigger Effect command (cmd 0x40) with effect_id and effect_variant.
 * Accepts a JSON string value mapping to effect IDs:
 *   "blink"          -> 0x00
 *   "breathe"        -> 0x01
 *   "okay"           -> 0x02
 *   "channel_change" -> 0x0B
 *   "finish_effect"  -> 0xFE
 *   "stop_effect"    -> 0xFF
 *
 * Effect variant is always 0x00 (default).
 * Payload: [effect_id (uint8), effect_variant (uint8)]
 */
esp_err_t tz_effect(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
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
        effect_id = 0x00;
    } else if (strcasecmp(effect_str, "breathe") == 0) {
        effect_id = 0x01;
    } else if (strcasecmp(effect_str, "okay") == 0) {
        effect_id = 0x02;
    } else if (strcasecmp(effect_str, "channel_change") == 0) {
        effect_id = 0x0B;
    } else if (strcasecmp(effect_str, "finish_effect") == 0) {
        effect_id = 0xFE;
    } else if (strcasecmp(effect_str, "stop_effect") == 0) {
        effect_id = 0xFF;
    } else {
        ESP_LOGW(TAG, "Unknown effect: '%s'", effect_str);
        return ESP_ERR_INVALID_ARG;
    }

    /* Trigger Effect payload: effect_id (uint8) + effect_variant (uint8) */
    uint8_t payload[2];
    payload[0] = effect_id;
    payload[1] = 0x00; /* effect_variant: default */

    esp_zb_zcl_custom_cluster_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY, /* 0x0003 */
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .custom_cmd_id = 0x40, /* Trigger Effect command ID */
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
            .size = sizeof(payload),
            .value = payload,
        },
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Trigger Effect '%s' (0x%02X) sent to 0x%04X EP%d (TSN=%d)",
             effect_str, effect_id, short_addr, endpoint, tsn);
    return ESP_OK;
}
