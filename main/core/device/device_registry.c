/**
 * @file device_registry.c
 * @brief Device Registry Implementation for ESP32-C5 Zigbee2MQTT NG
 *
 * Implements a PSRAM-backed device registry with hash table for O(1) lookups.
 * Thread-safe via FreeRTOS mutex.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "device_registry.h"
#include "memory_manager_ng.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

static const char *TAG = "device_reg";

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

/** @brief Hash table empty slot marker */
#define HASH_SLOT_EMPTY     0xFFFF

/** @brief Hash table deleted slot marker */
#define HASH_SLOT_DELETED   0xFFFE

/* ============================================================================
 * Internal State
 * ============================================================================ */

/** @brief Device array (PSRAM allocated) */
static device_t *s_devices = NULL;

/** @brief Per-device state JSON array (PSRAM allocated) */
static cJSON **s_device_states = NULL;

/** @brief Hash table mapping device_id hash to device index (PSRAM allocated) */
static uint16_t *s_hash_table = NULL;

/** @brief Registry mutex */
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Initialization flag */
static bool s_initialized = false;

/** @brief Current device count */
static size_t s_device_count = 0;

/* Round-robin cursor for slot allocation, see find_free_slot() */
static uint16_t s_alloc_cursor = 0;

/** @brief Memory used tracking */
static size_t s_memory_used = 0;

/* ============================================================================
 * Internal Helper Functions - Hash Table
 * ============================================================================ */

/**
 * @brief Compute hash from device ID
 *
 * Simple hash using XOR folding for 64-bit ID to fit hash table size.
 *
 * @param id Device ID
 * @return Hash index (0 to DEVICE_REGISTRY_HASH_SIZE - 1)
 */
static inline uint32_t hash_device_id(device_id_t id)
{
    /* XOR fold 64-bit to 32-bit, then take modulo */
    uint32_t hash = (uint32_t)(id ^ (id >> 32));
    /* Multiply-shift hash for better distribution */
    hash = (hash * 2654435761U) >> 16;
    return hash & (DEVICE_REGISTRY_HASH_SIZE - 1);
}

/**
 * @brief Find hash table slot for device ID
 *
 * Uses linear probing for collision resolution.
 *
 * @param id Device ID to find
 * @param[out] out_slot Found slot index (even if empty)
 * @return true if device found, false if empty slot found
 */
static bool hash_find_slot(device_id_t id, uint32_t *out_slot)
{
    uint32_t start = hash_device_id(id);
    uint32_t slot = start;

    do {
        uint16_t idx = s_hash_table[slot];

        if (idx == HASH_SLOT_EMPTY) {
            /* Empty slot - device not in table */
            *out_slot = slot;
            return false;
        }

        if (idx == HASH_SLOT_DELETED) {
            /* Deleted slot - continue probing */
            slot = (slot + 1) & (DEVICE_REGISTRY_HASH_SIZE - 1);
            continue;
        }

        /* Check if this slot contains our device */
        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].id == id) {
            *out_slot = slot;
            return true;
        }

        /* Linear probe to next slot */
        slot = (slot + 1) & (DEVICE_REGISTRY_HASH_SIZE - 1);

    } while (slot != start);

    /* Table is full (shouldn't happen if sized properly) */
    *out_slot = start;
    return false;
}

/**
 * @brief Insert device index into hash table
 *
 * @param id Device ID
 * @param device_idx Index in device array
 * @return true on success, false if table full
 */
static bool hash_insert(device_id_t id, uint16_t device_idx)
{
    uint32_t slot;

    if (hash_find_slot(id, &slot)) {
        /* Already exists - update index */
        s_hash_table[slot] = device_idx;
        return true;
    }

    /* Insert at empty/deleted slot */
    if (s_hash_table[slot] == HASH_SLOT_EMPTY ||
        s_hash_table[slot] == HASH_SLOT_DELETED) {
        s_hash_table[slot] = device_idx;
        return true;
    }

    /* Table full - should not happen */
    ESP_LOGE(TAG, "Hash table full!");
    return false;
}

/**
 * @brief Remove device from hash table
 *
 * @param id Device ID to remove
 */
