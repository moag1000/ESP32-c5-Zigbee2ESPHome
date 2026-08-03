/**
 * @file test_device_registry.c
 * @brief Tests for device_registry — focused on the concurrency-safety contract
 *
 * These pin down the behaviour that was changed to stop the registry handing
 * out pointers other tasks can have pulled out from under them:
 *
 *   - device_registry_snapshot_ids() / _release_ids(), which replaced the
 *     count()+get_by_index() loops that skipped or repeated devices whenever
 *     the set changed mid-iteration
 *   - device_registry_state_dup(), which hands back an owned copy instead of
 *     the registry's live cJSON — set_state() and remove() both delete that
 *     object, so a cross-task reader was holding freed heap
 *   - round-robin slot allocation, so a slot freed by remove() is not handed
 *     straight to the next device that joins
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "core/memory/memory_manager_ng.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "TEST_REG";

#define ID_A  0x00158D0000000001ULL
#define ID_B  0x00158D0000000002ULL
#define ID_C  0x00158D0000000003ULL

/** Bring the registry up (if needed) and empty it, so tests do not leak into
 *  each other. */
static void reset_registry(void)
{
    /* The memory manager must come up first. device_registry_init() allocates
     * its arrays through mem_ng_calloc(), which takes the memory manager's own
     * mutex — and that mutex is NULL until mem_manager_init() has run. Calling
     * the registry first aborts with
     * "assert failed: xQueueSemaphoreTake queue.c:1709 ((pxQueue))".
     * In production foundation_init.c enforces this order; a test harness has
     * to reproduce it. Both inits are idempotent. */
    mem_manager_init();

    if (!device_registry_is_initialized()) {
        device_registry_init();
    }
    device_registry_clear_all();
}

/* ============================================================================
 * Basics — these guard the fixtures the rest of the file relies on
 * ============================================================================ */

static void test_add_then_get(void)
{
    reset_registry();

    device_t *added = device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);
    TEST_ASSERT_NOT_NULL(added);

    device_t *found = device_registry_get(ID_A);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_TRUE(added == found);
    TEST_ASSERT_TRUE(found->in_use);
}

static void test_get_unknown_returns_null(void)
{
    reset_registry();
    TEST_ASSERT_NULL(device_registry_get(0xDEADBEEFCAFEBABEULL));
}

static void test_remove_makes_it_unfindable(void)
{
    reset_registry();

    TEST_ASSERT_NOT_NULL(device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE));
    TEST_ASSERT_EQUAL(ESP_OK, device_registry_remove(ID_A));
    TEST_ASSERT_NULL(device_registry_get(ID_A));
}

static void test_count_tracks_add_and_remove(void)
{
    reset_registry();
    TEST_ASSERT_EQUAL(0, (int)device_registry_count());

    device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);
    device_registry_add(ID_B, DEV_PROTOCOL_ZIGBEE);
    TEST_ASSERT_EQUAL(2, (int)device_registry_count());

    device_registry_remove(ID_A);
    TEST_ASSERT_EQUAL(1, (int)device_registry_count());
}

/* ============================================================================
 * snapshot_ids — replaces count() + get_by_index()
 * ============================================================================ */

static void test_snapshot_empty_registry(void)
{
    reset_registry();

    size_t n = 12345;  /* deliberately not zero */
    device_id_t *ids = device_registry_snapshot_ids(&n);

    TEST_ASSERT_NULL(ids);
    TEST_ASSERT_EQUAL(0, (int)n);   /* must be reset even when nothing is found */

    device_registry_release_ids(ids);  /* NULL must be safe */
}

static void test_snapshot_lists_every_device(void)
{
    reset_registry();
    device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);
    device_registry_add(ID_B, DEV_PROTOCOL_ZIGBEE);
    device_registry_add(ID_C, DEV_PROTOCOL_ZIGBEE);

    size_t n = 0;
    device_id_t *ids = device_registry_snapshot_ids(&n);
    TEST_ASSERT_NOT_NULL(ids);
    TEST_ASSERT_EQUAL(3, (int)n);

    bool seen_a = false, seen_b = false, seen_c = false;
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == ID_A) seen_a = true;
        if (ids[i] == ID_B) seen_b = true;
        if (ids[i] == ID_C) seen_c = true;
        /* every snapshotted id must still resolve */
        TEST_ASSERT_NOT_NULL(device_registry_get(ids[i]));
    }
    TEST_ASSERT_TRUE(seen_a);
    TEST_ASSERT_TRUE(seen_b);
    TEST_ASSERT_TRUE(seen_c);

    device_registry_release_ids(ids);
}

