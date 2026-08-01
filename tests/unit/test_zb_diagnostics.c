/**
 * @file test_zb_diagnostics.c
 * @brief Tests for zb_diagnostics_get_network_map()
 *
 * This function was rewritten twice over and neither change had runtime cover:
 *
 *   - it walked the registry with count() + get_by_index(), which reads the
 *     count without the mutex and re-derives dense indices on every call, so a
 *     concurrent add or remove made it skip or repeat devices. It now takes an
 *     ID snapshot.
 *   - it read dev->proto.zigbee.* without checking dev->protocol. proto is a
 *     union, so for a BLE device that reinterprets BLE fields as Zigbee ones.
 *     Harmless today only because BLE is compiled out.
 *
 * The union test below is the one that matters: it fails if the protocol check
 * is dropped again.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "zigbee/zb_diagnostics.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "core/memory/memory_manager_ng.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TEST_DIAG";

#define ZB_ID_1  0x00158D0000001001ULL
#define ZB_ID_2  0x00158D0000001002ULL
#define ZB_ID_3  0x00158D0000001003ULL
#define BLE_ID_1 0xAABBCCDD00002001ULL

#define MAP_CAP 8

/** Registry + diagnostics up and empty. Both inits are idempotent. */
static void reset_all(void)
{
    mem_manager_init();
    if (!device_registry_is_initialized()) {
        device_registry_init();
    }
    device_registry_clear_all();

    if (!zb_diagnostics_is_initialized()) {
        zb_diagnostics_init();
    }
}

/** Add a Zigbee device and give it the fields the network map reads. */
static device_t *add_zigbee(device_id_t id, uint16_t short_addr, bool online)
{
    device_t *dev = device_registry_add(id, DEV_PROTOCOL_ZIGBEE);
    if (dev != NULL) {
        dev->proto.zigbee.short_addr = short_addr;
        dev->availability = online ? DEV_AVAIL_ONLINE : DEV_AVAIL_OFFLINE;
    }
    return dev;
}

/* ============================================================================
 * Argument handling
 * ============================================================================ */

static void test_null_buffer_returns_zero(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x1001, true);

    TEST_ASSERT_EQUAL(0, (int)zb_diagnostics_get_network_map(NULL, MAP_CAP));
}

static void test_zero_capacity_returns_zero(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x1001, true);

    zb_device_diagnostics_t map[MAP_CAP];
    TEST_ASSERT_EQUAL(0, (int)zb_diagnostics_get_network_map(map, 0));
}

/* ============================================================================
 * Collection
 * ============================================================================ */

static void test_empty_registry_yields_empty_map(void)
{
    reset_all();

    zb_device_diagnostics_t map[MAP_CAP];
    TEST_ASSERT_EQUAL(0, (int)zb_diagnostics_get_network_map(map, MAP_CAP));
}

static void test_collects_every_zigbee_device(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x1001, true);
    add_zigbee(ZB_ID_2, 0x1002, true);
    add_zigbee(ZB_ID_3, 0x1003, false);

    zb_device_diagnostics_t map[MAP_CAP];
    memset(map, 0, sizeof(map));
    size_t n = zb_diagnostics_get_network_map(map, MAP_CAP);
    TEST_ASSERT_EQUAL(3, (int)n);

    bool seen_1 = false, seen_2 = false, seen_3 = false;
    for (size_t i = 0; i < n; i++) {
        if (map[i].short_addr == 0x1001) seen_1 = true;
        if (map[i].short_addr == 0x1002) seen_2 = true;
        if (map[i].short_addr == 0x1003) seen_3 = true;
    }
    TEST_ASSERT_TRUE(seen_1);
    TEST_ASSERT_TRUE(seen_2);
    TEST_ASSERT_TRUE(seen_3);
}

static void test_ieee_address_is_copied(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x1001, true);

    zb_device_diagnostics_t map[MAP_CAP];
    memset(map, 0, sizeof(map));
    TEST_ASSERT_EQUAL(1, (int)zb_diagnostics_get_network_map(map, MAP_CAP));

    device_id_t got = 0;
    memcpy(&got, map[0].ieee_addr, sizeof(got));
    TEST_ASSERT_TRUE(got == ZB_ID_1);
}

