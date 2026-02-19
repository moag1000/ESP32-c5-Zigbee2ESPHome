/**
 * @file mqtt_logger.c
 * @brief MQTT-based Logging System Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "mqtt_logger.h"
#include "bridge_response.h"
#include "gateway_mqtt.h"
#include "mqtt/mqtt_helpers.h"
#include "gateway_defaults.h"
#include "mqtt/mqtt_topics.h"
#include "memory/memory_manager_ng.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "utils/freertos_helpers.h"  /* For PSRAM task creation */
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdio.h>

/* Log tag for internal logging */
static const char *TAG = "MQTT_LOG";

/* MQTT topics */
#define MQTT_LOGGER_TOPIC           TOPIC_BRIDGE_LOGGING
#define MQTT_LOGGER_LEVEL_REQUEST   TOPIC_BRIDGE_REQUEST_LOGGING_LEVEL
#define MQTT_LOGGER_NS_REQUEST      TOPIC_BRIDGE_REQUEST_LOGGING_NAMESPACES
#define MQTT_LOGGER_LEVEL_RESPONSE  TOPIC_BRIDGE_RESPONSE_LOGGING_LEVEL
#define MQTT_LOGGER_NS_RESPONSE     TOPIC_BRIDGE_RESPONSE_LOGGING_NAMESPACES

/* Task configuration */
#define LOGGER_TASK_STACK_SIZE      4096    /* Reduced: uses buffer pools for JSON */
#define LOGGER_TASK_PRIORITY        3
#define LOGGER_TASK_NAME            "mqtt_logger"

/* Ring buffer configuration */
#define DEFAULT_RING_BUFFER_SIZE    32
#define MAX_RING_BUFFER_SIZE        128
#define RING_BUFFER_ITEM_SIZE       sizeof(mqtt_log_entry_t)

/* Publishing rate limit (milliseconds between publishes) */
#define PUBLISH_RATE_LIMIT_MS       50

/* Level strings */
static const char *s_level_strings[] = {
    [MQTT_LOG_ERROR] = "error",
    [MQTT_LOG_WARNING] = "warning",
    [MQTT_LOG_INFO] = "info",
    [MQTT_LOG_DEBUG] = "debug"
};

/* Namespace strings */
static const char *s_namespace_strings[] = {
    [MQTT_LOG_NS_SYSTEM] = "system",
    [MQTT_LOG_NS_ZIGBEE] = "zigbee",
    [MQTT_LOG_NS_MQTT] = "mqtt",
    [MQTT_LOG_NS_WIFI] = "wifi",
    [MQTT_LOG_NS_BLE] = "ble"
};

/* Logger state */
static struct {
    bool initialized;
    bool running;
    mqtt_log_level_t level;
    bool namespaces_enabled[MQTT_LOG_NS_COUNT];
    RingbufHandle_t ring_buffer;
    psram_task_handle_t psram_task;  /**< PSRAM-backed task (saves internal RAM) */
    SemaphoreHandle_t config_mutex;
    mqtt_logger_stats_t stats;
    vprintf_like_t original_vprintf;
    bool esp_log_hooked;
    bool publishing;  /**< Re-entry guard to prevent feedback loop */
} s_logger = {
    .initialized = false,
    .running = false,
    .level = MQTT_LOG_INFO,
    .namespaces_enabled = {true, true, true, true, true},
    .ring_buffer = NULL,
    .psram_task = {0},
    .config_mutex = NULL,
    .stats = {0},
    .original_vprintf = NULL,
    .esp_log_hooked = false,
    .publishing = false
};

/* Forward declarations */
static void logger_task(void *pvParameters);
static esp_err_t publish_log_entry(const mqtt_log_entry_t *entry);
static mqtt_log_namespace_t detect_namespace_from_tag(const char *tag);
static mqtt_log_level_t esp_log_level_to_mqtt(esp_log_level_t esp_level);
static int mqtt_logger_vprintf(const char *format, va_list args);

/**
 * @brief Initialize MQTT logger
 */
