/**
 * @file esphome_adapter.c
 * @brief ESPHome API Adapter Implementation
 *
 * Bridges the event bus and device registry with the ESPHome Native API,
 * enabling Home Assistant integration via the ESPHome protocol.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_adapter.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "esphome/esphome_api.h"
#include "esphome/esphome_entities.h"
#include "esphome/esphome_common.h"
#include "zigbee/converter/zb_converter.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#include "adapter_interface.h"

static const char *TAG = "ESPH_ADAPTER";

/* ============================================================================
 * Static State
 * ============================================================================ */

static bool s_initialized = false;
static esphome_adapter_stats_t s_stats = {0};

/* ============================================================================
 * Forward Declarations - Event Handlers
 * ============================================================================ */

static void handle_device_state_changed(event_type_t type, void *data,
                                         size_t data_size, void *ctx);
static void handle_device_joined(event_type_t type, void *data,
                                  size_t data_size, void *ctx);
static void handle_device_left(event_type_t type, void *data,
                                size_t data_size, void *ctx);
static void handle_device_availability_changed(event_type_t type, void *data,
                                                size_t data_size, void *ctx);

/* ============================================================================
 * Forward Declarations - Entity Registration
 * ============================================================================ */

static esp_err_t register_sensor_entity(const device_t *dev, device_capability_t cap);
static esp_err_t register_binary_sensor_entity(const device_t *dev, device_capability_t cap);
static esp_err_t register_switch_entity(const device_t *dev);
static esp_err_t register_light_entity(const device_t *dev);

/* ============================================================================
 * Forward Declarations - Command Callbacks
 * ============================================================================ */

static esp_err_t switch_command_callback(esphome_entity_key_t key, bool state);
static esp_err_t light_command_callback(esphome_entity_key_t key,
                                         const esphome_light_command_t *cmd);

/* ============================================================================
 * Helper Functions - Entity Key Management
 * ============================================================================ */

uint32_t esphome_adapter_make_entity_key(device_id_t id, device_capability_t cap)
{
    /*
     * Key structure (32 bits):
     * - Bits 31-28: Adapter identifier (0x1 to distinguish from other entities)
     * - Bits 27-24: Capability type (0-15)
     * - Bits 23-0:  Lower 24 bits of device ID hash
     */
    uint32_t id_hash = (uint32_t)(id ^ (id >> 32)); /* Simple hash of 64-bit ID */
    uint32_t key = ESPHOME_ADAPTER_KEY_OFFSET;
    key |= ((uint32_t)cap & 0x0F) << 24;
    key |= (id_hash & 0x00FFFFFF);
    return key;
}

esp_err_t esphome_adapter_parse_entity_key(uint32_t key, device_id_t *id,
                                            device_capability_t *cap)
{
    /* Check if key is from this adapter */
    if ((key & 0xF0000000) != ESPHOME_ADAPTER_KEY_OFFSET) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cap) {
        *cap = (device_capability_t)((key >> 24) & 0x0F);
    }

    /*
     * Note: We cannot fully reconstruct device_id from the key since we only
     * stored a hash. The caller should use device_registry iteration to find
     * the device by matching entity keys.
     */
    if (id) {
        *id = 0; /* Cannot reconstruct - use device iteration */
    }

    return ESP_OK;
}

/* ============================================================================
 * Helper Functions - Device Lookup from Entity Key
 * ============================================================================ */

/**
 * @brief Context for device lookup by entity key
 */
typedef struct {
    esphome_entity_key_t target_key;
    device_capability_t cap;
    device_t *result;
} find_device_ctx_t;

/**
 * @brief Iterator callback to find device matching an entity key
 */
static bool find_device_by_key_cb(device_t *dev, void *ctx)
{
    find_device_ctx_t *fc = (find_device_ctx_t *)ctx;
    uint32_t candidate_key = esphome_adapter_make_entity_key(dev->id, fc->cap);
    if (candidate_key == fc->target_key) {
        fc->result = dev;
        return false; /* Stop iteration — found it */
    }
    return true; /* Continue */
}

/**
 * @brief Find the device_t for a given ESPHome entity key
 *
 * Iterates the Zigbee device registry, regenerating the entity key for each
 * device+capability pair until a match is found. O(n) but n is small (<50).
 *
 * @param[in] key Entity key from ESPHome command
 * @param[out] out_cap Extracted capability type (optional, may be NULL)
 * @return Pointer to device_t or NULL if not found
 */
static device_t *find_device_for_entity_key(esphome_entity_key_t key,
                                              device_capability_t *out_cap)
{
    device_capability_t cap = 0;
    esphome_adapter_parse_entity_key(key, NULL, &cap);

    if (out_cap) {
        *out_cap = cap;
    }

    find_device_ctx_t ctx = {
        .target_key = key,
        .cap = cap,
        .result = NULL,
    };
    device_registry_iterate_zigbee(find_device_by_key_cb, &ctx);
    return ctx.result;
}

/* ============================================================================
 * Helper Functions - Name Generation
 * ============================================================================ */

