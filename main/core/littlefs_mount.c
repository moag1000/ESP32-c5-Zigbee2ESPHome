/**
 * @file littlefs_mount.c
 * @brief Shared LittleFS Mount Utility Implementation
 *
 * Thread-safe reference-counted LittleFS mount, shared by
 * state persistence, Zigbee backup, and OTA subsystems.
 */
#include "littlefs_mount.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include <string.h>

static const char *TAG = "LITTLEFS";
static int s_ref_count = 0;
static bool s_mounted = false;

/** @brief Mutex protecting s_ref_count and s_mounted against concurrent access */
static SemaphoreHandle_t s_mount_mutex = NULL;

/** @brief Spinlock protecting lazy mutex initialization */
static portMUX_TYPE s_init_spinlock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Ensure mount mutex exists (lazy init, safe for single-core boot)
 */
static void ensure_mutex(void)
{
    if (s_mount_mutex != NULL) {
        return;  /* Fast path — already initialized */
    }

    /* Create mutex outside critical section (heap allocation needs interrupts) */
    SemaphoreHandle_t new_mutex = xSemaphoreCreateMutex();
    configASSERT(new_mutex != NULL);

    portENTER_CRITICAL(&s_init_spinlock);
    if (s_mount_mutex == NULL) {
        s_mount_mutex = new_mutex;
    } else {
        /* Another task won the race — discard our mutex */
        portEXIT_CRITICAL(&s_init_spinlock);
        vSemaphoreDelete(new_mutex);
        return;
    }
    portEXIT_CRITICAL(&s_init_spinlock);
}

esp_err_t littlefs_mount(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mount_mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_mounted) {
        s_ref_count++;
        ESP_LOGD(TAG, "LittleFS already mounted (ref_count=%d)", s_ref_count);
        xSemaphoreGive(s_mount_mutex);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting LittleFS (partition=%s, mount=%s)...",
             LITTLEFS_PARTITION_LABEL, LITTLEFS_MOUNT_POINT);

    esp_vfs_littlefs_conf_t conf = {
        .base_path = LITTLEFS_MOUNT_POINT,
        .partition_label = LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
        .grow_on_mount = true,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_mount_mutex);
        return ret;
    }

    s_mounted = true;
    s_ref_count = 1;

    xSemaphoreGive(s_mount_mutex);

    size_t total = 0, used = 0;
    if (esp_littlefs_info(LITTLEFS_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted: %zu/%zu bytes used (%.1f%%)",
                 used, total, total > 0 ? (float)used * 100.0f / (float)total : 0.0f);
    }

    return ESP_OK;
}

esp_err_t littlefs_unmount(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mount_mutex, GW_TIMEOUT_LONG_TICKS);

    if (!s_mounted) {
        xSemaphoreGive(s_mount_mutex);
        return ESP_OK;
    }

    s_ref_count--;
    if (s_ref_count > 0) {
        ESP_LOGD(TAG, "LittleFS ref_count decremented to %d", s_ref_count);
        xSemaphoreGive(s_mount_mutex);
        return ESP_OK;
    }
    if (s_ref_count < 0) {
        s_ref_count = 0;
        ESP_LOGW(TAG, "LittleFS ref_count underflow corrected");
    }

    esp_err_t ret = esp_vfs_littlefs_unregister(LITTLEFS_PARTITION_LABEL);
    if (ret == ESP_OK) {
        s_mounted = false;
        s_ref_count = 0;
        ESP_LOGI(TAG, "LittleFS unmounted");
    } else {
        /* Restore ref_count on failure to keep state consistent */
        s_ref_count = 1;
        ESP_LOGE(TAG, "Failed to unmount LittleFS: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(s_mount_mutex);
    return ret;
}

bool littlefs_is_mounted(void)
{
    if (s_mount_mutex == NULL) {
        return s_mounted;
    }
    xSemaphoreTake(s_mount_mutex, GW_TIMEOUT_LONG_TICKS);
    bool mounted = s_mounted;
    xSemaphoreGive(s_mount_mutex);
    return mounted;
}

esp_err_t littlefs_get_info(size_t *total, size_t *used)
{
    if (total == NULL || used == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mount_mutex != NULL) {
        xSemaphoreTake(s_mount_mutex, GW_TIMEOUT_LONG_TICKS);
    }
    if (!s_mounted) {
        if (s_mount_mutex != NULL) {
            xSemaphoreGive(s_mount_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = esp_littlefs_info(LITTLEFS_PARTITION_LABEL, total, used);
    if (s_mount_mutex != NULL) {
        xSemaphoreGive(s_mount_mutex);
    }
    return ret;
}
