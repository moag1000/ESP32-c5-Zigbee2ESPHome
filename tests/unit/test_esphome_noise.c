/**
 * @file test_esphome_noise.c
 * @brief Unit tests for ESPHome Noise Protocol implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "test_framework.h"
#include "esphome/esphome_noise.h"
#include <string.h>

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

static void test_hex_dump(const char *name, const uint8_t *data, size_t len)
{
    printf("%s (%zu bytes): ", name, len);
    for (size_t i = 0; i < len && i < 32; i++) {
        printf("%02X", data[i]);
    }
    if (len > 32) {
        printf("...");
    }
    printf("\n");
}

/* ============================================================================
 * Basic Tests
 * ============================================================================ */

/**
 * @brief Test Noise module initialization
 */
TEST_CASE(test_noise_init)
{
    esp_err_t ret = esphome_noise_init();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * @brief Test Noise context creation with PSK
 */
TEST_CASE(test_noise_create_with_psk)
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
TEST_CASE(test_noise_create_without_psk)
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
TEST_CASE(test_noise_create_empty_psk)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        "", TEST_DEVICE_NAME, TEST_MAC_ADDRESS);

    TEST_ASSERT_NOT_NULL(ctx);

    esphome_noise_destroy(ctx);
}

/**
 * @brief Test invalid Base64 PSK
 */
TEST_CASE(test_noise_create_invalid_psk)
{
    /* Invalid Base64 should fail */
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        "invalid!!!base64", TEST_DEVICE_NAME, TEST_MAC_ADDRESS);

    TEST_ASSERT_NULL(ctx);
}

/**
 * @brief Test context reset
 */
TEST_CASE(test_noise_reset)
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
TEST_CASE(test_noise_is_hello)
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
TEST_CASE(test_noise_process_hello)
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
    TEST_ASSERT_GREATER(response_len, 0);

    /* Response should start with 0x01 marker */
    TEST_ASSERT_EQUAL(0x01, response[0]);

    /* Response should contain device name and MAC */
    TEST_ASSERT_EQUAL(ESPHOME_NOISE_STATE_HELLO_SENT, esphome_noise_get_state(ctx));

    test_hex_dump("Hello response", response, response_len);

    esphome_noise_destroy(ctx);
}

/**
 * @brief Test Hello with invalid marker
 */
TEST_CASE(test_noise_hello_invalid_marker)
{
    esphome_noise_ctx_t *ctx = esphome_noise_create(
        TEST_PSK_BASE64, TEST_DEVICE_NAME, TEST_MAC_ADDRESS);
    TEST_ASSERT_NOT_NULL(ctx);

    uint8_t invalid_hello[] = {0x00};  /* Wrong marker */
    uint8_t response[128];
    size_t response_len;

    esp_err_t ret = esphome_noise_process_hello(ctx, invalid_hello, 1,
                                                  response, sizeof(response),
                                                  &response_len);

    TEST_ASSERT_NOT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(ESPHOME_NOISE_STATE_ERROR, esphome_noise_get_state(ctx));

    esphome_noise_destroy(ctx);
}

/* ============================================================================
 * Key Utility Tests
 * ============================================================================ */

/**
 * @brief Test Base64 key decoding
 */
TEST_CASE(test_noise_decode_key)
{
    uint8_t key[32];

    esp_err_t ret = esphome_noise_decode_key(TEST_PSK_BASE64, key);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    test_hex_dump("Decoded key", key, 32);
}

/**
 * @brief Test key generation
 */
TEST_CASE(test_noise_generate_key)
{
    char key_base64[48];

    esp_err_t ret = esphome_noise_generate_key(key_base64, sizeof(key_base64));
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify it's valid Base64 by decoding it */
    uint8_t decoded[32];
    ret = esphome_noise_decode_key(key_base64, decoded);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    printf("Generated key: %s\n", key_base64);
}

/* ============================================================================
 * State Query Tests
 * ============================================================================ */

/**
 * @brief Test state name function
 */
TEST_CASE(test_noise_state_name)
{
    TEST_ASSERT_STR_EQ("INIT", esphome_noise_state_name(ESPHOME_NOISE_STATE_INIT));
    TEST_ASSERT_STR_EQ("HELLO_SENT", esphome_noise_state_name(ESPHOME_NOISE_STATE_HELLO_SENT));
    TEST_ASSERT_STR_EQ("READY", esphome_noise_state_name(ESPHOME_NOISE_STATE_READY));
    TEST_ASSERT_STR_EQ("ERROR", esphome_noise_state_name(ESPHOME_NOISE_STATE_ERROR));
}

/**
 * @brief Test is_ready with different states
 */
TEST_CASE(test_noise_is_ready_states)
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
TEST_CASE(test_noise_encrypt_before_ready)
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
TEST_CASE(test_noise_decrypt_before_ready)
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

TEST_GROUP(esphome_noise)
{
    /* Basic tests */
    TEST_ADD(test_noise_init);
    TEST_ADD(test_noise_create_with_psk);
    TEST_ADD(test_noise_create_without_psk);
    TEST_ADD(test_noise_create_empty_psk);
    TEST_ADD(test_noise_create_invalid_psk);
    TEST_ADD(test_noise_reset);

    /* Hello phase tests */
    TEST_ADD(test_noise_is_hello);
    TEST_ADD(test_noise_process_hello);
    TEST_ADD(test_noise_hello_invalid_marker);

    /* Key utility tests */
    TEST_ADD(test_noise_decode_key);
    TEST_ADD(test_noise_generate_key);

    /* State query tests */
    TEST_ADD(test_noise_state_name);
    TEST_ADD(test_noise_is_ready_states);

    /* Encryption/Decryption pre-condition tests */
    TEST_ADD(test_noise_encrypt_before_ready);
    TEST_ADD(test_noise_decrypt_before_ready);
}