/**
 * @brief Generate entity name from device and capability
 *
 * Format: "<friendly_name> <capability_suffix>"
 * Example: "Kitchen Sensor Temperature"
 */
static void make_entity_name(const device_t *dev, device_capability_t cap,
                              char *buf, size_t buf_size)
{
    const char *suffix = "";
    switch (cap) {
        case DEV_CAP_TEMPERATURE:   suffix = "Temperature"; break;
        case DEV_CAP_HUMIDITY:      suffix = "Humidity"; break;
        case DEV_CAP_PRESSURE:      suffix = "Pressure"; break;
        case DEV_CAP_BATTERY:       suffix = "Battery"; break;
        case DEV_CAP_MOTION:        suffix = "Motion"; break;
        case DEV_CAP_CONTACT:       suffix = "Contact"; break;
        case DEV_CAP_ON_OFF:        suffix = "Switch"; break;
        case DEV_CAP_BRIGHTNESS:    suffix = "Light"; break;
        case DEV_CAP_POWER:         suffix = "Power"; break;
        case DEV_CAP_ENERGY:        suffix = "Energy"; break;
        case DEV_CAP_VOLTAGE:       suffix = "Voltage"; break;
        case DEV_CAP_CURRENT:       suffix = "Current"; break;
        default:                    suffix = "Entity"; break;
    }

    const char *name = dev->friendly_name[0] ? dev->friendly_name : "Device";
    snprintf(buf, buf_size, "%s %s", name, suffix);
}

/**
 * @brief Generate unique ID from device and capability
 *
 * Format: "<ieee_addr_hex>_<capability_name>"
 * Example: "0x00158d0001234567_temperature"
 */
static void make_unique_id(const device_t *dev, device_capability_t cap,
                            char *buf, size_t buf_size)
{
    const char *cap_str = "";
    switch (cap) {
        case DEV_CAP_TEMPERATURE:   cap_str = "temperature"; break;
        case DEV_CAP_HUMIDITY:      cap_str = "humidity"; break;
        case DEV_CAP_PRESSURE:      cap_str = "pressure"; break;
        case DEV_CAP_BATTERY:       cap_str = "battery"; break;
        case DEV_CAP_MOTION:        cap_str = "motion"; break;
        case DEV_CAP_CONTACT:       cap_str = "contact"; break;
        case DEV_CAP_ON_OFF:        cap_str = "switch"; break;
        case DEV_CAP_BRIGHTNESS:    cap_str = "light"; break;
        case DEV_CAP_POWER:         cap_str = "power"; break;
        case DEV_CAP_ENERGY:        cap_str = "energy"; break;
        case DEV_CAP_VOLTAGE:       cap_str = "voltage"; break;
        case DEV_CAP_CURRENT:       cap_str = "current"; break;
        default:                    cap_str = "entity"; break;
    }

    snprintf(buf, buf_size, "0x%016llX_%s", (unsigned long long)dev->id, cap_str);
}

/* ============================================================================
 * Helper Functions - State Extraction from JSON
 * ============================================================================ */

/**
 * @brief Extract float value from device state JSON
 */
static bool get_state_float(cJSON *state, const char *key, float *value)
{
    if (!state || !key || !value) {
        return false;
    }

    cJSON *item = cJSON_GetObjectItem(state, key);
    if (item && cJSON_IsNumber(item)) {
        *value = (float)item->valuedouble;
        return true;
    }
    return false;
}

/**
 * @brief Extract boolean value from device state JSON
 */
static bool get_state_bool(cJSON *state, const char *key, bool *value)
{
    if (!state || !key || !value) {
        return false;
    }

    cJSON *item = cJSON_GetObjectItem(state, key);
    if (item) {
        if (cJSON_IsBool(item)) {
            *value = cJSON_IsTrue(item);
            return true;
        }
        /* Also handle "ON"/"OFF" strings */
        if (cJSON_IsString(item)) {
            const char *str = item->valuestring;
            if (strcasecmp(str, "ON") == 0 || strcasecmp(str, "true") == 0) {
                *value = true;
                return true;
            } else if (strcasecmp(str, "OFF") == 0 || strcasecmp(str, "false") == 0) {
                *value = false;
                return true;
            }
        }
    }
    return false;
}

/* ============================================================================
 * Sensor Entity Registration
 * ============================================================================ */

