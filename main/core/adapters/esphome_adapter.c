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
#include "zigbee/tuya/tuya_device_driver.h"
#include "zigbee/tuya/tuya_driver_registry.h"
#include "cJSON.h"
#include "esphome/esphome_gateway_services.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "adapter_interface.h"
#include "esphome_adapter_gateway.h"
#include "esphome_adapter_diagnostics.h"
#include "esphome_adapter_tuya.h"
#include "esphome_adapter_exposes.h"
#include "esphome_adapter_internal.h"
#include "esphome_adapter_cap_metadata.h"

static const char *TAG = "ESPH_ADAPTER";

/* ============================================================================
 * Static State
 * ============================================================================ */

static bool s_initialized = false;
static esphome_adapter_stats_t s_stats = {0};

/* Track which devices have entities registered (prevent duplicate registration) */
#define MAX_REGISTERED_DEVICES 64
static device_id_t s_registered_ids[MAX_REGISTERED_DEVICES];
static uint8_t     s_registered_count = 0;

static bool is_device_entities_registered(device_id_t id)
{
    for (uint8_t i = 0; i < s_registered_count; i++) {
        if (s_registered_ids[i] == id) {
            return true;
        }
    }
    return false;
}

static void mark_device_entities_registered(device_id_t id)
{
    if (s_registered_count < MAX_REGISTERED_DEVICES) {
        s_registered_ids[s_registered_count++] = id;
    }
}

static void unmark_device_entities_registered(device_id_t id)
{
    for (uint8_t i = 0; i < s_registered_count; i++) {
        if (s_registered_ids[i] == id) {
            s_registered_ids[i] = s_registered_ids[--s_registered_count];
            return;
        }
    }
}

/* Auto-clear timers for event-type entities (vibration, action) */
static esp_timer_handle_t s_vibration_clear_timer;
static esphome_entity_key_t s_vibration_clear_key;
static esp_timer_handle_t s_action_clear_timer;
static esphome_entity_key_t s_action_clear_key;

static void vibration_auto_clear_cb(void *arg)
{
    (void)arg;
    if (s_vibration_clear_key != 0) {
        esphome_entity_update_binary_sensor(s_vibration_clear_key, false);
        ESP_LOGD("ESPH_ADAPTER", "Auto-cleared vibration (key=0x%08lX)",
                 (unsigned long)s_vibration_clear_key);
    }
}

static void action_auto_clear_cb(void *arg)
{
    (void)arg;
    if (s_action_clear_key != 0) {
        esphome_entity_update_text_sensor(s_action_clear_key, "");
        ESP_LOGD("ESPH_ADAPTER", "Auto-cleared action (key=0x%08lX)",
                 (unsigned long)s_action_clear_key);
    }
}

/* ============================================================================
 * Forward Declarations - Event Handlers
 * ============================================================================ */

/** One-shot timer that collapses a burst of interviews into one disconnect */
static esp_timer_handle_t s_rediscover_timer = NULL;

static void rediscovery_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Disconnecting ESPHome clients to trigger entity re-discovery");
    esphome_api_disconnect_all_clients();
}

static void schedule_rediscovery(void)
{
    if (s_rediscover_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = rediscovery_timer_cb,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "esph_rediscover",
        };
        if (esp_timer_create(&args, &s_rediscover_timer) != ESP_OK) {
            ESP_LOGW(TAG, "No re-discovery timer — disconnecting immediately");
            esphome_api_disconnect_all_clients();
            return;
        }
    }

    if (esp_timer_is_active(s_rediscover_timer)) {
        ESP_LOGD(TAG, "Re-discovery already pending, folding this interview in");
        return;
    }

    esp_timer_start_once(s_rediscover_timer,
                         (uint64_t)ESPHOME_REDISCOVERY_DELAY_MS * 1000);
}

static void handle_device_state_changed(event_type_t type, void *data,
                                         size_t data_size, void *ctx);
static void handle_device_joined(event_type_t type, void *data,
                                  size_t data_size, void *ctx);
static void handle_device_left(event_type_t type, void *data,
                                size_t data_size, void *ctx);
static void handle_device_availability_changed(event_type_t type, void *data,
                                                size_t data_size, void *ctx);
static void handle_esphome_connected(event_type_t type, void *data,
                                      size_t data_size, void *ctx);
static void handle_esphome_disconnected(event_type_t type, void *data,
                                         size_t data_size, void *ctx);
static void handle_device_interviewed(event_type_t type, void *data,
                                       size_t data_size, void *ctx);

/* ============================================================================
 * Forward Declarations - Entity Registration
 * ============================================================================ */

static esp_err_t register_sensor_entity(const device_t *dev, device_capability_t cap);
static esp_err_t register_binary_sensor_entity(const device_t *dev, device_capability_t cap);
static esp_err_t register_switch_entity(const device_t *dev);
static esp_err_t register_light_entity(const device_t *dev);
static esp_err_t register_cover_entity(const device_t *dev);
static esp_err_t register_fan_entity(const device_t *dev);
static esp_err_t register_climate_entity(const device_t *dev);
static esp_err_t register_lock_entity(const device_t *dev);

/* ============================================================================
 * Forward Declarations - Command Callbacks
 * ============================================================================ */

static esp_err_t switch_command_callback(esphome_entity_key_t key, bool state);
static esp_err_t light_command_callback(esphome_entity_key_t key,
                                         const esphome_light_command_t *cmd);
static esp_err_t cover_command_callback(esphome_entity_key_t key,
                                         const esphome_cover_command_t *cmd);
static esp_err_t fan_command_callback(esphome_entity_key_t key,
                                       const esphome_fan_command_t *cmd);
static esp_err_t climate_command_callback(esphome_entity_key_t key,
                                           const esphome_climate_command_t *cmd);
static esp_err_t lock_command_callback(esphome_entity_key_t key,
                                        esphome_lock_command_t cmd);

/* ============================================================================
 * Helper Functions - Entity Key Management
 * ============================================================================ */

/**
 * @brief Convert bitmask capability to sequential index (0-28)
 *
 * DEV_CAP_* values are (1 << N), so we find N via bit position.
 */
static uint8_t cap_to_index(device_capability_t cap)
{
    uint32_t v = (uint32_t)cap;
    uint8_t idx = 0;
    while (v > 1) { v >>= 1; idx++; }
    return idx;
}

