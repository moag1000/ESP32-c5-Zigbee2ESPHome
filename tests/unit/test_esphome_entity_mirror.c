/**
 * @file test_esphome_entity_mirror.c
 * @brief Tests for the ESPHome entity state mirror
 *
 * This module exists because entity state used to be kept in the unified
 * device_registry, one virtual device per entity — which meant a gateway with
 * two paired Zigbee devices spent 54 of 64 registry slots on entities and had
 * no room left for actual devices.
 *
 * Two things here are worth real tests rather than smoke checks:
 *
 * 1. **Deletion.** The table is open-addressed with linear probing and closes
 *    gaps by backward shifting instead of leaving tombstones. Getting that
 *    wrong does not crash — it silently makes entries unreachable, and only the
 *    ones that had collided, so it would show up in the field as "some MQTT
 *    topics stopped updating after a device was removed" and nowhere else.
 *    test_deletion_keeps_collided_entries_reachable is the guard.
 *
 * 2. **Ownership.** _get() promises a copy taken under the mirror's lock,
 *    precisely so a subscriber on the dispatcher task cannot be reading a state
 *    object while sync_state() frees it. A test that only checked the contents
 *    would pass even if the pointer were shared, so the test mutates the mirror
 *    afterwards and asserts the copy is unchanged.
 *
 * The mirror publishes to the event bus, which is not initialized here. That is
 * deliberate: event_publish() failing is a path sync_state() has to tolerate
 * (state is stored, only the notification is lost), so running without a bus
 * exercises it.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "esphome/esphome_entity_mirror.h"
#include "esphome/esphome_entities.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "TEST_MIRROR";

/* ============================================================================
 * Helpers
 * ============================================================================ */

/** Start from a known-empty mirror regardless of what ran before. */
static void fresh_mirror(void)
{
    if (esphome_entity_mirror_is_initialized()) {
        esphome_entity_mirror_deinit();
    }
    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_init());
}

/** Register a sensor entity under @p key with a generated name. */
static esp_err_t add_sensor(esphome_entity_key_t key, const char *name)
{
    return esphome_entity_mirror_register(ESPHOME_ENTITY_SENSOR, key, name, NULL);
}

/** Push a sensor value through sync_state(). */
static esp_err_t sync_sensor(esphome_entity_key_t key, float value)
{
    esphome_sensor_state_t st = {
        .state         = value,
        .missing_state = false,
    };
    return esphome_entity_mirror_sync_state(ESPHOME_ENTITY_SENSOR, key, &st);
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

static void test_init_then_deinit(void)
{
    if (esphome_entity_mirror_is_initialized()) {
        esphome_entity_mirror_deinit();
    }
    TEST_ASSERT_FALSE(esphome_entity_mirror_is_initialized());

    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_init());
    TEST_ASSERT_TRUE(esphome_entity_mirror_is_initialized());
    TEST_ASSERT_EQUAL(0, esphome_entity_mirror_count());
    TEST_ASSERT_GREATER_THAN(0, esphome_entity_mirror_capacity());

    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_deinit());
    TEST_ASSERT_FALSE(esphome_entity_mirror_is_initialized());
}

static void test_double_init_is_not_an_error(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_init());
    TEST_ASSERT_TRUE(esphome_entity_mirror_is_initialized());
    esphome_entity_mirror_deinit();
}

static void test_calls_before_init_are_rejected(void)
{
    if (esphome_entity_mirror_is_initialized()) {
        esphome_entity_mirror_deinit();
    }

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, add_sensor(1, "x"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sync_sensor(1, 1.0f));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      esphome_entity_mirror_unregister(1));
    TEST_ASSERT_EQUAL(0, esphome_entity_mirror_count());
}

/* Deinit has to free every stored state object, not just the slots. Nothing
 * here can observe a leak directly, so this at least proves deinit survives a
 * populated table and leaves the mirror reusable. */
static void test_deinit_with_states_present(void)
{
    fresh_mirror();
    for (uint32_t i = 1; i <= 10; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, add_sensor(i, "s"));
        TEST_ASSERT_EQUAL(ESP_OK, sync_sensor(i, (float)i));
    }
    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_deinit());

    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_init());
    TEST_ASSERT_EQUAL(0, esphome_entity_mirror_count());
    esphome_entity_mirror_deinit();
}

/* ============================================================================
 * Registration
 * ============================================================================ */

static void test_register_then_read_name(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(0x1234, "Free Heap"));
    TEST_ASSERT_EQUAL(1, esphome_entity_mirror_count());

    char name[ESPHOME_ENTITY_MIRROR_NAME_LEN];
    TEST_ASSERT_EQUAL(ESP_OK,
                      esphome_entity_mirror_get(0x1234, name, sizeof(name), NULL));
    TEST_ASSERT_EQUAL_STRING("Free Heap", name);

    esphome_entity_mirror_deinit();
}

