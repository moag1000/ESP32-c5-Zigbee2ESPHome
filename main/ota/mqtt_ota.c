/**
 * @file mqtt_ota.c
 * @brief MQTT OTA Trigger Implementation
 *
 * Implements MQTT-triggered OTA updates compatible with Zigbee2MQTT
 * bridge commands. Handles check, update, and abort commands via MQTT,
 * and publishes progress updates during OTA operations.
 *
 * Uses the http_ota module for actual OTA operations and ota_handler
 * for version checking and configuration.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "mqtt_ota.h"
#include "http_ota.h"
#include "ota_handler.h"
#include "gateway_mqtt.h"
#include "core/bridge/bridge_response.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "MQTT_OTA";

/* ============================================================================
 * Internal State
 * ============================================================================ */

/** @brief Module initialization state */
static bool s_initialized = false;

/** @brief Current MQTT OTA status */
static mqtt_ota_status_t s_status = MQTT_OTA_STATUS_IDLE;

/** @brief Mutex for thread-safe status access */
static SemaphoreHandle_t s_status_mutex = NULL;

/** @brief Current transaction ID for response correlation */
static char s_transaction_id[128] = {0};

/** @brief Status string mapping */
static const char* s_status_strings[] = {
    "idle",
    "checking",
    "available",
    "started",
    "progress",
    "completed",
    "failed",
    "aborted"
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void http_ota_progress_callback(size_t received, size_t total, void *ctx);
static esp_err_t publish_response(const char *topic, bool success,
                                  const char *message, int progress,
                                  const char *error);

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * @brief Set internal status with mutex protection
 */
static void set_status(mqtt_ota_status_t status)
{
    if (s_status_mutex != NULL) {
        if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_status = status;
            xSemaphoreGive(s_status_mutex);
        }
    } else {
        s_status = status;
    }
}

/**
 * @brief Get internal status with mutex protection
 */
static mqtt_ota_status_t get_status(void)
{
    mqtt_ota_status_t status = MQTT_OTA_STATUS_IDLE;

    if (s_status_mutex != NULL) {
        if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            status = s_status;
            xSemaphoreGive(s_status_mutex);
        }
    } else {
        status = s_status;
    }

    return status;
}

/**
 * @brief Extract transaction ID from JSON payload
 */
static void extract_transaction_id(const cJSON *json)
{
    s_transaction_id[0] = '\0';

    if (json == NULL) {
        return;
    }

    const cJSON *txn = cJSON_GetObjectItem(json, "transaction");
    if (txn == NULL) {
        txn = cJSON_GetObjectItem(json, "id");
    }

    if (cJSON_IsString(txn) && txn->valuestring != NULL) {
        strncpy(s_transaction_id, txn->valuestring, sizeof(s_transaction_id) - 1);
        s_transaction_id[sizeof(s_transaction_id) - 1] = '\0';
    }
}

/**
 * @brief Get current transaction ID
 */
static const char* get_transaction_id(void)
{
    return s_transaction_id[0] != '\0' ? s_transaction_id : NULL;
}

/**
 * @brief Convert HTTP OTA state to MQTT OTA status
 */
static mqtt_ota_status_t http_state_to_mqtt_status(http_ota_state_t state)
{
    switch (state) {
        case HTTP_OTA_STATE_IDLE:
            return MQTT_OTA_STATUS_IDLE;
        case HTTP_OTA_STATE_STARTING:
            return MQTT_OTA_STATUS_STARTED;
        case HTTP_OTA_STATE_DOWNLOADING:
        case HTTP_OTA_STATE_VERIFYING:
        case HTTP_OTA_STATE_WRITING:
            return MQTT_OTA_STATUS_PROGRESS;
        case HTTP_OTA_STATE_COMPLETE:
            return MQTT_OTA_STATUS_COMPLETED;
        case HTTP_OTA_STATE_FAILED:
            return MQTT_OTA_STATUS_FAILED;
        case HTTP_OTA_STATE_ABORTED:
            return MQTT_OTA_STATUS_ABORTED;
        default:
            return MQTT_OTA_STATUS_IDLE;
    }
}

