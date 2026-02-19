/**
 * @file device_state_publisher.c
 * @brief Device State Publisher Implementation
 *
 * Publishes Zigbee device states via the event bus to MQTT.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "device_state_publisher.h"
#if CONFIG_STATE_PERSISTENCE_ENABLE
#include "state_persistence.h"
#endif
#include "gateway_defaults.h"
#include "utils/json_utils.h"
#include "zigbee/zb_constants.h"
#include "zigbee/zb_cluster_electrical.h"
#include "zigbee/zb_cluster_multistate.h"
#include "zigbee/zb_diagnostics.h"
#include "zigbee/zb_tuya.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/device/device_registry.h"
#include "core/memory/buffer_pool_helpers.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "DEV_STATE_PUB";

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * @brief Publish device state via event bus
 *
 * Merges the JSON state into device_registry, then publishes
 * EVT_DEVICE_STATE_CHANGED with json_state=NULL. The async handler
 * reads from device_registry_get_state() to avoid use-after-free.
 *
 * @param device Device to publish state for (device_t)
 * @param json_str JSON string of the state (will NOT be freed)
 * @param cluster_id Cluster ID (0 for general state)
 * @param attr_id Attribute ID (0 for general state)
 * @return ESP_OK on success
 */
static esp_err_t publish_state_event(const device_t *device, const char *json_str,
                                     uint16_t cluster_id, uint16_t attr_id)
{
    if (device == NULL || json_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Merge state into device_registry so the async handler can read it.
     * The event bus copies the struct but NOT pointed-to strings, so
     * json_state would be a dangling pointer by the time the handler runs. */
    cJSON *parsed = cJSON_Parse(json_str);
    if (parsed != NULL) {
        device_registry_merge_state(device->id, parsed);
        cJSON_Delete(parsed);
    }

    /* Get Zigbee fields from device */
    uint16_t short_addr = device->proto.zigbee.short_addr;
    uint8_t endpoint = device->proto.zigbee.endpoint;

    evt_device_state_t evt = {
        .ieee_addr = device->id,
        .short_addr = short_addr,
        .endpoint = endpoint,
        .cluster_id = cluster_id,
        .attr_id = attr_id,
        .json_state = NULL,  /* Handler reads from device_registry_get_state() */
    };

    esp_err_t ret = event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to publish state event for %s: %s",
                 device->friendly_name, esp_err_to_name(ret));
    }
#if CONFIG_STATE_PERSISTENCE_ENABLE
    if (ret == ESP_OK) {
        state_persistence_mark_dirty();
    }
#endif

    return ret;
}

/* ============================================================================
 * Generic Device State Publish Helper
 * ============================================================================ */

/**
 * @brief Callback type for building type-specific JSON payload
 *
 * @param[in] device The device being published (device_t)
 * @param[in] state Pointer to state data (type-specific)
 * @param[out] json The JSON object to add fields to
 * @return ESP_OK on success
 */
typedef esp_err_t (*json_builder_fn)(const device_t *device, const void *state, cJSON *json);

/**
 * @brief Generic device state publish helper
 *
 * Handles common publish workflow: device lookup, JSON creation,
 * and event publishing. The MQTT adapter will handle actual MQTT publishing.
 *
 * @param[in] short_addr Zigbee short address for device lookup
 * @param[in] state Pointer to type-specific state data
 * @param[in] builder Callback function to build JSON payload
 * @param[in] state_type Name of state type for logging
 * @return ESP_OK on success
 */
static esp_err_t generic_device_state_publish(uint16_t short_addr,
                                               const void *state,
                                               json_builder_fn builder,
                                               const char *state_type)
{
    if (state == NULL || builder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        ESP_LOGW(TAG, "Device 0x%04X not found for %s publish", short_addr, state_type);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for %s", state_type);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = builder(device, state, json);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build %s JSON: %s", state_type, esp_err_to_name(ret));
        cJSON_Delete(json);
        return ret;
    }

    /* Use buffer pool for JSON serialization to reduce heap fragmentation */
    bool from_pool;
    char *json_str = pool_json_print_unformatted(json, &from_pool);
    cJSON_Delete(json);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize %s JSON", state_type);
        return ESP_FAIL;
    }

    /* Publish via event bus - MQTT adapter will handle actual MQTT publishing */
    ret = publish_state_event(device, json_str, 0, 0);

    pool_json_free(json_str, from_pool);
    return ret;
}

