/**
 * @file zb_callbacks.c
 * @brief Zigbee Event Callbacks Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_callbacks.h"
#include "zb_constants.h"
#include "esp_timer.h"
#include "zb_coordinator.h"
#include "converter/zb_converter_std.h"
#include "zb_device_handler_types.h"
#include "zb_cluster_multistate.h"
#include "zb_cluster_security.h"
#include "zb_cluster_electrical.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "core/device/device_persistence.h"
#include "zb_network.h"
#include "zb_availability.h"
#include "zb_interview.h"
#include "zb_install_codes.h"
#include "zb_reporting.h"
#include "zb_alarms.h"
#include "zb_demand_response.h"
#include "zb_hvac_dehumid.h"
#include "zb_poll_control.h"
#include "zb_tuya.h"
#include "zb_cmd_retry.h"
#include "zb_rejoin_helper.h"
#include "zb_leave_helper.h"
#include "tuya/tuya_driver_registry.h"
#include "tuya/tuya_device_driver.h"
#include "converter/zb_converter.h"
#include "mqtt_bridge.h"
#include "mqtt_bridge_internal.h"
#include "device_state_publisher.h"
#include "bridge_events.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/monitoring/perf_metrics.h"
#include "esp_log.h"
#include "aps/esp_zigbee_aps.h"  /* API-005: APS Authentication State */
#include "mac/esp_zigbee_mac.h"  /* MAC indirect transaction persistence time */
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "sdkconfig.h"
#include "lifecycle_manager.h"
#include "state_persistence.h"

/* IEEE 802.15.4 coexistence for TCLK boost */
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_ieee802154.h"
#include "esp_coex_i154.h"
#endif

/* LED status integration */
#if CONFIG_GW_LED_ENABLED
#include "led/led_controller.h"
#include "core/led_status_manager.h"
#endif

static const char *TAG = "ZB_CB";

/**
 * @brief Copy a ZCL character string attribute into a fixed buffer
 *
 * ZCL character strings are [length][bytes], and the length byte comes off the
 * air from the reporting device. Clamping it against the destination alone is
 * not enough: a frame carrying a short value with a large length byte would
 * make memcpy read past the end of the attribute. The three Basic-cluster
 * string attributes did exactly that, while the numeric cases a few lines below
 * were already checking data->size.
 *
 * Clamps against both the attribute's own size and the destination, and always
 * null-terminates.
 *
 * @return true if something was copied, false if the attribute was unusable
 */
static bool zcl_copy_char_string(const esp_zb_zcl_attribute_data_t *data,
                                 char *dst, size_t dst_size)
{
    if (data == NULL || dst == NULL || dst_size < 2) {
        return false;
    }
    if (data->type != ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING || data->value == NULL) {
        return false;
    }
    /* Need at least the length byte itself. */
    if (data->size < 2) {
        return false;
    }

    const uint8_t *raw = (const uint8_t *)data->value;
    size_t len = raw[0];

    const size_t available = (size_t)data->size - 1;  /* bytes after the length */
    if (len > available) {
        len = available;
    }
    if (len > dst_size - 1) {
        len = dst_size - 1;
    }
    if (len == 0) {
        return false;
    }

    memcpy(dst, raw + 1, len);
    dst[len] = '\0';
    return true;
}

/* Forward declarations for internal helpers */
static esp_err_t zb_coordinator_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                                const void *message);
static const char* zdo_status_to_str(esp_zb_zdp_status_t status);
static void handle_basic_cluster_report(uint16_t short_addr, esp_zb_zcl_report_attr_message_t *msg);
static void handle_onoff_cluster_report(uint16_t short_addr, esp_zb_zcl_report_attr_message_t *msg);

/**
 * @brief Apply install code to Zigbee stack if entry exists
 *
 * Looks up install code for the device and applies it to the stack
 * for secure joining.
 *
 * @param ieee64 Device IEEE address as uint64_t
 * @param short_addr Device short address (for logging)
 * @return ESP_OK if applied, ESP_ERR_NOT_FOUND if no entry, other errors on failure
 */