/**
 * @brief HTTP OTA progress callback - called by http_ota during updates
 */
static void http_ota_progress_callback(size_t received, size_t total, void *ctx)
{
    (void)ctx;

    http_ota_state_t state = http_ota_get_state();
    mqtt_ota_status_t mqtt_status = http_state_to_mqtt_status(state);
    set_status(mqtt_status);

    /* Calculate progress percentage */
    int progress = -1;
    if (total > 0) {
        progress = (int)((received * 100) / total);
    }

    const char *status_str = NULL;
    const char *error = NULL;

    switch (state) {
        case HTTP_OTA_STATE_STARTING:
            status_str = "started";
            break;
        case HTTP_OTA_STATE_DOWNLOADING:
            status_str = "progress";
            break;
        case HTTP_OTA_STATE_VERIFYING:
            status_str = "verifying";
            progress = 100; /* Verifying happens after download complete */
            break;
        case HTTP_OTA_STATE_WRITING:
            status_str = "updating";
            break;
        case HTTP_OTA_STATE_COMPLETE:
            status_str = "completed";
            progress = 100;
            break;
        case HTTP_OTA_STATE_FAILED:
            status_str = "failed";
            error = http_ota_error_str(http_ota_get_error());
            break;
        case HTTP_OTA_STATE_ABORTED:
            status_str = "aborted";
            break;
        default:
            return; /* Don't publish for idle state */
    }

    if (status_str != NULL) {
        mqtt_ota_publish_status_with_id(status_str, progress, error,
                                        get_transaction_id());
    }
}

/**
 * @brief Publish response to MQTT
 */
static esp_err_t publish_response(const char *topic, bool success,
                                  const char *message, int progress,
                                  const char *error)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON response");
        return ESP_ERR_NO_MEM;
    }

    /* Add transaction ID if present */
    const char *txn = get_transaction_id();
    if (txn != NULL) {
        cJSON_AddStringToObject(root, "transaction", txn);
    }

    /* Build data object */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    if (message != NULL) {
        cJSON_AddStringToObject(data, "message", message);
    }

    if (progress >= 0) {
        cJSON_AddNumberToObject(data, "progress", progress);
    }

    cJSON_AddItemToObject(root, "data", data);

    /* Add status */
    cJSON_AddStringToObject(root, "status", success ? "ok" : "error");

    /* Add error if present */
    if (error != NULL && !success) {
        cJSON_AddStringToObject(root, "error", error);
    }

    /* Publish */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON response");
        return ESP_ERR_NO_MEM;
    }

    /* Ownership transfer: handler frees both topic and payload_json via free().
     * topic is a function param (constant) -> strdup.
     * json_str from cJSON_PrintUnformatted -> already heap, transfer directly. */
    char *topic_heap = strdup(topic);
    if (topic_heap == NULL) {
        ESP_LOGE(TAG, "Failed to strdup response topic");
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_heap,
        .payload_json = json_str,
        .qos = 0,
        .retain = false
    };
    esp_err_t ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret != ESP_OK) {
        /* event_publish failed - handler will not run, free both */
        free(topic_heap);
        free(json_str);
        ESP_LOGE(TAG, "Failed to publish response to %s", topic);
    }
    /* On success: do NOT free - ownership transferred to handler */

    return ret;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t mqtt_ota_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "MQTT OTA already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing MQTT OTA module...");

    /* Initialize HTTP OTA module if not already done */
    esp_err_t ret = http_ota_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize HTTP OTA module: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create mutex */
    s_status_mutex = xSemaphoreCreateMutex();
    if (s_status_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create status mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Subscribe to OTA topics */
    ret = mqtt_client_subscribe(MQTT_OTA_TOPIC_CHECK, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to check topic");
        vSemaphoreDelete(s_status_mutex);
        s_status_mutex = NULL;
        return ret;
    }

    ret = mqtt_client_subscribe(MQTT_OTA_TOPIC_UPDATE, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to update topic");
        mqtt_client_unsubscribe(MQTT_OTA_TOPIC_CHECK);
        vSemaphoreDelete(s_status_mutex);
        s_status_mutex = NULL;
        return ret;
    }

    ret = mqtt_client_subscribe(MQTT_OTA_TOPIC_ABORT, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to abort topic");
        mqtt_client_unsubscribe(MQTT_OTA_TOPIC_CHECK);
        mqtt_client_unsubscribe(MQTT_OTA_TOPIC_UPDATE);
        vSemaphoreDelete(s_status_mutex);
        s_status_mutex = NULL;
        return ret;
    }

    s_initialized = true;
    set_status(MQTT_OTA_STATUS_IDLE);

    ESP_LOGI(TAG, "MQTT OTA module initialized successfully");
    return ESP_OK;
}

