/**
 * @file zb_converter_std_security.c
 * @brief Security Converter Functions: IAS Zone, IAS WD, Door Lock
 *
 * Provides fromZigbee (fz_*) and toZigbee (tz_*) converter functions for
 * security-related clusters: IAS Zone (0x0500), IAS WD (0x0502),
 * and Door Lock (0x0101).
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

static const char *TAG = "CONV_SEC";

/* ============================================================================
 * fromZigbee (fz_*) Converters — Security Clusters
 * ============================================================================ */

/**
 * @brief Parse IAS Zone enrollment status from cluster 0x0500
 *
 * Reads zone state (enum8) and zone type (enum16) from raw data.
 * Outputs a JSON object with "enrolled" bool under the given key.
 *
 * Expected raw layout (attribute report of ZoneState 0x0000):
 *   byte 0: zone_state (0x00=not enrolled, 0x01=enrolled)
 * If additional bytes present:
 *   bytes 1-2: zone_type (uint16 LE)
 *
 * @param raw   Raw attribute data
 * @param len   Length of raw data
 * @param json  Output JSON object
 * @param key   JSON key for the enrollment object
 * @return ESP_OK on success
 */
esp_err_t fz_ias_zone_enrollment(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *data = (const uint8_t *)raw;
    uint8_t zone_state = data[0];
    bool enrolled = (zone_state == 0x01);

    cJSON *enrollment = cJSON_CreateObject();
    if (enrollment == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(enrollment, "enrolled", enrolled);
    cJSON_AddNumberToObject(enrollment, "zone_state", zone_state);

    /* If zone type is available in the data */
    if (len >= 3) {
        uint16_t zone_type;
        memcpy(&zone_type, &data[1], sizeof(zone_type));
        cJSON_AddNumberToObject(enrollment, "zone_type", zone_type);
        ESP_LOGD(TAG, "IAS Zone enrollment: state=%d type=0x%04X", zone_state, zone_type);
    } else {
        ESP_LOGD(TAG, "IAS Zone enrollment: state=%d", zone_state);
    }

    cJSON_AddItemToObject(json, key, enrollment);
    return ESP_OK;
}

/**
 * @brief Parse IAS WD (Warning Device / Siren) warning mode from cluster 0x0502
 *
 * Reads a uint8 warning mode value and maps to a string:
 *   0 = "stop", 1 = "burglar", 2 = "fire", 3 = "emergency"
 *
 * @param raw   Raw attribute data (uint8 warning mode)
 * @param len   Length of raw data
 * @param json  Output JSON object
 * @param key   JSON key for the warning mode string
 * @return ESP_OK on success
 */
esp_err_t fz_ias_wd(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mode = *(const uint8_t *)raw;
    const char *mode_str;

    switch (mode) {
        case 0: mode_str = "stop"; break;
        case 1: mode_str = "burglar"; break;
        case 2: mode_str = "fire"; break;
        case 3: mode_str = "emergency"; break;
        default:
            ESP_LOGW(TAG, "Unknown IAS WD warning mode: %d", mode);
            mode_str = "stop";
            break;
    }

    cJSON_AddStringToObject(json, key, mode_str);
    ESP_LOGD(TAG, "IAS WD mode: %d -> %s", mode, mode_str);
    return ESP_OK;
}

/**
 * @brief Parse Door Lock user status from cluster 0x0101, attr 0x0002
 *
 * Reads a uint8 user status and maps to a string:
 *   0 = "available", 1 = "occupied", 3 = "reserved"
 *
 * @param raw   Raw attribute data (uint8 user status)
 * @param len   Length of raw data
 * @param json  Output JSON object
 * @param key   JSON key for the user status string
 * @return ESP_OK on success
 */
esp_err_t fz_door_lock_user_status(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status = *(const uint8_t *)raw;
    const char *status_str;

    switch (status) {
        case 0: status_str = "available"; break;
        case 1: status_str = "occupied"; break;
        case 3: status_str = "reserved"; break;
        default:
            ESP_LOGW(TAG, "Unknown door lock user status: %d", status);
            status_str = "available";
            break;
    }

    cJSON_AddStringToObject(json, key, status_str);
    ESP_LOGD(TAG, "Door lock user status: %d -> %s", status, status_str);
    return ESP_OK;
}

/**
 * @brief Parse Door Lock PIN code response from cluster 0x0101
 *
 * Expected raw layout (Get PIN Code Response):
 *   byte 0-1: user_id (uint16 LE)
 *   byte 2:   user_status (uint8)
 *   byte 3:   user_type (uint8)
 *   byte 4:   pin_length (uint8)
 *   byte 5+:  pin_code (string, pin_length bytes)
 *
 * @param raw   Raw response data
 * @param len   Length of raw data
 * @param json  Output JSON object
 * @param key   JSON key for the PIN code object
 * @return ESP_OK on success
 */
esp_err_t fz_door_lock_pin_code(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 5) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *data = (const uint8_t *)raw;

    uint16_t user_id;
    memcpy(&user_id, &data[0], sizeof(user_id));
    uint8_t user_status = data[2];
    uint8_t user_type = data[3];
    uint8_t pin_length = data[4];

    cJSON *pin_obj = cJSON_CreateObject();
    if (pin_obj == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(pin_obj, "user_id", user_id);
    cJSON_AddNumberToObject(pin_obj, "user_status", user_status);
    cJSON_AddNumberToObject(pin_obj, "user_type", user_type);

    if (pin_length > 0 && len >= (size_t)(5 + pin_length)) {
        char pin_buf[64];
        size_t copy_len = (pin_length < sizeof(pin_buf) - 1) ? pin_length : sizeof(pin_buf) - 1;
        memcpy(pin_buf, &data[5], copy_len);
        pin_buf[copy_len] = '\0';
        cJSON_AddStringToObject(pin_obj, "pin_code", pin_buf);
    } else {
        cJSON_AddStringToObject(pin_obj, "pin_code", "");
    }

    cJSON_AddItemToObject(json, key, pin_obj);
    ESP_LOGD(TAG, "Door lock PIN: user_id=%" PRIu16 " status=%d type=%d pin_len=%d",
             user_id, user_status, user_type, pin_length);
    return ESP_OK;
}

/**
 * @brief Parse Door Lock operation event from cluster 0x0101
 *
 * Extracts operation event source (uint8), event_code (uint8),
 * and user_id (uint16) from raw data. Outputs as JSON object.
 *
 * Expected raw layout (Operation Event Notification):
 *   byte 0:   source (uint8) — 0=keypad, 1=RF, 2=manual, 0xFF=indeterminate
 *   byte 1:   event_code (uint8) — 0=unknown, 1=lock, 2=unlock, 3=lock_not_fully
 *   byte 2-3: user_id (uint16 LE)
 *
 * @param raw   Raw event data
 * @param len   Length of raw data
 * @param json  Output JSON object
 * @param key   JSON key for the event object
 * @return ESP_OK on success
 */
esp_err_t fz_door_lock_event(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 4) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *data = (const uint8_t *)raw;
    uint8_t source = data[0];
    uint8_t event_code = data[1];
    uint16_t user_id;
    memcpy(&user_id, &data[2], sizeof(user_id));

    cJSON *event = cJSON_CreateObject();
    if (event == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Map source to string */
    const char *source_str;
    switch (source) {
        case 0:    source_str = "keypad"; break;
        case 1:    source_str = "rf"; break;
        case 2:    source_str = "manual"; break;
        case 0xFF: source_str = "indeterminate"; break;
        default:
            ESP_LOGW(TAG, "Unknown door lock event source: %d", source);
            source_str = "unknown";
            break;
    }

    /* Map event_code to string */
    const char *event_str;
    switch (event_code) {
        case 0: event_str = "unknown"; break;
        case 1: event_str = "lock"; break;
        case 2: event_str = "unlock"; break;
        case 3: event_str = "lock_not_fully"; break;
        default:
            ESP_LOGW(TAG, "Unknown door lock event code: %d", event_code);
            event_str = "unknown";
            break;
    }

    cJSON_AddStringToObject(event, "source", source_str);
    cJSON_AddNumberToObject(event, "source_raw", source);
    cJSON_AddStringToObject(event, "event", event_str);
    cJSON_AddNumberToObject(event, "event_code", event_code);
    cJSON_AddNumberToObject(event, "user_id", user_id);

    cJSON_AddItemToObject(json, key, event);
    ESP_LOGD(TAG, "Door lock event: source=%s(%d) event=%s(%d) user=%" PRIu16,
             source_str, source, event_str, event_code, user_id);
    return ESP_OK;
}

/* ============================================================================
 * toZigbee (tz_*) Converters — Security Clusters
 * ============================================================================ */

/**
 * @brief Send IAS WD Start Warning command to cluster 0x0502
 *
 * Accepts a JSON object with:
 *   - "mode":     string ("stop"/"burglar"/"fire"/"emergency") or uint8
 *   - "strobe":   bool or uint8 (0=no strobe, 1=strobe)
 *   - "duration": uint16 (seconds), default 10
 *
 * IAS WD Start Warning payload (ZCL command 0x00):
 *   byte 0: warning_mode(4 bits) | strobe(2 bits) | siren_level(2 bits)
 *   byte 1-2: duration (uint16 LE, seconds)
 *
 * @param short_addr Device short address
 * @param endpoint   Device endpoint
 * @param value      JSON object with mode/strobe/duration
 * @return ESP_OK on success
 */
esp_err_t tz_warning(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t warning_mode = 0; /* stop */
    uint8_t strobe = 0;
    uint16_t duration = 10;

    /* Parse mode */
    cJSON *mode_item = cJSON_GetObjectItem(value, "mode");
    if (mode_item != NULL) {
        if (cJSON_IsString(mode_item)) {
            const char *mode_str = cJSON_GetStringValue(mode_item);
            if (mode_str != NULL) {
                if (strcasecmp(mode_str, "stop") == 0) warning_mode = 0;
                else if (strcasecmp(mode_str, "burglar") == 0) warning_mode = 1;
                else if (strcasecmp(mode_str, "fire") == 0) warning_mode = 2;
                else if (strcasecmp(mode_str, "emergency") == 0) warning_mode = 3;
                else {
                    ESP_LOGW(TAG, "Unknown warning mode: %s, using stop", mode_str);
                    warning_mode = 0;
                }
            }
        } else if (cJSON_IsNumber(mode_item)) {
            int mode_val = (int)cJSON_GetNumberValue(mode_item);
            if (mode_val < 0 || mode_val > 15) {
                ESP_LOGW(TAG, "Warning mode out of range: %d, clamping", mode_val);
                mode_val = mode_val < 0 ? 0 : 15;
            }
            warning_mode = (uint8_t)mode_val;
        }
    }

    /* Parse strobe */
    cJSON *strobe_item = cJSON_GetObjectItem(value, "strobe");
    if (strobe_item != NULL) {
        if (cJSON_IsBool(strobe_item)) {
            strobe = cJSON_IsTrue(strobe_item) ? 1 : 0;
        } else if (cJSON_IsNumber(strobe_item)) {
            strobe = (uint8_t)cJSON_GetNumberValue(strobe_item) != 0 ? 1 : 0;
        }
    }

    /* Parse duration */
    cJSON *dur_item = cJSON_GetObjectItem(value, "duration");
    if (dur_item != NULL && cJSON_IsNumber(dur_item)) {
        double dur_val = cJSON_GetNumberValue(dur_item);
        if (dur_val < 0) dur_val = 0;
        if (dur_val > 65535) dur_val = 65535;
        duration = (uint16_t)dur_val;
    }

    ESP_LOGI(TAG, "IAS WD warning to 0x%04X EP%d: mode=%d strobe=%d duration=%" PRIu16,
             short_addr, endpoint, warning_mode, strobe, duration);

    /*
     * Build Start Warning payload:
     * Byte 0: warning_mode (bits 7-4) | strobe (bits 3-2) | siren_level (bits 1-0)
     * Bytes 1-2: duration (uint16 LE)
     */
    uint8_t payload[3];
    payload[0] = (uint8_t)((warning_mode << 4) | (strobe << 2));
    memcpy(&payload[1], &duration, sizeof(duration));

    esp_zb_zcl_custom_cluster_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = 0x0502, /* IAS WD */
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .custom_cmd_id = 0x00, /* Start Warning */
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_NULL,
            .size = sizeof(payload),
            .value = payload,
        },
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "IAS WD warning command queued, TSN=0x%02X", tsn);
    return ESP_OK;
}

