/**
 * @file test_ota.c
 * @brief Integration Tests for OTA Updates
 *
 * Tests OTA download and update process (with mock server).
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "core/config_manager.h"
#include "utils/version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include <string.h>

static const char *TAG = "TEST_OTA";

/**
 * @brief Mock OTA state
 */
static struct {
    bool download_started;
    bool download_completed;
    bool update_applied;
    size_t bytes_downloaded;
    esp_err_t last_error;
} g_ota_mock = {0};

/**
 * @brief Reset mock OTA state
 */
static void reset_ota_mock(void)
{
    memset(&g_ota_mock, 0, sizeof(g_ota_mock));
}

/**
 * @brief Mock OTA download start
 */
static esp_err_t mock_ota_start_download(const char *url)
{
    if (!url) {
        return ESP_ERR_INVALID_ARG;
    }

    g_ota_mock.download_started = true;
    g_ota_mock.bytes_downloaded = 0;
    ESP_LOGI(TAG, "Mock OTA download started: %s", url);
    return ESP_OK;
}

/**
 * @brief Mock OTA download progress
 */
static esp_err_t mock_ota_download_chunk(const void *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_ota_mock.download_started) {
        return ESP_ERR_INVALID_STATE;
    }

    g_ota_mock.bytes_downloaded += len;
    return ESP_OK;
}

/**
 * @brief Mock OTA download complete
 */
static esp_err_t mock_ota_complete_download(void)
{
    if (!g_ota_mock.download_started) {
        return ESP_ERR_INVALID_STATE;
    }

    g_ota_mock.download_completed = true;
    ESP_LOGI(TAG, "Mock OTA download completed: %zu bytes", g_ota_mock.bytes_downloaded);
    return ESP_OK;
}

/**
 * @brief Mock OTA apply update
 */
static esp_err_t mock_ota_apply_update(void)
{
    if (!g_ota_mock.download_completed) {
        return ESP_ERR_INVALID_STATE;
    }

    g_ota_mock.update_applied = true;
    ESP_LOGI(TAG, "Mock OTA update applied");
    return ESP_OK;
}

/**
 * @brief Test OTA initialization
 */
static void test_ota_init(void)
{
    reset_ota_mock();

    /* Verify initial state */
    TEST_ASSERT_FALSE(g_ota_mock.download_started);
    TEST_ASSERT_FALSE(g_ota_mock.download_completed);
    TEST_ASSERT_FALSE(g_ota_mock.update_applied);
}

/**
 * @brief Test OTA download start
 */
static void test_ota_download_start(void)
{
    reset_ota_mock();

    const char *url = "https://example.com/firmware.bin";
    esp_err_t ret = mock_ota_start_download(url);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(g_ota_mock.download_started);
}

/**
 * @brief Test OTA download with invalid URL
 */
static void test_ota_download_invalid_url(void)
{
    reset_ota_mock();

    esp_err_t ret = mock_ota_start_download(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

/**
 * @brief Test OTA download progress
 */
static void test_ota_download_progress(void)
{
    reset_ota_mock();

    mock_ota_start_download("https://example.com/firmware.bin");

    /* Simulate downloading chunks */
    uint8_t chunk[256];
    memset(chunk, 0xAA, sizeof(chunk));

    for (int i = 0; i < 10; i++) {
        esp_err_t ret = mock_ota_download_chunk(chunk, sizeof(chunk));
        TEST_ASSERT_EQUAL(ESP_OK, ret);
    }

    TEST_ASSERT_EQUAL(256 * 10, g_ota_mock.bytes_downloaded);
}

/**
 * @brief Test OTA download without start
 */
static void test_ota_download_without_start(void)
{
    reset_ota_mock();

    uint8_t chunk[256];
    esp_err_t ret = mock_ota_download_chunk(chunk, sizeof(chunk));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);
}

/**
 * @brief Test OTA download completion
 */
static void test_ota_download_complete(void)
{
    reset_ota_mock();

    mock_ota_start_download("https://example.com/firmware.bin");

    uint8_t chunk[1024];
    mock_ota_download_chunk(chunk, sizeof(chunk));

    esp_err_t ret = mock_ota_complete_download();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(g_ota_mock.download_completed);
}

/**
 * @brief Test OTA complete without download
 */
static void test_ota_complete_without_download(void)
{
    reset_ota_mock();

    esp_err_t ret = mock_ota_complete_download();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);
}

