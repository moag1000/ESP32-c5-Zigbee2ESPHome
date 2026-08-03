/**
 * @file test_esphome_protocol.c
 * @brief Tests for the ESPHome protobuf encoding and the message type IDs
 *
 * Two reasons this module is worth covering before anything else in esphome/:
 *
 * 1. It is pure logic. esphome_protocol.c includes nothing but its own header
 *    and esp_log.h, so it can be exercised without a client, a socket or a
 *    radio.
 * 2. It is where real bugs lived. Wrong message type IDs put Home Assistant
 *    into a disconnect loop (errno 104, ECONNRESET) and were only found by
 *    auditing every entity against the official api.proto. Nothing stopped
 *    them from coming back.
 *
 * The ID assertions below are deliberately hardcoded rather than derived from
 * the enum — a test that reads the same constant it is checking proves nothing.
 * These numbers come from aioesphomeapi's api.proto.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "esphome/esphome_protocol.h"
#include "esphome/esphome_common.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "TEST_PROTO";

#define BUF_SIZE 256

static uint8_t s_bytes[BUF_SIZE];

/** Fresh buffer over the shared backing array. */
static esphome_buffer_t make_buffer(void)
{
    memset(s_bytes, 0, sizeof(s_bytes));
    esphome_buffer_t buf = {
        .data = s_bytes,
        .size = sizeof(s_bytes),
        .position = 0,
        .overflow = false,
    };
    return buf;
}

/** Rewind a written buffer so the same bytes can be read back. */
static void rewind_buffer(esphome_buffer_t *buf)
{
    buf->position = 0;
}

/* ============================================================================
 * Varint — the base of everything else
 * ============================================================================ */

static void test_varint_roundtrip_boundaries(void)
{
    /* One byte per 7 bits, so the interesting values sit either side of each
     * multiple of 7. */
    const uint64_t values[] = {
        0, 1, 126, 127,          /* 1 byte  */
        128, 129, 16382, 16383,  /* 2 bytes */
        16384, 2097151,          /* 3 bytes */
        2097152, 0xFFFFFFFFull,  /* 4-5 bytes */
    };

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        esphome_buffer_t buf = make_buffer();
        TEST_ASSERT_EQUAL(ESP_OK, esphome_encode_varint(&buf, values[i]));
        TEST_ASSERT_FALSE(buf.overflow);

        rewind_buffer(&buf);
        uint64_t out = 0;
        TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_varint(&buf, &out));
        TEST_ASSERT_TRUE(out == values[i]);
    }
}

/** 127 must fit in one byte, 128 must take two. */
static void test_varint_uses_minimal_bytes(void)
{
    esphome_buffer_t buf = make_buffer();
    esphome_encode_varint(&buf, 127);
    TEST_ASSERT_EQUAL(1, (int)buf.position);

    buf = make_buffer();
    esphome_encode_varint(&buf, 128);
    TEST_ASSERT_EQUAL(2, (int)buf.position);
}

/* ============================================================================
 * Tags — field number and wire type share one varint
 * ============================================================================ */

static void test_tag_roundtrip(void)
{
    const uint32_t fields[] = {1, 2, 15, 16, 97, 1000};
    const protobuf_wire_type_t wire_types[] = {0, 1, 2, 5};

    for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
        for (size_t w = 0; w < sizeof(wire_types) / sizeof(wire_types[0]); w++) {
            esphome_buffer_t buf = make_buffer();
            TEST_ASSERT_EQUAL(ESP_OK,
                esphome_encode_tag(&buf, fields[f], wire_types[w]));

            rewind_buffer(&buf);
            uint32_t got_field = 0;
            protobuf_wire_type_t got_wire = 0;
            TEST_ASSERT_EQUAL(ESP_OK,
                esphome_decode_tag(&buf, &got_field, &got_wire));
            TEST_ASSERT_EQUAL((int)fields[f], (int)got_field);
            TEST_ASSERT_EQUAL((int)wire_types[w], (int)got_wire);
        }
    }
}

/* ============================================================================
 * Scalars
 * ============================================================================ */

