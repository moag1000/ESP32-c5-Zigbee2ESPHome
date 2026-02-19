/**
 * @file test_wifi_mqtt_integration.c
 * @brief Integration Tests for WiFi + MQTT Connection Flow
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "../mocks/mock_mqtt.h"
#include "core/config_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "TEST_WIFI_MQTT";

/**
 * @brief Test MQTT mock initialization
 */
static void test_mqtt_mock_init(void)
{
    esp_err_t ret = mock_mqtt_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    bool connected = mock_mqtt_is_connected();
    TEST_ASSERT_FALSE(connected);
}

/**
 * @brief Test MQTT connection flow
 */
static void test_mqtt_connection_flow(void)
{
    /* Initialize mock */
    mock_mqtt_init();
    TEST_ASSERT_FALSE(mock_mqtt_is_connected());

    /* Simulate connection */
    esp_err_t ret = mock_mqtt_set_connected(true);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(mock_mqtt_is_connected());

    /* Check statistics */
    mock_mqtt_stats_t stats = mock_mqtt_get_stats();
    TEST_ASSERT_EQUAL(1, stats.connect_count);
    TEST_ASSERT_TRUE(stats.connected);

    /* Cleanup */
    mock_mqtt_deinit();
}

/**
 * @brief Test MQTT publish operation
 */
static void test_mqtt_publish(void)
{
    mock_mqtt_init();
    mock_mqtt_set_connected(true);

    /* Publish message */
    const char *topic = "zigbee2mqtt/bridge/state";
    const char *payload = "{\"state\":\"online\"}";
    int msg_id = mock_mqtt_publish(topic, payload, strlen(payload), 0, false);

    TEST_ASSERT_GREATER_THAN(0, msg_id);

    /* Verify message was stored */
    mock_mqtt_message_t msg;
    esp_err_t ret = mock_mqtt_get_last_published(&msg);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_STRING(topic, msg.topic);

    /* Check publish count */
    size_t count = mock_mqtt_get_published_count();
    TEST_ASSERT_EQUAL(1, count);

    mock_mqtt_deinit();
}

/**
 * @brief Test MQTT subscribe operation
 */
static void test_mqtt_subscribe(void)
{
    mock_mqtt_init();
    mock_mqtt_set_connected(true);

    /* Subscribe to topics */
    const char *cmd_topic = "zigbee2mqtt/+/set";
    const char *bridge_topic = "zigbee2mqtt/bridge/request/#";

    int msg_id1 = mock_mqtt_subscribe(cmd_topic, 0);
    TEST_ASSERT_GREATER_THAN(0, msg_id1);

    int msg_id2 = mock_mqtt_subscribe(bridge_topic, 0);
    TEST_ASSERT_GREATER_THAN(0, msg_id2);

    /* Verify subscriptions */
    TEST_ASSERT_TRUE(mock_mqtt_is_subscribed(cmd_topic));
    TEST_ASSERT_TRUE(mock_mqtt_is_subscribed(bridge_topic));

    /* Check statistics */
    mock_mqtt_stats_t stats = mock_mqtt_get_stats();
    TEST_ASSERT_EQUAL(2, stats.subscribe_count);

    mock_mqtt_deinit();
}

/**
 * @brief Test MQTT message injection
 */
static void test_mqtt_message_injection(void)
{
    static bool callback_called = false;

    void message_callback(const char *topic, const char *payload, size_t len) {
        callback_called = true;
        TEST_ASSERT_NOT_NULL(topic);
        TEST_ASSERT_NOT_NULL(payload);
        TEST_ASSERT_GREATER_THAN(0, len);
    }

    mock_mqtt_init();
    mock_mqtt_set_connected(true);
    mock_mqtt_register_callback(message_callback);

    callback_called = false;

    /* Inject message */
    const char *topic = "zigbee2mqtt/light1/set";
    const char *payload = "{\"state\":\"ON\"}";
    esp_err_t ret = mock_mqtt_inject_message(topic, payload, strlen(payload));

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(callback_called);

    mock_mqtt_deinit();
}

/**
 * @brief Test MQTT reconnection handling
 */
static void test_mqtt_reconnection(void)
{
    mock_mqtt_init();

    /* Connect */
    mock_mqtt_set_connected(true);
    TEST_ASSERT_TRUE(mock_mqtt_is_connected());

    /* Disconnect */
    mock_mqtt_set_connected(false);
    TEST_ASSERT_FALSE(mock_mqtt_is_connected());

    /* Reconnect */
    mock_mqtt_set_connected(true);
    TEST_ASSERT_TRUE(mock_mqtt_is_connected());

    /* Check statistics */
    mock_mqtt_stats_t stats = mock_mqtt_get_stats();
    TEST_ASSERT_EQUAL(2, stats.connect_count);
    TEST_ASSERT_EQUAL(1, stats.disconnect_count);

    mock_mqtt_deinit();
}