/* ============================================================================
 * JSON Builder Functions for Type-Specific Payloads
 * ============================================================================ */

/**
 * @brief Get link quality from device (Zigbee LQI)
 */
static uint8_t get_device_link_quality(const device_t *device)
{
    if (device != NULL) {
        return device->proto.zigbee.lqi;
    }
    return 0;
}

/**
 * @brief Build electrical measurement JSON payload
 */
static esp_err_t build_electrical_json(const device_t *device, const void *state, cJSON *json)
{
    const zb_electrical_state_t *electrical = (const zb_electrical_state_t *)state;

    float voltage = zb_electrical_get_voltage_v(electrical);
    float current = zb_electrical_get_current_a(electrical);
    float power = zb_electrical_get_power_w(electrical);
    float power_factor = zb_electrical_get_power_factor(electrical);

    cJSON_AddNumberToObject(json, "voltage", voltage);
    cJSON_AddNumberToObject(json, "current", current);
    cJSON_AddNumberToObject(json, "power", power);
    cJSON_AddNumberToObject(json, "power_factor", power_factor);
    cJSON_AddNumberToObject(json, "linkquality", get_device_link_quality(device));

    ESP_LOGI(TAG, "Published electrical state for %s: V=%.1f, A=%.3f, W=%.1f, PF=%.2f",
             device->friendly_name, voltage, current, power, power_factor);

    return ESP_OK;
}

/**
 * @brief Build metering (energy) JSON payload
 */
static esp_err_t build_metering_json(const device_t *device, const void *state, cJSON *json)
{
    const zb_metering_state_t *metering = (const zb_metering_state_t *)state;

    double energy = zb_metering_get_total_energy(metering);
    double power = zb_metering_get_power_w(metering);
    double current_day = zb_metering_get_current_day_energy(metering);
    double previous_day = zb_metering_get_previous_day_energy(metering);

    /* Calculate energy produced (for solar meters) */
    double energy_produced = 0.0;
    if (metering->current_summation_received > 0) {
        uint32_t divisor = (metering->divisor > 0) ? metering->divisor : 1;
        uint32_t multiplier = (metering->multiplier > 0) ? metering->multiplier : 1;
        energy_produced = ((double)metering->current_summation_received *
                          (double)multiplier) / (double)divisor;
    }

    /* Round to appropriate precision */
    cJSON_AddNumberToObject(json, "energy",
                            (int)(energy * GW_METERING_ENERGY_PRECISION + 0.5) / GW_METERING_ENERGY_PRECISION);
    cJSON_AddNumberToObject(json, "power",
                            (int)(power * GW_METERING_POWER_PRECISION + 0.5) / GW_METERING_POWER_PRECISION);
    cJSON_AddNumberToObject(json, "current_day_consumption",
                            (int)(current_day * GW_METERING_ENERGY_PRECISION + 0.5) / GW_METERING_ENERGY_PRECISION);
    cJSON_AddNumberToObject(json, "previous_day_consumption",
                            (int)(previous_day * GW_METERING_ENERGY_PRECISION + 0.5) / GW_METERING_ENERGY_PRECISION);

    /* Add energy produced if available */
    if (energy_produced > 0) {
        cJSON_AddNumberToObject(json, "energy_produced",
                                (int)(energy_produced * GW_METERING_ENERGY_PRECISION + 0.5) / GW_METERING_ENERGY_PRECISION);
    }

    /* Add unit and device type */
    const char *unit_str = zb_metering_get_unit_string(metering->unit_of_measure);
    if (unit_str != NULL && strlen(unit_str) > 0) {
        cJSON_AddStringToObject(json, "unit", unit_str);
    }

    /* Add battery percentage if available */
    if (metering->battery_percentage > 0 && metering->battery_percentage <= GW_BATTERY_PERCENTAGE_MAX) {
        cJSON_AddNumberToObject(json, "battery", metering->battery_percentage);
    }

    ESP_LOGI(TAG, "Published metering state for %s: %.3f %s, %.1f W",
             device->friendly_name, energy, unit_str ? unit_str : "kWh", power);

    return ESP_OK;
}

/**
 * @brief Build device temperature JSON payload
 */
