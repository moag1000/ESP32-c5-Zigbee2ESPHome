/**
 * @file test_esphome_noise.c
 * @brief Unit tests for ESPHome Noise Protocol implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "esphome/esphome_noise.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "TEST_NOISE";

/* ============================================================================
 * Test Fixtures
 * ============================================================================ */

/* Test PSK (Base64 encoded 32 random bytes) */
static const char *TEST_PSK_BASE64 = "MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTI=";

/* Test device info */
static const char *TEST_DEVICE_NAME = "esp32c5_test";
static const char *TEST_MAC_ADDRESS = "AABBCCDDEEFF";

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* No hex-dump helper here on purpose.
 *
 * This file used to print a decoded PSK and a freshly generated key to the
 * serial log. Test keys or not, dumping key material is the same habit that
 * put four bytes of a live session key into esphome_noise.c's debug output,
 * and test output is not a private channel. Assert on the bytes instead. */

/* ============================================================================
 * Basic Tests
 * ============================================================================ */

/**
 * @brief Test Noise module initialization
 */
static void test_noise_init(void)
{
    esp_err_t ret = esphome_noise_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * @brief Test Noise context creation with PSK
 */
static void test_noise_create_with_psk(void)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        TEST_PSK_BASE64, TEST_DEVICE_NAME, TEST_MAC_ADDRESS);

    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_EQUAL(ESPHOME_NOISE_STATE_INIT, esphome_noise_get_state(ctx));
    TEST_ASSERT_FALSE(esphome_noise_is_ready(ctx));

    esphome_noise_destroy(ctx);
}

/**
 * @brief Test Noise context creation without PSK
 */
static void test_noise_create_without_psk(void)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        NULL, TEST_DEVICE_NAME, TEST_MAC_ADDRESS);

    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_EQUAL(ESPHOME_NOISE_STATE_INIT, esphome_noise_get_state(ctx));

    esphome_noise_destroy(ctx);
}

/**
 * @brief Test Noise context creation with empty PSK
 */
static void test_noise_create_empty_psk(void)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        "", TEST_DEVICE_NAME, TEST_MAC_ADDRESS);

    TEST_ASSERT_NOT_NULL(ctx);

    esphome_noise_destroy(ctx);
}

/**
 * @brief Test invalid Base64 PSK
 */
static void test_noise_create_invalid_psk(void)
{
    /* Invalid Base64 should fail */
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        "invalid!!!base64", TEST_DEVICE_NAME, TEST_MAC_ADDRESS);

    TEST_ASSERT_NULL(ctx);
}

/**
 * @brief Test context reset
 */
static void test_noise_reset(void)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        TEST_PSK_BASE64, TEST_DEVICE_NAME, TEST_MAC_ADDRESS);
    TEST_ASSERT_NOT_NULL(ctx);

    /* Process Hello to change state */
    uint8_t hello_data[] = {0x01};
    uint8_t response[128];
    size_t response_len;

    esp_err_t ret = esphome_noise_process_hello(ctx, hello_data, 1,
                                                  response, sizeof(response),
                                                  &response_len);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(ESPHOME_NOISE_STATE_HELLO_SENT, esphome_noise_get_state(ctx));

    /* Reset should return to INIT state */
    ret = esphome_noise_reset(ctx);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(ESPHOME_NOISE_STATE_INIT, esphome_noise_get_state(ctx));

    esphome_noise_destroy(ctx);
}

/* ============================================================================
 * Hello Phase Tests
 * ============================================================================ */

/**
 * @brief Test is_hello detection
 */
static void test_noise_is_hello(void)
{
    uint8_t hello_marker[] = {0x01};
    uint8_t not_hello[] = {0x00};
    uint8_t empty[] = {};

    TEST_ASSERT_TRUE(esphome_noise_is_hello(hello_marker, 1));
    TEST_ASSERT_FALSE(esphome_noise_is_hello(not_hello, 1));
    TEST_ASSERT_FALSE(esphome_noise_is_hello(empty, 0));
    TEST_ASSERT_FALSE(esphome_noise_is_hello(NULL, 0));
}

/**
 * @brief Test Hello processing
 */
