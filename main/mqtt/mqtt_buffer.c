/**
 * @file mqtt_buffer.c
 * @brief MQTT Message Buffer Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "mqtt_buffer.h"
#include "gateway_mqtt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "core/memory/memory_manager_ng.h"
#include <string.h>

static const char *TAG = "MQTT_BUF";

/* Buffer state */
static struct {
    mqtt_buffered_msg_t *messages;      /**< Message array in PSRAM */
    size_t head;                        /**< Write index */
    size_t tail;                        /**< Read index */
    size_t count;                       /**< Current message count */
    SemaphoreHandle_t mutex;            /**< Thread safety mutex */
    bool initialized;                   /**< Initialization flag */
    bool buffering_enabled;             /**< Buffering mode active */
    mqtt_buffer_stats_t stats;          /**< Statistics */
} s_buffer = {
    .messages = NULL,
    .head = 0,
    .tail = 0,
    .count = 0,
    .mutex = NULL,
    .initialized = false,
    .buffering_enabled = false,
    .stats = {0}
};

esp_err_t mqtt_buffer_init(void)
{
    if (s_buffer.initialized) {
        ESP_LOGW(TAG, "Buffer already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create mutex */
    s_buffer.mutex = xSemaphoreCreateMutex();
    if (s_buffer.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate message array in PSRAM */
    s_buffer.messages = mem_ng_calloc(MQTT_BUFFER_MAX_MESSAGES,
                                      sizeof(mqtt_buffered_msg_t),
                                      MEM_CAP_PSRAM);
    if (s_buffer.messages == NULL) {
        ESP_LOGE(TAG, "Failed to allocate message buffer in PSRAM");
        vSemaphoreDelete(s_buffer.mutex);
        s_buffer.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Allocate payload buffers in PSRAM for each slot */
    for (size_t i = 0; i < MQTT_BUFFER_MAX_MESSAGES; i++) {
        s_buffer.messages[i].payload = mem_alloc(MQTT_BUFFER_MAX_PAYLOAD_LEN,
                                                 MEM_CAP_PSRAM);
        if (s_buffer.messages[i].payload == NULL) {
            ESP_LOGE(TAG, "Failed to allocate payload buffer %zu", i);
            /* Free already allocated */
            for (size_t j = 0; j < i; j++) {
                mem_ng_free(s_buffer.messages[j].payload);
            }
            mem_ng_free(s_buffer.messages);
            s_buffer.messages = NULL;
            vSemaphoreDelete(s_buffer.mutex);
            s_buffer.mutex = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    s_buffer.head = 0;
    s_buffer.tail = 0;
    s_buffer.count = 0;
    s_buffer.initialized = true;
    s_buffer.buffering_enabled = false;
    memset(&s_buffer.stats, 0, sizeof(s_buffer.stats));

    ESP_LOGI(TAG, "MQTT buffer initialized (%d slots, ~%d KB PSRAM)",
             MQTT_BUFFER_MAX_MESSAGES,
             (MQTT_BUFFER_MAX_MESSAGES * (MQTT_BUFFER_MAX_PAYLOAD_LEN + sizeof(mqtt_buffered_msg_t))) / 1024);

    return ESP_OK;
}

void mqtt_buffer_deinit(void)
{
    if (!s_buffer.initialized) {
        return;
    }

    xSemaphoreTake(s_buffer.mutex, GW_TIMEOUT_LONG_TICKS);

    /* Free payload buffers */
    if (s_buffer.messages != NULL) {
        for (size_t i = 0; i < MQTT_BUFFER_MAX_MESSAGES; i++) {
            if (s_buffer.messages[i].payload != NULL) {
                mem_ng_free(s_buffer.messages[i].payload);
            }
        }
        mem_ng_free(s_buffer.messages);
        s_buffer.messages = NULL;
    }

    s_buffer.initialized = false;

    xSemaphoreGive(s_buffer.mutex);
    vSemaphoreDelete(s_buffer.mutex);
    s_buffer.mutex = NULL;

    ESP_LOGI(TAG, "MQTT buffer deinitialized");
}

esp_err_t mqtt_buffer_add(const char *topic, const char *payload,
                          size_t payload_len, int qos, bool retain)
{
    if (!s_buffer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (topic == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check limits */
    size_t topic_len = strlen(topic);
    if (topic_len >= MQTT_BUFFER_MAX_TOPIC_LEN) {
        ESP_LOGW(TAG, "Topic too long: %zu (max %d)", topic_len, MQTT_BUFFER_MAX_TOPIC_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    if (payload_len > MQTT_BUFFER_MAX_PAYLOAD_LEN) {
        ESP_LOGW(TAG, "Payload too long: %zu (max %d)", payload_len, MQTT_BUFFER_MAX_PAYLOAD_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_buffer.mutex, GW_TIMEOUT_LONG_TICKS);

    /* Check if buffer is full */
    if (s_buffer.count >= MQTT_BUFFER_MAX_MESSAGES) {
        /* Drop oldest message */
        ESP_LOGW(TAG, "Buffer full, dropping oldest message");
        s_buffer.tail = (s_buffer.tail + 1) % MQTT_BUFFER_MAX_MESSAGES;
        s_buffer.count--;
        s_buffer.stats.messages_dropped++;
    }

    /* Store message */
    mqtt_buffered_msg_t *msg = &s_buffer.messages[s_buffer.head];
    strncpy(msg->topic, topic, MQTT_BUFFER_MAX_TOPIC_LEN - 1);
    msg->topic[MQTT_BUFFER_MAX_TOPIC_LEN - 1] = '\0';
    memcpy(msg->payload, payload, payload_len);
    msg->payload_len = payload_len;
    msg->qos = qos;
    msg->retain = retain;
    msg->timestamp = esp_timer_get_time();

    /* Advance head */
    s_buffer.head = (s_buffer.head + 1) % MQTT_BUFFER_MAX_MESSAGES;
    s_buffer.count++;
    s_buffer.stats.messages_buffered++;

    /* Track peak usage */
    if (s_buffer.count > s_buffer.stats.peak_count) {
        s_buffer.stats.peak_count = s_buffer.count;
    }

    s_buffer.stats.current_count = s_buffer.count;

    ESP_LOGD(TAG, "Buffered message to %s (%zu bytes, %zu in queue)",
             topic, payload_len, s_buffer.count);

    xSemaphoreGive(s_buffer.mutex);

    return ESP_OK;
}

esp_err_t mqtt_buffer_flush(void)
{
    if (!s_buffer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!mqtt_client_is_connected()) {
        ESP_LOGW(TAG, "Cannot flush - MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_buffer.mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_buffer.count == 0) {
        xSemaphoreGive(s_buffer.mutex);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Flushing %zu buffered messages...", s_buffer.count);

    size_t flushed = 0;
    size_t failed = 0;
    size_t to_flush = s_buffer.count;

    for (size_t i = 0; i < to_flush; i++) {
        mqtt_buffered_msg_t *msg = &s_buffer.messages[s_buffer.tail];

        /* Calculate message age */
        int64_t age_ms = (esp_timer_get_time() - msg->timestamp) / 1000;

        /* Release mutex during publish to avoid blocking */
        xSemaphoreGive(s_buffer.mutex);

        esp_err_t ret = mqtt_client_publish(msg->topic, msg->payload,
                                            msg->payload_len, msg->qos, msg->retain);

        xSemaphoreTake(s_buffer.mutex, GW_TIMEOUT_LONG_TICKS);

        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "Flushed: %s (age: %lld ms)", msg->topic, age_ms);
            flushed++;
            s_buffer.stats.messages_flushed++;
        } else {
            ESP_LOGW(TAG, "Failed to flush: %s (%s)", msg->topic, esp_err_to_name(ret));
            failed++;
            s_buffer.stats.flush_failures++;
        }

        /* Advance tail regardless of success (don't retry failed messages) */
        s_buffer.tail = (s_buffer.tail + 1) % MQTT_BUFFER_MAX_MESSAGES;
        s_buffer.count--;

        /* Small delay between messages to avoid overwhelming broker */
        if (i < to_flush - 1) {
            xSemaphoreGive(s_buffer.mutex);
            vTaskDelay(pdMS_TO_TICKS(10));
            xSemaphoreTake(s_buffer.mutex, GW_TIMEOUT_LONG_TICKS);
        }
    }

    s_buffer.stats.current_count = s_buffer.count;

    ESP_LOGI(TAG, "Flush complete: %zu sent, %zu failed", flushed, failed);

    xSemaphoreGive(s_buffer.mutex);

    return (failed > 0) ? ESP_FAIL : ESP_OK;
}

size_t mqtt_buffer_count(void)
{
    if (!s_buffer.initialized) {
        return 0;
    }

    xSemaphoreTake(s_buffer.mutex, GW_TIMEOUT_LONG_TICKS);
    size_t count = s_buffer.count;
    xSemaphoreGive(s_buffer.mutex);

    return count;
}

bool mqtt_buffer_is_empty(void)
{
    return mqtt_buffer_count() == 0;
}

void mqtt_buffer_clear(void)
{
    if (!s_buffer.initialized) {
        return;
    }

    xSemaphoreTake(s_buffer.mutex, GW_TIMEOUT_LONG_TICKS);

    s_buffer.head = 0;
    s_buffer.tail = 0;
    s_buffer.count = 0;
    s_buffer.stats.current_count = 0;

    ESP_LOGI(TAG, "Buffer cleared");

    xSemaphoreGive(s_buffer.mutex);
}

esp_err_t mqtt_buffer_get_stats(mqtt_buffer_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_buffer.initialized) {
        memset(stats, 0, sizeof(mqtt_buffer_stats_t));
        return ESP_OK;
    }

    xSemaphoreTake(s_buffer.mutex, GW_TIMEOUT_LONG_TICKS);
    *stats = s_buffer.stats;
    xSemaphoreGive(s_buffer.mutex);

    return ESP_OK;
}

bool mqtt_buffer_should_buffer(void)
{
    if (!s_buffer.initialized || s_buffer.mutex == NULL) {
        return false;
    }

    /* Use mutex to safely read buffering_enabled flag */
    bool should_buffer = false;
    if (xSemaphoreTake(s_buffer.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        should_buffer = s_buffer.buffering_enabled && !mqtt_client_is_connected();
        xSemaphoreGive(s_buffer.mutex);
    }
    /* If mutex not acquired within 10ms, assume no buffering to avoid blocking */

    return should_buffer;
}

void mqtt_buffer_set_enabled(bool enable)
{
    if (!s_buffer.initialized) {
        return;
    }

    xSemaphoreTake(s_buffer.mutex, GW_TIMEOUT_LONG_TICKS);
    bool was_enabled = s_buffer.buffering_enabled;
    s_buffer.buffering_enabled = enable;
    xSemaphoreGive(s_buffer.mutex);

    if (enable && !was_enabled) {
        ESP_LOGI(TAG, "Buffering enabled");
    } else if (!enable && was_enabled) {
        ESP_LOGI(TAG, "Buffering disabled");
    }
}
