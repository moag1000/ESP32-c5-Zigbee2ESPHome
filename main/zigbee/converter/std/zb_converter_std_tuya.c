/**
 * @file zb_converter_std_tuya.c
 * @brief Universal Tuya DataPoint Converter Functions
 *
 * Provides fz_tuya_dp (fromZigbee) and tz_tuya_dp (toZigbee) converters
 * for the Tuya private cluster 0xEF00 DP protocol.
 *
 * fz_tuya_dp parses incoming DP reports into JSON. If the converter
 * definition has a tuya_driver, the driver's process_dp handles
 * semantic mapping. Otherwise, generic keys (dp_1, dp_2, ...) are used.
 *
 * tz_tuya_dp builds DP payloads from JSON commands and sends them
 * via zb_tuya_send_dp().
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/converter/zb_converter_std.h"
#include "zigbee/converter/zb_converter.h"
#include "zigbee/zb_tuya.h"
#include "zigbee/tuya/tuya_device_driver.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "core/gateway_timeouts.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "CONV_TUYA";

/* ============================================================================
 * fz_tuya_dp — Universal Tuya DataPoint Parser (fromZigbee)
 *
 * Parses Tuya DP reports from cluster 0xEF00.
 *
 * Raw payload format (6-byte header, no status byte):
 *   [seq_hi][seq_lo][dp_id][dp_type][len_hi][len_lo][value...]
 *
 * dp_type:
 *   0x00 = raw bytes    -> skip (log warning)
 *   0x01 = bool         -> JSON bool
 *   0x02 = value        -> JSON number (4-byte big-endian int32)
 *   0x03 = string       -> JSON string
 *   0x04 = enum         -> JSON number (1 byte)
 *   0x05 = bitmap       -> JSON number (1/2/4 bytes)
 * ============================================================================ */

