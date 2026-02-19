/**
 * @file http_ota.c
 * @brief HTTP OTA Update Module Implementation
 *
 * Implements HTTP/HTTPS firmware update with lifecycle integration,
 * progress reporting, version validation, and automatic rollback support.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "http_ota.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "freertos/event_groups.h"
#include "core/lifecycle_manager.h"
#include "core/gateway_defaults.h"
#include "utils/version.h"
#include "utils/freertos_helpers.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static const char *TAG = "HTTP_OTA";

/* Event group bits */
#define OTA_ABORT_BIT       BIT0

/* OTA task configuration */
#define HTTP_OTA_TASK_STACK_SIZE    GW_TASK_STACK_OTA
#define HTTP_OTA_TASK_PRIORITY      GW_TASK_PRIORITY_MEDIUM
#define HTTP_OTA_RECV_TIMEOUT_MS    GW_OTA_CHECK_TIMEOUT_MS
#define HTTP_OTA_BUFFER_SIZE        GW_OTA_TRANSFER_BUFFER_SIZE

/* State string mappings */
static const char *s_state_strings[] = {
    [HTTP_OTA_STATE_IDLE]        = "idle",
    [HTTP_OTA_STATE_STARTING]    = "starting",
    [HTTP_OTA_STATE_DOWNLOADING] = "downloading",
    [HTTP_OTA_STATE_VERIFYING]   = "verifying",
    [HTTP_OTA_STATE_WRITING]     = "writing",
    [HTTP_OTA_STATE_COMPLETE]    = "complete",
    [HTTP_OTA_STATE_FAILED]      = "failed",
    [HTTP_OTA_STATE_ABORTED]     = "aborted",
};

/* Error string mappings */
static const char *s_error_strings[] = {
    [HTTP_OTA_ERROR_NONE]               = "none",
    [HTTP_OTA_ERROR_INVALID_CONFIG]     = "invalid_config",
    [HTTP_OTA_ERROR_LIFECYCLE_FAILED]   = "lifecycle_failed",
    [HTTP_OTA_ERROR_ALREADY_IN_PROGRESS]= "already_in_progress",
    [HTTP_OTA_ERROR_HTTP_CONNECT]       = "http_connect_failed",
    [HTTP_OTA_ERROR_HTTP_RESPONSE]      = "http_response_invalid",
    [HTTP_OTA_ERROR_DOWNLOAD]           = "download_failed",
    [HTTP_OTA_ERROR_VERIFY]             = "verify_failed",
    [HTTP_OTA_ERROR_WRITE]              = "write_failed",
    [HTTP_OTA_ERROR_VERSION_SAME]       = "version_same",
    [HTTP_OTA_ERROR_VERSION_OLDER]      = "version_older",
    [HTTP_OTA_ERROR_ABORTED]            = "aborted",
    [HTTP_OTA_ERROR_OUT_OF_MEMORY]      = "out_of_memory",
};

/* Module state */
static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;
static EventGroupHandle_t s_event_group = NULL;
static psram_task_handle_t s_ota_task_handle = {0};

/* OTA state (protected by mutex) */
static http_ota_state_t s_state = HTTP_OTA_STATE_IDLE;
static http_ota_error_t s_error = HTTP_OTA_ERROR_NONE;
static size_t s_bytes_received = 0;
static size_t s_total_size = 0;
static char s_new_version[32] = {0};
static lifecycle_phase_t s_previous_phase = LIFECYCLE_PHASE_NORMAL;

/* OTA task context */
typedef struct {
    http_ota_config_t config;
    http_ota_progress_cb_t progress_cb;
    void *user_ctx;
} ota_task_ctx_t;

static ota_task_ctx_t s_task_ctx;

