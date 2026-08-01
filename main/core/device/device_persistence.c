/**
 * @file device_persistence.c
 * @brief Device Persistence Module Implementation
 *
 * Persists device registry entries to NVS for recovery across reboots.
 * Uses blob storage for device_t structs with a separate device ID list
 * for efficient enumeration.
 *
 * NVS Key Format (index-based):
 *   - "dev_count": uint16_t - number of persisted devices
 *   - "dev_list": blob - array of device_id_t values (index → IEEE mapping)
 *   - "d0", "d1", "d2", ...: blob - persisted_device_t struct at list position
 *
 * Delete uses swap-with-last to avoid re-keying all subsequent entries.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "device_persistence.h"
#include "device_registry.h"
#include "core/memory/memory_manager_ng.h"
#include "core/gateway_timeouts.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "DEV_PERSIST";

/* ============================================================================
 * Internal Data Structures
 * ============================================================================ */

/**
 * @brief Persisted device structure
 *
 * Contains only the fields that should survive reboot.
 * Runtime state (last_seen, availability, cJSON state) is NOT persisted.
 */
typedef struct __attribute__((packed)) {
    device_id_t id;                 /**< Unique device identifier */
    char friendly_name[32];         /**< User-assigned device name */
    char model[32];                 /**< Device model identifier */
    char manufacturer[32];          /**< Manufacturer name */
    device_protocol_t protocol;     /**< Device protocol type */
    uint32_t capabilities;          /**< DEV_CAP_* bitmask */
    bool interviewed;               /**< True if device interview complete */

    /** Protocol-specific data */
    union {
        struct {
            uint16_t short_addr;    /**< Zigbee network short address */
            uint8_t endpoint;       /**< Zigbee endpoint number */
            device_power_info_t power_info; /**< Power source info for availability timeouts */
        } zigbee;
        struct {
            uint8_t addr_type;      /**< BLE address type */
        } ble;
    } proto;
} persisted_device_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief NVS handle for device namespace */
static nvs_handle_t s_nvs_handle = 0;

/** @brief Module initialized flag */
static bool s_initialized = false;

/** @brief Mutex for thread-safe NVS operations */
static SemaphoreHandle_t s_persist_mutex = NULL;

/* ============================================================================
 * Internal Functions - Forward Declarations
 * ============================================================================ */

static void format_device_key(uint16_t index, char *buf, size_t buf_len);
static uint16_t get_count_internal(void);
static void device_to_persisted(const device_t *dev, persisted_device_t *persisted);
static void persisted_to_device(const persisted_device_t *persisted, device_t *dev);
static bool validate_persisted_device(const persisted_device_t *persisted);

/* ============================================================================
 * Public API - Initialization
 * ============================================================================ */