static esp_err_t apply_install_code_if_present(uint64_t ieee64, uint16_t short_addr)
{
    if (!zb_install_codes_has_entry(ieee64)) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Device 0x%04X has install code, applying derived key", short_addr);
    esp_err_t ret = zb_install_codes_apply_to_stack(ieee64);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Install code applied successfully for 0x%016" PRIX64, ieee64);
    } else {
        ESP_LOGW(TAG, "Failed to apply install code: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief Coordinator Action Handler for ZCL commands (ESP-Zigbee-SDK v1.6.x+)
 *
 * This is the main callback for handling ZCL commands and responses.
 * Required for proper coordinator operation with the new SDK API.
 *
 * @param callback_id Action callback type ID
 * @param message Callback-specific message data
 * @return ESP_OK on success
 */
static esp_err_t zb_coordinator_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                                const void *message)
{
    switch (callback_id) {
        case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
            esp_zb_zcl_set_attr_value_message_t *msg =
                (esp_zb_zcl_set_attr_value_message_t *)message;
            if (msg != NULL) {
                ESP_LOGI(TAG, "Set attr: ep=%d cluster=0x%04x attr=0x%04x",
                         msg->info.dst_endpoint, msg->info.cluster, msg->attribute.id);

                /* Update coordinator statistics */
                zb_coordinator_update_rx_count();
            }
            return ESP_OK;
        }

        case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
            esp_zb_zcl_report_attr_message_t *msg =
                (esp_zb_zcl_report_attr_message_t *)message;
            if (msg != NULL) {
                cmd_retry_confirm(msg->src_address.u.short_addr);
                zb_callback_report_attr(msg);
                zb_coordinator_update_rx_count();
            }
            return ESP_OK;
        }

        case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID: {
            /* Handle read attributes response - forward to interview module for Basic cluster */
            esp_zb_zcl_cmd_read_attr_resp_message_t *msg =
                (esp_zb_zcl_cmd_read_attr_resp_message_t *)message;
            if (msg != NULL) {
                uint16_t short_addr = msg->info.src_address.u.short_addr;
                uint16_t cluster_id = msg->info.cluster;

                cmd_retry_confirm(short_addr);

                ESP_LOGD(TAG, "ZCL Read Attributes Response from 0x%04X cluster=0x%04X",
                         short_addr, cluster_id);

                /* Process each variable in the response list */
                esp_zb_zcl_read_attr_resp_variable_t *var = msg->variables;
                while (var != NULL) {
                    ESP_LOGD(TAG, "  attr=0x%04X status=0x%02X type=0x%02X",
                             var->attribute.id, var->status, var->attribute.data.type);

                    /* Forward to interview module for Basic cluster attributes */
                    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_BASIC) {
                        zb_interview_handle_read_attr_resp(short_addr, cluster_id,
                                                           var->attribute.id,
                                                           var->status,
                                                           var->attribute.data.type,
                                                           var->attribute.data.value,
                                                           var->attribute.data.size);
                    }

                    /* Also handle manufacturer/model reports for non-interview scenarios */
                    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
                        var->status == ESP_ZB_ZCL_STATUS_SUCCESS &&
                        var->attribute.data.value != NULL) {
                        esp_zb_zcl_report_attr_message_t report = {
                            .src_address.u.short_addr = short_addr,
                            .cluster = cluster_id,
                            .attribute.id = var->attribute.id,
                            .attribute.data = var->attribute.data
                        };
                        handle_basic_cluster_report(short_addr, &report);
                    }

                    var = var->next;
                }
            }
            zb_coordinator_update_rx_count();
            return ESP_OK;
        }

        case ESP_ZB_CORE_CMD_WRITE_ATTR_RESP_CB_ID: {
            ESP_LOGD(TAG, "ZCL Write Attributes Response received");
            zb_coordinator_update_rx_count();
            return ESP_OK;
        }

        case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID: {
            esp_zb_zcl_cmd_default_resp_message_t *msg =
                (esp_zb_zcl_cmd_default_resp_message_t *)message;
            if (msg != NULL) {
                cmd_retry_confirm(msg->info.src_address.u.short_addr);
                if (msg->status_code != 0) {
                    ESP_LOGW(TAG, "ZCL Default Response: FAILED status=0x%02x cmd=0x%02x from 0x%04X",
                             msg->status_code, msg->resp_to_cmd,
                             msg->info.src_address.u.short_addr);
                } else {
                    ESP_LOGI(TAG, "ZCL Default Response: OK cmd=0x%02x from 0x%04X",
                             msg->resp_to_cmd, msg->info.src_address.u.short_addr);
                }
            }
            return ESP_OK;
        }

        case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID: {
            /* Handle custom cluster commands including Alarms cluster (0x0009) */
            esp_zb_zcl_custom_cluster_command_message_t *msg =
                (esp_zb_zcl_custom_cluster_command_message_t *)message;
            if (msg != NULL) {
                uint16_t short_addr = msg->info.src_address.u.short_addr;
                uint8_t endpoint = msg->info.src_endpoint;
                uint16_t cluster_id = msg->info.cluster;
                uint8_t cmd_id = msg->info.command.id;

                ESP_LOGD(TAG, "Custom cluster cmd from 0x%04X ep=%d cluster=0x%04X cmd=0x%02X",
                         short_addr, endpoint, cluster_id, cmd_id);

                /* Handle Alarms cluster (0x0009) commands */
                if (cluster_id == ZB_ZCL_CLUSTER_ID_ALARMS) {
                    if (cmd_id == ZB_ZCL_CMD_ALARMS_ALARM_ID) {
                        /* Alarm notification: parse alarm_code (uint8) + cluster_id (uint16) */
                        if (msg->data.size >= 3) {
                            uint8_t alarm_code = ((uint8_t *)msg->data.value)[0];
                            uint16_t alarm_cluster = ((uint8_t *)msg->data.value)[1] |
                                                     (((uint8_t *)msg->data.value)[2] << 8);
                            ESP_LOGI(TAG, "Alarm notification: device=0x%04X code=%d cluster=0x%04X",
                                     short_addr, alarm_code, alarm_cluster);
                            zb_alarms_handle_notification(short_addr, endpoint, alarm_code, alarm_cluster);
                        }
                    } else if (cmd_id == ZB_ZCL_CMD_ALARMS_GET_ALARM_RESPONSE_ID) {
                        /* Get Alarm Response: parse status, alarm_code, cluster_id, timestamp */
                        if (msg->data.size >= 1) {
                            uint8_t status = ((uint8_t *)msg->data.value)[0];
                            uint8_t alarm_code = 0;
                            uint16_t alarm_cluster = 0;
                            uint32_t timestamp = 0;

                            if (status == 0x00 && msg->data.size >= 8) {
                                alarm_code = ((uint8_t *)msg->data.value)[1];
                                alarm_cluster = ((uint8_t *)msg->data.value)[2] |
                                                (((uint8_t *)msg->data.value)[3] << 8);
                                timestamp = ((uint8_t *)msg->data.value)[4] |
                                            (((uint8_t *)msg->data.value)[5] << 8) |
                                            (((uint8_t *)msg->data.value)[6] << 16) |
                                            (((uint8_t *)msg->data.value)[7] << 24);
                            }
                            zb_alarms_handle_get_response(short_addr, endpoint, status,
                                                          alarm_code, alarm_cluster, timestamp);
                        }
                    }
                }

                /* Handle Demand Response and Load Control cluster (0x0701) commands */
                if (cluster_id == ZB_ZCL_CLUSTER_ID_DEMAND_RESPONSE) {
                    if (cmd_id == ZB_ZCL_CMD_DRLC_LOAD_CONTROL_EVENT_ID) {
                        /* LoadControlEvent: parse full event structure */
                        if (msg->data.size >= ZB_EVENT_MESSAGE_MIN_SIZE) {  /* Minimum event size */
                            uint8_t *data = (uint8_t *)msg->data.value;
                            zb_load_control_event_t event = {0};

                            /* Parse event fields (little-endian) */
                            event.issuer_event_id = data[0] | (data[1] << 8) |
                                                    (data[2] << 16) | (data[3] << 24);
                            event.device_class = data[4] | (data[5] << 8);
                            event.utility_enrollment_group = data[6];
                            event.start_time = data[7] | (data[8] << 8) |
                                              (data[9] << 16) | (data[10] << 24);
                            event.duration_minutes = data[11] | (data[12] << 8);
                            event.criticality_level = data[13];
                            event.cooling_temp_offset = data[14];
                            event.heating_temp_offset = data[15];
                            event.cooling_temp_setpoint = (int16_t)(data[16] | (data[17] << 8));
                            event.heating_temp_setpoint = (int16_t)(data[18] | (data[19] << 8));
                            event.average_load_adjustment = (int8_t)data[20];
                            event.duty_cycle = data[21];
                            event.event_control = data[22];

                            ESP_LOGI(TAG, "DRLC LoadControlEvent: id=0x%08" PRIX32 " class=0x%04X",
                                     event.issuer_event_id, event.device_class);
                            zb_demand_response_handle_event(&event);
                        }
                    } else if (cmd_id == ZB_ZCL_CMD_DRLC_CANCEL_LOAD_CONTROL_EVENT_ID) {
                        /* CancelLoadControlEvent */
                        if (msg->data.size >= 8) {
                            uint8_t *data = (uint8_t *)msg->data.value;
                            uint32_t issuer_event_id = data[0] | (data[1] << 8) |
                                                        (data[2] << 16) | (data[3] << 24);
                            uint16_t device_class = data[4] | (data[5] << 8);
                            uint8_t enrollment_group = data[6];
                            uint8_t cancel_control = data[7];

                            ESP_LOGI(TAG, "DRLC CancelLoadControlEvent: id=0x%08" PRIX32,
                                     issuer_event_id);
                            zb_demand_response_handle_cancel_event(issuer_event_id, device_class,
                                                                    enrollment_group, cancel_control);
                        }
                    } else if (cmd_id == ZB_ZCL_CMD_DRLC_CANCEL_ALL_LOAD_CONTROL_ID) {
                        /* CancelAllLoadControlEvents */
                        uint8_t cancel_control = 0;
                        if (msg->data.size >= 1) {
                            cancel_control = ((uint8_t *)msg->data.value)[0];
                        }
                        ESP_LOGI(TAG, "DRLC CancelAllLoadControlEvents");
                        zb_demand_response_handle_cancel_all(cancel_control);
                    }
                }

                /* Handle Poll Control cluster (0x0020) commands */
                if (cluster_id == ZB_ZCL_CLUSTER_ID_POLL_CONTROL) {
                    if (cmd_id == ZB_POLL_CMD_CHECK_IN) {
                        /* Check-in from sleepy device */
                        ESP_LOGI(TAG, "Poll Control Check-in from 0x%04X EP%d",
                                 short_addr, endpoint);
                        zb_poll_control_handle_check_in(short_addr, endpoint);
                    }
                }

                /* Handle Tuya private cluster (0xEF00) - Datapoint protocol */
                if (cluster_id == ZB_TUYA_CLUSTER_ID) {
                    ESP_LOGI(TAG, "Tuya cmd 0x%02X from 0x%04X EP%d len=%zu",
                             cmd_id, short_addr, endpoint, msg->data.size);
                    cmd_retry_confirm(short_addr);
                    zb_tuya_handle_command(short_addr, endpoint, cmd_id,
                                           (const uint8_t *)msg->data.value,
                                           msg->data.size);
                }

                zb_coordinator_update_rx_count();
            }
            return ESP_OK;
        }

        /* BC-002: ESP-Zigbee-SDK v1.6.x Reporting Response Callbacks */
        case ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID: {
            /* Configure Reporting Response (v1.6.x struct: esp_zb_zcl_cmd_config_report_resp_message_t) */
            esp_zb_zcl_cmd_config_report_resp_message_t *msg =
                (esp_zb_zcl_cmd_config_report_resp_message_t *)message;
            if (msg != NULL) {
                uint16_t short_addr = msg->info.src_address.u.short_addr;
                uint8_t endpoint = msg->info.src_endpoint;
                uint16_t cluster_id = msg->info.cluster;

                ESP_LOGI(TAG, "Configure Reporting Response from 0x%04X ep=%d cluster=0x%04X",
                         short_addr, endpoint, cluster_id);

                /* Process each variable in the response list */
                esp_zb_zcl_config_report_resp_variable_t *var = msg->variables;
                while (var != NULL) {
                    ESP_LOGD(TAG, "  attr=0x%04X status=0x%02X direction=%d",
                             var->attribute_id, var->status, var->direction);

                    /* Forward to reporting module */
                    zb_reporting_handle_configure_response(short_addr, endpoint,
                                                            cluster_id, var->status,
                                                            var->attribute_id);
                    var = var->next;
                }
                zb_coordinator_update_rx_count();
            }
            return ESP_OK;
        }

        case ESP_ZB_CORE_CMD_READ_REPORT_CFG_RESP_CB_ID: {
            /* Read Reporting Configuration Response (v1.6.x struct: esp_zb_zcl_cmd_read_report_config_resp_message_t) */
            esp_zb_zcl_cmd_read_report_config_resp_message_t *msg =
                (esp_zb_zcl_cmd_read_report_config_resp_message_t *)message;
            if (msg != NULL) {
                uint16_t short_addr = msg->info.src_address.u.short_addr;
                uint8_t endpoint = msg->info.src_endpoint;
                uint16_t cluster_id = msg->info.cluster;

                ESP_LOGI(TAG, "Read Reporting Config Response from 0x%04X ep=%d cluster=0x%04X",
                         short_addr, endpoint, cluster_id);

                /* Process each variable in the response list */
                esp_zb_zcl_read_report_config_resp_variable_t *var = msg->variables;
                while (var != NULL) {
                    if (var->status == ESP_ZB_ZCL_STATUS_SUCCESS &&
                        var->report_direction == ZCL_REPORTING_DIRECTION_REPORTED) {
                        /* Extract reportable_change based on attribute type size */
                        uint16_t reportable_change = 0;
                        if (var->client.delta[0] != 0) {
                            /* Simple extraction for common types (1-2 bytes) */
                            reportable_change = var->client.delta[0];
                        }

                        ESP_LOGD(TAG, "  attr=0x%04X type=0x%02X min=%d max=%d change=%d",
                                 var->attribute_id, var->client.attr_type,
                                 var->client.min_interval, var->client.max_interval,
                                 reportable_change);

                        /* Forward to reporting module */
                        zb_reporting_handle_read_response(short_addr, endpoint,
                                                           cluster_id, var->status,
                                                           var->report_direction,
                                                           var->attribute_id,
                                                           var->client.attr_type,
                                                           var->client.min_interval,
                                                           var->client.max_interval,
                                                           reportable_change);
                    } else {
                        ESP_LOGD(TAG, "  attr=0x%04X status=0x%02X (failed or server direction)",
                                 var->attribute_id, var->status);

                        /* Forward failure to reporting module */
                        zb_reporting_handle_read_response(short_addr, endpoint,
                                                           cluster_id, var->status,
                                                           var->report_direction,
                                                           var->attribute_id,
                                                           0, 0, 0, 0);
                    }
                    var = var->next;
                }
                zb_coordinator_update_rx_count();
            }
            return ESP_OK;
        }

        default:
            ESP_LOGD(TAG, "Unhandled action callback: 0x%04x", callback_id);
            return ESP_OK;
    }
}


