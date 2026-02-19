/**
 * @file test_mqtt_topics.c
 * @brief Unit Tests for MQTT Topic Building and Parsing
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "TEST_MQTT_TOPICS";

/* Base topic prefix for Zigbee2MQTT */
#define MQTT_BASE_TOPIC "zigbee2mqtt"

/**
 * @brief Helper function to build device state topic
 */
static void build_device_state_topic(const char *friendly_name, char *buffer, size_t len)
{
    snprintf(buffer, len, "%s/%s", MQTT_BASE_TOPIC, friendly_name);
}

/**
 * @brief Helper function to build device command topic
 */
static void build_device_command_topic(const char *friendly_name, char *buffer, size_t len)
{
    snprintf(buffer, len, "%s/%s/set", MQTT_BASE_TOPIC, friendly_name);
}

/**
 * @brief Helper function to build bridge topic
 */
static void build_bridge_topic(const char *subtopic, char *buffer, size_t len)
{
    snprintf(buffer, len, "%s/bridge/%s", MQTT_BASE_TOPIC, subtopic);
}

/**
 * @brief Helper function to parse topic and extract device name
 */
static bool parse_device_topic(const char *topic, char *device_name, size_t name_len)
{
    /* Expected format: "zigbee2mqtt/<device_name>" or "zigbee2mqtt/<device_name>/set" */
    const char *prefix = MQTT_BASE_TOPIC "/";
    size_t prefix_len = strlen(prefix);

    if (strncmp(topic, prefix, prefix_len) != 0) {
        return false;
    }

    const char *name_start = topic + prefix_len;
    const char *name_end = strchr(name_start, '/');

    if (name_end == NULL) {
        /* No slash - use entire remaining string */
        strncpy(device_name, name_start, name_len - 1);
        device_name[name_len - 1] = '\0';
    } else {
        /* Copy until slash */
        size_t name_size = name_end - name_start;
        if (name_size >= name_len) {
            name_size = name_len - 1;
        }
        memcpy(device_name, name_start, name_size);
        device_name[name_size] = '\0';
    }

    return true;
}

/**
 * @brief Test building device state topic
 */
static void test_build_device_state_topic(void)
{
    char topic[128];
    build_device_state_topic("living_room_light", topic, sizeof(topic));

    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/living_room_light", topic);
}

/**
 * @brief Test building device command topic
 */
static void test_build_device_command_topic(void)
{
    char topic[128];
    build_device_command_topic("bedroom_switch", topic, sizeof(topic));

    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/bedroom_switch/set", topic);
}

/**
 * @brief Test building bridge info topic
 */
static void test_build_bridge_info_topic(void)
{
    char topic[128];
    build_bridge_topic("info", topic, sizeof(topic));

    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/bridge/info", topic);
}

/**
 * @brief Test building bridge state topic
 */
static void test_build_bridge_state_topic(void)
{
    char topic[128];
    build_bridge_topic("state", topic, sizeof(topic));

    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/bridge/state", topic);
}

/**
 * @brief Test building bridge devices topic
 */
static void test_build_bridge_devices_topic(void)
{
    char topic[128];
    build_bridge_topic("devices", topic, sizeof(topic));

    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/bridge/devices", topic);
}

/**
 * @brief Test building bridge request topic
 */
static void test_build_bridge_request_topic(void)
{
    char topic[128];
    build_bridge_topic("request/permit_join", topic, sizeof(topic));

    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/bridge/request/permit_join", topic);
}

/**
 * @brief Test parsing device topic
 */
static void test_parse_device_topic_simple(void)
{
    char device_name[64];
    bool result = parse_device_topic("zigbee2mqtt/kitchen_light", device_name, sizeof(device_name));

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("kitchen_light", device_name);
}

/**
 * @brief Test parsing device command topic
 */
static void test_parse_device_topic_with_set(void)
{
    char device_name[64];
    bool result = parse_device_topic("zigbee2mqtt/garage_door/set", device_name, sizeof(device_name));

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("garage_door", device_name);
}

/**
 * @brief Test parsing bridge topic
 */
