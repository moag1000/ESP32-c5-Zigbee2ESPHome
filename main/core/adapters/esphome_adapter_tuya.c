/**
 * @file esphome_adapter_tuya.c
 * @brief Tuya Driver Entity Sub-module for ESPHome Adapter
 *
 * Extracted from esphome_adapter.c — handles all Tuya driver-based
 * entity registration, command callbacks, and state updates.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_adapter_tuya.h"
#include "core/memory/memory_manager_ng.h"
#include "esphome_adapter.h"
#include "esphome_adapter_internal.h"
#include "core/device/device_registry.h"
#include "esphome/esphome_api.h"
#include "esphome/esphome_entities.h"
#include "esphome/esphome_common.h"
#include "zigbee/tuya/tuya_device_driver.h"
#include "zigbee/tuya/tuya_driver_registry.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "ESPH_TUYA";

/* ============================================================================
 * Tuya Key Cache
 * ============================================================================ */

#define TUYA_KEY_CACHE_MAX  64

typedef struct {
    esphome_entity_key_t key;
    device_id_t device_id;
    char field_name[24];
    tuya_entity_type_t entity_type;
} tuya_key_cache_entry_t;

/* 3 KB that internal RAM does not have to spare — allocated on first use, in
 * PSRAM. Only reached from task context (entity registration and state
 * updates), never from an ISR. */
static tuya_key_cache_entry_t *s_tuya_key_cache = NULL;

static bool tuya_key_cache_ready(void)
{
    if (s_tuya_key_cache == NULL) {
        s_tuya_key_cache = mem_ng_calloc(TUYA_KEY_CACHE_MAX,
                                         sizeof(tuya_key_cache_entry_t), MEM_CAP_PSRAM);
        if (s_tuya_key_cache == NULL) {
            ESP_LOGE(TAG, "Failed to allocate Tuya key cache");
            return false;
        }
    }
    return true;
}
static uint8_t s_tuya_key_cache_count = 0;

static uint32_t make_tuya_entity_key(device_id_t id, const char *field_name)
{
    uint32_t hash = 5381;
    uint32_t id_low = (uint32_t)(id ^ (id >> 32));
    hash = ((hash << 5) + hash) + (id_low & 0xFF);
    hash = ((hash << 5) + hash) + ((id_low >> 8) & 0xFF);
    hash = ((hash << 5) + hash) + ((id_low >> 16) & 0xFF);
    for (const char *p = field_name; *p; p++) {
        hash = ((hash << 5) + hash) + (uint8_t)*p;
    }
    return ESPHOME_ADAPTER_TUYA_KEY_OFFSET | (hash & 0x0FFFFFFF);
}

static void tuya_key_cache_add(esphome_entity_key_t key, device_id_t id,
                                const char *field_name, tuya_entity_type_t type)
{
    if (!tuya_key_cache_ready()) {
        return;
    }

    for (uint8_t i = 0; i < s_tuya_key_cache_count; i++) {
        if (s_tuya_key_cache[i].key == key) {
            return;
        }
    }
    if (s_tuya_key_cache_count >= TUYA_KEY_CACHE_MAX) {
        ESP_LOGW(TAG, "Tuya key cache full (%d entries)", TUYA_KEY_CACHE_MAX);
        return;
    }
    tuya_key_cache_entry_t *e = &s_tuya_key_cache[s_tuya_key_cache_count++];
    e->key = key;
    e->device_id = id;
    strncpy(e->field_name, field_name, sizeof(e->field_name) - 1);
    e->field_name[sizeof(e->field_name) - 1] = '\0';
    e->entity_type = type;
}

static tuya_key_cache_entry_t *tuya_key_cache_find(esphome_entity_key_t key)
{
    if (s_tuya_key_cache == NULL) {
        return NULL;
    }

    for (uint8_t i = 0; i < s_tuya_key_cache_count; i++) {
        if (s_tuya_key_cache[i].key == key) {
            return &s_tuya_key_cache[i];
        }
    }
    return NULL;
}

/* ============================================================================
 * Tuya Command Helpers
 * ============================================================================ */

