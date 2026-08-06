/**
 * @file json_utils.c
 * @brief JSON Building and Parsing Utilities Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "json_utils.h"
#include "zigbee/zb_network.h"
#include "zigbee/zb_constants.h"
#include "zigbee/zb_cluster_security.h"
#include "zigbee/zb_cluster_electrical.h"
#include "core/device/device_registry.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_zigbee_core.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "JSON_UTILS";

/* Project version - should be defined in build system */
#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

#ifndef PROJECT_COMMIT
#define PROJECT_COMMIT "unknown"
#endif

cJSON* json_create_device_state_ng(const device_t *device)
{
    if (device == NULL) {
        ESP_LOGE(TAG, "Device pointer is NULL");
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return NULL;
    }

    /* Add link quality (from Zigbee protocol data) */
    cJSON_AddNumberToObject(root, "linkquality", device->proto.zigbee.lqi);

    /* Add last seen timestamp */
    char timestamp[32];
    time_t last_seen = (time_t)device->last_seen;
    struct tm timeinfo;
    localtime_r(&last_seen, &timeinfo);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    cJSON_AddStringToObject(root, "last_seen", timestamp);

    /* Add power/battery information if available (API-007) */
    if (device->proto.zigbee.power_info.power_info_valid) {
        /* Add battery percentage for battery-powered devices */
        uint8_t source = device->proto.zigbee.power_info.current_power_source;
        bool is_battery = (source & ZB_POWER_SOURCE_RECHARGEABLE_BATTERY) ||
                          (source & ZB_POWER_SOURCE_DISPOSABLE_BATTERY);

        if (is_battery) {
            /* No "battery" percentage here — the node power descriptor only
             * distinguishes critical/33/66/100 and would present a guess as a
             * measurement. See device_state_publish_battery() for the full
             * reasoning. Real levels come from genPowerCfg or a Tuya datapoint. */
            bool battery_low = (device->proto.zigbee.power_info.current_power_source_level == 0);
            cJSON_AddBoolToObject(root, "battery_low", battery_low);
        }

        /* Add power source information */
        const char *power_source_str = "Unknown";
        if (source & ZB_POWER_SOURCE_MAINS) {
            power_source_str = "Mains";
        } else if (source & ZB_POWER_SOURCE_RECHARGEABLE_BATTERY) {
            power_source_str = "Battery";
        } else if (source & ZB_POWER_SOURCE_DISPOSABLE_BATTERY) {
            power_source_str = "Battery";
        }
        cJSON_AddStringToObject(root, "power_source", power_source_str);
    }

    /* Add device-type specific attributes based on clusters */
    for (uint16_t i = 0; i < device->proto.zigbee.cluster_count; i++) {
        switch (device->proto.zigbee.clusters[i]) {
            case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
                /* On/Off state - will be updated by actual attribute reports */
                cJSON_AddStringToObject(root, "state", "UNKNOWN");
                break;

            case ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL:
                /* Brightness level - will be updated by actual attribute reports */
                cJSON_AddNumberToObject(root, "brightness", 0);
                break;

            case ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
                /* Color - will be updated by actual attribute reports */
                {
                    cJSON *color = cJSON_CreateObject();
                    if (color == NULL) {
                        ESP_LOGE(TAG, "Failed to create color JSON object");
                        cJSON_Delete(root);
                        return NULL;
                    }
                    cJSON_AddNumberToObject(color, "x", 0);
                    cJSON_AddNumberToObject(color, "y", 0);
                    cJSON_AddItemToObject(root, "color", color);
                }
                break;

            case ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT:
                /* Temperature in Celsius */
                cJSON_AddNumberToObject(root, "temperature", 0);
                break;

            case ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:
                /* Humidity in percentage */
                cJSON_AddNumberToObject(root, "humidity", 0);
                break;

            case ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING:
                /* Occupancy */
                cJSON_AddBoolToObject(root, "occupancy", false);
                break;

            case ZB_ZCL_CLUSTER_ID_DOOR_LOCK:
                /* Door Lock state - Zigbee2MQTT format */
                cJSON_AddStringToObject(root, "state", "UNLOCK");
                break;

            case ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT:
                /* Electrical measurement - will be updated by actual attribute reports */
                cJSON_AddNumberToObject(root, "voltage", 0);
                cJSON_AddNumberToObject(root, "current", 0);
                cJSON_AddNumberToObject(root, "power", 0);
                cJSON_AddNumberToObject(root, "power_factor", 0);
                break;

            default:
                break;
        }
    }

    return root;
}

