/**
 * @file zb_converter_registry.c
 * @brief Zigbee Converter Registry and Dispatch Logic
 *
 * Manages registration of converter definitions, device-to-converter
 * bindings, and dispatches attribute reports and commands through
 * the appropriate converter functions.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_converter.h"
#include "zb_converter_std.h"
#include "zigbee/zb_tuya.h"
#include "zigbee/zb_device_handler_types.h"
#include "core/device/unified_device.h"
#include "core/device/device_registry.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "esp_log.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "cJSON.h"
#include <string.h>
#include <inttypes.h>
#include "zb_converter_loader.h"
#include "zb_custom_quirk.h"

static const char *TAG = "CONV_REG";

/* ============================================================================
 * Forward declarations for converter registration functions
 * ============================================================================ */

extern void conv_generic_register(void);
extern void conv_tuya_fingerbot_register(void);
extern void conv_tuya_bridge_register(void);

/* Generic capability-based converter lookup */
extern const zb_converter_def_t *conv_generic_for_capabilities(uint32_t caps);

/* ============================================================================
 * Static state
 * ============================================================================ */

/** @brief Registered converter definitions */
static const zb_converter_def_t *s_definitions[ZB_CONVERTER_MAX_DEFINITIONS];
static size_t s_definition_count = 0;

/** @brief Device-to-converter binding entry */
typedef struct {
    uint16_t short_addr;
    const zb_converter_def_t *def;
} zb_converter_binding_t;

/** @brief Device bindings */
static zb_converter_binding_t s_bindings[ZB_CONVERTER_MAX_BINDINGS];
static size_t s_binding_count = 0;

/** @brief Mutex for thread safety */
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Initialization flag */
static bool s_initialized = false;

/** @brief Dispatch context for fz/tz generic converters */
static zb_dispatch_ctx_t s_dispatch_ctx;

/** @brief Mutex protecting s_dispatch_ctx from concurrent report/command dispatch */
static SemaphoreHandle_t s_dispatch_mutex = NULL;

void zb_converter_set_dispatch_ctx(const zb_dispatch_ctx_t *ctx)
{
    if (ctx != NULL) {
        s_dispatch_ctx = *ctx;
    } else {
        memset(&s_dispatch_ctx, 0, sizeof(s_dispatch_ctx));
    }
}

const zb_dispatch_ctx_t *zb_converter_get_dispatch_ctx(void)
{
    return &s_dispatch_ctx;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t zb_converter_registry_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Converter registry already initialized");
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create converter registry mutex");
        return ESP_ERR_NO_MEM;
    }

    s_dispatch_mutex = xSemaphoreCreateMutex();
    if (s_dispatch_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create dispatch mutex");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(s_definitions, 0, sizeof(s_definitions));
    s_definition_count = 0;

    memset(s_bindings, 0, sizeof(s_bindings));
    s_binding_count = 0;

    s_initialized = true;
    ESP_LOGI(TAG, "Converter registry initialized");

    return ESP_OK;
}

esp_err_t zb_converter_register_all(void)
{
    if (!s_initialized) {
        esp_err_t ret = zb_converter_registry_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    ESP_LOGI(TAG, "Registering all built-in converters...");

    /* Register converter modules */
    conv_generic_register();
    conv_tuya_fingerbot_register();    /* BEFORE generic bridge — exact match wins */
    conv_tuya_bridge_register();

    ESP_LOGI(TAG, "Registered %zu converter definitions", s_definition_count);

    return ESP_OK;
}

esp_err_t zb_converter_register(const zb_converter_def_t *def)
{
    if (def == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized || s_mutex == NULL) {
        ESP_LOGE(TAG, "Converter registry not initialized, cannot register");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_definition_count >= ZB_CONVERTER_MAX_DEFINITIONS) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Converter registry full (%d/%d)",
                 (int)s_definition_count, ZB_CONVERTER_MAX_DEFINITIONS);
        return ESP_ERR_NO_MEM;
    }

    s_definitions[s_definition_count] = def;
    s_definition_count++;

    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG, "Registered converter: %s %s (%s)",
             def->manufacturer ? def->manufacturer : "*",
             def->model ? def->model : "*",
             def->description ? def->description : "");

    return ESP_OK;
}

