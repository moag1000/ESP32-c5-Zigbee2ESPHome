/**
 * @file test_framework.c
 * @brief Lightweight Testing Framework Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TEST";

/* Global test statistics */
static test_stats_t s_stats = {0};

/* Current test tracking */
static bool s_current_test_failed = false;
static const char *s_current_test_name = NULL;

/**
 * @brief Reset test statistics
 */
void test_reset_stats(void)
{
    memset(&s_stats, 0, sizeof(test_stats_t));
}

/**
 * @brief Get current test statistics
 */
test_stats_t test_get_stats(void)
{
    return s_stats;
}

/**
 * @brief Run a single test
 */
void test_run(const char *name, test_func_t func)
{
    if (!name || !func) {
        return;
    }

    s_current_test_name = name;
    s_current_test_failed = false;
    s_stats.total++;

    ESP_LOGI(TAG, "Running test: %s", name);

    /* Run the test function */
    func();

    /* Update statistics */
    if (s_current_test_failed) {
        s_stats.failed++;
        ESP_LOGE(TAG, "Test FAILED: %s", name);
    } else {
        s_stats.passed++;
        ESP_LOGI(TAG, "Test PASSED: %s", name);
    }

    s_current_test_name = NULL;
}

/**
 * @brief Run a test suite
 */
test_stats_t test_run_suite(const test_case_t *tests, size_t count)
{
    if (!tests) {
        return s_stats;
    }

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "Running test suite: %zu tests", count);
    ESP_LOGI(TAG, "==========================================");

    for (size_t i = 0; i < count; i++) {
        test_run(tests[i].name, tests[i].func);

        /* Brief delay between tests for system stability */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return s_stats;
}

/**
 * @brief Print test summary
 */
void test_print_summary(void)
{
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "Test Summary:");
    ESP_LOGI(TAG, "  Total:   %lu", (unsigned long)s_stats.total);
    ESP_LOGI(TAG, "  Passed:  %lu", (unsigned long)s_stats.passed);
    ESP_LOGI(TAG, "  Failed:  %lu", (unsigned long)s_stats.failed);
    ESP_LOGI(TAG, "  Skipped: %lu", (unsigned long)s_stats.skipped);

    if (s_stats.failed == 0) {
        ESP_LOGI(TAG, "Result: ALL TESTS PASSED");
    } else {
        ESP_LOGE(TAG, "Result: %lu TEST(S) FAILED", (unsigned long)s_stats.failed);
    }
    ESP_LOGI(TAG, "==========================================");
}

/**
 * @brief Assert implementation - generic boolean
 */
void test_assert_impl(const char *file, int line, const char *expr, bool condition)
{
    if (!condition) {
        ESP_LOGE(TAG, "Assertion failed: %s", expr);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert equal implementation - integers
 */
void test_assert_equal_impl(const char *file, int line, int expected, int actual)
{
    if (expected != actual) {
        ESP_LOGE(TAG, "Assertion failed: expected %d, got %d", expected, actual);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert not equal implementation - integers
 */
void test_assert_not_equal_impl(const char *file, int line, int expected, int actual)
{
    if (expected == actual) {
        ESP_LOGE(TAG, "Assertion failed: expected not equal to %d", expected);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert NULL implementation
 */
void test_assert_null_impl(const char *file, int line, const char *expr, const void *ptr)
{
    if (ptr != NULL) {
        ESP_LOGE(TAG, "Assertion failed: %s is not NULL", expr);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert NOT NULL implementation
 */
void test_assert_not_null_impl(const char *file, int line, const char *expr, const void *ptr)
{
    if (ptr == NULL) {
        ESP_LOGE(TAG, "Assertion failed: %s is NULL", expr);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert equal string implementation
 */
void test_assert_equal_string_impl(const char *file, int line, const char *expected, const char *actual)
{
    if (!expected || !actual) {
        ESP_LOGE(TAG, "Assertion failed: NULL string comparison");
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
        return;
    }

    if (strcmp(expected, actual) != 0) {
        ESP_LOGE(TAG, "Assertion failed: strings not equal");
        ESP_LOGE(TAG, "  Expected: \"%s\"", expected);
        ESP_LOGE(TAG, "  Actual:   \"%s\"", actual);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert equal memory implementation
 */
void test_assert_equal_memory_impl(const char *file, int line, const void *expected, const void *actual, size_t size)
{
    if (!expected || !actual) {
        ESP_LOGE(TAG, "Assertion failed: NULL memory comparison");
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
        return;
    }

    if (memcmp(expected, actual, size) != 0) {
        ESP_LOGE(TAG, "Assertion failed: memory not equal (%zu bytes)", size);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert equal uint8 implementation
 */
void test_assert_equal_uint8_impl(const char *file, int line, uint8_t expected, uint8_t actual)
{
    if (expected != actual) {
        ESP_LOGE(TAG, "Assertion failed: expected 0x%02X, got 0x%02X", expected, actual);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert equal uint16 implementation
 */
void test_assert_equal_uint16_impl(const char *file, int line, uint16_t expected, uint16_t actual)
{
    if (expected != actual) {
        ESP_LOGE(TAG, "Assertion failed: expected 0x%04X, got 0x%04X", expected, actual);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert equal uint32 implementation
 */
void test_assert_equal_uint32_impl(const char *file, int line, uint32_t expected, uint32_t actual)
{
    if (expected != actual) {
        ESP_LOGE(TAG, "Assertion failed: expected 0x%08lX, got 0x%08lX",
                 (unsigned long)expected, (unsigned long)actual);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert greater than implementation
 */
void test_assert_greater_than_impl(const char *file, int line, int threshold, int actual)
{
    if (actual <= threshold) {
        ESP_LOGE(TAG, "Assertion failed: %d is not greater than %d", actual, threshold);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Assert less than implementation
 */
void test_assert_less_than_impl(const char *file, int line, int threshold, int actual)
{
    if (actual >= threshold) {
        ESP_LOGE(TAG, "Assertion failed: %d is not less than %d", actual, threshold);
        ESP_LOGE(TAG, "  at %s:%d", file, line);
        s_current_test_failed = true;
    }
}

/**
 * @brief Fail with message implementation
 */
void test_fail_message_impl(const char *file, int line, const char *msg)
{
    ESP_LOGE(TAG, "Test failed: %s", msg ? msg : "no message");
    ESP_LOGE(TAG, "  at %s:%d", file, line);
    s_current_test_failed = true;
}