uint32_t esphome_adapter_make_entity_key(device_id_t id, device_capability_t cap)
{
    /*
     * Key structure (32 bits):
     * - Bits 31-28: Adapter identifier (0x1 to distinguish from other entities)
     * - Bits 27-23: Capability index (0-28, from bit position of DEV_CAP_*)
     * - Bits 22-0:  Lower 23 bits of device ID hash
     */
    uint32_t id_hash = (uint32_t)(id ^ (id >> 32)); /* Simple hash of 64-bit ID */
    uint32_t cap_idx = cap_to_index(cap);
    uint32_t key = ESPHOME_ADAPTER_KEY_OFFSET;
    key |= (cap_idx & 0x1F) << 23;
    key |= (id_hash & 0x007FFFFF);
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
        uint8_t cap_idx = (key >> 23) & 0x1F;
        *cap = (device_capability_t)(1U << cap_idx);
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
    const cap_metadata_t *meta = cap_metadata_find(cap);
    const char *suffix = meta ? meta->display_name : "Entity";

    if (dev->protocol == DEV_PROTOCOL_ZIGBEE) {
        snprintf(buf, buf_size, "%s", suffix);
    } else {
        const char *name = dev->friendly_name[0] ? dev->friendly_name : "Device";
        snprintf(buf, buf_size, "%s %s", name, suffix);
    }
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
    const cap_metadata_t *meta = cap_metadata_find(cap);
    const char *cap_str = meta ? meta->key : "entity";

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
 * Helper Functions - Color Conversion
 * ============================================================================ */

/**
 * @brief Clamp float to 0.0-1.0 range
 */
static inline float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/**
 * @brief Convert CIE 1931 XY color to sRGB
 *
 * Uses D65 illuminant and standard XYZ→sRGB matrix.
 * Brightness Y is assumed to be 1.0 (max brightness).
 */
static void color_xy_to_rgb(float x, float y, float *r, float *g, float *b)
{
    /* Avoid division by zero */
    if (y < 0.0001f) {
        *r = *g = *b = 0.0f;
        return;
    }

    /* CIE xy → XYZ (assume Y=1.0) */
    float Y = 1.0f;
    float X = (Y / y) * x;
    float Z = (Y / y) * (1.0f - x - y);

    /* XYZ → linear sRGB (D65, Wide RGB gamut matrix) */
    float lr =  X * 1.656492f - Y * 0.354851f - Z * 0.255038f;
    float lg = -X * 0.707196f + Y * 1.655397f + Z * 0.036152f;
    float lb =  X * 0.051713f - Y * 0.121364f + Z * 1.011530f;

    /* Clamp and output */
    *r = clamp01(lr);
    *g = clamp01(lg);
    *b = clamp01(lb);
}

/**
 * @brief Convert HSV color to RGB
 *
 * @param h Hue in degrees (0-360)
 * @param s Saturation (0.0-1.0 or 0-100, auto-detected)
 * @param r,g,b Output RGB values (0.0-1.0)
 */
static void color_hs_to_rgb(float h, float s, float *r, float *g, float *b)
{
    /* Normalize: Zigbee hue is 0-360, saturation might be 0-100 or 0-1 */
    if (s > 1.0f) s /= 100.0f;
    h = fmodf(h, 360.0f);
    if (h < 0.0f) h += 360.0f;

    float c = s;  /* V=1.0, so C = V*S = S */
    float hp = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));

    float r1, g1, b1;
    if (hp < 1.0f)      { r1 = c; g1 = x; b1 = 0; }
    else if (hp < 2.0f) { r1 = x; g1 = c; b1 = 0; }
    else if (hp < 3.0f) { r1 = 0; g1 = c; b1 = x; }
    else if (hp < 4.0f) { r1 = 0; g1 = x; b1 = c; }
    else if (hp < 5.0f) { r1 = x; g1 = 0; b1 = c; }
    else                { r1 = c; g1 = 0; b1 = x; }

    float m = 1.0f - c;
    *r = clamp01(r1 + m);
    *g = clamp01(g1 + m);
    *b = clamp01(b1 + m);
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

    /* Set device class, unit, icon, accuracy from cap metadata table */
    const cap_metadata_t *meta = cap_metadata_find(cap);
    if (meta) {
        config.device_class = meta->sensor_class;
        if (meta->unit) {
            strncpy(config.unit_of_measurement, meta->unit, sizeof(config.unit_of_measurement) - 1);
        }
        if (meta->icon) {
            strncpy(config.icon, meta->icon, sizeof(config.icon) - 1);
        }
        config.accuracy_decimals = meta->accuracy_decimals;
        config.state_class = meta->state_class;
        config.entity_category = meta->entity_category;
    } else {
        config.device_class = ESPHOME_SENSOR_CLASS_NONE;
        config.accuracy_decimals = 1;
    }

    /* Set default state_class only if not already set by capability-specific code
     * (e.g., energy uses TOTAL_INCREASING, not MEASUREMENT) */
    if (config.state_class == 0) {
        config.state_class = ESPHOME_STATE_CLASS_MEASUREMENT;
    }
    config.force_update = false;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_sensor(&config);
    if (ret == ESP_OK) {
        s_stats.entities_registered++;
        /* Set initial NaN state so entity is "available" in HA immediately.
         * Without this, missing_state=true and HA shows "Unavailable" until
         * the Zigbee device sends its first report (hours for battery devices). */
        esphome_entity_update_sensor(config.key, NAN);
        ESP_LOGD(TAG, "Registered sensor: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    } else if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Sensor max capacity reached, cannot register: %s", config.name);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "Sensor already exists: %s", config.name);
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

    /* Set device class, icon from cap metadata table */
    const cap_metadata_t *bmeta = cap_metadata_find(cap);
    if (bmeta) {
        config.device_class = bmeta->binary_class;
        if (bmeta->icon) {
            strncpy(config.icon, bmeta->icon, sizeof(config.icon) - 1);
        }
        config.entity_category = bmeta->entity_category;
    } else {
        config.device_class = ESPHOME_BINARY_CLASS_NONE;
    }

    config.is_status_binary_sensor = false;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_binary_sensor(&config);
    if (ret == ESP_OK) {
        s_stats.entities_registered++;
        /* Set initial false state so entity is "available" in HA immediately.
         * Without this, missing_state=true and HA shows "Unavailable" until
         * the Zigbee device sends its first report. */
        esphome_entity_update_binary_sensor(config.key, false);
        ESP_LOGD(TAG, "Registered binary sensor: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    } else if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Binary sensor max capacity reached, cannot register: %s", config.name);
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
        ESP_LOGW(TAG, "Switch max capacity reached, cannot register: %s", config.name);
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
        ESP_LOGW(TAG, "Light max capacity reached, cannot register: %s", config.name);
    } else {
        ESP_LOGW(TAG, "Failed to register light %s: %s",
                 config.name, esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================================
 * Cover Entity Registration
 * ============================================================================ */

static esp_err_t cover_command_callback(esphome_entity_key_t key,
                                         const esphome_cover_command_t *cmd)
{
    ESP_LOGI(TAG, "Cover command: key=0x%08lX, pos=%.2f, tilt=%.2f, stop=%d",
             (unsigned long)key,
             cmd->has_position ? cmd->position : -1.0f,
             cmd->has_tilt ? cmd->tilt : -1.0f,
             cmd->stop);

    s_stats.commands_received++;

    device_t *dev = find_device_for_entity_key(key, NULL);
    if (!dev) {
        ESP_LOGW(TAG, "Cover command: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    if (cmd->stop) {
        cJSON_AddStringToObject(json, "state", "STOP");
    } else if (cmd->has_position) {
        /* ESPHome position: 0.0=closed, 1.0=open → Z2M position: 0=closed, 100=open */
        cJSON_AddNumberToObject(json, "position", (int)(cmd->position * 100.0f));
    }
    if (cmd->has_tilt) {
        cJSON_AddNumberToObject(json, "tilt", (int)(cmd->tilt * 100.0f));
    }

    uint8_t ep = dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    esp_err_t ret = zb_converter_handle_command(
        dev->proto.zigbee.short_addr, ep, json);
    cJSON_Delete(json);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cover command failed for 0x%04X: %s",
                 dev->proto.zigbee.short_addr, esp_err_to_name(ret));
    }

    /* Optimistic state update */
    esphome_cover_state_t state = {0};
    state.key = key;
    esphome_entity_get_cover(key, &state);
    if (cmd->has_position) state.position = cmd->position;
    if (cmd->has_tilt) state.tilt = cmd->tilt;
    esphome_entity_update_cover(key, &state);

    return ret;
}

static esp_err_t register_cover_entity(const device_t *dev)
{
    esphome_cover_config_t config = {0};
    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);

    config.key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_COVER);
    config.device_id = device_id;
    make_entity_name(dev, DEV_CAP_COVER, config.name, sizeof(config.name));
    make_unique_id(dev, DEV_CAP_COVER, config.unique_id, sizeof(config.unique_id));

    strncpy(config.icon, "mdi:window-shutter", sizeof(config.icon) - 1);
    config.device_class = ESPHOME_COVER_CLASS_SHUTTER;
    config.assumed_state = false;
    config.supports_position = true;
    config.supports_tilt = true;
    config.command_callback = cover_command_callback;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_cover(&config);
    if (ret == ESP_OK) {
        s_stats.entities_registered++;
        ESP_LOGD(TAG, "Registered cover: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    } else if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Cover max capacity reached, cannot register: %s", config.name);
    } else {
        ESP_LOGW(TAG, "Failed to register cover %s: %s",
                 config.name, esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================================
 * Fan Entity Registration
 * ============================================================================ */

static esp_err_t fan_command_callback(esphome_entity_key_t key,
                                       const esphome_fan_command_t *cmd)
{
    ESP_LOGI(TAG, "Fan command: key=0x%08lX, state=%s, speed=%ld",
             (unsigned long)key,
             cmd->has_state ? (cmd->state ? "ON" : "OFF") : "-",
             cmd->has_speed_level ? (long)cmd->speed_level : -1L);

    s_stats.commands_received++;

    device_t *dev = find_device_for_entity_key(key, NULL);
    if (!dev) {
        ESP_LOGW(TAG, "Fan command: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    if (cmd->has_state) {
        cJSON_AddStringToObject(json, "state", cmd->state ? "ON" : "OFF");
    }
    if (cmd->has_speed_level) {
        /* Map speed level to fan_mode string for Z2M */
        const char *mode = "low";
        if (cmd->speed_level >= 3) mode = "high";
        else if (cmd->speed_level >= 2) mode = "medium";
        cJSON_AddStringToObject(json, "fan_mode", mode);
    }

    uint8_t ep = dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    esp_err_t ret = zb_converter_handle_command(
        dev->proto.zigbee.short_addr, ep, json);
    cJSON_Delete(json);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Fan command failed for 0x%04X: %s",
                 dev->proto.zigbee.short_addr, esp_err_to_name(ret));
    }

    /* Optimistic state update */
    esphome_fan_state_t state = {0};
    state.key = key;
    esphome_entity_get_fan(key, &state);
    if (cmd->has_state) state.state = cmd->state;
    if (cmd->has_speed_level) state.speed_level = cmd->speed_level;
    if (cmd->has_oscillating) state.oscillating = cmd->oscillating;
    if (cmd->has_direction) state.direction = cmd->direction;
    esphome_entity_update_fan(key, &state);

    return ret;
}

static esp_err_t register_fan_entity(const device_t *dev)
{
    esphome_fan_config_t config = {0};
    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);

    config.key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_FAN);
    config.device_id = device_id;
    make_entity_name(dev, DEV_CAP_FAN, config.name, sizeof(config.name));
    make_unique_id(dev, DEV_CAP_FAN, config.unique_id, sizeof(config.unique_id));

    strncpy(config.icon, "mdi:fan", sizeof(config.icon) - 1);
    config.supports_oscillation = false;
    config.supports_speed = true;
    config.supports_direction = false;
    config.supported_speed_count = 3; /* low, medium, high */
    config.command_callback = fan_command_callback;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_fan(&config);
    if (ret == ESP_OK) {
        s_stats.entities_registered++;
        ESP_LOGD(TAG, "Registered fan: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    } else if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Fan max capacity reached, cannot register: %s", config.name);
    } else {
        ESP_LOGW(TAG, "Failed to register fan %s: %s",
                 config.name, esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================================
 * Climate Entity Registration
 * ============================================================================ */

static esp_err_t climate_command_callback(esphome_entity_key_t key,
                                           const esphome_climate_command_t *cmd)
{
    ESP_LOGI(TAG, "Climate command: key=0x%08lX, mode=%d, target=%.1f",
             (unsigned long)key,
             cmd->has_mode ? (int)cmd->mode : -1,
             cmd->has_target_temperature ? cmd->target_temperature : -1.0f);

    s_stats.commands_received++;

    device_t *dev = find_device_for_entity_key(key, NULL);
    if (!dev) {
        ESP_LOGW(TAG, "Climate command: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    if (cmd->has_target_temperature) {
        cJSON_AddNumberToObject(json, "occupied_heating_setpoint",
                                cmd->target_temperature * 100.0f); /* Z2M uses centidegrees */
    }
    if (cmd->has_target_temperature_low) {
        cJSON_AddNumberToObject(json, "occupied_heating_setpoint",
                                cmd->target_temperature_low * 100.0f);
    }
    if (cmd->has_target_temperature_high) {
        cJSON_AddNumberToObject(json, "occupied_cooling_setpoint",
                                cmd->target_temperature_high * 100.0f);
    }
    if (cmd->has_mode) {
        /* Map ESPHome climate mode to Z2M system_mode */
        const char *mode = "off";
        switch (cmd->mode) {
            case ESPHOME_CLIMATE_MODE_HEAT: mode = "heat"; break;
            case ESPHOME_CLIMATE_MODE_COOL: mode = "cool"; break;
            case ESPHOME_CLIMATE_MODE_HEAT_COOL: mode = "auto"; break;
            case ESPHOME_CLIMATE_MODE_FAN_ONLY: mode = "fan_only"; break;
            case ESPHOME_CLIMATE_MODE_DRY: mode = "dry"; break;
            default: mode = "off"; break;
        }
        cJSON_AddStringToObject(json, "system_mode", mode);
    }

    uint8_t ep = dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    esp_err_t ret = zb_converter_handle_command(
        dev->proto.zigbee.short_addr, ep, json);
    cJSON_Delete(json);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Climate command failed for 0x%04X: %s",
                 dev->proto.zigbee.short_addr, esp_err_to_name(ret));
    }

    /* Optimistic state update */
    esphome_climate_state_t state = {0};
    state.key = key;
    esphome_entity_get_climate(key, &state);
    if (cmd->has_target_temperature) state.target_temperature = cmd->target_temperature;
    if (cmd->has_target_temperature_low) state.target_temperature_low = cmd->target_temperature_low;
    if (cmd->has_target_temperature_high) state.target_temperature_high = cmd->target_temperature_high;
    if (cmd->has_mode) state.mode = cmd->mode;
    esphome_entity_update_climate(key, &state);

    return ret;
}

static esp_err_t register_climate_entity(const device_t *dev)
{
    esphome_climate_config_t config = {0};
    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);

    config.key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_CLIMATE);
    config.device_id = device_id;
    make_entity_name(dev, DEV_CAP_CLIMATE, config.name, sizeof(config.name));
    make_unique_id(dev, DEV_CAP_CLIMATE, config.unique_id, sizeof(config.unique_id));

    strncpy(config.icon, "mdi:thermostat", sizeof(config.icon) - 1);
    config.supports_current_temperature = true;
    config.supports_two_point_target_temperature = true;
    config.visual_min_temperature = 5.0f;
    config.visual_max_temperature = 35.0f;
    config.visual_temperature_step = 0.5f;
    config.supports_action = true;
    config.supported_modes = (1 << ESPHOME_CLIMATE_MODE_OFF)
                           | (1 << ESPHOME_CLIMATE_MODE_HEAT)
                           | (1 << ESPHOME_CLIMATE_MODE_COOL)
                           | (1 << ESPHOME_CLIMATE_MODE_HEAT_COOL);
    config.command_callback = climate_command_callback;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_climate(&config);
    if (ret == ESP_OK) {
        s_stats.entities_registered++;
        ESP_LOGD(TAG, "Registered climate: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    } else if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Climate max capacity reached, cannot register: %s", config.name);
    } else {
        ESP_LOGW(TAG, "Failed to register climate %s: %s",
                 config.name, esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================================
 * Lock Entity Registration
 * ============================================================================ */

static esp_err_t lock_command_callback(esphome_entity_key_t key,
                                        esphome_lock_command_t cmd)
{
    ESP_LOGI(TAG, "Lock command: key=0x%08lX, cmd=%d",
             (unsigned long)key, (int)cmd);

    s_stats.commands_received++;

    device_t *dev = find_device_for_entity_key(key, NULL);
    if (!dev) {
        ESP_LOGW(TAG, "Lock command: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    /* Map ESPHome lock command to Z2M state */
    const char *state_str = "LOCK";
    switch (cmd) {
        case ESPHOME_LOCK_COMMAND_UNLOCK: state_str = "UNLOCK"; break;
        case ESPHOME_LOCK_COMMAND_OPEN:   state_str = "OPEN"; break;
        default:                          state_str = "LOCK"; break;
    }
    cJSON_AddStringToObject(json, "state", state_str);

    uint8_t ep = dev->proto.zigbee.endpoint;
    if (ep == 0) ep = 1;

    esp_err_t ret = zb_converter_handle_command(
        dev->proto.zigbee.short_addr, ep, json);
    cJSON_Delete(json);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Lock command failed for 0x%04X: %s",
                 dev->proto.zigbee.short_addr, esp_err_to_name(ret));
    }

    /* Optimistic state update */
    esphome_lock_state_t lock_state = ESPHOME_LOCK_STATE_LOCKED;
    if (cmd == ESPHOME_LOCK_COMMAND_UNLOCK) lock_state = ESPHOME_LOCK_STATE_UNLOCKED;
    esphome_entity_update_lock(key, lock_state);

    return ret;
}

static esp_err_t register_lock_entity(const device_t *dev)
{
    esphome_lock_config_t config = {0};
    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);

    config.key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_LOCK);
    config.device_id = device_id;
    make_entity_name(dev, DEV_CAP_LOCK, config.name, sizeof(config.name));
    make_unique_id(dev, DEV_CAP_LOCK, config.unique_id, sizeof(config.unique_id));

    strncpy(config.icon, "mdi:lock", sizeof(config.icon) - 1);
    config.assumed_state = false;
    config.supports_open = true;
    config.requires_code = false;
    config.command_callback = lock_command_callback;
    config.disabled_by_default = false;

    esp_err_t ret = esphome_entity_register_lock(&config);
    if (ret == ESP_OK) {
        s_stats.entities_registered++;
        ESP_LOGD(TAG, "Registered lock: %s (key=0x%08lX)",
                 config.name, (unsigned long)config.key);
    } else if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Lock max capacity reached, cannot register: %s", config.name);
    } else {
        ESP_LOGW(TAG, "Failed to register lock %s: %s",
                 config.name, esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================================
 * Internal API Implementations (for sub-modules)
 * ============================================================================ */

esphome_adapter_stats_t *esphome_adapter_get_stats_ptr(void)
{
    return &s_stats;
}


void esphome_adapter_make_expose_name(const device_t *dev, const char *expose_name,
                                       char *buf, size_t buf_size)
{
    char suffix[32];
    size_t j = 0;
    bool capitalize_next = true;
    for (size_t i = 0; expose_name[i] && j < sizeof(suffix) - 1; i++) {
        if (expose_name[i] == '_') {
            suffix[j++] = ' ';
            capitalize_next = true;
        } else if (capitalize_next) {
            suffix[j++] = (char)(expose_name[i] >= 'a' && expose_name[i] <= 'z'
                                   ? expose_name[i] - 32 : expose_name[i]);
            capitalize_next = false;
        } else {
            suffix[j++] = expose_name[i];
        }
    }
    suffix[j] = '\0';

    if (dev->protocol == DEV_PROTOCOL_ZIGBEE) {
        snprintf(buf, buf_size, "%s", suffix);
    } else {
        const char *name = dev->friendly_name[0] ? dev->friendly_name : "Device";
        snprintf(buf, buf_size, "%s %s", name, suffix);
    }
}

void esphome_adapter_make_expose_unique_id(const device_t *dev, const char *expose_name,
                                            char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "0x%016llX_%s", (unsigned long long)dev->id, expose_name);
}

esphome_sensor_device_class_t esphome_adapter_expose_dc_to_sensor_class(const char *dc)
{
    if (!dc) return ESPHOME_SENSOR_CLASS_NONE;
    if (strcmp(dc, "temperature") == 0) return ESPHOME_SENSOR_CLASS_TEMPERATURE;
    if (strcmp(dc, "humidity") == 0)    return ESPHOME_SENSOR_CLASS_HUMIDITY;
    if (strcmp(dc, "pressure") == 0)    return ESPHOME_SENSOR_CLASS_PRESSURE;
    if (strcmp(dc, "battery") == 0)     return ESPHOME_SENSOR_CLASS_BATTERY;
    if (strcmp(dc, "power") == 0)       return ESPHOME_SENSOR_CLASS_POWER;
    if (strcmp(dc, "energy") == 0)      return ESPHOME_SENSOR_CLASS_ENERGY;
    if (strcmp(dc, "voltage") == 0)     return ESPHOME_SENSOR_CLASS_VOLTAGE;
    if (strcmp(dc, "current") == 0)     return ESPHOME_SENSOR_CLASS_CURRENT;
    if (strcmp(dc, "illuminance") == 0) return ESPHOME_SENSOR_CLASS_ILLUMINANCE;
    return ESPHOME_SENSOR_CLASS_NONE;
}

esphome_state_class_t esphome_adapter_expose_sc_to_state_class(const char *sc)
{
    if (!sc) return ESPHOME_STATE_CLASS_NONE;
    if (strcmp(sc, "measurement") == 0)      return ESPHOME_STATE_CLASS_MEASUREMENT;
    if (strcmp(sc, "total") == 0)            return ESPHOME_STATE_CLASS_TOTAL;
    if (strcmp(sc, "total_increasing") == 0) return ESPHOME_STATE_CLASS_TOTAL_INCREASING;
    return ESPHOME_STATE_CLASS_NONE;
}

esphome_binary_device_class_t esphome_adapter_expose_dc_to_binary_class(const char *dc)
{
    if (!dc) return ESPHOME_BINARY_CLASS_NONE;
    if (strcmp(dc, "vibration") == 0)   return ESPHOME_BINARY_CLASS_VIBRATION;
    if (strcmp(dc, "motion") == 0)      return ESPHOME_BINARY_CLASS_MOTION;
    if (strcmp(dc, "occupancy") == 0)   return ESPHOME_BINARY_CLASS_OCCUPANCY;
    if (strcmp(dc, "door") == 0)        return ESPHOME_BINARY_CLASS_DOOR;
    if (strcmp(dc, "window") == 0)      return ESPHOME_BINARY_CLASS_WINDOW;
    if (strcmp(dc, "opening") == 0)     return ESPHOME_BINARY_CLASS_OPENING;
    if (strcmp(dc, "contact") == 0)     return ESPHOME_BINARY_CLASS_DOOR;
    if (strcmp(dc, "moisture") == 0)    return ESPHOME_BINARY_CLASS_MOISTURE;
    if (strcmp(dc, "water_leak") == 0)  return ESPHOME_BINARY_CLASS_MOISTURE;
    if (strcmp(dc, "smoke") == 0)       return ESPHOME_BINARY_CLASS_SMOKE;
    if (strcmp(dc, "gas") == 0)         return ESPHOME_BINARY_CLASS_GAS;
    if (strcmp(dc, "co") == 0)          return ESPHOME_BINARY_CLASS_GAS;
    if (strcmp(dc, "tamper") == 0)      return ESPHOME_BINARY_CLASS_TAMPER;
    if (strcmp(dc, "battery") == 0)     return ESPHOME_BINARY_CLASS_BATTERY;
    if (strcmp(dc, "presence") == 0)    return ESPHOME_BINARY_CLASS_PRESENCE;
    return ESPHOME_BINARY_CLASS_NONE;
}

void esphome_adapter_schedule_auto_clear(esphome_entity_key_t key,
                                          esphome_auto_clear_type_t type)
{
    switch (type) {
    case ESPHOME_AUTO_CLEAR_VIBRATION:
        if (s_vibration_clear_timer) {
            s_vibration_clear_key = key;
            esp_timer_stop(s_vibration_clear_timer);
            esp_timer_start_once(s_vibration_clear_timer, 5 * 1000000ULL);
        }
        break;
    case ESPHOME_AUTO_CLEAR_ACTION:
        if (s_action_clear_timer) {
            s_action_clear_key = key;
            esp_timer_stop(s_action_clear_timer);
            esp_timer_start_once(s_action_clear_timer, 1 * 1000000ULL);
        }
        break;
    }
}

/* ============================================================================
 * Cap-Based Entity Registration
 * ============================================================================ */

/**
 * @brief Register cap-based entities only for remaining capabilities
 *
 * Called with caps already filtered to exclude what exposes covered.
 */
static void register_from_caps(const device_t *dev, uint32_t caps)
{
    /* Sensors */
    if (caps & DEV_CAP_TEMPERATURE) register_sensor_entity(dev, DEV_CAP_TEMPERATURE);
    if (caps & DEV_CAP_HUMIDITY)    register_sensor_entity(dev, DEV_CAP_HUMIDITY);
    if (caps & DEV_CAP_PRESSURE)    register_sensor_entity(dev, DEV_CAP_PRESSURE);
    if (caps & DEV_CAP_BATTERY)     register_sensor_entity(dev, DEV_CAP_BATTERY);
    if (caps & DEV_CAP_POWER)       register_sensor_entity(dev, DEV_CAP_POWER);
    if (caps & DEV_CAP_ENERGY)      register_sensor_entity(dev, DEV_CAP_ENERGY);
    if (caps & DEV_CAP_VOLTAGE)     register_sensor_entity(dev, DEV_CAP_VOLTAGE);
    if (caps & DEV_CAP_CURRENT)     register_sensor_entity(dev, DEV_CAP_CURRENT);
    if (caps & DEV_CAP_ILLUMINANCE)   register_sensor_entity(dev, DEV_CAP_ILLUMINANCE);
    if (caps & DEV_CAP_CO2)           register_sensor_entity(dev, DEV_CAP_CO2);
    if (caps & DEV_CAP_VOC)           register_sensor_entity(dev, DEV_CAP_VOC);
    if (caps & DEV_CAP_PM25)          register_sensor_entity(dev, DEV_CAP_PM25);
    if (caps & DEV_CAP_SOIL_MOISTURE) register_sensor_entity(dev, DEV_CAP_SOIL_MOISTURE);

    /* Binary sensors */
    if (caps & DEV_CAP_MOTION)     register_binary_sensor_entity(dev, DEV_CAP_MOTION);
    if (caps & DEV_CAP_CONTACT)    register_binary_sensor_entity(dev, DEV_CAP_CONTACT);
    if (caps & DEV_CAP_VIBRATION)  register_binary_sensor_entity(dev, DEV_CAP_VIBRATION);
    if (caps & DEV_CAP_WATER_LEAK) register_binary_sensor_entity(dev, DEV_CAP_WATER_LEAK);
    if (caps & DEV_CAP_SMOKE)      register_binary_sensor_entity(dev, DEV_CAP_SMOKE);
    if (caps & DEV_CAP_CO)         register_binary_sensor_entity(dev, DEV_CAP_CO);
    if (caps & DEV_CAP_TAMPER)     register_binary_sensor_entity(dev, DEV_CAP_TAMPER);
    if (caps & DEV_CAP_BATTERY_LOW) register_binary_sensor_entity(dev, DEV_CAP_BATTERY_LOW);

    /* Light vs Switch priority */
    if (caps & DEV_CAP_BRIGHTNESS) {
        register_light_entity(dev);
    } else if (caps & DEV_CAP_ON_OFF) {
        register_switch_entity(dev);
    }

    /* Complex entities */
    if (caps & DEV_CAP_COVER)   register_cover_entity(dev);
    if (caps & DEV_CAP_FAN)     register_fan_entity(dev);
    if (caps & DEV_CAP_CLIMATE) register_climate_entity(dev);
    if (caps & DEV_CAP_LOCK)    register_lock_entity(dev);
}

/**
 * @brief Register all applicable ESPHome entities for a device
 *
 * Expose-first architecture:
 * 1. esphome_adapter_exposes_register() — creates entities from converter exposes (full metadata)
 * 2. register_from_caps()               — only for caps NOT already in exposes (fallback)
 * 3. esphome_adapter_tuya_register()    — last resort for driver-only devices without proper converter
 */
static esp_err_t register_device_entities(const device_t *dev)
{
    if (!dev || !dev->in_use) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Only register entities for Zigbee devices as ESPHome sub-devices.
     * System monitoring entities (ESPHome virtual devices) have their own
     * registration path in esphome_api_server.c. */
    if (dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        return ESP_OK;
    }

    /* Guard: skip if entities already registered for this device.
     * Multiple events (JOINED + INTERVIEWED) can trigger registration
     * for the same device — repeated mutex contention causes FreeRTOS
     * priority inheritance assert. */
    if (is_device_entities_registered(dev->id)) {
        ESP_LOGD(TAG, "Entities already registered for %s, skipping",
                 dev->friendly_name);
        return ESP_OK;
    }

    /* 1. Expose-first: create entities from converter exposes (full metadata) */
    const zb_converter_def_t *conv =
        (const zb_converter_def_t *)device_registry_get_converter(dev->id);

    ESP_LOGI(TAG, "Registering entities for device: %s (caps=0x%08lX, conv=%s, exposes=%u)",
             dev->friendly_name[0] ? dev->friendly_name : "unnamed",
             (unsigned long)dev->capabilities,
             conv ? (conv->description ? conv->description : conv->model) : "none",
             conv ? conv->expose_count : 0);

    uint32_t expose_caps = 0;
    if (conv && conv->expose_count > 1) {
        /* Only use expose-first for converters with more than the generic
         * single SWITCH expose (e.g. tuya_bridge has expose_count=1) */
        expose_caps = esphome_adapter_exposes_register(dev, conv);
    }

    /* 2. Cap fallback: only for capabilities NOT already covered by exposes */
    uint32_t remaining_caps = dev->capabilities & ~expose_caps;
    if (remaining_caps) {
        register_from_caps(dev, remaining_caps);
    }

    /* 3. Tuya auto-reg: only for driver-only devices without full converter exposes.
     * If a device has a converter with >1 exposes, all entities come from exposes. */
    bool has_full_converter = conv && conv->expose_count > 1;
    if (!has_full_converter) {
        esphome_adapter_tuya_register(dev);

        /* Legacy expose path for converters with minimal exposes */
        esphome_adapter_exposes_register_legacy(dev);
    }

    /* 4. Per-device diagnostic entities (LQI, RSSI, Identify, Remove, etc.) */
    esphome_adapter_diagnostics_register(dev);

    /* Mark as registered to prevent duplicate registration from subsequent events */
    mark_device_entities_registered(dev->id);

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

    /* Only process Zigbee devices here (system monitor has own path) */
    if (dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        return;
    }

    float value;
    bool bool_value;
    esp_err_t ret;

    /* Check if this device uses expose-first registration.
     * Expose-first devices have their SENSOR, BINARY_SENSOR, SWITCH entities
     * registered with 0x2XXXXXXX keys — cap-based updates would target the
     * wrong 0x1XXXXXXX keys. Skip cap-based updates for those entity types.
     * Complex entities (LIGHT, COVER, FAN, CLIMATE, LOCK) are always registered
     * via cap-based path even for expose-first devices. */
    const zb_converter_def_t *upd_conv =
        (const zb_converter_def_t *)device_registry_get_converter(dev->id);
    bool is_expose_first = (upd_conv && upd_conv->expose_count > 1);

    /* Temperature */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_TEMPERATURE) &&
        get_state_float(state, "temperature", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_TEMPERATURE);
        ret = esphome_entity_update_sensor(key, value);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed update temperature key=0x%08lX: %s", (unsigned long)key, esp_err_to_name(ret));
        }
        s_stats.state_updates_sent++;
    }

    /* Humidity */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_HUMIDITY) &&
        get_state_float(state, "humidity", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_HUMIDITY);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Pressure */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_PRESSURE) &&
        get_state_float(state, "pressure", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_PRESSURE);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Battery */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_BATTERY) &&
        get_state_float(state, "battery", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_BATTERY);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Power monitoring */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_POWER) &&
        get_state_float(state, "power", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_POWER);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    if (!is_expose_first && (dev->capabilities & DEV_CAP_ENERGY) &&
        (get_state_float(state, "energy", &value) ||
         get_state_float(state, "total_energy", &value))) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_ENERGY);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    if (!is_expose_first && (dev->capabilities & DEV_CAP_VOLTAGE) &&
        get_state_float(state, "voltage", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_VOLTAGE);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    if (!is_expose_first && (dev->capabilities & DEV_CAP_CURRENT) &&
        get_state_float(state, "current", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_CURRENT);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Illuminance */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_ILLUMINANCE) &&
        (get_state_float(state, "illuminance_lux", &value) ||
         get_state_float(state, "illuminance", &value))) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_ILLUMINANCE);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* CO2 */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_CO2) &&
        get_state_float(state, "co2", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_CO2);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* VOC */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_VOC) &&
        get_state_float(state, "voc", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_VOC);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* PM2.5 */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_PM25) &&
        get_state_float(state, "pm25", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_PM25);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Soil Moisture */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_SOIL_MOISTURE) &&
        get_state_float(state, "soil_moisture", &value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_SOIL_MOISTURE);
        esphome_entity_update_sensor(key, value);
        s_stats.state_updates_sent++;
    }

    /* Motion binary sensor */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_MOTION) &&
        get_state_bool(state, "occupancy", &bool_value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_MOTION);
        esphome_entity_update_binary_sensor(key, bool_value);
        s_stats.state_updates_sent++;
    }

    /* Contact binary sensor */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_CONTACT) &&
        get_state_bool(state, "contact", &bool_value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_CONTACT);
        /* Note: contact=true means closed (no detection), contact=false means open */
        esphome_entity_update_binary_sensor(key, !bool_value);
        s_stats.state_updates_sent++;
    }

    /* Vibration binary sensor (auto-clear after 5s) */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_VIBRATION) &&
        get_state_bool(state, "vibration", &bool_value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_VIBRATION);
        esphome_entity_update_binary_sensor(key, bool_value);
        s_stats.state_updates_sent++;
        if (bool_value && s_vibration_clear_timer) {
            s_vibration_clear_key = key;
            esp_timer_stop(s_vibration_clear_timer);
            esp_timer_start_once(s_vibration_clear_timer, 5 * 1000000ULL);
        }
    }

    /* Water leak binary sensor */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_WATER_LEAK) &&
        get_state_bool(state, "water_leak", &bool_value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_WATER_LEAK);
        esphome_entity_update_binary_sensor(key, bool_value);
        s_stats.state_updates_sent++;
    }

    /* Smoke binary sensor */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_SMOKE) &&
        get_state_bool(state, "smoke", &bool_value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_SMOKE);
        esphome_entity_update_binary_sensor(key, bool_value);
        s_stats.state_updates_sent++;
    }

    /* CO binary sensor */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_CO) &&
        get_state_bool(state, "co", &bool_value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_CO);
        esphome_entity_update_binary_sensor(key, bool_value);
        s_stats.state_updates_sent++;
    }

    /* Tamper binary sensor */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_TAMPER) &&
        get_state_bool(state, "tamper", &bool_value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_TAMPER);
        esphome_entity_update_binary_sensor(key, bool_value);
        s_stats.state_updates_sent++;
    }

    /* Battery low binary sensor */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_BATTERY_LOW) &&
        get_state_bool(state, "battery_low", &bool_value)) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_BATTERY_LOW);
        esphome_entity_update_binary_sensor(key, bool_value);
        s_stats.state_updates_sent++;
    }

    /* Switch state (only cap-based path — expose-first uses esphome_adapter_exposes_update_states) */
    if (!is_expose_first && (dev->capabilities & DEV_CAP_ON_OFF) &&
        !(dev->capabilities & DEV_CAP_BRIGHTNESS)) {
        if (get_state_bool(state, "state", &bool_value)) {
            esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_ON_OFF);
            esphome_entity_update_switch(key, bool_value);
            s_stats.state_updates_sent++;
        }
    }

    /* Light state (always cap-based, even for expose-first — expose defers to cap) */
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
            light_state.color_mode = ESPHOME_COLOR_MODE_COLOR_TEMP;
            updated = true;
        }

        /* Color XY → RGB (from fz_color_xy: {"color": {"x": float, "y": float}}) */
        cJSON *color_obj = cJSON_GetObjectItem(state, "color");
        if (color_obj && cJSON_IsObject(color_obj)) {
            cJSON *cx = cJSON_GetObjectItem(color_obj, "x");
            cJSON *cy = cJSON_GetObjectItem(color_obj, "y");
            if (cx && cy && cJSON_IsNumber(cx) && cJSON_IsNumber(cy)) {
                color_xy_to_rgb((float)cx->valuedouble, (float)cy->valuedouble,
                                &light_state.red, &light_state.green, &light_state.blue);
                light_state.color_mode = ESPHOME_COLOR_MODE_RGB;
                updated = true;
            } else {
                /* Color HS → RGB (from fz_color_hs: {"color": {"hue": float, "saturation": float}}) */
                cJSON *ch = cJSON_GetObjectItem(color_obj, "hue");
                cJSON *cs = cJSON_GetObjectItem(color_obj, "saturation");
                if (ch && cs && cJSON_IsNumber(ch) && cJSON_IsNumber(cs)) {
                    color_hs_to_rgb((float)ch->valuedouble, (float)cs->valuedouble,
                                    &light_state.red, &light_state.green, &light_state.blue);
                    light_state.color_mode = ESPHOME_COLOR_MODE_RGB;
                    updated = true;
                }
            }
        }

        /* Set color mode based on what was set if not already determined by color/color_temp */
        if (updated && light_state.color_mode == ESPHOME_COLOR_MODE_UNKNOWN) {
            light_state.color_mode = ESPHOME_COLOR_MODE_BRIGHTNESS;
        }

        if (updated) {
            esphome_entity_update_light(key, &light_state);
            s_stats.state_updates_sent++;
        }
    }

    /* Cover state */
    if (dev->capabilities & DEV_CAP_COVER) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_COVER);
        esphome_cover_state_t cover_state = {0};
        cover_state.key = key;
        bool updated = false;

        if (get_state_float(state, "position", &value)) {
            /* Z2M position 0-100 → ESPHome 0.0-1.0 */
            cover_state.position = value / 100.0f;
            updated = true;
        }

        if (get_state_float(state, "tilt", &value)) {
            /* Z2M tilt 0-100 → ESPHome 0.0-1.0 */
            cover_state.tilt = value / 100.0f;
            updated = true;
        }

        if (updated) {
            esphome_entity_update_cover(key, &cover_state);
            s_stats.state_updates_sent++;
        }
    }

    /* Fan state */
    if (dev->capabilities & DEV_CAP_FAN) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_FAN);
        esphome_fan_state_t fan_state = {0};
        fan_state.key = key;
        bool updated = false;

        if (get_state_bool(state, "state", &bool_value)) {
            fan_state.state = bool_value;
            updated = true;
        }

        cJSON *fan_mode = cJSON_GetObjectItem(state, "fan_mode");
        if (fan_mode && cJSON_IsString(fan_mode)) {
            const char *mode_str = fan_mode->valuestring;
            if (strcasecmp(mode_str, "low") == 0) fan_state.speed_level = 1;
            else if (strcasecmp(mode_str, "medium") == 0) fan_state.speed_level = 2;
            else if (strcasecmp(mode_str, "high") == 0) fan_state.speed_level = 3;
            updated = true;
        }

        if (updated) {
            esphome_entity_update_fan(key, &fan_state);
            s_stats.state_updates_sent++;
        }
    }

    /* Climate state */
    if (dev->capabilities & DEV_CAP_CLIMATE) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_CLIMATE);
        esphome_climate_state_t climate_state = {0};
        climate_state.key = key;
        bool updated = false;

        if (get_state_float(state, "local_temperature", &value)) {
            climate_state.current_temperature = value;
            updated = true;
        }

        if (get_state_float(state, "occupied_heating_setpoint", &value)) {
            /* Z2M uses centidegrees */
            climate_state.target_temperature_low = value / 100.0f;
            climate_state.target_temperature = value / 100.0f;
            updated = true;
        }

        if (get_state_float(state, "occupied_cooling_setpoint", &value)) {
            climate_state.target_temperature_high = value / 100.0f;
            updated = true;
        }

        cJSON *sys_mode = cJSON_GetObjectItem(state, "system_mode");
        if (sys_mode && cJSON_IsString(sys_mode)) {
            const char *m = sys_mode->valuestring;
            if (strcasecmp(m, "heat") == 0)
                climate_state.mode = ESPHOME_CLIMATE_MODE_HEAT;
            else if (strcasecmp(m, "cool") == 0)
                climate_state.mode = ESPHOME_CLIMATE_MODE_COOL;
            else if (strcasecmp(m, "auto") == 0)
                climate_state.mode = ESPHOME_CLIMATE_MODE_HEAT_COOL;
            else
                climate_state.mode = ESPHOME_CLIMATE_MODE_OFF;
            updated = true;
        }

        if (updated) {
            esphome_entity_update_climate(key, &climate_state);
            s_stats.state_updates_sent++;
        }
    }

    /* Lock state */
    if (dev->capabilities & DEV_CAP_LOCK) {
        esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, DEV_CAP_LOCK);
        cJSON *lock_item = cJSON_GetObjectItem(state, "lock_state");
        if (!lock_item) lock_item = cJSON_GetObjectItem(state, "state");
        if (lock_item && cJSON_IsString(lock_item)) {
            const char *s = lock_item->valuestring;
            esphome_lock_state_t ls = ESPHOME_LOCK_STATE_LOCKED;
            if (strcasecmp(s, "UNLOCK") == 0 || strcasecmp(s, "unlocked") == 0)
                ls = ESPHOME_LOCK_STATE_UNLOCKED;
            esphome_entity_update_lock(key, ls);
            s_stats.state_updates_sent++;
        }
    }

    /* ---- Ensure all registered entities are marked available ----
     * Battery-powered Zigbee devices may not report for hours after boot.
     * If the state JSON lacks a key (e.g. "battery", "voltage", "vibration"),
     * the entity stays missing_state=true and HA shows "Unavailable".
     * Set NaN (sensors) or false (binary sensors) as placeholder so HA shows
     * the entity as available with "Unknown" value.  Real values overwrite
     * on next Zigbee report.
     * Skip for expose-first devices — their entities use 0x2X keys, not 0x1X. */
    if (!is_expose_first) {
        esphome_sensor_state_t sensor_st;
        static const device_capability_t sensor_caps[] = {
            DEV_CAP_TEMPERATURE, DEV_CAP_HUMIDITY, DEV_CAP_PRESSURE,
            DEV_CAP_BATTERY, DEV_CAP_POWER, DEV_CAP_ENERGY,
            DEV_CAP_VOLTAGE, DEV_CAP_CURRENT,
            DEV_CAP_ILLUMINANCE, DEV_CAP_CO2, DEV_CAP_VOC,
            DEV_CAP_PM25, DEV_CAP_SOIL_MOISTURE
        };
        for (size_t i = 0; i < sizeof(sensor_caps) / sizeof(sensor_caps[0]); i++) {
            if (dev->capabilities & sensor_caps[i]) {
                esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, sensor_caps[i]);
                if (esphome_entity_get_sensor(key, &sensor_st) == ESP_OK && sensor_st.missing_state) {
                    esphome_entity_update_sensor(key, NAN);
                }
            }
        }

        esphome_binary_sensor_state_t binary_st;
        static const device_capability_t binary_caps[] = {
            DEV_CAP_MOTION, DEV_CAP_CONTACT, DEV_CAP_VIBRATION,
            DEV_CAP_WATER_LEAK, DEV_CAP_SMOKE, DEV_CAP_CO,
            DEV_CAP_TAMPER, DEV_CAP_BATTERY_LOW
        };
        for (size_t i = 0; i < sizeof(binary_caps) / sizeof(binary_caps[0]); i++) {
            if (dev->capabilities & binary_caps[i]) {
                esphome_entity_key_t key = esphome_adapter_make_entity_key(dev->id, binary_caps[i]);
                if (esphome_entity_get_binary_sensor(key, &binary_st) == ESP_OK && binary_st.missing_state) {
                    esphome_entity_update_binary_sensor(key, false);
                }
            }
        }
    }

    /* Update expose-based entity states */
    esphome_adapter_exposes_update_states(dev, state);

    /* Update Tuya driver entity states (only for driver-only devices) */
    if (!is_expose_first) {
        esphome_adapter_tuya_update_states(dev, state);
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

    ESP_LOGD(TAG, "State changed: ieee=0x%016llX short=0x%04X cluster=0x%04X attr=0x%04X",
             (unsigned long long)evt->ieee_addr, evt->short_addr,
             evt->cluster_id, evt->attr_id);

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

    /* Fall back to registry state if not in event.
     *
     * This handler runs on the event dispatcher task while the Zigbee task can
     * be replacing the very same state object, so take an owned copy rather
     * than the registry's live pointer — device_registry_set_state() would
     * cJSON_Delete() it out from under us. The copy is freed by the common
     * path below. */
    if (!state) {
        state = device_registry_state_dup(evt->ieee_addr);
        if (!state) {
            ESP_LOGW(TAG, "No state in registry for 0x%016llX",
                     (unsigned long long)evt->ieee_addr);
        }
    }

    if (state) {
        update_entity_states(dev, state);
        cJSON_Delete(state);
    }

    /* Update diagnostic entities (LQI, RSSI, last_seen) */
    esphome_adapter_diagnostics_update(dev);
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

    /* Allow re-registration if device re-joins */
    unmark_device_entities_registered(evt->ieee_addr);
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
        /* Unreachable is not the same as gone.
         *
         * This called esphome_adapter_remove_device(), which does what its name
         * says: it deletes the entities. The comment claimed it marked them
         * unavailable. A battery device that sleeps through a poll, or a route
         * that needs rebuilding after the coordinator restarts, was therefore
         * enough to remove a device from Home Assistant entirely — losing its
         * history and breaking every automation and dashboard card that named
         * it. Seen here: three devices went unreachable and the gateway's
         * entity count in Home Assistant fell from 34 to 5.
         *
         * The entities stay. Their values go stale, which is honest, and the
         * "Last Seen" diagnostic already says when the device was last heard —
         * the same shape zigbee2mqtt and ZHA use. Removal is right when a
         * device actually leaves the network, and that path (EVT_DEVICE_LEFT)
         * still does it. */
        ESP_LOGI(TAG, "Device 0x%016llX unreachable — entities kept, values now stale",
                 (unsigned long long)evt->device_id);
    } else {
        /* Device came back online — resync entities with current state */
        esphome_adapter_sync_device(evt->device_id);
    }
}

