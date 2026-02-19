/**
 * @file esphome_device_registry.c
 * @brief ESPHome Entity Integration with Unified Device Registry
 *
 * Implements the bridge between ESPHome entity system and the unified
 * device_registry. ESPHome entities are registered as virtual devices
 * with DEV_PROTOCOL_VIRTUAL protocol type.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_device_registry.h"
#include "esphome_entities.h"
#include "core/device/device_registry.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ESPHOME_DEV_REG";

/* ============================================================================
 * Module State
 * ============================================================================ */

static bool s_initialized = false;
static size_t s_registered_count = 0;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void esphome_state_change_handler(esphome_entity_type_t entity_type,
                                          esphome_entity_key_t key,
                                          const void *state);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

device_id_t esphome_key_to_device_id(esphome_entity_key_t key)
{
    /* Combine prefix with key to create unique device ID */
    return (ESPHOME_DEVICE_ID_PREFIX << 32) | (uint64_t)key;
}

esp_err_t esphome_device_id_to_key(device_id_t id, esphome_entity_key_t *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check if this is an ESPHome device ID */
    uint32_t prefix = (uint32_t)(id >> 32);
    if (prefix != ESPHOME_DEVICE_ID_PREFIX) {
        return ESP_ERR_INVALID_ARG;
    }

    *key = (esphome_entity_key_t)(id & 0xFFFFFFFF);
    return ESP_OK;
}

bool esphome_is_virtual_device(device_id_t id)
{
    uint32_t prefix = (uint32_t)(id >> 32);
    return (prefix == ESPHOME_DEVICE_ID_PREFIX);
}

uint32_t esphome_entity_type_to_capabilities(esphome_entity_type_t entity_type)
{
    switch (entity_type) {
        case ESPHOME_ENTITY_SENSOR:
            /* Generic sensor - could be temperature, humidity, etc.
             * Caller should set specific capability based on device_class */
            return DEV_CAP_TEMPERATURE;  /* Default for diagnostic sensors */

        case ESPHOME_ENTITY_BINARY_SENSOR:
            /* Binary sensors can be motion, contact, etc.
             * Caller should set specific capability based on device_class */
            return DEV_CAP_MOTION;  /* Default */

        case ESPHOME_ENTITY_SWITCH:
            return DEV_CAP_ON_OFF;

        case ESPHOME_ENTITY_LIGHT:
            return DEV_CAP_ON_OFF | DEV_CAP_BRIGHTNESS;

        case ESPHOME_ENTITY_COVER:
            return DEV_CAP_COVER;

        case ESPHOME_ENTITY_FAN:
            return DEV_CAP_FAN;

        case ESPHOME_ENTITY_CLIMATE:
            return DEV_CAP_CLIMATE | DEV_CAP_TEMPERATURE;

        case ESPHOME_ENTITY_LOCK:
            return DEV_CAP_LOCK;

        case ESPHOME_ENTITY_TEXT_SENSOR:
        case ESPHOME_ENTITY_NUMBER:
        case ESPHOME_ENTITY_BUTTON:
        case ESPHOME_ENTITY_SELECT:
        case ESPHOME_ENTITY_TEXT:
        case ESPHOME_ENTITY_MEDIA_PLAYER:
        case ESPHOME_ENTITY_ALARM_PANEL:
        default:
            /* These entity types don't map directly to standard capabilities */
            return 0;
    }
}

device_t *esphome_device_registry_get(esphome_entity_key_t key)
{
    device_id_t id = esphome_key_to_device_id(key);
    return device_registry_get(id);
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

esp_err_t esphome_device_registry_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* Check prerequisites */
    if (!device_registry_is_initialized()) {
        ESP_LOGE(TAG, "device_registry not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing ESPHome device registry integration");

    s_registered_count = 0;
    s_initialized = true;

    ESP_LOGI(TAG, "ESPHome device registry integration initialized");
    return ESP_OK;
}

esp_err_t esphome_device_registry_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing ESPHome device registry integration");

    /* Note: We don't remove devices from registry here as they may still
     * be needed. The device_registry_deinit will clean them up. */

    s_registered_count = 0;
    s_initialized = false;

    return ESP_OK;
}

bool esphome_device_registry_is_initialized(void)
{
    return s_initialized;
}

/* ============================================================================
 * Device Registration
 * ============================================================================ */

