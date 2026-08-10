/**
 * @file test_esphome_execute_service.c
 * @brief Tests for the ExecuteServiceRequest decoder
 *
 * This decoder was wrong for as long as the gateway's services existed, and
 * nothing caught it. The services were announced correctly, appeared in Home
 * Assistant's service list, and every call was rejected with an argument-count
 * mismatch — a failure that only showed up on a serial console. The bug was
 * that `repeated ExecuteServiceArgument args = 2` was read as a flat field per
 * type, a layout the protocol does not have.
 *
 * So the first test here is the exact frame aioesphomeapi puts on the wire for
 * a one-string-argument call. If that one passes, the shape of the message is
 * understood; the rest cover the parts that are easy to get subtly wrong —
 * zigzag on field 5, empty submessages for proto3 defaults, and messages that
 * lie about their lengths.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "esphome/esphome_protocol.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TEST_EXEC_SVC";

#define MAX_ARGS ESPHOME_MAX_SERVICE_ARGS

/* ============================================================================
 * Frame construction
 * ============================================================================ */

/** Append a protobuf tag: (field << 3) | wire_type, as a single-byte varint. */
static size_t put_tag(uint8_t *buf, size_t pos, uint32_t field, uint32_t wire)
{
    buf[pos++] = (uint8_t)((field << 3) | wire);
    return pos;
}

/** Append the `key` field: field 1, fixed32, little-endian. */
static size_t put_key(uint8_t *buf, size_t pos, uint32_t key)
{
    pos = put_tag(buf, pos, 1, 5 /* 32BIT */);
    buf[pos++] = (uint8_t)(key & 0xFF);
    buf[pos++] = (uint8_t)((key >> 8) & 0xFF);
    buf[pos++] = (uint8_t)((key >> 16) & 0xFF);
    buf[pos++] = (uint8_t)((key >> 24) & 0xFF);
    return pos;
}