static void test_snapshot_reflects_removal(void)
{
    reset_registry();
    device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);
    device_registry_add(ID_B, DEV_PROTOCOL_ZIGBEE);
    device_registry_remove(ID_A);

    size_t n = 0;
    device_id_t *ids = device_registry_snapshot_ids(&n);
    TEST_ASSERT_NOT_NULL(ids);
    TEST_ASSERT_EQUAL(1, (int)n);
    TEST_ASSERT_TRUE(ids[0] == ID_B);

    device_registry_release_ids(ids);
}

/**
 * A device removed after the snapshot must simply drop out of the loop.
 * Callers rely on device_registry_get() returning NULL rather than a stale
 * or recycled entry — that is the documented contract for the pattern.
 */
static void test_snapshot_id_removed_afterwards_resolves_null(void)
{
    reset_registry();
    device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);
    device_registry_add(ID_B, DEV_PROTOCOL_ZIGBEE);

    size_t n = 0;
    device_id_t *ids = device_registry_snapshot_ids(&n);
    TEST_ASSERT_NOT_NULL(ids);
    TEST_ASSERT_EQUAL(2, (int)n);

    device_registry_remove(ID_A);

    int resolved = 0, dropped = 0;
    for (size_t i = 0; i < n; i++) {
        if (device_registry_get(ids[i]) != NULL) resolved++;
        else dropped++;
    }
    TEST_ASSERT_EQUAL(1, resolved);
    TEST_ASSERT_EQUAL(1, dropped);

    device_registry_release_ids(ids);
}

/* ============================================================================
 * state_dup — owned copy instead of the registry's live object
 * ============================================================================ */

static void test_state_dup_null_when_no_state(void)
{
    reset_registry();
    device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);

    TEST_ASSERT_NULL(device_registry_state_dup(ID_A));
    TEST_ASSERT_NULL(device_registry_state_dup(0xABCDEF0123456789ULL));
}

static void test_state_dup_returns_the_values(void)
{
    reset_registry();
    device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);

    cJSON *state = cJSON_CreateObject();
    cJSON_AddNumberToObject(state, "temperature", 21);
    TEST_ASSERT_EQUAL(ESP_OK, device_registry_set_state(ID_A, state));

    cJSON *copy = device_registry_state_dup(ID_A);
    TEST_ASSERT_NOT_NULL(copy);

    cJSON *t = cJSON_GetObjectItem(copy, "temperature");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL(21, t->valueint);

    cJSON_Delete(copy);
}

/** The copy must be a separate object, not an alias of the registry's. */
static void test_state_dup_is_independent(void)
{
    reset_registry();
    device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);

    cJSON *first = cJSON_CreateObject();
    cJSON_AddNumberToObject(first, "temperature", 21);
    device_registry_set_state(ID_A, first);

    cJSON *copy = device_registry_state_dup(ID_A);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_TRUE(copy != device_registry_get_state(ID_A));

    /* Replacing the state deletes the old object inside the registry. */
    cJSON *second = cJSON_CreateObject();
    cJSON_AddNumberToObject(second, "temperature", 99);
    device_registry_set_state(ID_A, second);

    /* Our copy must be untouched by that. */
    cJSON *t = cJSON_GetObjectItem(copy, "temperature");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL(21, t->valueint);

    cJSON_Delete(copy);
}

/**
 * The regression test for the use-after-free: device_registry_remove() calls
 * cJSON_Delete() on the state. A caller holding the borrowed pointer would be
 * reading freed heap here; a caller holding a dup must be fine.
 */
static void test_state_dup_survives_device_removal(void)
{
    reset_registry();
    device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);

    cJSON *state = cJSON_CreateObject();
    cJSON_AddNumberToObject(state, "battery", 66);
    device_registry_set_state(ID_A, state);

    cJSON *copy = device_registry_state_dup(ID_A);
    TEST_ASSERT_NOT_NULL(copy);

    TEST_ASSERT_EQUAL(ESP_OK, device_registry_remove(ID_A));
    TEST_ASSERT_NULL(device_registry_get(ID_A));

    cJSON *b = cJSON_GetObjectItem(copy, "battery");
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL(66, b->valueint);

    cJSON_Delete(copy);
}

/* ============================================================================
 * Slot allocation — round-robin instead of always scanning from 0
 * ============================================================================ */

/**
 * A slot freed by remove() must not go straight to the next device that
 * joins. Scanning from index 0 did exactly that, so a device_t* another task
 * still held silently started describing a different device.
 */
static void test_freed_slot_is_not_reused_immediately(void)
{
    reset_registry();

    device_t *a = device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);
    TEST_ASSERT_NOT_NULL(a);

    device_registry_remove(ID_A);

    device_t *b = device_registry_add(ID_B, DEV_PROTOCOL_ZIGBEE);
    TEST_ASSERT_NOT_NULL(b);

    TEST_ASSERT_TRUE(a != b);
}

