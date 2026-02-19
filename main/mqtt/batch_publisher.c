/**
 * @file batch_publisher.c
 * @brief Batch MQTT Publisher Implementation
 *
 * Implements efficient batched MQTT publishing using a ring buffer queue
 * and esp_timer for auto-flush functionality.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "batch_publisher.h"
#include "gateway_mqtt.h"
#include "core/memory/memory_manager_ng.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "BATCH_PUB";

/* ============================================================================
 * Internal Data Structures
 * ============================================================================ */

/**
 * @brief Queued message structure
 */
typedef struct {
    char topic[BATCH_PUBLISHER_MAX_TOPIC_LEN];      /**< MQTT topic */
    char *payload;                                   /**< Message payload (heap allocated) */
    size_t payload_len;                              /**< Payload length */
    int qos;                                         /**< QoS level */
    bool retain;                                     /**< Retain flag */
    bool valid;                                      /**< Slot contains valid message */
} batch_message_t;

/**
 * @brief Batch publisher state
 */
static struct {
    batch_message_t *queue;             /**< Ring buffer queue */
    size_t head;                        /**< Write index */
    size_t tail;                        /**< Read index */
    size_t count;                       /**< Current message count */
    size_t max_batch;                   /**< Max messages before auto-flush */
    uint32_t flush_interval_ms;         /**< Flush interval in ms */
    esp_timer_handle_t flush_timer;     /**< Auto-flush timer */
    SemaphoreHandle_t mutex;            /**< Thread safety mutex */
    bool initialized;                   /**< Initialization flag */
    bool timer_running;                 /**< Timer active flag */
    batch_publisher_stats_t stats;      /**< Statistics */
} s_batch = {
    .queue = NULL,
    .head = 0,
    .tail = 0,
    .count = 0,
    .max_batch = BATCH_PUBLISHER_DEFAULT_MAX_BATCH,
    .flush_interval_ms = BATCH_PUBLISHER_DEFAULT_FLUSH_INTERVAL,
    .flush_timer = NULL,
    .mutex = NULL,
    .initialized = false,
    .timer_running = false,
    .stats = {0}
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void flush_timer_callback(void *arg);
static esp_err_t flush_queue_locked(void);
static void start_timer_if_needed(void);
static void stop_timer(void);
static void free_message(batch_message_t *msg);

/* ============================================================================
 * Timer Callback
 * ============================================================================ */

/**
 * @brief Timer callback for auto-flush
 *
 * Called when flush_interval expires. Flushes any pending messages.
 */
static void flush_timer_callback(void *arg)
{
    (void)arg;

    if (!s_batch.initialized) {
        return;
    }

    if (xSemaphoreTake(s_batch.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_batch.timer_running = false;

        if (s_batch.count > 0) {
            ESP_LOGD(TAG, "Timer flush: %zu messages", s_batch.count);
            flush_queue_locked();
        }

        xSemaphoreGive(s_batch.mutex);
    } else {
        ESP_LOGW(TAG, "Timer callback: failed to acquire mutex");
    }
}

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * @brief Start the flush timer if not already running
 *
 * Must be called with mutex held.
 */
static void start_timer_if_needed(void)
{
    if (!s_batch.timer_running && s_batch.flush_timer != NULL) {
        esp_err_t ret = esp_timer_start_once(s_batch.flush_timer,
                                              s_batch.flush_interval_ms * 1000);
        if (ret == ESP_OK) {
            s_batch.timer_running = true;
        } else {
            ESP_LOGW(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        }
    }
}

/**
 * @brief Stop the flush timer
 *
 * Must be called with mutex held.
 */
static void stop_timer(void)
{
    if (s_batch.timer_running && s_batch.flush_timer != NULL) {
        esp_timer_stop(s_batch.flush_timer);
        s_batch.timer_running = false;
    }
}

/**
 * @brief Free message payload memory
 */
static void free_message(batch_message_t *msg)
{
    if (msg != NULL && msg->payload != NULL) {
        mem_ng_free(msg->payload);
        msg->payload = NULL;
    }
    if (msg != NULL) {
        msg->valid = false;
        msg->payload_len = 0;
    }
}

/**
 * @brief Flush all queued messages (must be called with mutex held)
 *
 * NOTE: We keep the mutex held during publish to avoid priority inheritance
 * issues in ESP-IDF v6.0. The mutex release/re-acquire pattern caused
 * vTaskPriorityDisinheritAfterTimeout assertion failures. MQTT publish
 * just enqueues to a buffer and is fast enough to hold the mutex.
 *
 * @return ESP_OK if all messages sent, ESP_FAIL if some failed
 */
static esp_err_t flush_queue_locked(void)
{
    if (s_batch.count == 0) {
        return ESP_OK;
    }

    /* Check MQTT connection before attempting flush */
    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected, keeping %zu messages queued", s_batch.count);
        /* Restart timer to try again later */
        start_timer_if_needed();
        return ESP_ERR_INVALID_STATE;
    }

    size_t sent = 0;
    size_t failed = 0;
    size_t to_send = s_batch.count;

    ESP_LOGD(TAG, "Flushing %zu messages", to_send);

    for (size_t i = 0; i < to_send; i++) {
        batch_message_t *msg = &s_batch.queue[s_batch.tail];

        if (!msg->valid) {
            /* Skip invalid slots */
            s_batch.tail = (s_batch.tail + 1) % BATCH_PUBLISHER_QUEUE_CAPACITY;
            s_batch.count--;
            continue;
        }

        /* Keep mutex held during publish - releasing causes priority inheritance
         * issues in ESP-IDF v6.0 with vTaskPriorityDisinheritAfterTimeout */
        esp_err_t ret = mqtt_client_publish(msg->topic, msg->payload,
                                            msg->payload_len, msg->qos, msg->retain);

        if (ret == ESP_OK) {
            sent++;
            s_batch.stats.messages_sent++;
            ESP_LOGD(TAG, "Sent: %s", msg->topic);
        } else {
            failed++;
            ESP_LOGW(TAG, "Failed to send: %s (%s)", msg->topic, esp_err_to_name(ret));
        }

        /* Free message regardless of success (don't retry) */
        free_message(msg);

        /* Advance tail */
        s_batch.tail = (s_batch.tail + 1) % BATCH_PUBLISHER_QUEUE_CAPACITY;
        s_batch.count--;
    }

    s_batch.stats.batches_sent++;

    /* Stop timer since queue is empty */
    stop_timer();

    ESP_LOGI(TAG, "Batch flush: %zu sent, %zu failed", sent, failed);

    return (failed > 0) ? ESP_FAIL : ESP_OK;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t batch_publisher_init(void)
{
    if (s_batch.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing batch publisher...");

    /* Create mutex */
    s_batch.mutex = xSemaphoreCreateMutex();
    if (s_batch.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate queue (auto-PSRAM for large allocations) */
    s_batch.queue = mem_ng_calloc(BATCH_PUBLISHER_QUEUE_CAPACITY,
                                   sizeof(batch_message_t),
                                   MEM_CAP_PSRAM);
    if (s_batch.queue == NULL) {
        ESP_LOGE(TAG, "Failed to allocate queue");
        vSemaphoreDelete(s_batch.mutex);
        s_batch.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Create flush timer */
    esp_timer_create_args_t timer_args = {
        .callback = flush_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "batch_flush"
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_batch.flush_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        mem_ng_free(s_batch.queue);
        s_batch.queue = NULL;
        vSemaphoreDelete(s_batch.mutex);
        s_batch.mutex = NULL;
        return ret;
    }

    /* Initialize state */
    s_batch.head = 0;
    s_batch.tail = 0;
    s_batch.count = 0;
    s_batch.max_batch = BATCH_PUBLISHER_DEFAULT_MAX_BATCH;
    s_batch.flush_interval_ms = BATCH_PUBLISHER_DEFAULT_FLUSH_INTERVAL;
    s_batch.timer_running = false;
    memset(&s_batch.stats, 0, sizeof(batch_publisher_stats_t));

    s_batch.initialized = true;

    ESP_LOGI(TAG, "Batch publisher initialized (max_batch=%zu, interval=%lums)",
             s_batch.max_batch, s_batch.flush_interval_ms);

    return ESP_OK;
}

esp_err_t batch_publisher_deinit(void)
{
    if (!s_batch.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing batch publisher...");

    /* Acquire mutex and flush remaining messages */
    if (xSemaphoreTake(s_batch.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        /* Try to flush any pending messages */
        if (s_batch.count > 0 && mqtt_client_is_connected()) {
            ESP_LOGI(TAG, "Flushing %zu remaining messages...", s_batch.count);
            flush_queue_locked();
        }

        /* Stop timer */
        stop_timer();

        /* Free any remaining messages */
        for (size_t i = 0; i < BATCH_PUBLISHER_QUEUE_CAPACITY; i++) {
            free_message(&s_batch.queue[i]);
        }

        s_batch.initialized = false;

        xSemaphoreGive(s_batch.mutex);
    }

    /* Delete timer */
    if (s_batch.flush_timer != NULL) {
        esp_timer_delete(s_batch.flush_timer);
        s_batch.flush_timer = NULL;
    }

    /* Free queue */
    if (s_batch.queue != NULL) {
        mem_ng_free(s_batch.queue);
        s_batch.queue = NULL;
    }

    /* Delete mutex */
    if (s_batch.mutex != NULL) {
        vSemaphoreDelete(s_batch.mutex);
        s_batch.mutex = NULL;
    }

    ESP_LOGI(TAG, "Batch publisher deinitialized");

    return ESP_OK;
}

esp_err_t batch_publisher_queue(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_batch.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (topic == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);

    if (topic_len >= BATCH_PUBLISHER_MAX_TOPIC_LEN) {
        ESP_LOGW(TAG, "Topic too long: %zu (max %d)", topic_len, BATCH_PUBLISHER_MAX_TOPIC_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    if (payload_len > BATCH_PUBLISHER_MAX_PAYLOAD_LEN) {
        ESP_LOGW(TAG, "Payload too long: %zu (max %d)", payload_len, BATCH_PUBLISHER_MAX_PAYLOAD_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_batch.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex");
        return ESP_ERR_TIMEOUT;
    }

    /* Check if queue is full */
    if (s_batch.count >= BATCH_PUBLISHER_QUEUE_CAPACITY) {
        /* Drop oldest message to make room */
        ESP_LOGW(TAG, "Queue full, dropping oldest message");
        free_message(&s_batch.queue[s_batch.tail]);
        s_batch.tail = (s_batch.tail + 1) % BATCH_PUBLISHER_QUEUE_CAPACITY;
        s_batch.count--;
        s_batch.stats.queue_overflows++;
    }

    /* Allocate payload in PSRAM if available */
    batch_message_t *msg = &s_batch.queue[s_batch.head];

    /* Free any existing payload (shouldn't happen, but be safe) */
    if (msg->payload != NULL) {
        mem_ng_free(msg->payload);
    }

    msg->payload = mem_alloc(payload_len + 1, MEM_CAP_PSRAM);
    if (msg->payload == NULL) {
        ESP_LOGE(TAG, "Failed to allocate payload buffer");
        xSemaphoreGive(s_batch.mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Copy message data */
    strncpy(msg->topic, topic, BATCH_PUBLISHER_MAX_TOPIC_LEN - 1);
    msg->topic[BATCH_PUBLISHER_MAX_TOPIC_LEN - 1] = '\0';
    memcpy(msg->payload, payload, payload_len);
    msg->payload[payload_len] = '\0';
    msg->payload_len = payload_len;
    msg->qos = qos;
    msg->retain = retain;
    msg->valid = true;

    /* Advance head */
    s_batch.head = (s_batch.head + 1) % BATCH_PUBLISHER_QUEUE_CAPACITY;
    s_batch.count++;
    s_batch.stats.messages_queued++;

    ESP_LOGD(TAG, "Queued: %s (%zu bytes, %zu in queue)", topic, payload_len, s_batch.count);

    /* Check if we should flush immediately due to batch size */
    if (s_batch.count >= s_batch.max_batch) {
        ESP_LOGD(TAG, "Max batch reached, flushing");
        flush_queue_locked();
    } else {
        /* Start timer if not already running */
        start_timer_if_needed();
    }

    xSemaphoreGive(s_batch.mutex);

    return ESP_OK;
}

esp_err_t batch_publisher_flush(void)
{
    if (!s_batch.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_batch.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for flush");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = flush_queue_locked();

    xSemaphoreGive(s_batch.mutex);

    return ret;
}

void batch_publisher_set_max_batch(size_t max_messages)
{
    if (!s_batch.initialized) {
        return;
    }

    /* Clamp to valid range */
    if (max_messages < 1) {
        max_messages = 1;
    } else if (max_messages > BATCH_PUBLISHER_QUEUE_CAPACITY) {
        max_messages = BATCH_PUBLISHER_QUEUE_CAPACITY;
    }

    if (xSemaphoreTake(s_batch.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_batch.max_batch = max_messages;
        ESP_LOGI(TAG, "Max batch set to %zu", max_messages);

        /* Check if current queue count exceeds new max */
        if (s_batch.count >= s_batch.max_batch) {
            flush_queue_locked();
        }

        xSemaphoreGive(s_batch.mutex);
    }
}

void batch_publisher_set_flush_interval(uint32_t ms)
{
    if (!s_batch.initialized) {
        return;
    }

    /* Minimum 10ms interval */
    if (ms < 10) {
        ms = 10;
    }

    if (xSemaphoreTake(s_batch.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_batch.flush_interval_ms = ms;
        ESP_LOGI(TAG, "Flush interval set to %lu ms", ms);

        /* Restart timer if running with new interval */
        if (s_batch.timer_running) {
            stop_timer();
            if (s_batch.count > 0) {
                start_timer_if_needed();
            }
        }

        xSemaphoreGive(s_batch.mutex);
    }
}

void batch_publisher_get_stats(batch_publisher_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    if (!s_batch.initialized) {
        memset(stats, 0, sizeof(batch_publisher_stats_t));
        return;
    }

    if (xSemaphoreTake(s_batch.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *stats = s_batch.stats;
        xSemaphoreGive(s_batch.mutex);
    } else {
        memset(stats, 0, sizeof(batch_publisher_stats_t));
    }
}

void batch_publisher_reset_stats(void)
{
    if (!s_batch.initialized) {
        return;
    }

    if (xSemaphoreTake(s_batch.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(&s_batch.stats, 0, sizeof(batch_publisher_stats_t));
        ESP_LOGI(TAG, "Statistics reset");
        xSemaphoreGive(s_batch.mutex);
    }
}

size_t batch_publisher_queue_count(void)
{
    if (!s_batch.initialized) {
        return 0;
    }

    size_t count = 0;

    if (xSemaphoreTake(s_batch.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        count = s_batch.count;
        xSemaphoreGive(s_batch.mutex);
    }

    return count;
}

bool batch_publisher_is_initialized(void)
{
    return s_batch.initialized;
}