/**
 * @brief Test publishing multiple messages
 */
static void test_mqtt_multiple_publishes(void)
{
    mock_mqtt_init();
    mock_mqtt_set_connected(true);

    /* Publish multiple messages */
    for (int i = 0; i < 5; i++) {
        char topic[64];
        char payload[64];
        snprintf(topic, sizeof(topic), "zigbee2mqtt/device%d", i);
        snprintf(payload, sizeof(payload), "{\"id\":%d}", i);

        int msg_id = mock_mqtt_publish(topic, payload, strlen(payload), 0, false);
        TEST_ASSERT_GREATER_THAN(0, msg_id);
    }

    /* Verify count */
    size_t count = mock_mqtt_get_published_count();
    TEST_ASSERT_EQUAL(5, count);

    /* Verify individual messages */
    for (int i = 0; i < 5; i++) {
        mock_mqtt_message_t msg;
        esp_err_t ret = mock_mqtt_get_published(i, &msg);
        TEST_ASSERT_EQUAL(ESP_OK, ret);

        char expected_topic[64];
        snprintf(expected_topic, sizeof(expected_topic), "zigbee2mqtt/device%d", i);
        TEST_ASSERT_EQUAL_STRING(expected_topic, msg.topic);
    }

    mock_mqtt_deinit();
}

/**
 * @brief Test MQTT QoS and retain flags
 */
static void test_mqtt_qos_retain(void)
{
    mock_mqtt_init();
    mock_mqtt_set_connected(true);

    /* Publish with QoS 1 and retain */
    const char *topic = "zigbee2mqtt/bridge/info";
    const char *payload = "{\"version\":\"1.0.0\"}";
    mock_mqtt_publish(topic, payload, strlen(payload), 1, true);

    /* Verify message properties */
    mock_mqtt_message_t msg;
    mock_mqtt_get_last_published(&msg);
    TEST_ASSERT_EQUAL(1, msg.qos);
    TEST_ASSERT_TRUE(msg.retained);

    mock_mqtt_deinit();
}

/**
 * @brief Test MQTT statistics tracking
 */
static void test_mqtt_statistics(void)
{
    mock_mqtt_init();
    mock_mqtt_reset_stats();
    mock_mqtt_set_connected(true);

    /* Perform operations */
    mock_mqtt_publish("topic1", "msg1", 4, 0, false);
    mock_mqtt_publish("topic2", "msg2", 4, 0, false);
    mock_mqtt_subscribe("sub1", 0);
    mock_mqtt_unsubscribe("sub1");

    /* Check statistics */
    mock_mqtt_stats_t stats = mock_mqtt_get_stats();
    TEST_ASSERT_EQUAL(2, stats.publish_count);
    TEST_ASSERT_EQUAL(1, stats.subscribe_count);
    TEST_ASSERT_EQUAL(1, stats.unsubscribe_count);
    TEST_ASSERT_EQUAL(1, stats.connect_count);

    mock_mqtt_deinit();
}

/**
 * @brief Test clearing published messages
 */
static void test_mqtt_clear_published(void)
{
    mock_mqtt_init();
    mock_mqtt_set_connected(true);

    /* Publish some messages */
    mock_mqtt_publish("topic1", "msg1", 4, 0, false);
    mock_mqtt_publish("topic2", "msg2", 4, 0, false);
    TEST_ASSERT_EQUAL(2, mock_mqtt_get_published_count());

    /* Clear */
    mock_mqtt_clear_published();
    TEST_ASSERT_EQUAL(0, mock_mqtt_get_published_count());

    mock_mqtt_deinit();
}

/**
 * @brief WiFi + MQTT integration test suite
 */
static const test_case_t wifi_mqtt_integration_tests[] = {
    {"mqtt_mock_init", test_mqtt_mock_init},
    {"mqtt_connection_flow", test_mqtt_connection_flow},
    {"mqtt_publish", test_mqtt_publish},
    {"mqtt_subscribe", test_mqtt_subscribe},
    {"mqtt_message_injection", test_mqtt_message_injection},
    {"mqtt_reconnection", test_mqtt_reconnection},
    {"mqtt_multiple_publishes", test_mqtt_multiple_publishes},
    {"mqtt_qos_retain", test_mqtt_qos_retain},
    {"mqtt_statistics", test_mqtt_statistics},
    {"mqtt_clear_published", test_mqtt_clear_published},
};

/**
 * @brief Run all WiFi + MQTT integration tests
 */
test_stats_t run_wifi_mqtt_integration_tests(void)
{
    ESP_LOGI(TAG, "Running WiFi + MQTT Integration Tests");
    return test_run_suite(wifi_mqtt_integration_tests,
                         sizeof(wifi_mqtt_integration_tests) / sizeof(wifi_mqtt_integration_tests[0]));
}