esp_err_t device_persistence_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing device persistence...");

    /* Create mutex for thread-safe NVS access */
    if (s_persist_mutex == NULL) {
        s_persist_mutex = xSemaphoreCreateMutex();
        if (s_persist_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create persistence mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Clear old NVS namespace (zb_devices) if it exists */
    nvs_handle_t legacy_handle;
    if (nvs_open("zb_devices", NVS_READWRITE, &legacy_handle) == ESP_OK) {
        nvs_erase_all(legacy_handle);
        nvs_commit(legacy_handle);
        nvs_close(legacy_handle);
        ESP_LOGI(TAG, "Old NVS namespace 'zb_devices' cleared");
    }

    esp_err_t ret = nvs_open(DEVICE_PERSIST_NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s",
                 DEVICE_PERSIST_NVS_NAMESPACE, esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;

    uint16_t count = device_persistence_get_count();
    ESP_LOGI(TAG, "Device persistence initialized (%u devices stored)", count);

    return ESP_OK;
}

esp_err_t device_persistence_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_close(s_nvs_handle);
    s_nvs_handle = 0;
    s_initialized = false;

    if (s_persist_mutex != NULL) {
        vSemaphoreDelete(s_persist_mutex);
        s_persist_mutex = NULL;
    }

    ESP_LOGI(TAG, "Device persistence deinitialized");
    return ESP_OK;
}

bool device_persistence_is_initialized(void)
{
    return s_initialized;
}

/* ============================================================================
 * Public API - Single Device Operations
 * ============================================================================ */

esp_err_t device_persistence_save(const device_t *dev)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_persist_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Mutex timeout in save");
        return ESP_ERR_TIMEOUT;
    }

    /* Convert to persisted format */
    persisted_device_t persisted;
    device_to_persisted(dev, &persisted);

    /* Read current device count and list to find existing index */
    uint16_t count = get_count_internal();
    int16_t index = -1;

    device_id_t *id_list = NULL;
    if (count > 0) {
        size_t list_size = count * sizeof(device_id_t);
        id_list = mem_alloc(list_size, MEM_CAP_DEFAULT);
        if (id_list != NULL) {
            esp_err_t ret = nvs_get_blob(s_nvs_handle, "dev_list", id_list, &list_size);
            if (ret == ESP_OK) {
                for (uint16_t i = 0; i < count; i++) {
                    if (id_list[i] == dev->id) {
                        index = (int16_t)i;
                        break;
                    }
                }
            } else {
                /* List read failed, treat as empty */
                mem_ng_free(id_list);
                id_list = NULL;
                count = 0;
            }
        } else {
            count = 0;
        }
    }

    esp_err_t ret;

    if (index >= 0) {
        /* Device already in list - overwrite blob at existing index */
        char key[8];
        format_device_key((uint16_t)index, key, sizeof(key));
        ret = nvs_set_blob(s_nvs_handle, key, &persisted, sizeof(persisted_device_t));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save device 0x%016llx at d%d: %s",
                     (unsigned long long)dev->id, index, esp_err_to_name(ret));
        }
        mem_ng_free(id_list);
    } else {
        /* New device - append to list */
        if (count >= DEVICE_PERSIST_MAX_DEVICES) {
            ESP_LOGW(TAG, "Max persisted devices reached (%u)", DEVICE_PERSIST_MAX_DEVICES);
            mem_ng_free(id_list);
            xSemaphoreGive(s_persist_mutex);
            return ESP_ERR_NO_MEM;
        }

        /* Save blob at new index */
        char key[8];
        format_device_key(count, key, sizeof(key));
        ret = nvs_set_blob(s_nvs_handle, key, &persisted, sizeof(persisted_device_t));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save device 0x%016llx at d%u: %s",
                     (unsigned long long)dev->id, count, esp_err_to_name(ret));
            mem_ng_free(id_list);
            xSemaphoreGive(s_persist_mutex);
            return ret;
        }

        /* Grow ID list */
        size_t new_size = (count + 1) * sizeof(device_id_t);
        device_id_t *new_list = mem_alloc(new_size, MEM_CAP_DEFAULT);
        if (new_list == NULL) {
            ESP_LOGE(TAG, "Failed to allocate new device list");
            nvs_erase_key(s_nvs_handle, key);  /* Clean up orphan blob */
            mem_ng_free(id_list);
            xSemaphoreGive(s_persist_mutex);
            return ESP_ERR_NO_MEM;
        }
        if (id_list != NULL && count > 0) {
            memcpy(new_list, id_list, count * sizeof(device_id_t));
        }
        new_list[count] = dev->id;
        mem_ng_free(id_list);

        /* Save updated list and count */
        ret = nvs_set_blob(s_nvs_handle, "dev_list", new_list, new_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save device list: %s", esp_err_to_name(ret));
        }
        ret = nvs_set_u16(s_nvs_handle, "dev_count", count + 1);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save device count: %s", esp_err_to_name(ret));
        }
        mem_ng_free(new_list);
    }

    /* Commit changes */
    ret = nvs_commit(s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(s_persist_mutex);

    ESP_LOGD(TAG, "Saved device 0x%016llx ('%s')",
             (unsigned long long)dev->id, dev->friendly_name);

    return ESP_OK;
}

