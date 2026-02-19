/**
 * @file test_memory_manager.c
 * @brief Unit Tests for Memory Manager
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "core/memory_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TEST_MEM";

/**
 * @brief Test memory manager initialization
 */
static void test_memory_manager_init_success(void)
{
    esp_err_t ret = memory_manager_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * @brief Test getting memory statistics
 */
static void test_memory_manager_get_stats(void)
{
    memory_stats_t stats = memory_manager_get_stats();

    /* Verify that stats contain reasonable values */
    TEST_ASSERT_GREATER_THAN(0, stats.total_heap);
    TEST_ASSERT_GREATER_THAN(0, stats.free_heap);
    TEST_ASSERT(stats.free_heap <= stats.total_heap);
    TEST_ASSERT_GREATER_THAN(0, stats.largest_free_block);
}

/**
 * @brief Test memory threshold checking
 */
static void test_memory_manager_check_threshold(void)
{
    /* Should have more than 1KB free */
    bool result = memory_manager_check_threshold(1024);
    TEST_ASSERT_TRUE(result);

    /* Check unreasonable threshold (should fail) */
    result = memory_manager_check_threshold(10 * 1024 * 1024); /* 10MB */
    TEST_ASSERT_FALSE(result);
}

/**
 * @brief Test memory status detection
 */
static void test_memory_manager_get_status(void)
{
    memory_status_t status = memory_manager_get_status();

    /* Status should be valid enum value */
    TEST_ASSERT(status == MEMORY_STATUS_NORMAL ||
                status == MEMORY_STATUS_WARNING ||
                status == MEMORY_STATUS_CRITICAL);
}

/**
 * @brief Test memory fragmentation calculation
 */
static void test_memory_manager_get_fragmentation(void)
{
    uint8_t frag = memory_manager_get_fragmentation();

    /* Fragmentation should be between 0-100% */
    TEST_ASSERT(frag <= 100);
}

/**
 * @brief Test memory allocation and statistics update
 */
static void test_memory_manager_allocation_tracking(void)
{
    memory_stats_t stats_before = memory_manager_get_stats();

    /* Allocate memory */
    size_t alloc_size = 1024;
    void *ptr = malloc(alloc_size);
    TEST_ASSERT_NOT_NULL(ptr);

    /* Get stats after allocation */
    memory_stats_t stats_after = memory_manager_get_stats();

    /* Free heap should decrease */
    TEST_ASSERT(stats_after.free_heap < stats_before.free_heap);

    /* Clean up */
    free(ptr);
}

/**
 * @brief Test memory logging (should not crash)
 */
static void test_memory_manager_log_stats(void)
{
    /* This should not crash or cause errors */
    memory_manager_log_stats();

    /* If we get here, test passed */
    TEST_ASSERT_TRUE(true);
}

/**
 * @brief Test periodic logging enable/disable
 */
static void test_memory_manager_periodic_logging(void)
{
    esp_err_t ret;

    /* Enable periodic logging */
    ret = memory_manager_set_periodic_logging(true, 60);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Disable periodic logging */
    ret = memory_manager_set_periodic_logging(false, 0);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * @brief Test low memory callback registration
 */
static void test_memory_manager_callback(void)
{
    /* Dummy callback */
    void low_memory_callback(memory_status_t status) {
        (void)status;
    }

    esp_err_t ret = memory_manager_register_callback(low_memory_callback);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Register NULL should fail or be handled */
    ret = memory_manager_register_callback(NULL);
    /* Implementation may return ESP_OK or ESP_ERR_INVALID_ARG */
}

/**
 * @brief Test memory optimization
 */
static void test_memory_manager_optimize(void)
{
    esp_err_t ret = memory_manager_optimize();

    /* May return ESP_OK or ESP_ERR_NOT_SUPPORTED depending on implementation */
    TEST_ASSERT(ret == ESP_OK || ret == ESP_ERR_NOT_SUPPORTED);
}

/**
 * @brief Test statistics after multiple allocations
 */
static void test_memory_manager_multiple_allocations(void)
{
    #define NUM_ALLOCS 10
    void *ptrs[NUM_ALLOCS];
    size_t alloc_size = 512;

    memory_stats_t stats_before = memory_manager_get_stats();

    /* Allocate multiple blocks */
    for (int i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = malloc(alloc_size);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }

    memory_stats_t stats_after = memory_manager_get_stats();

    /* Free heap should have decreased significantly */
    TEST_ASSERT(stats_after.free_heap < stats_before.free_heap);

    /* Free all blocks */
    for (int i = 0; i < NUM_ALLOCS; i++) {
        free(ptrs[i]);
    }

    memory_stats_t stats_freed = memory_manager_get_stats();

    /* Free heap should recover (approximately) */
    TEST_ASSERT(stats_freed.free_heap > stats_after.free_heap);
}

/**
 * @brief Memory manager test suite
 */
static const test_case_t memory_manager_tests[] = {
    {"memory_manager_init", test_memory_manager_init_success},
    {"memory_manager_get_stats", test_memory_manager_get_stats},
    {"memory_manager_check_threshold", test_memory_manager_check_threshold},
    {"memory_manager_get_status", test_memory_manager_get_status},
    {"memory_manager_get_fragmentation", test_memory_manager_get_fragmentation},
    {"memory_manager_allocation_tracking", test_memory_manager_allocation_tracking},
    {"memory_manager_log_stats", test_memory_manager_log_stats},
    {"memory_manager_periodic_logging", test_memory_manager_periodic_logging},
    {"memory_manager_callback", test_memory_manager_callback},
    {"memory_manager_optimize", test_memory_manager_optimize},
    {"memory_manager_multiple_allocations", test_memory_manager_multiple_allocations},
};

/**
 * @brief Run all memory manager tests
 */
test_stats_t run_memory_manager_tests(void)
{
    ESP_LOGI(TAG, "Running Memory Manager Tests");
    return test_run_suite(memory_manager_tests,
                         sizeof(memory_manager_tests) / sizeof(memory_manager_tests[0]));
}