static void test_noise_process_hello(void)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        TEST_PSK_BASE64, TEST_DEVICE_NAME, TEST_MAC_ADDRESS);
    TEST_ASSERT_NOT_NULL(ctx);

    uint8_t hello_data[] = {0x01};
    uint8_t response[128];
    size_t response_len;

    esp_err_t ret = esphome_noise_process_hello(ctx, hello_data, 1,
                                                  response, sizeof(response),
                                                  &response_len);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_GREATER_THAN(0, response_len);

    /* Response should start with 0x01 marker */
    TEST_ASSERT_EQUAL(0x01, response[0]);

    /* Response should contain device name and MAC */
    TEST_ASSERT_EQUAL(ESPHOME_NOISE_STATE_HELLO_SENT, esphome_noise_get_state(ctx));


    esphome_noise_destroy(ctx);
}

/**
 * @brief Test Hello with invalid marker
 */
/**
 * @brief Hello accepts any payload — the marker is not this layer's job
 *
 * This test used to feed process_hello() a "wrong marker" byte and expect
 * ESP_ERR/STATE_ERROR. That contract does not exist: the 0x01 Noise frame
 * indicator is checked by esphome_api_server.c before it calls in here, and
 * only the payload is passed on (rx_buffer + ESPHOME_NOISE_FRAME_HEADER_SIZE).
 * The ESPHome Hello payload itself carries no marker and may be empty, so
 * rejecting a byte value here would break real clients.
 *
 * Auditing that layering is what turned up the missing length bound on the
 * same path — see the Hello size check in esphome_api_server.c.
 */
static void test_noise_hello_accepts_any_payload(void)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        TEST_PSK_BASE64, TEST_DEVICE_NAME, TEST_MAC_ADDRESS);
    TEST_ASSERT_NOT_NULL(ctx);

    uint8_t response[128];
    size_t response_len;

    /* Empty payload — the common case from Home Assistant. */
    TEST_ASSERT_EQUAL(ESP_OK, esphome_noise_process_hello(ctx, NULL, 0,
                                                          response, sizeof(response),
                                                          &response_len));
    TEST_ASSERT_EQUAL(ESPHOME_NOISE_STATE_HELLO_SENT, esphome_noise_get_state(ctx));
    TEST_ASSERT_EQUAL(0x01, response[0]);

    esphome_noise_destroy(ctx);
}

/**
 * @brief A response buffer too small for the ServerHello must be refused
 *
 * The ServerHello is 1 + name + 1 + mac + 1 bytes and is written into a
 * caller-provided buffer, so the size check is the only thing between a long
 * device name and a write past the end.
 */
static void test_noise_hello_small_response_buffer(void)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        TEST_PSK_BASE64, TEST_DEVICE_NAME, TEST_MAC_ADDRESS);
    TEST_ASSERT_NOT_NULL(ctx);

    uint8_t guarded[64];
    memset(guarded, 0x5A, sizeof(guarded));
    size_t response_len = 0;

    /* 4 bytes cannot hold "esp32c5_test" plus the MAC. */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, esphome_noise_process_hello(ctx, NULL, 0,
                                                              guarded, 4, &response_len));
    for (size_t i = 4; i < sizeof(guarded); i++) {
        TEST_ASSERT_EQUAL(0x5A, guarded[i]);
    }

    esphome_noise_destroy(ctx);
}

/* ============================================================================
 * Key Utility Tests
 * ============================================================================ */

/**
 * @brief Test Base64 key decoding
 */
static void test_noise_decode_key(void)
{
    uint8_t key[32];

    esp_err_t ret = esphome_noise_decode_key(TEST_PSK_BASE64, key);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* TEST_PSK_BASE64 is the Base64 of "12345678901234567890123456789012",
     * so the decode is checkable byte for byte rather than eyeballed. */
    TEST_ASSERT_EQUAL_MEMORY("12345678901234567890123456789012", key, 32);
}

/**
 * @brief Test key generation
 */
static void test_noise_generate_key(void)
{
    char key_base64[48];

    esp_err_t ret = esphome_noise_generate_key(key_base64, sizeof(key_base64));
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify it's valid Base64 by decoding it */
    uint8_t decoded[32];
    ret = esphome_noise_decode_key(key_base64, decoded);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* A generator that returned a constant or a zero key would still decode
     * cleanly, so check the bytes carry something. */
    bool all_zero = true;
    for (size_t i = 0; i < sizeof(decoded); i++) {
        if (decoded[i] != 0) {
            all_zero = false;
            break;
        }
    }
    TEST_ASSERT_FALSE(all_zero);

    /* Two calls must not agree. */
    char second_base64[48];
    uint8_t second[32];
    TEST_ASSERT_EQUAL(ESP_OK, esphome_noise_generate_key(second_base64, sizeof(second_base64)));
    TEST_ASSERT_EQUAL(ESP_OK, esphome_noise_decode_key(second_base64, second));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(decoded, second, sizeof(decoded)));
}