static void hash_remove(device_id_t id)
{
    uint32_t slot;

    if (hash_find_slot(id, &slot)) {
        s_hash_table[slot] = HASH_SLOT_DELETED;
    }
}

/* ============================================================================
 * Internal Helper Functions - Device Array
 * ============================================================================ */

/**
 * @brief Find free slot in device array (round-robin)
 *
 * Starts the search after the slot handed out last time instead of restarting
 * at 0. Scanning from 0 meant that a device leaving slot 3 handed that exact
 * slot to the next device that joined, so any device_t* another task was still
 * holding silently started pointing at a different device. Cycling through the
 * array first makes that window as wide as the table allows.
 *
 * This narrows the race, it does not remove it — see the pointer lifetime note
 * in device_registry.h.
 *
 * @return Index of free slot, or DEVICE_REGISTRY_MAX_DEVICES if full
 */
static uint16_t find_free_slot(void)
{
    for (uint16_t n = 0; n < DEVICE_REGISTRY_MAX_DEVICES; n++) {
        uint16_t i = (uint16_t)((s_alloc_cursor + n) % DEVICE_REGISTRY_MAX_DEVICES);
        if (!s_devices[i].in_use) {
            s_alloc_cursor = (uint16_t)((i + 1) % DEVICE_REGISTRY_MAX_DEVICES);
            return i;
        }
    }
    return DEVICE_REGISTRY_MAX_DEVICES;
}

/**
 * @brief Acquire registry mutex with timeout
 *
 * @return true if acquired, false on timeout
 */
static bool mutex_acquire(void)
{
    if (!s_mutex) {
        return false;
    }
    return xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(DEVICE_REGISTRY_MUTEX_TIMEOUT_MS)) == pdTRUE;
}

/**
 * @brief Release registry mutex
 */
static void mutex_release(void)
{
    if (s_mutex) {
        xSemaphoreGiveRecursive(s_mutex);
    }
}

/* ============================================================================
 * Public API - Initialization
 * ============================================================================ */

esp_err_t device_registry_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing device registry (max %d devices, hash size %d)",
             DEVICE_REGISTRY_MAX_DEVICES, DEVICE_REGISTRY_HASH_SIZE);

    /* Create mutex */
    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate device array in PSRAM */
    size_t devices_size = sizeof(device_t) * DEVICE_REGISTRY_MAX_DEVICES;
    s_devices = (device_t *)mem_ng_calloc(DEVICE_REGISTRY_MAX_DEVICES, sizeof(device_t), MEM_CAP_PSRAM);
    if (!s_devices) {
        ESP_LOGE(TAG, "Failed to allocate device array (%zu bytes)", devices_size);
        goto fail;
    }
    s_memory_used += devices_size;

    /* Allocate state pointer array in PSRAM */
    size_t states_size = sizeof(cJSON *) * DEVICE_REGISTRY_MAX_DEVICES;
    s_device_states = (cJSON **)mem_ng_calloc(DEVICE_REGISTRY_MAX_DEVICES, sizeof(cJSON *), MEM_CAP_PSRAM);
    if (!s_device_states) {
        ESP_LOGE(TAG, "Failed to allocate state array (%zu bytes)", states_size);
        goto fail;
    }
    s_memory_used += states_size;

    /* Allocate hash table in PSRAM */
    size_t hash_size = sizeof(uint16_t) * DEVICE_REGISTRY_HASH_SIZE;
    s_hash_table = (uint16_t *)mem_alloc(hash_size, MEM_CAP_PSRAM);
    if (!s_hash_table) {
        ESP_LOGE(TAG, "Failed to allocate hash table (%zu bytes)", hash_size);
        goto fail;
    }
    s_memory_used += hash_size;

    /* Initialize hash table to empty */
    for (size_t i = 0; i < DEVICE_REGISTRY_HASH_SIZE; i++) {
        s_hash_table[i] = HASH_SLOT_EMPTY;
    }

    s_device_count = 0;
    s_initialized = true;

    ESP_LOGI(TAG, "Device registry initialized, memory used: %zu bytes", s_memory_used);
    return ESP_OK;