cJSON* json_create_bridge_info(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    /* Version information */
    cJSON_AddStringToObject(root, "version", PROJECT_VERSION);
    cJSON_AddStringToObject(root, "commit", PROJECT_COMMIT);

    /* Coordinator IEEE address */
    esp_zb_ieee_addr_t ieee_addr;
    esp_zb_get_long_address(ieee_addr);

    char ieee_str[19];
    json_format_ieee_addr(ieee_addr, ieee_str, sizeof(ieee_str));
    cJSON_AddStringToObject(root, "coordinator_ieee", ieee_str);

    /* Network information */
    zb_network_info_t net_info;
    if (zb_network_get_info(&net_info) == ESP_OK) {
        cJSON *network = cJSON_CreateObject();
        if (network == NULL) {
            ESP_LOGE(TAG, "Failed to create network JSON object");
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddNumberToObject(network, "pan_id", net_info.pan_id);
        cJSON_AddNumberToObject(network, "channel", net_info.channel);
        cJSON_AddBoolToObject(network, "permit_join", net_info.permit_join);
        cJSON_AddNumberToObject(network, "device_count", net_info.device_count);
        cJSON_AddItemToObject(root, "network", network);
    }

    /* System information */
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "uptime", (uint32_t)(esp_timer_get_time() / 1000000));

    return root;
}

/* Callback for json_create_device_list iterator (NG unified device model) */
static bool json_device_list_iterator_ng(device_t *device, void *user_data)
{
    cJSON *array = (cJSON *)user_data;

    /* Skip virtual devices — ESPHome-only, not MQTT entities */
    if (device->protocol == DEV_PROTOCOL_VIRTUAL) {
        return true;
    }

    cJSON *device_obj = cJSON_CreateObject();
    if (device_obj == NULL) {
        return true;  /* Continue iteration even on allocation failure */
    }

    /* IEEE address - convert device_id_t to byte array for formatting */
    uint8_t ieee_addr[8];
    device_id_t id = device->id;
    for (int i = 0; i < 8; i++) {
        ieee_addr[i] = (uint8_t)(id >> (i * 8));
    }
    char ieee_str[19];
    json_format_ieee_addr(ieee_addr, ieee_str, sizeof(ieee_str));
    cJSON_AddStringToObject(device_obj, "ieee_address", ieee_str);

    /* Network address (Zigbee short address) */
    char short_addr_str[7];
    snprintf(short_addr_str, sizeof(short_addr_str), "0x%04X", device->proto.zigbee.short_addr);
    cJSON_AddStringToObject(device_obj, "network_address", short_addr_str);

    /* Friendly name */
    cJSON_AddStringToObject(device_obj, "friendly_name", device->friendly_name);

    /* Model and manufacturer */
    if (strlen(device->model) > 0) {
        cJSON_AddStringToObject(device_obj, "model", device->model);
    }
    if (strlen(device->manufacturer) > 0) {
        cJSON_AddStringToObject(device_obj, "manufacturer", device->manufacturer);
    }

    /* Device type - use unified device_zigbee_type_t */
    const char *type_str = "unknown";
    switch (device->proto.zigbee.device_type) {
        case DEVICE_ZB_TYPE_LIGHT: type_str = "light"; break;
        case DEVICE_ZB_TYPE_SWITCH: type_str = "switch"; break;
        case DEVICE_ZB_TYPE_SENSOR: type_str = "sensor"; break;
        case DEVICE_ZB_TYPE_PLUG: type_str = "plug"; break;
        case DEVICE_ZB_TYPE_COVER: type_str = "cover"; break;
        case DEVICE_ZB_TYPE_LOCK: type_str = "lock"; break;
        case DEVICE_ZB_TYPE_CLIMATE: type_str = "climate"; break;
        case DEVICE_ZB_TYPE_FAN: type_str = "fan"; break;
        case DEVICE_ZB_TYPE_METER: type_str = "meter"; break;
        case DEVICE_ZB_TYPE_IAS_ZONE: type_str = "ias_zone"; break;
        case DEVICE_ZB_TYPE_OTHER: type_str = "other"; break;
        default: break;
    }
    cJSON_AddStringToObject(device_obj, "type", type_str);

    /* Online status - convert availability enum to bool */
    bool online = (device->availability == DEV_AVAIL_ONLINE);
    cJSON_AddBoolToObject(device_obj, "online", online);

    /* Link quality (LQI) */
    cJSON_AddNumberToObject(device_obj, "link_quality", device->proto.zigbee.lqi);

    cJSON_AddItemToArray(array, device_obj);

    return true;  /* Continue iteration */
}