esp_err_t zb_callbacks_init(void)
{
    /* Register the coordinator action handler for ZCL command processing */
    esp_zb_core_action_handler_register(zb_coordinator_action_handler);

    ESP_LOGI(TAG, "Callback handlers initialized (action handler registered)");
    return ESP_OK;
}

void zb_callback_device_join(esp_zb_zdp_status_t zdo_status, uint16_t short_addr,
                              esp_zb_ieee_addr_t ieee_addr)
{
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Device join failed: addr=0x%04X, status=%s",
                 short_addr, zdo_status_to_str(zdo_status));
        return;
    }

    ESP_LOGI(TAG, "Device joined: 0x%04X [%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X]",
             short_addr,
             ieee_addr[7], ieee_addr[6], ieee_addr[5], ieee_addr[4],
             ieee_addr[3], ieee_addr[2], ieee_addr[1], ieee_addr[0]);

    /* API-005: APS Authentication State Check
     *
     * Note: esp_zb_aps_is_authenticated() in ESP-Zigbee-SDK v1.6.x only checks
     * the local device's (coordinator's) authentication state, not per-device
     * authentication. For per-device auth state, a custom implementation using
     * the APS key exchange mechanism would be needed.
     *
     * The esp_zb_aps_set_authenticated(bool) / esp_zb_aps_is_authenticated()
     * APIs are for the local device's APS layer state management.
     */
    bool coordinator_authenticated = esp_zb_aps_is_authenticated();
    ESP_LOGI(TAG, "Device 0x%04X joined, coordinator APS auth state: %s",
             short_addr, coordinator_authenticated ? "authenticated" : "not authenticated");

    /* Convert IEEE address to 64-bit for install code lookup and bridge events */
    uint64_t ieee64 = zb_ieee_to_u64(ieee_addr);

    /* Check if we have an install code for this device */
    apply_install_code_if_present(ieee64, short_addr);

    /* Add device to NG registry.
     * Note: device_registry_add() returns the existing device if already present
     * (non-NULL), so check if device existed BEFORE the add to distinguish
     * new joins from rejoins/re-authorization. */
    device_t *existing = device_registry_get(ieee64);
    device_t *dev = existing ? existing : device_registry_add(ieee64, DEV_PROTOCOL_ZIGBEE);
    if (dev == NULL) {
        ESP_LOGE(TAG, "Failed to add device to registry");
        return;
    }
    if (existing != NULL) {
        /* Rejoin: atomically update short_addr + availability + converter + tuya + NVS */
        uint16_t old_addr = dev->proto.zigbee.short_addr;
        zb_rejoin_update_short_addr(dev, old_addr, short_addr);
    } else {
        /* New device */
        dev->proto.zigbee.short_addr = short_addr;

        /* Set default friendly_name to IEEE hex (stable across rejoins).
         * HA discovery detects "0x..." prefix and uses model name for display.
         * MQTT topics use this as path: zigbee2mqtt/0x00158d0002c4aab4 */
        snprintf(dev->friendly_name, sizeof(dev->friendly_name),
                 "0x%016llx", (unsigned long long)ieee64);

        /* Persist new device to NVS immediately (critical during PAIRING phase) */
        esp_err_t ret = device_persistence_save(dev);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save device 0x%04X to NVS: %s",
                     short_addr, esp_err_to_name(ret));
        }

        /* Update network device count */
        size_t device_count = device_registry_count();
        zb_network_set_device_count((uint8_t)device_count);
        ESP_LOGI(TAG, "New device 0x%04X added, total: %zu", short_addr, device_count);

        /* Add to availability tracking (new device = ONLINE) */
        zb_availability_add_device(short_addr, ZB_AVAIL_POWER_UNKNOWN);
    }

#if CONFIG_GW_LED_ENABLED
    led_status_manager_notify(LED_NOTIFY_DEVICE_NEW);
#endif

    /* Publish EVT_DEVICE_JOINED event */
    evt_device_joined_t evt = {
        .ieee_addr = ieee64,
        .short_addr = short_addr,
        .endpoint = dev->proto.zigbee.endpoint,
        .manufacturer = dev->manufacturer[0] ? dev->manufacturer : NULL,
        .model = dev->model[0] ? dev->model : NULL,
    };
    event_publish(EVT_DEVICE_JOINED, &evt, sizeof(evt));

    /* Persist device immediately, even during PAIRING phase.
     * During PAIRING, state_persistence is deinitialized (to free RAM for Zigbee),
     * so we use save_immediate() which mounts LittleFS temporarily. */
    esp_err_t save_ret = state_persistence_save_immediate();
    if (save_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist device 0x%04X: %s (will retry on next save)",
                 short_addr, esp_err_to_name(save_ret));
    }
}

void zb_callback_device_leave(uint16_t short_addr)
{
    ESP_LOGI(TAG, "Device left: 0x%04X", short_addr);

    /* Look up IEEE for the leave helper */
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        ESP_LOGW(TAG, "Device 0x%04X not found in registry", short_addr);
        return;
    }

    zb_device_leave_cleanup(device->id, short_addr);
}

void zb_callback_attribute_change(uint8_t endpoint, uint16_t cluster_id,
                                   uint16_t attr_id, void *value, size_t value_len)
{
    ESP_LOGD(TAG, "Attribute changed: EP=%d, Cluster=0x%04X, Attr=0x%04X",
             endpoint, cluster_id, attr_id);

    /* This is for local attribute changes on the coordinator */
    /* For most coordinator implementations, this is rarely used */
}