fail:
    if (s_hash_table) {
        mem_ng_free(s_hash_table);
        s_hash_table = NULL;
    }
    if (s_device_states) {
        mem_ng_free(s_device_states);
        s_device_states = NULL;
    }
    if (s_devices) {
        mem_ng_free(s_devices);
        s_devices = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    s_memory_used = 0;
    return ESP_ERR_NO_MEM;
}

esp_err_t device_registry_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing device registry");

    if (!mutex_acquire()) {
        ESP_LOGE(TAG, "Failed to acquire mutex for deinit");
        return ESP_ERR_TIMEOUT;
    }

    /* Free all device states */
    for (size_t i = 0; i < DEVICE_REGISTRY_MAX_DEVICES; i++) {
        if (s_device_states[i]) {
            cJSON_Delete(s_device_states[i]);
            s_device_states[i] = NULL;
        }
    }

    /* Free allocations */
    mem_ng_free(s_hash_table);
    s_hash_table = NULL;

    mem_ng_free(s_device_states);
    s_device_states = NULL;

    mem_ng_free(s_devices);
    s_devices = NULL;

    s_device_count = 0;
    s_memory_used = 0;
    s_initialized = false;

    mutex_release();

    /* Delete mutex */
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;

    ESP_LOGI(TAG, "Device registry deinitialized");
    return ESP_OK;
}

esp_err_t device_registry_clear_all(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!mutex_acquire()) {
        ESP_LOGE(TAG, "Failed to acquire mutex for clear_all");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Clearing all devices from registry (count=%zu)", s_device_count);

    /* Free all device states */
    for (size_t i = 0; i < DEVICE_REGISTRY_MAX_DEVICES; i++) {
        if (s_device_states[i]) {
            cJSON_Delete(s_device_states[i]);
            s_device_states[i] = NULL;
        }
        /* Clear device slot */
        if (s_devices[i].in_use) {
            memset(&s_devices[i], 0, sizeof(device_t));
        }
    }

    /* Reset hash table to empty */
    for (size_t i = 0; i < DEVICE_REGISTRY_HASH_SIZE; i++) {
        s_hash_table[i] = HASH_SLOT_EMPTY;
    }

    s_device_count = 0;

    mutex_release();

    ESP_LOGI(TAG, "All devices cleared from registry");
    return ESP_OK;
}

bool device_registry_is_initialized(void)
{
    return s_initialized;
}

/* ============================================================================
 * Public API - Device Management
 * ============================================================================ */

device_t *device_registry_add(device_id_t id, device_protocol_t proto)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return NULL;
    }

    if (id == 0) {
        ESP_LOGE(TAG, "Invalid device ID (0)");
        return NULL;
    }

    if (!mutex_acquire()) {
        ESP_LOGE(TAG, "Mutex timeout in add");
        return NULL;
    }

    device_t *result = NULL;

    /* Check if device already exists */
    uint32_t hash_slot;
    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];
        ESP_LOGW(TAG, "Device 0x%016llx already exists at index %u",
                 (unsigned long long)id, idx);
        result = &s_devices[idx];
        goto done;
    }

    /* Find free slot */
    uint16_t free_idx = find_free_slot();
    if (free_idx >= DEVICE_REGISTRY_MAX_DEVICES) {
        ESP_LOGE(TAG, "Registry full (max %d devices)", DEVICE_REGISTRY_MAX_DEVICES);
        goto done;
    }

    /* Initialize device */
    device_t *dev = &s_devices[free_idx];
    memset(dev, 0, sizeof(device_t));
    device_init(dev, id, proto);
    dev->in_use = true;
    dev->last_seen = time(NULL);

    /* Insert into hash table */
    if (!hash_insert(id, free_idx)) {
        ESP_LOGE(TAG, "Failed to insert into hash table");
        dev->in_use = false;
        goto done;
    }

    s_device_count++;
    result = dev;

    ESP_LOGI(TAG, "Added device 0x%016llx at index %u (proto=%d, count=%zu)",
             (unsigned long long)id, free_idx, (int)proto, s_device_count);

done:
    mutex_release();
    return result;
}

device_t *device_registry_get(device_id_t id)
{
    if (!s_initialized || id == 0) {
        return NULL;
    }

    if (!mutex_acquire()) {
        return NULL;
    }

    device_t *result = NULL;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];
        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            result = &s_devices[idx];
        }
    }

    mutex_release();
    return result;
}

