/**
 * @file esphome_adapter_exposes.c
 * @brief Expose-Based Entity Sub-module for ESPHome Adapter
 *
 * Extracted from esphome_adapter.c — handles all converter expose-based
 * entity registration, command callbacks, and state updates.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_adapter_exposes.h"
#include "esphome_adapter.h"
#include "esphome_adapter_internal.h"
#include "core/device/device_registry.h"
#include "esphome/esphome_api.h"
#include "esphome/esphome_entities.h"
#include "esphome/esphome_common.h"
#include "zigbee/converter/zb_converter.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "ESPH_EXPOSE";

/* ============================================================================
 * Expose Key Generation
 * ============================================================================ */

esphome_entity_key_t esphome_adapter_make_expose_key(device_id_t id, uint8_t expose_idx)
{
    uint32_t id_hash = (uint32_t)(id ^ (id >> 32));
    uint32_t key = ESPHOME_ADAPTER_EXPOSE_KEY_OFFSET;
    key |= ((uint32_t)(expose_idx & 0x1F)) << 23;
    key |= (id_hash & 0x007FFFFF);
    return key;
}

/* ============================================================================
 * Expose Coverage Check
 * ============================================================================ */

bool esphome_adapter_is_expose_covered_by_cap(const zb_expose_t *expose, uint32_t capabilities)
{
    const char *dc = expose->device_class;

    switch (expose->type) {
    case ZB_EXPOSE_SENSOR:
        if (!dc) return false;
        if (strcmp(dc, "temperature") == 0) return (capabilities & DEV_CAP_TEMPERATURE) != 0;
        if (strcmp(dc, "humidity") == 0)    return (capabilities & DEV_CAP_HUMIDITY) != 0;
        if (strcmp(dc, "pressure") == 0)    return (capabilities & DEV_CAP_PRESSURE) != 0;
        if (strcmp(dc, "battery") == 0)     return (capabilities & DEV_CAP_BATTERY) != 0;
        if (strcmp(dc, "power") == 0)       return (capabilities & DEV_CAP_POWER) != 0;
        if (strcmp(dc, "energy") == 0)      return (capabilities & DEV_CAP_ENERGY) != 0;
        if (strcmp(dc, "voltage") == 0)     return (capabilities & DEV_CAP_VOLTAGE) != 0;
        if (strcmp(dc, "current") == 0)     return (capabilities & DEV_CAP_CURRENT) != 0;
        if (strcmp(dc, "illuminance") == 0) return (capabilities & DEV_CAP_ILLUMINANCE) != 0;
        if (strcmp(dc, "carbon_dioxide") == 0 || strcmp(dc, "co2") == 0)
            return (capabilities & DEV_CAP_CO2) != 0;
        if (strcmp(dc, "volatile_organic_compounds") == 0 || strcmp(dc, "voc") == 0)
            return (capabilities & DEV_CAP_VOC) != 0;
        if (strcmp(dc, "pm25") == 0)       return (capabilities & DEV_CAP_PM25) != 0;
        if (strcmp(dc, "moisture") == 0 && expose->name && strcmp(expose->name, "soil_moisture") == 0)
            return (capabilities & DEV_CAP_SOIL_MOISTURE) != 0;
        return false;

    case ZB_EXPOSE_BINARY_SENSOR:
        if (!dc) return false;
        if (strcmp(dc, "vibration") == 0)   return (capabilities & DEV_CAP_VIBRATION) != 0;
        if (strcmp(dc, "motion") == 0 || strcmp(dc, "occupancy") == 0)
            return (capabilities & DEV_CAP_MOTION) != 0;
        if (strcmp(dc, "door") == 0 || strcmp(dc, "contact") == 0 || strcmp(dc, "window") == 0)
            return (capabilities & DEV_CAP_CONTACT) != 0;
        if (strcmp(dc, "moisture") == 0 || strcmp(dc, "water_leak") == 0)
            return (capabilities & DEV_CAP_WATER_LEAK) != 0;
        if (strcmp(dc, "smoke") == 0)       return (capabilities & DEV_CAP_SMOKE) != 0;
        if (strcmp(dc, "co") == 0)          return (capabilities & DEV_CAP_CO) != 0;
        if (strcmp(dc, "tamper") == 0)     return (capabilities & DEV_CAP_TAMPER) != 0;
        if (strcmp(dc, "battery") == 0 && expose->name && strcmp(expose->name, "battery_low") == 0)
            return (capabilities & DEV_CAP_BATTERY_LOW) != 0;
        return false;

    case ZB_EXPOSE_LIGHT:   return (capabilities & DEV_CAP_BRIGHTNESS) != 0;
    case ZB_EXPOSE_SWITCH:  return (capabilities & DEV_CAP_ON_OFF) != 0;
    case ZB_EXPOSE_COVER:   return (capabilities & DEV_CAP_COVER) != 0;
    case ZB_EXPOSE_FAN:     return (capabilities & DEV_CAP_FAN) != 0;
    case ZB_EXPOSE_CLIMATE: return (capabilities & DEV_CAP_CLIMATE) != 0;
    case ZB_EXPOSE_LOCK:    return (capabilities & DEV_CAP_LOCK) != 0;

    default:
        return false;
    }
}