void zb_callback_report_attr(esp_zb_zcl_report_attr_message_t *message)
{
    if (message == NULL) {
        return;
    }

    uint16_t short_addr = message->src_address.u.short_addr;
    uint16_t cluster_id = message->cluster;
    uint16_t attr_id = message->attribute.id;

    ESP_LOGI(TAG, "Attribute report from 0x%04X: Cluster=0x%04X Attr=0x%04X len=%d",
             short_addr, cluster_id, attr_id, message->attribute.data.size);

    /* Record Zigbee message received for performance metrics */
    perf_metrics_record(PERF_METRIC_ZB_MSG_RECEIVED, 1);

    /* Check if device exists in registry */
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        /* Unknown short address — try to resolve by IEEE from Zigbee stack.
         * This handles rejoin with new short_addr after coordinator reboot. */
        esp_zb_ieee_addr_t ieee_addr;
        if (esp_zb_ieee_address_by_short(short_addr, ieee_addr) == ESP_OK) {
            uint64_t ieee64 = 0;
            memcpy(&ieee64, ieee_addr, sizeof(ieee64));
            device_t *ng_dev = device_registry_get(ieee64);
            if (ng_dev != NULL) {
                uint16_t old_addr = ng_dev->proto.zigbee.short_addr;
                zb_rejoin_update_short_addr(ng_dev, old_addr, short_addr);
                device = device_registry_get_by_short_addr(short_addr);
            } else {
                ESP_LOGD(TAG, "Device 0x%04X not in NG registry and no matching IEEE", short_addr);
            }
        }
    } else if (device->proto.zigbee.short_addr == ZB_SHORT_ADDR_PENDING) {
        /* Device was loaded from NVS but hadn't communicated yet — resolve pending address */
        esp_zb_ieee_addr_t ieee_addr;
        if (esp_zb_ieee_address_by_short(short_addr, ieee_addr) == ESP_OK) {
            ESP_LOGI(TAG, "Device 0x%04X: short address resolved from pending", short_addr);
            zb_rejoin_update_short_addr(device, ZB_SHORT_ADDR_PENDING, short_addr);
        }
    }

    /* Update device last seen (NG registry + availability tracking) */
    if (device != NULL) {
        device_registry_update_last_seen(device->id);
    }
    zb_availability_update_last_seen(short_addr);

    /* Retry any pending write commands for sleepy devices.
     * Aqara battery devices don't poll for indirect frames — we must
     * re-send the command immediately when they report (= radio is on). */
    zb_converter_std_retry_pending(short_addr);

    /* Add cluster to device if not already present */
    if (device != NULL) {
        device_zigbee_add_cluster(device, cluster_id);
    }

    /* Try converter-based handling first */
    {
        uint16_t attr_id = message->attribute.id;
        esp_err_t conv_ret = zb_converter_handle_report(
            short_addr, message->dst_endpoint, cluster_id, attr_id,
            message->attribute.data.value, message->attribute.data.size,
            message->attribute.data.type);
        if (conv_ret == ESP_OK) {
            /* Converter handled this report - skip generic handling */
            return;
        }
        /* Fall through to existing generic handlers */
    }

    /* Handle cluster-specific reports */
    switch (cluster_id) {
        case ESP_ZB_ZCL_CLUSTER_ID_BASIC:
            handle_basic_cluster_report(short_addr, message);
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
            handle_onoff_cluster_report(short_addr, message);
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL:
            ESP_LOGI(TAG, "Level control report from 0x%04X", short_addr);
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
            ESP_LOGI(TAG, "Color control report from 0x%04X", short_addr);
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT:
            ESP_LOGI(TAG, "Temperature measurement report from 0x%04X", short_addr);
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:
            ESP_LOGI(TAG, "Humidity measurement report from 0x%04X", short_addr);
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING:
            ESP_LOGI(TAG, "Occupancy sensing report from 0x%04X", short_addr);
            break;

        case ZB_ZCL_CLUSTER_ID_DOOR_LOCK:
            ESP_LOGI(TAG, "Door lock report from 0x%04X", short_addr);
            /* Handle door lock state report */
            if (message->attribute.id == ZB_ZCL_ATTR_DOOR_LOCK_LOCK_STATE_ID) {
                zb_door_lock_handle_report(short_addr, message->dst_endpoint,
                                           message->attribute.id,
                                           message->attribute.data.value,
                                           message->attribute.data.size);
            }
            break;

        case ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT:
            ESP_LOGI(TAG, "Electrical measurement report from 0x%04X attr=0x%04X",
                     short_addr, message->attribute.id);
            /* Handle electrical measurement report */
            zb_electrical_handle_report(short_addr, message->dst_endpoint,
                                         message->attribute.id,
                                         message->attribute.data.value,
                                         message->attribute.data.size);
            break;

        case ZB_ZCL_CLUSTER_ID_METERING:
            ESP_LOGI(TAG, "Metering report from 0x%04X attr=0x%04X",
                     short_addr, message->attribute.id);
            /* Handle metering (smart energy) report */
            zb_metering_handle_report(short_addr, message->dst_endpoint,
                                       message->attribute.id,
                                       message->attribute.data.value,
                                       message->attribute.data.size);
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_DEHUMIDIFICATION_CONTROL:
            ESP_LOGI(TAG, "Dehumidification control report from 0x%04X attr=0x%04X",
                     short_addr, message->attribute.id);
            /* Handle dehumidification control report */
            zb_dehumid_handle_report(short_addr, message->dst_endpoint,
                                      message->attribute.id,
                                      message->attribute.data.value,
                                      message->attribute.data.size);
            break;

        case ZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT:
        case ZB_ZCL_CLUSTER_ID_MULTISTATE_OUTPUT:
        case ZB_ZCL_CLUSTER_ID_MULTISTATE_VALUE:
            {
                const char *type_str = (cluster_id == ZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT) ? "Input" :
                                       (cluster_id == ZB_ZCL_CLUSTER_ID_MULTISTATE_OUTPUT) ? "Output" : "Value";
                ESP_LOGI(TAG, "Multistate %s report from 0x%04X attr=0x%04X",
                         type_str, short_addr, message->attribute.id);

                /* Handle multistate report (multi-button switches, rotary switches, multi-mode outputs) */
                zb_multistate_handle_report(short_addr, message->dst_endpoint,
                                             cluster_id, message->attribute.id,
                                             message->attribute.data.value,
                                             message->attribute.data.size);

                /* Publish multistate-specific state to MQTT */
                /* TEMPORARILY DISABLED - testing Aqara join timing issue */
                // if (mqtt_bridge_is_enabled()) {
                //     zb_multistate_state_t ms_state;
                //     if (zb_multistate_get_state(short_addr, message->dst_endpoint, &ms_state) == ESP_OK) {
                //         device_state_publish_multistate(short_addr, message->dst_endpoint, &ms_state);
                //     }
                // }
            }
            break;

        default:
            ESP_LOGD(TAG, "Unhandled cluster report: 0x%04X from 0x%04X",
                     cluster_id, short_addr);
            break;
    }

    /* Update device type based on clusters (NG) */
    device_t *type_dev = device_registry_get_by_short_addr(short_addr);
    if (type_dev != NULL) {
        device_determine_zigbee_type(type_dev);
    }

    /* Publish attribute update to MQTT */
    /* TEMPORARILY DISABLED - testing Aqara join timing issue */
    // if (mqtt_bridge_is_enabled()) {
    //     /* Parse and publish attribute - simplified approach */
    //     device_state_publish_by_addr(short_addr);
    // }
}