const zb_converter_def_t *zb_converter_find(const char *manufacturer, const char *model)
{
    if (model == NULL || model[0] == '\0' || !s_initialized || s_mutex == NULL) {
        return NULL;
    }

    /* Priority 0: Custom community quirks (highest priority) */
    const zb_converter_def_t *custom = zb_custom_quirk_find(manufacturer, model);
    if (custom != NULL) {
        ESP_LOGI(TAG, "Found custom quirk: %s %s",
                 manufacturer ? manufacturer : "*", model);
        return custom;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Priority 1: try exact match on both manufacturer and model */
    if (manufacturer != NULL && manufacturer[0] != '\0') {
        for (size_t i = 0; i < s_definition_count; i++) {
            const zb_converter_def_t *def = s_definitions[i];
            if (def == NULL || def->manufacturer == NULL || def->model == NULL) {
                continue;
            }

            if (strcmp(def->manufacturer, manufacturer) == 0 &&
                strcmp(def->model, model) == 0) {
                xSemaphoreGive(s_mutex);
                return def;
            }
        }
    }

    /* Priority 1b: try match by model only.
     * Only used when input manufacturer is NULL/empty (sleepy device that didn't
     * report manufacturer), OR when the definition has no manufacturer (wildcard).
     * NEVER match when both input and definition have manufacturer strings that differ —
     * this prevents e.g. Fingerbot (_TZ3210_dse8ogfy/TS0001) from matching the wrong
     * TS0001 definition registered by a different Tuya manufacturer. */
    bool input_has_mfr = (manufacturer != NULL && manufacturer[0] != '\0');
    for (size_t i = 0; i < s_definition_count; i++) {
        const zb_converter_def_t *def = s_definitions[i];
        if (def == NULL || def->model == NULL) {
            continue;
        }

        if (strcmp(def->model, model) == 0) {
            /* If both sides have a manufacturer string, they must match */
            bool def_has_mfr = (def->manufacturer != NULL && def->manufacturer[0] != '\0');
            if (input_has_mfr && def_has_mfr &&
                strcmp(def->manufacturer, manufacturer) != 0) {
                continue;  /* Skip: manufacturer mismatch */
            }
            xSemaphoreGive(s_mutex);
            return def;
        }
    }

    xSemaphoreGive(s_mutex);

    /* Priority 2: try runtime converter loader (LittleFS DB) */
    if (zb_converter_loader_is_available()) {
        const zb_converter_def_t *loaded = zb_converter_loader_find(manufacturer, model);
        if (loaded != NULL) {
            ESP_LOGI(TAG, "Loaded converter from LittleFS: %s %s",
                     manufacturer ? manufacturer : "*", model);
            /* Don't register into s_definitions[] — the loader cache owns the memory.
             * Registering would create a dangling pointer if the loader evicts this entry.
             * The caller should bind via zb_converter_bind() for per-device fast path. */
            return loaded;
        }
    }

    return NULL;
}

esp_err_t zb_converter_bind(uint16_t short_addr, const zb_converter_def_t *def)
{
    if (def == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Check if already bound - update existing */
    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].short_addr == short_addr) {
            s_bindings[i].def = def;
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "Updated converter binding: 0x%04X -> %s",
                     short_addr, def->description ? def->description : def->model);
            return ESP_OK;
        }
    }

    /* Add new binding */
    if (s_binding_count >= ZB_CONVERTER_MAX_BINDINGS) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Converter binding table full (%d/%d)",
                 (int)s_binding_count, ZB_CONVERTER_MAX_BINDINGS);
        return ESP_ERR_NO_MEM;
    }

    s_bindings[s_binding_count].short_addr = short_addr;
    s_bindings[s_binding_count].def = def;
    s_binding_count++;

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Bound converter: 0x%04X -> %s",
             short_addr, def->description ? def->description : def->model);

    return ESP_OK;
}