static esp_err_t build_device_temperature_json(const device_t *device,
                                                const void *state, cJSON *json)
{
    const zb_device_temperature_t *temp = (const zb_device_temperature_t *)state;

    cJSON *dev_temp = cJSON_CreateObject();
    if (dev_temp == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Current temperature */
    float current_temp = 0.0f;
    if (zb_device_temp_is_valid(temp->current_temperature)) {
        current_temp = zb_device_temp_to_celsius(temp->current_temperature);
        cJSON_AddNumberToObject(dev_temp, "current", current_temp);
    }

    /* Min/Max experienced temperatures */
    if (zb_device_temp_is_valid(temp->min_temp_experienced)) {
        float min_temp = zb_device_temp_to_celsius(temp->min_temp_experienced);
        cJSON_AddNumberToObject(dev_temp, "min_experienced", min_temp);
    }

    if (zb_device_temp_is_valid(temp->max_temp_experienced)) {
        float max_temp = zb_device_temp_to_celsius(temp->max_temp_experienced);
        cJSON_AddNumberToObject(dev_temp, "max_experienced", max_temp);
    }

    /* Alarm status */
    if (temp->over_temp_alarm != 0) {
        cJSON_AddNumberToObject(dev_temp, "over_temp_alarm", temp->over_temp_alarm);
    }

    /* Validity flag */
    cJSON_AddBoolToObject(dev_temp, "valid", temp->valid);

    cJSON_AddItemToObject(json, "device_temperature", dev_temp);
    cJSON_AddNumberToObject(json, "linkquality", get_device_link_quality(device));

    ESP_LOGI(TAG, "Published device temperature for %s: %.2fC",
             device->friendly_name, current_temp);

    return ESP_OK;
}

/**
 * @brief Build multistate JSON payload
 */
static esp_err_t build_multistate_json(const device_t *device, const void *state, cJSON *json)
{
    const zb_multistate_state_t *multistate = (const zb_multistate_state_t *)state;

    /* Add state (PresentValue) */
    cJSON_AddNumberToObject(json, "state", multistate->present_value);

    /* Add number of states if known */
    if (multistate->number_of_states > 0) {
        cJSON_AddNumberToObject(json, "states_count", multistate->number_of_states);
    }

    /* Add cluster type string */
    cJSON_AddStringToObject(json, "type", zb_multistate_type_to_string(multistate->type));

    /* Add endpoint if device has multiple */
    if (multistate->endpoint > 0) {
        cJSON_AddNumberToObject(json, "endpoint", multistate->endpoint);
    }

    /* Add out of service and status flags */
    cJSON_AddBoolToObject(json, "out_of_service", multistate->out_of_service);
    cJSON_AddBoolToObject(json, "in_alarm", zb_multistate_is_in_alarm(multistate));
    cJSON_AddBoolToObject(json, "fault", zb_multistate_has_fault(multistate));

    /* Add raw status flags for advanced users */
    if (multistate->status_flags != 0) {
        cJSON_AddNumberToObject(json, "status_flags", multistate->status_flags);
    }

    cJSON_AddNumberToObject(json, "linkquality", get_device_link_quality(device));

    ESP_LOGI(TAG, "Published multistate state for %s: state=%u (of %u), type=%s",
             device->friendly_name, multistate->present_value, multistate->number_of_states,
             zb_multistate_type_to_string(multistate->type));

    return ESP_OK;
}

/**
 * @brief Build binary output JSON payload
 */
static esp_err_t build_binary_output_json(const device_t *device, const void *state, cJSON *json)
{
    const zb_binary_output_state_t *binary_output = (const zb_binary_output_state_t *)state;

    cJSON_AddStringToObject(json, "state", binary_output->present_value ? "ON" : "OFF");
    cJSON_AddBoolToObject(json, "out_of_service", binary_output->out_of_service);
    cJSON_AddBoolToObject(json, "in_alarm", binary_output->in_alarm);
    cJSON_AddBoolToObject(json, "fault", binary_output->fault);
    cJSON_AddBoolToObject(json, "overridden", binary_output->overridden);

    if (binary_output->status_flags != 0) {
        cJSON_AddNumberToObject(json, "status_flags", binary_output->status_flags);
    }

    cJSON_AddNumberToObject(json, "linkquality", device->proto.zigbee.lqi);

    ESP_LOGI(TAG, "Published binary output state for %s: %s",
             device->friendly_name, binary_output->present_value ? "ON" : "OFF");

    return ESP_OK;
}

/**
 * @brief Build binary value JSON payload
 */
static esp_err_t build_binary_value_json(const device_t *device, const void *state, cJSON *json)
{
    const zb_binary_value_state_t *binary_value = (const zb_binary_value_state_t *)state;

    cJSON_AddStringToObject(json, "state", binary_value->present_value ? "ON" : "OFF");
    cJSON_AddBoolToObject(json, "out_of_service", binary_value->out_of_service);
    cJSON_AddBoolToObject(json, "in_alarm", binary_value->in_alarm);
    cJSON_AddBoolToObject(json, "fault", binary_value->fault);
    cJSON_AddBoolToObject(json, "overridden", binary_value->overridden);

    if (binary_value->status_flags != 0) {
        cJSON_AddNumberToObject(json, "status_flags", binary_value->status_flags);
    }

    cJSON_AddNumberToObject(json, "linkquality", device->proto.zigbee.lqi);

    ESP_LOGI(TAG, "Published binary value state for %s: %s",
             device->friendly_name, binary_value->present_value ? "ON" : "OFF");

    return ESP_OK;
}

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

/**
 * @brief Handle EVT_DEVICE_INTERVIEWED to publish battery state
 *
 * When a device interview completes successfully, publish battery state
 * if the device is battery-powered. This replaces the direct cross-layer
 * call from zb_interview.c.
 */
static void handle_device_interviewed(event_type_t type, void *data,
                                       size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

    if (!data || data_size < sizeof(evt_device_interviewed_t)) {
        return;
    }

    const evt_device_interviewed_t *evt = (const evt_device_interviewed_t *)data;
    if (!evt->success) {
        return;
    }

    device_t *dev = device_registry_get(evt->ieee_addr);
    if (dev && dev->proto.zigbee.power_info.power_info_valid) {
        esp_err_t ret = device_state_publish_battery(evt->short_addr);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Published battery state for 0x%04X after interview",
                     evt->short_addr);
        }
    }
}

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