static bool is_string_sensor_expose(const zb_expose_t *expose)
{
    return expose->name && strcmp(expose->name, "action") == 0;
}

/* ============================================================================
 * Expose Capability Mapping
 * ============================================================================ */

uint32_t esphome_adapter_expose_to_cap_bitmask(const zb_expose_t *expose)
{
    switch (expose->type) {
    case ZB_EXPOSE_SWITCH:
        return DEV_CAP_ON_OFF;

    case ZB_EXPOSE_LIGHT:
        {
            uint32_t caps = DEV_CAP_ON_OFF | DEV_CAP_BRIGHTNESS;
            if (expose->features & ZB_FEATURE_COLOR_TEMP) caps |= DEV_CAP_COLOR_TEMP;
            if (expose->features & (ZB_FEATURE_COLOR_XY | ZB_FEATURE_COLOR_HS)) caps |= DEV_CAP_COLOR_XY;
            return caps;
        }

    case ZB_EXPOSE_SENSOR:
        if (!expose->device_class) return 0;
        if (strcmp(expose->device_class, "temperature") == 0) return DEV_CAP_TEMPERATURE;
        if (strcmp(expose->device_class, "humidity") == 0)    return DEV_CAP_HUMIDITY;
        if (strcmp(expose->device_class, "pressure") == 0)    return DEV_CAP_PRESSURE;
        if (strcmp(expose->device_class, "battery") == 0)     return DEV_CAP_BATTERY;
        if (strcmp(expose->device_class, "power") == 0)       return DEV_CAP_POWER;
        if (strcmp(expose->device_class, "energy") == 0)      return DEV_CAP_ENERGY;
        if (strcmp(expose->device_class, "voltage") == 0)     return DEV_CAP_VOLTAGE;
        if (strcmp(expose->device_class, "current") == 0)     return DEV_CAP_CURRENT;
        return 0;

    case ZB_EXPOSE_BINARY_SENSOR:
        if (!expose->device_class) return 0;
        if (strcmp(expose->device_class, "motion") == 0 ||
            strcmp(expose->device_class, "occupancy") == 0)   return DEV_CAP_MOTION;
        if (strcmp(expose->device_class, "door") == 0 ||
            strcmp(expose->device_class, "contact") == 0)     return DEV_CAP_CONTACT;
        if (strcmp(expose->device_class, "vibration") == 0)   return DEV_CAP_VIBRATION;
        if (strcmp(expose->device_class, "moisture") == 0 ||
            strcmp(expose->device_class, "water_leak") == 0)  return DEV_CAP_WATER_LEAK;
        if (strcmp(expose->device_class, "smoke") == 0)       return DEV_CAP_SMOKE;
        if (strcmp(expose->device_class, "co") == 0)          return DEV_CAP_CO;
        return 0;

    case ZB_EXPOSE_COVER:   return DEV_CAP_COVER;
    case ZB_EXPOSE_FAN:     return DEV_CAP_FAN;
    case ZB_EXPOSE_CLIMATE: return DEV_CAP_CLIMATE;
    case ZB_EXPOSE_LOCK:    return DEV_CAP_LOCK;

    default:
        return 0;
    }
}

/* ============================================================================
 * Expose Device Lookup
 * ============================================================================ */