static bool tuya_find_device_driver(esphome_entity_key_t key,
                                     device_t **out_dev,
                                     const tuya_device_driver_t **out_drv,
                                     const char **out_field)
{
    tuya_key_cache_entry_t *entry = tuya_key_cache_find(key);
    if (!entry) return false;

    device_t *dev = device_registry_get(entry->device_id);
    if (!dev) return false;

    const tuya_device_driver_t *drv = tuya_driver_get(dev->proto.zigbee.short_addr);
    if (!drv) return false;

    *out_dev = dev;
    *out_drv = drv;
    *out_field = entry->field_name;
    return true;
}

static esp_err_t tuya_send_field_command(esphome_entity_key_t key, cJSON *json)
{
    device_t *dev = NULL;
    const tuya_device_driver_t *drv = NULL;
    const char *field = NULL;

    if (!tuya_find_device_driver(key, &dev, &drv, &field)) {
        ESP_LOGW(TAG, "Tuya command: no device for key 0x%08lX", (unsigned long)key);
        cJSON_Delete(json);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t ep = dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    ESP_LOGI(TAG, "Tuya command: 0x%04X field=%s", dev->proto.zigbee.short_addr, field);

    esp_err_t ret = drv->handle_command(dev->proto.zigbee.short_addr, ep, json);
    cJSON_Delete(json);
    return ret;
}

/* ============================================================================
 * Tuya Command Callbacks
 * ============================================================================ */

static esp_err_t tuya_switch_command_callback(esphome_entity_key_t key, bool state)
{
    esphome_adapter_get_stats_ptr()->commands_received++;

    tuya_key_cache_entry_t *entry = tuya_key_cache_find(key);
    if (!entry) return ESP_ERR_NOT_FOUND;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(json, entry->field_name, state);

    esp_err_t ret = tuya_send_field_command(key, json);
    if (ret == ESP_OK) {
        esphome_entity_update_switch(key, state);
    }
    return ret;
}

static esp_err_t tuya_number_command_callback(esphome_entity_key_t key, float value)
{
    esphome_adapter_get_stats_ptr()->commands_received++;

    tuya_key_cache_entry_t *entry = tuya_key_cache_find(key);
    if (!entry) return ESP_ERR_NOT_FOUND;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(json, entry->field_name, (double)value);

    esp_err_t ret = tuya_send_field_command(key, json);
    if (ret == ESP_OK) {
        esphome_entity_update_number(key, value);
    }
    return ret;
}

static esp_err_t tuya_select_command_callback(esphome_entity_key_t key, const char *option)
{
    esphome_adapter_get_stats_ptr()->commands_received++;

    tuya_key_cache_entry_t *entry = tuya_key_cache_find(key);
    if (!entry) return ESP_ERR_NOT_FOUND;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(json, entry->field_name, option);

    esp_err_t ret = tuya_send_field_command(key, json);
    if (ret == ESP_OK) {
        esphome_entity_update_select(key, option);
    }
    return ret;
}

static esp_err_t tuya_text_command_callback(esphome_entity_key_t key, const char *value)
{
    esphome_adapter_get_stats_ptr()->commands_received++;

    tuya_key_cache_entry_t *entry = tuya_key_cache_find(key);
    if (!entry) return ESP_ERR_NOT_FOUND;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(json, entry->field_name, value);

    esp_err_t ret = tuya_send_field_command(key, json);
    if (ret == ESP_OK) {
        esphome_entity_update_text(key, value);
    }
    return ret;
}

/* ============================================================================
 * Tuya Entity Registration
 * ============================================================================ */

static const tuya_entity_meta_t *find_tuya_meta(const tuya_device_driver_t *drv,
                                                  const char *field_name)
{
    if (!drv->entity_meta || drv->entity_meta_count == 0) {
        return NULL;
    }
    for (uint8_t i = 0; i < drv->entity_meta_count; i++) {
        if (strcmp(drv->entity_meta[i].field_name, field_name) == 0) {
            return &drv->entity_meta[i];
        }
    }
    return NULL;
}

esp_err_t esphome_adapter_tuya_register(const device_t *dev)
{
    const tuya_device_driver_t *drv = tuya_driver_get(dev->proto.zigbee.short_addr);
    if (!drv || !drv->build_state_json) {
        return ESP_OK;
    }

    cJSON *state_json = drv->build_state_json(dev->proto.zigbee.short_addr);
    if (!state_json) {
        return ESP_OK;
    }

    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);
    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();
    int registered = 0;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, state_json) {
        const char *field = item->string;
        if (!field) continue;

        if (strcmp(field, "linkquality") == 0) continue;

        const tuya_entity_meta_t *meta = find_tuya_meta(drv, field);

        tuya_entity_type_t etype = TUYA_ENTITY_AUTO;
        if (meta) {
            etype = meta->entity_type;
        }

        if (etype == TUYA_ENTITY_SKIP) {
            continue;
        }

        if (etype == TUYA_ENTITY_AUTO) {
            if (cJSON_IsBool(item)) {
                etype = TUYA_ENTITY_SWITCH;
            } else if (cJSON_IsNumber(item)) {
                etype = TUYA_ENTITY_SENSOR;
            } else if (cJSON_IsString(item)) {
                if (strcmp(field, "state") == 0 && (dev->capabilities & DEV_CAP_ON_OFF)) {
                    continue;
                }
                etype = TUYA_ENTITY_TEXT_SENSOR;
            } else {
                continue;
            }
        }

        uint32_t key = make_tuya_entity_key(dev->id, field);

        char name[64];
        esphome_adapter_make_expose_name(dev, field, name, sizeof(name));

        char unique_id[64];
        snprintf(unique_id, sizeof(unique_id), "0x%016llX_tuya_%s",
                 (unsigned long long)dev->id, field);

        switch (etype) {
        case TUYA_ENTITY_SWITCH: {
            esphome_switch_config_t cfg = {0};
            cfg.key = key;
            cfg.device_id = device_id;
            strncpy(cfg.name, name, sizeof(cfg.name) - 1);
            strncpy(cfg.unique_id, unique_id, sizeof(cfg.unique_id) - 1);
            if (meta && meta->icon) {
                strncpy(cfg.icon, meta->icon, sizeof(cfg.icon) - 1);
            } else {
                strncpy(cfg.icon, "mdi:toggle-switch", sizeof(cfg.icon) - 1);
            }
            cfg.command_callback = tuya_switch_command_callback;
            if (esphome_entity_register_switch(&cfg) == ESP_OK) {
                esphome_entity_update_switch(key, cJSON_IsTrue(item));
                tuya_key_cache_add(key, dev->id, field, etype);
                stats->entities_registered++;
                registered++;
            }
            break;
        }

        case TUYA_ENTITY_SENSOR: {
            esphome_sensor_config_t cfg = {0};
            cfg.key = key;
            cfg.device_id = device_id;
            strncpy(cfg.name, name, sizeof(cfg.name) - 1);
            strncpy(cfg.unique_id, unique_id, sizeof(cfg.unique_id) - 1);
            if (meta && meta->icon) {
                strncpy(cfg.icon, meta->icon, sizeof(cfg.icon) - 1);
            }
            if (meta && meta->unit) {
                strncpy(cfg.unit_of_measurement, meta->unit,
                        sizeof(cfg.unit_of_measurement) - 1);
            }
            if (meta && meta->device_class) {
                cfg.device_class = esphome_adapter_expose_dc_to_sensor_class(meta->device_class);
            }
            cfg.state_class = ESPHOME_STATE_CLASS_MEASUREMENT;
            cfg.accuracy_decimals = 0;
            if (esphome_entity_register_sensor(&cfg) == ESP_OK) {
                esphome_entity_update_sensor(key,
                    cJSON_IsNumber(item) ? (float)item->valuedouble : NAN);
                tuya_key_cache_add(key, dev->id, field, etype);
                stats->entities_registered++;
                registered++;
            }
            break;
        }

        case TUYA_ENTITY_NUMBER: {
            esphome_number_config_t cfg = {0};
            cfg.key = key;
            cfg.device_id = device_id;
            strncpy(cfg.name, name, sizeof(cfg.name) - 1);
            strncpy(cfg.unique_id, unique_id, sizeof(cfg.unique_id) - 1);
            if (meta && meta->icon) {
                strncpy(cfg.icon, meta->icon, sizeof(cfg.icon) - 1);
            }
            if (meta && meta->unit) {
                strncpy(cfg.unit_of_measurement, meta->unit,
                        sizeof(cfg.unit_of_measurement) - 1);
            }
            cfg.min_value = meta ? meta->min : 0;
            cfg.max_value = meta ? meta->max : 100;
            cfg.step = meta ? meta->step : 1;
            cfg.mode = ESPHOME_NUMBER_MODE_BOX;
            cfg.command_callback = tuya_number_command_callback;
            if (esphome_entity_register_number(&cfg) == ESP_OK) {
                esphome_entity_update_number(key,
                    cJSON_IsNumber(item) ? (float)item->valuedouble : NAN);
                tuya_key_cache_add(key, dev->id, field, etype);
                stats->entities_registered++;
                registered++;
            }
            break;
        }

        case TUYA_ENTITY_SELECT: {
            esphome_select_config_t cfg = {0};
            cfg.key = key;
            cfg.device_id = device_id;
            strncpy(cfg.name, name, sizeof(cfg.name) - 1);
            strncpy(cfg.unique_id, unique_id, sizeof(cfg.unique_id) - 1);
            if (meta && meta->icon) {
                strncpy(cfg.icon, meta->icon, sizeof(cfg.icon) - 1);
            } else {
                strncpy(cfg.icon, "mdi:format-list-bulleted", sizeof(cfg.icon) - 1);
            }
            if (meta && meta->options && meta->option_count > 0) {
                cfg.option_count = meta->option_count;
                if (cfg.option_count > ESPHOME_MAX_SELECT_OPTIONS) {
                    cfg.option_count = ESPHOME_MAX_SELECT_OPTIONS;
                }
                for (uint8_t o = 0; o < cfg.option_count; o++) {
                    strncpy(cfg.options[o], meta->options[o], 31);
                }
            }
            cfg.command_callback = tuya_select_command_callback;
            if (esphome_entity_register_select(&cfg) == ESP_OK) {
                if (cJSON_IsString(item) && item->valuestring) {
                    esphome_entity_update_select(key, item->valuestring);
                }
                tuya_key_cache_add(key, dev->id, field, etype);
                stats->entities_registered++;
                registered++;
            }
            break;
        }

        case TUYA_ENTITY_TEXT_SENSOR: {
            esphome_text_sensor_config_t cfg = {0};
            cfg.key = key;
            cfg.device_id = device_id;
            strncpy(cfg.name, name, sizeof(cfg.name) - 1);
            strncpy(cfg.unique_id, unique_id, sizeof(cfg.unique_id) - 1);
            if (meta && meta->icon) {
                strncpy(cfg.icon, meta->icon, sizeof(cfg.icon) - 1);
            }
            if (esphome_entity_register_text_sensor(&cfg) == ESP_OK) {
                if (cJSON_IsString(item) && item->valuestring) {
                    esphome_entity_update_text_sensor(key, item->valuestring);
                } else {
                    esphome_entity_update_text_sensor(key, "");
                }
                tuya_key_cache_add(key, dev->id, field, etype);
                stats->entities_registered++;
                registered++;
            }
            break;
        }

        case TUYA_ENTITY_TEXT: {
            esphome_text_config_t cfg = {0};
            cfg.key = key;
            cfg.device_id = device_id;
            strncpy(cfg.name, name, sizeof(cfg.name) - 1);
            strncpy(cfg.unique_id, unique_id, sizeof(cfg.unique_id) - 1);
            if (meta && meta->icon) {
                strncpy(cfg.icon, meta->icon, sizeof(cfg.icon) - 1);
            }
            cfg.min_length = 0;
            cfg.max_length = 127;
            cfg.mode = ESPHOME_TEXT_MODE_TEXT;
            cfg.command_callback = tuya_text_command_callback;
            if (esphome_entity_register_text(&cfg) == ESP_OK) {
                if (cJSON_IsString(item) && item->valuestring) {
                    esphome_entity_update_text(key, item->valuestring);
                } else {
                    esphome_entity_update_text(key, "");
                }
                tuya_key_cache_add(key, dev->id, field, etype);
                stats->entities_registered++;
                registered++;
            }
            break;
        }

        case TUYA_ENTITY_BINARY_SENSOR: {
            esphome_binary_sensor_config_t cfg = {0};
            cfg.key = key;
            cfg.device_id = device_id;
            strncpy(cfg.name, name, sizeof(cfg.name) - 1);
            strncpy(cfg.unique_id, unique_id, sizeof(cfg.unique_id) - 1);
            if (meta && meta->icon) {
                strncpy(cfg.icon, meta->icon, sizeof(cfg.icon) - 1);
            }
            cfg.device_class = ESPHOME_BINARY_CLASS_NONE;
            if (esphome_entity_register_binary_sensor(&cfg) == ESP_OK) {
                esphome_entity_update_binary_sensor(key, cJSON_IsTrue(item));
                tuya_key_cache_add(key, dev->id, field, etype);
                stats->entities_registered++;
                registered++;
            }
            break;
        }

        default:
            break;
        }
    }

    cJSON_Delete(state_json);

    if (registered > 0) {
        ESP_LOGI(TAG, "Registered %d Tuya entities for %s (driver=%s)",
                 registered, dev->friendly_name, drv->name);
    }

    return ESP_OK;
}