esp_err_t device_state_publisher_init(void)
{
    event_subscribe(EVT_DEVICE_INTERVIEWED, handle_device_interviewed, NULL);
    ESP_LOGI(TAG, "Device state publisher initialized");
    return ESP_OK;
}

esp_err_t device_state_publish(const device_t *device)
{
    if (device == NULL) {
        ESP_LOGE(TAG, "Device pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    /* Build state JSON */
    char *json_str = device_state_to_json(device);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to create state JSON for device %s", device->friendly_name);
        return ESP_FAIL;
    }

    /* Publish via event bus - MQTT adapter will handle actual MQTT publishing */
    esp_err_t ret = publish_state_event(device, json_str, 0, 0);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Published state event for device %s", device->friendly_name);
    }

    free(json_str);
    return ret;
}

esp_err_t device_state_publish_by_addr(uint16_t short_addr)
{
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        ESP_LOGE(TAG, "Device 0x%04X not found", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    return device_state_publish(device);
}

esp_err_t device_state_publish_availability(const device_t *device, bool available)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build availability JSON: {"state": "online"} or {"state": "offline"} */
    char json_state[64];
    snprintf(json_state, sizeof(json_state), "{\"state\":\"%s\"}",
             available ? "online" : "offline");

    /* Publish via event bus - MQTT adapter will handle actual MQTT publishing */
    esp_err_t ret = publish_state_event(device, json_state, 0, 0);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Published availability event %s for device %s",
                 available ? "online" : "offline", device->friendly_name);
    }

    return ret;
}

