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

/* External test suite runners */
extern test_stats_t run_memory_manager_tests(void);
extern test_stats_t run_config_manager_tests(void);
extern test_stats_t run_json_utils_tests(void);
extern test_stats_t run_version_tests(void);
extern test_stats_t run_mqtt_topics_tests(void);
extern test_stats_t run_wifi_mqtt_integration_tests(void);
extern test_stats_t run_zigbee_mqtt_bridge_tests(void);
extern test_stats_t run_ota_tests(void);

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

    /* Memory Manager Tests */
    test_reset_stats();
    suite_stats = run_memory_manager_tests();
    print_suite_result("Memory Manager", suite_stats);
    combine_stats(&total_stats, suite_stats);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Config Manager Tests */
    test_reset_stats();
    suite_stats = run_config_manager_tests();
    print_suite_result("Config Manager", suite_stats);
    combine_stats(&total_stats, suite_stats);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* JSON Utils Tests */
    test_reset_stats();
    suite_stats = run_json_utils_tests();
    print_suite_result("JSON Utilities", suite_stats);
    combine_stats(&total_stats, suite_stats);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Version Tests */
    test_reset_stats();
    suite_stats = run_version_tests();
    print_suite_result("Version Management", suite_stats);
    combine_stats(&total_stats, suite_stats);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* MQTT Topics Tests */
    test_reset_stats();
    suite_stats = run_mqtt_topics_tests();
    print_suite_result("MQTT Topics", suite_stats);
    combine_stats(&total_stats, suite_stats);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ========================================
     * INTEGRATION TESTS
     * ======================================== */
    print_banner("INTEGRATION TESTS");

    /* WiFi + MQTT Integration Tests */
    test_reset_stats();
    suite_stats = run_wifi_mqtt_integration_tests();
    print_suite_result("WiFi + MQTT Integration", suite_stats);
    combine_stats(&total_stats, suite_stats);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Zigbee-MQTT Bridge Tests */
    test_reset_stats();
    suite_stats = run_zigbee_mqtt_bridge_tests();
    print_suite_result("Zigbee-MQTT Bridge", suite_stats);
    combine_stats(&total_stats, suite_stats);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* OTA Tests */
    test_reset_stats();
    suite_stats = run_ota_tests();
    print_suite_result("OTA Updates", suite_stats);
    combine_stats(&total_stats, suite_stats);
    vTaskDelay(pdMS_TO_TICKS(100));

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

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Starting test runner...");

    /* Create test runner task */
    xTaskCreate(test_runner_task, "test_runner", 8192, NULL, 5, NULL);
}