device_t *device_registry_get_by_name(const char *friendly_name)
{
    if (!s_initialized || !friendly_name || friendly_name[0] == '\0') {
        return NULL;
    }

    if (!mutex_acquire()) {
        return NULL;
    }

    device_t *result = NULL;

    for (size_t i = 0; i < DEVICE_REGISTRY_MAX_DEVICES; i++) {
        if (s_devices[i].in_use &&
            strncmp(s_devices[i].friendly_name, friendly_name, sizeof(s_devices[i].friendly_name)) == 0) {
            result = &s_devices[i];
            break;
        }
    }

    mutex_release();
    return result;
}

device_t *device_registry_find_by_id(const char *id_str)
{
    if (id_str == NULL) {
        return NULL;
    }

    /* 1. Try as friendly_name */
    device_t *dev = device_registry_get_by_name(id_str);
    if (dev) {
        return dev;
    }

    /* 2. Try as IEEE address string (0x00124b001234abcd) */
    size_t len = strlen(id_str);
    if (len >= 18 && id_str[0] == '0' && id_str[1] == 'x') {
        device_id_t id = strtoull(id_str + 2, NULL, 16);
        return device_registry_get(id);
    }

    return NULL;
}

esp_err_t device_registry_get_friendly_name(device_id_t id, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!mutex_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            strncpy(buf, s_devices[idx].friendly_name, buf_len - 1);
            buf[buf_len - 1] = '\0';
            ret = ESP_OK;
        }
    }

    mutex_release();
    return ret;
}

device_t *device_registry_get_by_short_addr(uint16_t short_addr)
{
    if (!s_initialized || short_addr == 0xFFFF) {
        return NULL;
    }

    if (!mutex_acquire()) {
        return NULL;
    }

    device_t *result = NULL;

    for (size_t i = 0; i < DEVICE_REGISTRY_MAX_DEVICES; i++) {
        if (s_devices[i].in_use &&
            s_devices[i].protocol == DEV_PROTOCOL_ZIGBEE &&
            s_devices[i].proto.zigbee.short_addr == short_addr) {
            result = &s_devices[i];
            break;
        }
    }

    mutex_release();
    return result;
}

esp_err_t device_registry_remove(device_id_t id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!mutex_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            /* Free state if exists */
            if (s_device_states[idx]) {
                cJSON_Delete(s_device_states[idx]);
                s_device_states[idx] = NULL;
            }

            /* Clear device slot */
            memset(&s_devices[idx], 0, sizeof(device_t));
            s_devices[idx].in_use = false;

            /* Remove from hash table */
            hash_remove(id);

            s_device_count--;
            ret = ESP_OK;

            ESP_LOGI(TAG, "Removed device 0x%016llx (count=%zu)",
                     (unsigned long long)id, s_device_count);
        }
    }

    mutex_release();
    return ret;
}

/* ============================================================================
 * Public API - State Management
 * ============================================================================ */

cJSON *device_registry_get_state(device_id_t id)
{
    if (!s_initialized || id == 0) {
        return NULL;
    }

    if (!mutex_acquire()) {
        return NULL;
    }

    cJSON *result = NULL;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];
        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            result = s_device_states[idx];
        }
    }

    mutex_release();
    return result;
}

cJSON *device_registry_state_dup(device_id_t id)
{
    if (!s_initialized || id == 0) {
        return NULL;
    }

    if (!mutex_acquire()) {
        return NULL;
    }

    cJSON *copy = NULL;
    uint32_t hash_slot;

    /* Duplicate under the mutex: set_state()/remove() cannot delete the
     * original mid-copy, so the caller gets an object of their own. */
    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];
        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use &&
            s_device_states[idx] != NULL) {
            copy = cJSON_Duplicate(s_device_states[idx], true);
        }
    }

    mutex_release();
    return copy;
}

esp_err_t device_registry_set_state(device_id_t id, cJSON *state)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (id == 0 || !state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!mutex_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            /* Free existing state */
            if (s_device_states[idx]) {
                cJSON_Delete(s_device_states[idx]);
            }

            /* Take ownership of new state */
            s_device_states[idx] = state;
            ret = ESP_OK;

            ESP_LOGD(TAG, "Set state for device 0x%016llx", (unsigned long long)id);
        }
    }

    /* If device not found, caller still owns state - they should free it */
    mutex_release();
    return ret;
}