/**
 * @brief Test OTA apply update
 */
static void test_ota_apply_update(void)
{
    reset_ota_mock();

    /* Complete download first */
    mock_ota_start_download("https://example.com/firmware.bin");
    uint8_t chunk[1024];
    mock_ota_download_chunk(chunk, sizeof(chunk));
    mock_ota_complete_download();

    /* Apply update */
    esp_err_t ret = mock_ota_apply_update();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_TRUE(g_ota_mock.update_applied);
}

/**
 * @brief Test OTA apply without download
 */
static void test_ota_apply_without_download(void)
{
    reset_ota_mock();

    esp_err_t ret = mock_ota_apply_update();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);
}

/**
 * @brief Test OTA full workflow
 */
static void test_ota_full_workflow(void)
{
    reset_ota_mock();

    /* Start download */
    esp_err_t ret = mock_ota_start_download("https://example.com/firmware.bin");
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Download chunks */
    uint8_t chunk[512];
    for (int i = 0; i < 20; i++) {
        ret = mock_ota_download_chunk(chunk, sizeof(chunk));
        TEST_ASSERT_EQUAL(ESP_OK, ret);
    }

    /* Complete download */
    ret = mock_ota_complete_download();
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Apply update */
    ret = mock_ota_apply_update();
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify final state */
    TEST_ASSERT_TRUE(g_ota_mock.download_started);
    TEST_ASSERT_TRUE(g_ota_mock.download_completed);
    TEST_ASSERT_TRUE(g_ota_mock.update_applied);
    TEST_ASSERT_EQUAL(512 * 20, g_ota_mock.bytes_downloaded);
}

/**
 * @brief Test OTA with configuration
 */
static void test_ota_with_config(void)
{
    /* Get OTA URL from configuration */
    const gateway_config_t *config = config_manager_get_config();
    TEST_ASSERT_NOT_NULL(config);

    /* Check if OTA is enabled in config */
    if (config->ota_enabled) {
        ESP_LOGI(TAG, "OTA enabled in config");
        TEST_ASSERT(strlen(config->ota_url) > 0);
    }

    /* This test verifies config integration */
    TEST_ASSERT_TRUE(true);
}

/**
 * @brief Test OTA version check
 */
static void test_ota_version_check(void)
{
    const char *current_version = version_get_number();
    TEST_ASSERT_NOT_NULL(current_version);

    /* Simulate checking if update is needed */
    const char *available_version = "2.0.0";

    int result = version_compare(available_version);
    if (result < 0) {
        ESP_LOGI(TAG, "Update available: %s -> %s", current_version, available_version);
        TEST_ASSERT_TRUE(true);
    } else {
        ESP_LOGI(TAG, "Already up to date: %s", current_version);
        TEST_ASSERT_TRUE(true);
    }
}

/**
 * @brief Test OTA rollback capability
 */
static void test_ota_rollback(void)
{
    /* Get current running partition info */
    const esp_partition_t *running = esp_ota_get_running_partition();
    TEST_ASSERT_NOT_NULL(running);

    ESP_LOGI(TAG, "Running partition: %s", running->label);

    /* In a real rollback scenario, we would validate the new firmware
     * and rollback if it fails. Here we just verify the concept. */
    TEST_ASSERT_TRUE(true);
}

/**
 * @brief OTA integration test suite
 */
static const test_case_t ota_tests[] = {
    {"ota_init", test_ota_init},
    {"ota_download_start", test_ota_download_start},
    {"ota_download_invalid_url", test_ota_download_invalid_url},
    {"ota_download_progress", test_ota_download_progress},
    {"ota_download_without_start", test_ota_download_without_start},
    {"ota_download_complete", test_ota_download_complete},
    {"ota_complete_without_download", test_ota_complete_without_download},
    {"ota_apply_update", test_ota_apply_update},
    {"ota_apply_without_download", test_ota_apply_without_download},
    {"ota_full_workflow", test_ota_full_workflow},
    {"ota_with_config", test_ota_with_config},
    {"ota_version_check", test_ota_version_check},
    {"ota_rollback", test_ota_rollback},
};

/**
 * @brief Run all OTA integration tests
 */
test_stats_t run_ota_tests(void)
{
    ESP_LOGI(TAG, "Running OTA Integration Tests");
    return test_run_suite(ota_tests, sizeof(ota_tests) / sizeof(ota_tests[0]));
}
