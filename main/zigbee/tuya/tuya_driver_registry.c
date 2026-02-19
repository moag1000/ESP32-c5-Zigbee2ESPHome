/**
 * @file tuya_driver_registry.c
 * @brief Tuya Device Driver Registry Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "tuya_driver_registry.h"
#include "core/device/device_registry.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include <string.h>

static const char *TAG = "TUYA_REGISTRY";

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Registered drivers */
static const tuya_device_driver_t *s_drivers[TUYA_MAX_DRIVERS];
static size_t s_driver_count = 0;

/** @brief Device-to-driver binding cache */
typedef struct {
    uint16_t short_addr;
    const tuya_device_driver_t *driver;
} tuya_device_binding_t;

static tuya_device_binding_t s_bindings[TUYA_MAX_DEVICE_BINDINGS];
static size_t s_binding_count = 0;

/** @brief Mutex for thread-safe access */
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Initialization flag */
static bool s_initialized = false;

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t tuya_driver_registry_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create registry mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(s_drivers, 0, sizeof(s_drivers));
    s_driver_count = 0;
    memset(s_bindings, 0, sizeof(s_bindings));
    s_binding_count = 0;

    s_initialized = true;
    ESP_LOGI(TAG, "Tuya driver registry initialized (max %d drivers, %d bindings)",
             TUYA_MAX_DRIVERS, TUYA_MAX_DEVICE_BINDINGS);
    return ESP_OK;
}

esp_err_t tuya_driver_register(const tuya_device_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_driver_count >= TUYA_MAX_DRIVERS) {
        ESP_LOGE(TAG, "Driver registry full (%d/%d)", (int)s_driver_count, TUYA_MAX_DRIVERS);
        return ESP_ERR_NO_MEM;
    }

    s_drivers[s_driver_count++] = driver;
    ESP_LOGI(TAG, "Registered Tuya driver: '%s' (%d/%d)",
             driver->name, (int)s_driver_count, TUYA_MAX_DRIVERS);
    return ESP_OK;
}

const tuya_device_driver_t *tuya_driver_find(const char *manufacturer, const char *model)
{
    for (size_t i = 0; i < s_driver_count; i++) {
        if (s_drivers[i]->match != NULL &&
            s_drivers[i]->match(manufacturer, model)) {
            return s_drivers[i];
        }
    }
    return NULL;
}

const tuya_device_driver_t *tuya_driver_get(uint16_t short_addr)
{
    if (!s_initialized) {
        return NULL;
    }
    const tuya_device_driver_t *result = NULL;
    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].short_addr == short_addr) {
            result = s_bindings[i].driver;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t tuya_driver_bind(uint16_t short_addr, const tuya_device_driver_t *driver)
{
    if (driver == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Check if already bound — update driver */
    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].short_addr == short_addr) {
            s_bindings[i].driver = driver;
            xSemaphoreGive(s_mutex);
            ESP_LOGD(TAG, "Updated binding: 0x%04X -> '%s'", short_addr, driver->name);
            return ESP_OK;
        }
    }

    /* New binding */
    if (s_binding_count >= TUYA_MAX_DEVICE_BINDINGS) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Binding table full (%d/%d)", (int)s_binding_count, TUYA_MAX_DEVICE_BINDINGS);
        return ESP_ERR_NO_MEM;
    }

    s_bindings[s_binding_count].short_addr = short_addr;
    s_bindings[s_binding_count].driver = driver;
    s_binding_count++;

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Bound device 0x%04X to driver '%s' (%d/%d)",
             short_addr, driver->name, (int)s_binding_count, TUYA_MAX_DEVICE_BINDINGS);

    /* Persist binding to NVS so it survives reboot */
    tuya_driver_persist_binding(short_addr, driver);

    return ESP_OK;
}

void tuya_driver_unbind(uint16_t short_addr)
{
    if (!s_initialized) {
        return;
    }
    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    for (size_t i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].short_addr == short_addr) {
            ESP_LOGI(TAG, "Unbound device 0x%04X from driver '%s'",
                     short_addr, s_bindings[i].driver->name);

            /* Swap with last entry */
            if (i < s_binding_count - 1) {
                s_bindings[i] = s_bindings[s_binding_count - 1];
            }
            s_binding_count--;
            xSemaphoreGive(s_mutex);
            return;
        }
    }
    xSemaphoreGive(s_mutex);
}

/* ============================================================================
 * NVS Persistence for Tuya Driver Bindings
 *
 * Stores {ieee_addr, driver_name} pairs so bindings survive reboot.
 * On device join/announce, tuya_driver_restore_for_device() looks up the
 * persisted driver name and re-binds using the current short_addr.
 * ============================================================================ */