esp_err_t device_registry_update_state(device_id_t id, const char *key, cJSON *value)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (id == 0 || !key || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!mutex_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            /* Create state object if doesn't exist */
            if (!s_device_states[idx]) {
                s_device_states[idx] = cJSON_CreateObject();
                if (!s_device_states[idx]) {
                    mutex_release();
                    cJSON_Delete(value);
                    return ESP_ERR_NO_MEM;
                }
            }

            /* Delete existing key if present */
            cJSON_DeleteItemFromObject(s_device_states[idx], key);

            /* Add new value (takes ownership) */
            cJSON_AddItemToObject(s_device_states[idx], key, value);
            ret = ESP_OK;

            ESP_LOGD(TAG, "Updated state key '%s' for device 0x%016llx",
                     key, (unsigned long long)id);
        }
    }

    if (ret != ESP_OK) {
        /* Caller's value not consumed - they should free it */
        cJSON_Delete(value);
    }

    mutex_release();
    return ret;
}

esp_err_t device_registry_merge_state(device_id_t id, cJSON *partial)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (id == 0 || !partial || !cJSON_IsObject(partial)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!mutex_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            /* Create state object if doesn't exist */
            if (!s_device_states[idx]) {
                s_device_states[idx] = cJSON_CreateObject();
                if (!s_device_states[idx]) {
                    mutex_release();
                    return ESP_ERR_NO_MEM;
                }
            }

            /* Iterate partial and merge keys */
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, partial) {
                if (item->string) {
                    /* Delete existing key */
                    cJSON_DeleteItemFromObject(s_device_states[idx], item->string);

                    /* Duplicate and add new value */
                    cJSON *dup = cJSON_Duplicate(item, true);
                    if (dup) {
                        cJSON_AddItemToObject(s_device_states[idx], item->string, dup);
                    }
                }
            }

            ret = ESP_OK;
            ESP_LOGD(TAG, "Merged state for device 0x%016llx", (unsigned long long)id);
        }
    }

    mutex_release();
    return ret;
}

/* ============================================================================
 * Public API - Iteration
 * ============================================================================ */

/**
 * @brief Iterate over all devices with callback
 *
 * WARNING: Callback is invoked with registry mutex held.
 * Callbacks MUST NOT block on external I/O (MQTT, NVS, etc.)
 * or call functions that acquire other mutexes (deadlock risk).
 * For I/O-bound operations, use device_registry_collect_ids() instead.
 */
void device_registry_iterate(device_iterator_fn fn, void *ctx)
{
    if (!s_initialized || !fn) {
        return;
    }

    if (!mutex_acquire()) {
        return;
    }

    for (size_t i = 0; i < DEVICE_REGISTRY_MAX_DEVICES; i++) {
        if (s_devices[i].in_use) {
            if (!fn(&s_devices[i], ctx)) {
                break;  /* Callback requested stop */
            }
        }
    }

    mutex_release();
}

device_id_t *device_registry_snapshot_ids(size_t *out_count)
{
    if (out_count == NULL) {
        return NULL;
    }
    *out_count = 0;

    if (!s_initialized) {
        return NULL;
    }

    device_id_t *ids = (device_id_t *)mem_alloc(
        sizeof(device_id_t) * DEVICE_REGISTRY_MAX_DEVICES, MEM_CAP_PSRAM);
    if (ids == NULL) {
        ESP_LOGE(TAG, "Failed to allocate device ID snapshot");
        return NULL;
    }

    size_t n = 0;
    if (device_registry_collect_ids(ids, DEVICE_REGISTRY_MAX_DEVICES, &n) != ESP_OK || n == 0) {
        mem_ng_free(ids);
        return NULL;
    }

    *out_count = n;
    return ids;
}

void device_registry_release_ids(device_id_t *ids)
{
    if (ids != NULL) {
        mem_ng_free(ids);
    }
}