esp_err_t device_persistence_delete(device_id_t id)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_persist_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Mutex timeout in delete");
        return ESP_ERR_TIMEOUT;
    }

    /* Read current count and list */
    uint16_t count = get_count_internal();
    if (count == 0) {
        xSemaphoreGive(s_persist_mutex);
        return ESP_OK;
    }

    size_t list_size = count * sizeof(device_id_t);
    device_id_t *id_list = mem_alloc(list_size, MEM_CAP_DEFAULT);
    if (id_list == NULL) {
        xSemaphoreGive(s_persist_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nvs_get_blob(s_nvs_handle, "dev_list", id_list, &list_size);
    if (ret != ESP_OK) {
        mem_ng_free(id_list);
        xSemaphoreGive(s_persist_mutex);
        return ret;
    }

    /* Find device index in list */
    int16_t index = -1;
    for (uint16_t i = 0; i < count; i++) {
        if (id_list[i] == id) {
            index = (int16_t)i;
            break;
        }
    }

    if (index < 0) {
        mem_ng_free(id_list);
        xSemaphoreGive(s_persist_mutex);
        ESP_LOGD(TAG, "Device 0x%016llx not in persistence", (unsigned long long)id);
        return ESP_OK;
    }

    uint16_t last = count - 1;

    if ((uint16_t)index < last) {
        /* Swap-with-last: move last device blob into the deleted slot */
        char last_key[8], del_key[8];
        format_device_key(last, last_key, sizeof(last_key));
        format_device_key((uint16_t)index, del_key, sizeof(del_key));

        persisted_device_t last_blob;
        size_t blob_size = sizeof(persisted_device_t);
        ret = nvs_get_blob(s_nvs_handle, last_key, &last_blob, &blob_size);
        if (ret == ESP_OK) {
            nvs_set_blob(s_nvs_handle, del_key, &last_blob, sizeof(persisted_device_t));
        } else {
            ESP_LOGW(TAG, "Can't read d%u for swap: %s - erasing slot",
                     last, esp_err_to_name(ret));
            nvs_erase_key(s_nvs_handle, del_key);
        }

        /* Erase last slot */
        nvs_erase_key(s_nvs_handle, last_key);

        /* Update list: move last ID into deleted position */
        id_list[index] = id_list[last];
    } else {
        /* Deleting the last entry - just erase it */
        char del_key[8];
        format_device_key((uint16_t)index, del_key, sizeof(del_key));
        nvs_erase_key(s_nvs_handle, del_key);
    }

    /* Shrink list */
    count--;
    if (count > 0) {
        nvs_set_blob(s_nvs_handle, "dev_list", id_list, count * sizeof(device_id_t));
    } else {
        ret = nvs_erase_key(s_nvs_handle, "dev_list");
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    }
    nvs_set_u16(s_nvs_handle, "dev_count", count);

    mem_ng_free(id_list);

    /* Commit changes */
    ret = nvs_commit(s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(s_persist_mutex);

    ESP_LOGI(TAG, "Deleted device 0x%016llx from NVS", (unsigned long long)id);

    return ESP_OK;
}

/* ============================================================================
 * Public API - Bulk Operations
 * ============================================================================ */

esp_err_t device_persistence_load_all(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!device_registry_is_initialized()) {
        ESP_LOGE(TAG, "Device registry not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_persist_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Mutex timeout in load_all");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Loading devices from NVS...");

    /* Get device count (already holding mutex) */
    uint16_t count = get_count_internal();
    if (count == 0) {
        ESP_LOGI(TAG, "No devices to load");
        xSemaphoreGive(s_persist_mutex);
        return ESP_OK;
    }

    /* Read device ID list */
    size_t list_size = count * sizeof(device_id_t);
    device_id_t *id_list = mem_alloc(list_size, MEM_CAP_DEFAULT);
    if (id_list == NULL) {
        ESP_LOGE(TAG, "Failed to allocate device ID list");
        xSemaphoreGive(s_persist_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nvs_get_blob(s_nvs_handle, "dev_list", id_list, &list_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read device list: %s - clearing invalid data",
                 esp_err_to_name(ret));
        mem_ng_free(id_list);
        /* Clear NVS (inline to avoid recursive mutex) */
        nvs_erase_all(s_nvs_handle);
        nvs_commit(s_nvs_handle);
        xSemaphoreGive(s_persist_mutex);
        return ESP_OK;
    }

    /* Load each device */
    uint16_t loaded = 0;
    uint16_t invalid = 0;

    for (uint16_t i = 0; i < count; i++) {
        device_id_t id = id_list[i];

        /* Skip duplicate IDs in list */
        bool duplicate = false;
        for (uint16_t j = 0; j < i; j++) {
            if (id_list[j] == id) {
                ESP_LOGW(TAG, "Duplicate device 0x%016llx in list at index %u - skipping",
                         (unsigned long long)id, i);
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            invalid++;
            continue;
        }

        /* Generate index-based NVS key */
        char key[8];
        format_device_key(i, key, sizeof(key));

        /* Read device blob (memset for backward compat with smaller old blobs) */
        persisted_device_t persisted;
        memset(&persisted, 0, sizeof(persisted));
        size_t blob_size = sizeof(persisted_device_t);
        ret = nvs_get_blob(s_nvs_handle, key, &persisted, &blob_size);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load device %s (0x%016llx): %s",
                     key, (unsigned long long)id, esp_err_to_name(ret));
            invalid++;
            continue;
        }

        /* Validate record format */
        if (!validate_persisted_device(&persisted)) {
            ESP_LOGW(TAG, "Invalid device record at %s (0x%016llx) - skipping",
                     key, (unsigned long long)id);
            invalid++;
            continue;
        }

        /* Verify blob ID matches list ID */
        if (persisted.id != id) {
            ESP_LOGW(TAG, "ID mismatch at %s: list=0x%016llx blob=0x%016llx - skipping",
                     key, (unsigned long long)id, (unsigned long long)persisted.id);
            invalid++;
            continue;
        }

        /* Release mutex for registry operations (they acquire their own mutex) */
        xSemaphoreGive(s_persist_mutex);

        /* Check if device already exists in registry */
        if (device_registry_get(id) != NULL) {
            ESP_LOGD(TAG, "Device 0x%016llx already in registry, skipping",
                     (unsigned long long)id);
            xSemaphoreTake(s_persist_mutex, GW_TIMEOUT_LONG_TICKS);
            continue;
        }

        /* Add device to registry */
        device_t *dev = device_registry_add(id, persisted.protocol);
        if (dev == NULL) {
            ESP_LOGW(TAG, "Failed to add device 0x%016llx to registry",
                     (unsigned long long)id);
            xSemaphoreTake(s_persist_mutex, GW_TIMEOUT_LONG_TICKS);
            continue;
        }

        /* Populate device fields from persisted data */
        persisted_to_device(&persisted, dev);

        loaded++;
        ESP_LOGD(TAG, "Loaded device: %s (0x%016llx)",
                 dev->friendly_name, (unsigned long long)dev->id);

        /* Reacquire mutex for next NVS read */
        xSemaphoreTake(s_persist_mutex, GW_TIMEOUT_LONG_TICKS);
    }

    mem_ng_free(id_list);

    /* If all records were invalid, clear the NVS */
    if (invalid > 0 && loaded == 0) {
        ESP_LOGW(TAG, "All %u device records were invalid - clearing NVS", invalid);
        nvs_erase_all(s_nvs_handle);
        nvs_commit(s_nvs_handle);
    } else if (invalid > 0) {
        ESP_LOGW(TAG, "Loaded %u devices, %u invalid records skipped", loaded, invalid);
    } else {
        ESP_LOGI(TAG, "Loaded %u devices from NVS", loaded);
    }

    xSemaphoreGive(s_persist_mutex);
    return ESP_OK;
}

/** @brief Context for save_all collect-then-save */
typedef struct {
    device_id_t *id_list;
    nvs_handle_t nvs;
    uint16_t count;
    uint16_t saved;
    uint16_t failed;
} save_all_ctx_t;

esp_err_t device_persistence_save_all(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!device_registry_is_initialized()) {
        ESP_LOGE(TAG, "Device registry not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_persist_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Mutex timeout in save_all");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Saving all devices to NVS...");

    /* Clear existing persisted data (inline, already holding mutex) */
    esp_err_t ret = nvs_erase_all(s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear existing data: %s", esp_err_to_name(ret));
        /* Continue anyway - we'll overwrite */
    }

    /* Build new device list and save each device */
    size_t registry_count = device_registry_count();
    if (registry_count == 0) {
        nvs_commit(s_nvs_handle);
        ESP_LOGI(TAG, "No devices to save");
        xSemaphoreGive(s_persist_mutex);
        return ESP_OK;
    }

    /* Cap allocation to max persistable */
    size_t alloc_count = registry_count;
    if (alloc_count > DEVICE_PERSIST_MAX_DEVICES) {
        alloc_count = DEVICE_PERSIST_MAX_DEVICES;
    }

    /* Allocate ID list */
    device_id_t *id_list = mem_alloc(alloc_count * sizeof(device_id_t), MEM_CAP_DEFAULT);
    if (id_list == NULL) {
        ESP_LOGE(TAG, "Failed to allocate device ID list");
        xSemaphoreGive(s_persist_mutex);
        return ESP_ERR_NO_MEM;
    }

    save_all_ctx_t ctx = {
        .id_list = id_list,
        .nvs = s_nvs_handle,
        .count = 0,
        .saved = 0,
        .failed = 0
    };

    /* Collect device IDs first, then save each outside registry mutex
     * (AP-1.3: avoid holding registry mutex during NVS I/O) */
    device_id_t collected_ids[DEVICE_REGISTRY_MAX_DEVICES];
    size_t collected_count;
    xSemaphoreGive(s_persist_mutex);

    if (device_registry_collect_ids(collected_ids, DEVICE_REGISTRY_MAX_DEVICES,
                                     &collected_count) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to collect device IDs");
        collected_count = 0;
    }

    xSemaphoreTake(s_persist_mutex, GW_TIMEOUT_LONG_TICKS);

    for (size_t i = 0; i < collected_count && ctx.count < DEVICE_PERSIST_MAX_DEVICES; i++) {
        device_t *dev = device_registry_get(collected_ids[i]);
        if (dev == NULL) {
            continue;  /* Device removed between collect and save */
        }

        persisted_device_t persisted;
        device_to_persisted(dev, &persisted);

        /* Everything needed past this point is now copied out. nvs_set_blob()
         * writes flash and blocks, and the registry pointer is only valid
         * until the Zigbee task removes the device. */
        const device_id_t dev_id = dev->id;

        char key[8];
        format_device_key(ctx.count, key, sizeof(key));

        esp_err_t save_ret = nvs_set_blob(ctx.nvs, key, &persisted, sizeof(persisted_device_t));
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save device 0x%016llx at %s: %s",
                     (unsigned long long)dev_id, key, esp_err_to_name(save_ret));
            ctx.failed++;
        } else {
            ctx.id_list[ctx.count] = dev_id;
            ctx.count++;
            ctx.saved++;
        }
    }

    /* Save device list */
    if (ctx.count > 0) {
        ret = nvs_set_blob(s_nvs_handle, "dev_list", ctx.id_list, ctx.count * sizeof(device_id_t));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save device list: %s", esp_err_to_name(ret));
        }

        /* Save device count */
        ret = nvs_set_u16(s_nvs_handle, "dev_count", ctx.count);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save device count: %s", esp_err_to_name(ret));
        }
    }

    mem_ng_free(id_list);

    /* Commit all changes */
    ret = nvs_commit(s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(s_persist_mutex);

    if (registry_count > DEVICE_PERSIST_MAX_DEVICES) {
        ESP_LOGW(TAG, "Saved %u devices to NVS (%u failed, %zu exceeded max and not saved)",
                 ctx.saved, ctx.failed, registry_count - DEVICE_PERSIST_MAX_DEVICES);
    } else {
        ESP_LOGI(TAG, "Saved %u devices to NVS (%u failed)", ctx.saved, ctx.failed);
    }

    return ESP_OK;
}

