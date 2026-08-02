/**
 * @file test_zb_backup.c
 * @brief Tests for the device collection inside zb_backup_create()
 *
 * collect_devices() is the third place that was rewritten from
 * count() + get_by_index() to an ID snapshot, and the third that read
 * dev->proto.zigbee.* without checking dev->protocol. It is static, but
 * zb_backup_create() reaches it and builds the whole backup in memory — file
 * I/O only happens later in zb_backup_save() — so the collection is
 * observable without touching the filesystem.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "zigbee/zb_backup.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "core/memory/memory_manager_ng.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TEST_BKP";

#define ZB_ID_1  0x00158D0000003001ULL
#define ZB_ID_2  0x00158D0000003002ULL
#define BLE_ID_1 0xAABBCCDD00003001ULL

/** Backups are large; keep one instance rather than putting it on the stack. */
static zb_backup_t s_backup;

static void reset_all(void)
{
    mem_manager_init();
    if (!device_registry_is_initialized()) {
        device_registry_init();
    }
    device_registry_clear_all();

    zb_backup_init();
    memset(&s_backup, 0, sizeof(s_backup));
}

static device_t *add_zigbee(device_id_t id, uint16_t short_addr,
                            const char *model, uint8_t endpoint)
{
    device_t *dev = device_registry_add(id, DEV_PROTOCOL_ZIGBEE);
    if (dev != NULL) {
        dev->proto.zigbee.short_addr = short_addr;
        dev->proto.zigbee.endpoint = endpoint;
        snprintf(dev->model, sizeof(dev->model), "%s", model);
        snprintf(dev->manufacturer, sizeof(dev->manufacturer), "TestCorp");
        dev->availability = DEV_AVAIL_ONLINE;
    }
    return dev;
}

/* ============================================================================
 * Collection
 * ============================================================================ */

static void test_empty_registry_backs_up_no_devices(void)
{
    reset_all();

    TEST_ASSERT_EQUAL(ESP_OK, zb_backup_create(ZB_BACKUP_TYPE_DEVICES, &s_backup));
    TEST_ASSERT_EQUAL(0, (int)s_backup.device_count);
}

static void test_collects_every_zigbee_device(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x3001, "TS0001", 1);
    add_zigbee(ZB_ID_2, 0x3002, "lumi.sensor", 1);

    TEST_ASSERT_EQUAL(ESP_OK, zb_backup_create(ZB_BACKUP_TYPE_DEVICES, &s_backup));
    TEST_ASSERT_EQUAL(2, (int)s_backup.device_count);

    bool seen_1 = false, seen_2 = false;
    for (uint16_t i = 0; i < s_backup.device_count; i++) {
        if (s_backup.devices[i].ieee_addr == ZB_ID_1) seen_1 = true;
        if (s_backup.devices[i].ieee_addr == ZB_ID_2) seen_2 = true;
    }
    TEST_ASSERT_TRUE(seen_1);
    TEST_ASSERT_TRUE(seen_2);
}

static void test_device_fields_are_copied(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x3001, "TS0001", 3);

    TEST_ASSERT_EQUAL(ESP_OK, zb_backup_create(ZB_BACKUP_TYPE_DEVICES, &s_backup));
    TEST_ASSERT_EQUAL(1, (int)s_backup.device_count);

    const zb_backup_device_t *d = &s_backup.devices[0];
    TEST_ASSERT_TRUE(d->ieee_addr == ZB_ID_1);
    TEST_ASSERT_EQUAL(0x3001, (int)d->short_addr);
    TEST_ASSERT_EQUAL(3, (int)d->endpoint);
    TEST_ASSERT_EQUAL_STRING("TS0001", d->model);
    TEST_ASSERT_EQUAL_STRING("TestCorp", d->manufacturer);
}

static void test_reflects_removal(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x3001, "TS0001", 1);
    add_zigbee(ZB_ID_2, 0x3002, "lumi.sensor", 1);
    device_registry_remove(ZB_ID_1);

    TEST_ASSERT_EQUAL(ESP_OK, zb_backup_create(ZB_BACKUP_TYPE_DEVICES, &s_backup));
    TEST_ASSERT_EQUAL(1, (int)s_backup.device_count);
    TEST_ASSERT_TRUE(s_backup.devices[0].ieee_addr == ZB_ID_2);
}

/* ============================================================================
 * The union check
 * ============================================================================ */

/**
 * device_t::proto is a union. collect_devices() used to read the Zigbee arm
 * for every entry, so a BLE device contributed a garbage short address and
 * endpoint to the backup — and a restore would then have tried to recreate it
 * as a Zigbee device. Only DEV_PROTOCOL_ZIGBEE belongs in a Zigbee backup.
 */
static void test_skips_non_zigbee_devices(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x3001, "TS0001", 1);

    device_t *ble = device_registry_add(BLE_ID_1, DEV_PROTOCOL_BLE);
    TEST_ASSERT_NOT_NULL(ble);
    ble->availability = DEV_AVAIL_ONLINE;
    snprintf(ble->model, sizeof(ble->model), "LYWSD03MMC");

    TEST_ASSERT_EQUAL(2, (int)device_registry_count());

    TEST_ASSERT_EQUAL(ESP_OK, zb_backup_create(ZB_BACKUP_TYPE_DEVICES, &s_backup));

    TEST_ASSERT_EQUAL(1, (int)s_backup.device_count);
    TEST_ASSERT_TRUE(s_backup.devices[0].ieee_addr == ZB_ID_1);
}

static void test_skips_virtual_devices(void)
{
    reset_all();
    add_zigbee(ZB_ID_1, 0x3001, "TS0001", 1);

    device_t *virt = device_registry_add(0x9999000000003001ULL, DEV_PROTOCOL_VIRTUAL);
    TEST_ASSERT_NOT_NULL(virt);
    virt->availability = DEV_AVAIL_ONLINE;

    TEST_ASSERT_EQUAL(ESP_OK, zb_backup_create(ZB_BACKUP_TYPE_DEVICES, &s_backup));
    TEST_ASSERT_EQUAL(1, (int)s_backup.device_count);
}

/* ============================================================================
 * Arguments
 * ============================================================================ */

static void test_null_backup_is_rejected(void)
{
    reset_all();
    TEST_ASSERT_NOT_EQUAL(ESP_OK, zb_backup_create(ZB_BACKUP_TYPE_DEVICES, NULL));
}

/* ============================================================================
 * Suite
 * ============================================================================ */

static const test_case_t zb_backup_tests[] = {
    {"empty_registry_backs_up_no_devices", test_empty_registry_backs_up_no_devices},
    {"collects_every_zigbee_device",       test_collects_every_zigbee_device},
    {"device_fields_are_copied",           test_device_fields_are_copied},
    {"reflects_removal",                   test_reflects_removal},
    {"skips_non_zigbee_devices",           test_skips_non_zigbee_devices},
    {"skips_virtual_devices",              test_skips_virtual_devices},
    {"null_backup_is_rejected",            test_null_backup_is_rejected},
};

test_stats_t run_zb_backup_tests(void)
{
    ESP_LOGI(TAG, "Running Zigbee Backup Tests");
    return test_run_suite(zb_backup_tests,
                          sizeof(zb_backup_tests) / sizeof(zb_backup_tests[0]));
}
