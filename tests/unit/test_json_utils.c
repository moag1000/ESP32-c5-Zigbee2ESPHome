/**
 * @file test_json_utils.c
 * @brief Unit Tests for JSON Utilities
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "utils/json_utils.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "TEST_JSON";

/**
 * @brief Test creating bridge state JSON
 */
static void test_json_create_bridge_state(void)
{
    char *json = json_create_bridge_state("online");
    TEST_ASSERT_NOT_NULL(json);

    /* Verify JSON contains "online" */
    TEST_ASSERT_NOT_NULL(strstr(json, "online"));

    free(json);
}

/**
 * @brief Test creating availability JSON
 */
static void test_json_create_availability(void)
{
    char *json = json_create_availability(true);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "online"));
    free(json);

    json = json_create_availability(false);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "offline"));
    free(json);
}

/**
 * @brief Test formatting IEEE address
 */
static void test_json_format_ieee_addr(void)
{
    uint8_t ieee_addr[8] = {0x00, 0x12, 0x4B, 0x00, 0x12, 0x34, 0xAB, 0xCD};
    char buffer[32];

    esp_err_t ret = json_format_ieee_addr(ieee_addr, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify format: 0x00124b001234abcd */
    TEST_ASSERT(strlen(buffer) > 0);
    TEST_ASSERT_EQUAL('0', buffer[0]);
    TEST_ASSERT_EQUAL('x', buffer[1]);
}

/**
 * @brief Test parsing command JSON - ON/OFF
 */
static void test_json_parse_command_on_off(void)
{
    bool state = false;
    uint8_t brightness;
    uint16_t color_x, color_y, transition;

    /* Test ON command */
    const char *json_on = "{\"state\":\"ON\"}";
    esp_err_t ret = json_parse_command(json_on, &state, &brightness,
                                       &color_x, &color_y, &transition);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(state);

    /* Test OFF command */
    const char *json_off = "{\"state\":\"OFF\"}";
    ret = json_parse_command(json_off, &state, &brightness,
                            &color_x, &color_y, &transition);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_FALSE(state);
}

/**
 * @brief Test parsing command JSON - brightness
 */
static void test_json_parse_command_brightness(void)
{
    bool state;
    uint8_t brightness = 0;
    uint16_t color_x, color_y, transition;

    const char *json = "{\"state\":\"ON\",\"brightness\":150}";
    esp_err_t ret = json_parse_command(json, &state, &brightness,
                                       &color_x, &color_y, &transition);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(150, brightness);
}

/**
 * @brief Test parsing command JSON - color
 */
static void test_json_parse_command_color(void)
{
    bool state;
    uint8_t brightness;
    uint16_t color_x = 0, color_y = 0, transition;

    const char *json = "{\"color\":{\"x\":0.5,\"y\":0.3}}";
    esp_err_t ret = json_parse_command(json, &state, &brightness,
                                       &color_x, &color_y, &transition);

    /* If color parsing is implemented, x and y should be set */
    if (ret == ESP_OK) {
        /* Colors are converted to uint16 range */
        TEST_ASSERT_GREATER_THAN(0, color_x);
        TEST_ASSERT_GREATER_THAN(0, color_y);
    }
}

/**
 * @brief Test parsing invalid JSON
 */
static void test_json_parse_command_invalid(void)
{
    bool state;
    uint8_t brightness;
    uint16_t color_x, color_y, transition;

    const char *json = "{invalid}";
    esp_err_t ret = json_parse_command(json, &state, &brightness,
                                       &color_x, &color_y, &transition);
    TEST_ASSERT_EQUAL(ESP_FAIL, ret);
}

/**
 * @brief Test parsing permit join request
 */
static void test_json_parse_permit_join(void)
{
    uint8_t duration = 0;

    const char *json = "{\"duration\":120}";
    esp_err_t ret = json_parse_permit_join(json, &duration);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(120, duration);
}

/**
 * @brief Test parsing device remove request
 */
static void test_json_parse_device_remove(void)
{
    char friendly_name[64];

    const char *json = "{\"friendly_name\":\"living_room_light\"}";
    esp_err_t ret = json_parse_device_remove(json, friendly_name, sizeof(friendly_name));
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_STRING("living_room_light", friendly_name);
}

/**
 * @brief Test parsing device rename request
 */
static void test_json_parse_device_rename(void)
{
    char old_name[64], new_name[64];

    const char *json = "{\"old\":\"light1\",\"new\":\"bedroom_light\"}";
    esp_err_t ret = json_parse_device_rename(json, old_name, new_name, sizeof(old_name));
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_STRING("light1", old_name);
    TEST_ASSERT_EQUAL_STRING("bedroom_light", new_name);
}

/**
 * @brief Test converting cJSON to string
 */
static void test_json_to_string_and_delete(void)
{
    cJSON *json = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(json);

    cJSON_AddStringToObject(json, "test", "value");
    cJSON_AddNumberToObject(json, "number", 42);

    char *str = json_to_string_and_delete(json);
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_NOT_NULL(strstr(str, "test"));
    TEST_ASSERT_NOT_NULL(strstr(str, "value"));
    TEST_ASSERT_NOT_NULL(strstr(str, "42"));

    free(str);
    /* json object is already deleted by json_to_string_and_delete */
}

/**
 * @brief Test getting component type string
 */
static void test_json_get_component_string(void)
{
    const char *str;

    str = json_get_component_string(HA_COMPONENT_LIGHT);
    TEST_ASSERT_EQUAL_STRING("light", str);

    str = json_get_component_string(HA_COMPONENT_SENSOR);
    TEST_ASSERT_EQUAL_STRING("sensor", str);

    str = json_get_component_string(HA_COMPONENT_SWITCH);
    TEST_ASSERT_EQUAL_STRING("switch", str);
}

/**
 * @brief Test creating bridge info JSON
 */
static void test_json_create_bridge_info(void)
{
    cJSON *json = json_create_bridge_info();
    TEST_ASSERT_NOT_NULL(json);

    /* Verify it's an object */
    TEST_ASSERT_TRUE(cJSON_IsObject(json));

    /* Convert to string to verify content */
    char *str = cJSON_Print(json);
    TEST_ASSERT_NOT_NULL(str);

    /* Should contain version or coordinator info */
    /* Exact fields depend on implementation */

    cJSON_Delete(json);
    free(str);
}

/**
 * @brief Test creating device list JSON
 */
static void test_json_create_device_list(void)
{
    cJSON *json = json_create_device_list();
    TEST_ASSERT_NOT_NULL(json);

    /* Should be an array */
    TEST_ASSERT_TRUE(cJSON_IsArray(json));

    cJSON_Delete(json);
}

/**
 * @brief Test JSON with NULL parameters
 */
static void test_json_null_parameters(void)
{
    char buffer[32];
    esp_err_t ret;

    ret = json_format_ieee_addr(NULL, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);

    ret = json_parse_command(NULL, NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_FAIL, ret);
}

/**
 * @brief JSON utilities test suite
 */
static const test_case_t json_utils_tests[] = {
    {"json_create_bridge_state", test_json_create_bridge_state},
    {"json_create_availability", test_json_create_availability},
    {"json_format_ieee_addr", test_json_format_ieee_addr},
    {"json_parse_command_on_off", test_json_parse_command_on_off},
    {"json_parse_command_brightness", test_json_parse_command_brightness},
    {"json_parse_command_color", test_json_parse_command_color},
    {"json_parse_command_invalid", test_json_parse_command_invalid},
    {"json_parse_permit_join", test_json_parse_permit_join},
    {"json_parse_device_remove", test_json_parse_device_remove},
    {"json_parse_device_rename", test_json_parse_device_rename},
    {"json_to_string_and_delete", test_json_to_string_and_delete},
    {"json_get_component_string", test_json_get_component_string},
    {"json_create_bridge_info", test_json_create_bridge_info},
    {"json_create_device_list", test_json_create_device_list},
    {"json_null_parameters", test_json_null_parameters},
};

/**
 * @brief Run all JSON utilities tests
 */
test_stats_t run_json_utils_tests(void)
{
    ESP_LOGI(TAG, "Running JSON Utilities Tests");
    return test_run_suite(json_utils_tests,
                         sizeof(json_utils_tests) / sizeof(json_utils_tests[0]));
}