esp_err_t zb_converter_unbind(uint16_t short_addr)
{
    if (!s_initialized || s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].short_addr == short_addr) {
            /* Shift remaining entries */
            if (i < s_binding_count - 1) {
                memmove(&s_bindings[i], &s_bindings[i + 1],
                        (s_binding_count - i - 1) * sizeof(zb_converter_binding_t));
            }
            s_binding_count--;
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "Unbound converter for device 0x%04X", short_addr);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

void zb_converter_unbind_all(void)
{
    if (!s_initialized || s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    size_t old_count = s_binding_count;
    s_binding_count = 0;
    xSemaphoreGive(s_mutex);

    if (old_count > 0) {
        ESP_LOGI(TAG, "Cleared all %zu converter bindings", old_count);
    }
}

const zb_converter_def_t *zb_converter_get(uint16_t short_addr)
{
    if (!s_initialized || s_mutex == NULL) {
        return NULL;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].short_addr == short_addr) {
            const zb_converter_def_t *def = s_bindings[i].def;
            xSemaphoreGive(s_mutex);
            return def;
        }
    }

    xSemaphoreGive(s_mutex);
    return NULL;
}

/**
 * @brief Auto-rebind converter for a device after reboot
 *
 * After reboot, the binding table is empty but devices may be persisted
 * in NVS with manufacturer/model. This helper looks up the converter
 * from the DB and binds it, or falls back to the generic converter.
 *
 * @param short_addr Device short address
 * @param context    Caller context string for logging ("report" or "command")
 * @return Converter def, or NULL if no converter could be found
 */
static const zb_converter_def_t *auto_rebind_converter(uint16_t short_addr,
                                                         const char *context)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return NULL;
    }

    const zb_converter_def_t *def = NULL;

    if (dev->model[0] != '\0') {
        /* zb_converter_find() reads the converter DB from LittleFS and blocks.
         * Copy the lookup keys out first, then re-fetch the device afterwards:
         * the registry pointer is only good until the Zigbee task removes the
         * device, and this path writes back through it. */
        char manufacturer[sizeof(dev->manufacturer)];
        char model[sizeof(dev->model)];
        snprintf(manufacturer, sizeof(manufacturer), "%s", dev->manufacturer);
        snprintf(model, sizeof(model), "%s", dev->model);

        const zb_converter_def_t *conv = zb_converter_find(manufacturer, model);

        dev = device_registry_get_by_short_addr(short_addr);
        if (dev == NULL) {
            return NULL;  /* gone while we were reading the DB */
        }

        if (conv != NULL) {
            zb_converter_bind(short_addr, conv);
            dev->proto.zigbee.converter = (void *)conv;
            uint32_t caps = zb_converter_get_capabilities(conv);
            if (caps != 0) {
                dev->capabilities |= caps;
            }
            ESP_LOGI(TAG, "Auto-rebound converter for %s 0x%04X: %s",
                     context, short_addr,
                     conv->description ? conv->description : conv->model);
            def = conv;
        }
    }

    /* If still no converter, try generic fallback by capabilities */
    if (def == NULL && dev->capabilities != 0) {
        const zb_converter_def_t *generic =
            conv_generic_for_capabilities(dev->capabilities);
        if (generic != NULL) {
            zb_converter_bind(short_addr, generic);
            ESP_LOGI(TAG, "Bound generic converter for %s 0x%04X: %s",
                     context, short_addr, generic->description);
            def = generic;
        }
    }

    return def;
}

