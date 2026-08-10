/**
 * @file run_all_tests.c
 * @brief Main Test Runner for ESP32-C5 Zigbee2MQTT Gateway
 *
 * Runs all unit tests and integration tests, reports results.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "test_framework.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include <stdio.h>

static const char *TAG = "TEST_RUNNER";

/* External test suite runners.
 *
 * Each suite is opt-in via a TEST_SUITE_* define set in main/CMakeLists.txt,
 * because a suite can only be linked once the modules it exercises are in
 * SRCS. Enabling one here without adding its sources gives an undefined
 * reference at link time, not a compile error, so the two must stay in step.
 *
 * See README_TESTS.md for the current state of each suite. */
#ifdef TEST_SUITE_MEMORY_MANAGER
extern test_stats_t run_memory_manager_tests(void);
#endif
#ifdef TEST_SUITE_CONFIG_MANAGER
extern test_stats_t run_config_manager_tests(void);
#endif
#ifdef TEST_SUITE_JSON_UTILS
extern test_stats_t run_json_utils_tests(void);
#endif
#ifdef TEST_SUITE_VERSION
extern test_stats_t run_version_tests(void);
#endif
#ifdef TEST_SUITE_DEVICE_REGISTRY
extern test_stats_t run_device_registry_tests(void);
#endif
#ifdef TEST_SUITE_ZB_DIAGNOSTICS
extern test_stats_t run_zb_diagnostics_tests(void);
#endif
#ifdef TEST_SUITE_ZB_BACKUP
extern test_stats_t run_zb_backup_tests(void);
extern test_stats_t run_zb_tuya_parse_tests(void);
#endif
#ifdef TEST_SUITE_ESPHOME_PROTOCOL
extern test_stats_t run_esphome_protocol_tests(void);
extern test_stats_t run_esphome_execute_service_tests(void);
#endif
#ifdef TEST_SUITE_ESPHOME_ENTITY_MIRROR
extern test_stats_t run_esphome_entity_mirror_tests(void);
#endif
#ifdef TEST_SUITE_ESPHOME_NOISE
extern test_stats_t run_esphome_noise_tests(void);
#endif
#ifdef TEST_SUITE_MQTT_TOPICS
extern test_stats_t run_mqtt_topics_tests(void);
#endif
#ifdef TEST_SUITE_WIFI_MQTT
extern test_stats_t run_wifi_mqtt_integration_tests(void);
#endif
#ifdef TEST_SUITE_ZIGBEE_BRIDGE
extern test_stats_t run_zigbee_mqtt_bridge_tests(void);
#endif
#ifdef TEST_SUITE_OTA
extern test_stats_t run_ota_tests(void);
#endif

/**
 * @brief Print test banner
 */
static void print_banner(const char *title)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "%s", title);
    ESP_LOGI(TAG, "========================================");
}

/**
 * @brief Print test suite result
 */
static void print_suite_result(const char *name, test_stats_t stats)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Suite: %s", name);
    ESP_LOGI(TAG, "  Passed:  %lu / %lu", (unsigned long)stats.passed, (unsigned long)stats.total);
    if (stats.failed > 0) {
        ESP_LOGE(TAG, "  FAILED:  %lu", (unsigned long)stats.failed);
    }
    if (stats.skipped > 0) {
        ESP_LOGW(TAG, "  Skipped: %lu", (unsigned long)stats.skipped);
    }
}

/**
 * @brief Combine test statistics
 */
static void combine_stats(test_stats_t *total, test_stats_t suite)
{
    total->total += suite.total;
    total->passed += suite.passed;
    total->failed += suite.failed;
    total->skipped += suite.skipped;
}

/**
 * @brief Main test runner task
 */