esp_err_t fz_tuya_dp(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || len < 6) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *data = (const uint8_t *)raw;

    /* Parse DP header: [seq_hi][seq_lo][dp_id][dp_type][len_hi][len_lo] */
    uint8_t dp_id   = data[2];
    uint8_t dp_type = data[3];
    uint16_t dp_len = ((uint16_t)data[4] << 8) | data[5];

    /* Validate payload length */
    if (len < 6 + dp_len) {
        ESP_LOGW(TAG, "fz_tuya_dp: payload truncated, dp_id=%d need %d have %zu",
                 dp_id, dp_len, len - 6);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *payload = &data[6];

    /* Check for tuya_driver in dispatch context */
    const zb_dispatch_ctx_t *ctx = zb_converter_get_dispatch_ctx();
    if (ctx != NULL && ctx->conv != NULL && ctx->conv->tuya_driver != NULL) {
        /* Driver handles semantic DP parsing via its own process_dp callback.
         * Parse into tuya_dp_t and delegate — the driver will update its
         * internal state and publish via the event bus. We still output
         * generic keys as a fallback for JSON state. */
        ESP_LOGD(TAG, "fz_tuya_dp: driver '%s' bound, dp_id=%d type=%d",
                 ctx->conv->tuya_driver->name, dp_id, dp_type);
    }

    /* Build generic key: "dp_N" */
    char dp_key[16];
    snprintf(dp_key, sizeof(dp_key), "dp_%d", dp_id);

    /* Look up semantic key from DP map if available */
    const tuya_dp_entry_t *dp_entry = NULL;
    if (ctx != NULL && ctx->conv != NULL && ctx->conv->tuya_dp_map != NULL) {
        for (uint8_t i = 0; i < ctx->conv->tuya_dp_count; i++) {
            if (ctx->conv->tuya_dp_map[i].dp_id == dp_id) {
                dp_entry = &ctx->conv->tuya_dp_map[i];
                break;
            }
        }
    }

    /* Use semantic key if mapped, otherwise fallback to dp_N or provided key */
    const char *out_key = dp_entry && dp_entry->key ? dp_entry->key :
                          ((key != NULL && key[0] != '\0') ? key : dp_key);

    switch (dp_type) {
    case TUYA_DP_TYPE_BOOL:
        if (dp_len < 1) {
            ESP_LOGW(TAG, "fz_tuya_dp: bool dp_id=%d has length 0", dp_id);
            return ESP_ERR_INVALID_SIZE;
        }
        cJSON_AddBoolToObject(json, out_key, payload[0] != 0);
        ESP_LOGD(TAG, "fz_tuya_dp: dp_%d -> '%s' (bool) = %s", dp_id, out_key,
                 payload[0] ? "true" : "false");
        break;

    case TUYA_DP_TYPE_VALUE:
        if (dp_len < 4) {
            ESP_LOGW(TAG, "fz_tuya_dp: value dp_id=%d has length %d (need 4)",
                     dp_id, dp_len);
            return ESP_ERR_INVALID_SIZE;
        }
        {
            int32_t value = ((int32_t)payload[0] << 24) |
                            ((int32_t)payload[1] << 16) |
                            ((int32_t)payload[2] << 8)  |
                             (int32_t)payload[3];
            /* Apply scale from DP map if present */
            if (dp_entry && dp_entry->scale != 0.0f) {
                double scaled = (double)value * (double)dp_entry->scale;
                cJSON_AddNumberToObject(json, out_key, scaled);
                ESP_LOGD(TAG, "fz_tuya_dp: dp_%d -> '%s' (value) = %" PRId32 " * %.4f = %.2f",
                         dp_id, out_key, value, (double)dp_entry->scale, scaled);
            } else {
                cJSON_AddNumberToObject(json, out_key, value);
                ESP_LOGD(TAG, "fz_tuya_dp: dp_%d -> '%s' (value) = %" PRId32, dp_id, out_key, value);
            }
        }
        break;

    case TUYA_DP_TYPE_STRING:
        if (dp_len == 0) {
            cJSON_AddStringToObject(json, out_key, "");
            ESP_LOGD(TAG, "fz_tuya_dp: dp_%d -> '%s' (string) = \"\"", dp_id, out_key);
        } else {
            /* Copy with null terminator */
            char buf[128];
            size_t copy_len = (dp_len < sizeof(buf) - 1) ? dp_len : sizeof(buf) - 1;
            memcpy(buf, payload, copy_len);
            buf[copy_len] = '\0';
            cJSON_AddStringToObject(json, out_key, buf);
            ESP_LOGD(TAG, "fz_tuya_dp: dp_%d -> '%s' (string) = \"%s\"", dp_id, out_key, buf);
        }
        break;

    case TUYA_DP_TYPE_ENUM:
        if (dp_len < 1) {
            ESP_LOGW(TAG, "fz_tuya_dp: enum dp_id=%d has length 0", dp_id);
            return ESP_ERR_INVALID_SIZE;
        }
        /* Map raw enum value to display string if DP map has enum_names */
        if (dp_entry && dp_entry->enum_names && dp_entry->enum_count > 0) {
            const char *enum_str = NULL;
            for (uint8_t i = 0; i < dp_entry->enum_count; i++) {
                if (dp_entry->enum_values[i] == payload[0]) {
                    enum_str = dp_entry->enum_names[i];
                    break;
                }
            }
            if (enum_str) {
                cJSON_AddStringToObject(json, out_key, enum_str);
                ESP_LOGD(TAG, "fz_tuya_dp: dp_%d -> '%s' (enum) = %d -> \"%s\"",
                         dp_id, out_key, payload[0], enum_str);
            } else {
                cJSON_AddNumberToObject(json, out_key, payload[0]);
                ESP_LOGD(TAG, "fz_tuya_dp: dp_%d -> '%s' (enum) = %d (unmapped)",
                         dp_id, out_key, payload[0]);
            }
        } else {
            cJSON_AddNumberToObject(json, out_key, payload[0]);
            ESP_LOGD(TAG, "fz_tuya_dp: dp_%d -> '%s' (enum) = %d", dp_id, out_key, payload[0]);
        }
        break;

    case TUYA_DP_TYPE_BITMAP:
        if (dp_len == 0) {
            ESP_LOGW(TAG, "fz_tuya_dp: bitmap dp_id=%d has length 0", dp_id);
            return ESP_ERR_INVALID_SIZE;
        }
        {
            uint32_t bitmap = 0;
            if (dp_len == 1) {
                bitmap = payload[0];
            } else if (dp_len == 2) {
                bitmap = ((uint32_t)payload[0] << 8) | payload[1];
            } else if (dp_len >= 4) {
                bitmap = ((uint32_t)payload[0] << 24) |
                         ((uint32_t)payload[1] << 16) |
                         ((uint32_t)payload[2] << 8)  |
                          (uint32_t)payload[3];
            }
            cJSON_AddNumberToObject(json, out_key, (double)bitmap);
            ESP_LOGD(TAG, "fz_tuya_dp: dp_%d -> '%s' (bitmap) = 0x%08" PRIx32,
                     dp_id, out_key, bitmap);
        }
        break;

    case TUYA_DP_TYPE_RAW:
        ESP_LOGW(TAG, "fz_tuya_dp: dp_%d has raw type (len=%d), skipping",
                 dp_id, dp_len);
        /* Raw bytes are not representable in JSON without encoding.
         * The tuya_driver should handle these via process_dp(). */
        return ESP_OK;

    default:
        ESP_LOGW(TAG, "fz_tuya_dp: dp_%d has unknown type 0x%02X", dp_id, dp_type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

/* ============================================================================
 * tz_tuya_dp — Universal Tuya DataPoint Writer (toZigbee)
 *
 * Writes a Tuya DP command to cluster 0xEF00.
 *
 * Uses dispatch context to get the converter definition and extract
 * the dp_id from the json_key (format: "dp_N" where N is the DP number,
 * or named keys mapped via tuya_driver).
 *
 * Builds the DP payload and sends via zb_tuya_send_dp().
 * ============================================================================ */

esp_err_t tz_tuya_dp(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const zb_dispatch_ctx_t *ctx = zb_converter_get_dispatch_ctx();
    if (ctx == NULL || ctx->conv == NULL) {
        ESP_LOGW(TAG, "tz_tuya_dp: no dispatch context");
        return ESP_ERR_INVALID_STATE;
    }

    /* Find the matched to_zigbee entry to get the json_key */
    const char *json_key = NULL;
    for (uint8_t i = 0; i < ctx->conv->to_zigbee_count; i++) {
        if (ctx->conv->to_zigbee[i].convert == tz_tuya_dp &&
            ctx->conv->to_zigbee[i].cluster_id == ZB_TUYA_CLUSTER_ID) {
            json_key = ctx->conv->to_zigbee[i].json_key;
            break;
        }
    }

    /* Extract dp_id from json_key — try DP map first for semantic keys */
    uint8_t dp_id = 0;
    bool dp_found = false;
    const tuya_dp_entry_t *dp_entry = NULL;
    uint8_t dp_type_hint = 0xFF;  /* 0xFF = unknown */
    float tz_scale = 0.0f;

    /* Try DP map reverse lookup: semantic key -> dp_id */
    if (json_key != NULL && ctx->conv->tuya_dp_map != NULL) {
        for (uint8_t i = 0; i < ctx->conv->tuya_dp_count; i++) {
            if (ctx->conv->tuya_dp_map[i].key != NULL &&
                strcmp(ctx->conv->tuya_dp_map[i].key, json_key) == 0) {
                dp_entry = &ctx->conv->tuya_dp_map[i];
                dp_id = dp_entry->dp_id;
                dp_type_hint = dp_entry->dp_type;
                tz_scale = dp_entry->scale;
                dp_found = true;
                break;
            }
        }
    }

    /* Fallback: try "dp_N" format */
    if (!dp_found && json_key != NULL && strncmp(json_key, "dp_", 3) == 0) {
        int parsed = atoi(&json_key[3]);
        if (parsed > 0 && parsed <= 255) {
            dp_id = (uint8_t)parsed;
            dp_found = true;
        }
    }

    /* Fallback: try from_zigbee entries matching the json_key */
    if (!dp_found && json_key != NULL) {
        for (uint8_t i = 0; i < ctx->conv->from_zigbee_count; i++) {
            if (ctx->conv->from_zigbee[i].cluster_id == ZB_TUYA_CLUSTER_ID &&
                ctx->conv->from_zigbee[i].json_key != NULL &&
                strcmp(ctx->conv->from_zigbee[i].json_key, json_key) == 0) {
                dp_id = (uint8_t)ctx->conv->from_zigbee[i].attr_id;
                dp_found = true;
                break;
            }
        }
    }

    if (!dp_found) {
        ESP_LOGW(TAG, "tz_tuya_dp: cannot determine dp_id from key '%s'",
                 json_key ? json_key : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    /* Build tuya_dp_t from JSON value */
    tuya_dp_t dp;
    memset(&dp, 0, sizeof(dp));
    dp.dp_id = dp_id;

    if (cJSON_IsBool(value)) {
        dp.type = TUYA_DP_TYPE_BOOL;
        dp.length = 1;
        dp.value.bool_value = cJSON_IsTrue(value);
        ESP_LOGD(TAG, "tz_tuya_dp: dp_%d = %s (bool)", dp_id,
                 dp.value.bool_value ? "true" : "false");
    } else if (cJSON_IsNumber(value)) {
        double num = cJSON_GetNumberValue(value);

        /* Reverse scale: if fz multiplied by scale, tz must divide by scale */
        if (tz_scale != 0.0f) {
            num = num / (double)tz_scale;
        }

        /* Use dp_type_hint from DP map to pick correct Tuya type */
        if (dp_type_hint == TUYA_DP_TYPE_ENUM) {
            dp.type = TUYA_DP_TYPE_ENUM;
            dp.length = 1;
            dp.value.enum_value = (uint8_t)(int)num;
        } else if (dp_type_hint == TUYA_DP_TYPE_BOOL) {
            dp.type = TUYA_DP_TYPE_BOOL;
            dp.length = 1;
            dp.value.bool_value = ((int)num != 0);
        } else {
            dp.type = TUYA_DP_TYPE_VALUE;
            dp.length = 4;
            dp.value.int_value = (int32_t)num;
        }
        ESP_LOGD(TAG, "tz_tuya_dp: dp_%d = %" PRId32 " (type=%d)",
                 dp_id, dp.value.int_value, dp.type);
    } else if (cJSON_IsString(value)) {
        const char *str = cJSON_GetStringValue(value);
        if (str == NULL) {
            return ESP_ERR_INVALID_ARG;
        }

        /* If DP map has enum names, try reverse lookup string -> raw value */
        if (dp_entry && dp_entry->enum_names && dp_entry->enum_count > 0) {
            bool enum_found = false;
            for (uint8_t i = 0; i < dp_entry->enum_count; i++) {
                if (dp_entry->enum_names[i] &&
                    strcasecmp(dp_entry->enum_names[i], str) == 0) {
                    dp.type = TUYA_DP_TYPE_ENUM;
                    dp.length = 1;
                    dp.value.enum_value = dp_entry->enum_values[i];
                    enum_found = true;
                    ESP_LOGD(TAG, "tz_tuya_dp: dp_%d = \"%s\" -> enum %d",
                             dp_id, str, dp.value.enum_value);
                    break;
                }
            }
            if (enum_found) goto send_dp;
        }

        /* Check for boolean string values */
        if (strcasecmp(str, "true") == 0 || strcasecmp(str, "ON") == 0) {
            dp.type = TUYA_DP_TYPE_BOOL;
            dp.length = 1;
            dp.value.bool_value = true;
        } else if (strcasecmp(str, "false") == 0 || strcasecmp(str, "OFF") == 0) {
            dp.type = TUYA_DP_TYPE_BOOL;
            dp.length = 1;
            dp.value.bool_value = false;
        } else {
            /* Send as string DP */
            size_t str_len = strlen(str);
            if (str_len > ZB_TUYA_DP_MAX_RAW_SIZE) {
                ESP_LOGW(TAG, "tz_tuya_dp: string too long (%zu > %d)",
                         str_len, ZB_TUYA_DP_MAX_RAW_SIZE);
                return ESP_ERR_INVALID_SIZE;
            }
            dp.type = TUYA_DP_TYPE_STRING;
            dp.length = (uint16_t)str_len;
            memcpy(dp.value.raw, str, str_len);
        }
        ESP_LOGD(TAG, "tz_tuya_dp: dp_%d = \"%s\" (type=%d)",
                 dp_id, str, dp.type);
    } else {
        ESP_LOGW(TAG, "tz_tuya_dp: unsupported JSON type for dp_%d", dp_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

send_dp:

    /* Send via the Tuya module's send function (handles lock + framing) */
    esp_err_t ret = zb_tuya_send_dp(short_addr, endpoint, &dp);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "tz_tuya_dp: send failed for dp_%d: %s",
                 dp_id, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "tz_tuya_dp: sent dp_%d to 0x%04X ep=%d",
                 dp_id, short_addr, endpoint);
    }

    return ret;
}