esp_err_t device_persistence_clear(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_persist_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Mutex timeout in clear");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Clearing all persisted devices...");

    /* Erase the entire namespace */
    esp_err_t ret = nvs_erase_all(s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS namespace: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_persist_mutex);
        return ret;
    }

    /* Commit changes */
    ret = nvs_commit(s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(s_persist_mutex);

    ESP_LOGI(TAG, "All persisted devices cleared");

    return ESP_OK;
}

/* ============================================================================
 * Public API - Statistics
 * ============================================================================ */

uint16_t device_persistence_get_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    if (xSemaphoreTake(s_persist_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        return 0;
    }

    uint16_t count = get_count_internal();

    xSemaphoreGive(s_persist_mutex);
    return count;
}

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

/**
 * @brief Format device index as NVS key
 *
 * Uses simple index-based keys: "d0", "d1", "d2", etc.
 * Maximum key length is 6 chars ("d65535"), well within NVS limit of 15.
 * The dev_list blob provides the index → device_id mapping.
 */
static void format_device_key(uint16_t index, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "d%u", index);
}

/**
 * @brief Read device count from NVS (caller must hold mutex)
 */
static uint16_t get_count_internal(void)
{
    uint16_t count = 0;
    esp_err_t ret = nvs_get_u16(s_nvs_handle, "dev_count", &count);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read device count: %s", esp_err_to_name(ret));
    }
    return count;
}

