/**
 * @file mock_mqtt.c
 * @brief Mock MQTT Client Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "mock_mqtt.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "MOCK_MQTT";

#define MAX_PUBLISHED_MESSAGES 50
#define MAX_SUBSCRIPTIONS 20

/* Mock state */
static struct {
    bool initialized;
    bool connected;
    mock_mqtt_message_t published[MAX_PUBLISHED_MESSAGES];
    size_t published_count;
    char subscriptions[MAX_SUBSCRIPTIONS][128];
    size_t subscription_count;
    mock_mqtt_stats_t stats;
    mock_mqtt_message_callback_t callback;
    int next_msg_id;
} g_mock = {0};

/**
 * @brief Initialize mock MQTT client
 */
esp_err_t mock_mqtt_init(void)
{
    if (g_mock.initialized) {
        ESP_LOGW(TAG, "Mock MQTT already initialized");
        return ESP_OK;
    }

    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.initialized = true;
    g_mock.next_msg_id = 1;

    ESP_LOGI(TAG, "Mock MQTT initialized");
    return ESP_OK;
}

/**
 * @brief Deinitialize mock MQTT client
 */
esp_err_t mock_mqtt_deinit(void)
{
    g_mock.initialized = false;
    g_mock.connected = false;
    g_mock.published_count = 0;
    g_mock.subscription_count = 0;
    g_mock.callback = NULL;

    ESP_LOGI(TAG, "Mock MQTT deinitialized");
    return ESP_OK;
}

/**
 * @brief Set connection status
 */
esp_err_t mock_mqtt_set_connected(bool connected)
{
    g_mock.connected = connected;
    if (connected) {
        g_mock.stats.connect_count++;
    } else {
        g_mock.stats.disconnect_count++;
    }
    g_mock.stats.connected = connected;

    ESP_LOGI(TAG, "Mock MQTT connection status: %s", connected ? "connected" : "disconnected");
    return ESP_OK;
}

/**
 * @brief Get connection status
 */
bool mock_mqtt_is_connected(void)
{
    return g_mock.connected;
}

/**
 * @brief Publish message
 */
int mock_mqtt_publish(const char *topic, const char *payload, size_t len, int qos, bool retain)
{
    if (!g_mock.initialized || !g_mock.connected) {
        ESP_LOGE(TAG, "Mock MQTT not initialized or not connected");
        return -1;
    }

    if (!topic || !payload) {
        return -1;
    }

    if (g_mock.published_count >= MAX_PUBLISHED_MESSAGES) {
        ESP_LOGW(TAG, "Published messages buffer full, overwriting oldest");
        /* Shift array left */
        memmove(&g_mock.published[0], &g_mock.published[1],
                sizeof(mock_mqtt_message_t) * (MAX_PUBLISHED_MESSAGES - 1));
        g_mock.published_count = MAX_PUBLISHED_MESSAGES - 1;
    }

    /* Store message */
    mock_mqtt_message_t *msg = &g_mock.published[g_mock.published_count++];
    strncpy(msg->topic, topic, sizeof(msg->topic) - 1);
    msg->topic[sizeof(msg->topic) - 1] = '\0';

    size_t copy_len = (len < sizeof(msg->payload)) ? len : sizeof(msg->payload);
    memcpy(msg->payload, payload, copy_len);
    msg->payload_len = copy_len;
    msg->qos = qos;
    msg->retained = retain;

    g_mock.stats.publish_count++;

    ESP_LOGI(TAG, "Mock MQTT publish: %s (%zu bytes)", topic, len);
    return g_mock.next_msg_id++;
}

/**
 * @brief Subscribe to topic
 */
int mock_mqtt_subscribe(const char *topic, int qos)
{
    if (!g_mock.initialized || !g_mock.connected) {
        return -1;
    }

    if (!topic) {
        return -1;
    }

    if (g_mock.subscription_count >= MAX_SUBSCRIPTIONS) {
        ESP_LOGW(TAG, "Subscription limit reached");
        return -1;
    }

    /* Store subscription */
    strncpy(g_mock.subscriptions[g_mock.subscription_count++], topic,
            sizeof(g_mock.subscriptions[0]) - 1);

    g_mock.stats.subscribe_count++;

    ESP_LOGI(TAG, "Mock MQTT subscribe: %s (qos=%d)", topic, qos);
    return g_mock.next_msg_id++;
}

/**
 * @brief Unsubscribe from topic
 */
int mock_mqtt_unsubscribe(const char *topic)
{
    if (!g_mock.initialized || !topic) {
        return -1;
    }

    /* Find and remove subscription */
    for (size_t i = 0; i < g_mock.subscription_count; i++) {
        if (strcmp(g_mock.subscriptions[i], topic) == 0) {
            /* Shift array left */
            memmove(&g_mock.subscriptions[i], &g_mock.subscriptions[i + 1],
                    sizeof(g_mock.subscriptions[0]) * (g_mock.subscription_count - i - 1));
            g_mock.subscription_count--;
            break;
        }
    }

    g_mock.stats.unsubscribe_count++;

    ESP_LOGI(TAG, "Mock MQTT unsubscribe: %s", topic);
    return g_mock.next_msg_id++;
}

/**
 * @brief Inject message (simulate receiving)
 */
esp_err_t mock_mqtt_inject_message(const char *topic, const char *payload, size_t len)
{
    if (!g_mock.initialized || !g_mock.connected) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!topic || !payload) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Call registered callback */
    if (g_mock.callback) {
        g_mock.callback(topic, payload, len);
    }

    ESP_LOGI(TAG, "Mock MQTT inject: %s (%zu bytes)", topic, len);
    return ESP_OK;
}

/**
 * @brief Get last published message
 */
esp_err_t mock_mqtt_get_last_published(mock_mqtt_message_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }

    if (g_mock.published_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(msg, &g_mock.published[g_mock.published_count - 1], sizeof(mock_mqtt_message_t));
    return ESP_OK;
}

/**
 * @brief Get published message by index
 */
esp_err_t mock_mqtt_get_published(size_t index, mock_mqtt_message_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }

    if (index >= g_mock.published_count) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(msg, &g_mock.published[index], sizeof(mock_mqtt_message_t));
    return ESP_OK;
}

/**
 * @brief Get published message count
 */
size_t mock_mqtt_get_published_count(void)
{
    return g_mock.published_count;
}

/**
 * @brief Clear published messages
 */
void mock_mqtt_clear_published(void)
{
    g_mock.published_count = 0;
    memset(g_mock.published, 0, sizeof(g_mock.published));
}

/**
 * @brief Get statistics
 */
mock_mqtt_stats_t mock_mqtt_get_stats(void)
{
    return g_mock.stats;
}

/**
 * @brief Reset statistics
 */
void mock_mqtt_reset_stats(void)
{
    memset(&g_mock.stats, 0, sizeof(g_mock.stats));
    g_mock.stats.connected = g_mock.connected;
}

/**
 * @brief Check if subscribed to topic
 */
bool mock_mqtt_is_subscribed(const char *topic)
{
    if (!topic) {
        return false;
    }

    for (size_t i = 0; i < g_mock.subscription_count; i++) {
        if (strcmp(g_mock.subscriptions[i], topic) == 0) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Register message callback
 */
esp_err_t mock_mqtt_register_callback(mock_mqtt_message_callback_t callback)
{
    g_mock.callback = callback;
    return ESP_OK;
}