esp_err_t device_state_publish_availability_by_addr(uint16_t short_addr, bool available)
{
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        ESP_LOGE(TAG, "Device 0x%04X not found", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    return device_state_publish_availability(device, available);
}

esp_err_t device_state_publish_attribute(uint16_t short_addr,
                                        uint16_t cluster_id,
                                        uint16_t attr_id,
                                        const void *value,
                                        size_t value_len)
{
    if (value == NULL || value_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        ESP_LOGW(TAG, "%s: Device 0x%04X not found", __func__, short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    /* Build attribute-specific JSON */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_FAIL;
    }

    /* Handle specific cluster/attribute combinations */
    switch (cluster_id) {
        case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
            if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && value_len == 1) {
                bool on_off = *(bool *)value;
                cJSON_AddStringToObject(root, "state", on_off ? "ON" : "OFF");
            }
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL:
            if (attr_id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID && value_len == 1) {
                uint8_t level = *(uint8_t *)value;
                cJSON_AddNumberToObject(root, "brightness", level);
            }
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
            if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID && value_len == 2) {
                uint16_t x = *(uint16_t *)value;
                cJSON *color = cJSON_CreateObject();
                if (color == NULL) {
                    ESP_LOGE(TAG, "Failed to create color JSON object");
                    cJSON_Delete(root);
                    return ESP_ERR_NO_MEM;
                }
                cJSON_AddNumberToObject(color, "x", (double)x / 65535.0);
                cJSON_AddItemToObject(root, "color", color);
            } else if (attr_id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID && value_len == 2) {
                uint16_t y = *(uint16_t *)value;
                cJSON *color = cJSON_CreateObject();
                if (color == NULL) {
                    ESP_LOGE(TAG, "Failed to create color JSON object");
                    cJSON_Delete(root);
                    return ESP_ERR_NO_MEM;
                }
                cJSON_AddNumberToObject(color, "y", (double)y / 65535.0);
                cJSON_AddItemToObject(root, "color", color);
            }
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT:
            if (attr_id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID && value_len == 2) {
                int16_t temp_raw = *(int16_t *)value;
                double temp = (double)temp_raw / (double)ZCL_TEMP_SCALE;
                cJSON_AddNumberToObject(root, "temperature", temp);
            }
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:
            if (attr_id == ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID && value_len == 2) {
                uint16_t humidity_raw = *(uint16_t *)value;
                double humidity = (double)humidity_raw / (double)ZCL_HUMIDITY_SCALE;
                cJSON_AddNumberToObject(root, "humidity", humidity);
            }
            break;

        case ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING:
            if (attr_id == ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID && value_len == 1) {
                bool occupied = *(uint8_t *)value & 0x01;
                cJSON_AddBoolToObject(root, "occupancy", occupied);
            }
            break;

        case ZB_ZCL_CLUSTER_ID_DOOR_LOCK:
            if (attr_id == ZB_ZCL_ATTR_DOOR_LOCK_LOCK_STATE_ID && value_len >= 1) {
                uint8_t lock_state = *(uint8_t *)value;
                /* Zigbee2MQTT format: {"state": "LOCK"} or {"state": "UNLOCK"} */
                if (lock_state == ZB_DOOR_LOCK_STATE_LOCKED) {
                    cJSON_AddStringToObject(root, "state", "LOCK");
                } else if (lock_state == ZB_DOOR_LOCK_STATE_UNLOCKED) {
                    cJSON_AddStringToObject(root, "state", "UNLOCK");
                } else {
                    /* Not fully locked state */
                    cJSON_AddStringToObject(root, "state", "UNLOCK");
                }
                ESP_LOGI(TAG, "Door lock 0x%04X state: %s",
                         short_addr, (lock_state == ZB_DOOR_LOCK_STATE_LOCKED) ? "LOCK" : "UNLOCK");
            }
            break;

        case ZB_ZCL_CLUSTER_ID_DEVICE_TEMP_CONFIG:
            /* Device Temperature Configuration Cluster (0x0002) */
            /* Handle device temperature report and update cache */
            zb_diagnostics_handle_device_temp_report(short_addr, attr_id, value, value_len);

            /* Add device temperature to JSON response */
            if (attr_id == ZB_DEVICE_TEMP_ATTR_CURRENT && value_len >= 2) {
                int16_t raw_temp = *(int16_t *)value;
                if (zb_device_temp_is_valid(raw_temp)) {
                    float temp_celsius = zb_device_temp_to_celsius(raw_temp);
                    cJSON *dev_temp = cJSON_CreateObject();
                    if (dev_temp != NULL) {
                        cJSON_AddNumberToObject(dev_temp, "current", temp_celsius);
                        cJSON_AddBoolToObject(dev_temp, "valid", true);
                        cJSON_AddItemToObject(root, "device_temperature", dev_temp);
                        ESP_LOGI(TAG, "Device 0x%04X internal temperature: %.2fC",
                                 short_addr, temp_celsius);
                    }
                }
            }
            break;

        case ZB_ZCL_CLUSTER_ID_BINARY_OUTPUT:
            /* Binary Output cluster (0x0010) - Relays, Schaltaktoren */
            if (attr_id == ZB_ZCL_ATTR_BINARY_PRESENT_VALUE_ID && value_len >= 1) {
                bool state_val = *(uint8_t *)value != 0;
                cJSON_AddStringToObject(root, "state", state_val ? "ON" : "OFF");
                ESP_LOGI(TAG, "Binary Output 0x%04X: state=%s",
                         short_addr, state_val ? "ON" : "OFF");
            }
            break;

        case ZB_ZCL_CLUSTER_ID_BINARY_VALUE:
            /* Binary Value cluster (0x0011) - Generic binary state */
            if (attr_id == ZB_ZCL_ATTR_BINARY_PRESENT_VALUE_ID && value_len >= 1) {
                bool state_val = *(uint8_t *)value != 0;
                cJSON_AddStringToObject(root, "state", state_val ? "ON" : "OFF");
                ESP_LOGI(TAG, "Binary Value 0x%04X: state=%s",
                         short_addr, state_val ? "ON" : "OFF");
            }
            break;

        default:
            ESP_LOGD(TAG, "Unhandled cluster/attribute: 0x%04X/0x%04X", cluster_id, attr_id);
            cJSON_Delete(root);
            return ESP_OK; /* Not an error, just not handled */
    }

    /* Convert to JSON string */
    char *json_str = json_to_string_and_delete(root);
    if (json_str == NULL) {
        return ESP_FAIL;
    }

    /* Publish via event bus - MQTT adapter will handle actual MQTT publishing */
    esp_err_t ret = publish_state_event(device, json_str, cluster_id, attr_id);
    free(json_str);

    return ret;
}