/* Forward declarations */
static void http_ota_task(void *param);
static esp_err_t perform_ota_update(const http_ota_config_t *config);
static void update_state(http_ota_state_t state, http_ota_error_t error);
static void invoke_progress_callback(void);
static int compare_versions(const char *v1, const char *v2);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t http_ota_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing HTTP OTA module");

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create event group for signaling */
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Initialize state */
    s_state = HTTP_OTA_STATE_IDLE;
    s_error = HTTP_OTA_ERROR_NONE;
    s_bytes_received = 0;
    s_total_size = 0;
    memset(s_new_version, 0, sizeof(s_new_version));

    s_initialized = true;

    /* Check for pending OTA verification from previous boot */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "OTA image pending verification - call http_ota_mark_app_valid() after boot test");
        }
    }

    ESP_LOGI(TAG, "HTTP OTA module initialized (current version: %s)", version_get_number());
    return ESP_OK;
}

esp_err_t http_ota_start(const http_ota_config_t *config,
                         http_ota_progress_cb_t progress_cb,
                         void *ctx)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL || strlen(config->url) == 0) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Check if already in progress */
    if (s_state != HTTP_OTA_STATE_IDLE && s_state != HTTP_OTA_STATE_FAILED &&
        s_state != HTTP_OTA_STATE_ABORTED && s_state != HTTP_OTA_STATE_COMPLETE) {
        ESP_LOGW(TAG, "OTA already in progress (state: %s)", http_ota_state_str(s_state));
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Store context for task */
    memcpy(&s_task_ctx.config, config, sizeof(http_ota_config_t));
    s_task_ctx.progress_cb = progress_cb;
    s_task_ctx.user_ctx = ctx;

    /* Reset state */
    s_state = HTTP_OTA_STATE_STARTING;
    s_error = HTTP_OTA_ERROR_NONE;
    s_bytes_received = 0;
    s_total_size = 0;
    memset(s_new_version, 0, sizeof(s_new_version));

    /* Clear abort flag */
    xEventGroupClearBits(s_event_group, OTA_ABORT_BIT);

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Starting OTA update from: %s", config->url);

    /* Create OTA task with PSRAM stack to save internal RAM */
    esp_err_t ret = psram_task_create(
        http_ota_task,
        "http_ota",
        HTTP_OTA_TASK_STACK_SIZE,
        NULL,
        HTTP_OTA_TASK_PRIORITY,
        &s_ota_task_handle
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create OTA task: %s", esp_err_to_name(ret));
        xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
        s_state = HTTP_OTA_STATE_FAILED;
        s_error = HTTP_OTA_ERROR_OUT_OF_MEMORY;
        xSemaphoreGive(s_mutex);
        return ret;
    }

    return ESP_OK;
}

esp_err_t http_ota_abort(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    bool in_progress = (s_state == HTTP_OTA_STATE_STARTING ||
                        s_state == HTTP_OTA_STATE_DOWNLOADING ||
                        s_state == HTTP_OTA_STATE_VERIFYING ||
                        s_state == HTTP_OTA_STATE_WRITING);
    xSemaphoreGive(s_mutex);

    if (!in_progress) {
        ESP_LOGW(TAG, "No OTA in progress to abort");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "Abort requested");
    xEventGroupSetBits(s_event_group, OTA_ABORT_BIT);
    return ESP_OK;
}

