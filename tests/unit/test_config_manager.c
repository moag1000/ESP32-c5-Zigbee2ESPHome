/**
 * @file test_config_manager.c
 * @brief Unit Tests for Configuration Manager
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "core/config_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TEST_CFG";

/**
 * @brief Test config manager initialization
 */
static void test_config_manager_init_success(void)
{
    esp_err_t ret = config_manager_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * @brief Test loading defaults
 */
static void test_config_manager_load_defaults(void)
{
    gateway_config_t config;
    memset(&config, 0, sizeof(config));

    esp_err_t ret = config_manager_load_defaults(&config);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify default values are populated */
    TEST_ASSERT(config.mqtt_port > 0);
    TEST_ASSERT(config.mqtt_keepalive > 0);
    TEST_ASSERT(config.zigbee_channel >= 11 && config.zigbee_channel <= 26);
}

/**
 * @brief Test getting current configuration
 */
static void test_config_manager_get_config(void)
{
    const gateway_config_t *config = config_manager_get_config();
    TEST_ASSERT_NOT_NULL(config);

    /* Verify basic fields */
    TEST_ASSERT(config->mqtt_port > 0);
    TEST_ASSERT(config->config_version == CONFIG_VERSION);
}

/**
 * @brief Test configuration validation - valid config
 */
static void test_config_manager_validate_valid(void)
{
    gateway_config_t config;
    config_manager_load_defaults(&config);

    esp_err_t ret = config_manager_validate(&config);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * @brief Test configuration validation - invalid Zigbee channel
 */
static void test_config_manager_validate_invalid_channel(void)
{
    gateway_config_t config;
    config_manager_load_defaults(&config);

    /* Set invalid channel */
    config.zigbee_channel = 50; /* Invalid - must be 11-26 */

    esp_err_t ret = config_manager_validate(&config);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

/**
 * @brief Test configuration validation - invalid MQTT port
 */
static void test_config_manager_validate_invalid_port(void)
{
    gateway_config_t config;
    config_manager_load_defaults(&config);

    /* Set invalid port */
    config.mqtt_port = 0;

    esp_err_t ret = config_manager_validate(&config);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

/**
 * @brief Test save and load configuration
 */
static void test_config_manager_save_load(void)
{
    gateway_config_t config_save, config_load;

    /* Load defaults and modify */
    config_manager_load_defaults(&config_save);
    strcpy(config_save.wifi_ssid, "TestSSID");
    strcpy(config_save.mqtt_client_id, "test_client");
    config_save.mqtt_port = 1234;

    /* Save configuration */
    esp_err_t ret = config_manager_save(&config_save);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Load configuration */
    memset(&config_load, 0, sizeof(config_load));
    ret = config_manager_load(&config_load);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify loaded values match saved values */
    TEST_ASSERT_EQUAL_STRING("TestSSID", config_load.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("test_client", config_load.mqtt_client_id);
    TEST_ASSERT_EQUAL(1234, config_load.mqtt_port);
}

/**
 * @brief Test reset to defaults
 */
static void test_config_manager_reset_to_defaults(void)
{
    /* Modify and save config */
    gateway_config_t config;
    config_manager_load_defaults(&config);
    strcpy(config.wifi_ssid, "ModifiedSSID");
    config_manager_save(&config);

    /* Reset to defaults */
    esp_err_t ret = config_manager_reset_to_defaults();
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Load and verify defaults */
    const gateway_config_t *loaded = config_manager_get_config();
    TEST_ASSERT_NOT_NULL(loaded);
}

/**
 * @brief Test get config value by key
 */
static void test_config_manager_get_by_key(void)
{
    uint16_t port = 0;
    size_t len = sizeof(port);

    esp_err_t ret = config_manager_get("mqtt_port", &port, &len);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_GREATER_THAN(0, port);
}

/**
 * @brief Test set config value by key
 */
static void test_config_manager_set_by_key(void)
{
    uint16_t new_port = 5678;
    size_t len = sizeof(new_port);

    esp_err_t ret = config_manager_set("mqtt_port", &new_port, len);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify the change */
    uint16_t read_port = 0;
    len = sizeof(read_port);
    ret = config_manager_get("mqtt_port", &read_port, &len);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(5678, read_port);
}

/**
 * @brief Test export configuration as JSON
 */
static void test_config_manager_export_json(void)
{
    char buffer[1024];
    esp_err_t ret = config_manager_export_json(buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify JSON contains expected fields */
    TEST_ASSERT_NOT_NULL(strstr(buffer, "mqtt_port"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, "zigbee_channel"));
}

/**
 * @brief Test import configuration from JSON
 */
static void test_config_manager_import_json(void)
{
    const char *json = "{\"mqtt_port\":9999,\"mqtt_keepalive\":120}";

    esp_err_t ret = config_manager_import_json(json);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify imported values */
    const gateway_config_t *config = config_manager_get_config();
    TEST_ASSERT_EQUAL(9999, config->mqtt_port);
    TEST_ASSERT_EQUAL(120, config->mqtt_keepalive);
}

/**
 * @brief Test import invalid JSON
 */
static void test_config_manager_import_invalid_json(void)
{
    const char *json = "{invalid json}}";

    esp_err_t ret = config_manager_import_json(json);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

/**
 * @brief Test configuration with NULL parameters
 */
static void test_config_manager_null_parameters(void)
{
    esp_err_t ret;

    ret = config_manager_save(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);

    ret = config_manager_load(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);

    ret = config_manager_validate(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

/**
 * @brief Configuration manager test suite
 */
static const test_case_t config_manager_tests[] = {
    {"config_manager_init", test_config_manager_init_success},
    {"config_manager_load_defaults", test_config_manager_load_defaults},
    {"config_manager_get_config", test_config_manager_get_config},
    {"config_manager_validate_valid", test_config_manager_validate_valid},
    {"config_manager_validate_invalid_channel", test_config_manager_validate_invalid_channel},
    {"config_manager_validate_invalid_port", test_config_manager_validate_invalid_port},
    {"config_manager_save_load", test_config_manager_save_load},
    {"config_manager_reset_to_defaults", test_config_manager_reset_to_defaults},
    {"config_manager_get_by_key", test_config_manager_get_by_key},
    {"config_manager_set_by_key", test_config_manager_set_by_key},
    {"config_manager_export_json", test_config_manager_export_json},
    {"config_manager_import_json", test_config_manager_import_json},
    {"config_manager_import_invalid_json", test_config_manager_import_invalid_json},
    {"config_manager_null_parameters", test_config_manager_null_parameters},
};

/**
 * @brief Run all config manager tests
 */
test_stats_t run_config_manager_tests(void)
{
    ESP_LOGI(TAG, "Running Config Manager Tests");
    return test_run_suite(config_manager_tests,
                         sizeof(config_manager_tests) / sizeof(config_manager_tests[0]));
}