/** valid mirrors availability == ONLINE. */
static void test_valid_flag_follows_availability(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x1001, true);
    add_zigbee(ZB_ID_2, 0x1002, false);

    zb_device_diagnostics_t map[MAP_CAP];
    memset(map, 0, sizeof(map));
    size_t n = zb_diagnostics_get_network_map(map, MAP_CAP);
    TEST_ASSERT_EQUAL(2, (int)n);

    for (size_t i = 0; i < n; i++) {
        if (map[i].short_addr == 0x1001) TEST_ASSERT_TRUE(map[i].valid);
        if (map[i].short_addr == 0x1002) TEST_ASSERT_FALSE(map[i].valid);
    }
}

static void test_respects_max_count(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x1001, true);
    add_zigbee(ZB_ID_2, 0x1002, true);
    add_zigbee(ZB_ID_3, 0x1003, true);

    zb_device_diagnostics_t map[MAP_CAP];
    memset(map, 0, sizeof(map));
    TEST_ASSERT_EQUAL(2, (int)zb_diagnostics_get_network_map(map, 2));

    /* Nothing may be written past the requested capacity. */
    TEST_ASSERT_EQUAL(0, (int)map[2].short_addr);
}

/* ============================================================================
 * The union check
 * ============================================================================ */

/**
 * device_t::proto is a union of Zigbee and BLE data. Reading the Zigbee arm
 * for a BLE device yields whatever the BLE fields happen to contain. The
 * network map must skip anything that is not DEV_PROTOCOL_ZIGBEE.
 */
static void test_skips_non_zigbee_devices(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x1001, true);

    device_t *ble = device_registry_add(BLE_ID_1, DEV_PROTOCOL_BLE);
    TEST_ASSERT_NOT_NULL(ble);
    ble->availability = DEV_AVAIL_ONLINE;

    TEST_ASSERT_EQUAL(2, (int)device_registry_count());

    zb_device_diagnostics_t map[MAP_CAP];
    memset(map, 0, sizeof(map));
    size_t n = zb_diagnostics_get_network_map(map, MAP_CAP);

    /* Only the Zigbee device belongs in a Zigbee network map. */
    TEST_ASSERT_EQUAL(1, (int)n);
    TEST_ASSERT_EQUAL(0x1001, (int)map[0].short_addr);

    device_id_t got = 0;
    memcpy(&got, map[0].ieee_addr, sizeof(got));
    TEST_ASSERT_TRUE(got == ZB_ID_1);
}

/** A virtual device is not a Zigbee device either. */
static void test_skips_virtual_devices(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x1001, true);

    device_t *virt = device_registry_add(0x9999000000000001ULL, DEV_PROTOCOL_VIRTUAL);
    TEST_ASSERT_NOT_NULL(virt);
    virt->availability = DEV_AVAIL_ONLINE;

    zb_device_diagnostics_t map[MAP_CAP];
    memset(map, 0, sizeof(map));
    TEST_ASSERT_EQUAL(1, (int)zb_diagnostics_get_network_map(map, MAP_CAP));
}

/* ============================================================================
 * Suite
 * ============================================================================ */

static const test_case_t zb_diagnostics_tests[] = {
    {"null_buffer_returns_zero",        test_null_buffer_returns_zero},
    {"zero_capacity_returns_zero",      test_zero_capacity_returns_zero},
    {"empty_registry_yields_empty_map", test_empty_registry_yields_empty_map},
    {"collects_every_zigbee_device",    test_collects_every_zigbee_device},
    {"ieee_address_is_copied",          test_ieee_address_is_copied},
    {"valid_flag_follows_availability", test_valid_flag_follows_availability},
    {"respects_max_count",              test_respects_max_count},
    {"skips_non_zigbee_devices",        test_skips_non_zigbee_devices},
    {"skips_virtual_devices",           test_skips_virtual_devices},
};

test_stats_t run_zb_diagnostics_tests(void)
{
    ESP_LOGI(TAG, "Running Zigbee Diagnostics Tests");
    return test_run_suite(zb_diagnostics_tests,
                          sizeof(zb_diagnostics_tests) / sizeof(zb_diagnostics_tests[0]));
}