static esp_err_t register_sensor_entity(const device_t *dev, device_capability_t cap)
{
    esphome_sensor_config_t config = {0};
    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);

    config.key = esphome_adapter_make_entity_key(dev->id, cap);
    config.device_id = device_id;
    make_entity_name(dev, cap, config.name, sizeof(config.name));
    make_unique_id(dev, cap, config.unique_id, sizeof(config.unique_id));

    /* Set device class and unit based on capability */
    switch (cap) {
        case DEV_CAP_TEMPERATURE:
            config.device_class = ESPHOME_SENSOR_CLASS_TEMPERATURE;
            strncpy(config.unit_of_measurement, "C", sizeof(config.unit_of_measurement) - 1);
            strncpy(config.icon, "mdi:thermometer", sizeof(config.icon) - 1);
            config.accuracy_decimals = 1;
            break;

        case DEV_CAP_HUMIDITY:
            config.device_class = ESPHOME_SENSOR_CLASS_HUMIDITY;
            strncpy(config.unit_of_measurement, "%", sizeof(config.unit_of_measurement) - 1);
            strncpy(config.icon, "mdi:water-percent", sizeof(config.icon) - 1);
            config.accuracy_decimals = 1;
            break;

        case DEV_CAP_PRESSURE:
            config.device_class = ESPHOME_SENSOR_CLASS_PRESSURE;
            strncpy(config.unit_of_measurement, "hPa", sizeof(config.unit_of_measurement) - 1);
            strncpy(config.icon, "mdi:gauge", sizeof(config.icon) - 1);
            config.accuracy_decimals = 0;
            break;

        case DEV_CAP_BATTERY:
            config.device_class = ESPHOME_SENSOR_CLASS_BATTERY;
            strncpy(config.unit_of_measurement, "%", sizeof(config.unit_of_measurement) - 1);
            strncpy(config.icon, "mdi:battery", sizeof(config.icon) - 1);
            config.accuracy_decimals = 0;
            config.entity_category = 2; /* DIAGNOSTIC */
            break;

        case DEV_CAP_POWER:
            config.device_class = ESPHOME_SENSOR_CLASS_POWER;
            strncpy(config.unit_of_measurement, "W", sizeof(config.unit_of_measurement) - 1);
            strncpy(config.icon, "mdi:flash", sizeof(config.icon) - 1);
            config.accuracy_decimals = 1;
            break;

        case DEV_CAP_ENERGY:
            config.device_class = ESPHOME_SENSOR_CLASS_ENERGY;
            strncpy(config.unit_of_measurement, "kWh", sizeof(config.unit_of_measurement) - 1);
            strncpy(config.icon, "mdi:lightning-bolt", sizeof(config.icon) - 1);
            config.accuracy_decimals = 2;
            config.state_class = ESPHOME_STATE_CLASS_TOTAL_INCREASING;
            break;

        case DEV_CAP_VOLTAGE:
            config.device_class = ESPHOME_SENSOR_CLASS_VOLTAGE;
            strncpy(config.unit_of_measurement, "V", sizeof(config.unit_of_measurement) - 1);
            strncpy(config.icon, "mdi:sine-wave", sizeof(config.icon) - 1);
            config.accuracy_decimals = 1;
            break;

        case DEV_CAP_CURRENT:
            config.device_class = ESPHOME_SENSOR_CLASS_CURRENT;
            strncpy(config.unit_of_measurement, "A", sizeof(config.unit_of_measurement) - 1);
            strncpy(config.icon, "mdi:current-ac", sizeof(config.icon) - 1);
            config.accuracy_decimals = 2;
            break;

        default:
            config.device_class = ESPHOME_SENSOR_CLASS_NONE;
            config.accuracy_decimals = 1;
            break;
    }

    config.state_class = ESPHOME_STATE_CLASS_MEASUREMENT;
    config.force_update = false;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_sensor(&config);
    if (ret == ESP_OK) {
        s_stats.entities_registered++;
        ESP_LOGD(TAG, "Registered sensor: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    } else if (ret == ESP_ERR_NO_MEM) {
        /* Entity might already exist - not an error */
        ESP_LOGD(TAG, "Sensor already exists or max reached: %s", config.name);
    } else {
        ESP_LOGW(TAG, "Failed to register sensor %s: %s",
                 config.name, esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================================
 * Binary Sensor Entity Registration
 * ============================================================================ */

static esp_err_t register_binary_sensor_entity(const device_t *dev, device_capability_t cap)
{
    esphome_binary_sensor_config_t config = {0};
    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);

    config.key = esphome_adapter_make_entity_key(dev->id, cap);
    config.device_id = device_id;
    make_entity_name(dev, cap, config.name, sizeof(config.name));
    make_unique_id(dev, cap, config.unique_id, sizeof(config.unique_id));

    /* Set device class based on capability */
    switch (cap) {
        case DEV_CAP_MOTION:
            config.device_class = ESPHOME_BINARY_CLASS_MOTION;
            strncpy(config.icon, "mdi:motion-sensor", sizeof(config.icon) - 1);
            break;

        case DEV_CAP_CONTACT:
            config.device_class = ESPHOME_BINARY_CLASS_DOOR;
            strncpy(config.icon, "mdi:door", sizeof(config.icon) - 1);
            break;

        case DEV_CAP_VIBRATION:
            config.device_class = ESPHOME_BINARY_CLASS_VIBRATION;
            strncpy(config.icon, "mdi:vibrate", sizeof(config.icon) - 1);
            break;

        case DEV_CAP_WATER_LEAK:
            config.device_class = ESPHOME_BINARY_CLASS_MOISTURE;
            strncpy(config.icon, "mdi:water-alert", sizeof(config.icon) - 1);
            break;

        case DEV_CAP_SMOKE:
            config.device_class = ESPHOME_BINARY_CLASS_SMOKE;
            strncpy(config.icon, "mdi:smoke-detector", sizeof(config.icon) - 1);
            break;

        default:
            config.device_class = ESPHOME_BINARY_CLASS_NONE;
            break;
    }

    config.is_status_binary_sensor = false;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_binary_sensor(&config);
    if (ret == ESP_OK) {
        s_stats.entities_registered++;
        ESP_LOGD(TAG, "Registered binary sensor: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    } else if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGD(TAG, "Binary sensor already exists or max reached: %s", config.name);
    } else {
        ESP_LOGW(TAG, "Failed to register binary sensor %s: %s",
                 config.name, esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================================
 * Switch Entity Registration
 * ============================================================================ */

static esp_err_t switch_command_callback(esphome_entity_key_t key, bool state)
{
    ESP_LOGI(TAG, "Switch command: key=0x%08lX, state=%s",
             (unsigned long)key, state ? "ON" : "OFF");

    s_stats.commands_received++;

    /* Find the Zigbee device for this entity key */
    device_t *dev = find_device_for_entity_key(key, NULL);
    if (!dev) {
        ESP_LOGW(TAG, "Switch command: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    /* Build JSON command like MQTT command handler expects */
    cJSON *cmd = cJSON_CreateObject();
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(cmd, "state", state ? "ON" : "OFF");

    /* Route to Zigbee converter chain */
    uint8_t ep = dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1; /* EP0 is ZDO, default to EP1 */

    esp_err_t ret = zb_converter_handle_command(
        dev->proto.zigbee.short_addr, ep, cmd);
    cJSON_Delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Switch command failed for 0x%04X: %s",
                 dev->proto.zigbee.short_addr, esp_err_to_name(ret));
    }

    /* Optimistic state update — real state comes via EVT_DEVICE_STATE_CHANGED */
    esphome_entity_update_switch(key, state);
    return ret;
}

static esp_err_t register_switch_entity(const device_t *dev)
{
    esphome_switch_config_t config = {0};
    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);

    config.key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_ON_OFF);
    config.device_id = device_id;
    make_entity_name(dev, DEV_CAP_ON_OFF, config.name, sizeof(config.name));
    make_unique_id(dev, DEV_CAP_ON_OFF, config.unique_id, sizeof(config.unique_id));

    strncpy(config.icon, "mdi:power", sizeof(config.icon) - 1);
    config.assumed_state = false;
    config.disabled_by_default = false;
    config.command_callback = switch_command_callback;

    esp_err_t ret = esphome_entity_register_switch(&config);
    if (ret == ESP_OK) {
        s_stats.entities_registered++;
        ESP_LOGD(TAG, "Registered switch: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    } else if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGD(TAG, "Switch already exists or max reached: %s", config.name);
    } else {
        ESP_LOGW(TAG, "Failed to register switch %s: %s",
                 config.name, esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================================
 * Light Entity Registration
 * ============================================================================ */

static esp_err_t light_command_callback(esphome_entity_key_t key,
                                         const esphome_light_command_t *cmd)
{
    ESP_LOGI(TAG, "Light command: key=0x%08lX, state=%s, brightness=%.2f, color_temp=%.0f",
             (unsigned long)key,
             cmd->has_state ? (cmd->state ? "ON" : "OFF") : "-",
             cmd->has_brightness ? cmd->brightness : -1.0f,
             cmd->has_color_temp ? cmd->color_temp : -1.0f);

    s_stats.commands_received++;

    /* Find the Zigbee device for this entity key */
    device_t *dev = find_device_for_entity_key(key, NULL);
    if (!dev) {
        ESP_LOGW(TAG, "Light command: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    /* Build JSON command matching Zigbee2MQTT format */
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    if (cmd->has_state) {
        cJSON_AddStringToObject(json, "state", cmd->state ? "ON" : "OFF");
    }
    if (cmd->has_brightness) {
        /* ESPHome brightness is 0.0-1.0, Zigbee2MQTT expects 0-254 */
        cJSON_AddNumberToObject(json, "brightness", (int)(cmd->brightness * 254.0f));
    }
    if (cmd->has_color_temp) {
        cJSON_AddNumberToObject(json, "color_temp", (int)cmd->color_temp);
    }
    if (cmd->has_rgb) {
        /* Convert ESPHome 0.0-1.0 RGB to Z2M color object */
        cJSON *color = cJSON_CreateObject();
        if (color) {
            cJSON_AddNumberToObject(color, "r", (int)(cmd->red * 255.0f));
            cJSON_AddNumberToObject(color, "g", (int)(cmd->green * 255.0f));
            cJSON_AddNumberToObject(color, "b", (int)(cmd->blue * 255.0f));
            cJSON_AddItemToObject(json, "color", color);
        }
    }
    if (cmd->has_transition_length) {
        /* ESPHome transition in ms, Z2M expects seconds */
        cJSON_AddNumberToObject(json, "transition", cmd->transition_length / 1000.0f);
    }

    /* Route to Zigbee converter chain */
    uint8_t ep = dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    esp_err_t ret = zb_converter_handle_command(
        dev->proto.zigbee.short_addr, ep, json);
    cJSON_Delete(json);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Light command failed for 0x%04X: %s",
                 dev->proto.zigbee.short_addr, esp_err_to_name(ret));
    }

    /* Optimistic state update */
    esphome_light_state_t state = {0};
    state.key = key;
    esphome_entity_get_light(key, &state);

    if (cmd->has_state) state.state = cmd->state;
    if (cmd->has_brightness) state.brightness = cmd->brightness;
    if (cmd->has_color_temp) state.color_temp = cmd->color_temp;
    if (cmd->has_rgb) {
        state.red = cmd->red;
        state.green = cmd->green;
        state.blue = cmd->blue;
    }
    esphome_entity_update_light(key, &state);

    return ret;
}

static esp_err_t register_light_entity(const device_t *dev)
{
    esphome_light_config_t config = {0};
    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);

    config.key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_BRIGHTNESS);
    config.device_id = device_id;
    make_entity_name(dev, DEV_CAP_BRIGHTNESS, config.name, sizeof(config.name));
    make_unique_id(dev, DEV_CAP_BRIGHTNESS, config.unique_id, sizeof(config.unique_id));

    strncpy(config.icon, "mdi:lightbulb", sizeof(config.icon) - 1);

    /* Set supported color modes based on capabilities */
    config.supported_color_modes = ESPHOME_COLOR_MODE_ON_OFF | ESPHOME_COLOR_MODE_BRIGHTNESS;

    if (dev->capabilities & DEV_CAP_COLOR_TEMP) {
        config.supported_color_modes |= ESPHOME_COLOR_MODE_COLOR_TEMP;
        config.min_mireds = 153;  /* ~6500K cool white */
        config.max_mireds = 500;  /* ~2000K warm white */
    }

    if (dev->capabilities & DEV_CAP_COLOR_XY) {
        config.supported_color_modes |= ESPHOME_COLOR_MODE_RGB;
    }

    config.effect_count = 0;
    config.command_callback = light_command_callback;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_light(&config);
    if (ret == ESP_OK) {
        s_stats.entities_registered++;
        ESP_LOGD(TAG, "Registered light: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    } else if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGD(TAG, "Light already exists or max reached: %s", config.name);
    } else {
        ESP_LOGW(TAG, "Failed to register light %s: %s",
                 config.name, esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================================
 * Device Registration - Main Entry Point
 * ============================================================================ */

/**
 * @brief Register all applicable ESPHome entities for a device
 */
static esp_err_t register_device_entities(const device_t *dev)
{
    if (!dev || !dev->in_use) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Registering entities for device: %s (caps=0x%08lX)",
             dev->friendly_name[0] ? dev->friendly_name : "unnamed",
             (unsigned long)dev->capabilities);

    /* Temperature sensor */
    if (dev->capabilities & DEV_CAP_TEMPERATURE) {
        register_sensor_entity(dev, DEV_CAP_TEMPERATURE);
    }

    /* Humidity sensor */
    if (dev->capabilities & DEV_CAP_HUMIDITY) {
        register_sensor_entity(dev, DEV_CAP_HUMIDITY);
    }

    /* Pressure sensor */
    if (dev->capabilities & DEV_CAP_PRESSURE) {
        register_sensor_entity(dev, DEV_CAP_PRESSURE);
    }

    /* Battery sensor */
    if (dev->capabilities & DEV_CAP_BATTERY) {
        register_sensor_entity(dev, DEV_CAP_BATTERY);
    }

    /* Power monitoring sensors */
    if (dev->capabilities & DEV_CAP_POWER) {
        register_sensor_entity(dev, DEV_CAP_POWER);
    }
    if (dev->capabilities & DEV_CAP_ENERGY) {
        register_sensor_entity(dev, DEV_CAP_ENERGY);
    }
    if (dev->capabilities & DEV_CAP_VOLTAGE) {
        register_sensor_entity(dev, DEV_CAP_VOLTAGE);
    }
    if (dev->capabilities & DEV_CAP_CURRENT) {
        register_sensor_entity(dev, DEV_CAP_CURRENT);
    }

    /* Binary sensors */
    if (dev->capabilities & DEV_CAP_MOTION) {
        register_binary_sensor_entity(dev, DEV_CAP_MOTION);
    }
    if (dev->capabilities & DEV_CAP_CONTACT) {
        register_binary_sensor_entity(dev, DEV_CAP_CONTACT);
    }
    if (dev->capabilities & DEV_CAP_VIBRATION) {
        register_binary_sensor_entity(dev, DEV_CAP_VIBRATION);
    }
    if (dev->capabilities & DEV_CAP_WATER_LEAK) {
        register_binary_sensor_entity(dev, DEV_CAP_WATER_LEAK);
    }
    if (dev->capabilities & DEV_CAP_SMOKE) {
        register_binary_sensor_entity(dev, DEV_CAP_SMOKE);
    }

    /* Light entity (has priority over switch if device has brightness) */
    if (dev->capabilities & DEV_CAP_BRIGHTNESS) {
        register_light_entity(dev);
    }
    /* Switch entity (only if no brightness control) */
    else if (dev->capabilities & DEV_CAP_ON_OFF) {
        register_switch_entity(dev);
    }

    return ESP_OK;
}

/* ============================================================================
 * State Update Helpers
 * ============================================================================ */

/**
 * @brief Update ESPHome entity states from device registry state JSON
 */
static void update_entity_states(const device_t *dev, cJSON *state)
{
    if (!dev || !state) {
        return;
    }

    float value;
    bool bool_value;

    /* Temperature */
    if ((dev->capabilities & DEV_CAP_TEMPERATURE) &&
        get_state_float(state, "temperature", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_TEMPERATURE);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Humidity */
    if ((dev->capabilities & DEV_CAP_HUMIDITY) &&
        get_state_float(state, "humidity", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_HUMIDITY);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Pressure */
    if ((dev->capabilities & DEV_CAP_PRESSURE) &&
        get_state_float(state, "pressure", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_PRESSURE);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Battery */
    if ((dev->capabilities & DEV_CAP_BATTERY) &&
        get_state_float(state, "battery", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_BATTERY);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Power monitoring */
    if ((dev->capabilities & DEV_CAP_POWER) &&
        get_state_float(state, "power", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_POWER);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    if ((dev->capabilities & DEV_CAP_ENERGY) &&
        get_state_float(state, "energy", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_ENERGY);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    if ((dev->capabilities & DEV_CAP_VOLTAGE) &&
        get_state_float(state, "voltage", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_VOLTAGE);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    if ((dev->capabilities & DEV_CAP_CURRENT) &&
        get_state_float(state, "current", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_CURRENT);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Motion binary sensor */
    if ((dev->capabilities & DEV_CAP_MOTION) &&
        get_state_bool(state, "occupancy", &bool_value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_MOTION);
        esphome_entity_update_binary_sensor(key, bool_value);
        s_stats.state_updates_sent++;
    }

    /* Contact binary sensor */
    if ((dev->capabilities & DEV_CAP_CONTACT) &&
        get_state_bool(state, "contact", &bool_value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_CONTACT);
        /* Note: contact=true means closed (no detection), contact=false means open */
        esphome_entity_update_binary_sensor(key, !bool_value);
        s_stats.state_updates_sent++;
    }

    /* Switch state */
    if ((dev->capabilities & DEV_CAP_ON_OFF) && !(dev->capabilities & DEV_CAP_BRIGHTNESS)) {
        if (get_state_bool(state, "state", &bool_value)) {
            esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_ON_OFF);
            esphome_entity_update_switch(key, bool_value);
            s_stats.state_updates_sent++;
        }
    }

    /* Light state */
    if (dev->capabilities & DEV_CAP_BRIGHTNESS) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_BRIGHTNESS);
        esphome_light_state_t light_state = {0};
        light_state.key = key;

        bool updated = false;

        if (get_state_bool(state, "state", &bool_value)) {
            light_state.state = bool_value;
            updated = true;
        }

        if (get_state_float(state, "brightness", &value)) {
            /* Zigbee brightness is 0-254, convert to 0.0-1.0 */
            light_state.brightness = value / 254.0f;
            updated = true;
        }

        if (get_state_float(state, "color_temp", &value)) {
            light_state.color_temp = value;
            updated = true;
        }

        if (updated) {
            esphome_entity_update_light(key, &light_state);
            s_stats.state_updates_sent++;
        }
    }
}

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

/**
 * @brief Handle EVT_DEVICE_STATE_CHANGED event
 */
static void handle_device_state_changed(event_type_t type, void *data,
                                         size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

    if (!data || data_size < sizeof(evt_device_state_t)) {
        ESP_LOGW(TAG, "Invalid state changed event data");
        return;
    }

    const evt_device_state_t *evt = (const evt_device_state_t *)data;

    /* Look up device in registry */
    device_t *dev = device_registry_get(evt->ieee_addr);
    if (!dev) {
        ESP_LOGD(TAG, "Device 0x%016llX not in registry",
                 (unsigned long long)evt->ieee_addr);
        return;
    }

    /* Parse JSON state if provided in event */
    cJSON *state = NULL;
    if (evt->json_state) {
        state = cJSON_Parse(evt->json_state);
    }

    /* Fall back to registry state if not in event */
    if (!state) {
        state = device_registry_get_state(evt->ieee_addr);
        if (state) {
            /* Registry state is not owned by us, don't delete it */
            update_entity_states(dev, state);
            return;
        }
    }

    if (state) {
        update_entity_states(dev, state);
        cJSON_Delete(state);
    }
}

/**
 * @brief Handle EVT_DEVICE_JOINED event
 */
static void handle_device_joined(event_type_t type, void *data,
                                  size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

    if (!data || data_size < sizeof(evt_device_joined_t)) {
        ESP_LOGW(TAG, "Invalid device joined event data");
        return;
    }

    const evt_device_joined_t *evt = (const evt_device_joined_t *)data;

    ESP_LOGI(TAG, "Device joined: IEEE=0x%016llX",
             (unsigned long long)evt->ieee_addr);

    /* Sync device to ESPHome entities */
    esphome_adapter_sync_device(evt->ieee_addr);
}

/**
 * @brief Handle EVT_DEVICE_LEFT event
 */
static void handle_device_left(event_type_t type, void *data,
                                size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

    if (!data || data_size < sizeof(evt_device_left_t)) {
        ESP_LOGW(TAG, "Invalid device left event data");
        return;
    }

    const evt_device_left_t *evt = (const evt_device_left_t *)data;

    ESP_LOGI(TAG, "Device left: IEEE=0x%016llX",
             (unsigned long long)evt->ieee_addr);

    /* Remove ESPHome entities for this device */
    esphome_adapter_remove_device(evt->ieee_addr);
}

/**
 * @brief Handle EVT_DEVICE_AVAILABILITY_CHANGED event
 *
 * When a device goes offline, marks all its ESPHome entities as
 * missing/unavailable. When a device comes back online, triggers
 * a full state resync so entities reflect current values.
 */
static void handle_device_availability_changed(event_type_t type, void *data,
                                                size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;

    if (!data || data_size < sizeof(evt_device_availability_t)) {
        ESP_LOGW(TAG, "Invalid availability changed event data");
        return;
    }

    const evt_device_availability_t *evt = (const evt_device_availability_t *)data;

    ESP_LOGI(TAG, "Device 0x%016llX availability: %s",
             (unsigned long long)evt->device_id,
             evt->online ? "online" : "offline");

    if (!evt->online) {
        /* Device went offline — mark all entities as missing/unavailable */
        esphome_adapter_remove_device(evt->device_id);
    } else {
        /* Device came back online — resync entities with current state */
        esphome_adapter_sync_device(evt->device_id);
    }
}

/* ============================================================================
 * Device Registry Iterator Callback
 * ============================================================================ */

/**
 * @brief Iterator callback to sync devices to ESPHome
 */
static bool sync_device_iterator(device_t *dev, void *ctx)
{
    (void)ctx;

    if (!dev->in_use) {
        return true; /* Continue */
    }

    /* Register entities for this device */
    register_device_entities(dev);

    /* Get current state and update entities */
    cJSON *state = device_registry_get_state(dev->id);
    if (state) {
        update_entity_states(dev, state);
    }

    return true; /* Continue iteration */
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t esphome_adapter_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "ESPHome adapter already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing ESPHome adapter...");

    /* Subscribe to device state changed events */
    esp_err_t ret = event_subscribe(EVT_DEVICE_STATE_CHANGED,
                                     handle_device_state_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_DEVICE_STATE_CHANGED: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* Subscribe to device joined events */
    ret = event_subscribe(EVT_DEVICE_JOINED, handle_device_joined, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_DEVICE_JOINED: %s",
                 esp_err_to_name(ret));
        event_unsubscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed);
        return ret;
    }

    /* Subscribe to device left events */
    ret = event_subscribe(EVT_DEVICE_LEFT, handle_device_left, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_DEVICE_LEFT: %s",
                 esp_err_to_name(ret));
        event_unsubscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed);
        event_unsubscribe(EVT_DEVICE_JOINED, handle_device_joined);
        return ret;
    }

    /* Subscribe to device availability changed events */
    ret = event_subscribe(EVT_DEVICE_AVAILABILITY_CHANGED,
                           handle_device_availability_changed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_DEVICE_AVAILABILITY_CHANGED: %s",
                 esp_err_to_name(ret));
        event_unsubscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed);
        event_unsubscribe(EVT_DEVICE_JOINED, handle_device_joined);
        event_unsubscribe(EVT_DEVICE_LEFT, handle_device_left);
        return ret;
    }

    /* Reset statistics */
    memset(&s_stats, 0, sizeof(s_stats));

    s_initialized = true;
    ESP_LOGI(TAG, "ESPHome adapter initialized successfully");
    return ESP_OK;
}

esp_err_t esphome_adapter_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "ESPHome adapter not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing ESPHome adapter...");

    /* Unsubscribe from all events */
    event_unsubscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed);
    event_unsubscribe(EVT_DEVICE_JOINED, handle_device_joined);
    event_unsubscribe(EVT_DEVICE_LEFT, handle_device_left);
    event_unsubscribe(EVT_DEVICE_AVAILABILITY_CHANGED, handle_device_availability_changed);

    s_initialized = false;

    ESP_LOGI(TAG, "ESPHome adapter deinitialized");
    return ESP_OK;
}

bool esphome_adapter_is_initialized(void)
{
    return s_initialized;
}

void esphome_adapter_on_client_connected(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Adapter not initialized, ignoring client connect");
        return;
    }

    s_stats.client_connects++;
    ESP_LOGI(TAG, "ESPHome client connected, syncing entities...");

    /* Sync all devices to ESPHome */
    esphome_adapter_sync_all_devices();
}

void esphome_adapter_on_client_disconnected(void)
{
    if (!s_initialized) {
        return;
    }

    s_stats.client_disconnects++;
    ESP_LOGI(TAG, "ESPHome client disconnected");
}

esp_err_t esphome_adapter_sync_device(device_id_t id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    device_t *dev = device_registry_get(id);
    if (!dev) {
        ESP_LOGD(TAG, "Device 0x%016llX not in registry", (unsigned long long)id);
        return ESP_ERR_NOT_FOUND;
    }

    s_stats.sync_operations++;

    /* Register entities */
    register_device_entities(dev);

    /* Update states */
    cJSON *state = device_registry_get_state(id);
    if (state) {
        update_entity_states(dev, state);
    }

    return ESP_OK;
}

esp_err_t esphome_adapter_sync_all_devices(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Syncing all devices to ESPHome...");

    s_stats.sync_operations++;
    device_registry_iterate(sync_device_iterator, NULL);

    /* Broadcast all states to connected clients */
    esphome_api_broadcast_all_states();

    ESP_LOGI(TAG, "Device sync complete (entities=%lu)",
             (unsigned long)s_stats.entities_registered);
    return ESP_OK;
}

esp_err_t esphome_adapter_remove_device(device_id_t id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Removing ESPHome entities for device 0x%016llX",
             (unsigned long long)id);

    /*
     * Note: ESPHome entity API doesn't currently provide unregistration.
     * Entities remain registered but will show as unavailable.
     * A full implementation would need entity removal support in esphome_entities.h
     */

    /* For now, set entities to missing/unavailable state */
    device_t *dev = device_registry_get(id);
    if (dev) {
        /* Sensor capabilities — all have set_sensor_missing() */
        static const device_capability_t sensor_caps[] = {
            DEV_CAP_TEMPERATURE, DEV_CAP_HUMIDITY, DEV_CAP_PRESSURE, DEV_CAP_BATTERY,
            DEV_CAP_POWER, DEV_CAP_ENERGY, DEV_CAP_VOLTAGE, DEV_CAP_CURRENT
        };
        for (size_t i = 0; i < sizeof(sensor_caps) / sizeof(sensor_caps[0]); i++) {
            if (dev->capabilities & sensor_caps[i]) {
                esphome_entity_key_t key = esphome_adapter_make_entity_key(id, sensor_caps[i]);
                esphome_entity_set_sensor_missing(key);
                s_stats.entities_removed++;
            }
        }

        /* Binary sensor capabilities — all have set_binary_sensor_missing() */
        static const device_capability_t binary_caps[] = {
            DEV_CAP_MOTION, DEV_CAP_CONTACT, DEV_CAP_VIBRATION,
            DEV_CAP_WATER_LEAK, DEV_CAP_SMOKE
        };
        for (size_t i = 0; i < sizeof(binary_caps) / sizeof(binary_caps[0]); i++) {
            if (dev->capabilities & binary_caps[i]) {
                esphome_entity_key_t key = esphome_adapter_make_entity_key(id, binary_caps[i]);
                esphome_entity_set_binary_sensor_missing(key);
                s_stats.entities_removed++;
            }
        }

        /* Light entity — no set_light_missing API, force state to OFF */
        if (dev->capabilities & DEV_CAP_BRIGHTNESS) {
            esphome_entity_key_t key = esphome_adapter_make_entity_key(id, DEV_CAP_BRIGHTNESS);
            esphome_light_state_t light_state = {0};
            light_state.key = key;
            light_state.state = false;
            esphome_entity_update_light(key, &light_state);
            s_stats.entities_removed++;
            ESP_LOGD(TAG, "Light entity set to OFF for removed device");
        }
        /* Switch entity (only if no brightness — mirrors registration logic) */
        else if (dev->capabilities & DEV_CAP_ON_OFF) {
            esphome_entity_key_t key = esphome_adapter_make_entity_key(id, DEV_CAP_ON_OFF);
            esphome_entity_update_switch(key, false);
            s_stats.entities_removed++;
            ESP_LOGD(TAG, "Switch entity set to OFF for removed device");
        }
    }

    return ESP_OK;
}

void esphome_adapter_get_stats(esphome_adapter_stats_t *stats)
{
    if (stats) {
        *stats = s_stats;
    }
}

void esphome_adapter_reset_stats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    ESP_LOGD(TAG, "Statistics reset");
}

/* Adapter interface vtable */
const adapter_ops_t esphome_adapter_ops = {
    .init = esphome_adapter_init,
    .start = NULL,
    .stop = NULL,
    .deinit = esphome_adapter_deinit,
    .is_running = NULL,
};