static void test_reregister_updates_name_without_growing(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(7, "Old Name"));
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(7, "New Name"));
    TEST_ASSERT_EQUAL(1, esphome_entity_mirror_count());

    char name[ESPHOME_ENTITY_MIRROR_NAME_LEN];
    TEST_ASSERT_EQUAL(ESP_OK,
                      esphome_entity_mirror_get(7, name, sizeof(name), NULL));
    TEST_ASSERT_EQUAL_STRING("New Name", name);

    esphome_entity_mirror_deinit();
}

/* The name buffer is deliberately 32 bytes, matching what device_t's
 * friendly_name gave the old registry-backed mirror, because MQTT state topics
 * are built from it. A longer buffer would rename topics that used to be
 * truncated. */
static void test_long_name_is_truncated_not_overflowed(void)
{
    fresh_mirror();
    const char *long_name = "A name far longer than the mirror buffer allows";
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(9, long_name));

    char name[ESPHOME_ENTITY_MIRROR_NAME_LEN];
    TEST_ASSERT_EQUAL(ESP_OK,
                      esphome_entity_mirror_get(9, name, sizeof(name), NULL));
    TEST_ASSERT_EQUAL(ESPHOME_ENTITY_MIRROR_NAME_LEN - 1, strlen(name));
    TEST_ASSERT_EQUAL_MEMORY(long_name, name, ESPHOME_ENTITY_MIRROR_NAME_LEN - 1);

    esphome_entity_mirror_deinit();
}

static void test_register_without_name_is_allowed(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(11, NULL));

    char name[ESPHOME_ENTITY_MIRROR_NAME_LEN];
    memset(name, 'x', sizeof(name));
    TEST_ASSERT_EQUAL(ESP_OK,
                      esphome_entity_mirror_get(11, name, sizeof(name), NULL));
    TEST_ASSERT_EQUAL_STRING("", name);

    esphome_entity_mirror_deinit();
}

static void test_unknown_key_is_not_found(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(1, "one"));

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      esphome_entity_mirror_get(2, NULL, 0, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, sync_sensor(2, 1.0f));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, esphome_entity_mirror_unregister(2));

    esphome_entity_mirror_deinit();
}

/* Key 0 is a legitimate entity key and must not be mistaken for an empty slot. */
static void test_key_zero_is_a_real_key(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(0, "Zero"));
    TEST_ASSERT_EQUAL(1, esphome_entity_mirror_count());

    char name[ESPHOME_ENTITY_MIRROR_NAME_LEN];
    TEST_ASSERT_EQUAL(ESP_OK,
                      esphome_entity_mirror_get(0, name, sizeof(name), NULL));
    TEST_ASSERT_EQUAL_STRING("Zero", name);

    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_unregister(0));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      esphome_entity_mirror_get(0, NULL, 0, NULL));

    esphome_entity_mirror_deinit();
}

/* ============================================================================
 * State
 * ============================================================================ */

static void test_sync_state_is_readable(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(42, "Temp"));
    TEST_ASSERT_EQUAL(ESP_OK, sync_sensor(42, 21.5f));

    cJSON *state = NULL;
    TEST_ASSERT_EQUAL(ESP_OK,
                      esphome_entity_mirror_get(42, NULL, 0, &state));
    TEST_ASSERT_NOT_NULL(state);

    cJSON *value = cJSON_GetObjectItem(state, "value");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_TRUE(cJSON_IsNumber(value));
    TEST_ASSERT_TRUE(value->valuedouble > 21.4 && value->valuedouble < 21.6);

    cJSON_Delete(state);
    esphome_entity_mirror_deinit();
}

/* A registered entity that has never reported yet must come back OK with a NULL
 * state, not NOT_FOUND — the distinction is what lets the MQTT handler tell
 * "unknown entity" from "nothing to publish yet". */
static void test_registered_without_state_yields_null_state(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(5, "Quiet"));

    cJSON *state = (cJSON *)0xDEADBEEF;
    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_get(5, NULL, 0, &state));
    TEST_ASSERT_NULL(state);

    esphome_entity_mirror_deinit();
}

/* The ownership promise: _get() hands back a copy. If it returned the mirror's
 * own pointer, the second sync_state() below would free it under the caller and
 * this test would be reading freed memory. */
