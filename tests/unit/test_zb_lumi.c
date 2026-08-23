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


/**
 * @brief The power outage counter is off by one on the wire
 *
 * zigbee-herdsman-converters subtracts 1 unconditionally (lumi.ts, case "5"),
 * so a device that has never lost power reports 1 and must read as 0.
 */
static void test_power_outage_count_is_off_by_one(void)
{
    const uint8_t payload[] = { 0x05, 0x21, 0x04, 0x00 };   /* uint16, raw 4 */
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(payload, sizeof(payload), &a));
    TEST_ASSERT_TRUE(a.has_power_outages);
    TEST_ASSERT_EQUAL(3, a.power_outages);
}

/** A raw 0 must not wrap to 4294967295. */
static void test_power_outage_count_clamps_at_zero(void)
{
    const uint8_t payload[] = { 0x05, 0x20, 0x00 };   /* uint8, raw 0 */
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(payload, sizeof(payload), &a));
    TEST_ASSERT_TRUE(a.has_power_outages);
    TEST_ASSERT_EQUAL(0, a.power_outages);
}

/**
 * @brief The width comes from the declared type, not from a guess
 *
 * Devices send this counter as one, two or four bytes. Reading a uint32 as if
 * it were a uint16 would both truncate the value and put every following entry
 * at the wrong offset.
 */
static void test_power_outage_count_reads_each_declared_width(void)
{
    zb_lumi_attrs_t a;

    const uint8_t as_u8[]  = { 0x05, 0x20, 0x08 };
    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(as_u8, sizeof(as_u8), &a));
    TEST_ASSERT_EQUAL(7, a.power_outages);

    const uint8_t as_u32[] = { 0x05, 0x23, 0x01, 0x01, 0x00, 0x00 };  /* raw 257 */
    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(as_u32, sizeof(as_u32), &a));
    TEST_ASSERT_EQUAL(256, a.power_outages);
}

/** The counter sits between other entries and must not shift them. */
static void test_power_outage_count_beside_voltage(void)
{
    const uint8_t payload[] = {
        0x01, 0x21, 0xB8, 0x0B,   /* voltage 3000 mV */
        0x05, 0x23, 0x03, 0x00, 0x00, 0x00,
        0x03, 0x28, 0x17,         /* temperature 23 °C */
    };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_special(payload, sizeof(payload), &a));
    TEST_ASSERT_EQUAL(3, a.tags_seen);
    TEST_ASSERT_EQUAL(3000, a.voltage_mv);
    TEST_ASSERT_EQUAL(2, a.power_outages);
    TEST_ASSERT_EQUAL(23, a.temperature_c);
}


/* ============================================================================
 * The ZCL string length byte
 *
 * The attribute arrives length-prefixed, and three places used to strip that
 * byte themselves. One of them — the converter's fz_xiaomi_ff01(), which is what
 * actually runs once a converter is bound — did not, and read the length as the
 * first tag. Every following read was then off by one, the walk stopped at a
 * type it could not size, and it reported success having decoded nothing.
 * Nothing logged, nothing failed, and the battery stayed 'unknown'.
 * ============================================================================ */

/** The first byte is a length, not a tag. */
static void test_attribute_skips_the_length_byte(void)
{
    /* len=4, then tag 0x01 type uint16 = 3000 mV */
    const uint8_t attr[] = { 0x04, 0x01, 0x21, 0xB8, 0x0B };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_attribute(attr, sizeof(attr), &a));
    TEST_ASSERT_TRUE(a.has_voltage);
    TEST_ASSERT_EQUAL(3000, a.voltage_mv);
    TEST_ASSERT_EQUAL(100, zb_lumi_voltage_to_percent(a.voltage_mv));
}

/**
 * @brief Reading the length byte as a tag must not look like success
 *
 * This is the exact shape that failed: byte 0 is a length, and taken as a tag
 * it pairs with 0x01 as its type — a type with no known width, so the walk
 * stops immediately having read nothing.
 */