/**
 * @brief Publish availability for all devices
 *
 * Uses collect-then-iterate pattern to avoid holding registry mutex
 * during MQTT event publishing (deadlock risk per AP-1.3).
 */
esp_err_t device_state_publish_all_availability(bool available)
{
    device_id_t ids[DEVICE_REGISTRY_MAX_DEVICES];
    size_t count;
    if (device_registry_collect_ids(ids, DEVICE_REGISTRY_MAX_DEVICES, &count) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to collect device IDs for availability publish");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Publishing availability %s for %zu devices",
             available ? "online" : "offline", count);

    for (size_t i = 0; i < count; i++) {
        device_t *dev = device_registry_get(ids[i]);
        if (dev != NULL) {
            device_state_publish_availability(dev, available);
        }
    }

    return ESP_OK;
}

char* device_state_to_json(const device_t *device)
{
    if (device == NULL) {
        return NULL;
    }

    cJSON *root = json_create_device_state_ng(device);
    if (root == NULL) {
        return NULL;
    }

    return json_to_string_and_delete(root);
}

esp_err_t device_state_publish_lock_state(uint16_t short_addr, bool locked)
{
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        ESP_LOGE(TAG, "Device 0x%04X not found", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    /* Create lock state JSON: {"state": "LOCK"} or {"state": "UNLOCK"} */
    char *json_str = json_create_lock_state(locked);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to create lock state JSON");
        return ESP_FAIL;
    }

    /* Publish via event bus - MQTT adapter will handle actual MQTT publishing */
    esp_err_t ret = publish_state_event(device, json_str, ZB_ZCL_CLUSTER_ID_DOOR_LOCK,
                                        ZB_ZCL_ATTR_DOOR_LOCK_LOCK_STATE_ID);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Published lock state event for %s: %s",
                 device->friendly_name, locked ? "LOCK" : "UNLOCK");
    }

    free(json_str);
    return ret;
}

esp_err_t device_state_publish_electrical(uint16_t short_addr,
                                           const zb_electrical_state_t *state)
{
    return generic_device_state_publish(short_addr, state, build_electrical_json,
                                        "electrical");
}

esp_err_t device_state_publish_metering(uint16_t short_addr,
                                         const zb_metering_state_t *state)
{
    return generic_device_state_publish(short_addr, state, build_metering_json,
                                        "metering");
}

esp_err_t device_state_publish_device_temperature(uint16_t short_addr,
                                                   const zb_device_temperature_t *temp)
{
    return generic_device_state_publish(short_addr, temp, build_device_temperature_json,
                                        "device_temperature");
}