/** Append a varint. Lengths above 127 need more than one byte. */
static size_t put_varint(uint8_t *buf, size_t pos, uint64_t value)
{
    while (value >= 0x80) {
        buf[pos++] = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    buf[pos++] = (uint8_t)value;
    return pos;
}

/** Append one args entry: field 2, length-delimited, wrapping @p body. */
static size_t put_arg(uint8_t *buf, size_t pos, const uint8_t *body, size_t body_len)
{
    pos = put_tag(buf, pos, 2, 2 /* LEN */);
    pos = put_varint(buf, pos, body_len);
    if (body_len > 0) {
        memcpy(buf + pos, body, body_len);
        pos += body_len;
    }
    return pos;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

/**
 * The frame Home Assistant actually sends for reconfigure_device.
 *
 * Captured from aioesphomeapi: key 3007205343, one string argument holding an
 * IEEE address. This is the case the old decoder failed, reporting two
 * arguments where one was sent.
 */
static void test_the_frame_home_assistant_sends(void)
{
    const char *ieee = "0x00158d0002c4aab4";
    uint8_t body[64];
    size_t bl = 0;
    bl = put_tag(body, bl, 4, 2);              /* string_ */
    body[bl++] = (uint8_t)strlen(ieee);
    memcpy(body + bl, ieee, strlen(ieee));
    bl += strlen(ieee);

    uint8_t frame[128];
    size_t fl = put_key(frame, 0, 3007205343u);
    fl = put_arg(frame, fl, body, bl);

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(3007205343u, key);
    TEST_ASSERT_EQUAL(1, count);              /* the old decoder said 2 */
    TEST_ASSERT_TRUE(present[0]);
    TEST_ASSERT_EQUAL(ESPHOME_SERVICE_ARG_STRING, args[0].type);
    TEST_ASSERT_EQUAL_STRING(ieee, args[0].string_value);
}

/**
 * permit_join(0) — an argument at its type's default.
 *
 * Proto3 leaves such a field out, so the submessage arrives empty. The argument
 * still exists and still counts; the caller learns nothing about its type,
 * which is what `present` reports.
 */
static void test_default_valued_argument_arrives_empty(void)
{
    uint8_t frame[32];
    size_t fl = put_key(frame, 0, 3930921259u);
    fl = put_arg(frame, fl, NULL, 0);

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(3930921259u, key);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_FALSE(present[0]);
    TEST_ASSERT_EQUAL(0, args[0].int_value);
}

/** Field 5 is a sint32: zigzag, so 1 encodes as 2 and -1 as 1. */
static void test_sint32_is_zigzag_decoded(void)
{
    struct { uint8_t encoded; int32_t expected; } cases[] = {
        {0, 0}, {1, -1}, {2, 1}, {3, -2}, {4, 2}, {120, 60},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t body[4];
        size_t bl = put_tag(body, 0, 5, 0 /* VARINT */);
        body[bl++] = cases[i].encoded;

        uint8_t frame[32];
        size_t fl = put_key(frame, 0, 7);
        fl = put_arg(frame, fl, body, bl);

        uint32_t key = 0;
        esphome_service_arg_value_t args[MAX_ARGS];
        bool present[MAX_ARGS];
        size_t count = 0;

        TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_execute_service(
            frame, fl, &key, args, present, MAX_ARGS, &count));
        TEST_ASSERT_EQUAL(1, count);
        TEST_ASSERT_TRUE(present[0]);
        TEST_ASSERT_EQUAL(ESPHOME_SERVICE_ARG_INT, args[0].type);
        TEST_ASSERT_EQUAL(cases[i].expected, args[0].int_value);
    }
}

/** Field 2 is a plain int32, not zigzag — the two integer fields differ. */
static void test_legacy_int_is_not_zigzag(void)
{
    uint8_t body[4];
    size_t bl = put_tag(body, 0, 2, 0);
    body[bl++] = 60;

    uint8_t frame[32];
    size_t fl = put_key(frame, 0, 7);
    fl = put_arg(frame, fl, body, bl);

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(60, args[0].int_value);   /* zigzag would read 30 */
}

/** bool_ is field 1 of the submessage, which is also the outer key's number. */
static void test_bool_argument(void)
{
    uint8_t body[4];
    size_t bl = put_tag(body, 0, 1, 0);
    body[bl++] = 1;

    uint8_t frame[32];
    size_t fl = put_key(frame, 0, 7);
    fl = put_arg(frame, fl, body, bl);

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(ESPHOME_SERVICE_ARG_BOOL, args[0].type);
    TEST_ASSERT_TRUE(args[0].bool_value);
}

/** float_ is a fixed32 at field 3. */
static void test_float_argument(void)
{
    float value = 21.5f;
    uint8_t body[8];
    size_t bl = put_tag(body, 0, 3, 5 /* 32BIT */);
    memcpy(body + bl, &value, sizeof(value));
    bl += sizeof(value);

    uint8_t frame[32];
    size_t fl = put_key(frame, 0, 7);
    fl = put_arg(frame, fl, body, bl);

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(ESPHOME_SERVICE_ARG_FLOAT, args[0].type);
    TEST_ASSERT_TRUE(args[0].float_value > 21.4f && args[0].float_value < 21.6f);
}

/** Several arguments keep their order. */
static void test_arguments_stay_in_order(void)
{
    uint8_t frame[128];
    size_t fl = put_key(frame, 0, 7);

    for (uint8_t i = 0; i < 3; i++) {
        uint8_t body[4];
        size_t bl = put_tag(body, 0, 2, 0);
        body[bl++] = (uint8_t)(10 + i);
        fl = put_arg(frame, fl, body, bl);
    }

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(3, count);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(10 + i, args[i].int_value);
    }
}

/**
 * The key may follow the arguments.
 *
 * Protobuf does not promise field order. Nothing sends it this way today, so
 * this is the case that would rot unnoticed.
 */
static void test_key_after_arguments(void)
{
    uint8_t body[4];
    size_t bl = put_tag(body, 0, 2, 0);
    body[bl++] = 42;

    uint8_t frame[32];
    size_t fl = put_arg(frame, 0, body, bl);
    fl = put_key(frame, fl, 12345u);

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(12345u, key);
    TEST_ASSERT_EQUAL(42, args[0].int_value);
}