esp_err_t device_registry_collect_ids(device_id_t *ids, size_t max_ids, size_t *count)
{
    if (!ids || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!mutex_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    size_t collected = 0;

    for (size_t i = 0; i < DEVICE_REGISTRY_MAX_DEVICES && collected < max_ids; i++) {
        if (s_devices[i].in_use) {
            ids[collected++] = s_devices[i].id;
        }
    }

    mutex_release();

    *count = collected;
    return ESP_OK;
}

size_t device_registry_iterate_zigbee(device_iterator_fn fn, void *ctx)
{
    if (!s_initialized || !fn) {
        return 0;
    }

    if (!mutex_acquire()) {
        return 0;
    }

    size_t count = 0;

    for (size_t i = 0; i < DEVICE_REGISTRY_MAX_DEVICES; i++) {
        if (s_devices[i].in_use && s_devices[i].protocol == DEV_PROTOCOL_ZIGBEE) {
            count++;
            if (!fn(&s_devices[i], ctx)) {
                break;  /* Callback requested stop */
            }
        }
    }

    mutex_release();
    return count;
}

size_t device_registry_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    /* No mutex needed for atomic read */
    return s_device_count;
}

device_t *device_registry_get_by_index(size_t index)
{
    if (!s_initialized || s_devices == NULL) {
        return NULL;
    }

    if (!mutex_acquire()) {
        return NULL;
    }

    device_t *result = NULL;
    size_t current_idx = 0;

    /* Iterate through device array finding in_use slots */
    for (size_t i = 0; i < DEVICE_REGISTRY_MAX_DEVICES; i++) {
        if (s_devices[i].in_use) {
            if (current_idx == index) {
                result = &s_devices[i];
                break;
            }
            current_idx++;
        }
    }

    mutex_release();
    return result;
}

/* ============================================================================
 * Public API - Availability Updates
 * ============================================================================ */

esp_err_t device_registry_set_availability(device_id_t id, device_availability_t avail)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!mutex_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            device_availability_t old_avail = s_devices[idx].availability;
            s_devices[idx].availability = avail;
            ret = ESP_OK;

            if (old_avail != avail) {
                ESP_LOGD(TAG, "Device 0x%016llx availability: %s -> %s",
                         (unsigned long long)id,
                         device_availability_to_str(old_avail),
                         device_availability_to_str(avail));
            }
        }
    }

    mutex_release();
    return ret;
}

void device_registry_update_last_seen(device_id_t id)
{
    if (!s_initialized || id == 0) {
        return;
    }

    if (!mutex_acquire()) {
        return;
    }

    uint32_t hash_slot;
    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            s_devices[idx].last_seen = time(NULL);
        }
    }

    mutex_release();
}

/* ============================================================================
 * Public API - Statistics
 * ============================================================================ */

void device_registry_get_stats(device_registry_stats_t *stats)
{
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(device_registry_stats_t));

    if (!s_initialized) {
        stats->max_devices = DEVICE_REGISTRY_MAX_DEVICES;
        return;
    }

    if (!mutex_acquire()) {
        return;
    }

    stats->max_devices = DEVICE_REGISTRY_MAX_DEVICES;
    stats->device_count = s_device_count;
    stats->memory_used = s_memory_used;

    /* Count by protocol and status */
    for (size_t i = 0; i < DEVICE_REGISTRY_MAX_DEVICES; i++) {
        if (s_devices[i].in_use) {
            switch (s_devices[i].protocol) {
                case DEV_PROTOCOL_ZIGBEE:
                    stats->zigbee_count++;
                    break;
                case DEV_PROTOCOL_BLE:
                    stats->ble_count++;
                    break;
                default:
                    break;
            }

            if (s_devices[i].availability == DEV_AVAIL_ONLINE) {
                stats->online_count++;
            }

            /* Estimate state JSON memory (rough approximation) */
            if (s_device_states[i]) {
                char *json_str = cJSON_PrintUnformatted(s_device_states[i]);
                if (json_str) {
                    stats->memory_used += strlen(json_str) + 1;
                    cJSON_free(json_str);
                }
            }
        }
    }

    mutex_release();
}

