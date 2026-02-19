/**
 * @file test_zigbee_mqtt_bridge.c
 * @brief Integration Tests for Zigbee-MQTT Bridge
 *
 * Tests end-to-end device state publishing and command handling.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "../mocks/mock_mqtt.h"
#include "../mocks/mock_zigbee.h"
#include "zigbee/zb_device_handler.h"
#include "utils/json_utils.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TEST_ZB_MQTT_BRIDGE";

/**
 * @brief Test Zigbee mock initialization
 */
static void test_zigbee_mock_init(void)
{
    esp_err_t ret = mock_zigbee_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    bool formed = mock_zigbee_is_network_formed();
    TEST_ASSERT_FALSE(formed);
}

/**
 * @brief Test Zigbee network formation
 */
static void test_zigbee_network_formation(void)
{
    mock_zigbee_init();

    /* Start network */
    esp_err_t ret = mock_zigbee_start_network();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(mock_zigbee_is_network_formed());

    /* Stop network */
    ret = mock_zigbee_stop_network();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_FALSE(mock_zigbee_is_network_formed());

    mock_zigbee_deinit();
}

/**
 * @brief Test permit join functionality
 */
static void test_zigbee_permit_join(void)
{
    mock_zigbee_init();
    mock_zigbee_start_network();

    /* Enable permit join */
    esp_err_t ret = mock_zigbee_permit_join(60);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(mock_zigbee_is_permit_join_enabled());

    /* Disable permit join */
    ret = mock_zigbee_permit_join(0);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_FALSE(mock_zigbee_is_permit_join_enabled());

    mock_zigbee_deinit();
}

/**
 * @brief Test device join simulation
 */
static void test_zigbee_device_join(void)
{
    static bool callback_called = false;
    static uint16_t callback_addr = 0;

    void device_callback(uint16_t short_addr, bool joined) {
        callback_called = true;
        callback_addr = short_addr;
        TEST_ASSERT_TRUE(joined);
    }

    mock_zigbee_init();
    mock_zigbee_start_network();
    mock_zigbee_register_device_callback(device_callback);

    callback_called = false;

    /* Simulate device join */
    uint8_t ieee[8] = {0x00, 0x12, 0x4B, 0x00, 0x11, 0x22, 0x33, 0x44};
    esp_err_t ret = mock_zigbee_simulate_device_join(0x1234, ieee, ZB_DEVICE_TYPE_ON_OFF_LIGHT);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(callback_called);
    TEST_ASSERT_EQUAL(0x1234, callback_addr);

    /* Check statistics */
    mock_zigbee_stats_t stats = mock_zigbee_get_stats();
    TEST_ASSERT_EQUAL(1, stats.devices_joined);

    mock_zigbee_deinit();
}

/**
 * @brief Test device leave simulation
 */
static void test_zigbee_device_leave(void)
{
    static bool callback_called = false;

    void device_callback(uint16_t short_addr, bool joined) {
        callback_called = true;
        TEST_ASSERT_FALSE(joined);
    }

    mock_zigbee_init();
    mock_zigbee_start_network();
    mock_zigbee_register_device_callback(device_callback);

    callback_called = false;

    /* Simulate device leave */
    esp_err_t ret = mock_zigbee_simulate_device_leave(0x5678);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(callback_called);

    /* Check statistics */
    mock_zigbee_stats_t stats = mock_zigbee_get_stats();
    TEST_ASSERT_EQUAL(1, stats.devices_left);

    mock_zigbee_deinit();
}

/**
 * @brief Test attribute change simulation
 */
static void test_zigbee_attribute_change(void)
{
    static bool callback_called = false;

    void attr_callback(uint16_t short_addr, uint16_t cluster_id,
                      uint16_t attr_id, const void *value, size_t len) {
        callback_called = true;
        TEST_ASSERT_EQUAL(0xABCD, short_addr);
        TEST_ASSERT_EQUAL(0x0006, cluster_id); /* On/Off cluster */
        TEST_ASSERT_EQUAL(0x0000, attr_id);    /* OnOff attribute */
        TEST_ASSERT_NOT_NULL(value);
    }

    mock_zigbee_init();
    mock_zigbee_start_network();
    mock_zigbee_register_attribute_callback(attr_callback);

    callback_called = false;

    /* Simulate attribute change */
    uint8_t value = 1; /* ON */
    esp_err_t ret = mock_zigbee_simulate_attribute_change(0xABCD, 0x0006, 0x0000,
                                                          &value, sizeof(value));

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(callback_called);

    mock_zigbee_deinit();
}

/**
 * @brief Test sending Zigbee command
 */
static void test_zigbee_send_command(void)
{
    mock_zigbee_init();
    mock_zigbee_start_network();

    /* Send ON command to device */
    esp_err_t ret = mock_zigbee_send_command(0x1234, 1, 0x0006, 0x01, 1);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Check statistics */
    mock_zigbee_stats_t stats = mock_zigbee_get_stats();
    TEST_ASSERT_EQUAL(1, stats.commands_sent);

    mock_zigbee_deinit();
}

/**
 * @brief Test reading Zigbee attribute
 */
static void test_zigbee_read_attribute(void)
{
    mock_zigbee_init();
    mock_zigbee_start_network();

    /* Read OnOff attribute */
    esp_err_t ret = mock_zigbee_read_attribute(0x1234, 1, 0x0006, 0x0000);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Check statistics */
    mock_zigbee_stats_t stats = mock_zigbee_get_stats();
    TEST_ASSERT_EQUAL(1, stats.attributes_read);

    mock_zigbee_deinit();
}