/**
 * @brief Validate a persisted device record
 *
 * Checks if the record appears to be in the correct format.
 * Invalid records (e.g., from old format) are rejected.
 *
 * @param persisted Pointer to persisted device record
 * @return true if valid, false if invalid/corrupted
 */
static bool validate_persisted_device(const persisted_device_t *persisted)
{
    /* Check for valid device ID (non-zero) */
    if (persisted->id == 0) {
        ESP_LOGD(TAG, "Validation: device_id=0");
        return false;
    }

    /* Check for valid protocol */
    if (persisted->protocol > DEV_PROTOCOL_VIRTUAL) {
        ESP_LOGD(TAG, "Validation: invalid protocol %d", (int)persisted->protocol);
        return false;
    }

    /* Ensure strings are null-terminated (prevent reading garbage) */
    if (persisted->friendly_name[sizeof(persisted->friendly_name) - 1] != '\0' ||
        persisted->model[sizeof(persisted->model) - 1] != '\0' ||
        persisted->manufacturer[sizeof(persisted->manufacturer) - 1] != '\0') {
        ESP_LOGD(TAG, "Validation: unterminated string in device 0x%016llx",
                 (unsigned long long)persisted->id);
        return false;
    }

    /* For Zigbee, validate protocol-specific fields */
    if (persisted->protocol == DEV_PROTOCOL_ZIGBEE) {
        /* EP0 fixup: devices persisted with endpoint=0 (ZDO) are invalid.
         * Fix to EP1 (standard application endpoint) instead of rejecting. */
        if (persisted->proto.zigbee.endpoint == 0) {
            ESP_LOGW(TAG, "Migrating EP0 -> EP1 for device 0x%016llx",
                     (unsigned long long)persisted->id);
            ((persisted_device_t *)persisted)->proto.zigbee.endpoint = 1;
        }
        if (persisted->proto.zigbee.endpoint > 240) {
            ESP_LOGD(TAG, "Validation: invalid endpoint %d for 0x%016llx",
                     persisted->proto.zigbee.endpoint, (unsigned long long)persisted->id);
            return false;
        }
        /* 0xFFFF is broadcast/invalid short address */
        if (persisted->proto.zigbee.short_addr == 0xFFFF) {
            ESP_LOGD(TAG, "Validation: invalid short_addr 0xFFFF for 0x%016llx",
                     (unsigned long long)persisted->id);
            return false;
        }
    }

    return true;
}