esp_err_t mqtt_logger_init(mqtt_log_level_t level)
{
    mqtt_logger_config_t config = MQTT_LOGGER_CONFIG_DEFAULT;
    config.level = level;
    return mqtt_logger_init_config(&config);
}

/**
 * @brief Initialize MQTT logger with full configuration
 */
esp_err_t mqtt_logger_init_config(const mqtt_logger_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_logger.initialized) {
        ESP_LOGW(TAG, "MQTT logger already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing MQTT logger...");

    /* Validate configuration */
    uint32_t buffer_size = config->ring_buffer_size;
    if (buffer_size == 0) {
        buffer_size = DEFAULT_RING_BUFFER_SIZE;
    } else if (buffer_size > MAX_RING_BUFFER_SIZE) {
        buffer_size = MAX_RING_BUFFER_SIZE;
    }

    /* Create configuration mutex */
    s_logger.config_mutex = xSemaphoreCreateMutex();
    if (s_logger.config_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create config mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create ring buffer */
    s_logger.ring_buffer = xRingbufferCreate(
        buffer_size * RING_BUFFER_ITEM_SIZE,
        RINGBUF_TYPE_NOSPLIT
    );
    if (s_logger.ring_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        vSemaphoreDelete(s_logger.config_mutex);
        s_logger.config_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Apply configuration */
    s_logger.level = config->level;
    memcpy(s_logger.namespaces_enabled, config->namespaces_enabled,
           sizeof(s_logger.namespaces_enabled));

    /* Reset statistics */
    memset(&s_logger.stats, 0, sizeof(mqtt_logger_stats_t));

    /* Hook into ESP_LOG if requested */
    if (config->hook_esp_log) {
        s_logger.original_vprintf = esp_log_set_vprintf(mqtt_logger_vprintf);
        s_logger.esp_log_hooked = true;
        ESP_LOGI(TAG, "Hooked into ESP_LOG system");
    }

    s_logger.initialized = true;
    ESP_LOGI(TAG, "MQTT logger initialized (level: %s, buffer: %lu entries)",
             mqtt_logger_level_to_str(s_logger.level), buffer_size);

    return ESP_OK;
}

/**
 * @brief Deinitialize MQTT logger
 */
esp_err_t mqtt_logger_deinit(void)
{
    if (!s_logger.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing MQTT logger...");

    /* Stop task if running */
    mqtt_logger_stop();

    /* Unhook from ESP_LOG */
    if (s_logger.esp_log_hooked && s_logger.original_vprintf != NULL) {
        esp_log_set_vprintf(s_logger.original_vprintf);
        s_logger.esp_log_hooked = false;
        s_logger.original_vprintf = NULL;
    }

    /* Delete ring buffer */
    if (s_logger.ring_buffer != NULL) {
        vRingbufferDelete(s_logger.ring_buffer);
        s_logger.ring_buffer = NULL;
    }

    /* Delete mutex */
    if (s_logger.config_mutex != NULL) {
        vSemaphoreDelete(s_logger.config_mutex);
        s_logger.config_mutex = NULL;
    }

    s_logger.initialized = false;
    ESP_LOGI(TAG, "MQTT logger deinitialized");

    return ESP_OK;
}

/**
 * @brief Start MQTT logger publishing task
 */
esp_err_t mqtt_logger_start(void)
{
    if (!s_logger.initialized) {
        ESP_LOGE(TAG, "MQTT logger not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_logger.running) {
        ESP_LOGW(TAG, "MQTT logger already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting MQTT logger task...");

    /* CRITICAL: Set running flag BEFORE creating task to prevent race condition
     * The task checks s_logger.running in its while loop - if this is false
     * when the task starts, it will exit immediately */
    s_logger.running = true;

    /* Create publisher task with PSRAM stack (saves ~6KB internal RAM) */
    esp_err_t ret = psram_task_create(
        logger_task,
        LOGGER_TASK_NAME,
        LOGGER_TASK_STACK_SIZE,
        NULL,
        LOGGER_TASK_PRIORITY,
        &s_logger.psram_task
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create logger task: %s", esp_err_to_name(ret));
        s_logger.running = false;  /* Reset flag on failure */
        return ret;
    }

    ESP_LOGI(TAG, "MQTT logger started (PSRAM stack)");

    return ESP_OK;
}

/**
 * @brief Stop MQTT logger publishing task
 */
esp_err_t mqtt_logger_stop(void)
{
    if (!s_logger.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping MQTT logger task...");

    s_logger.running = false;

    /* Wait for task to finish gracefully */
    if (psram_task_is_valid(&s_logger.psram_task)) {
        /* Give task time to exit gracefully */
        uint32_t timeout = GW_DEFAULT_TASK_STOP_POLL_MS * 5;
        while (psram_task_is_valid(&s_logger.psram_task) && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(GW_DEFAULT_TASK_STOP_POLL_MS));
            timeout -= GW_DEFAULT_TASK_STOP_POLL_MS;
        }

        if (psram_task_is_valid(&s_logger.psram_task)) {
            ESP_LOGW(TAG, "Logger task did not stop gracefully, deleting");
        }
    }

    /* Free PSRAM resources (handles task deletion if needed) */
    psram_task_delete(&s_logger.psram_task);

    ESP_LOGI(TAG, "MQTT logger stopped");
    return ESP_OK;
}

/**
 * @brief Log a message with printf-style formatting
 */
esp_err_t mqtt_logger_log(mqtt_log_level_t level, mqtt_log_namespace_t ns,
                          const char *format, ...)
{
    va_list args;
    va_start(args, format);
    esp_err_t ret = mqtt_logger_logv(level, ns, format, args);
    va_end(args);
    return ret;
}

/**
 * @brief Log a message with va_list arguments
 */
esp_err_t mqtt_logger_logv(mqtt_log_level_t level, mqtt_log_namespace_t ns,
                           const char *format, va_list args)
{
    if (!s_logger.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check level threshold */
    if (level > s_logger.level) {
        s_logger.stats.messages_filtered++;
        return ESP_OK;
    }

    /* Check namespace filter */
    if (ns < MQTT_LOG_NS_COUNT && !s_logger.namespaces_enabled[ns]) {
        s_logger.stats.messages_filtered++;
        return ESP_OK;
    }

    /* Create log entry */
    mqtt_log_entry_t entry;
    entry.level = level;
    entry.ns = ns;
    entry.timestamp = (uint32_t)(esp_timer_get_time() / 1000);  /* Convert to ms */

    /* Format message */
    vsnprintf(entry.message, sizeof(entry.message), format, args);

    /* Try to add to ring buffer (non-blocking) */
    BaseType_t ret;
    if (xPortInIsrContext()) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        ret = xRingbufferSendFromISR(s_logger.ring_buffer, &entry,
                                     sizeof(mqtt_log_entry_t),
                                     &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    } else {
        ret = xRingbufferSend(s_logger.ring_buffer, &entry,
                              sizeof(mqtt_log_entry_t), 0);
    }

    if (ret != pdTRUE) {
        s_logger.stats.messages_dropped++;
        return ESP_ERR_NO_MEM;
    }

    s_logger.stats.messages_logged++;
    return ESP_OK;
}

/**
 * @brief Set log level threshold
 */
esp_err_t mqtt_logger_set_level(mqtt_log_level_t level)
{
    if (level > MQTT_LOG_DEBUG) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_logger.config_mutex, GW_DEFAULT_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        mqtt_log_level_t old_level = s_logger.level;
        s_logger.level = level;
        xSemaphoreGive(s_logger.config_mutex);

        ESP_LOGI(TAG, "Log level changed: %s -> %s",
                 mqtt_logger_level_to_str(old_level),
                 mqtt_logger_level_to_str(level));
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Get current log level threshold
 */
mqtt_log_level_t mqtt_logger_get_level(void)
{
    return s_logger.level;
}

/**
 * @brief Enable or disable a namespace
 */
esp_err_t mqtt_logger_enable_namespace(mqtt_log_namespace_t ns, bool enable)
{
    if (ns >= MQTT_LOG_NS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_logger.config_mutex, GW_DEFAULT_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        s_logger.namespaces_enabled[ns] = enable;
        xSemaphoreGive(s_logger.config_mutex);

        ESP_LOGI(TAG, "Namespace %s %s",
                 mqtt_logger_namespace_to_str(ns),
                 enable ? "enabled" : "disabled");
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Check if namespace is enabled
 */
bool mqtt_logger_is_namespace_enabled(mqtt_log_namespace_t ns)
{
    if (ns >= MQTT_LOG_NS_COUNT) {
        return false;
    }
    return s_logger.namespaces_enabled[ns];
}

/**
 * @brief Get logger statistics
 */
esp_err_t mqtt_logger_get_stats(mqtt_logger_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(stats, &s_logger.stats, sizeof(mqtt_logger_stats_t));
    return ESP_OK;
}

/**
 * @brief Reset logger statistics
 */
esp_err_t mqtt_logger_reset_stats(void)
{
    memset(&s_logger.stats, 0, sizeof(mqtt_logger_stats_t));
    return ESP_OK;
}

/**
 * @brief Convert log level to string
 */
const char* mqtt_logger_level_to_str(mqtt_log_level_t level)
{
    if (level > MQTT_LOG_DEBUG) {
        return "unknown";
    }
    return s_level_strings[level];
}

/**
 * @brief Convert string to log level
 */
mqtt_log_level_t mqtt_logger_str_to_level(const char *str)
{
    if (str == NULL) {
        return MQTT_LOG_INFO;
    }

    for (int i = 0; i <= MQTT_LOG_DEBUG; i++) {
        if (strcasecmp(str, s_level_strings[i]) == 0) {
            return (mqtt_log_level_t)i;
        }
    }

    return MQTT_LOG_INFO;
}

/**
 * @brief Convert namespace to string
 */
const char* mqtt_logger_namespace_to_str(mqtt_log_namespace_t ns)
{
    if (ns >= MQTT_LOG_NS_COUNT) {
        return "unknown";
    }
    return s_namespace_strings[ns];
}

/**
 * @brief Convert string to namespace
 */
mqtt_log_namespace_t mqtt_logger_str_to_namespace(const char *str)
{
    if (str == NULL) {
        return MQTT_LOG_NS_SYSTEM;
    }

    for (int i = 0; i < MQTT_LOG_NS_COUNT; i++) {
        if (strcasecmp(str, s_namespace_strings[i]) == 0) {
            return (mqtt_log_namespace_t)i;
        }
    }

    return MQTT_LOG_NS_SYSTEM;
}

/**
 * @brief Process logging configuration request
 */
esp_err_t mqtt_logger_process_request(const char *topic, const char *payload, size_t len)
{
    if (topic == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Processing logging request: %s", topic);

    /* Parse JSON payload */
    char *payload_str = (char *)mem_alloc(len + 1, MEM_CAP_DEFAULT);
    if (payload_str == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(payload_str, payload, len);
    payload_str[len] = '\0';

    cJSON *json = cJSON_Parse(payload_str);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON payload");
        mem_ng_free(payload_str);
        return ESP_ERR_INVALID_ARG;
    }

    /* Extract transaction ID if present */
    const char *txn_id = bridge_request_get_transaction(json);

    /* Handle level request */
    if (strstr(topic, "/level") != NULL) {
        cJSON *level_item = cJSON_GetObjectItem(json, "level");
        if (level_item != NULL && cJSON_IsString(level_item)) {
            mqtt_log_level_t new_level = mqtt_logger_str_to_level(level_item->valuestring);
            esp_err_t ret = mqtt_logger_set_level(new_level);

            if (ret == ESP_OK) {
                /* Build success response */
                cJSON *data = cJSON_CreateObject();
                if (data == NULL) {
                    ESP_LOGE(TAG, "Failed to create response data JSON object");
                    cJSON_Delete(json);
                    mem_ng_free(payload_str);
                    return ESP_ERR_NO_MEM;
                }
                cJSON_AddStringToObject(data, "level", mqtt_logger_level_to_str(new_level));
                bridge_response_publish_ok(MQTT_LOGGER_LEVEL_RESPONSE, data, txn_id);
                cJSON_Delete(data);
            } else {
                bridge_response_publish_error(MQTT_LOGGER_LEVEL_RESPONSE,
                                             "Failed to set log level", txn_id);
            }
        } else {
            bridge_response_publish_error(MQTT_LOGGER_LEVEL_RESPONSE,
                                         "Missing or invalid 'level' field", txn_id);
        }
    }
    /* Handle namespaces request */
    else if (strstr(topic, "/namespaces") != NULL) {
        /* Iterate through namespace names in payload */
        for (int i = 0; i < MQTT_LOG_NS_COUNT; i++) {
            const char *ns_name = s_namespace_strings[i];
            cJSON *ns_item = cJSON_GetObjectItem(json, ns_name);
            if (ns_item != NULL && cJSON_IsBool(ns_item)) {
                mqtt_logger_enable_namespace((mqtt_log_namespace_t)i, cJSON_IsTrue(ns_item));
            }
        }

        /* Build response with current namespace states */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            ESP_LOGE(TAG, "Failed to create response data JSON object");
            cJSON_Delete(json);
            mem_ng_free(payload_str);
            return ESP_ERR_NO_MEM;
        }
        cJSON *namespaces = cJSON_CreateObject();
        if (namespaces == NULL) {
            ESP_LOGE(TAG, "Failed to create namespaces JSON object");
            cJSON_Delete(data);
            cJSON_Delete(json);
            mem_ng_free(payload_str);
            return ESP_ERR_NO_MEM;
        }
        for (int i = 0; i < MQTT_LOG_NS_COUNT; i++) {
            cJSON_AddBoolToObject(namespaces, s_namespace_strings[i],
                                  s_logger.namespaces_enabled[i]);
        }
        cJSON_AddItemToObject(data, "namespaces", namespaces);

        bridge_response_publish_ok(MQTT_LOGGER_NS_RESPONSE, data, txn_id);
        cJSON_Delete(data);
    } else {
        ESP_LOGW(TAG, "Unknown logging request topic: %s", topic);
        cJSON_Delete(json);
        mem_ng_free(payload_str);
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON_Delete(json);
    mem_ng_free(payload_str);
    return ESP_OK;
}

/**
 * @brief Logger publishing task
 */
static void logger_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Logger task started");
    uint32_t backoff_ms = PUBLISH_RATE_LIMIT_MS;
    const uint32_t max_backoff_ms = 5000;  /* Max 5 seconds when MQTT disconnected */

    while (s_logger.running) {
        /* If MQTT not connected, wait longer and drop queued messages to prevent buildup */
        if (!mqtt_client_is_connected()) {
            /* Drain the ring buffer silently when not connected */
            size_t item_size;
            mqtt_log_entry_t *entry = (mqtt_log_entry_t *)xRingbufferReceive(
                s_logger.ring_buffer,
                &item_size,
                0  /* Don't wait */
            );
            if (entry != NULL) {
                vRingbufferReturnItem(s_logger.ring_buffer, entry);
                s_logger.stats.messages_dropped++;
            }

            /* Backoff when disconnected */
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            if (backoff_ms < max_backoff_ms) {
                backoff_ms *= 2;
            }
            continue;
        }

        /* Reset backoff when connected */
        backoff_ms = PUBLISH_RATE_LIMIT_MS;

        /* Receive log entry from ring buffer */
        size_t item_size;
        mqtt_log_entry_t *entry = (mqtt_log_entry_t *)xRingbufferReceive(
            s_logger.ring_buffer,
            &item_size,
            GW_DEFAULT_MUTEX_TIMEOUT_TICKS
        );

        if (entry != NULL && item_size == sizeof(mqtt_log_entry_t)) {
            /* Publish log entry */
            publish_log_entry(entry);

            /* Return item to ring buffer */
            vRingbufferReturnItem(s_logger.ring_buffer, entry);

            /* Rate limit publishing */
            vTaskDelay(pdMS_TO_TICKS(PUBLISH_RATE_LIMIT_MS));
        }
    }

    ESP_LOGI(TAG, "Logger task exiting");
    /* Mark task as self-deleted - resources freed by stop function */
    psram_task_mark_deleted(&s_logger.psram_task);
    vTaskDelete(NULL);
}

/**
 * @brief Publish a log entry to MQTT via event bus
 *
 * Uses EVT_HA_DISCOVERY_PUBLISH with ownership transfer:
 * - topic: strdup'd (MQTT_LOGGER_TOPIC is a macro/constant)
 * - payload_json: heap-allocated by cJSON_PrintUnformatted, ownership transfers
 * - Handler (mqtt_event_handler.c) frees both via free()
 */
static esp_err_t publish_log_entry(const mqtt_log_entry_t *entry)
{
    if (entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check if MQTT is connected - silently return if not.
     * We don't use REQUIRE_MQTT_CONNECTED here because it logs a warning,
     * which would create spam when MQTT is disconnected */
    if (!mqtt_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Set re-entry guard to prevent feedback loop:
     * Publishing generates logs, which would be captured by mqtt_logger_vprintf,
     * which would queue more messages, creating an infinite loop */
    s_logger.publishing = true;

    /* Build JSON message */
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        s_logger.publishing = false;
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(json, "level", mqtt_logger_level_to_str(entry->level));
    cJSON_AddStringToObject(json, "message", entry->message);
    cJSON_AddStringToObject(json, "namespace", mqtt_logger_namespace_to_str(entry->ns));
    cJSON_AddNumberToObject(json, "timestamp", entry->timestamp);

    /* Convert to string - heap allocated, ownership will transfer to handler */
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str == NULL) {
        s_logger.publishing = false;
        return ESP_ERR_NO_MEM;
    }

    /* strdup the topic: MQTT_LOGGER_TOPIC is a macro/constant, handler will free() it */
    char *topic_heap = strdup(MQTT_LOGGER_TOPIC);
    if (topic_heap == NULL) {
        free(json_str);
        s_logger.publishing = false;
        return ESP_ERR_NO_MEM;
    }

    /* Publish via event bus - ownership of topic_heap and json_str transfers to handler */
    evt_ha_discovery_publish_t evt = {
        .topic = topic_heap,
        .payload_json = json_str,
        .qos = 0,
        .retain = false
    };
    esp_err_t ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret != ESP_OK) {
        /* event_publish failed - ownership was NOT transferred, we must free */
        free(topic_heap);
        free(json_str);
    }

    /* Clear re-entry guard */
    s_logger.publishing = false;

    if (ret == ESP_OK) {
        s_logger.stats.messages_published++;
    }

    return ret;
}

/**
 * @brief Check if tag should be excluded from MQTT logging
 *
 * High-volume internal logging (system monitor, task stats) would flood
 * the MQTT queue. These are meant for serial console debugging only.
 */
static bool should_exclude_tag(const char *tag)
{
    if (tag == NULL) {
        return false;
    }

    /* Exclude high-volume diagnostic tags */
    if (strstr(tag, "SYS_MON") != NULL ||
        strstr(tag, "TASK_MGR") != NULL ||
        strstr(tag, "MEM_MGR") != NULL ||
        strstr(tag, "MQTT_LOG") != NULL) {
        return true;
    }

    return false;
}

/**
 * @brief Detect namespace from ESP_LOG tag
 */
static mqtt_log_namespace_t detect_namespace_from_tag(const char *tag)
{
    if (tag == NULL) {
        return MQTT_LOG_NS_SYSTEM;
    }

    /* Check for known tag prefixes/patterns */
    if (strstr(tag, "ZIGBEE") != NULL || strstr(tag, "ZB_") != NULL ||
        strstr(tag, "zb_") != NULL || strstr(tag, "ESP_ZB") != NULL) {
        return MQTT_LOG_NS_ZIGBEE;
    }

    if (strstr(tag, "MQTT") != NULL || strstr(tag, "mqtt") != NULL) {
        return MQTT_LOG_NS_MQTT;
    }

    if (strstr(tag, "WIFI") != NULL || strstr(tag, "wifi") != NULL ||
        strstr(tag, "WiFi") != NULL) {
        return MQTT_LOG_NS_WIFI;
    }

    if (strstr(tag, "BLE") != NULL || strstr(tag, "BT") != NULL ||
        strstr(tag, "ble") != NULL || strstr(tag, "GATT") != NULL) {
        return MQTT_LOG_NS_BLE;
    }

    return MQTT_LOG_NS_SYSTEM;
}

/**
 * @brief Convert ESP log level to MQTT log level
 * @note Used by mqtt_logger_vprintf for ESP_LOG integration
 */
__attribute__((unused))
static mqtt_log_level_t esp_log_level_to_mqtt(esp_log_level_t esp_level)
{
    switch (esp_level) {
        case ESP_LOG_ERROR:
            return MQTT_LOG_ERROR;
        case ESP_LOG_WARN:
            return MQTT_LOG_WARNING;
        case ESP_LOG_INFO:
            return MQTT_LOG_INFO;
        case ESP_LOG_DEBUG:
        case ESP_LOG_VERBOSE:
        default:
            return MQTT_LOG_DEBUG;
    }
}

/**
 * @brief Custom vprintf handler for ESP_LOG integration
 *
 * This function intercepts ESP_LOG output, extracts level and tag,
 * and forwards to MQTT logger while also passing to original handler.
 */
static int mqtt_logger_vprintf(const char *format, va_list args)
{
    /* Always call original handler first */
    int ret = 0;
    if (s_logger.original_vprintf != NULL) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_logger.original_vprintf(format, args_copy);
        va_end(args_copy);
    }

    /* Skip if not initialized or not running */
    if (!s_logger.initialized) {
        return ret;
    }

    /* Skip if we're currently publishing to prevent feedback loop:
     * publish_log_entry() -> mqtt_client_publish() -> ESP_LOGI() -> here
     * Without this guard, each publish generates logs that get queued,
     * which causes more publishes, filling the queue instantly */
    if (s_logger.publishing) {
        return ret;
    }

    /* Parse ESP_LOG format: "[LEVEL] (tag) message"
     * ESP_LOG format typically starts with a single character level indicator
     * followed by space, timestamp in parentheses, then tag and message
     */

    /* Format the message first */
    char msg_buffer[MQTT_PAYLOAD_SMALL];
    va_list args_copy2;
    va_copy(args_copy2, args);
    vsnprintf(msg_buffer, sizeof(msg_buffer), format, args_copy2);
    va_end(args_copy2);

    /* Try to extract level from first character */
    mqtt_log_level_t level = MQTT_LOG_INFO;
    const char *msg_start = msg_buffer;

    if (msg_buffer[0] == 'E') {
        level = MQTT_LOG_ERROR;
    } else if (msg_buffer[0] == 'W') {
        level = MQTT_LOG_WARNING;
    } else if (msg_buffer[0] == 'I') {
        level = MQTT_LOG_INFO;
    } else if (msg_buffer[0] == 'D' || msg_buffer[0] == 'V') {
        level = MQTT_LOG_DEBUG;
    }

    /* Try to extract tag from parentheses */
    mqtt_log_namespace_t ns = MQTT_LOG_NS_SYSTEM;
    char *tag_start = strchr(msg_buffer, '(');
    char *tag_end = strchr(msg_buffer, ')');
    char tag[MQTT_SHORT_STR_MAX_LEN] = {0};

    if (tag_start != NULL && tag_end != NULL && tag_end > tag_start) {
        size_t tag_len = tag_end - tag_start - 1;
        if (tag_len < sizeof(tag)) {
            strncpy(tag, tag_start + 1, tag_len);
            tag[tag_len] = '\0';

            /* Skip high-volume diagnostic tags to prevent queue flooding */
            if (should_exclude_tag(tag)) {
                return ret;
            }

            ns = detect_namespace_from_tag(tag);
        }
        msg_start = tag_end + 1;
        /* Skip leading spaces/colon after tag */
        while (*msg_start == ' ' || *msg_start == ':') {
            msg_start++;
        }
    }

    /* Queue the log message (use the cleaned message portion) */
    if (msg_start[0] != '\0') {
        /* Remove trailing newline if present */
        size_t len = strlen(msg_start);
        char clean_msg[MQTT_PAYLOAD_SMALL];
        strncpy(clean_msg, msg_start, sizeof(clean_msg) - 1);
        clean_msg[sizeof(clean_msg) - 1] = '\0';
        if (len > 0 && clean_msg[len - 1] == '\n') {
            clean_msg[len - 1] = '\0';
        }

        mqtt_logger_log(level, ns, "%s", clean_msg);
    }

    return ret;
}

/**
 * @brief Test MQTT logger functionality
 */
esp_err_t mqtt_logger_test(void)
{
    ESP_LOGI(TAG, "=== MQTT Logger Test ===");

    /* Test level conversion */
    if (strcmp(mqtt_logger_level_to_str(MQTT_LOG_ERROR), "error") != 0) {
        ESP_LOGE(TAG, "Test FAILED: level_to_str(ERROR)");
        return ESP_FAIL;
    }

    if (strcmp(mqtt_logger_level_to_str(MQTT_LOG_WARNING), "warning") != 0) {
        ESP_LOGE(TAG, "Test FAILED: level_to_str(WARNING)");
        return ESP_FAIL;
    }

    if (strcmp(mqtt_logger_level_to_str(MQTT_LOG_INFO), "info") != 0) {
        ESP_LOGE(TAG, "Test FAILED: level_to_str(INFO)");
        return ESP_FAIL;
    }

    if (strcmp(mqtt_logger_level_to_str(MQTT_LOG_DEBUG), "debug") != 0) {
        ESP_LOGE(TAG, "Test FAILED: level_to_str(DEBUG)");
        return ESP_FAIL;
    }

    /* Test string to level conversion */
    if (mqtt_logger_str_to_level("error") != MQTT_LOG_ERROR) {
        ESP_LOGE(TAG, "Test FAILED: str_to_level(error)");
        return ESP_FAIL;
    }

    if (mqtt_logger_str_to_level("DEBUG") != MQTT_LOG_DEBUG) {
        ESP_LOGE(TAG, "Test FAILED: str_to_level(DEBUG) case-insensitive");
        return ESP_FAIL;
    }

    /* Test namespace conversion */
    if (strcmp(mqtt_logger_namespace_to_str(MQTT_LOG_NS_ZIGBEE), "zigbee") != 0) {
        ESP_LOGE(TAG, "Test FAILED: namespace_to_str(ZIGBEE)");
        return ESP_FAIL;
    }

    if (mqtt_logger_str_to_namespace("mqtt") != MQTT_LOG_NS_MQTT) {
        ESP_LOGE(TAG, "Test FAILED: str_to_namespace(mqtt)");
        return ESP_FAIL;
    }

    /* Test statistics */
    mqtt_logger_stats_t stats;
    esp_err_t ret = mqtt_logger_get_stats(&stats);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test FAILED: get_stats");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Stats: logged=%lu, published=%lu, dropped=%lu, filtered=%lu",
             stats.messages_logged, stats.messages_published,
             stats.messages_dropped, stats.messages_filtered);

    ESP_LOGI(TAG, "Test PASSED");
    return ESP_OK;
}