#define TUYA_NVS_NAMESPACE  "tuya_drv"
#define TUYA_NVS_KEY        "binds"
#define TUYA_DRIVER_NAME_MAX 16

typedef struct {
    uint64_t ieee_addr;
    char driver_name[TUYA_DRIVER_NAME_MAX];
} tuya_nvs_entry_t;

/**
 * @brief Find a registered driver by name
 */
static const tuya_device_driver_t *find_driver_by_name(const char *name)
{
    for (size_t i = 0; i < s_driver_count; i++) {
        if (s_drivers[i]->name != NULL && strcmp(s_drivers[i]->name, name) == 0) {
            return s_drivers[i];
        }
    }
    return NULL;
}

void tuya_driver_persist_binding(uint16_t short_addr, const tuya_device_driver_t *driver)
{
    /* Look up IEEE address from device registry */
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL || driver == NULL || driver->name == NULL) {
        return;
    }
    uint64_t ieee = dev->id;

    /* Read existing entries */
    nvs_handle_t nvs;
    if (nvs_open(TUYA_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    tuya_nvs_entry_t entries[TUYA_MAX_DEVICE_BINDINGS];
    size_t blob_size = sizeof(entries);
    size_t count = 0;

    if (nvs_get_blob(nvs, TUYA_NVS_KEY, entries, &blob_size) == ESP_OK) {
        count = blob_size / sizeof(tuya_nvs_entry_t);
    }

    /* Check if this IEEE already exists — update driver name */
    for (size_t i = 0; i < count; i++) {
        if (entries[i].ieee_addr == ieee) {
            if (strncmp(entries[i].driver_name, driver->name, TUYA_DRIVER_NAME_MAX) == 0) {
                nvs_close(nvs);
                return; /* Already persisted with same driver */
            }
            strncpy(entries[i].driver_name, driver->name, TUYA_DRIVER_NAME_MAX - 1);
            entries[i].driver_name[TUYA_DRIVER_NAME_MAX - 1] = '\0';
            nvs_set_blob(nvs, TUYA_NVS_KEY, entries, count * sizeof(tuya_nvs_entry_t));
            nvs_commit(nvs);
            nvs_close(nvs);
            ESP_LOGI(TAG, "Updated persisted binding: 0x%016llx -> %s",
                     (unsigned long long)ieee, driver->name);
            return;
        }
    }

    /* Add new entry */
    if (count >= TUYA_MAX_DEVICE_BINDINGS) {
        nvs_close(nvs);
        ESP_LOGW(TAG, "NVS binding table full, cannot persist");
        return;
    }

    entries[count].ieee_addr = ieee;
    strncpy(entries[count].driver_name, driver->name, TUYA_DRIVER_NAME_MAX - 1);
    entries[count].driver_name[TUYA_DRIVER_NAME_MAX - 1] = '\0';
    count++;

    nvs_set_blob(nvs, TUYA_NVS_KEY, entries, count * sizeof(tuya_nvs_entry_t));
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Persisted binding: 0x%016llx -> %s",
             (unsigned long long)ieee, driver->name);
}

esp_err_t tuya_driver_restore_for_device(uint64_t ieee_addr, uint16_t short_addr)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Already bound? */
    if (tuya_driver_get(short_addr) != NULL) {
        return ESP_OK;
    }

    nvs_handle_t nvs;
    if (nvs_open(TUYA_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    tuya_nvs_entry_t entries[TUYA_MAX_DEVICE_BINDINGS];
    size_t blob_size = sizeof(entries);

    esp_err_t ret = nvs_get_blob(nvs, TUYA_NVS_KEY, entries, &blob_size);
    nvs_close(nvs);

    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t count = blob_size / sizeof(tuya_nvs_entry_t);

    for (size_t i = 0; i < count; i++) {
        if (entries[i].ieee_addr == ieee_addr) {
            const tuya_device_driver_t *drv = find_driver_by_name(entries[i].driver_name);
            if (drv != NULL) {
                tuya_driver_bind(short_addr, drv);
                if (drv->init_device) {
                    drv->init_device(short_addr);
                }
                ESP_LOGI(TAG, "Restored persisted binding: 0x%04X -> %s",
                         short_addr, drv->name);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Persisted driver '%s' not registered", entries[i].driver_name);
            return ESP_ERR_NOT_FOUND;
        }
    }

    return ESP_ERR_NOT_FOUND;
}