static void test_runner_task(void *pvParameters)
{
    test_stats_t total_stats = {0};
    test_stats_t suite_stats;

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  ESP32-C5 Zigbee2MQTT Gateway Test Suite");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "");

    /* Give system time to stabilize */
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* ========================================
     * UNIT TESTS
     * ======================================== */
    print_banner("UNIT TESTS");

#define RUN_SUITE(fn, label)                       \
    do {                                           \
        test_reset_stats();                        \
        suite_stats = fn();                        \
        print_suite_result((label), suite_stats);  \
        combine_stats(&total_stats, suite_stats);  \
        vTaskDelay(pdMS_TO_TICKS(100));            \
    } while (0)

#ifdef TEST_SUITE_MEMORY_MANAGER
    RUN_SUITE(run_memory_manager_tests, "Memory Manager");
#endif
#ifdef TEST_SUITE_CONFIG_MANAGER
    RUN_SUITE(run_config_manager_tests, "Config Manager");
#endif
#ifdef TEST_SUITE_JSON_UTILS
    RUN_SUITE(run_json_utils_tests, "JSON Utilities");
#endif
#ifdef TEST_SUITE_VERSION
    RUN_SUITE(run_version_tests, "Version Management");
#endif
#ifdef TEST_SUITE_DEVICE_REGISTRY
    RUN_SUITE(run_device_registry_tests, "Device Registry");
#endif
#ifdef TEST_SUITE_ZB_DIAGNOSTICS
    RUN_SUITE(run_zb_diagnostics_tests, "Zigbee Diagnostics");
#endif
#ifdef TEST_SUITE_ZB_BACKUP
    RUN_SUITE(run_zb_backup_tests, "Zigbee Backup");
    RUN_SUITE(run_zb_tuya_parse_tests, "Tuya Datapoint Parser");
#endif
#ifdef TEST_SUITE_ESPHOME_PROTOCOL
    RUN_SUITE(run_esphome_protocol_tests, "ESPHome Protocol");
    RUN_SUITE(run_esphome_execute_service_tests, "ESPHome ExecuteService");
#endif
#ifdef TEST_SUITE_ESPHOME_ENTITY_MIRROR
    RUN_SUITE(run_esphome_entity_mirror_tests, "ESPHome Entity Mirror");
#endif
#ifdef TEST_SUITE_ESPHOME_NOISE
    RUN_SUITE(run_esphome_noise_tests, "ESPHome Noise");
#endif
#ifdef TEST_SUITE_MQTT_TOPICS
    RUN_SUITE(run_mqtt_topics_tests, "MQTT Topics");
#endif

    /* ========================================
     * INTEGRATION TESTS
     * ======================================== */
#if defined(TEST_SUITE_WIFI_MQTT) || defined(TEST_SUITE_ZIGBEE_BRIDGE) || \
    defined(TEST_SUITE_OTA)
    print_banner("INTEGRATION TESTS");
#endif

#ifdef TEST_SUITE_WIFI_MQTT
    RUN_SUITE(run_wifi_mqtt_integration_tests, "WiFi + MQTT Integration");
#endif
#ifdef TEST_SUITE_ZIGBEE_BRIDGE
    RUN_SUITE(run_zigbee_mqtt_bridge_tests, "Zigbee-MQTT Bridge");
#endif
#ifdef TEST_SUITE_OTA
    RUN_SUITE(run_ota_tests, "OTA Updates");
#endif

    /* ========================================
     * FINAL SUMMARY
     * ======================================== */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  FINAL TEST SUMMARY");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Total Tests:   %lu", (unsigned long)total_stats.total);
    ESP_LOGI(TAG, "Passed:        %lu", (unsigned long)total_stats.passed);
    if (total_stats.failed > 0) {
        ESP_LOGE(TAG, "FAILED:        %lu", (unsigned long)total_stats.failed);
    } else {
        ESP_LOGI(TAG, "FAILED:        %lu", (unsigned long)total_stats.failed);
    }
    if (total_stats.skipped > 0) {
        ESP_LOGW(TAG, "Skipped:       %lu", (unsigned long)total_stats.skipped);
    }
    ESP_LOGI(TAG, "================================================");

    if (total_stats.failed == 0) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "*** ALL TESTS PASSED ***");
        ESP_LOGI(TAG, "");
    } else {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "*** %lu TEST(S) FAILED ***", (unsigned long)total_stats.failed);
        ESP_LOGE(TAG, "");
    }

    /* Print memory usage */
    ESP_LOGI(TAG, "Final heap free: %lu bytes", (unsigned long)esp_get_free_heap_size());

    /* Keep task alive for a moment */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Delete task */
    vTaskDelete(NULL);
}

/**
 * @brief Application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Initializing test environment...");

    /* Initialize NVS.
     *
     * Deliberately NOT the usual "on NO_FREE_PAGES, erase and retry" dance.
     * This test binary shares the gateway's flash layout, so nvs_flash_erase()
     * here would wipe the real NVS partition — device pairings, WiFi
     * credentials, Tuya bindings, the lot — just because someone ran the tests.
     * Losing that to a test run is not an acceptable trade.
     *
     * If NVS is genuinely unusable, say so and carry on: none of the suites
     * currently need it, and a test run must never be destructive. */
    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable (%s) — continuing without it. "
                      "NOT erasing: this binary shares the gateway's partitions.",
                 esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Starting test runner...");

    /* Create test runner task */
    xTaskCreate(test_runner_task, "test_runner", 8192, NULL, 5, NULL);
}
