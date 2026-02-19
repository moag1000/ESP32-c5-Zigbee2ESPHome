/**
 * @file zb_command_handler.c
 * @brief MQTT Command Handler Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_command_handler.h"
#include "events/event_bus.h"
#include "events/event_data.h"
#include "device/device_registry.h"
#include "zigbee/zb_device_handler_types.h"
#include "zigbee/zb_cluster_closures.h"
#include "zigbee/zb_cluster_security.h"
#include "zigbee/zb_coordinator.h"
#include "zigbee/zb_alarms.h"
#include "zigbee/zb_constants.h"
#include "zigbee/zb_tuya.h"
#include "zigbee/zb_interview.h"
#include "zigbee/zb_cmd_retry.h"
#include "zigbee/tuya/tuya_driver_registry.h"
#include "zigbee/tuya/tuya_device_driver.h"
#include "zigbee/converter/zb_converter.h"
#include "mqtt/mqtt_topics.h"
#include "utils/json_utils.h"
#include "memory/memory_manager_ng.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "core/gateway_timeouts.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "CMD_HANDLER";

/* Statistics */
static uint32_t s_commands_processed = 0;
static uint32_t s_command_errors = 0;

/**
 * @brief Convert IEEE address array to uint64_t
 * @param ieee_addr IEEE address as uint8_t[8]
 * @return uint64_t representation of the address
 */
static inline uint64_t ieee_to_u64(const esp_zb_ieee_addr_t ieee_addr)
{
    uint64_t result;
    memcpy(&result, ieee_addr, sizeof(uint64_t));
    return result;
}

/**
 * @brief Publish command received event
 *
 * @param ieee_addr Target device IEEE address (0 for bridge commands)
 * @param command Command name
 * @param payload_json JSON payload string
 * @param source Command source (CMD_SOURCE_MQTT, etc.)
 */
static void publish_command_received(uint64_t ieee_addr, const char *command,
                                     const char *payload_json, uint8_t source)
{
    evt_command_received_t evt = {
        .target_device = ieee_addr,
        .command = command,
        .payload_json = payload_json,
        .source = source,
    };
    event_publish(EVT_COMMAND_RECEIVED, &evt, sizeof(evt));
}

/**
 * @brief Publish command response event
 *
 * @param ieee_addr Device IEEE address
 * @param command Command that was executed
 * @param success True if command succeeded
 * @param error_msg Error message (NULL if success)
 */
static void publish_command_response(uint64_t ieee_addr, const char *command,
                                     bool success, const char *error_msg)
{
    evt_command_response_t evt = {
        .device = ieee_addr,
        .command = command,
        .success = success,
        .error_msg = error_msg,
    };
    event_publish(EVT_COMMAND_RESPONSE, &evt, sizeof(evt));
}

esp_err_t command_handler_init(void)
{
    s_commands_processed = 0;
    s_command_errors = 0;
    ESP_LOGI(TAG, "Command handler initialized");
    return ESP_OK;
}