static void test_length_byte_read_as_a_tag_decodes_nothing(void)
{
    const uint8_t attr[] = { 0x04, 0x01, 0x21, 0xB8, 0x0B };
    zb_lumi_attrs_t wrong;

    /* What the broken converter did: parse from offset 0. */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      zb_lumi_parse_special(attr, sizeof(attr), &wrong));
    TEST_ASSERT_FALSE(wrong.has_voltage);

    /* What it must do instead. */
    zb_lumi_attrs_t right;
    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_attribute(attr, sizeof(attr), &right));
    TEST_ASSERT_EQUAL(3000, right.voltage_mv);
}

/** A realistic payload carrying three tags at once. */
static void test_attribute_decodes_voltage_temperature_and_outages(void)
{
    const uint8_t attr[] = {
        0x0B,                           /* 11 bytes follow */
        0x01, 0x21, 0x8B, 0x0B,         /* voltage 2955 mV */
        0x03, 0x28, 0x17,               /* temperature 23 °C */
        0x05, 0x21, 0x04, 0x00,         /* power outages, raw 4 */
    };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_attribute(attr, sizeof(attr), &a));
    TEST_ASSERT_EQUAL(2955, a.voltage_mv);
    TEST_ASSERT_EQUAL(91,   zb_lumi_voltage_to_percent(a.voltage_mv));
    TEST_ASSERT_EQUAL(23,   a.temperature_c);
    TEST_ASSERT_EQUAL(3,    a.power_outages);
}

/** A length byte larger than the buffer is the device's claim, not a fact. */
static void test_attribute_clamps_an_overlong_length_byte(void)
{
    const uint8_t attr[] = { 0xFF, 0x01, 0x21, 0xB8, 0x0B };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_attribute(attr, sizeof(attr), &a));
    TEST_ASSERT_EQUAL(3000, a.voltage_mv);
}

/** A length byte shorter than the buffer wins — the rest is not ours to read. */
static void test_attribute_honours_a_short_length_byte(void)
{
    const uint8_t attr[] = {
        0x04,                     /* only the first entry belongs to us */
        0x01, 0x21, 0xB8, 0x0B,   /* voltage 3000 mV */
        0x03, 0x28, 0x17,         /* temperature — outside the declared length */
    };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_OK, zb_lumi_parse_attribute(attr, sizeof(attr), &a));
    TEST_ASSERT_TRUE(a.has_voltage);
    TEST_ASSERT_FALSE(a.has_temperature);
    TEST_ASSERT_EQUAL(1, a.tags_seen);
}

/** NULL and undersized buffers are rejected rather than dereferenced. */
static void test_attribute_rejects_bad_arguments(void)
{
    const uint8_t attr[] = { 0x04, 0x01, 0x21, 0xB8, 0x0B };
    zb_lumi_attrs_t a;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, zb_lumi_parse_attribute(NULL, 5, &a));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, zb_lumi_parse_attribute(attr, sizeof(attr), NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, zb_lumi_parse_attribute(attr, 1, &a));
    TEST_ASSERT_FALSE(a.has_voltage);
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
    {"power_outage_off_by_one",      test_power_outage_count_is_off_by_one},
    {"power_outage_clamps_at_zero",  test_power_outage_count_clamps_at_zero},
    {"power_outage_declared_width",  test_power_outage_count_reads_each_declared_width},
    {"power_outage_beside_voltage",  test_power_outage_count_beside_voltage},
    {"attr_skips_length_byte",       test_attribute_skips_the_length_byte},
    {"length_byte_as_tag_fails",     test_length_byte_read_as_a_tag_decodes_nothing},
    {"attr_decodes_three_tags",      test_attribute_decodes_voltage_temperature_and_outages},
    {"attr_clamps_overlong_length",  test_attribute_clamps_an_overlong_length_byte},
    {"attr_honours_short_length",    test_attribute_honours_a_short_length_byte},
    {"attr_rejects_bad_arguments",   test_attribute_rejects_bad_arguments},
};

test_stats_t run_zb_lumi_tests(void)
{
    ESP_LOGI(TAG, "Running Aqara 0xFF01 Parser Tests");
    return test_run_suite(zb_lumi_tests, sizeof(zb_lumi_tests) / sizeof(zb_lumi_tests[0]));
}