/**
 * @brief Convert device_t to persisted_device_t
 */
static void device_to_persisted(const device_t *dev, persisted_device_t *persisted)
{
    memset(persisted, 0, sizeof(persisted_device_t));

    persisted->id = dev->id;
    persisted->protocol = dev->protocol;
    persisted->capabilities = dev->capabilities;
    persisted->interviewed = dev->interviewed;

    strncpy(persisted->friendly_name, dev->friendly_name, sizeof(persisted->friendly_name) - 1);
    strncpy(persisted->model, dev->model, sizeof(persisted->model) - 1);
    strncpy(persisted->manufacturer, dev->manufacturer, sizeof(persisted->manufacturer) - 1);

    /* Protocol-specific data */
    switch (dev->protocol) {
        case DEV_PROTOCOL_ZIGBEE:
            persisted->proto.zigbee.short_addr = dev->proto.zigbee.short_addr;
            persisted->proto.zigbee.endpoint = dev->proto.zigbee.endpoint;
            persisted->proto.zigbee.power_info = dev->proto.zigbee.power_info;
            break;
        case DEV_PROTOCOL_BLE:
            persisted->proto.ble.addr_type = dev->proto.ble.addr_type;
            break;
        case DEV_PROTOCOL_VIRTUAL:
            /* No protocol-specific data */
            break;
    }
}