/**
 * @brief Handle EVT_ESPHOME_CONNECTED event
 *
 * When an ESPHome client (Home Assistant) connects, sync ALL devices
 * so the client receives entity list and current states.
 */
static void handle_esphome_connected(event_type_t type, void *data,
                                      size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;
    (void)data;
    (void)data_size;

    ESP_LOGI(TAG, "ESPHome client connected — syncing all devices");
    esphome_adapter_on_client_connected();
}

/**
 * @brief Handle EVT_ESPHOME_DISCONNECTED event
 */
static void handle_esphome_disconnected(event_type_t type, void *data,
                                         size_t data_size, void *ctx)
{
    (void)type;
    (void)ctx;
    (void)data;
    (void)data_size;

    esphome_adapter_on_client_disconnected();
}

/**
 * @brief Handle EVT_DEVICE_INTERVIEWED event
 *
 * At join time devices have caps=0 (no capabilities known yet).
 * After interview completes, capabilities are populated and we can
 * register the correct ESPHome entities.
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
        ESP_LOGD(TAG, "Interview failed for 0x%04X, skipping entity sync",
                 evt->short_addr);
        return;
    }

    ESP_LOGI(TAG, "Device 0x%04X interviewed — registering entities",
             evt->short_addr);

    /* Unmark the device so register_device_entities() can create proper
     * entities.  The initial registration at HA-connect time may have only
     * created diagnostics (conv=none, caps=0) because the interview hadn't
     * completed yet.  Now that the converter is bound and capabilities are
     * known, we need a fresh registration pass. */
    unmark_device_entities_registered(evt->ieee_addr);

    /* Sync device: registers entities with the now-known converter + capabilities */
    esphome_adapter_sync_device(evt->ieee_addr);

    /* Force HA client reconnect so it re-requests ListEntities and discovers
     * the newly registered entities. The ESPHome HA integration reconnects
     * automatically within seconds after a server-initiated disconnect.
     *
     * Collapsed into one disconnect per burst. Provisioning at boot can
     * complete several interviews in quick succession, and one disconnect each
     * means Home Assistant is thrown off and re-enumerates repeatedly — which
     * is why measurements taken right after a flash always looked bad.
     *
     * Armed only if not already armed, deliberately. Restarting the timer on
     * every interview would push it out for as long as interviews keep
     * arriving, so a long burst would never trigger the re-discovery it exists
     * for. That is exactly how the Wi-Fi watchdog in this project managed never
     * to fire: reset by the event it was watching for. */
    schedule_rediscovery();
}