/* ============================================================================
 * Tuya State Update
 * ============================================================================ */

void esphome_adapter_tuya_update_states(const device_t *dev, cJSON *state)
{
    esphome_adapter_stats_t *stats = esphome_adapter_get_stats_ptr();

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, state) {
        const char *field = item->string;
        if (!field) continue;

        uint32_t key = make_tuya_entity_key(dev->id, field);
        tuya_key_cache_entry_t *entry = tuya_key_cache_find(key);
        if (!entry) continue;

        switch (entry->entity_type) {
        case TUYA_ENTITY_SWITCH:
            if (cJSON_IsBool(item)) {
                esphome_entity_update_switch(key, cJSON_IsTrue(item));
                stats->state_updates_sent++;
            }
            break;

        case TUYA_ENTITY_SENSOR:
            if (cJSON_IsNumber(item)) {
                esphome_entity_update_sensor(key, (float)item->valuedouble);
                stats->state_updates_sent++;
            }
            break;

        case TUYA_ENTITY_NUMBER:
            if (cJSON_IsNumber(item)) {
                esphome_entity_update_number(key, (float)item->valuedouble);
                stats->state_updates_sent++;
            }
            break;

        case TUYA_ENTITY_SELECT:
            if (cJSON_IsString(item) && item->valuestring) {
                esphome_entity_update_select(key, item->valuestring);
                stats->state_updates_sent++;
            }
            break;

        case TUYA_ENTITY_TEXT_SENSOR:
            if (cJSON_IsString(item) && item->valuestring) {
                esphome_entity_update_text_sensor(key, item->valuestring);
                stats->state_updates_sent++;
            }
            break;

        case TUYA_ENTITY_TEXT:
            if (cJSON_IsString(item) && item->valuestring) {
                esphome_entity_update_text(key, item->valuestring);
                stats->state_updates_sent++;
            }
            break;

        case TUYA_ENTITY_BINARY_SENSOR:
            if (cJSON_IsBool(item)) {
                esphome_entity_update_binary_sensor(key, cJSON_IsTrue(item));
                stats->state_updates_sent++;
            }
            break;

        default:
            break;
        }
    }
}

/* ============================================================================
 * Tuya Device Removal
 * ============================================================================ */

void esphome_adapter_tuya_remove_device(device_id_t id)
{
    uint8_t dst = 0;
    if (s_tuya_key_cache == NULL) {
        return;
    }

    for (uint8_t src = 0; src < s_tuya_key_cache_count; src++) {
        if (s_tuya_key_cache[src].device_id != id) {
            if (dst != src) {
                s_tuya_key_cache[dst] = s_tuya_key_cache[src];
            }
            dst++;
        }
    }
    if (dst < s_tuya_key_cache_count) {
        ESP_LOGD(TAG, "Removed %d Tuya key cache entries for device",
                 s_tuya_key_cache_count - dst);
    }
    s_tuya_key_cache_count = dst;
}
