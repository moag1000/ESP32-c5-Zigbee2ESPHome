/**
 * @file test_zb_lumi.c
 * @brief Tests for the Aqara 0xFF01 parser
 *
 * The payload is a flat run of tag/type/value entries with no length prefix
 * per entry, which makes it unforgiving: get one width wrong and every
 * following entry is read from the wrong offset, silently, producing numbers
 * that look plausible. A battery sensor reading 41 % when the cell is nearly
 * flat is worse than one reading nothing.
 *
 * So the cases below are mostly about position: unknown types stopping the
 * walk rather than guessing, payloads that end mid-entry, and uninteresting
 * tags being skipped by exactly their own width.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "zigbee/zb_lumi.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TEST_LUMI";

/* ============================================================================
 * Voltage to percentage
 * ============================================================================ */

/** A fresh CR2032 and a flat one, plus the clamps either side. */
static void test_voltage_endpoints_and_clamps(void)
{
    TEST_ASSERT_EQUAL(100, zb_lumi_voltage_to_percent(3000));
    TEST_ASSERT_EQUAL(100, zb_lumi_voltage_to_percent(3300));  /* above full */
    TEST_ASSERT_EQUAL(0,   zb_lumi_voltage_to_percent(2500));
    TEST_ASSERT_EQUAL(0,   zb_lumi_voltage_to_percent(2000));  /* below empty */
    TEST_ASSERT_EQUAL(0,   zb_lumi_voltage_to_percent(0));
}

/** Halfway between empty and full reads as half. */
static void test_voltage_midpoint(void)
{
    TEST_ASSERT_EQUAL(50, zb_lumi_voltage_to_percent(2750));
    TEST_ASSERT_EQUAL(20, zb_lumi_voltage_to_percent(2600));
    TEST_ASSERT_EQUAL(80, zb_lumi_voltage_to_percent(2900));
}

/* ============================================================================
 * Payload parsing
 * ============================================================================ */

/** The shape these devices actually send: voltage first, as a uint16. */
static void test_voltage_is_little_endian(void)
{
    /* tag 0x01, type 0x21 (uint16), 2955 mV = 0x0B8B */
    const uint8_t payload[] = { 0x01, 0x21, 0x8B, 0x0B };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(payload, sizeof(payload), &a));
    TEST_ASSERT_TRUE(a.has_voltage);
    TEST_ASSERT_EQUAL(2955, a.voltage_mv);
    TEST_ASSERT_EQUAL(1, a.tags_seen);
    TEST_ASSERT_EQUAL(91, zb_lumi_voltage_to_percent(a.voltage_mv));
}

/** Device temperature is a signed byte, so below freezing has to survive. */
static void test_temperature_is_signed(void)
{
    const uint8_t payload[] = { 0x03, 0x28, 0xF8 };   /* -8 °C */
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(payload, sizeof(payload), &a));
    TEST_ASSERT_TRUE(a.has_temperature);
    TEST_ASSERT_EQUAL(-8, a.temperature_c);
}

/** Several entries in one payload, each found at its own offset. */
static void test_multiple_tags(void)
{
    const uint8_t payload[] = {
        0x01, 0x21, 0xB8, 0x0B,   /* voltage 3000 mV */
        0x03, 0x28, 0x17,         /* temperature 23 °C */
        0x0A, 0x21, 0x00, 0x00,   /* parent 0x0000 */
    };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(payload, sizeof(payload), &a));
    TEST_ASSERT_EQUAL(3, a.tags_seen);
    TEST_ASSERT_EQUAL(3000, a.voltage_mv);
    TEST_ASSERT_EQUAL(23, a.temperature_c);
    TEST_ASSERT_TRUE(a.has_parent);
    TEST_ASSERT_EQUAL(0x0000, a.parent_addr);
}

/**
 * A tag this parser does not care about must be skipped by its own width.
 *
 * If it were skipped by a guessed width, the voltage after it would be read
 * from the wrong offset and still look like a number.
 */
static void test_uninteresting_tag_is_skipped_by_its_width(void)
{
    const uint8_t payload[] = {
        0x64, 0x23, 0xDE, 0xAD, 0xBE, 0xEF,   /* tag 0x64, uint32 — not ours */
        0x01, 0x21, 0xB8, 0x0B,               /* voltage 3000 mV */
    };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(payload, sizeof(payload), &a));
    TEST_ASSERT_EQUAL(2, a.tags_seen);
    TEST_ASSERT_TRUE(a.has_voltage);
    TEST_ASSERT_EQUAL(3000, a.voltage_mv);
}