esp_err_t command_handler_process(const char *topic, const char *payload, size_t len)
{
    if (topic == NULL || payload == NULL || len == 0) {
        s_command_errors++;
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Processing command: %s", topic);
    ESP_LOGD(TAG, "Payload: %.*s", (int)len, payload);

    /* Extract friendly name from topic */
    char friendly_name[ZB_DEVICE_FRIENDLY_NAME_LEN];
    esp_err_t ret = mqtt_topic_extract_friendly_name(topic, friendly_name, sizeof(friendly_name));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to extract friendly name from topic");
        s_command_errors++;
        publish_command_response(0, "set", false, "Failed to extract device name from topic");
        return ret;
    }

    ESP_LOGD(TAG, "Friendly name: %s", friendly_name);

    /* Find device by friendly name (avoids stack allocation) */
    device_t *target_device = device_registry_get_by_name(friendly_name);
    if (target_device == NULL) {
        ESP_LOGW(TAG, "Device not found: %s", friendly_name);
        s_command_errors++;
        publish_command_response(0, "set", false, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }

    /* Check if device address is still pending (loaded from NVS but hasn't communicated yet) */
    if (target_device->proto.zigbee.short_addr == ZB_SHORT_ADDR_PENDING) {
        ESP_LOGW(TAG, "Device %s address pending - waiting for device to communicate", friendly_name);
        s_command_errors++;
        publish_command_response(target_device->id, "set", false, "Device address pending");
        return ESP_ERR_INVALID_STATE;
    }

    /* EP0 is the ZDO management endpoint - not valid for application commands.
     * Default to EP1 (standard application endpoint for most devices including Tuya). */
    uint8_t target_endpoint = target_device->proto.zigbee.endpoint;
    if (target_endpoint == 0) {
        target_endpoint = 1;
        ESP_LOGW(TAG, "Device %s has EP0, defaulting to EP1", friendly_name);
    }

    /* Publish command received event - allows other handlers to react */
    publish_command_received(target_device->id, "set", payload, CMD_SOURCE_MQTT);

    /* Null-terminate payload for JSON parsing */
    char *payload_str = (char *)mem_alloc(len + 1, MEM_CAP_DEFAULT);
    if (payload_str == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for payload");
        s_command_errors++;
        publish_command_response(target_device->id, "set", false, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    memcpy(payload_str, payload, len);
    payload_str[len] = '\0';

    /* Try converter-based command handling first */
    {
        cJSON *cmd_json = cJSON_Parse(payload_str);
        if (cmd_json != NULL) {
            esp_err_t conv_ret = zb_converter_handle_command(
                target_device->proto.zigbee.short_addr, target_endpoint, cmd_json);
            cJSON_Delete(cmd_json);
            if (conv_ret == ESP_OK) {
                s_commands_processed++;
                mem_ng_free(payload_str);
                publish_command_response(target_device->id, "set", true, NULL);
                return ESP_OK;
            }
            /* Converter found but command failed (e.g. device asleep) —
             * don't fall through to "no converter" message */
            if (conv_ret != ESP_ERR_NOT_FOUND) {
                s_command_errors++;
                mem_ng_free(payload_str);
                ESP_LOGW(TAG, "Converter command failed for %s: %s",
                         friendly_name, esp_err_to_name(conv_ret));
                publish_command_response(target_device->id, "set", false,
                                         "Command send failed (device may be asleep)");
                return conv_ret;
            }
        }
    }

    /* Check for alarm reset commands: {"alarm_reset": code} or {"alarm_reset_all": true} */
    if (strstr(payload, "\"alarm_reset\"") != NULL || strstr(payload, "\"alarm_reset_all\"") != NULL) {
        cJSON *json = cJSON_Parse(payload_str);
        if (json != NULL) {
            /* Check for alarm_reset_all first */
            cJSON *reset_all = cJSON_GetObjectItem(json, "alarm_reset_all");
            if (cJSON_IsTrue(reset_all)) {
                cJSON_Delete(json);
                mem_ng_free(payload_str);
                ret = command_send_alarm_reset_all(target_device->proto.zigbee.short_addr, target_endpoint);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Reset all alarms for %s", friendly_name);
                    s_commands_processed++;
                } else {
                    s_command_errors++;
                }
                return ret;
            }

            /* Check for alarm_reset with code and cluster */
            cJSON *reset_obj = cJSON_GetObjectItem(json, "alarm_reset");
            if (cJSON_IsObject(reset_obj)) {
                cJSON *code_json = cJSON_GetObjectItem(reset_obj, "code");
                cJSON *cluster_json = cJSON_GetObjectItem(reset_obj, "cluster");
                if (cJSON_IsNumber(code_json) && cJSON_IsNumber(cluster_json)) {
                    uint8_t alarm_code = (uint8_t)cJSON_GetNumberValue(code_json);
                    uint16_t cluster_id = (uint16_t)cJSON_GetNumberValue(cluster_json);
                    cJSON_Delete(json);
                    mem_ng_free(payload_str);
                    ret = command_send_alarm_reset(target_device->proto.zigbee.short_addr, target_endpoint,
                                                   alarm_code, cluster_id);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "Reset alarm %d (cluster 0x%04X) for %s",
                                 alarm_code, cluster_id, friendly_name);
                        s_commands_processed++;
                    } else {
                        s_command_errors++;
                    }
                    return ret;
                }
            } else if (cJSON_IsNumber(reset_obj)) {
                /* Simple format: {"alarm_reset": code} - reset all alarms with this code */
                uint8_t alarm_code = (uint8_t)cJSON_GetNumberValue(reset_obj);
                cJSON_Delete(json);
                mem_ng_free(payload_str);
                /* For simple format, reset from all clusters (use 0xFFFF) */
                ret = zb_alarms_remove_from_table(target_device->proto.zigbee.short_addr, alarm_code, 0xFFFF);
                ESP_LOGI(TAG, "Removed alarm code %d from table for %s", alarm_code, friendly_name);
                s_commands_processed++;
                return ret;
            }
            cJSON_Delete(json);
        }
    }

    /* Handle Tuya device-specific commands via driver registry.
     * Don't gate on zb_device_is_tuya() — clusters aren't persisted in NVS,
     * so after reboot the cluster check fails. Instead, try the driver
     * directly; tuya_driver_find() returns NULL for non-Tuya devices. */
    {
        bool tuya_cmd_handled = false;
        const tuya_device_driver_t *drv = tuya_driver_get(target_device->proto.zigbee.short_addr);

        /* On-demand binding: after gateway reboot the driver isn't bound yet
         * because no DPs or Basic Cluster reports have arrived. Try to bind now. */
        if (drv == NULL && target_device->manufacturer[0] != '\0') {
            drv = tuya_driver_find(target_device->manufacturer, target_device->model);
            if (drv != NULL) {
                tuya_driver_bind(target_device->proto.zigbee.short_addr, drv);
                if (drv->init_device) {
                    drv->init_device(target_device->proto.zigbee.short_addr);
                }
                ESP_LOGI(TAG, "On-demand Tuya driver bind: 0x%04X -> '%s'",
                         target_device->proto.zigbee.short_addr, drv->name);
            }
        }

        if (drv != NULL && drv->handle_command != NULL) {
            cJSON *json = cJSON_Parse(payload_str);
            if (json != NULL) {
                ret = drv->handle_command(target_device->proto.zigbee.short_addr,
                                          target_endpoint, json);
                if (ret == ESP_OK) {
                    tuya_cmd_handled = true;
                    s_commands_processed++;
                    ESP_LOGI(TAG, "Tuya driver '%s' handled command for %s",
                             drv->name, friendly_name);
                }
                cJSON_Delete(json);
            }
        }

        /* If the Tuya driver handled the command, we're done.
         * The driver is responsible for ALL commands for its device type,
         * including state (ZCL On/Off + Tuya DP). No ZCL fallback needed. */
        if (tuya_cmd_handled) {
            mem_ng_free(payload_str);
            publish_command_response(target_device->id, "set", true, NULL);
            return ESP_OK;
        }
    }

    /* No converter or Tuya driver handled this command.
     * The device has no known converter and no capabilities set.
     * Log for debugging and return error. */
    mem_ng_free(payload_str);
    ESP_LOGW(TAG, "No converter for %s (0x%04X) — command not handled",
             friendly_name, target_device->proto.zigbee.short_addr);
    s_command_errors++;
    publish_command_response(target_device->id, "set", false, "No converter bound");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t command_send_on_off(uint16_t short_addr, uint8_t endpoint, bool on)
{
    ESP_LOGD(TAG, "Sending ON/OFF to 0x%04X EP%d: %s", short_addr, endpoint, on ? "ON" : "OFF");

    esp_zb_zcl_on_off_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = {
                .addr_short = short_addr,
            },
            .dst_endpoint = endpoint,
            .src_endpoint = 1, /* Coordinator endpoint */
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID,
    };

    /* Thread-safety: Acquire Zigbee lock before API call */
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_on_off_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "On/off command queued for 0x%04X (TSN=%d)", short_addr, tsn);

    /* Update TX statistics */
    zb_coordinator_update_tx_count();

    /* Store for retry on device unavailable / timeout */
    cmd_retry_store_on_off(short_addr, endpoint, on);

    return ESP_OK;
}