/**
 * @brief Write Door Lock PIN code via ZCL command on cluster 0x0101
 *
 * Accepts a JSON object with:
 *   - "user_id":  uint16 (user slot)
 *   - "pin_code": string (PIN digits)
 *   - "user_status": uint8 (optional, default 1 = occupied/enabled)
 *   - "user_type": uint8 (optional, default 0 = unrestricted)
 *
 * ZCL Set PIN Code command (0x05):
 *   bytes 0-1: user_id (uint16 LE)
 *   byte 2:    user_status
 *   byte 3:    user_type
 *   byte 4:    pin_length
 *   byte 5+:   pin_code (ASCII)
 *
 * @param short_addr Device short address
 * @param endpoint   Device endpoint
 * @param value      JSON object with user_id and pin_code
 * @return ESP_OK on success
 */
esp_err_t tz_door_lock_pin(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    if (value == NULL || !cJSON_IsObject(value)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *uid_item = cJSON_GetObjectItem(value, "user_id");
    cJSON *pin_item = cJSON_GetObjectItem(value, "pin_code");
    if (uid_item == NULL || !cJSON_IsNumber(uid_item) ||
        pin_item == NULL || !cJSON_IsString(pin_item)) {
        ESP_LOGW(TAG, "tz_door_lock_pin: missing user_id (number) or pin_code (string)");
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t user_id = (uint16_t)cJSON_GetNumberValue(uid_item);
    const char *pin_code = cJSON_GetStringValue(pin_item);
    if (pin_code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t pin_length = (uint8_t)strlen(pin_code);
    if (pin_length > 32) {
        ESP_LOGW(TAG, "PIN code too long (%d), truncating to 32", pin_length);
        pin_length = 32;
    }

    /* Optional fields */
    uint8_t user_status = 1; /* occupied/enabled */
    uint8_t user_type = 0;   /* unrestricted */

    cJSON *status_item = cJSON_GetObjectItem(value, "user_status");
    if (status_item != NULL && cJSON_IsNumber(status_item)) {
        user_status = (uint8_t)cJSON_GetNumberValue(status_item);
    }

    cJSON *type_item = cJSON_GetObjectItem(value, "user_type");
    if (type_item != NULL && cJSON_IsNumber(type_item)) {
        user_type = (uint8_t)cJSON_GetNumberValue(type_item);
    }

    ESP_LOGI(TAG, "Setting PIN for 0x%04X EP%d: user_id=%" PRIu16 " pin_len=%d",
             short_addr, endpoint, user_id, pin_length);

    /* Build Set PIN Code command payload */
    uint8_t payload[5 + 32]; /* max 32-char PIN */
    memcpy(&payload[0], &user_id, sizeof(user_id));
    payload[2] = user_status;
    payload[3] = user_type;
    payload[4] = pin_length;
    memcpy(&payload[5], pin_code, pin_length);

    size_t payload_size = 5 + pin_length;

    esp_zb_zcl_custom_cluster_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = 0x0101, /* Door Lock */
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .custom_cmd_id = 0x05, /* Set PIN Code */
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_NULL,
            .size = payload_size,
            .value = payload,
        },
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Door lock Set PIN command queued, TSN=0x%02X", tsn);
    return ESP_OK;
}

/**
 * @brief Send IAS Zone Enroll Response on cluster 0x0500
 *
 * Accepts a JSON object with:
 *   - "zone_id": uint8 (zone ID to assign), default 0
 *   - "response": string ("success"/"not_supported"/"no_permit"/"too_many"),
 *                 default "success"
 *
 * ZCL Zone Enroll Response (command 0x00, client-to-server):
 *   byte 0: enroll_response_code (uint8)
 *   byte 1: zone_id (uint8)
 *
 * @param short_addr Device short address
 * @param endpoint   Device endpoint
 * @param value      JSON object with zone_id and response
 * @return ESP_OK on success
 */
esp_err_t tz_ias_zone_enroll_response(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    uint8_t zone_id = 0;
    uint8_t response_code = 0; /* success */

    if (value != NULL && cJSON_IsObject(value)) {
        /* Parse zone_id */
        cJSON *zid_item = cJSON_GetObjectItem(value, "zone_id");
        if (zid_item != NULL && cJSON_IsNumber(zid_item)) {
            double zid_val = cJSON_GetNumberValue(zid_item);
            if (zid_val < 0) zid_val = 0;
            if (zid_val > 255) zid_val = 255;
            zone_id = (uint8_t)zid_val;
        }

        /* Parse response code */
        cJSON *resp_item = cJSON_GetObjectItem(value, "response");
        if (resp_item != NULL && cJSON_IsString(resp_item)) {
            const char *resp_str = cJSON_GetStringValue(resp_item);
            if (resp_str != NULL) {
                if (strcasecmp(resp_str, "success") == 0) response_code = 0;
                else if (strcasecmp(resp_str, "not_supported") == 0) response_code = 1;
                else if (strcasecmp(resp_str, "no_permit") == 0) response_code = 2;
                else if (strcasecmp(resp_str, "too_many") == 0) response_code = 3;
                else {
                    ESP_LOGW(TAG, "Unknown enroll response: %s, using success", resp_str);
                    response_code = 0;
                }
            }
        } else if (resp_item != NULL && cJSON_IsNumber(resp_item)) {
            int resp_val = (int)cJSON_GetNumberValue(resp_item);
            if (resp_val < 0 || resp_val > 3) {
                ESP_LOGW(TAG, "Enroll response code out of range: %d, using success", resp_val);
                resp_val = 0;
            }
            response_code = (uint8_t)resp_val;
        }
    }

    ESP_LOGI(TAG, "IAS Zone enroll response to 0x%04X EP%d: code=%d zone_id=%d",
             short_addr, endpoint, response_code, zone_id);

    /* Build Zone Enroll Response payload */
    uint8_t payload[2] = { response_code, zone_id };

    esp_zb_zcl_custom_cluster_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = 0x0500, /* IAS Zone */
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .custom_cmd_id = 0x00, /* Zone Enroll Response */
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_NULL,
            .size = sizeof(payload),
            .value = payload,
        },
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "IAS Zone enroll response queued, TSN=0x%02X", tsn);
    return ESP_OK;
}
