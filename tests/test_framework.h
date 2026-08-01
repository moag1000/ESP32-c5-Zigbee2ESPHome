/**
 * @file test_framework.h
 * @brief Lightweight Testing Framework for ESP32-C5 Zigbee2MQTT Gateway
 *
 * Provides simple, efficient unit testing for embedded systems.
 * No external dependencies - works standalone on ESP-IDF.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Test result codes
 */
typedef enum {
    TEST_PASS = 0,
    TEST_FAIL = 1,
    TEST_SKIP = 2
} test_result_t;

/**
 * @brief Test function pointer
 */
typedef void (*test_func_t)(void);

/**
 * @brief Test case structure
 */
typedef struct {
    const char *name;       /**< Test case name */
    test_func_t func;       /**< Test function pointer */
} test_case_t;

/**
 * @brief Test suite statistics
 */
typedef struct {
    uint32_t total;         /**< Total tests run */
    uint32_t passed;        /**< Tests passed */
    uint32_t failed;        /**< Tests failed */
    uint32_t skipped;       /**< Tests skipped */
} test_stats_t;

/**
 * @brief Test assertion macros
 */
#define TEST_ASSERT(condition) \
    test_assert_impl(__FILE__, __LINE__, #condition, condition)

#define TEST_ASSERT_TRUE(condition) \
    TEST_ASSERT(condition)

#define TEST_ASSERT_FALSE(condition) \
    TEST_ASSERT(!(condition))

#define TEST_ASSERT_EQUAL(expected, actual) \
    test_assert_equal_impl(__FILE__, __LINE__, expected, actual)

#define TEST_ASSERT_NOT_EQUAL(expected, actual) \
    test_assert_not_equal_impl(__FILE__, __LINE__, expected, actual)

#define TEST_ASSERT_NULL(ptr) \
    test_assert_null_impl(__FILE__, __LINE__, #ptr, ptr)

#define TEST_ASSERT_NOT_NULL(ptr) \
    test_assert_not_null_impl(__FILE__, __LINE__, #ptr, ptr)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) \
    test_assert_equal_string_impl(__FILE__, __LINE__, expected, actual)

#define TEST_ASSERT_EQUAL_MEMORY(expected, actual, size) \
    test_assert_equal_memory_impl(__FILE__, __LINE__, expected, actual, size)

#define TEST_ASSERT_EQUAL_UINT8(expected, actual) \
    test_assert_equal_uint8_impl(__FILE__, __LINE__, expected, actual)

#define TEST_ASSERT_EQUAL_UINT16(expected, actual) \
    test_assert_equal_uint16_impl(__FILE__, __LINE__, expected, actual)

#define TEST_ASSERT_EQUAL_UINT32(expected, actual) \
    test_assert_equal_uint32_impl(__FILE__, __LINE__, expected, actual)

#define TEST_ASSERT_GREATER_THAN(threshold, actual) \
    test_assert_greater_than_impl(__FILE__, __LINE__, threshold, actual)

#define TEST_ASSERT_LESS_THAN(threshold, actual) \
    test_assert_less_than_impl(__FILE__, __LINE__, threshold, actual)

#define TEST_FAIL_MESSAGE(msg) \
    test_fail_message_impl(__FILE__, __LINE__, msg)

/**
 * @brief Run a single test
 *
 * @param[in] name Test name
 * @param[in] func Test function
 */
void test_run(const char *name, test_func_t func);

/**
 * @brief Run a test suite
 *
 * @param[in] tests Array of test cases
 * @param[in] count Number of tests
 * @return Test statistics
 */
test_stats_t test_run_suite(const test_case_t *tests, size_t count);

/**
 * @brief Get current test statistics
 *
 * @return Test statistics
 */
test_stats_t test_get_stats(void);

/**
 * @brief Reset test statistics
 */
void test_reset_stats(void);

/**
 * @brief Print test summary
 */
void test_print_summary(void);

/**
 * @brief Implementation functions (do not call directly)
 */
void test_assert_impl(const char *file, int line, const char *expr, bool condition);
void test_assert_equal_impl(const char *file, int line, int expected, int actual);
void test_assert_not_equal_impl(const char *file, int line, int expected, int actual);
void test_assert_null_impl(const char *file, int line, const char *expr, const void *ptr);
void test_assert_not_null_impl(const char *file, int line, const char *expr, const void *ptr);
void test_assert_equal_string_impl(const char *file, int line, const char *expected, const char *actual);
void test_assert_equal_memory_impl(const char *file, int line, const void *expected, const void *actual, size_t size);
void test_assert_equal_uint8_impl(const char *file, int line, uint8_t expected, uint8_t actual);
void test_assert_equal_uint16_impl(const char *file, int line, uint16_t expected, uint16_t actual);
void test_assert_equal_uint32_impl(const char *file, int line, uint32_t expected, uint32_t actual);
void test_assert_greater_than_impl(const char *file, int line, int threshold, int actual);
void test_assert_less_than_impl(const char *file, int line, int threshold, int actual);
void test_fail_message_impl(const char *file, int line, const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* TEST_FRAMEWORK_H */