esp_err_t mqtt_ota_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing MQTT OTA module...");

    /* Unsubscribe from topics */
    mqtt_client_unsubscribe(MQTT_OTA_TOPIC_CHECK);
    mqtt_client_unsubscribe(MQTT_OTA_TOPIC_UPDATE);
    mqtt_client_unsubscribe(MQTT_OTA_TOPIC_ABORT);

    /* Delete mutex */
    if (s_status_mutex != NULL) {
        vSemaphoreDelete(s_status_mutex);
        s_status_mutex = NULL;
    }

    s_initialized = false;

    ESP_LOGI(TAG, "MQTT OTA module deinitialized");
    return ESP_OK;
}

esp_err_t mqtt_ota_publish_status(const char *status, int progress, const char *error)
{
    return mqtt_ota_publish_status_with_id(status, progress, error, NULL);
}

esp_err_t mqtt_ota_publish_status_with_id(const char *status, int progress,
                                          const char *error, const char *transaction_id)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON status");
        return ESP_ERR_NO_MEM;
    }

    /* Add transaction ID if present */
    if (transaction_id != NULL) {
        cJSON_AddStringToObject(root, "id", transaction_id);
    }

    /* Add status */
    cJSON_AddStringToObject(root, "status", status);

    /* Add progress if valid */
    if (progress >= 0 && progress <= 100) {
        cJSON_AddNumberToObject(root, "progress", progress);
    }

    /* Add error if present */
    if (error != NULL) {
        cJSON_AddStringToObject(root, "error", error);
    }

    /* Add version info */
    char version[32];
    if (http_ota_get_current_version(version, sizeof(version)) == ESP_OK) {
        cJSON_AddStringToObject(root, "current_version", version);
    }

    if (http_ota_get_new_version(version, sizeof(version)) == ESP_OK) {
        cJSON_AddStringToObject(root, "available_version", version);
    }

    /* Serialize and publish */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON status");
        return ESP_ERR_NO_MEM;
    }

    /* Ownership transfer: handler frees both topic and payload_json via free().
     * MQTT_OTA_TOPIC_STATUS is a constant -> strdup.
     * json_str from cJSON_PrintUnformatted -> already heap, transfer directly. */
    char *topic_heap = strdup(MQTT_OTA_TOPIC_STATUS);
    if (topic_heap == NULL) {
        ESP_LOGE(TAG, "Failed to strdup OTA status topic");
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_heap,
        .payload_json = json_str,
        .qos = 0,
        .retain = false
    };
    esp_err_t ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret != ESP_OK) {
        /* event_publish failed - handler will not run, free both */
        free(topic_heap);
        free(json_str);
        ESP_LOGE(TAG, "Failed to publish OTA status");
    } else {
        ESP_LOGD(TAG, "Published OTA status: %s, progress: %d", status, progress);
    }
    /* On success: do NOT free - ownership transferred to handler */

    return ret;
}