bool http_ota_is_in_progress(void)
{
    if (!s_initialized) {
        return false;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    bool in_progress = (s_state == HTTP_OTA_STATE_STARTING ||
                        s_state == HTTP_OTA_STATE_DOWNLOADING ||
                        s_state == HTTP_OTA_STATE_VERIFYING ||
                        s_state == HTTP_OTA_STATE_WRITING);
    xSemaphoreGive(s_mutex);

    return in_progress;
}

esp_err_t http_ota_get_current_version(char *version, size_t len)
{
    if (version == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(version, version_get_number(), len - 1);
    version[len - 1] = '\0';
    return ESP_OK;
}

esp_err_t http_ota_get_new_version(char *version, size_t len)
{
    if (version == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    if (strlen(s_new_version) == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(version, s_new_version, len - 1);
    version[len - 1] = '\0';
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

http_ota_state_t http_ota_get_state(void)
{
    if (!s_initialized) {
        return HTTP_OTA_STATE_IDLE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    http_ota_state_t state = s_state;
    xSemaphoreGive(s_mutex);
    return state;
}

http_ota_error_t http_ota_get_error(void)
{
    if (!s_initialized) {
        return HTTP_OTA_ERROR_NONE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    http_ota_error_t error = s_error;
    xSemaphoreGive(s_mutex);
    return error;
}

const char *http_ota_state_str(http_ota_state_t state)
{
    if (state < sizeof(s_state_strings) / sizeof(s_state_strings[0])) {
        return s_state_strings[state];
    }
    return "unknown";
}

const char *http_ota_error_str(http_ota_error_t error)
{
    if (error < sizeof(s_error_strings) / sizeof(s_error_strings[0])) {
        return s_error_strings[error];
    }
    return "unknown";
}

esp_err_t http_ota_mark_app_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "App marked as valid, rollback cancelled");
                return ESP_OK;
            } else {
                ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(ret));
                return ret;
            }
        }
    }

    ESP_LOGD(TAG, "No pending OTA verification");
    return ESP_OK;
}

bool http_ota_can_rollback(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        /* Can rollback if in pending verify state or if we have a valid previous app */
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            return true;
        }
    }

    /* Check if there's a valid previous partition */
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition != NULL && update_partition != running) {
        esp_app_desc_t app_desc;
        if (esp_ota_get_partition_description(update_partition, &app_desc) == ESP_OK) {
            return true;
        }
    }

    return false;
}

esp_err_t http_ota_rollback(void)
{
    if (!http_ota_can_rollback()) {
        ESP_LOGE(TAG, "Rollback not available");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Initiating rollback to previous firmware...");

    esp_err_t ret = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(ret));
    }
    /* If successful, this function does not return */
    return ret;
}

esp_err_t http_ota_get_json(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    uint32_t progress = 0;
    if (s_total_size > 0) {
        progress = (s_bytes_received * 100) / s_total_size;
    }

    int written = snprintf(buffer, buffer_size,
        "{"
        "\"state\":\"%s\","
        "\"error\":\"%s\","
        "\"progress\":%lu,"
        "\"bytes_received\":%zu,"
        "\"total_size\":%zu,"
        "\"current_version\":\"%s\","
        "\"new_version\":\"%s\","
        "\"can_rollback\":%s"
        "}",
        http_ota_state_str(s_state),
        http_ota_error_str(s_error),
        (unsigned long)progress,
        s_bytes_received,
        s_total_size,
        version_get_number(),
        s_new_version,
        http_ota_can_rollback() ? "true" : "false"
    );

    xSemaphoreGive(s_mutex);

    if (written < 0 || (size_t)written >= buffer_size) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

/**
 * @brief Update OTA state (thread-safe)
 */
static void update_state(http_ota_state_t state, http_ota_error_t error)
{
    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    s_state = state;
    s_error = error;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "State: %s, Error: %s",
             http_ota_state_str(state), http_ota_error_str(error));

    /* Invoke progress callback */
    invoke_progress_callback();
}

/**
 * @brief Invoke progress callback if registered
 */
static void invoke_progress_callback(void)
{
    if (s_task_ctx.progress_cb != NULL) {
        xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
        size_t received = s_bytes_received;
        size_t total = s_total_size;
        xSemaphoreGive(s_mutex);

        s_task_ctx.progress_cb(received, total, s_task_ctx.user_ctx);
    }
}

/**
 * @brief Check if abort was requested
 */
static bool check_abort(void)
{
    EventBits_t bits = xEventGroupGetBits(s_event_group);
    return (bits & OTA_ABORT_BIT) != 0;
}

/**
 * @brief Compare semantic version strings
 *
 * @return <0 if v1 < v2, 0 if equal, >0 if v1 > v2
 */
static int compare_versions(const char *v1, const char *v2)
{
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;

    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);

    if (major1 != major2) return major1 - major2;
    if (minor1 != minor2) return minor1 - minor2;
    return patch1 - patch2;
}

