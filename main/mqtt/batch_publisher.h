/**
 * @file batch_publisher.h
 * @brief Batch MQTT Publisher for Efficient Bulk Publishing
 *
 * Provides batched MQTT publishing to reduce overhead when many devices
 * update simultaneously (e.g., after reconnect, bulk discovery).
 * Messages are queued and sent in batches based on count or time interval.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BATCH_PUBLISHER_H
#define BATCH_PUBLISHER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Batch Publisher Constants
 * ============================================================================ */

/** @brief Default maximum messages per batch */
#define BATCH_PUBLISHER_DEFAULT_MAX_BATCH       10

/** @brief Default flush interval in milliseconds */
#define BATCH_PUBLISHER_DEFAULT_FLUSH_INTERVAL  100

/** @brief Maximum queue capacity (ring buffer size) */
#define BATCH_PUBLISHER_QUEUE_CAPACITY          32

/** @brief Maximum topic length for queued messages */
#define BATCH_PUBLISHER_MAX_TOPIC_LEN           128

/** @brief Maximum payload length for queued messages */
#define BATCH_PUBLISHER_MAX_PAYLOAD_LEN         2048

/**
 * @brief Batch publisher statistics
 */
typedef struct {
    size_t messages_queued;     /**< Total messages queued for batching */
    size_t messages_sent;       /**< Total messages successfully sent */
    size_t batches_sent;        /**< Total batches flushed */
    size_t queue_overflows;     /**< Messages dropped due to full queue */
} batch_publisher_stats_t;

/**
 * @brief Initialize batch publisher
 *
 * Allocates resources and starts the auto-flush timer.
 * Must be called before any other batch publisher operations.
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_NO_MEM: Failed to allocate resources
 *     - ESP_ERR_INVALID_STATE: Already initialized
 */
esp_err_t batch_publisher_init(void);

/**
 * @brief Deinitialize batch publisher
 *
 * Flushes any pending messages and releases all resources.
 * Safe to call even if not initialized.
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_FAIL: Error during cleanup
 */
esp_err_t batch_publisher_deinit(void);

/**
 * @brief Queue a message for batched publishing
 *
 * Adds message to the batch queue. The message will be sent when:
 * - max_batch count is reached, OR
 * - flush_interval timer expires, OR
 * - batch_publisher_flush() is called explicitly
 *
 * If the queue is full, the oldest message is dropped.
 *
 * @param[in] topic MQTT topic (null-terminated)
 * @param[in] payload Message payload (null-terminated)
 * @param[in] qos QoS level (0, 1, or 2)
 * @param[in] retain Retain flag
 *
 * @return
 *     - ESP_OK: Message queued successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments (NULL topic/payload)
 *     - ESP_ERR_INVALID_STATE: Publisher not initialized
 */
esp_err_t batch_publisher_queue(const char *topic, const char *payload, int qos, bool retain);

/**
 * @brief Flush all queued messages immediately
 *
 * Sends all pending messages in the queue to MQTT.
 * Resets the flush timer after completion.
 *
 * @return
 *     - ESP_OK: All messages sent successfully
 *     - ESP_ERR_INVALID_STATE: Publisher not initialized or MQTT not connected
 *     - ESP_FAIL: Some messages failed to send
 */
esp_err_t batch_publisher_flush(void);

/**
 * @brief Set maximum batch size
 *
 * When this many messages are queued, an automatic flush is triggered.
 * Default is BATCH_PUBLISHER_DEFAULT_MAX_BATCH (10).
 *
 * @param[in] max_messages Maximum messages before auto-flush (1-QUEUE_CAPACITY)
 */
void batch_publisher_set_max_batch(size_t max_messages);

/**
 * @brief Set flush interval
 *
 * Timer triggers automatic flush after this interval if any messages
 * are pending. Default is BATCH_PUBLISHER_DEFAULT_FLUSH_INTERVAL (100ms).
 *
 * @param[in] ms Flush interval in milliseconds (minimum 10ms)
 */
void batch_publisher_set_flush_interval(uint32_t ms);

/**
 * @brief Get batch publisher statistics
 *
 * Retrieves current statistics about batching performance.
 *
 * @param[out] stats Pointer to statistics structure to fill
 */
void batch_publisher_get_stats(batch_publisher_stats_t *stats);

/**
 * @brief Reset batch publisher statistics
 *
 * Clears all counters to zero.
 */
void batch_publisher_reset_stats(void);

/**
 * @brief Get current queue count
 *
 * @return Number of messages currently in the queue
 */
size_t batch_publisher_queue_count(void);

/**
 * @brief Check if batch publisher is initialized
 *
 * @return true if initialized, false otherwise
 */
bool batch_publisher_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* BATCH_PUBLISHER_H */