/** Consecutive additions must land in distinct slots. */
static void test_consecutive_adds_get_distinct_slots(void)
{
    reset_registry();

    device_t *a = device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);
    device_t *b = device_registry_add(ID_B, DEV_PROTOCOL_ZIGBEE);
    device_t *c = device_registry_add(ID_C, DEV_PROTOCOL_ZIGBEE);

    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(a != b);
    TEST_ASSERT_TRUE(b != c);
    TEST_ASSERT_TRUE(a != c);
}

/** Adding the same id twice must not create a second entry. */
static void test_duplicate_add_returns_existing(void)
{
    reset_registry();

    device_t *first = device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);
    device_t *again = device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);

    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(again);
    TEST_ASSERT_TRUE(first == again);
    TEST_ASSERT_EQUAL(1, (int)device_registry_count());
}

/* ============================================================================
 * find_by_id — reachable from Home Assistant via remove_device
 * ============================================================================ */

static void test_find_by_friendly_name(void)
{
    reset_registry();
    device_t *dev = device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);
    TEST_ASSERT_NOT_NULL(dev);
    snprintf(dev->friendly_name, sizeof(dev->friendly_name), "Vibration Sensor");

    TEST_ASSERT_TRUE(device_registry_find_by_id("Vibration Sensor") == dev);
}

static void test_find_by_exact_ieee_string(void)
{
    reset_registry();
    device_t *dev = device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE);
    TEST_ASSERT_NOT_NULL(dev);

    /* ID_A is 0x00158D0000000001 — "0x" plus exactly 16 hex digits. */
    TEST_ASSERT_TRUE(device_registry_find_by_id("0x00158d0000000001") == dev);
}

/**
 * Trailing characters must not resolve. The check used to be a length
 * minimum, so anything appended to a valid address still found the device —
 * and remove_device acts on whatever comes back.
 */
static void test_find_rejects_trailing_garbage(void)
{
    reset_registry();
    TEST_ASSERT_NOT_NULL(device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE));

    TEST_ASSERT_NULL(device_registry_find_by_id("0x00158d0000000001_typo"));
    TEST_ASSERT_NULL(device_registry_find_by_id("0x00158d0000000001AB"));
}

static void test_find_rejects_malformed_input(void)
{
    reset_registry();
    TEST_ASSERT_NOT_NULL(device_registry_add(ID_A, DEV_PROTOCOL_ZIGBEE));

    TEST_ASSERT_NULL(device_registry_find_by_id(NULL));
    TEST_ASSERT_NULL(device_registry_find_by_id(""));
    TEST_ASSERT_NULL(device_registry_find_by_id("0x00158d"));           /* too short */
    TEST_ASSERT_NULL(device_registry_find_by_id("00158d0000000001xx")); /* no 0x     */
    TEST_ASSERT_NULL(device_registry_find_by_id("0xzzzzzzzzzzzzzzzz")); /* not hex   */
    TEST_ASSERT_NULL(device_registry_find_by_id("0x0000000000000000")); /* id 0      */
}

/* ============================================================================
 * Suite
 * ============================================================================ */

static const test_case_t device_registry_tests[] = {
    {"add_then_get",                          test_add_then_get},
    {"get_unknown_returns_null",              test_get_unknown_returns_null},
    {"remove_makes_it_unfindable",            test_remove_makes_it_unfindable},
    {"count_tracks_add_and_remove",           test_count_tracks_add_and_remove},

    {"snapshot_empty_registry",               test_snapshot_empty_registry},
    {"snapshot_lists_every_device",           test_snapshot_lists_every_device},
    {"snapshot_reflects_removal",             test_snapshot_reflects_removal},
    {"snapshot_removed_id_resolves_null",     test_snapshot_id_removed_afterwards_resolves_null},

    {"state_dup_null_when_no_state",          test_state_dup_null_when_no_state},
    {"state_dup_returns_the_values",          test_state_dup_returns_the_values},
    {"state_dup_is_independent",              test_state_dup_is_independent},
    {"state_dup_survives_device_removal",     test_state_dup_survives_device_removal},

    {"freed_slot_not_reused_immediately",     test_freed_slot_is_not_reused_immediately},
    {"consecutive_adds_distinct_slots",       test_consecutive_adds_get_distinct_slots},
    {"duplicate_add_returns_existing",        test_duplicate_add_returns_existing},

    {"find_by_friendly_name",                 test_find_by_friendly_name},
    {"find_by_exact_ieee_string",             test_find_by_exact_ieee_string},
    {"find_rejects_trailing_garbage",         test_find_rejects_trailing_garbage},
    {"find_rejects_malformed_input",          test_find_rejects_malformed_input},
};

test_stats_t run_device_registry_tests(void)
{
    ESP_LOGI(TAG, "Running Device Registry Tests");
    return test_run_suite(device_registry_tests,
                          sizeof(device_registry_tests) / sizeof(device_registry_tests[0]));
}