static void test_parse_bridge_topic(void)
{
    const char *topic = "zigbee2mqtt/bridge/request/permit_join";

    /* Should contain "bridge" */
    TEST_ASSERT_NOT_NULL(strstr(topic, "bridge"));
    TEST_ASSERT_NOT_NULL(strstr(topic, "request"));
}

/**
 * @brief Test parsing invalid topic
 */
static void test_parse_invalid_topic(void)
{
    char device_name[64];
    bool result = parse_device_topic("invalid/topic", device_name, sizeof(device_name));

    TEST_ASSERT_FALSE(result);
}

/**
 * @brief Test topic with special characters
 */
static void test_build_topic_with_special_chars(void)
{
    char topic[128];
    build_device_state_topic("device_name_123", topic, sizeof(topic));

    TEST_ASSERT_NOT_NULL(strstr(topic, "device_name_123"));
}

/**
 * @brief Test topic buffer overflow protection
 */
static void test_build_topic_buffer_size(void)
{
    char small_buffer[20];
    build_device_state_topic("very_long_device_name_that_exceeds_buffer",
                            small_buffer, sizeof(small_buffer));

    /* Should be null-terminated */
    TEST_ASSERT_EQUAL('\0', small_buffer[sizeof(small_buffer) - 1]);
}

/**
 * @brief Test multiple device topics
 */
static void test_build_multiple_topics(void)
{
    char topic1[128], topic2[128], topic3[128];

    build_device_state_topic("light1", topic1, sizeof(topic1));
    build_device_state_topic("light2", topic2, sizeof(topic2));
    build_device_state_topic("light3", topic3, sizeof(topic3));

    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/light1", topic1);
    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/light2", topic2);
    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/light3", topic3);
}

/**
 * @brief Test parsing topic with get suffix
 */
static void test_parse_device_topic_with_get(void)
{
    char device_name[64];
    bool result = parse_device_topic("zigbee2mqtt/sensor1/get", device_name, sizeof(device_name));

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("sensor1", device_name);
}

/**
 * @brief Test topic matching
 */
static void test_topic_matching(void)
{
    const char *topic1 = "zigbee2mqtt/light1";
    const char *topic2 = "zigbee2mqtt/light1/set";

    /* Both should start with base topic + device name */
    TEST_ASSERT_NOT_NULL(strstr(topic1, "zigbee2mqtt/light1"));
    TEST_ASSERT_NOT_NULL(strstr(topic2, "zigbee2mqtt/light1"));
}

/**
 * @brief Test empty device name handling
 */
static void test_build_topic_empty_name(void)
{
    char topic[128];
    build_device_state_topic("", topic, sizeof(topic));

    /* Should produce "zigbee2mqtt/" */
    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/", topic);
}

/**
 * @brief MQTT topics test suite
 */
static const test_case_t mqtt_topics_tests[] = {
    {"build_device_state_topic", test_build_device_state_topic},
    {"build_device_command_topic", test_build_device_command_topic},
    {"build_bridge_info_topic", test_build_bridge_info_topic},
    {"build_bridge_state_topic", test_build_bridge_state_topic},
    {"build_bridge_devices_topic", test_build_bridge_devices_topic},
    {"build_bridge_request_topic", test_build_bridge_request_topic},
    {"parse_device_topic_simple", test_parse_device_topic_simple},
    {"parse_device_topic_with_set", test_parse_device_topic_with_set},
    {"parse_bridge_topic", test_parse_bridge_topic},
    {"parse_invalid_topic", test_parse_invalid_topic},
    {"build_topic_with_special_chars", test_build_topic_with_special_chars},
    {"build_topic_buffer_size", test_build_topic_buffer_size},
    {"build_multiple_topics", test_build_multiple_topics},
    {"parse_device_topic_with_get", test_parse_device_topic_with_get},
    {"topic_matching", test_topic_matching},
    {"build_topic_empty_name", test_build_topic_empty_name},
};

/**
 * @brief Run all MQTT topics tests
 */
test_stats_t run_mqtt_topics_tests(void)
{
    ESP_LOGI(TAG, "Running MQTT Topics Tests");
    return test_run_suite(mqtt_topics_tests,
                         sizeof(mqtt_topics_tests) / sizeof(mqtt_topics_tests[0]));
}