void device_registry_print_stats(void)
{
    device_registry_stats_t stats;
    device_registry_get_stats(&stats);

    ESP_LOGI(TAG, "=== Device Registry Stats ===");
    ESP_LOGI(TAG, "  Devices: %zu / %zu", stats.device_count, stats.max_devices);
    ESP_LOGI(TAG, "  Zigbee:  %zu", stats.zigbee_count);
    ESP_LOGI(TAG, "  BLE:     %zu", stats.ble_count);
    ESP_LOGI(TAG, "  Online:  %zu", stats.online_count);
    ESP_LOGI(TAG, "  Memory:  %zu bytes", stats.memory_used);
}

/* ============================================================================
 * Public API - Converter Management
 * ============================================================================ */

esp_err_t device_registry_set_converter(device_id_t id, const void *converter)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!mutex_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            /* Verify this is a Zigbee device */
            if (s_devices[idx].protocol != DEV_PROTOCOL_ZIGBEE) {
                ESP_LOGW(TAG, "Cannot set converter on non-Zigbee device 0x%016llx",
                         (unsigned long long)id);
                ret = ESP_ERR_INVALID_ARG;
            } else {
                s_devices[idx].proto.zigbee.converter = converter;
                ret = ESP_OK;
                ESP_LOGD(TAG, "Set converter for device 0x%016llx", (unsigned long long)id);
            }
        }
    }

    mutex_release();
    return ret;
}

const void *device_registry_get_converter(device_id_t id)
{
    if (!s_initialized || id == 0) {
        return NULL;
    }

    if (!mutex_acquire()) {
        return NULL;
    }

    const void *result = NULL;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            if (s_devices[idx].protocol == DEV_PROTOCOL_ZIGBEE) {
                result = s_devices[idx].proto.zigbee.converter;
            }
        }
    }

    mutex_release();
    return result;
}

/* ============================================================================
 * Public API - Device Attribute Setters
 * ============================================================================ */

esp_err_t device_registry_set_friendly_name(device_id_t id, const char *name)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (id == 0 || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!mutex_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            strncpy(s_devices[idx].friendly_name, name,
                    sizeof(s_devices[idx].friendly_name) - 1);
            s_devices[idx].friendly_name[sizeof(s_devices[idx].friendly_name) - 1] = '\0';
            ret = ESP_OK;
            ESP_LOGD(TAG, "Set friendly_name for 0x%016llx: %s",
                     (unsigned long long)id, s_devices[idx].friendly_name);
        }
    }

    mutex_release();
    return ret;
}

esp_err_t device_registry_add_cluster(device_id_t id, uint16_t cluster_id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!mutex_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint32_t hash_slot;

    if (hash_find_slot(id, &hash_slot)) {
        uint16_t idx = s_hash_table[hash_slot];

        if (idx < DEVICE_REGISTRY_MAX_DEVICES && s_devices[idx].in_use) {
            /* Verify this is a Zigbee device */
            if (s_devices[idx].protocol != DEV_PROTOCOL_ZIGBEE) {
                ESP_LOGW(TAG, "Cannot add cluster to non-Zigbee device 0x%016llx",
                         (unsigned long long)id);
                ret = ESP_ERR_INVALID_ARG;
            } else {
                device_zigbee_data_t *zb = &s_devices[idx].proto.zigbee;

                /* Check if cluster already exists */
                bool found = false;
                for (uint16_t i = 0; i < zb->cluster_count; i++) {
                    if (zb->clusters[i] == cluster_id) {
                        found = true;
                        break;
                    }
                }

                if (found) {
                    /* Already exists, that's OK */
                    ret = ESP_OK;
                } else if (zb->cluster_count >= DEVICE_MAX_CLUSTERS) {
                    ESP_LOGW(TAG, "Cluster list full for device 0x%016llx",
                             (unsigned long long)id);
                    ret = ESP_ERR_NO_MEM;
                } else {
                    zb->clusters[zb->cluster_count++] = cluster_id;
                    ret = ESP_OK;
                    ESP_LOGD(TAG, "Added cluster 0x%04X to device 0x%016llx",
                             cluster_id, (unsigned long long)id);
                }
            }
        }
    }

    mutex_release();
    return ret;
}

esp_err_t device_registry_set_online(device_id_t id, bool online)
{
    return device_registry_set_availability(id,
        online ? DEV_AVAIL_ONLINE : DEV_AVAIL_OFFLINE);
}
