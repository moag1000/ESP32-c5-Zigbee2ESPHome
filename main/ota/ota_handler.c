/**
 * @file ota_handler.c
 * @brief OTA Handler Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ota_handler.h"
#include "gateway_defaults.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "utils/freertos_helpers.h"  /* For PSRAM task creation */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "../utils/version.h"
#include "sdkconfig.h"

/* LED status integration */
#if CONFIG_GW_LED_ENABLED
#include "core/led_status_manager.h"
#endif

static const char *TAG = "OTA";

/* OTA configuration */
static char s_firmware_url[256] = {0};
static bool s_ota_initialized = false;
static bool s_auto_check_enabled = false;
static uint32_t s_auto_check_interval_hours = 24;

/* OTA state */
static ota_info_t s_ota_info;
static SemaphoreHandle_t s_ota_mutex = NULL;
static psram_task_handle_t s_ota_psram_task = {0};
static bool s_ota_in_progress = false;

/* Progress callback */
static ota_progress_callback_t s_progress_callback = NULL;

/* State and error string mappings */
static const char* ota_state_strings[] = {
    "idle", "checking", "available", "downloading",
    "verifying", "updating", "success", "failed"
};

static const char* ota_error_strings[] = {
    "none", "no_connection", "http_failed", "download_failed",
    "verify_failed", "write_failed", "no_update", "invalid_image", "out_of_memory"
};

/**
 * @brief Update OTA state
 */
static void update_ota_state(ota_state_t state, ota_error_t error)
{
    if (xSemaphoreTake(s_ota_mutex, GW_TIMEOUT_LONG_TICKS) == pdTRUE) {
        s_ota_info.state = state;
        s_ota_info.error = error;

        /* Trigger callback if registered */
        if (s_progress_callback != NULL) {
            s_progress_callback(s_ota_info.download_progress, state);
        }

        xSemaphoreGive(s_ota_mutex);
    }

#if CONFIG_GW_LED_ENABLED
    /* Update LED status based on OTA state */
    bool ota_active = (state >= OTA_STATE_CHECKING && state <= OTA_STATE_UPDATING);
    led_status_manager_set_condition(LED_COND_OTA_ACTIVE, ota_active);
#endif
}

/**
 * @brief HTTP event handler for OTA
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (xSemaphoreTake(s_ota_mutex, 10 / portTICK_PERIOD_MS) == pdTRUE) {
                s_ota_info.downloaded_size += evt->data_len;
                if (s_ota_info.total_size > 0) {
                    s_ota_info.download_progress =
                        (s_ota_info.downloaded_size * 100) / s_ota_info.total_size;
                }
                xSemaphoreGive(s_ota_mutex);
            }
            break;

        case HTTP_EVENT_ON_HEADER:
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                char *endptr = NULL;
                errno = 0;
                long val = strtol(evt->header_value, &endptr, 10);
                if (endptr == evt->header_value || errno == ERANGE || val <= 0) {
                    ESP_LOGW(TAG, "Invalid Content-Length header: '%s'", evt->header_value);
                } else if (xSemaphoreTake(s_ota_mutex, GW_TIMEOUT_LONG_TICKS) == pdTRUE) {
                    s_ota_info.total_size = (uint32_t)val;
                    xSemaphoreGive(s_ota_mutex);
                }
            }
            break;

        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief Perform OTA update
 */
static esp_err_t perform_ota_update(void)
{
    ESP_LOGI(TAG, "Starting OTA update from: %s", s_firmware_url);

    update_ota_state(OTA_STATE_DOWNLOADING, OTA_ERROR_NONE);

    /* Reset progress */
    if (xSemaphoreTake(s_ota_mutex, GW_TIMEOUT_LONG_TICKS) == pdTRUE) {
        s_ota_info.downloaded_size = 0;
        s_ota_info.total_size = 0;
        s_ota_info.download_progress = 0;
        xSemaphoreGive(s_ota_mutex);
    }

    /* Configure OTA */
    esp_http_client_config_t http_config = {
        .url = s_firmware_url,
        .timeout_ms = GW_OTA_CHECK_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    /* Start OTA */
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        update_ota_state(OTA_STATE_FAILED, OTA_ERROR_HTTP_FAILED);
        return ret;
    }

    /* Get image header */
    esp_app_desc_t new_app_info;
    ret = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get image descriptor: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);
        update_ota_state(OTA_STATE_FAILED, OTA_ERROR_INVALID_IMAGE);
        return ret;
    }

    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

    /* Store available version */
    if (xSemaphoreTake(s_ota_mutex, GW_TIMEOUT_LONG_TICKS) == pdTRUE) {
        strncpy(s_ota_info.available_version, new_app_info.version,
                sizeof(s_ota_info.available_version) - 1);
        xSemaphoreGive(s_ota_mutex);
    }

    update_ota_state(OTA_STATE_VERIFYING, OTA_ERROR_NONE);

    /* Download and verify */
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        /* Update progress - acquire mutex first to avoid TOCTOU */
        if (xSemaphoreTake(s_ota_mutex, 0) == pdTRUE) {
            if (s_progress_callback != NULL) {
                s_progress_callback(s_ota_info.download_progress, OTA_STATE_DOWNLOADING);
            }
            xSemaphoreGive(s_ota_mutex);
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);
        update_ota_state(OTA_STATE_FAILED, OTA_ERROR_DOWNLOAD_FAILED);
        return ret;
    }

    /* Finish OTA */
    update_ota_state(OTA_STATE_UPDATING, OTA_ERROR_NONE);

    ret = esp_https_ota_finish(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
        update_ota_state(OTA_STATE_FAILED, OTA_ERROR_WRITE_FAILED);
        return ret;
    }

    update_ota_state(OTA_STATE_SUCCESS, OTA_ERROR_NONE);
    ESP_LOGI(TAG, "OTA update successful! Rebooting in %d seconds...", GW_OTA_RESTART_DELAY_MS / 1000);

    vTaskDelay(pdMS_TO_TICKS(GW_OTA_RESTART_DELAY_MS));
    esp_restart();

    return ESP_OK;
}