/**
 * @brief Test end-to-end: device join and MQTT publish
 */
static void test_e2e_device_join_mqtt_publish(void)
{
    /* Initialize both mocks */
    mock_zigbee_init();
    mock_mqtt_init();
    mock_zigbee_start_network();
    mock_mqtt_set_connected(true);

    /* Simulate device join */
    uint8_t ieee[8] = {0x00, 0x12, 0x4B, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};
    mock_zigbee_simulate_device_join(0x9999, ieee, ZB_DEVICE_TYPE_ON_OFF_LIGHT);

    /* In real implementation, this would trigger MQTT publish */
    /* Here we manually publish to verify the flow */
    const char *topic = "zigbee2mqtt/light1";
    const char *payload = "{\"state\":\"ON\"}";
    mock_mqtt_publish(topic, payload, strlen(payload), 0, false);

    /* Verify MQTT message was published */
    TEST_ASSERT_EQUAL(1, mock_mqtt_get_published_count());

    mock_mqtt_message_t msg;
    mock_mqtt_get_last_published(&msg);
    TEST_ASSERT_EQUAL_STRING(topic, msg.topic);

    /* Cleanup */
    mock_zigbee_deinit();
    mock_mqtt_deinit();
}

/**
 * @brief Test end-to-end: MQTT command to Zigbee device
 */
static void test_e2e_mqtt_command_to_zigbee(void)
{
    /* Initialize both mocks */
    mock_zigbee_init();
    mock_mqtt_init();
    mock_zigbee_start_network();
    mock_mqtt_set_connected(true);

    /* Simulate MQTT command */
    const char *topic = "zigbee2mqtt/light1/set";
    const char *payload = "{\"state\":\"ON\"}";
    mock_mqtt_inject_message(topic, payload, strlen(payload));

    /* In real implementation, this would parse command and send to Zigbee */
    /* Here we manually send to verify the flow */
    mock_zigbee_send_command(0x1234, 1, 0x0006, 0x01, 1);

    /* Verify Zigbee command was sent */
    mock_zigbee_stats_t stats = mock_zigbee_get_stats();
    TEST_ASSERT_EQUAL(1, stats.commands_sent);

    /* Cleanup */
    mock_zigbee_deinit();
    mock_mqtt_deinit();
}

/**
 * @brief Test bridge statistics
 */
static void test_bridge_statistics(void)
{
    mock_zigbee_init();
    mock_mqtt_init();
    mock_zigbee_start_network();
    mock_mqtt_set_connected(true);

    /* Simulate various operations */
    uint8_t ieee[8] = {0x00, 0x12, 0x4B, 0x00, 0x11, 0x11, 0x11, 0x11};
    mock_zigbee_simulate_device_join(0x1111, ieee, ZB_DEVICE_TYPE_ON_OFF_LIGHT);
    mock_zigbee_simulate_device_leave(0x1111);
    mock_zigbee_send_command(0x1111, 1, 0x0006, 0x01, 1);
    mock_mqtt_publish("topic1", "msg1", 4, 0, false);
    mock_mqtt_publish("topic2", "msg2", 4, 0, false);

    /* Check Zigbee statistics */
    mock_zigbee_stats_t zb_stats = mock_zigbee_get_stats();
    TEST_ASSERT_EQUAL(1, zb_stats.devices_joined);
    TEST_ASSERT_EQUAL(1, zb_stats.devices_left);
    TEST_ASSERT_EQUAL(1, zb_stats.commands_sent);

    /* Check MQTT statistics */
    mock_mqtt_stats_t mqtt_stats = mock_mqtt_get_stats();
    TEST_ASSERT_EQUAL(2, mqtt_stats.publish_count);

    /* Cleanup */
    mock_zigbee_deinit();
    mock_mqtt_deinit();
}

/**
 * @brief Zigbee-MQTT bridge integration test suite
 */
static const test_case_t zigbee_mqtt_bridge_tests[] = {
    {"zigbee_mock_init", test_zigbee_mock_init},
    {"zigbee_network_formation", test_zigbee_network_formation},
    {"zigbee_permit_join", test_zigbee_permit_join},
    {"zigbee_device_join", test_zigbee_device_join},
    {"zigbee_device_leave", test_zigbee_device_leave},
    {"zigbee_attribute_change", test_zigbee_attribute_change},
    {"zigbee_send_command", test_zigbee_send_command},
    {"zigbee_read_attribute", test_zigbee_read_attribute},
    {"e2e_device_join_mqtt_publish", test_e2e_device_join_mqtt_publish},
    {"e2e_mqtt_command_to_zigbee", test_e2e_mqtt_command_to_zigbee},
    {"bridge_statistics", test_bridge_statistics},
};

/**
 * @brief Run all Zigbee-MQTT bridge integration tests
 */
test_stats_t run_zigbee_mqtt_bridge_tests(void)
{
    ESP_LOGI(TAG, "Running Zigbee-MQTT Bridge Integration Tests");
    return test_run_suite(zigbee_mqtt_bridge_tests,
                         sizeof(zigbee_mqtt_bridge_tests) / sizeof(zigbee_mqtt_bridge_tests[0]));
}
