/**
 * @file ble_device_registry.c
 * @brief BLE Device Registry and Parser Dispatch Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_device_registry.h"
#include "ble_xiaomi.h"
#include "ble_govee.h"
#include "ble_beacon.h"
#include "ble_ruuvi.h"
#include "ble_qingping.h"
#include "ble_switchbot.h"
#include "ble_inkbird.h"
#include "../ble_constants.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "core/memory/memory_manager_ng.h"
#include <string.h>
#include <stdint.h>
#include <time.h>

static const char *TAG = "BLE_REGISTRY";

/* ============================================================================
 * Private Types
 * ============================================================================ */

/**
 * @brief Parser entry for dispatch table
 */
typedef struct {
    ble_reg_device_type_t type;
    ble_device_detector_fn_t detector;
    ble_device_parser_fn_t parser;
    const char *name;
} ble_parser_entry_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Device registry storage */
static ble_registry_tracked_device_t *s_device_registry = NULL;

/** @brief Current device count */
static size_t s_device_count = 0;

/** @brief Registry mutex for thread safety */
static SemaphoreHandle_t s_registry_mutex = NULL;

/** @brief Initialization flag */
static bool s_initialized = false;

/** @brief Device update callback */
static ble_device_update_cb_t s_update_callback = NULL;

/**
 * @brief Parser dispatch table
 *
 * Order matters - more specific detectors should come first.
 */
static const ble_parser_entry_t s_parsers[] = {
    {
        .type = BLE_REG_DEVICE_TYPE_XIAOMI_LYWSD03MMC,
        .detector = ble_xiaomi_detect,
        .parser = ble_xiaomi_parse,
        .name = "Xiaomi LYWSD03MMC"
    },
    {
        .type = BLE_REG_DEVICE_TYPE_GOVEE_H5075,
        .detector = ble_govee_detect,
        .parser = ble_govee_parse,
        .name = "Govee H5075"
    },
    {
        .type = BLE_REG_DEVICE_TYPE_RUUVI_TAG,
        .detector = ble_ruuvi_detect,
        .parser = ble_ruuvi_parse,
        .name = "Ruuvi Tag"
    },
    {
        .type = BLE_REG_DEVICE_TYPE_QINGPING,
        .detector = ble_qingping_detect,
        .parser = ble_qingping_parse,
        .name = "Qingping"
    },
    {
        .type = BLE_REG_DEVICE_TYPE_SWITCHBOT_METER,
        .detector = ble_switchbot_detect,
        .parser = ble_switchbot_parse,
        .name = "SwitchBot"
    },
    {
        .type = BLE_REG_DEVICE_TYPE_INKBIRD,
        .detector = ble_inkbird_detect,
        .parser = ble_inkbird_parse,
        .name = "Inkbird"
    },
    {
        .type = BLE_REG_DEVICE_TYPE_IBEACON,
        .detector = ble_ibeacon_detect,
        .parser = ble_ibeacon_parse,
        .name = "iBeacon"
    },
    {
        .type = BLE_REG_DEVICE_TYPE_EDDYSTONE_UID,
        .detector = ble_eddystone_detect,
        .parser = ble_beacon_parse,
        .name = "Eddystone"
    },
};

#define NUM_PARSERS (sizeof(s_parsers) / sizeof(s_parsers[0]))

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static ble_registry_tracked_device_t *find_device_by_mac(const uint8_t mac[6]);
static ble_registry_tracked_device_t *find_free_slot(void);
static ble_reg_device_type_t detect_device_type(const ble_registry_adv_data_t *adv);
static const ble_parser_entry_t *get_parser_for_type(ble_reg_device_type_t type);
static bool mac_equal(const uint8_t mac1[6], const uint8_t mac2[6]);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t ble_device_registry_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Registry already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create mutex */
    s_registry_mutex = xSemaphoreCreateMutex();
    if (s_registry_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate device registry - try PSRAM first, fall back to internal heap */
    size_t alloc_size = BLE_REGISTRY_MAX_TRACKED_DEVICES * sizeof(ble_registry_tracked_device_t);
    const char *mem_type = "internal";

    s_device_registry = mem_ng_calloc(BLE_REGISTRY_MAX_TRACKED_DEVICES,
                                       sizeof(ble_registry_tracked_device_t),
                                       MEM_CAP_PSRAM);
    if (s_device_registry != NULL) {
        mem_type = "PSRAM";
    } else {
        ESP_LOGW(TAG, "PSRAM allocation failed, falling back to internal heap");
        s_device_registry = mem_ng_calloc(BLE_REGISTRY_MAX_TRACKED_DEVICES,
                                           sizeof(ble_registry_tracked_device_t),
                                           MEM_CAP_DEFAULT);
    }

    /* Critical NULL-check after all allocation attempts */
    if (s_device_registry == NULL) {
        ESP_LOGE(TAG, "Failed to allocate device registry (requested %zu bytes)", alloc_size);
        vSemaphoreDelete(s_registry_mutex);
        s_registry_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_device_count = 0;
    s_update_callback = NULL;
    s_initialized = true;

    ESP_LOGI(TAG, "Initialized (max devices: %d, parsers: %d, memory: %s, size: %zu bytes)",
             BLE_REGISTRY_MAX_TRACKED_DEVICES, NUM_PARSERS, mem_type, alloc_size);
    return ESP_OK;
}

esp_err_t ble_device_registry_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_registry_mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_device_registry != NULL) {
        mem_ng_free(s_device_registry);
        s_device_registry = NULL;
    }

    s_device_count = 0;
    s_update_callback = NULL;
    s_initialized = false;

    xSemaphoreGive(s_registry_mutex);
    vSemaphoreDelete(s_registry_mutex);
    s_registry_mutex = NULL;

    ESP_LOGI(TAG, "Deinitialized");
    return ESP_OK;
}