cJSON* json_create_device_list(void)
{
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        return NULL;
    }

    /* Iterate devices using NG device registry */
    device_registry_iterate(json_device_list_iterator_ng, array);

    return array;
}

char* json_create_availability(bool available)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "state", available ? "online" : "offline");

    return json_to_string_and_delete(root);
}

char* json_create_bridge_state(const char *state)
{
    if (state == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "state", state);

    /* Add timestamp */
    time_t now = time(NULL);
    char timestamp[32];
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    cJSON_AddStringToObject(root, "timestamp", timestamp);

    return json_to_string_and_delete(root);
}

esp_err_t json_parse_command(const char *json_str,
                             bool *state,
                             uint8_t *brightness,
                             uint16_t *color_x,
                             uint16_t *color_y,
                             uint16_t *transition)
{
    if (json_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse command JSON");
        return ESP_FAIL;
    }

    /* Parse state */
    if (state != NULL) {
        cJSON *state_item = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(state_item)) {
            const char *state_str = cJSON_GetStringValue(state_item);
            if (strcmp(state_str, "ON") == 0) {
                *state = true;
            } else if (strcmp(state_str, "OFF") == 0) {
                *state = false;
            }
        }
    }

    /* Parse brightness */
    if (brightness != NULL) {
        cJSON *brightness_item = cJSON_GetObjectItem(root, "brightness");
        if (cJSON_IsNumber(brightness_item)) {
            int val = brightness_item->valueint;
            *brightness = (val > ZCL_BRIGHTNESS_MAX) ? ZCL_BRIGHTNESS_MAX : (val < 0) ? 0 : (uint8_t)val;
        }
    }

    /* Parse color */
    if (color_x != NULL && color_y != NULL) {
        cJSON *color_item = cJSON_GetObjectItem(root, "color");
        if (cJSON_IsObject(color_item)) {
            cJSON *x_item = cJSON_GetObjectItem(color_item, "x");
            cJSON *y_item = cJSON_GetObjectItem(color_item, "y");
            if (cJSON_IsNumber(x_item)) {
                *color_x = (uint16_t)(x_item->valuedouble * 65535.0);
            }
            if (cJSON_IsNumber(y_item)) {
                *color_y = (uint16_t)(y_item->valuedouble * 65535.0);
            }
        }
    }

    /* Parse transition */
    if (transition != NULL) {
        cJSON *transition_item = cJSON_GetObjectItem(root, "transition");
        if (cJSON_IsNumber(transition_item)) {
            *transition = (uint16_t)transition_item->valueint;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t json_parse_permit_join(const char *json_str, uint8_t *duration)
{
    if (json_str == NULL || duration == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    json_permit_join_options_t options;
    esp_err_t ret = json_parse_permit_join_extended(json_str, &options);
    if (ret != ESP_OK) {
        return ret;
    }

    *duration = options.value ? options.time : 0;
    return ESP_OK;
}

esp_err_t json_parse_permit_join_extended(const char *json_str, json_permit_join_options_t *options)
{
    if (json_str == NULL || options == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Initialize options with defaults */
    options->value = false;
    options->time = 254;  /* Default duration */
    options->has_device = false;
    memset(options->device_ieee, 0, sizeof(options->device_ieee));

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse permit join JSON");
        return ESP_FAIL;
    }

    /* Parse "value" field (required) */
    cJSON *value_item = cJSON_GetObjectItem(root, "value");
    if (cJSON_IsNumber(value_item)) {
        /* Numeric value: 0=disable, >0=enable with duration */
        int val = value_item->valueint;
        options->value = (val > 0);
        if (val > 0 && val <= 255) {
            options->time = (uint8_t)val;
        }
    } else if (cJSON_IsBool(value_item)) {
        options->value = cJSON_IsTrue(value_item);
    } else {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Missing or invalid 'value' field in permit join request");
        return ESP_FAIL;
    }

    /* Parse optional "time" field */
    cJSON *time_item = cJSON_GetObjectItem(root, "time");
    if (time_item != NULL && cJSON_IsNumber(time_item)) {
        int time_val = time_item->valueint;
        if (time_val >= 0 && time_val <= 255) {
            options->time = (uint8_t)time_val;
        }
    }

    /* Parse optional "device" field (IEEE address as hex string) */
    cJSON *device_item = cJSON_GetObjectItem(root, "device");
    if (device_item != NULL && cJSON_IsString(device_item)) {
        const char *device_str = cJSON_GetStringValue(device_item);
        if (device_str != NULL && strlen(device_str) >= (IEEE_ADDR_STRING_LEN - 1)) {
            /* Parse IEEE address from hex string (e.g., "0x00124b001234abcd") */
            const char *hex_start = device_str;
            if (device_str[0] == '0' && (device_str[1] == 'x' || device_str[1] == 'X')) {
                hex_start = device_str + 2;
            }

            /* Parse 16 hex characters (8 bytes) */
            if (strlen(hex_start) >= IEEE_ADDR_HEX_LEN) {
                uint8_t ieee[8];
                bool parse_ok = true;

                for (int i = 0; i < 8 && parse_ok; i++) {
                    char byte_str[3] = {hex_start[i * 2], hex_start[i * 2 + 1], '\0'};
                    char *endptr;
                    long val = strtol(byte_str, &endptr, 16);
                    if (*endptr != '\0' || val < 0 || val > 255) {
                        parse_ok = false;
                    } else {
                        /* IEEE address is stored in reverse byte order */
                        ieee[7 - i] = (uint8_t)val;
                    }
                }

                if (parse_ok) {
                    memcpy(options->device_ieee, ieee, 8);
                    options->has_device = true;
                    ESP_LOGI(TAG, "Permit join for device: %s", device_str);
                } else {
                    ESP_LOGW(TAG, "Invalid device IEEE address format: %s", device_str);
                }
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t json_parse_device_remove(const char *json_str, char *friendly_name, size_t buf_len)
{
    if (json_str == NULL || friendly_name == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *name_item = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsString(name_item)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *name = cJSON_GetStringValue(name_item);
    if (strlen(name) >= buf_len) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    strlcpy(friendly_name, name, buf_len);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t json_parse_device_rename(const char *json_str,
                                   char *old_name,
                                   char *new_name,
                                   size_t buf_len)
{
    if (json_str == NULL || old_name == NULL || new_name == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *from_item = cJSON_GetObjectItem(root, "from");
    cJSON *to_item = cJSON_GetObjectItem(root, "to");

    if (!cJSON_IsString(from_item) || !cJSON_IsString(to_item)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *from = cJSON_GetStringValue(from_item);
    const char *to = cJSON_GetStringValue(to_item);

    if (strlen(from) >= buf_len || strlen(to) >= buf_len) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    strlcpy(old_name, from, buf_len);
    strlcpy(new_name, to, buf_len);

    cJSON_Delete(root);
    return ESP_OK;
}

char* json_to_string_and_delete(cJSON *json)
{
    if (json == NULL) {
        return NULL;
    }

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return str;
}

const char* json_get_component_string(ha_component_t component)
{
    switch (component) {
        case HA_COMPONENT_LIGHT: return "light";
        case HA_COMPONENT_SENSOR: return "sensor";
        case HA_COMPONENT_BINARY_SENSOR: return "binary_sensor";
        case HA_COMPONENT_SWITCH: return "switch";
        case HA_COMPONENT_COVER: return "cover";
        case HA_COMPONENT_LOCK: return "lock";
        case HA_COMPONENT_CLIMATE: return "climate";
        case HA_COMPONENT_FAN: return "fan";
        default: return "unknown";
    }
}

esp_err_t json_format_ieee_addr(const uint8_t ieee_addr[8], char *str_buf, size_t buf_len)
{
    if (ieee_addr == NULL || str_buf == NULL || buf_len < IEEE_ADDR_BUFFER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(str_buf, buf_len, "0x%02X%02X%02X%02X%02X%02X%02X%02X",
             ieee_addr[7], ieee_addr[6], ieee_addr[5], ieee_addr[4],
             ieee_addr[3], ieee_addr[2], ieee_addr[1], ieee_addr[0]);

    return ESP_OK;
}

esp_err_t json_utils_test(void)
{
    ESP_LOGI(TAG, "Running JSON utilities test...");

    /* Test 1: Create and parse availability */
    char *avail = json_create_availability(true);
    if (avail == NULL) {
        ESP_LOGE(TAG, "Failed to create availability JSON");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Availability JSON: %s", avail);
    free(avail);

    /* Test 2: Create bridge state */
    char *state = json_create_bridge_state("online");
    if (state == NULL) {
        ESP_LOGE(TAG, "Failed to create bridge state JSON");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Bridge state JSON: %s", state);
    free(state);

    /* Test 3: Parse command */
    const char *cmd_json = "{\"state\":\"ON\",\"brightness\":200,\"transition\":5}";
    bool cmd_state = false;
    uint8_t cmd_brightness = 0;
    uint16_t cmd_transition = 0;

    esp_err_t ret = json_parse_command(cmd_json, &cmd_state, &cmd_brightness, NULL, NULL, &cmd_transition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse command JSON");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Parsed: state=%d, brightness=%d, transition=%d",
             cmd_state, cmd_brightness, cmd_transition);

    /* Test 4: IEEE address formatting */
    uint8_t test_ieee[8] = {0xCD, 0xAB, 0x34, 0x12, 0x00, 0x4B, 0x12, 0x00};
    char ieee_str[19];
    ret = json_format_ieee_addr(test_ieee, ieee_str, sizeof(ieee_str));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to format IEEE address");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "IEEE address: %s", ieee_str);

    ESP_LOGI(TAG, "JSON utilities test PASSED");
    return ESP_OK;
}

esp_err_t json_parse_lock_command(const char *json_str, bool *lock_cmd)
{
    if (json_str == NULL || lock_cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse lock command JSON");
        return ESP_FAIL;
    }

    /* Parse state - expect "LOCK" or "UNLOCK" (Zigbee2MQTT format) */
    cJSON *state_item = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(state_item)) {
        const char *state_str = cJSON_GetStringValue(state_item);
        if (strcmp(state_str, "LOCK") == 0) {
            *lock_cmd = true;
            cJSON_Delete(root);
            return ESP_OK;
        } else if (strcmp(state_str, "UNLOCK") == 0) {
            *lock_cmd = false;
            cJSON_Delete(root);
            return ESP_OK;
        }
    }

    cJSON_Delete(root);
    ESP_LOGW(TAG, "Invalid lock command state, expected 'LOCK' or 'UNLOCK'");
    return ESP_FAIL;
}

char* json_create_lock_state(bool locked)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    /* Zigbee2MQTT format: {"state": "LOCK"} or {"state": "UNLOCK"} */
    cJSON_AddStringToObject(root, "state", locked ? "LOCK" : "UNLOCK");

    return json_to_string_and_delete(root);
}
