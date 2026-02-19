/**
 * @file mqtt_buffer.h
 * @brief MQTT Message Buffer for Offline Queuing
 *
 * Provides PSRAM-based message buffering when MQTT is unavailable
 * (e.g., during Zigbee pairing when WiFi is disconnected).
 * Messages are automatically flushed when MQTT reconnects.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MQTT_BUFFER_H
#define MQTT_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MQTT Buffer Constants
 * ============================================================================ */

/** @brief Maximum number of buffered messages */
#define MQTT_BUFFER_MAX_MESSAGES        50

/** @brief Maximum topic length */
#define MQTT_BUFFER_MAX_TOPIC_LEN       128

/** @brief Maximum payload length per message */
#define MQTT_BUFFER_MAX_PAYLOAD_LEN     2048

/** @brief Total buffer size in PSRAM (approx 100KB for 50 messages) */
#define MQTT_BUFFER_TOTAL_SIZE          (MQTT_BUFFER_MAX_MESSAGES * \
                                         (MQTT_BUFFER_MAX_TOPIC_LEN + MQTT_BUFFER_MAX_PAYLOAD_LEN + 16))

/**
 * @brief Buffered message structure
 */
typedef struct {
    char topic[MQTT_BUFFER_MAX_TOPIC_LEN];   /**< MQTT topic */
    char *payload;                            /**< Message payload (in PSRAM) */
    size_t payload_len;                       /**< Payload length */
    int qos;                                  /**< QoS level */
    bool retain;                              /**< Retain flag */
    int64_t timestamp;                        /**< Timestamp when buffered (us) */
} mqtt_buffered_msg_t;

/**
 * @brief Buffer statistics
 */
typedef struct {
    uint32_t messages_buffered;     /**< Total messages buffered */
    uint32_t messages_flushed;      /**< Total messages successfully flushed */
    uint32_t messages_dropped;      /**< Messages dropped (buffer full) */
    uint32_t flush_failures;        /**< Failed flush attempts */
    size_t current_count;           /**< Current messages in buffer */
    size_t peak_count;              /**< Peak buffer usage */
} mqtt_buffer_stats_t;

/**
 * @brief Initialize MQTT message buffer
 *
 * Allocates PSRAM for message storage. Must be called before using buffer.
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_NO_MEM: Failed to allocate PSRAM
 *     - ESP_ERR_INVALID_STATE: Already initialized
 */
esp_err_t mqtt_buffer_init(void);

/**
 * @brief Deinitialize MQTT message buffer
 *
 * Frees all allocated memory. Buffered messages are lost.
 */
void mqtt_buffer_deinit(void);

/**
 * @brief Add message to buffer
 *
 * Stores a message for later transmission when MQTT reconnects.
 * If buffer is full, oldest message is dropped.
 *
 * @param[in] topic MQTT topic
 * @param[in] payload Message payload
 * @param[in] payload_len Payload length
 * @param[in] qos QoS level
 * @param[in] retain Retain flag
 *
 * @return
 *     - ESP_OK: Message buffered successfully
 *     - ESP_ERR_NO_MEM: Buffer full, message dropped
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_INVALID_STATE: Buffer not initialized
 */
esp_err_t mqtt_buffer_add(const char *topic, const char *payload,
                          size_t payload_len, int qos, bool retain);

/**
 * @brief Flush all buffered messages to MQTT
 *
 * Sends all buffered messages to the MQTT broker.
 * Should be called when MQTT reconnects.
 *
 * @return
 *     - ESP_OK: All messages flushed successfully
 *     - ESP_ERR_INVALID_STATE: MQTT not connected or buffer not initialized
 *     - ESP_FAIL: Some messages failed to send
 */
esp_err_t mqtt_buffer_flush(void);

/**
 * @brief Get number of buffered messages
 *
 * @return Number of messages currently in buffer
 */
size_t mqtt_buffer_count(void);

/**
 * @brief Check if buffer is empty
 *
 * @return true if no messages buffered
 */
bool mqtt_buffer_is_empty(void);

/**
 * @brief Clear all buffered messages
 *
 * Discards all messages without sending.
 */
void mqtt_buffer_clear(void);

/**
 * @brief Get buffer statistics
 *
 * @param[out] stats Pointer to statistics structure
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid stats pointer
 */
esp_err_t mqtt_buffer_get_stats(mqtt_buffer_stats_t *stats);

/**
 * @brief Check if buffering is enabled
 *
 * Buffering is enabled when MQTT is not connected.
 *
 * @return true if messages should be buffered
 */
bool mqtt_buffer_should_buffer(void);

/**
 * @brief Enable or disable buffering mode
 *
 * When enabled, publish attempts go to buffer instead of failing.
 * Automatically enabled when entering PAIRING phase.
 *
 * @param[in] enable true to enable buffering
 */
void mqtt_buffer_set_enabled(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_BUFFER_H */
