/**
 * @file zb_binding.c
 * @brief Zigbee Binding Management Implementation
 *
 * Implements Zigbee binding management including creation, removal,
 * ZCL bind/unbind requests, NVS persistence, and MQTT integration.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_binding.h"
#include "zb_constants.h"
#include "zb_device_handler_types.h"
#include "core/device/device_registry.h"
#include "bridge_events.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/memory/memory_manager_ng.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "ZB_BINDING";

/* Module state */
static zb_binding_entry_t *s_bindings = NULL;
static size_t s_binding_count = 0;
static bool s_initialized = false;
static SemaphoreHandle_t s_binding_mutex = NULL;
static zb_binding_event_cb_t s_event_callback = NULL;

/* Pending request tracking */
typedef struct {
    uint64_t source_ieee;
    uint8_t source_ep;
    uint16_t cluster_id;
    uint64_t dest_ieee;
    uint8_t dest_ep;
    bool is_bind;           /* true = bind, false = unbind */
    bool pending;
    TickType_t timestamp;
} zb_binding_pending_t;

#define ZB_BINDING_MAX_PENDING 8
#define ZB_BINDING_REQUEST_TIMEOUT_MS 10000

static zb_binding_pending_t s_pending_requests[ZB_BINDING_MAX_PENDING];

/* NVS keys */
#define NVS_KEY_BINDING_COUNT "bind_count"
#define NVS_KEY_BINDING_DATA_FMT "bind_%02d"

/* MQTT topic patterns */
#define TOPIC_BIND_REQUEST "zigbee2mqtt/bridge/request/device/bind"
#define TOPIC_UNBIND_REQUEST "zigbee2mqtt/bridge/request/device/unbind"
#define TOPIC_BIND_RESPONSE "zigbee2mqtt/bridge/response/device/bind"
#define TOPIC_UNBIND_RESPONSE "zigbee2mqtt/bridge/response/device/unbind"
#define TOPIC_BINDINGS_LIST "zigbee2mqtt/bridge/bindings"

/* Common cluster name mappings */
typedef struct {
    uint16_t cluster_id;
    const char *name;
} cluster_name_map_t;

static const cluster_name_map_t s_cluster_names[] = {
    { 0x0000, "genBasic" },
    { 0x0001, "genPowerCfg" },
    { 0x0003, "genIdentify" },
    { 0x0004, "genGroups" },
    { 0x0005, "genScenes" },
    { 0x0006, "genOnOff" },
    { 0x0008, "genLevelCtrl" },
    { 0x000A, "genTime" },
    { 0x0019, "genOta" },
    { 0x0020, "genPollCtrl" },
    { 0x0101, "closuresDoorLock" },
    { 0x0102, "closuresWindowCovering" },
    { 0x0201, "hvacThermostat" },
    { 0x0202, "hvacFanCtrl" },
    { 0x0300, "lightingColorCtrl" },
    { 0x0400, "msIlluminanceMeasurement" },
    { 0x0402, "msTemperatureMeasurement" },
    { 0x0403, "msPressureMeasurement" },
    { 0x0405, "msRelativeHumidity" },
    { 0x0406, "msOccupancySensing" },
    { 0x0500, "ssIasZone" },
    { 0x0502, "ssIasWd" },
    { 0x0702, "seMetering" },
    { 0x0B04, "haElectricalMeasurement" },
    { 0xFC00, "manuSpecificTuya" },
    { 0, NULL }  /* Sentinel */
};

/* Forward declarations */
static zb_binding_entry_t* find_binding_entry(uint64_t source_ieee, uint8_t source_ep,
                                               uint16_t cluster_id,
                                               uint64_t dest_ieee, uint8_t dest_ep);
static zb_binding_entry_t* find_free_slot(void);
static esp_err_t send_zcl_bind_request(uint64_t source_ieee, uint8_t source_ep,
                                        uint16_t cluster_id,
                                        uint64_t dest_ieee, uint8_t dest_ep);
static esp_err_t send_zcl_unbind_request(uint64_t source_ieee, uint8_t source_ep,
                                          uint16_t cluster_id,
                                          uint64_t dest_ieee, uint8_t dest_ep);
static void notify_event(zb_binding_event_type_t event,
                         const zb_binding_entry_t *entry,
                         zb_binding_status_t status);
static esp_err_t parse_mqtt_bind_request(const char *payload, size_t len,
                                          char *from_device, size_t from_len,
                                          char *to_device, size_t to_len,
                                          uint16_t *clusters, size_t *cluster_count);
static uint64_t resolve_device_ieee(const char *device_str, uint8_t *endpoint);
static void add_pending_request(uint64_t source_ieee, uint8_t source_ep,
                                uint16_t cluster_id,
                                uint64_t dest_ieee, uint8_t dest_ep,
                                bool is_bind);
static zb_binding_pending_t* find_pending_request(uint64_t source_ieee);
static void remove_pending_request(uint64_t source_ieee);

/**
 * @brief Generic ZDO bind/unbind response callback
 *
 * Logs the result of bind/unbind ZDO requests.
 *
 * @param status ZDO response status
 * @param user_ctx User context (unused)
 */
static void zdo_bind_callback(esp_zb_zdp_status_t status, void *user_ctx)
{
    (void)user_ctx;
    if (status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGD(TAG, "ZDO bind/unbind request succeeded");
    } else {
        ESP_LOGW(TAG, "ZDO bind/unbind request failed: 0x%02x", status);
    }
}