/**
 * An unknown type ends the walk, and what came before it is kept.
 *
 * The width of an unknown type is unknown, so the offset of the next entry is
 * too. Continuing would be inventing data.
 */
static void test_unknown_type_stops_the_walk_without_losing_earlier_tags(void)
{
    const uint8_t payload[] = {
        0x01, 0x21, 0xB8, 0x0B,   /* voltage 3000 mV */
        0x05, 0x4C, 0x01, 0x02,   /* type 0x4C (struct) — width unknown here */
        0x03, 0x28, 0x17,         /* would be temperature, unreachable */
    };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(payload, sizeof(payload), &a));
    TEST_ASSERT_TRUE(a.has_voltage);
    TEST_ASSERT_EQUAL(3000, a.voltage_mv);
    TEST_ASSERT_FALSE(a.has_temperature);
    TEST_ASSERT_EQUAL(1, a.tags_seen);
}

/** A payload cut off mid-value keeps what was already complete. */
static void test_truncated_entry_is_dropped_not_read(void)
{
    const uint8_t payload[] = {
        0x01, 0x21, 0xB8, 0x0B,   /* voltage, complete */
        0x0A, 0x21, 0x34,         /* parent, one byte short */
    };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(payload, sizeof(payload), &a));
    TEST_ASSERT_TRUE(a.has_voltage);
    TEST_ASSERT_FALSE(a.has_parent);
    TEST_ASSERT_EQUAL(1, a.tags_seen);
}

/** A payload holding only a tag and type, with no value, yields nothing. */
static void test_header_without_value(void)
{
    const uint8_t payload[] = { 0x01, 0x21 };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, zb_lumi_parse_special(payload, sizeof(payload), &a));
    TEST_ASSERT_FALSE(a.has_voltage);
}

/** An empty payload is not an error to crash on. */
static void test_empty_payload(void)
{
    const uint8_t payload[] = { 0x00 };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, zb_lumi_parse_special(payload, 0, &a));
    TEST_ASSERT_EQUAL(0, a.tags_seen);
}

/** NULL arguments are rejected rather than dereferenced. */
static void test_null_arguments(void)
{
    zb_lumi_attrs_t a;
    const uint8_t payload[] = { 0x01, 0x21, 0xB8, 0x0B };

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, zb_lumi_parse_special(NULL, 4, &a));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      zb_lumi_parse_special(payload, sizeof(payload), NULL));
}

/**
 * The result is zeroed before use.
 *
 * Callers keep one of these on the stack and check the has_* flags, so a
 * failed parse must not leave the previous call's voltage sitting there.
 */
static void test_result_is_zeroed_before_use(void)
{
    zb_lumi_attrs_t a;
    memset(&a, 0xAA, sizeof(a));

    const uint8_t payload[] = { 0x03, 0x28, 0x17 };   /* temperature only */
    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(payload, sizeof(payload), &a));

    TEST_ASSERT_FALSE(a.has_voltage);
    TEST_ASSERT_EQUAL(0, a.voltage_mv);
    TEST_ASSERT_TRUE(a.has_temperature);
}

/* ============================================================================
 * Suite
 * ============================================================================ */

static const test_case_t zb_lumi_tests[] = {
    {"voltage_endpoints_and_clamps", test_voltage_endpoints_and_clamps},
    {"voltage_midpoint",             test_voltage_midpoint},
    {"voltage_little_endian",        test_voltage_is_little_endian},
    {"temperature_is_signed",        test_temperature_is_signed},
    {"multiple_tags",                test_multiple_tags},
    {"uninteresting_tag_skipped",    test_uninteresting_tag_is_skipped_by_its_width},
    {"unknown_type_stops_walk",      test_unknown_type_stops_the_walk_without_losing_earlier_tags},
    {"truncated_entry_dropped",      test_truncated_entry_is_dropped_not_read},
    {"header_without_value",         test_header_without_value},
    {"empty_payload",                test_empty_payload},
    {"null_arguments",               test_null_arguments},
    {"result_zeroed_before_use",     test_result_is_zeroed_before_use},
};

test_stats_t run_zb_lumi_tests(void)
{
    ESP_LOGI(TAG, "Running Aqara 0xFF01 Parser Tests");
    return test_run_suite(zb_lumi_tests, sizeof(zb_lumi_tests) / sizeof(zb_lumi_tests[0]));
}