esp_err_t esphome_device_registry_register(esphome_entity_type_t entity_type,
                                            esphome_entity_key_t key,
                                            const char *name,
                                            const char *unique_id)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    device_id_t id = esphome_key_to_device_id(key);

    /* Check if already registered */
    device_t *existing = device_registry_get(id);
    if (existing) {
        ESP_LOGD(TAG, "Entity key=%lu already registered", key);
        return ESP_OK;  /* Already registered, not an error */
    }

    /* Add new virtual device */
    device_t *dev = device_registry_add(id, DEV_PROTOCOL_VIRTUAL);
    if (!dev) {
        ESP_LOGE(TAG, "Failed to add virtual device for key=%lu", key);
        return ESP_ERR_NO_MEM;
    }

    /* Set device properties */
    if (name) {
        strlcpy(dev->friendly_name, name, sizeof(dev->friendly_name));
    }
    if (unique_id) {
        strlcpy(dev->model, unique_id, sizeof(dev->model));
    }
    strlcpy(dev->manufacturer, "ESPHome", sizeof(dev->manufacturer));

    /* Set capabilities based on entity type */
    dev->capabilities = esphome_entity_type_to_capabilities(entity_type);

    /* Virtual devices are always online */
    dev->availability = DEV_AVAIL_ONLINE;
    dev->interviewed = true;

    s_registered_count++;

    ESP_LOGI(TAG, "Registered ESPHome entity: key=%lu name='%s' type=%d caps=0x%lx",
             key, name ? name : "", entity_type, dev->capabilities);

    return ESP_OK;
}

esp_err_t esphome_device_registry_unregister(esphome_entity_key_t key)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    device_id_t id = esphome_key_to_device_id(key);
    esp_err_t ret = device_registry_remove(id);

    if (ret == ESP_OK && s_registered_count > 0) {
        s_registered_count--;
    }

    return ret;
}

/* ============================================================================
 * State Synchronization
 * ============================================================================ */

/**
 * @brief Build state JSON from ESPHome entity state
 *
 * @param[in] entity_type Entity type
 * @param[in] state Pointer to state structure
 * @return cJSON object (caller must NOT free - ownership transferred to registry)
 */
static cJSON *build_state_json(esphome_entity_type_t entity_type, const void *state)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return NULL;
    }

    switch (entity_type) {
        case ESPHOME_ENTITY_SENSOR: {
            const esphome_sensor_state_t *s = (const esphome_sensor_state_t *)state;
            if (!s->missing_state) {
                cJSON_AddNumberToObject(json, "value", s->state);
            }
            cJSON_AddBoolToObject(json, "available", !s->missing_state);
            break;
        }

        case ESPHOME_ENTITY_BINARY_SENSOR: {
            const esphome_binary_sensor_state_t *s = (const esphome_binary_sensor_state_t *)state;
            if (!s->missing_state) {
                cJSON_AddBoolToObject(json, "state", s->state);
            }
            cJSON_AddBoolToObject(json, "available", !s->missing_state);
            break;
        }

        case ESPHOME_ENTITY_SWITCH: {
            const esphome_switch_state_t *s = (const esphome_switch_state_t *)state;
            cJSON_AddBoolToObject(json, "state", s->state);
            break;
        }

        case ESPHOME_ENTITY_TEXT_SENSOR: {
            const esphome_text_sensor_state_t *s = (const esphome_text_sensor_state_t *)state;
            if (!s->missing_state) {
                cJSON_AddStringToObject(json, "state", s->state);
            }
            cJSON_AddBoolToObject(json, "available", !s->missing_state);
            break;
        }

        case ESPHOME_ENTITY_NUMBER: {
            const esphome_number_state_t *s = (const esphome_number_state_t *)state;
            if (!s->missing_state) {
                cJSON_AddNumberToObject(json, "state", s->state);
            }
            cJSON_AddBoolToObject(json, "available", !s->missing_state);
            break;
        }

        case ESPHOME_ENTITY_SELECT: {
            const esphome_select_state_t *s = (const esphome_select_state_t *)state;
            if (!s->missing_state) {
                cJSON_AddStringToObject(json, "state", s->state);
            }
            cJSON_AddBoolToObject(json, "available", !s->missing_state);
            break;
        }

        case ESPHOME_ENTITY_LIGHT: {
            const esphome_light_state_t *s = (const esphome_light_state_t *)state;
            cJSON_AddBoolToObject(json, "state", s->state);
            cJSON_AddNumberToObject(json, "brightness", s->brightness);
            if (s->color_mode != 0) {
                cJSON_AddNumberToObject(json, "color_temp", s->color_temp);
            }
            break;
        }

        case ESPHOME_ENTITY_COVER: {
            const esphome_cover_state_t *s = (const esphome_cover_state_t *)state;
            cJSON_AddNumberToObject(json, "position", s->position);
            cJSON_AddNumberToObject(json, "tilt", s->tilt);
            cJSON_AddNumberToObject(json, "operation", (int)s->current_operation);
            break;
        }

        case ESPHOME_ENTITY_FAN: {
            const esphome_fan_state_t *s = (const esphome_fan_state_t *)state;
            cJSON_AddBoolToObject(json, "state", s->state);
            cJSON_AddBoolToObject(json, "oscillating", s->oscillating);
            cJSON_AddNumberToObject(json, "speed_level", s->speed_level);
            cJSON_AddNumberToObject(json, "direction", (int)s->direction);
            break;
        }

        case ESPHOME_ENTITY_CLIMATE: {
            const esphome_climate_state_t *s = (const esphome_climate_state_t *)state;
            cJSON_AddNumberToObject(json, "mode", (int)s->mode);
            cJSON_AddNumberToObject(json, "action", (int)s->action);
            cJSON_AddNumberToObject(json, "current_temperature", s->current_temperature);
            cJSON_AddNumberToObject(json, "target_temperature", s->target_temperature);
            cJSON_AddNumberToObject(json, "target_temperature_low", s->target_temperature_low);
            cJSON_AddNumberToObject(json, "target_temperature_high", s->target_temperature_high);
            cJSON_AddNumberToObject(json, "fan_mode", (int)s->fan_mode);
            break;
        }

        case ESPHOME_ENTITY_LOCK: {
            const esphome_lock_entity_state_t *s = (const esphome_lock_entity_state_t *)state;
            cJSON_AddNumberToObject(json, "state", (int)s->state);
            break;
        }

        default:
            /* Unknown entity type - just store empty object */
            break;
    }

    return json;
}