esp_err_t zb_binding_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Binding module already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing Zigbee binding management...");

    /* Create mutex */
    s_binding_mutex = xSemaphoreCreateMutex();
    if (s_binding_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create binding mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate binding storage using memory manager (auto-selects PSRAM for large allocs) */
    s_bindings = mem_ng_calloc(ZB_BINDING_MAX_ENTRIES, sizeof(zb_binding_entry_t), MEM_CAP_PSRAM);
    if (s_bindings == NULL) {
        ESP_LOGE(TAG, "Failed to allocate binding storage");
        vSemaphoreDelete(s_binding_mutex);
        s_binding_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Binding storage allocated: %zu bytes",
             ZB_BINDING_MAX_ENTRIES * sizeof(zb_binding_entry_t));

    /* Initialize all entries as inactive */
    for (size_t i = 0; i < ZB_BINDING_MAX_ENTRIES; i++) {
        s_bindings[i].active = false;
    }

    /* Initialize pending requests */
    memset(s_pending_requests, 0, sizeof(s_pending_requests));

    s_binding_count = 0;
    s_initialized = true;

    /* Try to load bindings from NVS */
    esp_err_t ret = zb_binding_load_from_nvs();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Loaded %zu bindings from NVS", s_binding_count);
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved bindings found in NVS");
    } else {
        ESP_LOGW(TAG, "Failed to load bindings from NVS: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Binding management initialized (max: %d entries)", ZB_BINDING_MAX_ENTRIES);
    return ESP_OK;
}

esp_err_t zb_binding_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing Zigbee binding management...");

    /* Save bindings before shutdown */
    zb_binding_save_to_nvs();

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_bindings != NULL) {
        mem_ng_free(s_bindings);
        s_bindings = NULL;
    }

    s_binding_count = 0;
    s_initialized = false;
    s_event_callback = NULL;

    xSemaphoreGive(s_binding_mutex);
    vSemaphoreDelete(s_binding_mutex);
    s_binding_mutex = NULL;

    ESP_LOGI(TAG, "Binding management deinitialized");
    return ESP_OK;
}

esp_err_t zb_binding_create(uint64_t source_ieee, uint8_t source_ep,
                            uint16_t cluster_id,
                            uint64_t dest_ieee, uint8_t dest_ep)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Binding module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (source_ieee == 0 || dest_ieee == 0) {
        ESP_LOGE(TAG, "Invalid IEEE address");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Creating binding: 0x%016" PRIX64 ":%d -> 0x%016" PRIX64 ":%d (cluster 0x%04X)",
             source_ieee, source_ep, dest_ieee, dest_ep, cluster_id);

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Check if binding already exists */
    if (find_binding_entry(source_ieee, source_ep, cluster_id, dest_ieee, dest_ep) != NULL) {
        xSemaphoreGive(s_binding_mutex);
        /* Provisioning runs on every boot and is deliberately idempotent, so
         * an existing binding is the normal case, not a problem. */
        ESP_LOGD(TAG, "Binding already exists");
        return ESP_ERR_INVALID_ARG;
    }

    /* Find free slot */
    zb_binding_entry_t *entry = find_free_slot();
    if (entry == NULL) {
        xSemaphoreGive(s_binding_mutex);
        ESP_LOGE(TAG, "Binding table full (max: %d)", ZB_BINDING_MAX_ENTRIES);
        return ESP_ERR_NO_MEM;
    }

    /* Initialize entry (but mark as inactive until confirmed) */
    entry->source_ieee = source_ieee;
    entry->source_endpoint = source_ep;
    entry->cluster_id = cluster_id;
    entry->dest_ieee = dest_ieee;
    entry->dest_endpoint = dest_ep;
    entry->active = true;  /* Mark active optimistically */

    s_binding_count++;

    xSemaphoreGive(s_binding_mutex);

    /* Send ZCL Bind Request */
    esp_err_t ret = send_zcl_bind_request(source_ieee, source_ep, cluster_id, dest_ieee, dest_ep);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send Bind Request: %s", esp_err_to_name(ret));
        /* Keep binding in table anyway - device might be offline */
    }

    /* Track pending request */
    add_pending_request(source_ieee, source_ep, cluster_id, dest_ieee, dest_ep, true);

    /* Save to NVS */
    zb_binding_save_to_nvs();

    /* Notify event */
    notify_event(ZB_BINDING_EVENT_CREATED, entry, ZB_BINDING_STATUS_SUCCESS);

    ESP_LOGI(TAG, "Binding created (total: %zu)", s_binding_count);
    return ESP_OK;
}

esp_err_t zb_binding_remove(uint64_t source_ieee, uint8_t source_ep,
                            uint16_t cluster_id,
                            uint64_t dest_ieee, uint8_t dest_ep)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Removing binding: 0x%016" PRIX64 ":%d -> 0x%016" PRIX64 ":%d (cluster 0x%04X)",
             source_ieee, source_ep, dest_ieee, dest_ep, cluster_id);

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);

    zb_binding_entry_t *entry = find_binding_entry(source_ieee, source_ep, cluster_id,
                                                    dest_ieee, dest_ep);
    if (entry == NULL) {
        xSemaphoreGive(s_binding_mutex);
        ESP_LOGW(TAG, "Binding not found");
        return ESP_ERR_NOT_FOUND;
    }

    /* Copy entry for event notification */
    zb_binding_entry_t entry_copy = *entry;

    /* Mark as inactive */
    entry->active = false;
    s_binding_count--;

    xSemaphoreGive(s_binding_mutex);

    /* Send ZCL Unbind Request */
    esp_err_t ret = send_zcl_unbind_request(source_ieee, source_ep, cluster_id, dest_ieee, dest_ep);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send Unbind Request: %s", esp_err_to_name(ret));
    }

    /* Track pending request */
    add_pending_request(source_ieee, source_ep, cluster_id, dest_ieee, dest_ep, false);

    /* Save to NVS */
    zb_binding_save_to_nvs();

    /* Notify event */
    notify_event(ZB_BINDING_EVENT_REMOVED, &entry_copy, ZB_BINDING_STATUS_SUCCESS);

    ESP_LOGI(TAG, "Binding removed (remaining: %zu)", s_binding_count);
    return ESP_OK;
}