static void test_uint32_roundtrip(void)
{
    esphome_buffer_t buf = make_buffer();
    TEST_ASSERT_EQUAL(ESP_OK, esphome_encode_uint32(&buf, 3, 123456u));

    rewind_buffer(&buf);
    uint32_t field = 0;
    protobuf_wire_type_t wire = 0;
    esphome_decode_tag(&buf, &field, &wire);
    TEST_ASSERT_EQUAL(3, (int)field);

    uint32_t value = 0;
    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_uint32(&buf, &value));
    TEST_ASSERT_EQUAL(123456, (int)value);
}

static void test_bool_roundtrip(void)
{
    for (int i = 0; i < 2; i++) {
        bool expected = (i == 1);
        esphome_buffer_t buf = make_buffer();
        TEST_ASSERT_EQUAL(ESP_OK, esphome_encode_bool(&buf, 1, expected));

        rewind_buffer(&buf);
        uint32_t field = 0;
    protobuf_wire_type_t wire = 0;
        esphome_decode_tag(&buf, &field, &wire);

        bool value = !expected;
        TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_bool(&buf, &value));
        TEST_ASSERT_TRUE(value == expected);
    }
}

/** fixed32 is little-endian and always four bytes — no varint compression. */
static void test_fixed32_is_four_bytes(void)
{
    esphome_buffer_t buf = make_buffer();
    TEST_ASSERT_EQUAL(ESP_OK, esphome_encode_fixed32(&buf, 1, 1u));
    /* one tag byte for a low field number, then exactly four payload bytes */
    TEST_ASSERT_EQUAL(5, (int)buf.position);

    rewind_buffer(&buf);
    uint32_t field = 0;
    protobuf_wire_type_t wire = 0;
    esphome_decode_tag(&buf, &field, &wire);
    TEST_ASSERT_EQUAL(5, (int)wire);  /* wire type 5 = 32-bit */

    uint32_t value = 0;
    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_fixed32(&buf, &value));
    TEST_ASSERT_EQUAL(1, (int)value);
}

static void test_float_roundtrip(void)
{
    esphome_buffer_t buf = make_buffer();
    TEST_ASSERT_EQUAL(ESP_OK, esphome_encode_float(&buf, 2, 21.5f));

    rewind_buffer(&buf);
    uint32_t field = 0;
    protobuf_wire_type_t wire = 0;
    esphome_decode_tag(&buf, &field, &wire);

    float value = 0.0f;
    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_float(&buf, &value));
    TEST_ASSERT_TRUE(fabsf(value - 21.5f) < 0.0001f);
}