void zb_callback_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            /* Always use NETWORK_FORMATION for the coordinator.
             * If a network already exists in zb_storage, the SDK restores it
             * (emitting DEVICE_REBOOT). If not, it forms a new one
             * (emitting DEVICE_FIRST_START). NETWORK_STEERING is for
             * routers/end-devices and skips the formation/restore step. */
            ESP_LOGI(TAG, "Signal: Skip startup - starting network formation");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
            ESP_LOGI(TAG, "Signal: Device first start - new network forming");
            /* Clear stale devices and bindings from previous network */
            ESP_LOGI(TAG, "Clearing stale devices from persistence and registry");
            device_persistence_clear();
            device_registry_clear_all();
            zb_converter_unbind_all();
            tuya_driver_unbind_all();
            zb_callback_network_formed();
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            ESP_LOGI(TAG, "Signal: Device reboot");
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "Network restored from storage");
                zb_callback_network_formed();
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "Signal: Network steering successful");
            } else {
                ESP_LOGW(TAG, "Signal: Network steering failed: %s",
                        esp_err_to_name(err_status));
            }
            break;

        case ESP_ZB_BDB_SIGNAL_FORMATION:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "Signal: Network formation successful");
                /* Get extended PAN ID using ESP-Zigbee-SDK v1.6.x API (API-004) */
                esp_zb_ieee_addr_t extended_pan_id;
                esp_zb_nwk_get_extended_pan_id(extended_pan_id);
                ESP_LOGI(TAG, "Extended PAN ID: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                         extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                         extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0]);
                zb_callback_network_formed();
            } else {
                ESP_LOGE(TAG, "Signal: Network formation failed: %s",
                        esp_err_to_name(err_status));
            }
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
            {
                esp_zb_zdo_signal_device_annce_params_t *dev_annce_params =
                    (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (dev_annce_params) {
                    zb_callback_device_announce(dev_annce_params);
                } else {
                    ESP_LOGE(TAG, "Device announce signal received but params are NULL! "
                             "err_status=%s", esp_err_to_name(err_status));
                }
            }
            break;

        case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
            {
                if (err_status == ESP_OK) {
                    uint8_t duration = *(uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
                    zb_callback_permit_join_changed(duration);
                }
            }
            break;

        case ESP_ZB_ZDO_SIGNAL_LEAVE:
            {
                /* ESP-Zigbee-SDK v1.6.x: esp_zb_zdo_signal_leave_params_t contains leave_type.
                 * For device leave handling, we rely on the device announce timeout
                 * or explicit leave indication from the device. The coordinator
                 * doesn't receive the leaving device's short address in this signal. */
                esp_zb_zdo_signal_leave_params_t *leave_params =
                    (esp_zb_zdo_signal_leave_params_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (leave_params) {
                    ESP_LOGW(TAG, "Device leave indication: leave_type=%d",
                             leave_params->leave_type);
                    /* Note: leave_type indicates the reason for leave.
                     * Device tracking is handled via availability module timeouts. */
                } else {
                    ESP_LOGW(TAG, "Device leave indication (no parameters)");
                }
            }
            break;

        case ESP_ZB_NLME_STATUS_INDICATION:
            {
                esp_zb_zdo_signal_nwk_status_indication_params_t *nwk_params =
                    (esp_zb_zdo_signal_nwk_status_indication_params_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (nwk_params) {
                    /* Rate-limit per (addr, status) to avoid log spam from sleepy
                     * devices that generate continuous indirect_transaction_expiry
                     * or bad_key_sequence_number after failed interview/rejoin. */
                    static uint16_t s_nlme_last_addr = 0;
                    static uint8_t  s_nlme_last_status = 0xFF;
                    static uint32_t s_nlme_suppress_count = 0;
                    static int64_t  s_nlme_last_log_us = 0;

                    int64_t now_us = esp_timer_get_time();
                    bool same = (nwk_params->network_addr == s_nlme_last_addr &&
                                 nwk_params->status == s_nlme_last_status);
                    bool throttled = same && (now_us - s_nlme_last_log_us < 30000000); /* 30s */

                    if (throttled) {
                        s_nlme_suppress_count++;
                    } else {
                        const char *dev_name = "unknown";
                        device_t *dev = device_registry_get_by_short_addr(nwk_params->network_addr);
                        if (dev && dev->friendly_name[0]) {
                            dev_name = dev->friendly_name;
                        }
                        if (s_nlme_suppress_count > 0) {
                            ESP_LOGW(TAG, "NLME Status: %s (0x%02X), addr=0x%04X (%s) "
                                     "[+%lu suppressed]",
                                     nlme_status_to_str(nwk_params->status), nwk_params->status,
                                     nwk_params->network_addr, dev_name,
                                     (unsigned long)s_nlme_suppress_count);
                        } else {
                            ESP_LOGW(TAG, "NLME Status: %s (0x%02X), addr=0x%04X (%s)",
                                     nlme_status_to_str(nwk_params->status), nwk_params->status,
                                     nwk_params->network_addr, dev_name);
                        }
                        s_nlme_last_addr = nwk_params->network_addr;
                        s_nlme_last_status = nwk_params->status;
                        s_nlme_last_log_us = now_us;
                        s_nlme_suppress_count = 0;
                    }
                } else {
                    ESP_LOGI(TAG, "NLME Status Indication (status: %s)", esp_err_to_name(err_status));
                }

                if (err_status != ESP_OK) {
                    zb_coordinator_update_route_error_count();
                }
            }
            break;

        /* Note: ESP_ZB_ZDO_SIGNAL_PARENT_ANNCE_RSP and ESP_ZB_ZDO_TC_REJOIN_DONE
         * are not available in all ESP-Zigbee-SDK versions. Parent announcements
         * and TC rejoins are tracked via device announce and leave signals. */

        case ESP_ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY:
            ESP_LOGI(TAG, "Signal: Production configuration ready");
            break;

        case ESP_ZB_NWK_SIGNAL_DEVICE_ASSOCIATED:
            {
                esp_zb_nwk_signal_device_associated_params_t *assoc_params =
                    (esp_zb_nwk_signal_device_associated_params_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (assoc_params) {
                    ESP_LOGI(TAG, "Device ASSOCIATED: IEEE=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                             assoc_params->device_addr[7], assoc_params->device_addr[6],
                             assoc_params->device_addr[5], assoc_params->device_addr[4],
                             assoc_params->device_addr[3], assoc_params->device_addr[2],
                             assoc_params->device_addr[1], assoc_params->device_addr[0]);
                }
            }
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE:
            {
                esp_zb_zdo_signal_device_update_params_t *update_params =
                    (esp_zb_zdo_signal_device_update_params_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (update_params) {
                    ESP_LOGI(TAG, "Device UPDATE: short=0x%04X, status=%d, tc_action=%d, parent=0x%04X",
                             update_params->short_addr, update_params->status,
                             update_params->tc_action, update_params->parent_short);
                }
            }
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED:
            {
                esp_zb_zdo_signal_device_authorized_params_t *auth_params =
                    (esp_zb_zdo_signal_device_authorized_params_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (auth_params) {
                    const char *auth_status_str = "unknown";
                    if (auth_params->authorization_type == 0x00) {
                        auth_status_str = (auth_params->authorization_status == 0) ? "SUCCESS" : "FAILED";
                    } else if (auth_params->authorization_type == 0x01) {
                        switch (auth_params->authorization_status) {
                            case 0: auth_status_str = "SUCCESS"; break;
                            case 1: auth_status_str = "TIMEOUT"; break;
                            case 2: auth_status_str = "FAILED"; break;
                        }
                    }
                    ESP_LOGW(TAG, "Device AUTHORIZED: short=0x%04X, auth_type=%d, status=%d (%s)",
                             auth_params->short_addr, auth_params->authorization_type,
                             auth_params->authorization_status, auth_status_str);

                    if (auth_params->authorization_status == 0) {
                        /* Authorization success — trigger join flow immediately.
                         * Some sleepy devices (e.g. Aqara) never send Device
                         * Announce, so we must trigger the join here.
                         * Use long_addr directly from the signal params instead
                         * of esp_zb_ieee_address_by_short() which may fail if
                         * the address table hasn't been populated yet. */
                        ESP_LOGI(TAG, "Device 0x%04X authorized, triggering join flow",
                                 auth_params->short_addr);
                        zb_callback_device_join(ESP_ZB_ZDP_STATUS_SUCCESS,
                                                auth_params->short_addr,
                                                auth_params->long_addr);
                    } else {
                        ESP_LOGE(TAG, "Device 0x%04X authorization FAILED (type=%d, status=%d) - "
                                 "TCLK exchange issue, device will leave",
                                 auth_params->short_addr, auth_params->authorization_type,
                                 auth_params->authorization_status);
                    }
                }
            }
            break;

        case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION:
            {
                esp_zb_zdo_signal_leave_indication_params_t *leave_ind =
                    (esp_zb_zdo_signal_leave_indication_params_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (leave_ind) {
                    ESP_LOGW(TAG, "Device LEAVE_INDICATION: short=0x%04X, rejoin=%d, "
                             "IEEE=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                             leave_ind->short_addr, leave_ind->rejoin,
                             leave_ind->device_addr[7], leave_ind->device_addr[6],
                             leave_ind->device_addr[5], leave_ind->device_addr[4],
                             leave_ind->device_addr[3], leave_ind->device_addr[2],
                             leave_ind->device_addr[1], leave_ind->device_addr[0]);

                    if (!leave_ind->rejoin) {
                        /* Device leaving permanently — full cleanup via helper */
                        uint64_t leave_ieee = zb_ieee_to_u64(leave_ind->device_addr);
                        zb_device_leave_cleanup(leave_ieee, leave_ind->short_addr);
                    } else {
                        ESP_LOGI(TAG, "Device 0x%04X leaving to rejoin, keeping in registry",
                                 leave_ind->short_addr);
                    }
                }
            }
            break;

        case ESP_ZB_ZDO_DEVICE_UNAVAILABLE:
            {
                esp_zb_zdo_device_unavailable_params_t *unavail_params =
                    (esp_zb_zdo_device_unavailable_params_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (unavail_params) {
                    /* Rate-limit: log once per device per 30 seconds */
                    static uint16_t s_unavail_last_addr = 0;
                    static uint32_t s_unavail_suppress = 0;
                    static int64_t  s_unavail_last_log_us = 0;

                    int64_t now_us = esp_timer_get_time();
                    bool throttled = (unavail_params->short_addr == s_unavail_last_addr &&
                                      (now_us - s_unavail_last_log_us) < 30000000);
                    if (throttled) {
                        s_unavail_suppress++;
                    } else {
                        if (s_unavail_suppress > 0) {
                            ESP_LOGW(TAG, "Device unavailable: 0x%04X (no MAC/APS ACK) "
                                     "[+%lu suppressed]",
                                     unavail_params->short_addr,
                                     (unsigned long)s_unavail_suppress);
                        } else {
                            ESP_LOGW(TAG, "Device unavailable: 0x%04X (no MAC/APS ACK)",
                                     unavail_params->short_addr);
                        }
                        s_unavail_last_addr = unavail_params->short_addr;
                        s_unavail_last_log_us = now_us;
                        s_unavail_suppress = 0;
                    }
                    cmd_retry_on_unavailable(unavail_params->short_addr);
                } else {
                    ESP_LOGW(TAG, "Device unavailable signal (no params)");
                }
            }
            break;

        default:
            {
                ESP_LOGW(TAG, "Unhandled ZDO signal: 0x%x, status: %s",
                         sig_type, esp_err_to_name(err_status));
                uint8_t *raw_params = (uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (raw_params) {
                    ESP_LOG_BUFFER_HEX_LEVEL(TAG, raw_params, 16, ESP_LOG_WARN);
                }
            }
            break;
    }
}

#if CONFIG_ZIGBEE_DEVICE_TYPE_COORDINATOR
/**
 * @brief Zigbee application signal handler for coordinator mode
 *
 * This is the entry point required by ESP-Zigbee-SDK. It dispatches
 * to the internal signal handler implementation.
 */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    zb_callback_signal_handler(signal_struct);
}
#endif /* CONFIG_ZIGBEE_DEVICE_TYPE_COORDINATOR */

/* Note: esp_zb_zcl_cmd_recv_message_t is deprecated in SDK v1.6.x
 * ZCL commands are now handled through the action handler callback.
 * See esp_zb_core_action_handler_register() for the new approach. */

void zb_callback_device_announce(esp_zb_zdo_signal_device_annce_params_t *device_annce)
{
    if (device_annce == NULL) {
        ESP_LOGE(TAG, "zb_callback_device_announce: params are NULL!");
        return;
    }

    uint16_t short_addr = device_annce->device_short_addr;

    ESP_LOGI(TAG, "Device announce: 0x%04X [%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X]",
             short_addr,
             device_annce->ieee_addr[7], device_annce->ieee_addr[6],
             device_annce->ieee_addr[5], device_annce->ieee_addr[4],
             device_annce->ieee_addr[3], device_annce->ieee_addr[2],
             device_annce->ieee_addr[1], device_annce->ieee_addr[0]);

    /* API-005: Log APS Authentication State during device announce
     *
     * This logs the coordinator's APS authentication state when a device
     * announces. A properly secured network should show authenticated state.
     */
    bool coordinator_auth = esp_zb_aps_is_authenticated();
    ESP_LOGD(TAG, "Device announce 0x%04X: coordinator APS auth=%s",
             short_addr, coordinator_auth ? "yes" : "no");

    /* Convert IEEE address to 64-bit */
    uint64_t ieee64 = zb_ieee_to_u64(device_annce->ieee_addr);

    /* Check and apply install code for secure joining */
    apply_install_code_if_present(ieee64, short_addr);

    /* Publish bridge event: device_announce with network address */
    /* TEMPORARILY DISABLED - testing Aqara join timing issue */
    // bridge_event_device_announce_full(ieee64, short_addr);

    ESP_LOGI(TAG, "Device announce processing for 0x%04X", short_addr);

    /* Check if this is a new device or rejoin */
    device_t *existing_device = device_registry_get_by_short_addr(short_addr);
    bool is_new_device = (existing_device == NULL);

    ESP_LOGI(TAG, "Device 0x%04X: is_new=%d, existing=%p", short_addr, is_new_device, (void *)existing_device);

    /* Treat device announce as a join/rejoin event */
    zb_callback_device_join(ESP_ZB_ZDP_STATUS_SUCCESS,
                           short_addr,
                           device_annce->ieee_addr);

    ESP_LOGI(TAG, "Device join callback completed for 0x%04X", short_addr);

    /* Limit permit_join to 10 seconds after a successful device join.
     * This saves battery on sleepy devices (network closes quickly) and
     * ensures WiFi/MQTT can reconnect sooner. Only reduce if current
     * remaining time is greater than 10 seconds. */
    zb_permit_join_state_t pj_state;
    if (zb_coordinator_get_permit_join_state(&pj_state) == ESP_OK &&
        pj_state.enabled && pj_state.remaining_time > 10) {
        ESP_LOGI(TAG, "Device joined - reducing permit_join from %d to 10 seconds",
                 pj_state.remaining_time);
        zb_coordinator_permit_join(10);
    }

    /* Trigger interview for new devices */
    if (is_new_device) {
        ESP_LOGI(TAG, "New device detected, starting interview for 0x%04X", short_addr);
#if CONFIG_GW_LED_ENABLED
        led_status_manager_notify(LED_NOTIFY_DEVICE_NEW);
#endif
        esp_err_t ret = zb_interview_start(ieee64, short_addr);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to start interview: %s", esp_err_to_name(ret));
#if CONFIG_GW_LED_ENABLED
            led_status_manager_notify(LED_NOTIFY_DEVICE_FAILED);
#endif
        }
    } else {
        ESP_LOGI(TAG, "Device 0x%04X rejoined (existing device)", short_addr);
#if CONFIG_GW_LED_ENABLED
        led_status_manager_notify(LED_NOTIFY_DEVICE_KNOWN);
#endif

        /* Re-interview if manufacturer/model are missing (e.g. first interview's
         * Basic cluster read failed, or device was persisted before interview
         * completed). This populates the fields needed for converter/driver binding. */
        device_t *rejoin_dev = device_registry_get(ieee64);
        if (rejoin_dev != NULL && rejoin_dev->manufacturer[0] == '\0') {
            ESP_LOGI(TAG, "Device 0x%04X missing manufacturer — re-interviewing", short_addr);
            esp_err_t ret = zb_interview_start(ieee64, short_addr);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Re-interview failed to start: %s", esp_err_to_name(ret));
            }
        }
    }
}

/** Auto-permit-join timer, re-armed by its own callback while it waits. */
static esp_timer_handle_t s_rejoin_timer;

/** How long to keep waiting for LIFECYCLE_PHASE_NORMAL, in 5s steps. */
#define ZB_REJOIN_PHASE_RETRY_INTERVAL_US (5 * 1000000ULL)
#define ZB_REJOIN_PHASE_MAX_RETRIES       36    /* 3 minutes */

/**
 * @brief Timer callback to open permit_join after boot.
 *
 * Runs ~15s after network formation, outside the Zigbee callback context so
 * the lock is available.
 *
 * It used to assume the lifecycle was NORMAL by then. That held only while the
 * coordinator was started after the WiFi and MQTT phases; it now starts before
 * them (see zigbee_stack_start() in main.c), so on a boot with the uplink down
 * app_main can still be sitting in the captive portal when this fires, and
 * zb_coordinator_permit_join() rejects the request outright:
 *
 *     W ZB_COORD: Rejecting permit_join during BOOT phase - services not ready
 *
 * The request was then simply lost. Wait for the phase instead of assuming it —
 * the devices this exists for are the ones already paired, and they are exactly
 * the ones that need it after a coordinator restart.
 */
static void zb_rejoin_permit_join_cb(void *arg)
{
    (void)arg;

    static uint32_t retries;

    if (lifecycle_get_phase() == LIFECYCLE_PHASE_BOOT) {
        if (retries < ZB_REJOIN_PHASE_MAX_RETRIES) {
            retries++;
            esp_timer_start_once(s_rejoin_timer, ZB_REJOIN_PHASE_RETRY_INTERVAL_US);
            ESP_LOGD("ZB_CB", "Still in BOOT phase, retrying permit_join (%lu/%d)",
                     (unsigned long)retries, ZB_REJOIN_PHASE_MAX_RETRIES);
        } else {
            ESP_LOGW("ZB_CB", "Boot never left BOOT phase — giving up on the "
                              "automatic permit_join. Paired devices still rejoin "
                              "on their own; use the bridge request to open it.");
        }
        return;
    }

    retries = 0;
    size_t dev_count = device_registry_count();
    ESP_LOGI("ZB_CB", "Auto-opening permit_join for 120s (%zu persisted devices)", dev_count);
    zb_coordinator_permit_join(120);
}

void zb_callback_network_formed(void)
{
    ESP_LOGI(TAG, "=== Network Formed ===");

    /* Update network status */
    zb_network_set_formed(true);

    /* API-005: Set APS Authentication State
     *
     * After network formation, the coordinator is authenticated with its
     * Trust Center (itself for coordinator role). Setting authenticated
     * state enables secure APS layer operations.
     */
    esp_zb_aps_set_authenticated(true);
    ESP_LOGI(TAG, "APS Authentication State: authenticated (coordinator/TC)");

    /* Increase MAC indirect transaction persistence time from default ~7.7s to 60s.
     * Sleepy end devices (e.g. Aqara sensors) only poll infrequently — the short
     * default causes write-attribute commands to expire before the device wakes up. */
    esp_err_t mac_ret = esp_zb_mac_set_transaction_persistence_time(60 * 1000000ULL);
    if (mac_ret == ESP_OK) {
        ESP_LOGI(TAG, "MAC indirect persistence time set to 60s");
    } else {
        ESP_LOGW(TAG, "Failed to set MAC persistence time: %s", esp_err_to_name(mac_ret));
    }

    /* Get network info */
    zb_network_info_t net_info;
    if (zb_network_get_info(&net_info) == ESP_OK) {
        ESP_LOGI(TAG, "PAN ID: 0x%04X", net_info.pan_id);
        ESP_LOGI(TAG, "Channel: %d", net_info.channel);
    }

    /* Save network configuration */
    esp_err_t ret = zb_network_save_config();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Network configuration saved");
    } else {
        ESP_LOGW(TAG, "Failed to save network configuration: %s",
                esp_err_to_name(ret));
    }

    /* Publish bridge event: network_started */
    /* TEMPORARILY DISABLED - testing Aqara join timing issue */
    // bridge_event_network_started();

    /* Schedule auto-permit-join after a delay.  We cannot call
     * zb_coordinator_permit_join() directly here because:
     *   1. Lifecycle is still BOOT → permit_join is blocked
     *   2. We're inside the Zigbee callback → acquiring Zigbee lock would deadlock
     * The callback waits for LIFECYCLE_PHASE_NORMAL itself. */
    size_t dev_count = device_registry_count();
    if (dev_count > 0) {
        /* Create once. The network can form again (a rejoin, a channel change)
         * and creating a second timer would leak the first one's handle. */
        if (s_rejoin_timer == NULL) {
            esp_timer_create_args_t timer_cfg = {
                .callback = zb_rejoin_permit_join_cb,
                .name = "rejoin_pj",
            };
            if (esp_timer_create(&timer_cfg, &s_rejoin_timer) != ESP_OK) {
                ESP_LOGW(TAG, "Could not create the permit_join timer");
                return;
            }
        }

        esp_timer_stop(s_rejoin_timer);   /* No-op if it is not running */
        if (esp_timer_start_once(s_rejoin_timer, 15 * 1000000ULL) == ESP_OK) {
            ESP_LOGI(TAG, "Scheduled permit_join in 15s for %zu persisted device(s)", dev_count);
        }
    }
}

void zb_callback_bdb_commissioning_complete(esp_zb_bdb_commissioning_status_t status)
{
    ESP_LOGI(TAG, "BDB commissioning complete: status=0x%02X", status);

    if (status == ESP_ZB_BDB_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Commissioning successful");

        /* API-005: Verify APS Authentication State after successful commissioning
         *
         * After successful BDB commissioning, verify that the APS layer
         * authentication state is properly set. This is a sanity check
         * to ensure security is enabled.
         */
        bool is_authenticated = esp_zb_aps_is_authenticated();
        if (!is_authenticated) {
            ESP_LOGW(TAG, "APS not authenticated after commissioning, setting now");
            esp_zb_aps_set_authenticated(true);
        }
        ESP_LOGI(TAG, "APS Authentication State verified: authenticated");
    } else {
        ESP_LOGW(TAG, "Commissioning failed with status: 0x%02X", status);
    }
}

void zb_callback_permit_join_changed(uint8_t permit_duration)
{
    bool permit_join = (permit_duration > 0);

    ESP_LOGI(TAG, "Permit join %s (duration: %d seconds)",
             permit_join ? "OPENED" : "CLOSED", permit_duration);

    /* Log network diagnostics when permit_join opens */
    if (permit_join) {
        uint16_t pan_id = esp_zb_get_pan_id();
        uint16_t short_addr = esp_zb_get_short_address();
        uint8_t channel = zb_network_get_channel_config();
        esp_zb_ieee_addr_t ext_pan_id;
        esp_zb_nwk_get_extended_pan_id(ext_pan_id);

        ESP_LOGW(TAG, "=== ZIGBEE NETWORK DIAGNOSTICS ===");
        ESP_LOGW(TAG, "  PAN ID: 0x%04X", pan_id);
        ESP_LOGW(TAG, "  Short Address: 0x%04X (Coordinator)", short_addr);
        ESP_LOGW(TAG, "  Channel: %d", channel);
        ESP_LOGW(TAG, "  Extended PAN ID: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                 ext_pan_id[7], ext_pan_id[6], ext_pan_id[5], ext_pan_id[4],
                 ext_pan_id[3], ext_pan_id[2], ext_pan_id[1], ext_pan_id[0]);
        ESP_LOGW(TAG, "  WiFi: Running (Zigbee has MIDDLE coex priority)");
        ESP_LOGW(TAG, "================================");
    }

    zb_network_set_permit_join(permit_join);

#if CONFIG_GW_LED_ENABLED
    /* Update LED status for permit join / pairing mode */
    led_status_manager_set_condition(LED_COND_PERMIT_JOIN, permit_join);
#endif

    /* Handle lifecycle phase transitions.
     * When permit_join is closed (by Zigbee stack, timer, or explicit call),
     * transition back to NORMAL phase to re-enable services and WiFi auto-reconnect. */
    if (!permit_join && lifecycle_get_phase() == LIFECYCLE_PHASE_PAIRING) {
        ESP_LOGI(TAG, "Permit join closed - transitioning to NORMAL phase");
        lifecycle_enter_phase(LIFECYCLE_PHASE_NORMAL);
    }

    /* Publish bridge event: permit_join */
    /* TEMPORARILY DISABLED - testing Aqara join timing issue */
    // bridge_event_permit_join(permit_join, permit_duration);
}

/* Internal helper functions */
static const char* zdo_status_to_str(esp_zb_zdp_status_t status)
{
    switch (status) {
        case ESP_ZB_ZDP_STATUS_SUCCESS: return "SUCCESS";
        case ESP_ZB_ZDP_STATUS_INV_REQUESTTYPE: return "INVALID_REQUEST";
        case ESP_ZB_ZDP_STATUS_DEVICE_NOT_FOUND: return "DEVICE_NOT_FOUND";
        case ESP_ZB_ZDP_STATUS_INVALID_EP: return "INVALID_EP";
        case ESP_ZB_ZDP_STATUS_NOT_ACTIVE: return "NOT_ACTIVE";
        case ESP_ZB_ZDP_STATUS_NOT_SUPPORTED: return "NOT_SUPPORTED";
        case ESP_ZB_ZDP_STATUS_TIMEOUT: return "TIMEOUT";
        case ESP_ZB_ZDP_STATUS_NO_MATCH: return "NO_MATCH";
        case ESP_ZB_ZDP_STATUS_NO_ENTRY: return "NO_ENTRY";
        case ESP_ZB_ZDP_STATUS_NO_DESCRIPTOR: return "NO_DESCRIPTOR";
        case ESP_ZB_ZDP_STATUS_INSUFFICIENT_SPACE: return "INSUFFICIENT_SPACE";
        case ESP_ZB_ZDP_STATUS_NOT_PERMITTED: return "NOT_PERMITTED";
        case ESP_ZB_ZDP_STATUS_TABLE_FULL: return "TABLE_FULL";
        case ESP_ZB_ZDP_STATUS_NOT_AUTHORIZED: return "NOT_AUTHORIZED";
        default: return "UNKNOWN";
    }
}

static void handle_basic_cluster_report(uint16_t short_addr, esp_zb_zcl_report_attr_message_t *msg)
{
    if (msg == NULL) {
        return;
    }

    uint16_t attr_id = msg->attribute.id;
    esp_zb_zcl_attribute_data_t *data = &msg->attribute.data;

    ESP_LOGI(TAG, "Basic cluster report from 0x%04X: attr=0x%04X", short_addr, attr_id);

    /* Get NG device for most operations */
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        return;
    }

    switch (attr_id) {
        case ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID:
            if (zcl_copy_char_string(data, device->manufacturer,
                                     sizeof(device->manufacturer))) {
                ESP_LOGI(TAG, "Device 0x%04X manufacturer: %s", short_addr, device->manufacturer);
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID:
            {
                if (zcl_copy_char_string(data, device->model, sizeof(device->model))) {
                    ESP_LOGI(TAG, "Device 0x%04X model: %s", short_addr, device->model);

                    /* Check if this is a sleepy device with an active interview.
                     * Sleepy devices (battery-powered) go to sleep quickly after joining,
                     * causing ZDO interview requests to timeout. If we have a converter
                     * for this device, cancel the interview and use the converter's
                     * cluster information instead. */
                    uint64_t ieee64 = device->id;
                    if (strncmp(device->model, "lumi.", 5) == 0 &&
                        zb_interview_is_active(ieee64)) {
                        /* The DB lookup goes to LittleFS and blocks, and the
                         * branch below writes back through `device`. Copy the
                         * keys, then re-fetch: a remove from the MQTT or
                         * ESPHome task can drop this device meanwhile. */
                        char mfr_key[sizeof(device->manufacturer)];
                        char model_key[sizeof(device->model)];
                        snprintf(mfr_key, sizeof(mfr_key), "%s", device->manufacturer);
                        snprintf(model_key, sizeof(model_key), "%s", device->model);

                        const zb_converter_def_t *conv = zb_converter_find(mfr_key, model_key);

                        device = device_registry_get_by_short_addr(short_addr);
                        if (device == NULL) {
                            return;  /* gone while we were reading the DB */
                        }

                        if (conv != NULL) {
                            ESP_LOGW(TAG, "Sleepy device 0x%04X detected with active interview - canceling",
                                     short_addr);
                            zb_interview_cancel(ieee64);

                            /* Extract clusters from converter's definitions */
                            for (uint8_t i = 0; i < conv->from_zigbee_count; i++) {
                                device_zigbee_add_cluster(device, conv->from_zigbee[i].cluster_id);
                            }
                            for (uint8_t i = 0; i < conv->to_zigbee_count; i++) {
                                device_zigbee_add_cluster(device, conv->to_zigbee[i].cluster_id);
                            }

                            /* Determine device type (NG) and bind converter */
                            device_determine_zigbee_type(device);
                            zb_converter_bind(short_addr, conv);
                            device->proto.zigbee.converter = conv;
                            if (conv->init != NULL) {
                                conv->init(short_addr);
                            }
                            ESP_LOGI(TAG, "Sleepy device 0x%04X: using converter '%s' instead of ZDO interview",
                                     short_addr, conv->description ? conv->description : conv->model);

                            /* Merge converter-derived capabilities for discovery */
                            uint32_t conv_caps = zb_converter_get_capabilities(conv);
                            if (conv_caps != 0) {
                                device->capabilities |= conv_caps;
                            }

                            /* Generate readable friendly_name if still auto-generated IEEE hex */
                            char ieee_default[24];
                            device_id_to_str(device->id, ieee_default, sizeof(ieee_default));
                            if (strcmp(device->friendly_name, ieee_default) == 0) {
                                if (device->model[0] != '\0') {
                                    snprintf(device->friendly_name, sizeof(device->friendly_name),
                                             "%.23s 0x%04X", device->model, short_addr);
                                } else {
                                    snprintf(device->friendly_name, sizeof(device->friendly_name),
                                             "Device 0x%04X", short_addr);
                                }
                                ESP_LOGI(TAG, "Device 0x%04X friendly_name: '%s'",
                                         short_addr, device->friendly_name);
                            }

                            /* Update NVS with model, clusters, and device type via NG persistence */
                            esp_err_t nvs_ret = device_persistence_save(device);
                            if (nvs_ret == ESP_OK) {
                                ESP_LOGI(TAG, "Device 0x%04X updated in NVS with model", short_addr);
                            }

                            /* Publish EVT_DEVICE_INTERVIEWED so HA discovery picks up
                             * this sleepy device that skipped the normal interview path */
                            evt_device_interviewed_t evt = {
                                .ieee_addr = device->id,
                                .short_addr = short_addr,
                                .success = true,
                                .endpoint_count = 1
                            };
                            event_publish(EVT_DEVICE_INTERVIEWED, &evt, sizeof(evt));
                        }
                    }

                    /* Early Tuya driver binding: manufacturer+model now known.
                     * Bind before first DP arrives so discovery entities are
                     * already published when the device starts reporting. */
                    if (device->manufacturer[0] != '\0' &&
                        zb_device_is_tuya(short_addr) &&
                        tuya_driver_get(short_addr) == NULL) {
                        const tuya_device_driver_t *drv =
                            tuya_driver_find(device->manufacturer, device->model);
                        if (drv != NULL) {
                            tuya_driver_bind(short_addr, drv);
                            if (drv->init_device) {
                                drv->init_device(short_addr);
                            }
                            ESP_LOGI(TAG, "Device 0x%04X: early-bound to Tuya driver '%s'",
                                     short_addr, drv->name);
                            /* Tuya drivers now use device_t */
                            if (drv->publish_discovery && device) {
                                drv->publish_discovery(device);
                            }
                        }
                    }

                    /* Late converter binding: model is now known but no converter
                     * bound yet (e.g. device was persisted with empty model before
                     * the converter DB was flashed, and no interview is active).
                     * Try to find converter, set capabilities, persist, and notify
                     * ESPHome adapter so entities get registered on next wake-up. */
                    if (device->proto.zigbee.converter == NULL &&
                        !zb_interview_is_active(ieee64)) {
                        const zb_converter_def_t *late_conv = zb_converter_find(
                            device->manufacturer, device->model);
                        if (late_conv != NULL) {
                            for (uint8_t i = 0; i < late_conv->from_zigbee_count; i++) {
                                device_zigbee_add_cluster(device, late_conv->from_zigbee[i].cluster_id);
                            }
                            for (uint8_t i = 0; i < late_conv->to_zigbee_count; i++) {
                                device_zigbee_add_cluster(device, late_conv->to_zigbee[i].cluster_id);
                            }
                            device_determine_zigbee_type(device);
                            zb_converter_bind(short_addr, late_conv);
                            device->proto.zigbee.converter = late_conv;
                            if (late_conv->init != NULL) {
                                late_conv->init(short_addr);
                            }
                            uint32_t conv_caps = zb_converter_get_capabilities(late_conv);
                            if (conv_caps != 0) {
                                device->capabilities |= conv_caps;
                            }
                            ESP_LOGI(TAG, "Late-bound converter for 0x%04X: %s (caps=0x%08lx)",
                                     short_addr,
                                     late_conv->description ? late_conv->description : late_conv->model,
                                     (unsigned long)device->capabilities);

                            /* Publish interviewed event to trigger ESPHome entity registration */
                            evt_device_interviewed_t evt = {
                                .ieee_addr = device->id,
                                .short_addr = short_addr,
                                .success = true,
                                .endpoint_count = 1
                            };
                            event_publish(EVT_DEVICE_INTERVIEWED, &evt, sizeof(evt));
                        }
                    }

                    /* Always persist when model is newly learned — even if no
                     * converter was found.  Next firmware with a larger DB may
                     * find it via try_rebind_converter() on boot. */
                    device_persistence_save(device);
                }
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID:
            {
                char sw_version[65];
                if (zcl_copy_char_string(data, sw_version, sizeof(sw_version))) {
                    ESP_LOGI(TAG, "Device 0x%04X SW version: %s", short_addr, sw_version);
                }
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID:
            if (data->value != NULL && data->size >= 1) {
                uint8_t power_source = *(uint8_t *)data->value;
                ESP_LOGI(TAG, "Device 0x%04X power source: 0x%02X", short_addr, power_source);
            }
            break;

        default:
            ESP_LOGD(TAG, "Unhandled basic cluster attr 0x%04X", attr_id);
            break;
    }
}

static void handle_onoff_cluster_report(uint16_t short_addr, esp_zb_zcl_report_attr_message_t *msg)
{
    if (msg == NULL) {
        return;
    }

    uint16_t attr_id = msg->attribute.id;
    esp_zb_zcl_attribute_data_t *data = &msg->attribute.data;

    ESP_LOGI(TAG, "On/Off cluster report from 0x%04X: attr=0x%04X", short_addr, attr_id);

    if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        if (data->value != NULL && data->size >= 1) {
            bool on_off_state = *(bool *)data->value;
            ESP_LOGI(TAG, "Device 0x%04X on/off state: %s", short_addr, on_off_state ? "ON" : "OFF");

            /* Update last_seen via device registry */
            device_t *dev = device_registry_get_by_short_addr(short_addr);
            if (dev != NULL) {
                dev->last_seen = (uint32_t)time(NULL);
            }
        }
    } else if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_GLOBAL_SCENE_CONTROL) {
        if (data->value != NULL && data->size >= 1) {
            bool scene_control = *(bool *)data->value;
            ESP_LOGD(TAG, "Device 0x%04X global scene control: %s", short_addr, scene_control ? "enabled" : "disabled");
        }
    } else if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_TIME) {
        if (data->value != NULL && data->size >= 2) {
            uint16_t on_time = *(uint16_t *)data->value;
            ESP_LOGD(TAG, "Device 0x%04X on time: %d", short_addr, on_time);
        }
    } else {
        ESP_LOGD(TAG, "Unhandled on/off cluster attr 0x%04X", attr_id);
    }
}