/**
 * @brief HTTP event handler for progress tracking
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                char *endptr = NULL;
                errno = 0;
                long val = strtol(evt->header_value, &endptr, 10);
                if (endptr == evt->header_value || errno == ERANGE || val <= 0) {
                    ESP_LOGW(TAG, "Invalid Content-Length header: '%s'", evt->header_value);
                } else {
                    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
                    s_total_size = (size_t)val;
                    xSemaphoreGive(s_mutex);
                    ESP_LOGI(TAG, "Firmware size: %zu bytes", s_total_size);
                }
            }
            break;

        case HTTP_EVENT_ON_DATA:
            /* Progress is tracked via esp_https_ota_get_image_len_read() */
            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP error event");
            break;

        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief Perform the actual OTA update
 */
static esp_err_t perform_ota_update(const http_ota_config_t *config)
{
    esp_err_t ret = ESP_FAIL;
    esp_https_ota_handle_t ota_handle = NULL;

    ESP_LOGI(TAG, "Connecting to: %s", config->url);

    /* Configure HTTP client */
    esp_http_client_config_t http_config = {
        .url = config->url,
        .timeout_ms = HTTP_OTA_RECV_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .keep_alive_enable = true,
        .buffer_size = HTTP_OTA_BUFFER_SIZE,
        .buffer_size_tx = 1024,
    };

    /* Configure HTTPS certificate */
    if (strlen(config->cert_pem) > 0) {
        http_config.cert_pem = config->cert_pem;
        ESP_LOGI(TAG, "Using provided server certificate");
    } else if (config->skip_cert_verify) {
        http_config.skip_cert_common_name_check = true;
        ESP_LOGW(TAG, "Certificate verification DISABLED (insecure!)");
    }

    /* Configure OTA */
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .bulk_flash_erase = false,  /* Prefer sector erase for reliability */
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        .partial_http_download = false,
#endif
    };

    /* Begin OTA */
    update_state(HTTP_OTA_STATE_DOWNLOADING, HTTP_OTA_ERROR_NONE);

    ret = esp_https_ota_begin(&ota_config, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        update_state(HTTP_OTA_STATE_FAILED, HTTP_OTA_ERROR_HTTP_CONNECT);
        return ret;
    }

    /* Get and validate firmware image header */
    esp_app_desc_t new_app_info;
    ret = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get image descriptor: %s", esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);
        update_state(HTTP_OTA_STATE_FAILED, HTTP_OTA_ERROR_VERIFY);
        return ret;
    }

    /* Store new version */
    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    strncpy(s_new_version, new_app_info.version, sizeof(s_new_version) - 1);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
    ESP_LOGI(TAG, "New firmware project: %s", new_app_info.project_name);

    /* Version validation */
    const char *current_version = version_get_number();
    int version_cmp = compare_versions(new_app_info.version, current_version);

    if (version_cmp == 0) {
        ESP_LOGW(TAG, "New version (%s) is same as current (%s)",
                 new_app_info.version, current_version);
        /* Allow re-flashing same version (useful for recovery) */
    } else if (version_cmp < 0) {
        ESP_LOGW(TAG, "New version (%s) is older than current (%s) - proceeding anyway",
                 new_app_info.version, current_version);
        /* Allow downgrade (useful for rollback scenarios) */
    } else {
        ESP_LOGI(TAG, "Upgrading from %s to %s", current_version, new_app_info.version);
    }

    update_state(HTTP_OTA_STATE_VERIFYING, HTTP_OTA_ERROR_NONE);

    /* Get total image size if not from headers */
    int image_size = esp_https_ota_get_image_size(ota_handle);
    if (image_size > 0) {
        xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
        s_total_size = (size_t)image_size;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Image size from OTA: %d bytes", image_size);
    }

    /* Download and write firmware */
    update_state(HTTP_OTA_STATE_WRITING, HTTP_OTA_ERROR_NONE);

    while (1) {
        /* Check for abort request */
        if (check_abort()) {
            ESP_LOGW(TAG, "OTA aborted by user");
            esp_https_ota_abort(ota_handle);
            update_state(HTTP_OTA_STATE_ABORTED, HTTP_OTA_ERROR_ABORTED);
            return ESP_ERR_INVALID_STATE;
        }

        ret = esp_https_ota_perform(ota_handle);

        if (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            /* Update progress */
            xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
            s_bytes_received = (size_t)esp_https_ota_get_image_len_read(ota_handle);
            xSemaphoreGive(s_mutex);

            /* Invoke progress callback periodically (every ~10%) */
            static size_t last_percent = 0;
            size_t current_percent = 0;
            if (s_total_size > 0) {
                current_percent = (s_bytes_received * 100) / s_total_size;
            }
            if (current_percent >= last_percent + 10) {
                last_percent = current_percent;
                ESP_LOGI(TAG, "Progress: %zu%% (%zu / %zu bytes)",
                         current_percent, s_bytes_received, s_total_size);
                invoke_progress_callback();
            }

            continue;
        }

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(ret));
            esp_https_ota_abort(ota_handle);
            update_state(HTTP_OTA_STATE_FAILED, HTTP_OTA_ERROR_DOWNLOAD);
            return ret;
        }

        /* Download complete */
        break;
    }

    /* Update final progress */
    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    s_bytes_received = (size_t)esp_https_ota_get_image_len_read(ota_handle);
    xSemaphoreGive(s_mutex);
    invoke_progress_callback();

    /* Verify image is complete */
    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "Incomplete data received");
        esp_https_ota_abort(ota_handle);
        update_state(HTTP_OTA_STATE_FAILED, HTTP_OTA_ERROR_DOWNLOAD);
        return ESP_FAIL;
    }

    /* Finalize OTA */
    ret = esp_https_ota_finish(ota_handle);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            update_state(HTTP_OTA_STATE_FAILED, HTTP_OTA_ERROR_VERIFY);
        } else {
            ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ret));
            update_state(HTTP_OTA_STATE_FAILED, HTTP_OTA_ERROR_WRITE);
        }
        return ret;
    }

    ESP_LOGI(TAG, "OTA update successful!");
    update_state(HTTP_OTA_STATE_COMPLETE, HTTP_OTA_ERROR_NONE);
    return ESP_OK;
}