/** An unknown outer field is skipped, and parsing continues past it. */
static void test_unknown_field_is_skipped(void)
{
    uint8_t frame[64];
    size_t fl = put_key(frame, 0, 7);
    fl = put_tag(frame, fl, 9, 0);      /* not in this message */
    frame[fl++] = 99;

    uint8_t body[4];
    size_t bl = put_tag(body, 0, 2, 0);
    body[bl++] = 5;
    fl = put_arg(frame, fl, body, bl);

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(7u, key);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(5, args[0].int_value);
}

/**
 * A submessage claiming more bytes than the frame holds is refused.
 *
 * Reading it would walk off the payload — the same shape as the Hello-length
 * overflow this project already shipped once.
 */
static void test_submessage_longer_than_frame_is_refused(void)
{
    uint8_t frame[32];
    size_t fl = put_key(frame, 0, 7);
    fl = put_tag(frame, fl, 2, 2);
    frame[fl++] = 100;                  /* claims 100 bytes; none follow */

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
}

/** Arguments beyond capacity are refused, not silently dropped. */
static void test_too_many_arguments_are_refused(void)
{
    uint8_t frame[256];
    size_t fl = put_key(frame, 0, 7);

    for (size_t i = 0; i < MAX_ARGS + 1; i++) {
        uint8_t body[4];
        size_t bl = put_tag(body, 0, 2, 0);
        body[bl++] = 1;
        fl = put_arg(frame, fl, body, bl);
    }

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
}

/** A string longer than the field is truncated, and parsing survives it. */
static void test_overlong_string_is_truncated(void)
{
    const size_t big_len = 150;         /* > the 128-byte string field */
    uint8_t body[256];
    size_t bl = put_tag(body, 0, 4, 2);
    bl = put_varint(body, bl, big_len);
    memset(body + bl, 'x', big_len);
    bl += big_len;

    uint8_t frame[512];
    size_t fl = put_key(frame, 0, 7);
    fl = put_arg(frame, fl, body, bl);

    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;

    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_execute_service(
        frame, fl, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(sizeof(args[0].string_value) - 1,
                      strlen(args[0].string_value));
}

/** Null arguments and an empty payload are rejected rather than dereferenced. */
static void test_null_and_empty_are_rejected(void)
{
    uint32_t key = 0;
    esphome_service_arg_value_t args[MAX_ARGS];
    bool present[MAX_ARGS];
    size_t count = 0;
    uint8_t frame[4] = {0};

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esphome_decode_execute_service(
        NULL, 4, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esphome_decode_execute_service(
        frame, 0, &key, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esphome_decode_execute_service(
        frame, 4, NULL, args, present, MAX_ARGS, &count));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esphome_decode_execute_service(
        frame, 4, &key, args, present, 0, &count));
}

/* ============================================================================
 * Suite
 * ============================================================================ */

static const test_case_t esphome_execute_service_tests[] = {
    {"the_frame_home_assistant_sends",       test_the_frame_home_assistant_sends},
    {"default_valued_argument_arrives_empty", test_default_valued_argument_arrives_empty},
    {"sint32_is_zigzag_decoded",             test_sint32_is_zigzag_decoded},
    {"legacy_int_is_not_zigzag",             test_legacy_int_is_not_zigzag},
    {"bool_argument",                        test_bool_argument},
    {"float_argument",                       test_float_argument},
    {"arguments_stay_in_order",              test_arguments_stay_in_order},
    {"key_after_arguments",                  test_key_after_arguments},
    {"unknown_field_is_skipped",             test_unknown_field_is_skipped},
    {"submessage_longer_than_frame_refused", test_submessage_longer_than_frame_is_refused},
    {"too_many_arguments_are_refused",       test_too_many_arguments_are_refused},
    {"overlong_string_is_truncated",         test_overlong_string_is_truncated},
    {"null_and_empty_are_rejected",          test_null_and_empty_are_rejected},
};

test_stats_t run_esphome_execute_service_tests(void)
{
    ESP_LOGI(TAG, "Running ExecuteServiceRequest Decoder Tests");
    return test_run_suite(esphome_execute_service_tests,
                          sizeof(esphome_execute_service_tests) /
                          sizeof(esphome_execute_service_tests[0]));
}