static void test_get_returns_an_independent_copy(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(3, "Sensor"));
    TEST_ASSERT_EQUAL(ESP_OK, sync_sensor(3, 1.0f));

    cJSON *first = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_get(3, NULL, 0, &first));
    TEST_ASSERT_NOT_NULL(first);

    /* Replace the mirror's state; the copy must not follow. */
    TEST_ASSERT_EQUAL(ESP_OK, sync_sensor(3, 99.0f));

    cJSON *value = cJSON_GetObjectItem(first, "value");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_TRUE(value->valuedouble < 2.0);

    cJSON *second = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_get(3, NULL, 0, &second));
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_NOT_EQUAL((uintptr_t)first, (uintptr_t)second);
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(second, "value")->valuedouble > 98.0);

    cJSON_Delete(first);
    cJSON_Delete(second);
    esphome_entity_mirror_deinit();
}

static void test_sync_state_rejects_null_state(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(1, "s"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      esphome_entity_mirror_sync_state(ESPHOME_ENTITY_SENSOR, 1, NULL));
    esphome_entity_mirror_deinit();
}

/* missing_state omits the value and reports available=false. */
static void test_missing_state_reports_unavailable(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(4, "Gone"));

    esphome_sensor_state_t st = { .state = 0.0f, .missing_state = true };
    TEST_ASSERT_EQUAL(ESP_OK,
                      esphome_entity_mirror_sync_state(ESPHOME_ENTITY_SENSOR, 4, &st));

    cJSON *state = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_get(4, NULL, 0, &state));
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(state, "value"));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(state, "available")));

    cJSON_Delete(state);
    esphome_entity_mirror_deinit();
}

/* ============================================================================
 * Table behaviour
 * ============================================================================ */

static void test_unregister_frees_the_slot(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(100, "A"));
    TEST_ASSERT_EQUAL(ESP_OK, sync_sensor(100, 1.0f));
    TEST_ASSERT_EQUAL(1, esphome_entity_mirror_count());

    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_unregister(100));
    TEST_ASSERT_EQUAL(0, esphome_entity_mirror_count());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      esphome_entity_mirror_get(100, NULL, 0, NULL));

    /* The key must be reusable, and must not inherit the old state. */
    TEST_ASSERT_EQUAL(ESP_OK, add_sensor(100, "B"));
    cJSON *state = (cJSON *)0xDEADBEEF;
    TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_get(100, NULL, 0, &state));
    TEST_ASSERT_NULL(state);

    esphome_entity_mirror_deinit();
}

/**
 * The one that matters.
 *
 * Linear probing puts colliding keys in the slots after their home position, so
 * a lookup walks forward until it hits an empty slot. Clearing a slot in the
 * middle of such a run therefore truncates the probe sequence: every colliding
 * entry that lived past the hole becomes unreachable even though it is still in
 * the table. Backward shifting is what prevents that.
 *
 * Filling most of the table guarantees collisions without needing to know the
 * hash function, then every second entry is removed and every survivor is
 * looked up again by both key and stored state.
 */
static void test_deletion_keeps_collided_entries_reachable(void)
{
    fresh_mirror();

    const uint32_t n = 40;   /* Well past the point where collisions are certain */
    for (uint32_t i = 0; i < n; i++) {
        char name[16];
        snprintf(name, sizeof(name), "e%lu", (unsigned long)i);
        TEST_ASSERT_EQUAL(ESP_OK, add_sensor(i, name));
        TEST_ASSERT_EQUAL(ESP_OK, sync_sensor(i, (float)i));
    }
    TEST_ASSERT_EQUAL(n, esphome_entity_mirror_count());

    /* Drop the even keys. */
    for (uint32_t i = 0; i < n; i += 2) {
        TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_unregister(i));
    }
    TEST_ASSERT_EQUAL(n / 2, esphome_entity_mirror_count());

    /* Every odd key must still be reachable, with its own name and value. */
    for (uint32_t i = 1; i < n; i += 2) {
        char expected[16];
        snprintf(expected, sizeof(expected), "e%lu", (unsigned long)i);

        char name[ESPHOME_ENTITY_MIRROR_NAME_LEN];
        cJSON *state = NULL;
        TEST_ASSERT_EQUAL(ESP_OK,
                          esphome_entity_mirror_get(i, name, sizeof(name), &state));
        TEST_ASSERT_EQUAL_STRING(expected, name);
        TEST_ASSERT_NOT_NULL(state);

        cJSON *value = cJSON_GetObjectItem(state, "value");
        TEST_ASSERT_NOT_NULL(value);
        TEST_ASSERT_TRUE(value->valuedouble > (double)i - 0.5 &&
                         value->valuedouble < (double)i + 0.5);
        cJSON_Delete(state);
    }

    /* And every even key must be gone. */
    for (uint32_t i = 0; i < n; i += 2) {
        TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                          esphome_entity_mirror_get(i, NULL, 0, NULL));
    }

    esphome_entity_mirror_deinit();
}