esp_err_t zb_binding_remove_device(uint64_t ieee_addr)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Removing all bindings for device 0x%016" PRIX64, ieee_addr);

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);

    size_t removed = 0;
    for (size_t i = 0; i < ZB_BINDING_MAX_ENTRIES; i++) {
        if (s_bindings[i].active &&
            (s_bindings[i].source_ieee == ieee_addr || s_bindings[i].dest_ieee == ieee_addr)) {
            s_bindings[i].active = false;
            removed++;
        }
    }

    s_binding_count -= removed;

    xSemaphoreGive(s_binding_mutex);

    if (removed > 0) {
        zb_binding_save_to_nvs();
        ESP_LOGI(TAG, "Removed %zu bindings for device", removed);
    }

    return ESP_OK;
}

const zb_binding_entry_t* zb_binding_get(size_t index)
{
    if (!s_initialized || index >= ZB_BINDING_MAX_ENTRIES) {
        return NULL;
    }

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);
    const zb_binding_entry_t *entry = NULL;
    if (s_bindings[index].active) {
        entry = &s_bindings[index];
    }
    xSemaphoreGive(s_binding_mutex);

    return entry;
}

size_t zb_binding_get_all(zb_binding_entry_t *entries, size_t max_count)
{
    if (!s_initialized || entries == NULL) {
        return 0;
    }

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);

    size_t copied = 0;
    for (size_t i = 0; i < ZB_BINDING_MAX_ENTRIES && copied < max_count; i++) {
        if (s_bindings[i].active) {
            memcpy(&entries[copied], &s_bindings[i], sizeof(zb_binding_entry_t));
            copied++;
        }
    }

    xSemaphoreGive(s_binding_mutex);

    return copied;
}

size_t zb_binding_get_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);
    size_t count = s_binding_count;
    xSemaphoreGive(s_binding_mutex);

    return count;
}

const zb_binding_entry_t* zb_binding_find(uint64_t source_ieee, uint8_t source_ep,
                                          uint16_t cluster_id,
                                          uint64_t dest_ieee, uint8_t dest_ep)
{
    if (!s_initialized) {
        return NULL;
    }

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);
    zb_binding_entry_t *entry = find_binding_entry(source_ieee, source_ep, cluster_id,
                                                    dest_ieee, dest_ep);
    xSemaphoreGive(s_binding_mutex);

    return entry;
}

size_t zb_binding_get_by_source(uint64_t source_ieee,
                                 zb_binding_entry_t *entries, size_t max_count)
{
    if (!s_initialized || entries == NULL) {
        return 0;
    }

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);

    size_t copied = 0;
    for (size_t i = 0; i < ZB_BINDING_MAX_ENTRIES && copied < max_count; i++) {
        if (s_bindings[i].active && s_bindings[i].source_ieee == source_ieee) {
            memcpy(&entries[copied], &s_bindings[i], sizeof(zb_binding_entry_t));
            copied++;
        }
    }

    xSemaphoreGive(s_binding_mutex);

    return copied;
}

size_t zb_binding_get_by_dest(uint64_t dest_ieee,
                               zb_binding_entry_t *entries, size_t max_count)
{
    if (!s_initialized || entries == NULL) {
        return 0;
    }

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);

    size_t copied = 0;
    for (size_t i = 0; i < ZB_BINDING_MAX_ENTRIES && copied < max_count; i++) {
        if (s_bindings[i].active && s_bindings[i].dest_ieee == dest_ieee) {
            memcpy(&entries[copied], &s_bindings[i], sizeof(zb_binding_entry_t));
            copied++;
        }
    }

    xSemaphoreGive(s_binding_mutex);

    return copied;
}