bool ble_device_registry_is_initialized(void)
{
    return s_initialized;
}

esp_err_t ble_device_registry_process_adv(const ble_registry_adv_data_t *adv)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (adv == NULL || adv->adv_data == NULL || adv->adv_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Detect device type */
    ble_reg_device_type_t type = detect_device_type(adv);
    if (type == BLE_REG_DEVICE_TYPE_UNKNOWN) {
        /* Unknown device type - skip */
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Get parser for device type */
    const ble_parser_entry_t *parser_entry = get_parser_for_type(type);
    if (parser_entry == NULL || parser_entry->parser == NULL) {
        ESP_LOGW(TAG, "No parser for device type %d", type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Parse advertisement data */
    ble_parsed_data_t parsed_data;
    memset(&parsed_data, 0, sizeof(parsed_data));

    esp_err_t ret = parser_entry->parser(adv, &parsed_data);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Parser failed for %s: %s", parser_entry->name, esp_err_to_name(ret));
        return ret;
    }

    /* Update device registry */
    xSemaphoreTake(s_registry_mutex, GW_TIMEOUT_LONG_TICKS);

    ble_registry_tracked_device_t *device = find_device_by_mac(adv->mac);
    bool is_new = false;

    if (device == NULL) {
        /* New device - find free slot (uses LRU eviction if registry is full) */
        device = find_free_slot();
        is_new = true;
        memcpy(device->mac, adv->mac, 6);
        device->type = type;
        device->first_seen = adv->timestamp;
        device->update_count = 0;
        device->active = true;

        /* Generate default name */
        snprintf(device->name, BLE_REGISTRY_DEVICE_NAME_MAX_LEN, "%s_%02X%02X",
                 parser_entry->name, adv->mac[4], adv->mac[5]);

        s_device_count++;

        char mac_str[18];
        ble_registry_mac_to_string(adv->mac, mac_str, sizeof(mac_str));
        ESP_LOGI(TAG, "New device: %s [%s] type=%s",
                 device->name, mac_str, parser_entry->name);
    }

    /* Update device data */
    memcpy(&device->data, &parsed_data, sizeof(ble_parsed_data_t));
    device->last_seen = adv->timestamp;
    device->update_count++;

    /* Log update for sensor types */
    if (type == BLE_REG_DEVICE_TYPE_XIAOMI_LYWSD03MMC ||
        type == BLE_REG_DEVICE_TYPE_GOVEE_H5075 ||
        type == BLE_REG_DEVICE_TYPE_RUUVI_TAG ||
        type == BLE_REG_DEVICE_TYPE_QINGPING ||
        type == BLE_REG_DEVICE_TYPE_SWITCHBOT_METER ||
        type == BLE_REG_DEVICE_TYPE_INKBIRD) {
        ESP_LOGD(TAG, "%s: T=%.1f C, H=%.1f%%, Batt=%d%%, RSSI=%d",
                 device->name,
                 device->data.sensor.temperature,
                 device->data.sensor.humidity,
                 device->data.sensor.battery,
                 device->data.sensor.rssi);
    }

    xSemaphoreGive(s_registry_mutex);

    /* Invoke callback if registered */
    if (s_update_callback != NULL) {
        s_update_callback(device, is_new);
    }

    return ESP_OK;
}

const ble_registry_tracked_device_t *ble_device_registry_get_by_mac(const uint8_t mac[6])
{
    if (!s_initialized || mac == NULL) {
        return NULL;
    }

    xSemaphoreTake(s_registry_mutex, GW_TIMEOUT_LONG_TICKS);
    const ble_registry_tracked_device_t *device = find_device_by_mac(mac);
    xSemaphoreGive(s_registry_mutex);

    return device;
}

size_t ble_device_registry_get_all(const ble_registry_tracked_device_t **devices, size_t max_count)
{
    if (!s_initialized || devices == NULL || max_count == 0) {
        return 0;
    }

    xSemaphoreTake(s_registry_mutex, GW_TIMEOUT_LONG_TICKS);

    size_t count = 0;
    for (size_t i = 0; i < BLE_REGISTRY_MAX_TRACKED_DEVICES && count < max_count; i++) {
        if (s_device_registry[i].active) {
            devices[count++] = &s_device_registry[i];
        }
    }

    xSemaphoreGive(s_registry_mutex);
    return count;
}

size_t ble_device_registry_get_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    xSemaphoreTake(s_registry_mutex, GW_TIMEOUT_LONG_TICKS);
    size_t count = s_device_count;
    xSemaphoreGive(s_registry_mutex);

    return count;
}

size_t ble_device_registry_cleanup_stale(uint32_t timeout_s)
{
    if (!s_initialized) {
        return 0;
    }

    if (timeout_s == 0) {
        timeout_s = BLE_DEVICE_CACHE_TIMEOUT_S;
    }

    uint32_t now = (uint32_t)time(NULL);
    size_t removed = 0;

    xSemaphoreTake(s_registry_mutex, GW_TIMEOUT_LONG_TICKS);

    for (size_t i = 0; i < BLE_REGISTRY_MAX_TRACKED_DEVICES; i++) {
        if (s_device_registry[i].active) {
            uint32_t age = now - s_device_registry[i].last_seen;
            if (age > timeout_s) {
                char mac_str[18];
                ble_registry_mac_to_string(s_device_registry[i].mac, mac_str, sizeof(mac_str));
                ESP_LOGI(TAG, "Removing stale device: %s [%s] (age: %lu s)",
                         s_device_registry[i].name, mac_str, (unsigned long)age);

                memset(&s_device_registry[i], 0, sizeof(ble_registry_tracked_device_t));
                s_device_count--;
                removed++;
            }
        }
    }

    xSemaphoreGive(s_registry_mutex);

    if (removed > 0) {
        ESP_LOGI(TAG, "Cleanup: removed %d stale devices (remaining: %d)",
                 removed, s_device_count);
    }

    return removed;
}

esp_err_t ble_device_registry_set_name(const uint8_t mac[6], const char *name)
{
    if (!s_initialized || mac == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_registry_mutex, GW_TIMEOUT_LONG_TICKS);

    ble_registry_tracked_device_t *device = find_device_by_mac(mac);
    if (device == NULL) {
        xSemaphoreGive(s_registry_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(device->name, name, BLE_REGISTRY_DEVICE_NAME_MAX_LEN - 1);
    device->name[BLE_REGISTRY_DEVICE_NAME_MAX_LEN - 1] = '\0';

    xSemaphoreGive(s_registry_mutex);

    char mac_str[18];
    ble_registry_mac_to_string(mac, mac_str, sizeof(mac_str));
    ESP_LOGI(TAG, "Device [%s] renamed to '%s'", mac_str, device->name);

    return ESP_OK;
}

esp_err_t ble_device_registry_register_callback(ble_device_update_cb_t callback)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_update_callback = callback;
    ESP_LOGI(TAG, "Device update callback %s",
             callback ? "registered" : "unregistered");

    return ESP_OK;
}

const char *ble_reg_device_type_to_string(ble_reg_device_type_t type)
{
    switch (type) {
        case BLE_REG_DEVICE_TYPE_UNKNOWN:           return "Unknown";
        case BLE_REG_DEVICE_TYPE_XIAOMI_LYWSD03MMC: return "Xiaomi LYWSD03MMC";
        case BLE_REG_DEVICE_TYPE_GOVEE_H5075:       return "Govee H5075";
        case BLE_REG_DEVICE_TYPE_IBEACON:           return "iBeacon";
        case BLE_REG_DEVICE_TYPE_EDDYSTONE_UID:     return "Eddystone-UID";
        case BLE_REG_DEVICE_TYPE_EDDYSTONE_TLM:     return "Eddystone-TLM";
        case BLE_REG_DEVICE_TYPE_RUUVI_TAG:         return "Ruuvi Tag";
        case BLE_REG_DEVICE_TYPE_QINGPING:          return "Qingping";
        case BLE_REG_DEVICE_TYPE_SWITCHBOT_METER:   return "SwitchBot Meter";
        case BLE_REG_DEVICE_TYPE_SWITCHBOT_BOT:     return "SwitchBot Bot";
        case BLE_REG_DEVICE_TYPE_SWITCHBOT_CURTAIN: return "SwitchBot Curtain";
        case BLE_REG_DEVICE_TYPE_INKBIRD:           return "Inkbird";
        case BLE_REG_DEVICE_TYPE_GENERIC_SENSOR:    return "Generic Sensor";
        default:                                    return "Invalid";
    }
}

char *ble_registry_mac_to_string(const uint8_t mac[6], char *buf, size_t buf_len)
{
    if (mac == NULL || buf == NULL || buf_len < BLE_MAC_ADDRESS_STRING_LEN) {
        return NULL;
    }

    snprintf(buf, buf_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return buf;
}

/* ============================================================================
 * Private Function Implementation
 * ============================================================================ */

/**
 * @brief Find device in registry by MAC address
 *
 * @param[in] mac 6-byte MAC address
 * @return Pointer to device entry or NULL
 * @note Must be called with mutex held
 */
static ble_registry_tracked_device_t *find_device_by_mac(const uint8_t mac[6])
{
    for (size_t i = 0; i < BLE_REGISTRY_MAX_TRACKED_DEVICES; i++) {
        if (s_device_registry[i].active && mac_equal(s_device_registry[i].mac, mac)) {
            return &s_device_registry[i];
        }
    }
    return NULL;
}

/**
 * @brief Find free slot in device registry with LRU eviction
 *
 * First attempts to find an empty slot. If the registry is full,
 * evicts the least recently used (LRU) device to make room.
 *
 * @return Pointer to available slot (never NULL unless registry is broken)
 * @note Must be called with mutex held
 */
static ble_registry_tracked_device_t *find_free_slot(void)
{
    /* First pass: look for an empty slot */
    for (size_t i = 0; i < BLE_REGISTRY_MAX_TRACKED_DEVICES; i++) {
        if (!s_device_registry[i].active) {
            return &s_device_registry[i];
        }
    }

    /* Registry full - find LRU device for eviction */
    size_t lru_idx = 0;
    uint32_t oldest_seen = UINT32_MAX;

    for (size_t i = 0; i < BLE_REGISTRY_MAX_TRACKED_DEVICES; i++) {
        if (s_device_registry[i].last_seen < oldest_seen) {
            oldest_seen = s_device_registry[i].last_seen;
            lru_idx = i;
        }
    }

    /* Log the eviction */
    char mac_str[18];
    ble_registry_mac_to_string(s_device_registry[lru_idx].mac, mac_str, sizeof(mac_str));
    ESP_LOGW(TAG, "Registry full, evicting LRU device: %s [%s] (last seen: %lu s ago)",
             s_device_registry[lru_idx].name, mac_str,
             (unsigned long)((uint32_t)time(NULL) - s_device_registry[lru_idx].last_seen));

    /* Clear the slot */
    memset(&s_device_registry[lru_idx], 0, sizeof(ble_registry_tracked_device_t));
    s_device_count--;

    return &s_device_registry[lru_idx];
}

/**
 * @brief Detect device type from advertisement
 *
 * @param[in] adv Advertisement data
 * @return Detected device type
 */
static ble_reg_device_type_t detect_device_type(const ble_registry_adv_data_t *adv)
{
    for (size_t i = 0; i < NUM_PARSERS; i++) {
        if (s_parsers[i].detector != NULL) {
            ble_reg_device_type_t type = s_parsers[i].detector(adv);
            if (type != BLE_REG_DEVICE_TYPE_UNKNOWN) {
                return type;
            }
        }
    }
    return BLE_REG_DEVICE_TYPE_UNKNOWN;
}

/**
 * @brief Get parser entry for device type
 *
 * @param[in] type Device type
 * @return Pointer to parser entry or NULL
 */
static const ble_parser_entry_t *get_parser_for_type(ble_reg_device_type_t type)
{
    for (size_t i = 0; i < NUM_PARSERS; i++) {
        if (s_parsers[i].type == type) {
            return &s_parsers[i];
        }
    }

    /* Handle Eddystone TLM using same parser entry as UID */
    if (type == BLE_REG_DEVICE_TYPE_EDDYSTONE_TLM) {
        for (size_t i = 0; i < NUM_PARSERS; i++) {
            if (s_parsers[i].type == BLE_REG_DEVICE_TYPE_EDDYSTONE_UID) {
                return &s_parsers[i];
            }
        }
    }

    return NULL;
}

/**
 * @brief Compare two MAC addresses
 *
 * @param[in] mac1 First MAC address
 * @param[in] mac2 Second MAC address
 * @return true if equal, false otherwise
 */
static bool mac_equal(const uint8_t mac1[6], const uint8_t mac2[6])
{
    return memcmp(mac1, mac2, 6) == 0;
}