/**
 * @brief Convert persisted_device_t to device_t
 *
 * Note: This populates fields in an existing device_t that was
 * already initialized via device_registry_add().
 */
static void persisted_to_device(const persisted_device_t *persisted, device_t *dev)
{
    /* Note: id and protocol are already set by device_registry_add() */
    dev->capabilities = persisted->capabilities;
    dev->interviewed = persisted->interviewed;

    /* Reset runtime state */
    dev->availability = DEV_AVAIL_UNKNOWN;
    dev->last_seen = 0;

    strncpy(dev->friendly_name, persisted->friendly_name, sizeof(dev->friendly_name) - 1);
    strncpy(dev->model, persisted->model, sizeof(dev->model) - 1);
    strncpy(dev->manufacturer, persisted->manufacturer, sizeof(dev->manufacturer) - 1);

    /* Protocol-specific data */
    switch (persisted->protocol) {
        case DEV_PROTOCOL_ZIGBEE:
            dev->proto.zigbee.short_addr = persisted->proto.zigbee.short_addr;
            dev->proto.zigbee.endpoint = persisted->proto.zigbee.endpoint;
            dev->proto.zigbee.power_info = persisted->proto.zigbee.power_info;
            dev->proto.zigbee.lqi = 0;  /* Runtime value */

            /* Generate default friendly_name for Zigbee devices persisted
             * without one. Without this, state_topic becomes "zigbee2mqtt/"
             * and discovery name falls back to model. */
            if (dev->friendly_name[0] == '\0' && dev->proto.zigbee.short_addr != 0) {
                snprintf(dev->friendly_name, sizeof(dev->friendly_name),
                         "Device_0x%04X", dev->proto.zigbee.short_addr);
            }
            break;
        case DEV_PROTOCOL_BLE:
            dev->proto.ble.addr_type = persisted->proto.ble.addr_type;
            dev->proto.ble.rssi = 0;    /* Runtime value */
            dev->proto.ble.service_uuid = 0;  /* Runtime value */
            break;
        case DEV_PROTOCOL_VIRTUAL:
            /* No protocol-specific data */
            break;
    }
}