esp_err_t esphome_device_registry_sync_state(esphome_entity_type_t entity_type,
                                              esphome_entity_key_t key,
                                              const void *state)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    device_id_t id = esphome_key_to_device_id(key);

    /* Check if device exists in registry */
    device_t *dev = device_registry_get(id);
    if (!dev) {
        ESP_LOGD(TAG, "Device for key=%lu not in registry, skipping sync", key);
        return ESP_ERR_NOT_FOUND;
    }

    /* Build state JSON */
    cJSON *json = build_state_json(entity_type, state);
    if (!json) {
        ESP_LOGE(TAG, "Failed to build state JSON for key=%lu", key);
        return ESP_ERR_NO_MEM;
    }

    /* Store state in device registry (registry takes ownership of json) */
    esp_err_t ret = device_registry_set_state(id, json);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set state for key=%lu: %s", key, esp_err_to_name(ret));
        cJSON_Delete(json);
        return ret;
    }

    /* Update last_seen timestamp */
    device_registry_update_last_seen(id);

    /* Publish state change event using standard evt_device_state_t.
     * ESPHome device_id goes into ieee_addr (both uint64_t).
     * json_state = NULL → adapter falls back to device_registry_get_state() */
    evt_device_state_t evt = {
        .ieee_addr = id,
        .short_addr = 0,
        .endpoint = 0,
        .cluster_id = 0,
        .attr_id = 0,
        .json_state = NULL,
    };

    ret = event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Failed to publish state event for key=%lu: %s",
                 key, esp_err_to_name(ret));
        /* Don't return error - state was successfully stored */
    }

    ESP_LOGD(TAG, "Synced state for ESPHome entity key=%lu type=%d", key, entity_type);
    return ESP_OK;
}

/* ============================================================================
 * Internal State Change Handler
 * ============================================================================ */

/**
 * @brief State change handler called by ESPHome entity system
 *
 * This function is registered as a callback with esphome_entities_set_state_callback()
 * to automatically sync state changes to the device registry.
 *
 * @note Reserved for callback registration with ESPHome entity system
 */
__attribute__((unused))
static void esphome_state_change_handler(esphome_entity_type_t entity_type,
                                          esphome_entity_key_t key,
                                          const void *state)
{
    if (!s_initialized) {
        return;
    }

    /* Sync state to device registry */
    esphome_device_registry_sync_state(entity_type, key, state);
}

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

void esphome_device_registry_log_stats(void)
{
    ESP_LOGI(TAG, "=== ESPHome Device Registry Stats ===");
    ESP_LOGI(TAG, "Initialized: %s", s_initialized ? "yes" : "no");
    ESP_LOGI(TAG, "Registered entities: %zu", s_registered_count);

    if (s_initialized) {
        device_registry_stats_t stats;
        device_registry_get_stats(&stats);
        ESP_LOGI(TAG, "Total devices in registry: %zu", stats.device_count);
    }
}