esp_err_t device_state_publish_battery(uint16_t short_addr)
{
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        ESP_LOGE(TAG, "Device 0x%04X not found", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    /* Check if power info is valid */
    if (!device->proto.zigbee.power_info.power_info_valid) {
        ESP_LOGW(TAG, "Device 0x%04X has no valid power info", short_addr);
        return ESP_ERR_INVALID_STATE;
    }

    /* Create battery state JSON */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_FAIL;
    }

    /* Check if device is battery powered */
    uint8_t source = device->proto.zigbee.power_info.current_power_source;
    bool is_battery = (source & ZB_POWER_SOURCE_RECHARGEABLE_BATTERY) ||
                      (source & ZB_POWER_SOURCE_DISPOSABLE_BATTERY);

    if (is_battery) {
        uint8_t battery_pct = device_power_level_to_percent(
            device->proto.zigbee.power_info.current_power_source_level);
        cJSON_AddNumberToObject(root, "battery", battery_pct);

        /* Add battery_low indicator (level 0 = critical) */
        bool battery_low =
            (device->proto.zigbee.power_info.current_power_source_level == 0);
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

    /* Add link quality for consistency with other state messages */
    cJSON_AddNumberToObject(root, "linkquality", device->proto.zigbee.lqi);

    /* Convert to string */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize battery state JSON");
        return ESP_FAIL;
    }

    /* Publish via event bus - MQTT adapter will handle actual MQTT publishing */
    esp_err_t ret = publish_state_event(device, json_str, ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG, 0);
    if (ret == ESP_OK) {
        if (is_battery) {
            ESP_LOGI(TAG, "Published battery state event for %s: %d%% (%s)",
                     device->friendly_name,
                     device_power_level_to_percent(device->proto.zigbee.power_info.current_power_source_level),
                     power_source_str);
        } else {
            ESP_LOGI(TAG, "Published power state event for %s: %s",
                     device->friendly_name, power_source_str);
        }
    }

    free(json_str);
    return ret;
}

esp_err_t device_state_publish_multistate(uint16_t short_addr, uint8_t endpoint,
                                           const zb_multistate_state_t *state)
{
    /* Note: endpoint parameter kept for API compatibility, but state->endpoint is used */
    (void)endpoint;  /* Use state->endpoint instead */
    return generic_device_state_publish(short_addr, state, build_multistate_json,
                                        "multistate");
}

esp_err_t device_state_publish_binary_output(uint16_t short_addr,
                                              const zb_binary_output_state_t *state)
{
    return generic_device_state_publish(short_addr, state, build_binary_output_json,
                                        "binary_output");
}

esp_err_t device_state_publish_binary_value(uint16_t short_addr,
                                             const zb_binary_value_state_t *state)
{
    return generic_device_state_publish(short_addr, state, build_binary_value_json,
                                        "binary_value");
}

esp_err_t device_state_publish_fingerbot(uint16_t short_addr)
{
    /* Delegate to generic Tuya driver state publisher */
    return device_state_publish_tuya(short_addr);
}

esp_err_t device_state_publisher_test(void)
{
    ESP_LOGI(TAG, "Running device state publisher test...");

    /* Create a mock device using device_t */
    device_t mock_device = {0};
    mock_device.id = 0x00124B001234ABCDULL;
    mock_device.protocol = DEV_PROTOCOL_ZIGBEE;
    mock_device.availability = DEV_AVAIL_ONLINE;
    mock_device.proto.zigbee.short_addr = 0x1234;
    mock_device.proto.zigbee.endpoint = 1;
    mock_device.proto.zigbee.lqi = GW_DEFAULT_LINK_QUALITY_TEST;
    mock_device.proto.zigbee.rssi = -50;
    mock_device.proto.zigbee.cluster_count = 2;
    mock_device.proto.zigbee.clusters[0] = ESP_ZB_ZCL_CLUSTER_ID_BASIC;
    mock_device.proto.zigbee.clusters[1] = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
    mock_device.proto.zigbee.device_type = DEVICE_ZB_TYPE_LIGHT;
    mock_device.last_seen = (uint32_t)time(NULL);
    strlcpy(mock_device.friendly_name, "test_light", sizeof(mock_device.friendly_name));
    strlcpy(mock_device.model, "TEST-001", sizeof(mock_device.model));
    strlcpy(mock_device.manufacturer, "TestCorp", sizeof(mock_device.manufacturer));

    /* Test 1: Create JSON */
    char *json_str = device_state_to_json(&mock_device);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to create device state JSON");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Device state JSON: %s", json_str);
    free(json_str);

    /* Test 2: Publish state via event bus */
    esp_err_t ret = device_state_publish(&mock_device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to publish device state event");
        return ESP_FAIL;
    }

    /* Test 3: Publish availability via event bus */
    ret = device_state_publish_availability(&mock_device, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to publish availability event");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Device state publisher test PASSED");
    return ESP_OK;
}