esp_err_t mqtt_ota_handle_check(const char *payload, size_t len)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "MQTT OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Handling OTA check request");

    /* Parse payload */
    cJSON *json = NULL;
    if (payload != NULL && len > 0) {
        json = cJSON_ParseWithLength(payload, len);
    }

    /* Extract transaction ID */
    extract_transaction_id(json);

    /* Check if OTA is already in progress */
    if (http_ota_is_in_progress()) {
        ESP_LOGW(TAG, "OTA operation already in progress");
        publish_response(MQTT_OTA_RESPONSE_CHECK, false,
                        "OTA operation already in progress", -1,
                        "OTA busy");
        if (json != NULL) {
            cJSON_Delete(json);
        }
        return ESP_ERR_INVALID_STATE;
    }

    /* Set status to checking */
    set_status(MQTT_OTA_STATUS_CHECKING);

    /* For check, we use the ota_handler which has version checking support */
    esp_err_t ret = ota_handler_check_for_update();

    ota_info_t info = ota_handler_get_info();

    if (ret != ESP_OK && info.error != OTA_ERROR_NO_UPDATE) {
        set_status(MQTT_OTA_STATUS_FAILED);
        publish_response(MQTT_OTA_RESPONSE_CHECK, false,
                        "OTA check failed", -1,
                        ota_handler_get_error_string(info.error));
        if (json != NULL) {
            cJSON_Delete(json);
        }
        return ret;
    }

    /* Build response */
    cJSON *response_data = cJSON_CreateObject();
    if (response_data != NULL) {
        cJSON_AddBoolToObject(response_data, "checked", true);
        cJSON_AddStringToObject(response_data, "current_version", info.current_version);
        cJSON_AddBoolToObject(response_data, "update_available", info.update_available);
        if (info.update_available) {
            cJSON_AddStringToObject(response_data, "available_version", info.available_version);
            set_status(MQTT_OTA_STATUS_AVAILABLE);
        } else {
            set_status(MQTT_OTA_STATUS_IDLE);
        }
    }

    bridge_response_publish_ok(MQTT_OTA_RESPONSE_CHECK, response_data,
                               get_transaction_id());
    cJSON_Delete(response_data);

    if (json != NULL) {
        cJSON_Delete(json);
    }

    ESP_LOGI(TAG, "OTA check completed, update_available: %s",
             info.update_available ? "yes" : "no");

    return ESP_OK;
}

esp_err_t mqtt_ota_handle_update(const char *payload, size_t len)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "MQTT OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Handling OTA update request");

    /* Parse payload */
    cJSON *json = NULL;
    if (payload != NULL && len > 0) {
        json = cJSON_ParseWithLength(payload, len);
    }

    if (json == NULL) {
        ESP_LOGE(TAG, "Invalid JSON payload for OTA update");
        publish_response(MQTT_OTA_RESPONSE_UPDATE, false,
                        "Invalid JSON payload", -1,
                        "Parse error");
        return ESP_ERR_INVALID_ARG;
    }

    /* Extract transaction ID */
    extract_transaction_id(json);

    /* Check if OTA is already in progress */
    if (http_ota_is_in_progress()) {
        ESP_LOGW(TAG, "OTA operation already in progress");
        publish_response(MQTT_OTA_RESPONSE_UPDATE, false,
                        "OTA operation already in progress", -1,
                        "OTA busy");
        cJSON_Delete(json);
        return ESP_ERR_INVALID_STATE;
    }

    /* Extract URL - required for update */
    const cJSON *url_item = cJSON_GetObjectItem(json, "url");
    if (!cJSON_IsString(url_item) || url_item->valuestring == NULL ||
        strlen(url_item->valuestring) == 0) {
        ESP_LOGE(TAG, "Missing or invalid 'url' field in OTA update request");
        publish_response(MQTT_OTA_RESPONSE_UPDATE, false,
                        "Missing 'url' field", -1,
                        "URL required");
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }

    /* Build HTTP OTA configuration */
    http_ota_config_t ota_config = {0};
    strncpy(ota_config.url, url_item->valuestring, sizeof(ota_config.url) - 1);

    /* Check for skip_cert_verify flag */
    const cJSON *skip_cert = cJSON_GetObjectItem(json, "skip_cert_verify");
    if (cJSON_IsBool(skip_cert)) {
        ota_config.skip_cert_verify = cJSON_IsTrue(skip_cert);
    }

    /* Check force flag (for version validation skip) */
    bool force = false;
    const cJSON *force_item = cJSON_GetObjectItem(json, "force");
    if (cJSON_IsBool(force_item)) {
        force = cJSON_IsTrue(force_item);
    }
    (void)force; /* Note: http_ota doesn't have force flag yet, but we accept it */

    cJSON_Delete(json);

    /* Publish starting response */
    set_status(MQTT_OTA_STATUS_STARTED);
    publish_response(MQTT_OTA_RESPONSE_UPDATE, true,
                    "OTA update starting", 0, NULL);

    /* Publish status update */
    mqtt_ota_publish_status_with_id("started", 0, NULL, get_transaction_id());

    /* Small delay to ensure MQTT message is sent */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Start HTTP OTA update with progress callback */
    ESP_LOGI(TAG, "Starting OTA update from URL: %s", ota_config.url);
    esp_err_t ret = http_ota_start(&ota_config, http_ota_progress_callback, NULL);

    if (ret != ESP_OK) {
        /* Immediate failure (couldn't start) */
        set_status(MQTT_OTA_STATUS_FAILED);
        const char *error_str = http_ota_error_str(http_ota_get_error());
        publish_response(MQTT_OTA_RESPONSE_UPDATE, false,
                        "OTA update failed to start", -1, error_str);
        mqtt_ota_publish_status_with_id("failed", -1, error_str, get_transaction_id());
        ESP_LOGE(TAG, "OTA update failed to start: %s", error_str);
        return ret;
    }

    /* OTA started successfully in background task
     * Progress will be reported via callback */
    ESP_LOGI(TAG, "OTA update started in background");
    return ESP_OK;
}