esp_err_t command_send_level(uint16_t short_addr, uint8_t endpoint,
                             uint8_t level, uint16_t transition_time)
{
    ESP_LOGD(TAG, "Sending LEVEL to 0x%04X EP%d: %d (trans=%d)",
             short_addr, endpoint, level, transition_time);

    esp_zb_zcl_move_to_level_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = {
                .addr_short = short_addr,
            },
            .dst_endpoint = endpoint,
            .src_endpoint = 1, /* Coordinator endpoint */
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .level = level,
        .transition_time = transition_time,
    };

    /* Thread-safety: Acquire Zigbee lock before API call */
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_level_move_to_level_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Level command queued for 0x%04X (TSN=%d)", short_addr, tsn);

    /* Update TX statistics */
    zb_coordinator_update_tx_count();

    /* Store for retry on device unavailable / timeout */
    cmd_retry_store_level(short_addr, endpoint, level, transition_time);

    return ESP_OK;
}

esp_err_t command_send_color(uint16_t short_addr, uint8_t endpoint,
                             uint16_t x, uint16_t y, uint16_t transition_time)
{
    ESP_LOGD(TAG, "Sending COLOR to 0x%04X EP%d: x=%d, y=%d (trans=%d)",
             short_addr, endpoint, x, y, transition_time);

    esp_zb_zcl_color_move_to_color_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = {
                .addr_short = short_addr,
            },
            .dst_endpoint = endpoint,
            .src_endpoint = 1, /* Coordinator endpoint */
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .color_x = x,
        .color_y = y,
        .transition_time = transition_time,
    };

    /* Thread-safety: Acquire Zigbee lock before API call */
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_color_move_to_color_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Color command queued for 0x%04X (TSN=%d)", short_addr, tsn);

    /* Update TX statistics */
    zb_coordinator_update_tx_count();

    /* Store for retry on device unavailable / timeout */
    cmd_retry_store_color(short_addr, endpoint, x, y, transition_time);

    return ESP_OK;
}