/**
 * @brief Initialize OTA handler
 */
esp_err_t ota_handler_init(const char *firmware_url)
{
    if (firmware_url == NULL || strlen(firmware_url) == 0) {
        ESP_LOGE(TAG, "Invalid firmware URL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing OTA handler...");

    /* Create mutex */
    if (s_ota_mutex == NULL) {
        s_ota_mutex = xSemaphoreCreateMutex();
        if (s_ota_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create OTA mutex");
            return ESP_FAIL;
        }
    }

    /* Store firmware URL */
    strncpy(s_firmware_url, firmware_url, sizeof(s_firmware_url) - 1);

    /* Initialize OTA info */
    memset(&s_ota_info, 0, sizeof(ota_info_t));
    strncpy(s_ota_info.current_version, version_get_number(),
            sizeof(s_ota_info.current_version) - 1);
    s_ota_info.state = OTA_STATE_IDLE;
    s_ota_info.error = OTA_ERROR_NONE;

    s_ota_initialized = true;

    ESP_LOGI(TAG, "OTA handler initialized (URL: %s)", s_firmware_url);
    ESP_LOGI(TAG, "Current version: %s", s_ota_info.current_version);

    /* Check if previous OTA was successful */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "OTA image pending verification - marking as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    return ESP_OK;
}

/**
 * @brief Check for available firmware updates
 */
esp_err_t ota_handler_check_for_update(void)
{
    if (!s_ota_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Checking for firmware updates...");
    update_ota_state(OTA_STATE_CHECKING, OTA_ERROR_NONE);

    /* NOTE: This is a simplified implementation.
     * In production, you would query a version manifest file first,
     * then compare versions before downloading the full firmware.
     * For now, we just indicate that checking is complete.
     */

    /* Simulate version check (placeholder) */
    update_ota_state(OTA_STATE_IDLE, OTA_ERROR_NO_UPDATE);
    ESP_LOGI(TAG, "No update check mechanism implemented yet");

    return ESP_OK;
}

/**
 * @brief Start firmware update
 */
esp_err_t ota_handler_start_update(void)
{
    if (!s_ota_initialized) {
        ESP_LOGE(TAG, "OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    s_ota_in_progress = true;

    /* Perform OTA update (blocking) */
    esp_err_t ret = perform_ota_update();

    s_ota_in_progress = false;
    return ret;
}

/**
 * @brief Get current OTA information
 */
ota_info_t ota_handler_get_info(void)
{
    ota_info_t info;

    if (xSemaphoreTake(s_ota_mutex, GW_TIMEOUT_LONG_TICKS) == pdTRUE) {
        memcpy(&info, &s_ota_info, sizeof(ota_info_t));
        xSemaphoreGive(s_ota_mutex);
    } else {
        memset(&info, 0, sizeof(ota_info_t));
    }

    return info;
}

/**
 * @brief Get OTA state as string
 */
const char* ota_handler_get_state_string(ota_state_t state)
{
    if (state >= OTA_STATE_IDLE && state <= OTA_STATE_FAILED) {
        return ota_state_strings[state];
    }
    return "unknown";
}

/**
 * @brief Get OTA error as string
 */
const char* ota_handler_get_error_string(ota_error_t error)
{
    if (error >= OTA_ERROR_NONE && error <= OTA_ERROR_OUT_OF_MEMORY) {
        return ota_error_strings[error];
    }
    return "unknown";
}

/**
 * @brief Register progress callback
 */
esp_err_t ota_handler_register_callback(ota_progress_callback_t callback)
{
    /* Thread-safe callback registration */
    if (xSemaphoreTake(s_ota_mutex, GW_TIMEOUT_LONG_TICKS) == pdTRUE) {
        s_progress_callback = callback;
        xSemaphoreGive(s_ota_mutex);
    }
    ESP_LOGI(TAG, "Progress callback registered");
    return ESP_OK;
}

/**
 * @brief Mark current app as valid
 */
esp_err_t ota_handler_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "OTA image marked as valid");
                return ESP_OK;
            } else {
                ESP_LOGE(TAG, "Failed to mark OTA image as valid: %s", esp_err_to_name(ret));
                return ret;
            }
        }
    }

    ESP_LOGD(TAG, "No pending OTA verification");
    return ESP_OK;
}

