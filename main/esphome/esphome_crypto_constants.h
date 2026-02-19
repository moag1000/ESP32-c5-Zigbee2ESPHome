/**
 * @file esphome_crypto_constants.h
 * @brief Cryptographic Constants for ESPHome Noise Protocol
 *
 * Centralizes cryptographic constants used in Noise Protocol encryption,
 * HMAC operations, and related security functions.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_CRYPTO_CONSTANTS_H
#define ESPHOME_CRYPTO_CONSTANTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Field Element Constants (Curve25519)
 * ============================================================================ */

/** @brief Number of limbs in a field element (gf type) */
#define NOISE_FIELD_ELEMENT_LIMBS           16

/* ============================================================================
 * HMAC Constants
 * ============================================================================ */

/** @brief HMAC block size (SHA256 internal block) */
#define NOISE_HMAC_BLOCK_SIZE               64

/** @brief Maximum key length before hashing in HMAC */
#define NOISE_HMAC_KEY_MAX_LEN              64

/* ============================================================================
 * SHA256 / Hash Constants
 * ============================================================================ */

/** @brief SHA256 hash output size in bytes */
#define NOISE_HASH_SIZE                     32

/** @brief X25519 key size and DH output size in bytes */
#define NOISE_DH_OUTPUT_SIZE                32

/* ============================================================================
 * Handshake Constants
 * ============================================================================ */

/** @brief Minimum response buffer size for error responses */
#define NOISE_HANDSHAKE_MIN_RESPONSE_SIZE   20

/* ============================================================================
 * Encoding Constants
 * ============================================================================ */

/** @brief Minimum buffer size for Base64 encoded 32-byte key (44 chars + null) */
#define NOISE_BASE64_ENCODED_MIN_SIZE       45

/* ============================================================================
 * Testing Constants
 * ============================================================================ */

/** @brief Test advertisement count for BLE Proxy self-test */
#define ESPHOME_PROXY_TEST_ADV_COUNT        100

/** @brief Test sensor value for entity core self-test */
#define ESPHOME_SENSOR_TEST_VALUE           23.5f

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_CRYPTO_CONSTANTS_H */