esp_err_t command_send_color_temp(uint16_t short_addr, uint8_t endpoint,
                                   uint16_t color_temp_mireds, uint16_t transition_time)
{
    ESP_LOGD(TAG, "Sending COLOR_TEMP to 0x%04X EP%d: %d mireds (trans=%d)",
             short_addr, endpoint, color_temp_mireds, transition_time);

    esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = {
                .addr_short = short_addr,
            },
            .dst_endpoint = endpoint,
            .src_endpoint = 1, /* Coordinator endpoint */
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .color_temperature = color_temp_mireds,
        .transition_time = transition_time,
    };

    /* Thread-safety: Acquire Zigbee lock before API call */
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Color temp command queued for 0x%04X (TSN=%d)", short_addr, tsn);

    /* Update TX statistics */
    zb_coordinator_update_tx_count();

    return ESP_OK;
}

esp_err_t command_send_toggle(uint16_t short_addr, uint8_t endpoint)
{
    ESP_LOGD(TAG, "Sending TOGGLE to 0x%04X EP%d", short_addr, endpoint);

    esp_zb_zcl_on_off_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = {
                .addr_short = short_addr,
            },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID,
    };

    /* Thread-safety: Acquire Zigbee lock before API call */
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_on_off_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Toggle command queued for 0x%04X (TSN=%d)", short_addr, tsn);

    /* Update TX statistics */
    zb_coordinator_update_tx_count();

    return ESP_OK;
}

esp_err_t command_send_lock(uint16_t short_addr, uint8_t endpoint)
{
    ESP_LOGI(TAG, "Sending LOCK command to 0x%04X EP%d", short_addr, endpoint);

    /* ZCL Door Lock cluster command */
    esp_err_t ret = zb_door_lock_cmd_lock(short_addr, endpoint);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send lock command: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t command_send_unlock(uint16_t short_addr, uint8_t endpoint)
{
    ESP_LOGI(TAG, "Sending UNLOCK command to 0x%04X EP%d", short_addr, endpoint);

    /* ZCL Door Lock cluster command */
    esp_err_t ret = zb_door_lock_cmd_unlock(short_addr, endpoint);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send unlock command: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t command_send_alarm_reset(uint16_t short_addr, uint8_t endpoint,
                                    uint8_t alarm_code, uint16_t cluster_id)
{
    ESP_LOGI(TAG, "Sending RESET ALARM to 0x%04X EP%d: code=%d cluster=0x%04X",
             short_addr, endpoint, alarm_code, cluster_id);

    /* Use the Alarms API from zb_alarms */
    esp_err_t ret = zb_alarms_cmd_reset_alarm(short_addr, endpoint, alarm_code, cluster_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send reset alarm command: %s", esp_err_to_name(ret));
    }

    /* Update TX statistics */
    if (ret == ESP_OK) {
        zb_coordinator_update_tx_count();
    }

    return ret;
}

esp_err_t command_send_alarm_reset_all(uint16_t short_addr, uint8_t endpoint)
{
    ESP_LOGI(TAG, "Sending RESET ALL ALARMS to 0x%04X EP%d", short_addr, endpoint);

    /* Use the Alarms API from zb_alarms */
    esp_err_t ret = zb_alarms_cmd_reset_all(short_addr, endpoint);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send reset all alarms command: %s", esp_err_to_name(ret));
    }

    /* Update TX statistics */
    if (ret == ESP_OK) {
        zb_coordinator_update_tx_count();
    }

    return ret;
}

esp_err_t command_handler_get_stats(uint32_t *processed, uint32_t *errors)
{
    if (processed != NULL) {
        *processed = s_commands_processed;
    }
    if (errors != NULL) {
        *errors = s_command_errors;
    }
    return ESP_OK;
}

esp_err_t command_handler_test(void)
{
    ESP_LOGI(TAG, "Running command handler test...");

    /* Test command parsing */
    const char *test_topic = "zigbee2mqtt/test_light/set";
    const char *test_payload = "{\"state\":\"ON\",\"brightness\":254}";

    /* Extract friendly name */
    char friendly_name[64];
    esp_err_t ret = mqtt_topic_extract_friendly_name(test_topic, friendly_name, sizeof(friendly_name));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to extract friendly name");
        return ESP_FAIL;
    }

    if (strcmp(friendly_name, "test_light") != 0) {
        ESP_LOGE(TAG, "Friendly name mismatch: expected 'test_light', got '%s'", friendly_name);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Extracted friendly name: %s", friendly_name);

    /* Parse command */
    bool state = false;
    uint8_t brightness = 0;
    ret = json_parse_command(test_payload, &state, &brightness, NULL, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse command");
        return ESP_FAIL;
    }

    if (!state || brightness != ZCL_BRIGHTNESS_MAX) {
        ESP_LOGE(TAG, "Command parsing failed: state=%d, brightness=%d", state, brightness);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Command handler test PASSED");
    return ESP_OK;
}