/**
 * @brief Check if rollback is possible
 */
bool ota_handler_can_rollback(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        return (ota_state == ESP_OTA_IMG_VALID || ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    }

    return false;
}

/**
 * @brief Trigger rollback to previous firmware
 */
esp_err_t ota_handler_rollback(void)
{
    ESP_LOGW(TAG, "Initiating rollback to previous firmware...");

    esp_err_t ret = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Get OTA info as JSON
 */
esp_err_t ota_handler_get_json(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_info_t info = ota_handler_get_info();

    int written = snprintf(buffer, buffer_size,
        "{"
        "\"current_version\":\"%s\","
        "\"available_version\":\"%s\","
        "\"state\":\"%s\","
        "\"error\":\"%s\","
        "\"progress\":%lu,"
        "\"update_available\":%s"
        "}",
        info.current_version,
        info.available_version,
        ota_handler_get_state_string(info.state),
        ota_handler_get_error_string(info.error),
        info.download_progress,
        info.update_available ? "true" : "false"
    );

    if (written < 0 || (size_t)written >= buffer_size) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Get OTA partition information
 */
esp_err_t ota_handler_get_partition_info(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();

    int written = snprintf(buffer, buffer_size,
        "Running: %s (offset=0x%lx, size=%lu)\n"
        "Boot: %s (offset=0x%lx, size=%lu)",
        running->label, running->address, running->size,
        boot->label, boot->address, boot->size
    );

    if (written < 0 || (size_t)written >= buffer_size) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Cancel ongoing OTA update
 */
esp_err_t ota_handler_cancel(void)
{
    if (!s_ota_in_progress) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "OTA cancel not fully implemented");
    /* Note: esp_https_ota doesn't provide easy cancellation.
     * Would need to implement custom OTA logic for proper cancellation.
     */
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Set firmware URL
 */
esp_err_t ota_handler_set_url(const char *firmware_url)
{
    if (firmware_url == NULL || strlen(firmware_url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_firmware_url, firmware_url, sizeof(s_firmware_url) - 1);
    ESP_LOGI(TAG, "Firmware URL updated: %s", s_firmware_url);
    return ESP_OK;
}

/**
 * @brief OTA task function
 */
void ota_handler_task(void *pvParameters)
{
    ESP_LOGI(TAG, "OTA task started");

    while (s_auto_check_enabled) {
        /* Wait for check interval */
        vTaskDelay(pdMS_TO_TICKS(s_auto_check_interval_hours * 3600 * 1000));

        /* Check for updates */
        ota_handler_check_for_update();
    }

    ESP_LOGI(TAG, "OTA task stopped");
    /* Mark task as self-deleted - resources freed by set_auto_check */
    psram_task_mark_deleted(&s_ota_psram_task);
    vTaskDelete(NULL);
}

/**
 * @brief Enable/disable automatic OTA checks
 */
esp_err_t ota_handler_set_auto_check(bool enable, uint32_t interval_hours)
{
    s_auto_check_enabled = enable;
    s_auto_check_interval_hours = interval_hours;

    if (enable && !psram_task_is_valid(&s_ota_psram_task)) {
        /* Free any leftover PSRAM resources from previous task */
        if (s_ota_psram_task.stack != NULL) {
            psram_task_delete(&s_ota_psram_task);
        }

        /* Start OTA task with PSRAM stack (saves ~4KB internal RAM) */
        esp_err_t result = psram_task_create(
            ota_handler_task,
            "ota_handler",
            4096,
            NULL,
            3,
            &s_ota_psram_task
        );

        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create OTA task: %s", esp_err_to_name(result));
            return result;
        }

        ESP_LOGI(TAG, "Automatic OTA checks enabled (interval: %lu hours, PSRAM stack)",
                 interval_hours);
    } else if (!enable && psram_task_is_valid(&s_ota_psram_task)) {
        /* Stop OTA task - task will exit on next iteration */
        ESP_LOGI(TAG, "Automatic OTA checks disabled");
    }

    return ESP_OK;
}