/**
 * @brief OTA task function
 */
static void http_ota_task(void *param)
{
    (void)param;
    esp_err_t ret;

    ESP_LOGI(TAG, "OTA task started");

    /* Enter LIFECYCLE_OTA phase to stop non-essential services */
    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    s_previous_phase = lifecycle_get_phase();
    xSemaphoreGive(s_mutex);

    ret = lifecycle_enter_phase(LIFECYCLE_PHASE_OTA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enter OTA phase: %s", esp_err_to_name(ret));
        update_state(HTTP_OTA_STATE_FAILED, HTTP_OTA_ERROR_LIFECYCLE_FAILED);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Entered LIFECYCLE_OTA phase (previous: %s)",
             lifecycle_phase_name(s_previous_phase));

    /* Perform the OTA update */
    ret = perform_ota_update(&s_task_ctx.config);

    if (ret == ESP_OK) {
        /* OTA successful - wait briefly then reboot */
        ESP_LOGI(TAG, "Rebooting in %d seconds...", GW_OTA_RESTART_DELAY_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(GW_OTA_RESTART_DELAY_MS));
        esp_restart();
        /* Does not return */
    }

cleanup:
    /* Return to previous lifecycle phase on failure/abort */
    ESP_LOGI(TAG, "Returning to %s phase", lifecycle_phase_name(s_previous_phase));

    /* Default to NORMAL if previous phase was BOOT */
    if (s_previous_phase == LIFECYCLE_PHASE_BOOT ||
        s_previous_phase == LIFECYCLE_PHASE_OTA) {
        s_previous_phase = LIFECYCLE_PHASE_NORMAL;
    }

    lifecycle_enter_phase(s_previous_phase);

    /* Mark task as self-deleted */
    psram_task_mark_deleted(&s_ota_task_handle);

    ESP_LOGI(TAG, "OTA task exiting");
    vTaskDelete(NULL);
}
