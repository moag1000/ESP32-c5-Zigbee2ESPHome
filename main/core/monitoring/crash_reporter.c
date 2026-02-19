/**
 * @file crash_reporter.c
 * @brief Crash Reporter Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "crash_reporter.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "gateway_mqtt.h"
#include "mqtt/mqtt_helpers.h"
#include <string.h>

static const char *TAG = "CRASH_RPT";

/* ============================================================================
 * Module State
 * ============================================================================ */

/** @brief Module initialized flag */
static bool s_initialized = false;

/** @brief Cached crash information */
static crash_info_t s_crash_info = {0};

/* ============================================================================
 * Reset Reason Helpers
 * ============================================================================ */

const char* crash_reporter_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN:   return "unknown";
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:  return "task_watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

bool crash_reporter_is_crash(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;

        case ESP_RST_UNKNOWN:
        case ESP_RST_POWERON:
        case ESP_RST_EXT:
        case ESP_RST_SW:
        case ESP_RST_DEEPSLEEP:
        case ESP_RST_SDIO:
        default:
            return false;
    }
}

/* ============================================================================
 * NVS Boot Count Management
 * ============================================================================ */

/**
 * @brief Read boot count from NVS
 *
 * @param[out] boot_count Pointer to store boot count
 * @return ESP_OK on success, or NVS error code
 */
static esp_err_t read_boot_count(uint32_t *boot_count)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(CRASH_REPORTER_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace doesn't exist yet, return 0 */
        *boot_count = 0;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for reading: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_get_u32(nvs_handle, CRASH_REPORTER_NVS_KEY_BOOT_COUNT, boot_count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *boot_count = 0;
        ret = ESP_OK;
    }

    nvs_close(nvs_handle);
    return ret;
}

/**
 * @brief Write boot count to NVS
 *
 * @param[in] boot_count Boot count to store
 * @return ESP_OK on success, or NVS error code
 */
static esp_err_t write_boot_count(uint32_t boot_count)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(CRASH_REPORTER_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u32(nvs_handle, CRASH_REPORTER_NVS_KEY_BOOT_COUNT, boot_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write boot count: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    }

    nvs_close(nvs_handle);
    return ret;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t crash_reporter_init(void)
{
    if (s_initialized) {
        ESP_LOGD(TAG, "Crash reporter already initialized");
        return ESP_OK;
    }

    /* Get reset reason */
    s_crash_info.reason = esp_reset_reason();
    s_crash_info.reason_str = crash_reporter_reason_to_string(s_crash_info.reason);
    s_crash_info.was_crash = crash_reporter_is_crash(s_crash_info.reason);

    /* Read and increment boot count */
    esp_err_t ret = read_boot_count(&s_crash_info.boot_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read boot count, starting at 0");
        s_crash_info.boot_count = 0;
    }

    s_crash_info.boot_count++;

    ret = write_boot_count(s_crash_info.boot_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to write boot count: %s", esp_err_to_name(ret));
        /* Continue anyway - not critical */
    }

    s_initialized = true;

    /* Log the reset reason */
    if (s_crash_info.was_crash) {
        ESP_LOGW(TAG, "CRASH DETECTED! Reset reason: %s (boot #%lu)",
                 s_crash_info.reason_str, s_crash_info.boot_count);
    } else {
        ESP_LOGI(TAG, "Reset reason: %s (boot #%lu)",
                 s_crash_info.reason_str, s_crash_info.boot_count);
    }

    return ESP_OK;
}

esp_err_t crash_reporter_publish(void)
{
    REQUIRE_MQTT_CONNECTED(ESP_ERR_INVALID_STATE);

    if (!s_initialized) {
        ESP_LOGW(TAG, "Crash reporter not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create JSON payload */
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(json, "reset_reason", s_crash_info.reason_str);
    cJSON_AddBoolToObject(json, "was_crash", s_crash_info.was_crash);
    cJSON_AddNumberToObject(json, "boot_count", s_crash_info.boot_count);

    /* Add timestamp (uptime since boot in seconds) */
    int64_t uptime_us = esp_timer_get_time();
    uint32_t uptime_sec = (uint32_t)(uptime_us / 1000000);
    cJSON_AddNumberToObject(json, "uptime_since_boot", uptime_sec);

    /* Serialize and publish via event bus */
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return ESP_ERR_NO_MEM;
    }

    /* Ownership transfer: strdup topic (constant), json_str (heap) transfers directly */
    char *topic_heap = strdup(CRASH_REPORTER_MQTT_TOPIC);
    if (topic_heap == NULL) {
        free(json_str);
        ESP_LOGE(TAG, "Failed to strdup topic");
        return ESP_ERR_NO_MEM;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_heap,
        .payload_json = json_str,
        .qos = 1,
        .retain = true
    };
    esp_err_t ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret == ESP_OK) {
        /* Ownership transferred to handler - do NOT free */
        ESP_LOGI(TAG, "Published crash report: reason=%s, was_crash=%s, boot_count=%lu",
                 s_crash_info.reason_str,
                 s_crash_info.was_crash ? "true" : "false",
                 s_crash_info.boot_count);
    } else {
        /* event_publish failed - ownership NOT transferred, free here */
        free(topic_heap);
        free(json_str);
        ESP_LOGE(TAG, "Failed to publish crash report: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t crash_reporter_get_info(crash_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *info = s_crash_info;
    return ESP_OK;
}

uint32_t crash_reporter_get_boot_count(void)
{
    if (!s_initialized) {
        return 0;
    }
    return s_crash_info.boot_count;
}

const char* crash_reporter_get_reason_str(void)
{
    if (!s_initialized) {
        return "unknown";
    }
    return s_crash_info.reason_str;
}

bool crash_reporter_was_crash(void)
{
    if (!s_initialized) {
        return false;
    }
    return s_crash_info.was_crash;
}

bool crash_reporter_is_initialized(void)
{
    return s_initialized;
}

esp_err_t crash_reporter_reset_boot_count(void)
{
    esp_err_t ret = write_boot_count(0);
    if (ret == ESP_OK) {
        s_crash_info.boot_count = 0;
        ESP_LOGI(TAG, "Boot count reset to 0");
    }
    return ret;
}
