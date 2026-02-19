/**
 * @file mock_mqtt.h
 * @brief Mock MQTT Client for Testing
 *
 * Provides a mock MQTT client that simulates MQTT operations
 * without requiring an actual broker connection.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MOCK_MQTT_H
#define MOCK_MQTT_H

#include "esp_err.h"
#include "mqtt_client.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mock MQTT message structure
 */
typedef struct {
    char topic[128];
    char payload[512];
    size_t payload_len;
    int qos;
    bool retained;
} mock_mqtt_message_t;

/**
 * @brief Mock MQTT statistics
 */
typedef struct {
    uint32_t publish_count;
    uint32_t subscribe_count;
    uint32_t unsubscribe_count;
    uint32_t connect_count;
    uint32_t disconnect_count;
    bool connected;
} mock_mqtt_stats_t;

/**
 * @brief Initialize mock MQTT client
 *
 * @return ESP_OK on success
 */
esp_err_t mock_mqtt_init(void);

/**
 * @brief Deinitialize mock MQTT client
 *
 * @return ESP_OK on success
 */
esp_err_t mock_mqtt_deinit(void);

/**
 * @brief Simulate MQTT connection
 *
 * @param[in] connected Connection status
 * @return ESP_OK on success
 */
esp_err_t mock_mqtt_set_connected(bool connected);

/**
 * @brief Get mock MQTT connection status
 *
 * @return true if connected, false otherwise
 */
bool mock_mqtt_is_connected(void);

/**
 * @brief Simulate publishing a message
 *
 * Stores the message internally for verification.
 *
 * @param[in] topic MQTT topic
 * @param[in] payload Message payload
 * @param[in] len Payload length
 * @param[in] qos QoS level
 * @param[in] retain Retain flag
 * @return Message ID (always > 0 on success)
 */
int mock_mqtt_publish(const char *topic, const char *payload, size_t len, int qos, bool retain);

/**
 * @brief Simulate subscribing to a topic
 *
 * @param[in] topic MQTT topic pattern
 * @param[in] qos QoS level
 * @return Message ID (always > 0 on success)
 */
int mock_mqtt_subscribe(const char *topic, int qos);

/**
 * @brief Simulate unsubscribing from a topic
 *
 * @param[in] topic MQTT topic pattern
 * @return Message ID (always > 0 on success)
 */
int mock_mqtt_unsubscribe(const char *topic);

/**
 * @brief Simulate receiving a message
 *
 * Triggers the registered message callback.
 *
 * @param[in] topic MQTT topic
 * @param[in] payload Message payload
 * @param[in] len Payload length
 * @return ESP_OK on success
 */
esp_err_t mock_mqtt_inject_message(const char *topic, const char *payload, size_t len);

/**
 * @brief Get last published message
 *
 * @param[out] msg Message structure to fill
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no messages
 */
esp_err_t mock_mqtt_get_last_published(mock_mqtt_message_t *msg);

/**
 * @brief Get published message by index
 *
 * @param[in] index Message index (0 = oldest)
 * @param[out] msg Message structure to fill
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if index out of range
 */
esp_err_t mock_mqtt_get_published(size_t index, mock_mqtt_message_t *msg);

/**
 * @brief Get number of published messages
 *
 * @return Number of messages in buffer
 */
size_t mock_mqtt_get_published_count(void);

/**
 * @brief Clear all published messages
 */
void mock_mqtt_clear_published(void);

/**
 * @brief Get mock MQTT statistics
 *
 * @return Statistics structure
 */
mock_mqtt_stats_t mock_mqtt_get_stats(void);

/**
 * @brief Reset mock MQTT statistics
 */
void mock_mqtt_reset_stats(void);

/**
 * @brief Check if topic was subscribed
 *
 * @param[in] topic Topic to check
 * @return true if subscribed, false otherwise
 */
bool mock_mqtt_is_subscribed(const char *topic);

/**
 * @brief Register message callback
 *
 * @param[in] callback Callback function
 * @return ESP_OK on success
 */
typedef void (*mock_mqtt_message_callback_t)(const char *topic, const char *payload, size_t len);
esp_err_t mock_mqtt_register_callback(mock_mqtt_message_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_MQTT_H */