/* Re-registering into a table that has been churned must still find the entries
 * again — a probe sequence broken by deletion would show up here as duplicate
 * entries rather than lookups failing. */
static void test_churn_does_not_duplicate_entries(void)
{
    fresh_mirror();

    for (uint32_t round = 0; round < 5; round++) {
        for (uint32_t i = 0; i < 20; i++) {
            TEST_ASSERT_EQUAL(ESP_OK, add_sensor(i, "x"));
        }
        TEST_ASSERT_EQUAL(20, esphome_entity_mirror_count());

        for (uint32_t i = 0; i < 20; i++) {
            TEST_ASSERT_EQUAL(ESP_OK, esphome_entity_mirror_unregister(i));
        }
        TEST_ASSERT_EQUAL(0, esphome_entity_mirror_count());
    }

    esphome_entity_mirror_deinit();
}

/* Past the configured capacity, registration fails cleanly and the entities
 * already mirrored are untouched. */
static void test_full_table_refuses_further_entities(void)
{
    fresh_mirror();

    const size_t cap = esphome_entity_mirror_capacity();
    size_t added = 0;
    for (size_t i = 0; i < cap + 32; i++) {
        if (add_sensor((esphome_entity_key_t)(i + 1000), "x") != ESP_OK) {
            break;
        }
        added++;
    }

    TEST_ASSERT_GREATER_THAN(0, added);
    TEST_ASSERT_EQUAL(added, esphome_entity_mirror_count());
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, add_sensor(999999, "overflow"));

    /* A full table must still serve the entries it holds. */
    char name[ESPHOME_ENTITY_MIRROR_NAME_LEN];
    TEST_ASSERT_EQUAL(ESP_OK,
                      esphome_entity_mirror_get(1000, name, sizeof(name), NULL));
    TEST_ASSERT_EQUAL_STRING("x", name);

    esphome_entity_mirror_deinit();
}

/* Entities are not devices: the mirror keeps its own capacity and must not be
 * bounded by the device registry's. This is the coupling the module was split
 * out to remove. */
static void test_capacity_is_independent_of_device_registry(void)
{
    fresh_mirror();
    TEST_ASSERT_EQUAL(CONFIG_ESPHOME_ENTITY_MIRROR_MAX,
                      esphome_entity_mirror_capacity());
    esphome_entity_mirror_deinit();

    /* Reported as zero while down, so callers cannot mistake it for room. */
    TEST_ASSERT_EQUAL(0, esphome_entity_mirror_capacity());
}

/* ============================================================================
 * Suite
 * ============================================================================ */

static const test_case_t esphome_entity_mirror_tests[] = {
    {"init_then_deinit",              test_init_then_deinit},
    {"double_init_ok",                test_double_init_is_not_an_error},
    {"calls_before_init_rejected",    test_calls_before_init_are_rejected},
    {"deinit_with_states_present",    test_deinit_with_states_present},
    {"register_then_read_name",       test_register_then_read_name},
    {"reregister_updates_name",       test_reregister_updates_name_without_growing},
    {"long_name_truncated",           test_long_name_is_truncated_not_overflowed},
    {"register_without_name",         test_register_without_name_is_allowed},
    {"unknown_key_not_found",         test_unknown_key_is_not_found},
    {"key_zero_is_real",              test_key_zero_is_a_real_key},
    {"sync_state_readable",           test_sync_state_is_readable},
    {"no_state_yields_null",          test_registered_without_state_yields_null_state},
    {"get_returns_copy",              test_get_returns_an_independent_copy},
    {"sync_rejects_null_state",       test_sync_state_rejects_null_state},
    {"missing_state_unavailable",     test_missing_state_reports_unavailable},
    {"unregister_frees_slot",         test_unregister_frees_the_slot},
    {"deletion_keeps_collided",       test_deletion_keeps_collided_entries_reachable},
    {"churn_no_duplicates",           test_churn_does_not_duplicate_entries},
    {"full_table_refuses",            test_full_table_refuses_further_entities},
    {"capacity_independent",          test_capacity_is_independent_of_device_registry},
};

test_stats_t run_esphome_entity_mirror_tests(void)
{
    ESP_LOGI(TAG, "Running ESPHome Entity Mirror Tests");
    return test_run_suite(esphome_entity_mirror_tests,
                          sizeof(esphome_entity_mirror_tests) /
                          sizeof(esphome_entity_mirror_tests[0]));
}