/* ============================================================================
 * Device Registry Iterator Callback
 * ============================================================================ */

/**
 * @brief Try to rebind converter for a persisted device
 *
 * After reboot, devices loaded from NVS have model/manufacturer but no
 * converter bound (pointers aren't persisted).  This tries to find the
 * converter from the DB and update capabilities accordingly.  Without
 * this, sleepy devices that never send a report would remain with
 * caps=0 and no functional ESPHome entities.
 */
static void try_rebind_converter(device_t *dev)
{
    if (dev->protocol != DEV_PROTOCOL_ZIGBEE) return;
    if (dev->model[0] == '\0') return;

    /* Already bound? */
    if (device_registry_get_converter(dev->id) != NULL) return;

    const zb_converter_def_t *conv = zb_converter_find(dev->manufacturer, dev->model);
    if (conv == NULL) return;

    /* Bind converter and update capabilities */
    zb_converter_bind(dev->proto.zigbee.short_addr, conv);
    dev->proto.zigbee.converter = (void *)conv;

    uint32_t conv_caps = zb_converter_get_capabilities(conv);
    if (conv_caps != 0 && (dev->capabilities & conv_caps) != conv_caps) {
        dev->capabilities |= conv_caps;
        ESP_LOGI(TAG, "Rebind enriched caps for %s: 0x%08lX -> 0x%08lX",
                 dev->model, (unsigned long)(dev->capabilities & ~conv_caps),
                 (unsigned long)dev->capabilities);
    }

    ESP_LOGI(TAG, "Rebound converter for %s (0x%04X): %s",
             dev->model, dev->proto.zigbee.short_addr,
             conv->description ? conv->description : conv->model);
}