esp_err_t mqtt_ota_handle_abort(const char *payload, size_t len)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "MQTT OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Handling OTA abort request");

    /* Parse payload for transaction ID */
    cJSON *json = NULL;
    if (payload != NULL && len > 0) {
        json = cJSON_ParseWithLength(payload, len);
    }

    extract_transaction_id(json);
    if (json != NULL) {
        cJSON_Delete(json);
    }

    /* Check if OTA is in progress */
    if (!http_ota_is_in_progress()) {
        ESP_LOGW(TAG, "No OTA operation in progress to abort");
        publish_response(MQTT_OTA_RESPONSE_ABORT, false,
                        "No OTA operation in progress", -1,
                        "Nothing to abort");
        return ESP_ERR_INVALID_STATE;
    }

    /* Abort OTA */
    esp_err_t ret = http_ota_abort();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to abort OTA: %s", esp_err_to_name(ret));
        publish_response(MQTT_OTA_RESPONSE_ABORT, false,
                        "Failed to abort OTA", -1,
                        esp_err_to_name(ret));
        return ret;
    }

    set_status(MQTT_OTA_STATUS_ABORTED);
    publish_response(MQTT_OTA_RESPONSE_ABORT, true,
                    "OTA update aborted", -1, NULL);
    mqtt_ota_publish_status_with_id("aborted", -1, NULL, get_transaction_id());

    ESP_LOGI(TAG, "OTA update aborted successfully");
    return ESP_OK;
}

bool mqtt_ota_is_initialized(void)
{
    return s_initialized;
}

mqtt_ota_status_t mqtt_ota_get_status(void)
{
    return get_status();
}

const char* mqtt_ota_status_to_string(mqtt_ota_status_t status)
{
    if (status < sizeof(s_status_strings) / sizeof(s_status_strings[0])) {
        return s_status_strings[status];
    }
    return "unknown";
}

esp_err_t mqtt_ota_resubscribe(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "MQTT OTA not initialized, skipping resubscribe");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Re-subscribing to OTA topics...");

    esp_err_t ret = mqtt_client_subscribe(MQTT_OTA_TOPIC_CHECK, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-subscribe to check topic");
        return ret;
    }

    ret = mqtt_client_subscribe(MQTT_OTA_TOPIC_UPDATE, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-subscribe to update topic");
        return ret;
    }

    ret = mqtt_client_subscribe(MQTT_OTA_TOPIC_ABORT, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-subscribe to abort topic");
        return ret;
    }

    ESP_LOGI(TAG, "OTA topics re-subscribed successfully");
    return ESP_OK;
}