esp_err_t zb_binding_save_to_nvs(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Saving bindings to NVS...");

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(ZB_BINDING_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Save binding count */
    ret = nvs_set_u16(nvs_handle, NVS_KEY_BINDING_COUNT, (uint16_t)s_binding_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save binding count: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_binding_mutex);
        nvs_close(nvs_handle);
        return ret;
    }

    /* Save each active binding */
    uint8_t saved_idx = 0;
    for (size_t i = 0; i < ZB_BINDING_MAX_ENTRIES; i++) {
        if (s_bindings[i].active) {
            char key[16];
            snprintf(key, sizeof(key), NVS_KEY_BINDING_DATA_FMT, saved_idx);

            ret = nvs_set_blob(nvs_handle, key, &s_bindings[i], sizeof(zb_binding_entry_t));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save binding %zu: %s", i, esp_err_to_name(ret));
                xSemaphoreGive(s_binding_mutex);
                nvs_close(nvs_handle);
                return ret;
            }
            saved_idx++;
        }
    }

    xSemaphoreGive(s_binding_mutex);

    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Saved %zu bindings to NVS", s_binding_count);
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t zb_binding_load_from_nvs(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Loading bindings from NVS...");

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(ZB_BINDING_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS namespace not found - no saved bindings");
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Load binding count */
    uint16_t count = 0;
    ret = nvs_get_u16(nvs_handle, NVS_KEY_BINDING_COUNT, &count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load binding count: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Clear existing bindings */
    for (size_t i = 0; i < ZB_BINDING_MAX_ENTRIES; i++) {
        s_bindings[i].active = false;
    }
    s_binding_count = 0;

    /* Load each binding */
    for (uint8_t i = 0; i < count && i < ZB_BINDING_MAX_ENTRIES; i++) {
        char key[16];
        snprintf(key, sizeof(key), NVS_KEY_BINDING_DATA_FMT, i);

        size_t required_size = sizeof(zb_binding_entry_t);
        ret = nvs_get_blob(nvs_handle, key, &s_bindings[i], &required_size);
        if (ret == ESP_OK) {
            s_bindings[i].active = true;
            s_binding_count++;
            ESP_LOGD(TAG, "Loaded binding: 0x%016" PRIX64 " -> 0x%016" PRIX64 " (cluster 0x%04X)",
                     s_bindings[i].source_ieee, s_bindings[i].dest_ieee, s_bindings[i].cluster_id);
        } else {
            ESP_LOGW(TAG, "Failed to load binding %d: %s", i, esp_err_to_name(ret));
        }
    }

    xSemaphoreGive(s_binding_mutex);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Loaded %zu bindings from NVS", s_binding_count);
    return ESP_OK;
}

esp_err_t zb_binding_clear_nvs(void)
{
    ESP_LOGI(TAG, "Clearing bindings from NVS...");

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(ZB_BINDING_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_all(nvs_handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Cleared all bindings from NVS");
    }

    return ret;
}

esp_err_t zb_binding_process_mqtt_request(const char *topic, const char *payload, size_t len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (topic == NULL || payload == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Processing binding request: %s", topic);
    ESP_LOGD(TAG, "Payload: %.*s", (int)len, payload);

    bool is_bind = (strcmp(topic, TOPIC_BIND_REQUEST) == 0);
    bool is_unbind = (strcmp(topic, TOPIC_UNBIND_REQUEST) == 0);

    if (!is_bind && !is_unbind) {
        ESP_LOGW(TAG, "Unknown binding topic: %s", topic);
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse request */
    char from_device[ZB_BINDING_NAME_MAX_LEN] = {0};
    char to_device[ZB_BINDING_NAME_MAX_LEN] = {0};
    uint16_t clusters[16] = {0};
    size_t cluster_count = 0;

    esp_err_t ret = parse_mqtt_bind_request(payload, len, from_device, sizeof(from_device),
                                            to_device, sizeof(to_device),
                                            clusters, &cluster_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse binding request");
        evt_zb_bind_t evt = {
            .src_ieee = 0,
            .dst_ieee = 0,
            .dst_group = 0,
            .src_endpoint = 0,
            .dst_endpoint = 0,
            .cluster_id = 0,
            .error_code = (uint8_t)ZB_BINDING_STATUS_ERROR,
        };
        ESP_LOGI(TAG, "Publishing %s failed event via event bus", is_bind ? "bind" : "unbind");
        event_publish(EVT_ZB_BIND_FAILED, &evt, sizeof(evt));
        return ret;
    }

    ESP_LOGI(TAG, "Binding %s: from='%s' to='%s' clusters=%zu",
             is_bind ? "request" : "remove", from_device, to_device, cluster_count);

    /* Resolve device addresses */
    uint8_t from_ep = 1;
    uint8_t to_ep = 1;
    uint64_t from_ieee = resolve_device_ieee(from_device, &from_ep);
    uint64_t to_ieee = resolve_device_ieee(to_device, &to_ep);

    if (from_ieee == 0 || to_ieee == 0) {
        ESP_LOGE(TAG, "Failed to resolve device addresses");
        evt_zb_bind_t evt = {
            .src_ieee = from_ieee,
            .dst_ieee = to_ieee,
            .dst_group = 0,
            .src_endpoint = from_ep,
            .dst_endpoint = to_ep,
            .cluster_id = 0,
            .error_code = (uint8_t)ZB_BINDING_STATUS_DEVICE_NOT_FOUND,
        };
        ESP_LOGI(TAG, "Publishing %s failed event via event bus (device not found)",
                 is_bind ? "bind" : "unbind");
        event_publish(EVT_ZB_BIND_FAILED, &evt, sizeof(evt));
        return ESP_ERR_NOT_FOUND;
    }

    /* If no clusters specified, use default clusters */
    if (cluster_count == 0) {
        /* Default to common control clusters */
        clusters[0] = 0x0006;  /* genOnOff */
        clusters[1] = 0x0008;  /* genLevelCtrl */
        clusters[2] = 0x0300;  /* lightingColorCtrl */
        cluster_count = 3;
        ESP_LOGI(TAG, "No clusters specified, using defaults");
    }

    /* Create/remove bindings for each cluster */
    bool any_success = false;
    for (size_t i = 0; i < cluster_count; i++) {
        if (is_bind) {
            ret = zb_binding_create(from_ieee, from_ep, clusters[i], to_ieee, to_ep);
        } else {
            ret = zb_binding_remove(from_ieee, from_ep, clusters[i], to_ieee, to_ep);
        }

        if (ret == ESP_OK) {
            any_success = true;
        } else {
            ESP_LOGW(TAG, "Failed to %s cluster 0x%04X: %s",
                     is_bind ? "bind" : "unbind", clusters[i], esp_err_to_name(ret));
        }
    }

    /* Publish response via event bus */
    evt_zb_bind_t evt = {
        .src_ieee = from_ieee,
        .dst_ieee = to_ieee,
        .dst_group = 0,
        .src_endpoint = from_ep,
        .dst_endpoint = to_ep,
        .cluster_id = clusters[0],
        .error_code = any_success ? 0 : (uint8_t)ZB_BINDING_STATUS_ERROR,
    };

    ESP_LOGI(TAG, "Publishing %s %s event via event bus",
             is_bind ? "bind" : "unbind",
             any_success ? "success" : "failed");
    event_publish(any_success ? EVT_ZB_BIND_SUCCESS : EVT_ZB_BIND_FAILED, &evt, sizeof(evt));

    /* Publish updated binding list */
    zb_binding_publish_list();

    return any_success ? ESP_OK : ESP_FAIL;
}

esp_err_t zb_binding_publish_response(bool is_bind, const zb_binding_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build event payload */
    evt_zb_bind_t evt = {
        .src_ieee = 0,
        .dst_ieee = 0,
        .dst_group = 0,
        .src_endpoint = 1,
        .dst_endpoint = 1,
        .cluster_id = result->cluster_id,
        .error_code = result->success ? 0 : (uint8_t)result->status,
    };

    /* Try to parse IEEE addresses from source/dest names if they are hex strings */
    if (strncmp(result->source_name, "0x", 2) == 0 || strncmp(result->source_name, "0X", 2) == 0) {
        evt.src_ieee = strtoull(result->source_name, NULL, 16);
    }
    if (strncmp(result->dest_name, "0x", 2) == 0 || strncmp(result->dest_name, "0X", 2) == 0) {
        evt.dst_ieee = strtoull(result->dest_name, NULL, 16);
    }

    ESP_LOGI(TAG, "Publishing %s %s event via event bus",
             is_bind ? "bind" : "unbind",
             result->success ? "success" : "failed");

    /* Publish appropriate event based on success/failure */
    event_type_t evt_type = result->success ? EVT_ZB_BIND_SUCCESS : EVT_ZB_BIND_FAILED;
    return event_publish(evt_type, &evt, sizeof(evt));
}

esp_err_t zb_binding_publish_list(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *json = cJSON_CreateArray();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_binding_mutex, GW_TIMEOUT_LONG_TICKS);

    size_t active_binding_count = 0;
    for (size_t i = 0; i < ZB_BINDING_MAX_ENTRIES; i++) {
        if (s_bindings[i].active) {
            active_binding_count++;
            cJSON *binding_obj = cJSON_CreateObject();

            /* Format IEEE addresses as strings */
            char source_str[20];
            char dest_str[20];
            snprintf(source_str, sizeof(source_str), "0x%016" PRIx64, s_bindings[i].source_ieee);
            snprintf(dest_str, sizeof(dest_str), "0x%016" PRIx64, s_bindings[i].dest_ieee);

            /* Build binding object */
            cJSON *source = cJSON_CreateObject();
            cJSON_AddStringToObject(source, "ieee_address", source_str);
            cJSON_AddNumberToObject(source, "endpoint", s_bindings[i].source_endpoint);
            cJSON_AddItemToObject(binding_obj, "source", source);

            cJSON *target = cJSON_CreateObject();
            cJSON_AddStringToObject(target, "ieee_address", dest_str);
            cJSON_AddNumberToObject(target, "endpoint", s_bindings[i].dest_endpoint);
            cJSON_AddStringToObject(target, "type", "endpoint");
            cJSON_AddItemToObject(binding_obj, "target", target);

            /* Add cluster info */
            cJSON *clusters = cJSON_CreateArray();
            if (clusters == NULL) {
                cJSON_Delete(binding_obj);
                cJSON_Delete(json);
                xSemaphoreGive(s_binding_mutex);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddItemToArray(clusters, cJSON_CreateString(
                zb_binding_get_cluster_name(s_bindings[i].cluster_id)));
            cJSON_AddItemToObject(binding_obj, "clusters", clusters);

            cJSON_AddItemToArray(json, binding_obj);
        }
    }

    xSemaphoreGive(s_binding_mutex);

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Publishing bindings list via event bus (%zu bindings)", active_binding_count);
    ESP_LOGD(TAG, "Bindings: %s", json_str);

    /* Publish via event bus instead of direct MQTT call */
    evt_zb_bindings_list_t evt = {
        .json_bindings = json_str,
        .binding_count = active_binding_count
    };
    esp_err_t ret = event_publish(EVT_ZB_BINDINGS_LIST, &evt, sizeof(evt));

    free(json_str);
    return ret;
}

esp_err_t zb_binding_handle_bind_response(uint64_t source_ieee, uint8_t status)
{
    ESP_LOGI(TAG, "Received Bind Response from 0x%016" PRIX64 ": status=0x%02X",
             source_ieee, status);

    zb_binding_pending_t *pending = find_pending_request(source_ieee);
    if (pending == NULL) {
        ESP_LOGW(TAG, "No pending bind request for device");
        return ESP_ERR_NOT_FOUND;
    }

    zb_binding_status_t bind_status;
    if (status == 0x00) {  /* ZCL_STATUS_SUCCESS */
        bind_status = ZB_BINDING_STATUS_SUCCESS;
        ESP_LOGI(TAG, "Bind request successful");
    } else if (status == 0x84) {  /* NOT_SUPPORTED */
        bind_status = ZB_BINDING_STATUS_NOT_SUPPORTED;
        ESP_LOGW(TAG, "Binding not supported by device");
    } else if (status == 0x87) {  /* TABLE_FULL */
        bind_status = ZB_BINDING_STATUS_TABLE_FULL;
        ESP_LOGW(TAG, "Device binding table full");
    } else {
        bind_status = ZB_BINDING_STATUS_ERROR;
        ESP_LOGW(TAG, "Bind request failed with status 0x%02X", status);
    }

    /* Publish result via event bus */
    bool success = (bind_status == ZB_BINDING_STATUS_SUCCESS);
    evt_zb_bind_t evt = {
        .src_ieee = pending->source_ieee,
        .dst_ieee = pending->dest_ieee,
        .dst_group = 0,
        .src_endpoint = pending->source_ep,
        .dst_endpoint = pending->dest_ep,
        .cluster_id = pending->cluster_id,
        .error_code = success ? 0 : status,
    };

    ESP_LOGI(TAG, "Publishing bind %s event via event bus", success ? "success" : "failed");
    event_publish(success ? EVT_ZB_BIND_SUCCESS : EVT_ZB_BIND_FAILED, &evt, sizeof(evt));

    remove_pending_request(source_ieee);
    return ESP_OK;
}

esp_err_t zb_binding_handle_unbind_response(uint64_t source_ieee, uint8_t status)
{
    ESP_LOGI(TAG, "Received Unbind Response from 0x%016" PRIX64 ": status=0x%02X",
             source_ieee, status);

    zb_binding_pending_t *pending = find_pending_request(source_ieee);
    if (pending == NULL) {
        ESP_LOGW(TAG, "No pending unbind request for device");
        return ESP_ERR_NOT_FOUND;
    }

    zb_binding_status_t bind_status;
    if (status == 0x00) {  /* ZCL_STATUS_SUCCESS */
        bind_status = ZB_BINDING_STATUS_SUCCESS;
        ESP_LOGI(TAG, "Unbind request successful");
    } else if (status == 0x88) {  /* NO_ENTRY */
        bind_status = ZB_BINDING_STATUS_NO_ENTRY;
        ESP_LOGW(TAG, "No matching binding entry on device");
    } else {
        bind_status = ZB_BINDING_STATUS_ERROR;
        ESP_LOGW(TAG, "Unbind request failed with status 0x%02X", status);
    }

    /* Publish result via event bus */
    bool success = (bind_status == ZB_BINDING_STATUS_SUCCESS ||
                    bind_status == ZB_BINDING_STATUS_NO_ENTRY);
    evt_zb_bind_t evt = {
        .src_ieee = pending->source_ieee,
        .dst_ieee = pending->dest_ieee,
        .dst_group = 0,
        .src_endpoint = pending->source_ep,
        .dst_endpoint = pending->dest_ep,
        .cluster_id = pending->cluster_id,
        .error_code = success ? 0 : status,
    };

    ESP_LOGI(TAG, "Publishing unbind %s event via event bus", success ? "success" : "failed");
    event_publish(success ? EVT_ZB_BIND_SUCCESS : EVT_ZB_BIND_FAILED, &evt, sizeof(evt));

    remove_pending_request(source_ieee);
    return ESP_OK;
}

esp_err_t zb_binding_register_callback(zb_binding_event_cb_t callback)
{
    s_event_callback = callback;
    return ESP_OK;
}

const char* zb_binding_get_cluster_name(uint16_t cluster_id)
{
    for (const cluster_name_map_t *entry = s_cluster_names; entry->name != NULL; entry++) {
        if (entry->cluster_id == cluster_id) {
            return entry->name;
        }
    }

    /* Return hex string for unknown clusters */
    static char unknown_cluster[16];
    snprintf(unknown_cluster, sizeof(unknown_cluster), "0x%04X", cluster_id);
    return unknown_cluster;
}

uint16_t zb_binding_get_cluster_id(const char *name)
{
    if (name == NULL) {
        return 0xFFFF;
    }

    /* Check if it's a hex string */
    if (strncmp(name, "0x", 2) == 0 || strncmp(name, "0X", 2) == 0) {
        return (uint16_t)strtoul(name, NULL, 16);
    }

    /* Look up by name */
    for (const cluster_name_map_t *entry = s_cluster_names; entry->name != NULL; entry++) {
        if (strcasecmp(entry->name, name) == 0) {
            return entry->cluster_id;
        }
    }

    return 0xFFFF;
}

esp_err_t zb_binding_test(void)
{
    ESP_LOGI(TAG, "Running binding self-test...");

    if (!s_initialized) {
        ESP_LOGE(TAG, "Binding module not initialized");
        return ESP_FAIL;
    }

    esp_err_t ret;
    uint64_t test_src_ieee = 0x00124B0012345678ULL;
    uint64_t test_dst_ieee = 0x00124B00ABCDEF01ULL;

    /* Test 1: Create binding */
    ret = zb_binding_create(test_src_ieee, 1, 0x0006, test_dst_ieee, 1);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "Test 1 FAILED: Create binding returned %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Test 1 PASSED: Binding created");

    /* Test 2: Find binding */
    const zb_binding_entry_t *entry = zb_binding_find(test_src_ieee, 1, 0x0006, test_dst_ieee, 1);
    if (entry == NULL) {
        ESP_LOGE(TAG, "Test 2 FAILED: Binding not found");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Test 2 PASSED: Binding found");

    /* Test 3: Get count */
    size_t count = zb_binding_get_count();
    if (count == 0) {
        ESP_LOGE(TAG, "Test 3 FAILED: Count is 0");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Test 3 PASSED: Count = %zu", count);

    /* Test 4: Get by source */
    zb_binding_entry_t entries[8];
    size_t found = zb_binding_get_by_source(test_src_ieee, entries, 8);
    if (found == 0) {
        ESP_LOGE(TAG, "Test 4 FAILED: No bindings found by source");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Test 4 PASSED: Found %zu bindings by source", found);

    /* Test 5: Cluster name lookup */
    const char *name = zb_binding_get_cluster_name(0x0006);
    if (strcmp(name, "genOnOff") != 0) {
        ESP_LOGE(TAG, "Test 5 FAILED: Wrong cluster name: %s", name);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Test 5 PASSED: Cluster name = %s", name);

    /* Test 6: Cluster ID lookup */
    uint16_t cluster_id = zb_binding_get_cluster_id("genOnOff");
    if (cluster_id != 0x0006) {
        ESP_LOGE(TAG, "Test 6 FAILED: Wrong cluster ID: 0x%04X", cluster_id);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Test 6 PASSED: Cluster ID = 0x%04X", cluster_id);

    /* Test 7: Remove binding */
    ret = zb_binding_remove(test_src_ieee, 1, 0x0006, test_dst_ieee, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test 7 FAILED: Remove binding returned %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Test 7 PASSED: Binding removed");

    /* Test 8: Verify removal */
    entry = zb_binding_find(test_src_ieee, 1, 0x0006, test_dst_ieee, 1);
    if (entry != NULL) {
        ESP_LOGE(TAG, "Test 8 FAILED: Binding still exists after removal");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Test 8 PASSED: Binding no longer exists");

    ESP_LOGI(TAG, "Binding self-test PASSED (all 8 tests)");
    return ESP_OK;
}

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

static zb_binding_entry_t* find_binding_entry(uint64_t source_ieee, uint8_t source_ep,
                                               uint16_t cluster_id,
                                               uint64_t dest_ieee, uint8_t dest_ep)
{
    for (size_t i = 0; i < ZB_BINDING_MAX_ENTRIES; i++) {
        if (s_bindings[i].active &&
            s_bindings[i].source_ieee == source_ieee &&
            s_bindings[i].source_endpoint == source_ep &&
            s_bindings[i].cluster_id == cluster_id &&
            s_bindings[i].dest_ieee == dest_ieee &&
            s_bindings[i].dest_endpoint == dest_ep) {
            return &s_bindings[i];
        }
    }
    return NULL;
}

static zb_binding_entry_t* find_free_slot(void)
{
    for (size_t i = 0; i < ZB_BINDING_MAX_ENTRIES; i++) {
        if (!s_bindings[i].active) {
            return &s_bindings[i];
        }
    }
    return NULL;
}

static esp_err_t send_zcl_bind_request(uint64_t source_ieee, uint8_t source_ep,
                                        uint16_t cluster_id,
                                        uint64_t dest_ieee, uint8_t dest_ep)
{
    ESP_LOGD(TAG, "Sending Bind Request: src=0x%016" PRIX64 ":%d cluster=0x%04X dst=0x%016" PRIX64 ":%d",
             source_ieee, source_ep, cluster_id, dest_ieee, dest_ep);

    /* Find source device using NG device registry */
    device_t *device = device_registry_get(source_ieee);
    if (device == NULL) {
        ESP_LOGW(TAG, "Source device not found in registry");
        return ESP_ERR_NOT_FOUND;
    }

    /* Prepare source and destination IEEE addresses */
    esp_zb_ieee_addr_t src_ieee_arr;
    esp_zb_ieee_addr_t dst_ieee_arr;
    memcpy(src_ieee_arr, &source_ieee, sizeof(esp_zb_ieee_addr_t));
    memcpy(dst_ieee_arr, &dest_ieee, sizeof(esp_zb_ieee_addr_t));

    /* Create ZDO Bind Request */
    esp_zb_zdo_bind_req_param_t bind_req = {
        .req_dst_addr = device->proto.zigbee.short_addr,
        .src_endp = source_ep,
        .cluster_id = cluster_id,
        .dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED,
        .dst_endp = dest_ep,
    };
    memcpy(bind_req.src_address, src_ieee_arr, sizeof(esp_zb_ieee_addr_t));
    memcpy(bind_req.dst_address_u.addr_long, dst_ieee_arr, sizeof(esp_zb_ieee_addr_t));

    /* Send bind request with response logger callback.
     *
     * The stack is not thread-safe: every caller outside the Zigbee task must
     * hold the lock. Bindings used to be reachable only from the MQTT request
     * handler, but zb_reporting_provision_device() now binds from the event
     * dispatcher task after an interview, so the lock is mandatory. */
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    esp_zb_zdo_device_bind_req(&bind_req, zdo_bind_callback, NULL);
    esp_zb_lock_release();

    return ESP_OK;
}

static esp_err_t send_zcl_unbind_request(uint64_t source_ieee, uint8_t source_ep,
                                          uint16_t cluster_id,
                                          uint64_t dest_ieee, uint8_t dest_ep)
{
    ESP_LOGD(TAG, "Sending Unbind Request: src=0x%016" PRIX64 ":%d cluster=0x%04X dst=0x%016" PRIX64 ":%d",
             source_ieee, source_ep, cluster_id, dest_ieee, dest_ep);

    /* Find source device using NG device registry */
    device_t *device = device_registry_get(source_ieee);
    if (device == NULL) {
        ESP_LOGW(TAG, "Source device not found in registry");
        return ESP_ERR_NOT_FOUND;
    }

    /* Prepare source and destination IEEE addresses */
    esp_zb_ieee_addr_t src_ieee_arr;
    esp_zb_ieee_addr_t dst_ieee_arr;
    memcpy(src_ieee_arr, &source_ieee, sizeof(esp_zb_ieee_addr_t));
    memcpy(dst_ieee_arr, &dest_ieee, sizeof(esp_zb_ieee_addr_t));

    /* Create ZDO Unbind Request */
    esp_zb_zdo_bind_req_param_t unbind_req = {
        .req_dst_addr = device->proto.zigbee.short_addr,
        .src_endp = source_ep,
        .cluster_id = cluster_id,
        .dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED,
        .dst_endp = dest_ep,
    };
    memcpy(unbind_req.src_address, src_ieee_arr, sizeof(esp_zb_ieee_addr_t));
    memcpy(unbind_req.dst_address_u.addr_long, dst_ieee_arr, sizeof(esp_zb_ieee_addr_t));

    /* Send unbind request with response logger callback (see bind above for
     * why the lock is taken here). */
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    esp_zb_zdo_device_unbind_req(&unbind_req, zdo_bind_callback, NULL);
    esp_zb_lock_release();

    return ESP_OK;
}

static void notify_event(zb_binding_event_type_t event,
                         const zb_binding_entry_t *entry,
                         zb_binding_status_t status)
{
    /* Notify registered callback */
    if (s_event_callback != NULL) {
        s_event_callback(event, entry, status);
    }

    /* Publish bridge event for successful operations */
    if (status == ZB_BINDING_STATUS_SUCCESS && entry != NULL) {
        const char *cluster_name = zb_binding_get_cluster_name(entry->cluster_id);

        if (event == ZB_BINDING_EVENT_CREATED) {
            bridge_event_device_bind(entry->source_ieee, NULL,
                                     entry->dest_ieee, NULL,
                                     cluster_name);
        } else if (event == ZB_BINDING_EVENT_REMOVED) {
            bridge_event_device_unbind(entry->source_ieee, NULL,
                                       entry->dest_ieee, NULL,
                                       cluster_name);
        }
    }
}

static esp_err_t parse_mqtt_bind_request(const char *payload, size_t len,
                                          char *from_device, size_t from_len,
                                          char *to_device, size_t to_len,
                                          uint16_t *clusters, size_t *cluster_count)
{
    /* Null-terminate payload */
    char *payload_str = mem_alloc(len + 1, MEM_CAP_DEFAULT);
    if (payload_str == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(payload_str, payload, len);
    payload_str[len] = '\0';

    cJSON *json = cJSON_Parse(payload_str);
    mem_ng_free(payload_str);

    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON payload");
        return ESP_FAIL;
    }

    /* Get "from" device */
    cJSON *from = cJSON_GetObjectItem(json, "from");
    if (from == NULL || !cJSON_IsString(from)) {
        cJSON_Delete(json);
        ESP_LOGE(TAG, "Missing 'from' field in request");
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(from_device, from->valuestring, from_len - 1);
    from_device[from_len - 1] = '\0';

    /* Get "to" device */
    cJSON *to = cJSON_GetObjectItem(json, "to");
    if (to == NULL || !cJSON_IsString(to)) {
        cJSON_Delete(json);
        ESP_LOGE(TAG, "Missing 'to' field in request");
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(to_device, to->valuestring, to_len - 1);
    to_device[to_len - 1] = '\0';

    /* Get "clusters" array (optional) */
    *cluster_count = 0;
    cJSON *clusters_arr = cJSON_GetObjectItem(json, "clusters");
    if (clusters_arr != NULL && cJSON_IsArray(clusters_arr)) {
        cJSON *cluster;
        cJSON_ArrayForEach(cluster, clusters_arr) {
            if (*cluster_count >= ZB_BINDING_MAX_CLUSTERS) {
                break;  /* Maximum clusters per request */
            }
            if (cJSON_IsString(cluster)) {
                uint16_t cid = zb_binding_get_cluster_id(cluster->valuestring);
                if (cid != 0xFFFF) {
                    clusters[(*cluster_count)++] = cid;
                }
            } else if (cJSON_IsNumber(cluster)) {
                clusters[(*cluster_count)++] = (uint16_t)cluster->valueint;
            }
        }
    }

    cJSON_Delete(json);
    return ESP_OK;
}

static uint64_t resolve_device_ieee(const char *device_str, uint8_t *endpoint)
{
    if (device_str == NULL) {
        return 0;
    }

    /* Check if it's an IEEE address string (starts with 0x) */
    if (strncmp(device_str, "0x", 2) == 0 || strncmp(device_str, "0X", 2) == 0) {
        *endpoint = 1;  /* Default endpoint */
        return strtoull(device_str, NULL, 16);
    }

    /* Otherwise, treat as friendly name using NG device registry */
    device_t *device = device_registry_get_by_name(device_str);
    if (device != NULL) {
        *endpoint = device->proto.zigbee.endpoint;
        return device->id;
    }

    ESP_LOGW(TAG, "Device '%s' not found", device_str);
    return 0;
}

static void add_pending_request(uint64_t source_ieee, uint8_t source_ep,
                                uint16_t cluster_id,
                                uint64_t dest_ieee, uint8_t dest_ep,
                                bool is_bind)
{
    /* Find free slot or oldest entry */
    int oldest_idx = 0;
    TickType_t oldest_time = portMAX_DELAY;

    for (int i = 0; i < ZB_BINDING_MAX_PENDING; i++) {
        if (!s_pending_requests[i].pending) {
            /* Use free slot */
            s_pending_requests[i].source_ieee = source_ieee;
            s_pending_requests[i].source_ep = source_ep;
            s_pending_requests[i].cluster_id = cluster_id;
            s_pending_requests[i].dest_ieee = dest_ieee;
            s_pending_requests[i].dest_ep = dest_ep;
            s_pending_requests[i].is_bind = is_bind;
            s_pending_requests[i].pending = true;
            s_pending_requests[i].timestamp = xTaskGetTickCount();
            return;
        }

        if (s_pending_requests[i].timestamp < oldest_time) {
            oldest_time = s_pending_requests[i].timestamp;
            oldest_idx = i;
        }
    }

    /* Overwrite oldest entry */
    s_pending_requests[oldest_idx].source_ieee = source_ieee;
    s_pending_requests[oldest_idx].source_ep = source_ep;
    s_pending_requests[oldest_idx].cluster_id = cluster_id;
    s_pending_requests[oldest_idx].dest_ieee = dest_ieee;
    s_pending_requests[oldest_idx].dest_ep = dest_ep;
    s_pending_requests[oldest_idx].is_bind = is_bind;
    s_pending_requests[oldest_idx].pending = true;
    s_pending_requests[oldest_idx].timestamp = xTaskGetTickCount();
}

static zb_binding_pending_t* find_pending_request(uint64_t source_ieee)
{
    for (int i = 0; i < ZB_BINDING_MAX_PENDING; i++) {
        if (s_pending_requests[i].pending &&
            s_pending_requests[i].source_ieee == source_ieee) {
            return &s_pending_requests[i];
        }
    }
    return NULL;
}

static void remove_pending_request(uint64_t source_ieee)
{
    for (int i = 0; i < ZB_BINDING_MAX_PENDING; i++) {
        if (s_pending_requests[i].pending &&
            s_pending_requests[i].source_ieee == source_ieee) {
            s_pending_requests[i].pending = false;
            return;
        }
    }
}