/* ============================================================================
 * State Query Tests
 * ============================================================================ */

/**
 * @brief Test state name function
 */
static void test_noise_state_name(void)
{
    TEST_ASSERT_EQUAL_STRING("INIT", esphome_noise_state_name(ESPHOME_NOISE_STATE_INIT));
    TEST_ASSERT_EQUAL_STRING("HELLO_SENT", esphome_noise_state_name(ESPHOME_NOISE_STATE_HELLO_SENT));
    TEST_ASSERT_EQUAL_STRING("READY", esphome_noise_state_name(ESPHOME_NOISE_STATE_READY));
    TEST_ASSERT_EQUAL_STRING("ERROR", esphome_noise_state_name(ESPHOME_NOISE_STATE_ERROR));
}

/**
 * @brief Test is_ready with different states
 */
static void test_noise_is_ready_states(void)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        TEST_PSK_BASE64, TEST_DEVICE_NAME, TEST_MAC_ADDRESS);
    TEST_ASSERT_NOT_NULL(ctx);

    /* Initially not ready */
    TEST_ASSERT_FALSE(esphome_noise_is_ready(ctx));

    /* After Hello, still not ready */
    uint8_t hello_data[] = {0x01};
    uint8_t response[128];
    size_t response_len;

    esphome_noise_process_hello(ctx, hello_data, 1, response, sizeof(response), &response_len);
    TEST_ASSERT_FALSE(esphome_noise_is_ready(ctx));

    esphome_noise_destroy(ctx);
}

/* ============================================================================
 * Encryption/Decryption Tests (require full handshake simulation)
 * ============================================================================ */

/**
 * @brief Test encryption fails before handshake complete
 */
static void test_noise_encrypt_before_ready(void)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        TEST_PSK_BASE64, TEST_DEVICE_NAME, TEST_MAC_ADDRESS);
    TEST_ASSERT_NOT_NULL(ctx);

    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t output[64];
    size_t output_len;

    esp_err_t ret = esphome_noise_encrypt(ctx, 1, payload, sizeof(payload),
                                           output, sizeof(output), &output_len);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);

    esphome_noise_destroy(ctx);
}

/**
 * @brief Test decryption fails before handshake complete
 */
static void test_noise_decrypt_before_ready(void)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        TEST_PSK_BASE64, TEST_DEVICE_NAME, TEST_MAC_ADDRESS);
    TEST_ASSERT_NOT_NULL(ctx);

    uint8_t ciphertext[] = {0x00, 0x00, 0x00, 0x00};
    uint16_t msg_type;
    uint8_t plaintext[64];
    size_t plaintext_len;

    esp_err_t ret = esphome_noise_decrypt(ctx, ciphertext, sizeof(ciphertext),
                                           &msg_type, plaintext, sizeof(plaintext),
                                           &plaintext_len);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);

    esphome_noise_destroy(ctx);
}

/* ============================================================================
 * Test Registration
 * ============================================================================ */

static const test_case_t esphome_noise_tests[] = {
    {"init",                     test_noise_init},
    {"create_with_psk",          test_noise_create_with_psk},
    {"create_without_psk",       test_noise_create_without_psk},
    {"create_empty_psk",         test_noise_create_empty_psk},
    {"create_invalid_psk",       test_noise_create_invalid_psk},
    {"reset",                    test_noise_reset},
    {"is_hello",                 test_noise_is_hello},
    {"process_hello",            test_noise_process_hello},
    {"hello_accepts_any_payload", test_noise_hello_accepts_any_payload},
    {"hello_small_response_buf",  test_noise_hello_small_response_buffer},
    {"decode_key",               test_noise_decode_key},
    {"generate_key",             test_noise_generate_key},
    {"state_name",               test_noise_state_name},
    {"is_ready_states",          test_noise_is_ready_states},
    {"encrypt_before_ready",     test_noise_encrypt_before_ready},
    {"decrypt_before_ready",     test_noise_decrypt_before_ready},
};

test_stats_t run_esphome_noise_tests(void)
{
    ESP_LOGI(TAG, "Running ESPHome Noise Tests");
    return test_run_suite(esphome_noise_tests,
                          sizeof(esphome_noise_tests) / sizeof(esphome_noise_tests[0]));
}