/**
 * @brief Iterator callback to sync devices to ESPHome
 */
static bool sync_device_iterator(device_t *dev, void *ctx)
{
    (void)ctx;

    if (!dev->in_use) {
        return true; /* Continue */
    }

    /* Rebind converter from DB if not yet bound (after reboot) */
    try_rebind_converter(dev);

    /* Register entities for this device */
    register_device_entities(dev);

    /* Get current state and update entities.
     * If no state in registry, pass an empty object so the fallback
     * logic in update_entity_states() still sets defaults. */
    cJSON *state = device_registry_state_dup(dev->id);
    if (state) {
        update_entity_states(dev, state);
        cJSON_Delete(state);
    } else {
        cJSON *empty = cJSON_CreateObject();
        if (empty) {
            update_entity_states(dev, empty);
            cJSON_Delete(empty);
        }
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

    /* Subscribe to ESPHome client connect/disconnect events */
    ret = event_subscribe(EVT_ESPHOME_CONNECTED, handle_esphome_connected, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ESPHOME_CONNECTED: %s",
                 esp_err_to_name(ret));
        /* Non-fatal: entities still work via EVT_DEVICE_JOINED */
    }
    ret = event_subscribe(EVT_ESPHOME_DISCONNECTED, handle_esphome_disconnected, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_ESPHOME_DISCONNECTED: %s",
                 esp_err_to_name(ret));
    }

    /* Subscribe to device interview completion — entities need capabilities
     * which are only available after interview (caps=0 at join time) */
    ret = event_subscribe(EVT_DEVICE_INTERVIEWED, handle_device_interviewed, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to EVT_DEVICE_INTERVIEWED: %s",
                 esp_err_to_name(ret));
    }

    /* Create auto-clear timers for event-type entities */
    const esp_timer_create_args_t vib_timer_args = {
        .callback = vibration_auto_clear_cb,
        .name = "vib_clear",
    };
    if (esp_timer_create(&vib_timer_args, &s_vibration_clear_timer) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create vibration clear timer");
        s_vibration_clear_timer = NULL;
    }
    const esp_timer_create_args_t act_timer_args = {
        .callback = action_auto_clear_cb,
        .name = "act_clear",
    };
    if (esp_timer_create(&act_timer_args, &s_action_clear_timer) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create action clear timer");
        s_action_clear_timer = NULL;
    }

    /* Register the gateway's Home Assistant services here rather than on
     * client connect. Registration is local bookkeeping — it needs no client —
     * and doing it at init keeps the service list stable and ready before the
     * first ListEntities request arrives, instead of tying availability to
     * connection order. */
    esp_err_t svc_ret = esphome_gateway_services_register();
    if (svc_ret != ESP_OK) {
        ESP_LOGW(TAG, "Gateway services partially unavailable: %s",
                 esp_err_to_name(svc_ret));
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

    /* Stop and delete auto-clear timers */
    if (s_vibration_clear_timer) {
        esp_timer_stop(s_vibration_clear_timer);
        esp_timer_delete(s_vibration_clear_timer);
        s_vibration_clear_timer = NULL;
    }
    if (s_action_clear_timer) {
        esp_timer_stop(s_action_clear_timer);
        esp_timer_delete(s_action_clear_timer);
        s_action_clear_timer = NULL;
    }

    /* Unsubscribe from all events */
    event_unsubscribe(EVT_DEVICE_STATE_CHANGED, handle_device_state_changed);
    event_unsubscribe(EVT_DEVICE_JOINED, handle_device_joined);
    event_unsubscribe(EVT_DEVICE_LEFT, handle_device_left);
    event_unsubscribe(EVT_DEVICE_AVAILABILITY_CHANGED, handle_device_availability_changed);
    event_unsubscribe(EVT_ESPHOME_CONNECTED, handle_esphome_connected);
    event_unsubscribe(EVT_ESPHOME_DISCONNECTED, handle_esphome_disconnected);
    event_unsubscribe(EVT_DEVICE_INTERVIEWED, handle_device_interviewed);

    esphome_adapter_gateway_deinit();
    esphome_adapter_diagnostics_deinit();

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

    esphome_adapter_gateway_register();

    /* Sync all devices to ESPHome */
    esphome_adapter_sync_all_devices();
}