esp_err_t zb_converter_handle_report(uint16_t short_addr, uint8_t endpoint,
                                      uint16_t cluster_id, uint16_t attr_id,
                                      const void *raw, size_t len,
                                      uint8_t zcl_attr_type)
{
    const zb_converter_def_t *def = zb_converter_get(short_addr);
    if (def == NULL) {
        def = auto_rebind_converter(short_addr, "report");
        if (def == NULL) {
            ESP_LOGD(TAG, "No converter bound for 0x%04X", short_addr);
            return ESP_ERR_NOT_FOUND;
        }
    }

    ESP_LOGI(TAG, "Handle report 0x%04X: cluster=0x%04X attr=0x%04X (conv=%s)",
             short_addr, cluster_id, attr_id, def->description ? def->description : def->model);

    /* Build JSON object with matching from_zigbee converters */
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Set dispatch context for generic converters (fz_generic_attr etc.) */
    xSemaphoreTake(s_dispatch_mutex, portMAX_DELAY);

    zb_dispatch_ctx_t ctx = {
        .conv = def,
        .zcl_attr_type = zcl_attr_type,
        .cluster_id = cluster_id,
        .attr_id = attr_id,
    };
    zb_converter_set_dispatch_ctx(&ctx);

    bool handled = false;
    for (uint8_t i = 0; i < def->from_zigbee_count; i++) {
        const zb_from_zigbee_t *fz = &def->from_zigbee[i];

        /* Match cluster, and either match attr_id or accept wildcard (0xFFFF) */
        if (fz->cluster_id == cluster_id &&
            (fz->attr_id == attr_id || fz->attr_id == 0xFFFF) &&
            (fz->endpoint == 0 || fz->endpoint == endpoint)) {
            if (fz->convert != NULL && fz->json_key != NULL) {
                ESP_LOGI(TAG, "  Matched fz[%d]: key=%s", i, fz->json_key);
                esp_err_t conv_ret = fz->convert(raw, len, json, fz->json_key);
                if (conv_ret == ESP_OK) {
                    handled = true;
                } else {
                    ESP_LOGW(TAG, "Converter failed for 0x%04X cluster=0x%04X attr=0x%04X: %s",
                             short_addr, cluster_id, attr_id, esp_err_to_name(conv_ret));
                }
            }
        }
    }

    /* Clear dispatch context */
    zb_converter_set_dispatch_ctx(NULL);
    xSemaphoreGive(s_dispatch_mutex);

    if (!handled) {
        ESP_LOGD(TAG, "No fz match for 0x%04X cluster=0x%04X attr=0x%04X — fallback to generic",
                 short_addr, cluster_id, attr_id);
        cJSON_Delete(json);
        return ESP_ERR_NOT_FOUND;
    }

    /* Get device info from registry */
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        ESP_LOGW(TAG, "Device 0x%04X not found in registry", short_addr);
        cJSON_Delete(json);
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Merge the converted state into the device's state in device_registry.
     * This keeps the device_t state object in sync with the latest values.
     */
    esp_err_t merge_ret = device_registry_merge_state(device->id, json);
    if (merge_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to merge state for device 0x%04X: %s",
                 short_addr, esp_err_to_name(merge_ret));
    } else {
        ESP_LOGD(TAG, "Merged state into device_registry for 0x%04X", short_addr);
    }

    cJSON_Delete(json);

    /* Publish device state change event via event bus.
     * json_state is NULL because the event bus is async - the handler
     * reads from device_registry_get_state() instead (already merged above). */
    evt_device_state_t evt = {
        .ieee_addr = device->id,
        .short_addr = short_addr,
        .endpoint = endpoint,
        .cluster_id = cluster_id,
        .attr_id = attr_id,
        .json_state = NULL,
    };

    esp_err_t ret = event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Published state change event for 0x%04X", short_addr);
    } else {
        ESP_LOGW(TAG, "Failed to publish state event for 0x%04X: %s",
                 short_addr, esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t zb_converter_handle_command(uint16_t short_addr, uint8_t endpoint,
                                       const cJSON *json)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const zb_converter_def_t *def = zb_converter_get(short_addr);
    if (def == NULL) {
        def = auto_rebind_converter(short_addr, "command");
        if (def == NULL) {
            device_t *rebind_dev = device_registry_get_by_short_addr(short_addr);
            if (rebind_dev != NULL) {
                ESP_LOGW(TAG, "Auto-rebind failed for 0x%04X: model='%s' caps=0x%08" PRIx32,
                         short_addr, rebind_dev->model, rebind_dev->capabilities);
            } else {
                ESP_LOGW(TAG, "Auto-rebind failed for 0x%04X: device not in registry", short_addr);
            }
            return ESP_ERR_NOT_FOUND;
        }
    }

    /*
     * Get device from device_registry to access capabilities and state.
     * This allows us to validate commands against device capabilities
     * and update optimistic state if needed.
     */
    /* Only the id is needed further down, and everything between here and there
     * can block — the dispatch mutex, then the converter's own tz handlers.
     * Take the id now rather than holding the registry pointer across all of
     * that; device_id_valid tracks whether we had a device at all. */
    device_id_t device_id = 0;
    bool device_id_valid = false;
    {
        device_t *device = device_registry_get_by_short_addr(short_addr);
        if (device == NULL) {
            ESP_LOGW(TAG, "Device 0x%04X not found in device_registry for command", short_addr);
            /* Continue anyway - the device may not be synced yet */
        } else {
            device_id = device->id;
            device_id_valid = true;
            ESP_LOGD(TAG, "Processing command for device 0x%04X (caps=0x%08" PRIx32 ")",
                     short_addr, device->capabilities);
        }
    }

    /* Set dispatch context for generic converters (tz_generic_write_attr etc.) */
    xSemaphoreTake(s_dispatch_mutex, portMAX_DELAY);

    zb_dispatch_ctx_t tz_ctx = {
        .conv = def,
        .zcl_attr_type = 0,
        .cluster_id = 0,
        .attr_id = 0,
    };
    zb_converter_set_dispatch_ctx(&tz_ctx);

    bool handled = false;
    bool found = false;
    esp_err_t last_err = ESP_ERR_NOT_FOUND;

    for (uint8_t i = 0; i < def->to_zigbee_count; i++) {
        const zb_to_zigbee_t *tz = &def->to_zigbee[i];

        if (tz->json_key == NULL || tz->convert == NULL) {
            continue;
        }

        cJSON *item = cJSON_GetObjectItem(json, tz->json_key);
        if (item != NULL) {
            found = true;
            /* Update dispatch context with the matched tz entry's cluster and
             * key. The key is what tells tz_tuya_dp() which datapoint this
             * call is for. */
            tz_ctx.cluster_id = tz->cluster_id;
            tz_ctx.json_key = tz->json_key;
            zb_converter_set_dispatch_ctx(&tz_ctx);
            /* Tuya devices expect the full JSON command object, not individual values */
            const cJSON *convert_arg = (def->quirk_flags & ZB_QUIRK_TUYA_DEVICE) ? json : item;
            uint8_t target_ep = (tz->endpoint != 0) ? tz->endpoint : endpoint;
            esp_err_t conv_ret = tz->convert(short_addr, target_ep, convert_arg);
            if (conv_ret == ESP_OK) {
                handled = true;
                ESP_LOGD(TAG, "Converter handled command key '%s' for 0x%04X",
                         tz->json_key, short_addr);

                /*
                 * Update optimistic state in device_registry.
                 * This allows the device state to reflect the command immediately,
                 * before we receive the actual attribute report from the device.
                 */
                if (device_id_valid) {
                    cJSON *state_update = cJSON_CreateObject();
                    if (state_update != NULL) {
                        cJSON *value_copy = cJSON_Duplicate(item, true);
                        if (value_copy != NULL) {
                            cJSON_AddItemToObject(state_update, tz->json_key, value_copy);
                            esp_err_t merge_ret = device_registry_merge_state(device_id, state_update);
                            if (merge_ret != ESP_OK) {
                                ESP_LOGW(TAG, "Failed to update optimistic state for '%s': %s",
                                         tz->json_key, esp_err_to_name(merge_ret));
                            }
                        }
                        cJSON_Delete(state_update);
                    }
                }
            } else {
                last_err = conv_ret;
                ESP_LOGW(TAG, "Converter failed for key '%s' on 0x%04X: %s",
                         tz->json_key, short_addr, esp_err_to_name(conv_ret));
            }
        }
    }

    /* Nothing matched by name — try the datapoint map.
     *
     * A Tuya device's writable properties are exactly the keys of its datapoint
     * map, so the map is the authority and a to_zigbee entry per property is a
     * copy of it. Carrying both put the shipped database over the 7036 KB
     * partition: 1649 devices with maps, roughly ten datapoints each, every one
     * of them repeated as a to_zigbee entry that says nothing the map does not.
     *
     * Looking the command's keys up here instead keeps one source of truth and
     * costs a walk of a table that is a dozen entries long. */
    if (!handled && !found && def->tuya_dp_map != NULL && def->tuya_dp_count > 0) {
        xSemaphoreTake(s_dispatch_mutex, portMAX_DELAY);
        for (uint8_t i = 0; i < def->tuya_dp_count && !handled; i++) {
            const char *key = def->tuya_dp_map[i].key;
            if (key == NULL || cJSON_GetObjectItem(json, key) == NULL) {
                continue;
            }
            found = true;
            tz_ctx.cluster_id = ZB_TUYA_CLUSTER_ID;
            tz_ctx.json_key = key;
            zb_converter_set_dispatch_ctx(&tz_ctx);

            esp_err_t conv_ret = tz_tuya_dp(short_addr, endpoint, json);
            if (conv_ret == ESP_OK) {
                handled = true;
                if (device_id_valid) {
                    cJSON *state_update = cJSON_CreateObject();
                    if (state_update != NULL) {
                        cJSON *value_copy = cJSON_Duplicate(cJSON_GetObjectItem(json, key), true);
                        if (value_copy != NULL) {
                            cJSON_AddItemToObject(state_update, key, value_copy);
                            device_registry_merge_state(device_id, state_update);
                        }
                        cJSON_Delete(state_update);
                    }
                }
            } else {
                last_err = conv_ret;
                ESP_LOGW(TAG, "Datapoint write failed for '%s' on 0x%04X: %s",
                         key, short_addr, esp_err_to_name(conv_ret));
            }
        }
        zb_converter_set_dispatch_ctx(NULL);
        xSemaphoreGive(s_dispatch_mutex);
    }

    /* Clear dispatch context */
    zb_converter_set_dispatch_ctx(NULL);

    if (handled) return ESP_OK;
    if (found) return last_err;  /* Converter found but failed (not ESP_ERR_NOT_FOUND) */
    return ESP_ERR_NOT_FOUND;
}

size_t zb_converter_get_count(void)
{
    if (s_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    size_t count = s_definition_count;
    xSemaphoreGive(s_mutex);
    return count;
}

/* ============================================================================
 * Capability Mapping Helper
 * ============================================================================ */

/**
 * @brief Map sensor device_class string to capability flag
 */
static uint32_t map_sensor_device_class(const char *device_class)
{
    if (device_class == NULL) {
        return 0;
    }

    /* Environmental sensors */
    if (strcmp(device_class, "temperature") == 0) {
        return DEV_CAP_TEMPERATURE;
    }
    if (strcmp(device_class, "humidity") == 0) {
        return DEV_CAP_HUMIDITY;
    }
    if (strcmp(device_class, "pressure") == 0 ||
        strcmp(device_class, "atmospheric_pressure") == 0) {
        return DEV_CAP_PRESSURE;
    }
    if (strcmp(device_class, "battery") == 0) {
        return DEV_CAP_BATTERY;
    }

    /* Energy monitoring */
    if (strcmp(device_class, "power") == 0) {
        return DEV_CAP_POWER;
    }
    if (strcmp(device_class, "energy") == 0) {
        return DEV_CAP_ENERGY;
    }
    if (strcmp(device_class, "voltage") == 0) {
        return DEV_CAP_VOLTAGE;
    }
    if (strcmp(device_class, "current") == 0) {
        return DEV_CAP_CURRENT;
    }

    return 0;
}

/**
 * @brief Map binary_sensor device_class string to capability flag
 */
static uint32_t map_binary_sensor_device_class(const char *device_class)
{
    if (device_class == NULL) {
        return 0;
    }

    /* Motion/occupancy */
    if (strcmp(device_class, "motion") == 0 ||
        strcmp(device_class, "occupancy") == 0) {
        return DEV_CAP_MOTION;
    }

    /* Contact sensors (door/window) */
    if (strcmp(device_class, "contact") == 0 ||
        strcmp(device_class, "door") == 0 ||
        strcmp(device_class, "window") == 0 ||
        strcmp(device_class, "opening") == 0 ||
        strcmp(device_class, "garage_door") == 0) {
        return DEV_CAP_CONTACT;
    }

    /* Vibration */
    if (strcmp(device_class, "vibration") == 0) {
        return DEV_CAP_VIBRATION;
    }

    /* Water leak */
    if (strcmp(device_class, "water_leak") == 0 ||
        strcmp(device_class, "moisture") == 0) {
        return DEV_CAP_WATER_LEAK;
    }

    /* Safety sensors */
    if (strcmp(device_class, "smoke") == 0) {
        return DEV_CAP_SMOKE;
    }
    if (strcmp(device_class, "co") == 0 ||
        strcmp(device_class, "carbon_monoxide") == 0) {
        return DEV_CAP_CO;
    }

    return 0;
}

uint32_t zb_converter_get_capabilities(const zb_converter_def_t *def)
{
    if (def == NULL || def->exposes == NULL || def->expose_count == 0) {
        return 0;
    }

    uint32_t caps = 0;

    for (uint8_t i = 0; i < def->expose_count; i++) {
        const zb_expose_t *expose = &def->exposes[i];

        switch (expose->type) {
            case ZB_EXPOSE_LIGHT:
                /* Lights always have on/off and brightness */
                caps |= DEV_CAP_ON_OFF | DEV_CAP_BRIGHTNESS;

                /* Check feature flags for additional capabilities */
                if (expose->features & ZB_FEATURE_COLOR_TEMP) {
                    caps |= DEV_CAP_COLOR_TEMP;
                }
                if (expose->features & (ZB_FEATURE_COLOR_XY | ZB_FEATURE_COLOR_HS)) {
                    caps |= DEV_CAP_COLOR_XY;
                }
                break;

            case ZB_EXPOSE_SWITCH:
                caps |= DEV_CAP_ON_OFF;
                break;

            case ZB_EXPOSE_SENSOR:
                caps |= map_sensor_device_class(expose->device_class);
                break;

            case ZB_EXPOSE_BINARY_SENSOR:
                caps |= map_binary_sensor_device_class(expose->device_class);
                break;

            case ZB_EXPOSE_COVER:
                caps |= DEV_CAP_COVER;
                break;

            case ZB_EXPOSE_LOCK:
                caps |= DEV_CAP_LOCK;
                break;

            case ZB_EXPOSE_CLIMATE:
                caps |= DEV_CAP_CLIMATE;
                break;

            case ZB_EXPOSE_FAN:
                caps |= DEV_CAP_FAN;
                break;

            case ZB_EXPOSE_SELECT:
            case ZB_EXPOSE_NUMBER:
            case ZB_EXPOSE_BUTTON:
            case ZB_EXPOSE_TEXT:
            case ZB_EXPOSE_MAX:
            default:
                /* These expose types don't map to specific device capabilities */
                break;
        }
    }

    ESP_LOGD(TAG, "Converter %s -> capabilities 0x%08" PRIx32,
             def->description ? def->description : def->model,
             caps);

    return caps;
}