typedef struct {
    esphome_entity_key_t target_key;
    device_t *result;
    const zb_expose_t *expose;
    uint8_t expose_idx;
} find_expose_device_ctx_t;

static bool find_device_for_expose_key_cb(device_t *dev, void *ctx)
{
    find_expose_device_ctx_t *fc = (find_expose_device_ctx_t *)ctx;
    const zb_converter_def_t *conv =
        (const zb_converter_def_t *)device_registry_get_converter(dev->id);
    if (!conv) return true;

    for (uint8_t i = 0; i < conv->expose_count; i++) {
        uint32_t key = esphome_adapter_make_expose_key(dev->id, i);
        if (key == fc->target_key) {
            fc->result = dev;
            fc->expose = &conv->exposes[i];
            fc->expose_idx = i;
            return false;
        }
    }
    return true;
}

static device_t *find_device_for_expose_key(esphome_entity_key_t key,
                                             const zb_expose_t **out_expose)
{
    find_expose_device_ctx_t ctx = {
        .target_key = key,
        .result = NULL,
        .expose = NULL,
        .expose_idx = 0,
    };
    device_registry_iterate_zigbee(find_device_for_expose_key_cb, &ctx);
    if (out_expose) *out_expose = ctx.expose;
    return ctx.result;
}

/* ============================================================================
 * Expose Command Callbacks
 * ============================================================================ */

