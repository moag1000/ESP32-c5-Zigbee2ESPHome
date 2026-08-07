/**
 * @file test_zb_tuya_parse.c
 * @brief Tests for the Tuya datapoint parser
 *
 * This parser reads frames a paired device puts on the air, including a 16-bit
 * length field that it then uses to fill a 64-byte buffer. That combination is
 * the one this project has already been bitten by once, in the ESPHome Hello
 * handler, so it is worth pinning down.
 *
 * The cases below are mostly about what happens when the frame lies: a length
 * larger than the buffer, a length larger than the frame, a frame too short to
 * hold its own header. The happy paths are here too, so a future change cannot
 * quietly stop parsing real devices in the name of safety.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "zigbee/zb_tuya.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TEST_TUYA";

/* Header layouts, from the notes at the top of zb_tuya.h:
 *   cmd 0x01/0x02: [status:1][seq:2][dp_id:1][type:1][len:2][data...]  (7 bytes)
 *   cmd 0x05:      [seq:2][dp_id:1][type:1][len:2][data...]            (6 bytes)
 * zb_tuya_parse_dp() takes the 7-byte form. */
#define HDR7 7

/** Build a 7-byte-header frame into buf, returning its total length. */
static size_t build_frame(uint8_t *buf, uint8_t dp_id, uint8_t type,
                          uint16_t declared_len, const uint8_t *payload,
                          size_t payload_len)
{
    buf[0] = 0x00;                          /* status */
    buf[1] = 0x00; buf[2] = 0x01;           /* seq */
    buf[3] = dp_id;
    buf[4] = type;
    buf[5] = (uint8_t)(declared_len >> 8);  /* length, big endian */
    buf[6] = (uint8_t)(declared_len & 0xFF);
    if (payload && payload_len) {
        memcpy(&buf[HDR7], payload, payload_len);
    }
    return HDR7 + payload_len;
}

/* ============================================================================
 * Rejection of malformed frames
 * ============================================================================ */

static void test_null_arguments_are_rejected(void)
{
    tuya_dp_t dp;
    uint8_t buf[16] = {0};

    TEST_ASSERT_NOT_EQUAL(ESP_OK, zb_tuya_parse_dp(NULL, sizeof(buf), &dp));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, sizeof(buf), NULL));
}

static void test_frame_shorter_than_header_is_rejected(void)
{
    tuya_dp_t dp;
    uint8_t buf[16] = {0};

    /* One byte short of the header is still short. */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, HDR7 - 1, &dp));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, 0, &dp));
}

static void test_payload_shorter_than_declared_is_rejected(void)
{
    tuya_dp_t dp;
    uint8_t buf[32] = {0};
    const uint8_t payload[2] = {0xAA, 0xBB};

    /* Frame says four bytes of payload and carries two. */
    size_t len = build_frame(buf, 101, TUYA_DP_TYPE_VALUE, 4, payload, sizeof(payload));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));
}

/* ============================================================================
 * The length field versus the buffer
 * ============================================================================ */

static void test_oversized_payload_is_clamped_to_the_buffer(void)
{
    tuya_dp_t dp;
    static uint8_t buf[HDR7 + 300];
    static uint8_t payload[300];

    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i + 1);
    }

    size_t len = build_frame(buf, 109, TUYA_DP_TYPE_RAW, sizeof(payload),
                             payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));

    /* dp->length has to describe the buffer, not the frame: every consumer
     * reads it as "bytes available in dp->value.raw" and sizes loops from it.
     * Leaving the frame's 300 there is what made tuya_fingerbot.c's step loop
     * safe only by accident. */
    TEST_ASSERT_EQUAL_UINT16(ZB_TUYA_DP_MAX_RAW_SIZE, dp.length);

    /* And the bytes that did fit are the leading ones, unchanged. */
    TEST_ASSERT_EQUAL_MEMORY(payload, dp.value.raw, ZB_TUYA_DP_MAX_RAW_SIZE);
}

static void test_payload_exactly_filling_the_buffer_is_kept_whole(void)
{
    tuya_dp_t dp;
    static uint8_t buf[HDR7 + ZB_TUYA_DP_MAX_RAW_SIZE];
    static uint8_t payload[ZB_TUYA_DP_MAX_RAW_SIZE];

    memset(payload, 0x5A, sizeof(payload));

    size_t len = build_frame(buf, 109, TUYA_DP_TYPE_RAW, sizeof(payload),
                             payload, sizeof(payload));
    TEST_ASSERT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));
    TEST_ASSERT_EQUAL_UINT16(ZB_TUYA_DP_MAX_RAW_SIZE, dp.length);
    TEST_ASSERT_EQUAL_MEMORY(payload, dp.value.raw, ZB_TUYA_DP_MAX_RAW_SIZE);
}