static void test_string_roundtrip(void)
{
    const char *text = "lumi.vibration.aq1";
    esphome_buffer_t buf = make_buffer();
    TEST_ASSERT_EQUAL(ESP_OK, esphome_encode_string(&buf, 4, text));

    rewind_buffer(&buf);
    uint32_t field = 0;
    protobuf_wire_type_t wire = 0;
    esphome_decode_tag(&buf, &field, &wire);
    TEST_ASSERT_EQUAL(2, (int)wire);  /* wire type 2 = length-delimited */

    char out[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_string(&buf, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(text, out);
}

static void test_empty_string_roundtrip(void)
{
    esphome_buffer_t buf = make_buffer();
    TEST_ASSERT_EQUAL(ESP_OK, esphome_encode_string(&buf, 1, ""));

    rewind_buffer(&buf);
    uint32_t field = 0;
    protobuf_wire_type_t wire = 0;
    esphome_decode_tag(&buf, &field, &wire);

    char out[8] = "dirty";
    TEST_ASSERT_EQUAL(ESP_OK, esphome_decode_string(&buf, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ============================================================================
 * Overflow
 * ============================================================================ */

/** Writing past the end must set the flag rather than scribble past it. */
static void test_overflow_is_flagged_not_written(void)
{
    uint8_t tiny[2] = {0};
    esphome_buffer_t buf = {
        .data = tiny, .size = sizeof(tiny), .position = 0, .overflow = false,
    };

    /* A long string cannot fit in two bytes. */
    esphome_encode_string(&buf, 1, "this string is definitely too long");

    TEST_ASSERT_TRUE(buf.overflow);
    TEST_ASSERT_TRUE(buf.position <= sizeof(tiny));
}

/* ============================================================================
 * Message type IDs
 *
 * These are the regression tests that matter. Wrong IDs here put Home
 * Assistant into a disconnect loop, and the numbers are not guessable — they
 * come from api.proto. Hardcoded on purpose.
 * ============================================================================ */

static void test_message_ids_that_were_wrong(void)
{
    /* Each of these four was corrected after an audit against api.proto. */
    TEST_ASSERT_EQUAL(97, (int)ESPHOME_MSG_LIST_ENTITIES_TEXT);
    TEST_ASSERT_EQUAL(94, (int)ESPHOME_MSG_LIST_ENTITIES_ALARM_PANEL);
    TEST_ASSERT_EQUAL(83, (int)ESPHOME_MSG_BLE_GATT_WRITE_RESPONSE);
    TEST_ASSERT_EQUAL(59, (int)ESPHOME_MSG_LOCK_STATE);
}

static void test_service_message_ids(void)
{
    /* The gateway's Home Assistant services depend on these two. */
    TEST_ASSERT_EQUAL(41, (int)ESPHOME_MSG_LIST_SERVICES_RESPONSE);
    TEST_ASSERT_EQUAL(42, (int)ESPHOME_MSG_EXECUTE_SERVICE);
}

static void test_neighbouring_ids_are_distinct(void)
{
    /* The original bug was an ID colliding with a different message. Anything
     * sharing a number here would do the same again. */
    const int ids[] = {
        ESPHOME_MSG_LIST_ENTITIES_TEXT_SENSOR,
        ESPHOME_MSG_LIST_SERVICES_RESPONSE,
        ESPHOME_MSG_EXECUTE_SERVICE,
        ESPHOME_MSG_LOCK_STATE,
        ESPHOME_MSG_LOCK_COMMAND,
        ESPHOME_MSG_BLE_GATT_WRITE_REQUEST,
        ESPHOME_MSG_BLE_GATT_WRITE_RESPONSE,
        ESPHOME_MSG_LIST_ENTITIES_ALARM_PANEL,
        ESPHOME_MSG_ALARM_PANEL_STATE,
        ESPHOME_MSG_ALARM_PANEL_COMMAND,
        ESPHOME_MSG_LIST_ENTITIES_TEXT,
    };
    const size_t n = sizeof(ids) / sizeof(ids[0]);

    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            TEST_ASSERT_NOT_EQUAL(ids[i], ids[j]);
        }
    }
}

/* ============================================================================
 * Suite
 * ============================================================================ */

static const test_case_t esphome_protocol_tests[] = {
    {"varint_roundtrip_boundaries",   test_varint_roundtrip_boundaries},
    {"varint_uses_minimal_bytes",     test_varint_uses_minimal_bytes},
    {"tag_roundtrip",                 test_tag_roundtrip},
    {"uint32_roundtrip",              test_uint32_roundtrip},
    {"bool_roundtrip",                test_bool_roundtrip},
    {"fixed32_is_four_bytes",         test_fixed32_is_four_bytes},
    {"float_roundtrip",               test_float_roundtrip},
    {"string_roundtrip",              test_string_roundtrip},
    {"empty_string_roundtrip",        test_empty_string_roundtrip},
    {"overflow_is_flagged",           test_overflow_is_flagged_not_written},
    {"message_ids_that_were_wrong",   test_message_ids_that_were_wrong},
    {"service_message_ids",           test_service_message_ids},
    {"neighbouring_ids_distinct",     test_neighbouring_ids_are_distinct},
};

test_stats_t run_esphome_protocol_tests(void)
{
    ESP_LOGI(TAG, "Running ESPHome Protocol Tests");
    return test_run_suite(esphome_protocol_tests,
                          sizeof(esphome_protocol_tests) / sizeof(esphome_protocol_tests[0]));
}