static esp_err_t expose_select_command_callback(esphome_entity_key_t key, const char *option)
{
    ESP_LOGI(TAG, "Select command: key=0x%08lX, option=%s", (unsigned long)key, option);

    esphome_adapter_get_stats_ptr()->commands_received++;

    const zb_expose_t *expose = NULL;
    device_t *dev = find_device_for_expose_key(key, &expose);
    if (!dev || !expose) {
        ESP_LOGW(TAG, "Select command: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    const char *prop = zb_expose_property(expose);
    if (!prop) {
        ESP_LOGW(TAG, "Select command: expose has no property for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cmd = cJSON_CreateObject();
    if (!cmd) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(cmd, prop, option);

    uint8_t ep = expose->endpoint ? expose->endpoint : dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    esp_err_t ret = zb_converter_handle_command(dev->proto.zigbee.short_addr, ep, cmd);
    cJSON_Delete(cmd);

    if (ret == ESP_OK) {
        esphome_entity_update_select(key, option);
    }
    return ret;
}

static esp_err_t expose_switch_command_callback(esphome_entity_key_t key, bool state)
{
    ESP_LOGI(TAG, "Expose switch command: key=0x%08lX, state=%d", (unsigned long)key, state);

    esphome_adapter_get_stats_ptr()->commands_received++;

    const zb_expose_t *expose = NULL;
    device_t *dev = find_device_for_expose_key(key, &expose);
    if (!dev || !expose) {
        ESP_LOGW(TAG, "Switch command: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    const char *prop = zb_expose_property(expose);
    if (!prop) prop = "state";

    cJSON *cmd = cJSON_CreateObject();
    if (!cmd) return ESP_ERR_NO_MEM;

    if (!expose->name) {
        cJSON_AddStringToObject(cmd, prop, state ? "ON" : "OFF");
    } else {
        cJSON_AddBoolToObject(cmd, prop, state);
    }

    uint8_t ep = expose->endpoint ? expose->endpoint : dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    esp_err_t ret = zb_converter_handle_command(dev->proto.zigbee.short_addr, ep, cmd);
    cJSON_Delete(cmd);

    if (ret == ESP_OK) {
        esphome_entity_update_switch(key, state);
    }
    return ret;
}

static esp_err_t expose_button_press_callback(esphome_entity_key_t key)
{
    ESP_LOGI(TAG, "Expose button press: key=0x%08lX", (unsigned long)key);

    esphome_adapter_get_stats_ptr()->commands_received++;

    const zb_expose_t *expose = NULL;
    device_t *dev = find_device_for_expose_key(key, &expose);
    if (!dev || !expose) {
        ESP_LOGW(TAG, "Button press: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    const char *prop = zb_expose_property(expose);
    if (!prop) {
        ESP_LOGW(TAG, "Button press: expose has no property for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cmd = cJSON_CreateObject();
    if (!cmd) return ESP_ERR_NO_MEM;

    if (strcmp(prop, "state") == 0) {
        cJSON_AddStringToObject(cmd, prop, "ON");
    } else {
        cJSON_AddStringToObject(cmd, prop, "");
    }

    uint8_t ep = expose->endpoint ? expose->endpoint : dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    esp_err_t ret = zb_converter_handle_command(dev->proto.zigbee.short_addr, ep, cmd);
    cJSON_Delete(cmd);
    return ret;
}

static esp_err_t expose_number_command_callback(esphome_entity_key_t key, float value)
{
    ESP_LOGI(TAG, "Expose number command: key=0x%08lX, value=%.1f", (unsigned long)key, value);

    esphome_adapter_get_stats_ptr()->commands_received++;

    const zb_expose_t *expose = NULL;
    device_t *dev = find_device_for_expose_key(key, &expose);
    if (!dev || !expose) {
        ESP_LOGW(TAG, "Number command: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    const char *prop = zb_expose_property(expose);
    if (!prop) {
        ESP_LOGW(TAG, "Number command: expose has no property for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cmd = cJSON_CreateObject();
    if (!cmd) return ESP_ERR_NO_MEM;

    cJSON_AddNumberToObject(cmd, prop, (double)value);

    uint8_t ep = expose->endpoint ? expose->endpoint : dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    esp_err_t ret = zb_converter_handle_command(dev->proto.zigbee.short_addr, ep, cmd);
    cJSON_Delete(cmd);

    if (ret == ESP_OK) {
        esphome_entity_update_number(key, value);
    }
    return ret;
}

static esp_err_t expose_text_command_callback(esphome_entity_key_t key, const char *value)
{
    ESP_LOGI(TAG, "Expose text command: key=0x%08lX, value=%s", (unsigned long)key, value ? value : "NULL");

    esphome_adapter_get_stats_ptr()->commands_received++;

    const zb_expose_t *expose = NULL;
    device_t *dev = find_device_for_expose_key(key, &expose);
    if (!dev || !expose) {
        ESP_LOGW(TAG, "Text command: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    const char *prop = zb_expose_property(expose);
    if (!prop) {
        ESP_LOGW(TAG, "Text command: expose has no property for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *cmd = cJSON_CreateObject();
    if (!cmd) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(cmd, prop, value ? value : "");

    uint8_t ep = expose->endpoint ? expose->endpoint : dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    esp_err_t ret = zb_converter_handle_command(dev->proto.zigbee.short_addr, ep, cmd);
    cJSON_Delete(cmd);

    if (ret == ESP_OK) {
        esphome_entity_update_text(key, value ? value : "");
    }
    return ret;
}

/* ============================================================================
 * Expose Entity Registration Helpers
 * ============================================================================ */

static esp_err_t register_expose_sensor(const device_t *dev, const zb_expose_t *expose,
                                         uint32_t key, uint32_t device_id)
{
    esphome_sensor_config_t config = {0};
    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();

    config.key = key;
    config.device_id = device_id;
    esphome_adapter_make_expose_name(dev, expose->name, config.name, sizeof(config.name));
    esphome_adapter_make_expose_unique_id(dev, expose->name, config.unique_id, sizeof(config.unique_id));

    config.device_class = esphome_adapter_expose_dc_to_sensor_class(expose->device_class);
    config.state_class = esphome_adapter_expose_sc_to_state_class(expose->state_class);
    if (expose->unit) {
        strncpy(config.unit_of_measurement, expose->unit,
                sizeof(config.unit_of_measurement) - 1);
    }

    config.accuracy_decimals = 0;
    if (config.device_class == ESPHOME_SENSOR_CLASS_TEMPERATURE ||
        config.device_class == ESPHOME_SENSOR_CLASS_HUMIDITY ||
        config.device_class == ESPHOME_SENSOR_CLASS_PRESSURE ||
        config.device_class == ESPHOME_SENSOR_CLASS_POWER ||
        config.device_class == ESPHOME_SENSOR_CLASS_ENERGY ||
        config.device_class == ESPHOME_SENSOR_CLASS_CURRENT) {
        config.accuracy_decimals = 1;
    }
    config.force_update = false;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_sensor(&config);
    if (ret == ESP_OK) {
        stats->entities_registered++;
        esphome_entity_update_sensor(config.key, NAN);
        ESP_LOGD(TAG, "Registered expose sensor: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    }
    return ret;
}

static esp_err_t register_expose_text_sensor(const device_t *dev, const zb_expose_t *expose,
                                              uint32_t key, uint32_t device_id)
{
    esphome_text_sensor_config_t config = {0};
    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();

    config.key = key;
    config.device_id = device_id;
    esphome_adapter_make_expose_name(dev, expose->name, config.name, sizeof(config.name));
    esphome_adapter_make_expose_unique_id(dev, expose->name, config.unique_id, sizeof(config.unique_id));

    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_text_sensor(&config);
    if (ret == ESP_OK) {
        stats->entities_registered++;
        esphome_entity_update_text_sensor(config.key, "");
        ESP_LOGD(TAG, "Registered expose text sensor: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    }
    return ret;
}

static esp_err_t register_expose_select(const device_t *dev, const zb_expose_t *expose,
                                         uint32_t key, uint32_t device_id)
{
    esphome_select_config_t config = {0};
    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();

    config.key = key;
    config.device_id = device_id;
    esphome_adapter_make_expose_name(dev, expose->name, config.name, sizeof(config.name));
    esphome_adapter_make_expose_unique_id(dev, expose->name, config.unique_id, sizeof(config.unique_id));

    if (expose->icon) {
        strncpy(config.icon, expose->icon, sizeof(config.icon) - 1);
    } else {
        strncpy(config.icon, "mdi:tune", sizeof(config.icon) - 1);
    }

    if (expose->ext.select.values && expose->ext.select.count > 0) {
        uint8_t max_opts = expose->ext.select.count;
        if (max_opts > 8) max_opts = 8;
        for (uint8_t oi = 0; oi < max_opts; oi++) {
            if (expose->ext.select.values[oi]) {
                strncpy(config.options[oi], expose->ext.select.values[oi], 31);
            }
        }
        config.option_count = max_opts;
    } else if (expose->name && strcmp(expose->name, "sensitivity") == 0) {
        strncpy(config.options[0], "low", 31);
        strncpy(config.options[1], "medium", 31);
        strncpy(config.options[2], "high", 31);
        config.option_count = 3;
    }

    config.command_callback = expose_select_command_callback;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_select(&config);
    if (ret == ESP_OK) {
        stats->entities_registered++;
        if (config.option_count > 0) {
            esphome_entity_update_select(config.key, config.options[0]);
        }
        ESP_LOGD(TAG, "Registered expose select: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    }
    return ret;
}

static esp_err_t register_expose_switch(const device_t *dev, const zb_expose_t *expose,
                                         uint32_t key, uint32_t device_id, bool writable)
{
    esphome_switch_config_t cfg = {0};
    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();

    cfg.key = key;
    cfg.device_id = device_id;
    if (expose->name) {
        esphome_adapter_make_expose_name(dev, expose->name, cfg.name, sizeof(cfg.name));
        esphome_adapter_make_expose_unique_id(dev, zb_expose_property(expose), cfg.unique_id, sizeof(cfg.unique_id));
    } else {
        esphome_adapter_make_expose_name(dev, "Switch", cfg.name, sizeof(cfg.name));
        esphome_adapter_make_expose_unique_id(dev, "state", cfg.unique_id, sizeof(cfg.unique_id));
    }
    if (expose->icon) {
        strncpy(cfg.icon, expose->icon, sizeof(cfg.icon) - 1);
    } else {
        strncpy(cfg.icon, "mdi:toggle-switch", sizeof(cfg.icon) - 1);
    }
    cfg.command_callback = writable ? expose_switch_command_callback : NULL;

    esp_err_t ret = esphome_entity_register_switch(&cfg);
    if (ret == ESP_OK) {
        stats->entities_registered++;
        esphome_entity_update_switch(key, false);
        ESP_LOGD(TAG, "Registered expose switch: %s (key=0x%08lX)",
                 cfg.name, (unsigned long)cfg.key);
    }
    return ret;
}

static esp_err_t register_expose_number(const device_t *dev, const zb_expose_t *expose,
                                         uint32_t key, uint32_t device_id, bool writable)
{
    esphome_number_config_t cfg = {0};
    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();

    cfg.key = key;
    cfg.device_id = device_id;
    esphome_adapter_make_expose_name(dev, expose->name, cfg.name, sizeof(cfg.name));
    esphome_adapter_make_expose_unique_id(dev, zb_expose_property(expose), cfg.unique_id, sizeof(cfg.unique_id));
    if (expose->icon) {
        strncpy(cfg.icon, expose->icon, sizeof(cfg.icon) - 1);
    }
    if (expose->unit) {
        strncpy(cfg.unit_of_measurement, expose->unit, sizeof(cfg.unit_of_measurement) - 1);
    }
    cfg.min_value = expose->ext.numeric.min;
    cfg.max_value = expose->ext.numeric.max;
    cfg.step = expose->ext.numeric.step > 0 ? expose->ext.numeric.step : 1;
    cfg.mode = ESPHOME_NUMBER_MODE_BOX;
    cfg.command_callback = writable ? expose_number_command_callback : NULL;

    esp_err_t ret = esphome_entity_register_number(&cfg);
    if (ret == ESP_OK) {
        stats->entities_registered++;
        esphome_entity_update_number(key, NAN);
        ESP_LOGD(TAG, "Registered expose number: %s (key=0x%08lX, min=%.0f max=%.0f step=%.0f)",
                 cfg.name, (unsigned long)cfg.key, cfg.min_value, cfg.max_value, cfg.step);
    }
    return ret;
}

static esp_err_t register_expose_text(const device_t *dev, const zb_expose_t *expose,
                                       uint32_t key, uint32_t device_id, bool writable)
{
    esphome_text_config_t cfg = {0};
    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();

    cfg.key = key;
    cfg.device_id = device_id;
    esphome_adapter_make_expose_name(dev, expose->name, cfg.name, sizeof(cfg.name));
    esphome_adapter_make_expose_unique_id(dev, zb_expose_property(expose), cfg.unique_id, sizeof(cfg.unique_id));
    if (expose->icon) {
        strncpy(cfg.icon, expose->icon, sizeof(cfg.icon) - 1);
    }
    cfg.min_length = 0;
    cfg.max_length = 127;
    cfg.mode = ESPHOME_TEXT_MODE_TEXT;
    cfg.command_callback = writable ? expose_text_command_callback : NULL;

    esp_err_t ret = esphome_entity_register_text(&cfg);
    if (ret == ESP_OK) {
        stats->entities_registered++;
        esphome_entity_update_text(key, "");
        ESP_LOGD(TAG, "Registered expose text: %s (key=0x%08lX)",
                 cfg.name, (unsigned long)cfg.key);
    }
    return ret;
}

/* ============================================================================
 * Public API: Expose-First Registration
 * ============================================================================ */

uint32_t esphome_adapter_exposes_register(const device_t *dev,
                                           const zb_converter_def_t *conv)
{
    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);
    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();
    uint32_t covered_caps = 0;

    ESP_LOGD(TAG, "Expose-first registration for %s (%u exposes)",
             conv->description ? conv->description : conv->model, conv->expose_count);

    for (uint8_t i = 0; i < conv->expose_count; i++) {
        const zb_expose_t *expose = &conv->exposes[i];
        uint32_t key = esphome_adapter_make_expose_key(dev->id, i);
        bool writable = (expose->access & EA_SET) != 0;

        switch (expose->type) {
        case ZB_EXPOSE_SWITCH:
            register_expose_switch(dev, expose, key, device_id, writable);
            covered_caps |= DEV_CAP_ON_OFF;
            break;

        case ZB_EXPOSE_LIGHT:
            covered_caps |= DEV_CAP_ON_OFF;
            break;

        case ZB_EXPOSE_SENSOR:
            if (expose->name) {
                if (is_string_sensor_expose(expose)) {
                    register_expose_text_sensor(dev, expose, key, device_id);
                } else {
                    register_expose_sensor(dev, expose, key, device_id);
                }
                covered_caps |= esphome_adapter_expose_to_cap_bitmask(expose);
            }
            break;

        case ZB_EXPOSE_BINARY_SENSOR:
            if (expose->name) {
                esphome_binary_sensor_config_t bconfig = {0};
                bconfig.key = key;
                bconfig.device_id = device_id;
                esphome_adapter_make_expose_name(dev, expose->name, bconfig.name, sizeof(bconfig.name));
                esphome_adapter_make_expose_unique_id(dev, expose->name, bconfig.unique_id, sizeof(bconfig.unique_id));
                bconfig.device_class = esphome_adapter_expose_dc_to_binary_class(expose->device_class);
                if (expose->icon) {
                    strncpy(bconfig.icon, expose->icon, sizeof(bconfig.icon) - 1);
                }

                if (esphome_entity_register_binary_sensor(&bconfig) == ESP_OK) {
                    stats->entities_registered++;
                    esphome_entity_update_binary_sensor(bconfig.key, false);
                    ESP_LOGD(TAG, "Registered expose binary sensor: %s dc=%d (key=0x%08lX)",
                             bconfig.name, (int)bconfig.device_class, (unsigned long)bconfig.key);
                }
                covered_caps |= esphome_adapter_expose_to_cap_bitmask(expose);
            }
            break;

        case ZB_EXPOSE_SELECT:
            if (expose->name) {
                if (!(expose->access & EA_SET)) {
                    register_expose_text_sensor(dev, expose, key, device_id);
                } else {
                    register_expose_select(dev, expose, key, device_id);
                }
            }
            break;

        case ZB_EXPOSE_NUMBER:
            if (expose->name) {
                register_expose_number(dev, expose, key, device_id, writable);
            }
            break;

        case ZB_EXPOSE_TEXT:
            if (expose->name) {
                register_expose_text(dev, expose, key, device_id, writable);
            }
            break;

        case ZB_EXPOSE_COVER:
        case ZB_EXPOSE_FAN:
        case ZB_EXPOSE_CLIMATE:
        case ZB_EXPOSE_LOCK:
            break;

        case ZB_EXPOSE_BUTTON:
            if (expose->name) {
                esphome_button_config_t bconfig = {0};
                bconfig.key = key;
                bconfig.device_id = device_id;
                esphome_adapter_make_expose_name(dev, expose->name, bconfig.name, sizeof(bconfig.name));
                esphome_adapter_make_expose_unique_id(dev, expose->name, bconfig.unique_id, sizeof(bconfig.unique_id));
                bconfig.device_class = ESPHOME_BUTTON_CLASS_NONE;
                if (expose->icon) {
                    strncpy(bconfig.icon, expose->icon, sizeof(bconfig.icon) - 1);
                }
                bconfig.press_callback = expose_button_press_callback;

                if (esphome_entity_register_button(&bconfig) == ESP_OK) {
                    stats->entities_registered++;
                    ESP_LOGD(TAG, "Registered expose button: %s (key=0x%08lX)",
                             bconfig.name, (unsigned long)bconfig.key);
                }
            }
            break;

        default:
            break;
        }
    }

    ESP_LOGD(TAG, "Expose registration covered caps: 0x%08lX", (unsigned long)covered_caps);
    return covered_caps;
}

/* ============================================================================
 * Public API: Legacy Expose Registration
 * ============================================================================ */

void esphome_adapter_exposes_register_legacy(const device_t *dev)
{
    const zb_converter_def_t *conv =
        (const zb_converter_def_t *)device_registry_get_converter(dev->id);
    if (!conv || conv->expose_count == 0) {
        return;
    }

    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);
    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();

    ESP_LOGD(TAG, "Registering expose-based entities for %s (%u exposes)",
             conv->model ? conv->model : "unknown", conv->expose_count);

    for (uint8_t i = 0; i < conv->expose_count; i++) {
        const zb_expose_t *expose = &conv->exposes[i];

        if (esphome_adapter_is_expose_covered_by_cap(expose, dev->capabilities)) {
            continue;
        }

        if (!expose->name) {
            continue;
        }

        uint32_t key = esphome_adapter_make_expose_key(dev->id, i);

        switch (expose->type) {
        case ZB_EXPOSE_SENSOR:
            if (is_string_sensor_expose(expose)) {
                register_expose_text_sensor(dev, expose, key, device_id);
            } else {
                register_expose_sensor(dev, expose, key, device_id);
            }
            break;

        case ZB_EXPOSE_BINARY_SENSOR: {
            esphome_binary_sensor_config_t bconfig = {0};
            bconfig.key = key;
            bconfig.device_id = device_id;
            esphome_adapter_make_expose_name(dev, expose->name, bconfig.name, sizeof(bconfig.name));
            esphome_adapter_make_expose_unique_id(dev, expose->name, bconfig.unique_id, sizeof(bconfig.unique_id));
            bconfig.device_class = ESPHOME_BINARY_CLASS_NONE;
            bconfig.disabled_by_default = false;

            esp_err_t ret = esphome_entity_register_binary_sensor(&bconfig);
            if (ret == ESP_OK) {
                stats->entities_registered++;
                esphome_entity_update_binary_sensor(bconfig.key, false);
                ESP_LOGD(TAG, "Registered expose binary sensor: %s (key=0x%08lX)",
                         bconfig.name, (unsigned long)bconfig.key);
            }
            break;
        }

        case ZB_EXPOSE_SELECT:
            register_expose_select(dev, expose, key, device_id);
            break;

        default:
            break;
        }
    }
}

/* ============================================================================
 * Public API: State Update
 * ============================================================================ */

void esphome_adapter_exposes_update_states(const device_t *dev, cJSON *state)
{
    const zb_converter_def_t *conv =
        (const zb_converter_def_t *)device_registry_get_converter(dev->id);
    if (!conv || conv->expose_count == 0) {
        return;
    }

    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();
    bool expose_first = (conv->expose_count > 1);

    for (uint8_t i = 0; i < conv->expose_count; i++) {
        const zb_expose_t *expose = &conv->exposes[i];

        if (!expose_first) {
            if (esphome_adapter_is_expose_covered_by_cap(expose, dev->capabilities)) {
                continue;
            }
            if (!expose->name) {
                continue;
            }
        }

        uint32_t key = esphome_adapter_make_expose_key(dev->id, i);
        const char *prop = zb_expose_property(expose);
        cJSON *item = prop ? cJSON_GetObjectItem(state, prop) : NULL;
        if (!item) continue;

        switch (expose->type) {
        case ZB_EXPOSE_SENSOR:
            if (is_string_sensor_expose(expose)) {
                if (cJSON_IsString(item)) {
                    esphome_entity_update_text_sensor(key, item->valuestring);
                    stats->state_updates_sent++;
                    if (item->valuestring[0] != '\0') {
                        esphome_adapter_schedule_auto_clear(key, ESPHOME_AUTO_CLEAR_ACTION);
                    }
                }
            } else {
                if (cJSON_IsNumber(item)) {
                    esphome_entity_update_sensor(key, (float)item->valuedouble);
                    stats->state_updates_sent++;
                }
            }
            break;

        case ZB_EXPOSE_BINARY_SENSOR:
            if (cJSON_IsBool(item)) {
                bool bval = cJSON_IsTrue(item);
                esphome_entity_update_binary_sensor(key, bval);
                stats->state_updates_sent++;
                if (bval && expose->device_class &&
                    strcmp(expose->device_class, "vibration") == 0) {
                    esphome_adapter_schedule_auto_clear(key, ESPHOME_AUTO_CLEAR_VIBRATION);
                }
            }
            break;

        case ZB_EXPOSE_SELECT:
            if (cJSON_IsString(item)) {
                if (!(expose->access & EA_SET)) {
                    esphome_entity_update_text_sensor(key, item->valuestring);
                    stats->state_updates_sent++;
                    if (item->valuestring[0] != '\0') {
                        esphome_adapter_schedule_auto_clear(key, ESPHOME_AUTO_CLEAR_ACTION);
                    }
                } else {
                    esphome_entity_update_select(key, item->valuestring);
                    stats->state_updates_sent++;
                }
            }
            break;

        case ZB_EXPOSE_SWITCH:
            if (cJSON_IsBool(item)) {
                esphome_entity_update_switch(key, cJSON_IsTrue(item));
                stats->state_updates_sent++;
            } else if (cJSON_IsString(item)) {
                esphome_entity_update_switch(key, strcmp(item->valuestring, "ON") == 0);
                stats->state_updates_sent++;
            }
            break;

        case ZB_EXPOSE_NUMBER:
            if (cJSON_IsNumber(item)) {
                esphome_entity_update_number(key, (float)item->valuedouble);
                stats->state_updates_sent++;
            }
            break;

        case ZB_EXPOSE_TEXT:
            if (cJSON_IsString(item)) {
                esphome_entity_update_text(key, item->valuestring);
                stats->state_updates_sent++;
            }
            break;

        default:
            break;
        }
    }
}