/* ============================================================================
 * Types that real devices send
 * ============================================================================ */

static void test_bool_datapoint(void)
{
    tuya_dp_t dp;
    uint8_t buf[16] = {0};
    const uint8_t on[1] = {0x01};
    const uint8_t off[1] = {0x00};

    size_t len = build_frame(buf, 107, TUYA_DP_TYPE_BOOL, 1, on, 1);
    TEST_ASSERT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));
    TEST_ASSERT_EQUAL_UINT8(107, dp.dp_id);
    TEST_ASSERT_TRUE(dp.value.bool_value);

    len = build_frame(buf, 107, TUYA_DP_TYPE_BOOL, 1, off, 1);
    TEST_ASSERT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));
    TEST_ASSERT_FALSE(dp.value.bool_value);
}

static void test_value_datapoint_is_big_endian(void)
{
    tuya_dp_t dp;
    uint8_t buf[16] = {0};
    /* 0x00000064 = 100, the shape a sustain time or movement limit arrives in */
    const uint8_t payload[4] = {0x00, 0x00, 0x00, 0x64};

    size_t len = build_frame(buf, 102, TUYA_DP_TYPE_VALUE, 4, payload, 4);
    TEST_ASSERT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));
    TEST_ASSERT_EQUAL(100, dp.value.int_value);
}

static void test_enum_datapoint(void)
{
    tuya_dp_t dp;
    uint8_t buf[16] = {0};
    const uint8_t payload[1] = {0x01};   /* Fingerbot mode 1 = switch */

    size_t len = build_frame(buf, 101, TUYA_DP_TYPE_ENUM, 1, payload, 1);
    TEST_ASSERT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));
    TEST_ASSERT_EQUAL_UINT8(1, dp.value.enum_value);
}

static void test_bitmap_widths(void)
{
    tuya_dp_t dp;
    uint8_t buf[16] = {0};
    const uint8_t one[1] = {0xAB};
    const uint8_t two[2] = {0x12, 0x34};
    const uint8_t four[4] = {0xDE, 0xAD, 0xBE, 0xEF};

    size_t len = build_frame(buf, 10, TUYA_DP_TYPE_BITMAP, 1, one, 1);
    TEST_ASSERT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));
    TEST_ASSERT_EQUAL(0xABu, dp.value.bitmap_value);

    len = build_frame(buf, 10, TUYA_DP_TYPE_BITMAP, 2, two, 2);
    TEST_ASSERT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));
    TEST_ASSERT_EQUAL(0x1234u, dp.value.bitmap_value);

    len = build_frame(buf, 10, TUYA_DP_TYPE_BITMAP, 4, four, 4);
    TEST_ASSERT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));
    TEST_ASSERT_EQUAL(0xDEADBEEFu, dp.value.bitmap_value);
}

static void test_zero_length_payload_leaves_value_zeroed(void)
{
    tuya_dp_t dp;
    uint8_t buf[16] = {0};

    memset(&dp, 0xFF, sizeof(dp));
    size_t len = build_frame(buf, 105, TUYA_DP_TYPE_VALUE, 0, NULL, 0);
    TEST_ASSERT_EQUAL(ESP_OK, zb_tuya_parse_dp(buf, len, &dp));
    TEST_ASSERT_EQUAL(0, dp.value.int_value);
}

/* ============================================================================
 * Suite
 * ============================================================================ */

static const test_case_t zb_tuya_parse_tests[] = {
    {"null_arguments_are_rejected",              test_null_arguments_are_rejected},
    {"frame_shorter_than_header_is_rejected",    test_frame_shorter_than_header_is_rejected},
    {"payload_shorter_than_declared_is_rejected", test_payload_shorter_than_declared_is_rejected},
    {"oversized_payload_is_clamped_to_buffer",   test_oversized_payload_is_clamped_to_the_buffer},
    {"payload_exactly_filling_buffer_kept",      test_payload_exactly_filling_the_buffer_is_kept_whole},
    {"bool_datapoint",                           test_bool_datapoint},
    {"value_datapoint_is_big_endian",            test_value_datapoint_is_big_endian},
    {"enum_datapoint",                           test_enum_datapoint},
    {"bitmap_widths",                            test_bitmap_widths},
    {"zero_length_payload_leaves_value_zeroed",  test_zero_length_payload_leaves_value_zeroed},
};

test_stats_t run_zb_tuya_parse_tests(void)
{
    ESP_LOGI(TAG, "Running Tuya Datapoint Parser Tests");
    return test_run_suite(zb_tuya_parse_tests,
                          sizeof(zb_tuya_parse_tests) / sizeof(zb_tuya_parse_tests[0]));
}