void esphome_adapter_on_client_disconnected(void)
{
    if (!s_initialized) {
        return;
    }

    s_stats.client_disconnects++;

    /* Reset entity registration tracker — next client connect will re-register all */
    s_registered_count = 0;

    ESP_LOGI(TAG, "ESPHome client disconnected (entity registration tracker reset)");
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

    /* Rebind converter from DB if not yet bound (after reboot) */
    try_rebind_converter(dev);

    /* Register entities */
    register_device_entities(dev);

    /* Update states.  If no state in registry, pass an empty object so the
     * fallback logic in update_entity_states() still sets defaults. */
    cJSON *state = device_registry_state_dup(id);
    if (state) {
        update_entity_states(dev, state);
        cJSON_Delete(state);
    } else {
        cJSON *empty = cJSON_CreateObject();
        if (empty) {
            update_entity_states(dev, empty);
            cJSON_Delete(empty);
        }
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

    /* Remove Tuya key cache entries for this device */
    esphome_adapter_tuya_remove_device(id);

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

        /* Cover entity — set to closed */
        if (dev->capabilities & DEV_CAP_COVER) {
            esphome_entity_key_t key = esphome_adapter_make_entity_key(id, DEV_CAP_COVER);
            esphome_cover_state_t cover_state = {0};
            cover_state.key = key;
            cover_state.position = 0.0f;
            esphome_entity_update_cover(key, &cover_state);
            s_stats.entities_removed++;
            ESP_LOGD(TAG, "Cover entity set to closed for removed device");
        }

        /* Fan entity — set to OFF */
        if (dev->capabilities & DEV_CAP_FAN) {
            esphome_entity_key_t key = esphome_adapter_make_entity_key(id, DEV_CAP_FAN);
            esphome_fan_state_t fan_state = {0};
            fan_state.key = key;
            fan_state.state = false;
            esphome_entity_update_fan(key, &fan_state);
            s_stats.entities_removed++;
            ESP_LOGD(TAG, "Fan entity set to OFF for removed device");
        }

        /* Climate entity — set to OFF mode */
        if (dev->capabilities & DEV_CAP_CLIMATE) {
            esphome_entity_key_t key = esphome_adapter_make_entity_key(id, DEV_CAP_CLIMATE);
            esphome_climate_state_t climate_state = {0};
            climate_state.key = key;
            climate_state.mode = ESPHOME_CLIMATE_MODE_OFF;
            esphome_entity_update_climate(key, &climate_state);
            s_stats.entities_removed++;
            ESP_LOGD(TAG, "Climate entity set to OFF for removed device");
        }

        /* Lock entity — set to locked */
        if (dev->capabilities & DEV_CAP_LOCK) {
            esphome_entity_key_t key = esphome_adapter_make_entity_key(id, DEV_CAP_LOCK);
            esphome_entity_update_lock(key, ESPHOME_LOCK_STATE_LOCKED);
            s_stats.entities_removed++;
            ESP_LOGD(TAG, "Lock entity set to locked for removed device");
        }

        /* Remove expose-based entities */
        const zb_converter_def_t *conv =
            (const zb_converter_def_t *)device_registry_get_converter(id);
        if (conv) {
            for (uint8_t i = 0; i < conv->expose_count; i++) {
                const zb_expose_t *expose = &conv->exposes[i];
                if (esphome_adapter_is_expose_covered_by_cap(expose, dev->capabilities)) continue;
                if (!expose->name) continue;

                uint32_t ekey = esphome_adapter_make_expose_key(id, i);

                switch (expose->type) {
                case ZB_EXPOSE_SENSOR:
                    if (expose->name && strcmp(expose->name, "action") == 0) {
                        esphome_entity_update_text_sensor(ekey, "");
                    } else {
                        esphome_entity_set_sensor_missing(ekey);
                    }
                    s_stats.entities_removed++;
                    break;
                case ZB_EXPOSE_BINARY_SENSOR:
                    esphome_entity_set_binary_sensor_missing(ekey);
                    s_stats.entities_removed++;
                    break;
                case ZB_EXPOSE_SELECT:
                    if (!(expose->access & EA_SET)) {
                        esphome_entity_update_text_sensor(ekey, "");
                    } else {
                        esphome_entity_update_select(ekey, "");
                    }
                    s_stats.entities_removed++;
                    break;
                default:
                    break;
                }
            }
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
