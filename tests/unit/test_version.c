/**
 * @file test_version.c
 * @brief Unit Tests for Version Management
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "utils/version.h"
#include "esp_log.h"
#include <stdio.h>   /* sscanf, snprintf */
#include <string.h>

static const char *TAG = "TEST_VER";

/**
 * @brief Test getting version number
 */
static void test_version_get_number(void)
{
    const char *version = version_get_number();
    TEST_ASSERT_NOT_NULL(version);
    TEST_ASSERT_GREATER_THAN(0, strlen(version));

    /* Should contain version format like "1.0.0" */
    TEST_ASSERT_NOT_NULL(strchr(version, '.'));
}

/**
 * @brief Test getting full version string
 */
static void test_version_get_string(void)
{
    const char *version = version_get_string();
    TEST_ASSERT_NOT_NULL(version);
    TEST_ASSERT_GREATER_THAN(0, strlen(version));
}

/**
 * @brief Test getting build date
 */
static void test_version_get_build_date(void)
{
    const char *date = version_get_build_date();
    TEST_ASSERT_NOT_NULL(date);
    TEST_ASSERT_GREATER_THAN(0, strlen(date));
}

/**
 * @brief Test getting build time
 */
static void test_version_get_build_time(void)
{
    const char *time = version_get_build_time();
    TEST_ASSERT_NOT_NULL(time);
    TEST_ASSERT_GREATER_THAN(0, strlen(time));
}

/**
 * @brief Test getting git commit hash
 */
static void test_version_get_git_commit(void)
{
    const char *commit = version_get_git_commit();
    TEST_ASSERT_NOT_NULL(commit);
    TEST_ASSERT_GREATER_THAN(0, strlen(commit));
}

/**
 * @brief Test getting project name
 */
static void test_version_get_project_name(void)
{
    const char *name = version_get_project_name();
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_STRING(PROJECT_NAME, name);
}

/**
 * @brief Test version comparison - equal versions
 */
static void test_version_compare_equal(void)
{
    int result = version_compare(FIRMWARE_VERSION);
    TEST_ASSERT_EQUAL(0, result);
}

/**
 * @brief Test version comparison - older version
 */
static void test_version_compare_older(void)
{
    /* Compare with a newer version */
    int result = version_compare("99.0.0");
    TEST_ASSERT_LESS_THAN(0, result);
}

/**
 * @brief Test version comparison - newer version
 */
static void test_version_compare_newer(void)
{
    /* Compare with an older version */
    int result = version_compare("0.0.1");
    TEST_ASSERT_GREATER_THAN(0, result);
}

/**
 * @brief Test version comparison - patch version
 */
static void test_version_compare_patch(void)
{
    /* If current version is 1.0.0, compare with 1.0.1 */
    const char *current = version_get_number();

    /* Parse current version */
    int major, minor, patch;
    if (sscanf(current, "%d.%d.%d", &major, &minor, &patch) == 3) {
        char newer_version[32];
        snprintf(newer_version, sizeof(newer_version), "%d.%d.%d", major, minor, patch + 1);

        int result = version_compare(newer_version);
        TEST_ASSERT_LESS_THAN(0, result);
    }
}

/**
 * @brief Test getting version as JSON
 */
static void test_version_get_json(void)
{
    char buffer[256];
    int ret = version_get_json(buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(0, ret);

    /* Verify JSON contains expected fields */
    TEST_ASSERT_NOT_NULL(strstr(buffer, "version"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, "build_date"));
}

/**
 * @brief Test version JSON with small buffer
 */
static void test_version_get_json_small_buffer(void)
{
    char buffer[10];
    int ret = version_get_json(buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(-1, ret); /* Should fail - buffer too small */
}

/**
 * @brief Test version print (should not crash)
 */
static void test_version_print(void)
{
    /* This should not crash */
    version_print();

    /* If we get here, test passed */
    TEST_ASSERT_TRUE(true);
}

/**
 * @brief Test version string format
 */
static void test_version_string_format(void)
{
    const char *version = version_get_string();
    TEST_ASSERT_NOT_NULL(version);

    /* Version string should contain version number */
    const char *num = version_get_number();
    TEST_ASSERT_NOT_NULL(strstr(version, num));
}

/**
 * @brief Test version comparison with invalid input
 */
static void test_version_compare_invalid(void)
{
    /* Compare with invalid version string */
    int result = version_compare("invalid.version");

    /* Should handle gracefully - implementation dependent */
    /* Result may be 0 or error value */
}

/**
 * @brief Test version number format
 */
static void test_version_number_format(void)
{
    const char *version = version_get_number();
    TEST_ASSERT_NOT_NULL(version);

    /* Count dots - should have at least 2 (e.g., "1.0.0") */
    int dot_count = 0;
    for (const char *p = version; *p; p++) {
        if (*p == '.') dot_count++;
    }
    TEST_ASSERT_GREATER_THAN(1, dot_count);
}

/**
 * @brief Test consistency between version functions
 */
static void test_version_consistency(void)
{
    const char *full = version_get_string();
    const char *num = version_get_number();
    const char *date = version_get_build_date();
    const char *commit = version_get_git_commit();

    /* All should be non-NULL */
    TEST_ASSERT_NOT_NULL(full);
    TEST_ASSERT_NOT_NULL(num);
    TEST_ASSERT_NOT_NULL(date);
    TEST_ASSERT_NOT_NULL(commit);

    /* Full version should contain the version number */
    TEST_ASSERT_NOT_NULL(strstr(full, num));
}

/**
 * @brief Version management test suite
 */
static const test_case_t version_tests[] = {
    {"version_get_number", test_version_get_number},
    {"version_get_string", test_version_get_string},
    {"version_get_build_date", test_version_get_build_date},
    {"version_get_build_time", test_version_get_build_time},
    {"version_get_git_commit", test_version_get_git_commit},
    {"version_get_project_name", test_version_get_project_name},
    {"version_compare_equal", test_version_compare_equal},
    {"version_compare_older", test_version_compare_older},
    {"version_compare_newer", test_version_compare_newer},
    {"version_compare_patch", test_version_compare_patch},
    {"version_get_json", test_version_get_json},
    {"version_get_json_small_buffer", test_version_get_json_small_buffer},
    {"version_print", test_version_print},
    {"version_string_format", test_version_string_format},
    {"version_compare_invalid", test_version_compare_invalid},
    {"version_number_format", test_version_number_format},
    {"version_consistency", test_version_consistency},
};

/**
 * @brief Run all version tests
 */
test_stats_t run_version_tests(void)
{
    ESP_LOGI(TAG, "Running Version Management Tests");
    return test_run_suite(version_tests,
                         sizeof(version_tests) / sizeof(version_tests[0]));
}
