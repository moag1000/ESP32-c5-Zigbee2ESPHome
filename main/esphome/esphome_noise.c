/**
 * @file esphome_noise.c
 * @brief ESPHome Noise Protocol Encryption Implementation
 *
 * Implements Noise Protocol (NNpsk0) for ESPHome Native API encryption.
 * Uses mbedtls for cryptographic primitives.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_noise.h"
#include "esphome_common.h"
#include "esphome_crypto_constants.h"
#include "core/memory/memory_manager_ng.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_idf_version.h"

/* ESP-IDF v6.0+ uses PSA Crypto API, v5.x uses legacy mbedTLS API */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include "psa/crypto.h"
#define USE_PSA_CRYPTO 1
#else
#include "mbedtls/sha256.h"
#include "mbedtls/chachapoly.h"
#define USE_PSA_CRYPTO 0
#endif

#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"

/* Log tag */
static const char *TAG = "ESPHOME_NOISE";

/* ============================================================================
 * X25519 Implementation (TweetNaCl-based)
 * ============================================================================
 * Minimal X25519 implementation for Curve25519 Diffie-Hellman.
 * Based on TweetNaCl for portability.
 * ============================================================================ */

/*
 * X25519 Implementation (canonical TweetNaCl, RFC 7748)
 *
 * This is the portable, audited TweetNaCl implementation of X25519.
 * It uses car25519 for carry reduction which correctly handles negative
 * limbs (adding 1<<16 before shifting ensures non-negative values).
 */

typedef int64_t gf[16];

static const gf GF_121665 = {0xDB41, 1};
static const uint8_t CURVE25519_BASEPOINT[32] = {9};

static void car25519(gf o)
{
    int i;
    int64_t c;
    for (i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    int64_t t, i, c = ~(b - 1);
    for (i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n)
{
    int i, j, b;
    gf m, t;
    for (i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xFFED;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xFFFF - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xFFFF;
        }
        m[15] = t[15] - 0x7FFF - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[14] &= 0xFFFF;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2 * i] = t[i] & 0xFF;
        o[2 * i + 1] = t[i] >> 8;
    }
}

static void unpack25519(gf o, const uint8_t *n)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7FFF;
}

static void A(gf o, const gf a, const gf b)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void Z(gf o, const gf a, const gf b)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b)
{
    int64_t i, j, t[31];
    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++) {
        for (j = 0; j < 16; j++) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a)
{
    M(o, a, a);
}

static void inv25519(gf o, const gf i)
{
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

static void scalarmult(uint8_t *q, const uint8_t *n, const uint8_t *p)
{
    uint8_t z[32];
    int64_t x[80], r, i;
    gf a, b, c, d, e, f;

    for (i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;

    unpack25519(x, p);
    for (i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;

    for (i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        A(e, a, c);
        Z(a, a, c);
        A(c, b, d);
        Z(b, b, d);
        S(d, e);
        S(f, a);
        M(a, c, a);
        M(c, b, e);
        A(e, a, c);
        Z(a, a, c);
        S(b, a);
        Z(c, d, f);
        M(a, c, GF_121665);
        A(a, a, d);
        M(c, c, a);
        M(a, d, f);
        M(d, b, x);
        S(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    for (i = 0; i < 16; i++) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    inv25519(x + 32, x + 32);
    M(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);
}

/**
 * @brief Generate X25519 keypair
 */
static void x25519_keypair(uint8_t *public_key, uint8_t *private_key)
{
    esp_fill_random(private_key, 32);
    private_key[0] &= 248;
    private_key[31] &= 127;
    private_key[31] |= 64;
    scalarmult(public_key, private_key, CURVE25519_BASEPOINT);
}

/**
 * @brief Compute X25519 shared secret
 */
static void x25519_shared_secret(uint8_t *shared, const uint8_t *private_key,
                                  const uint8_t *public_key)
{
    scalarmult(shared, private_key, public_key);
}

/* ============================================================================
 * HMAC-SHA256 Implementation
 * ============================================================================ */

/**
 * @brief Compute HMAC-SHA256
 */
static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len,
                        uint8_t *out)
{
#if USE_PSA_CRYPTO
    /* PSA Crypto API: Use psa_mac_compute for HMAC-SHA256 */
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    size_t out_len = 0;

    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, key_len * 8);

    psa_status_t status = psa_import_key(&attributes, key, key_len, &key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key for HMAC failed: %d", (int)status);
        memset(out, 0, NOISE_HASH_SIZE);
        return;
    }

    status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                              data, data_len,
                              out, NOISE_HASH_SIZE, &out_len);
    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_mac_compute failed: %d", (int)status);
        memset(out, 0, NOISE_HASH_SIZE);
    }
#else
    /* Legacy mbedTLS API */
    uint8_t k_ipad[NOISE_HMAC_BLOCK_SIZE];
    uint8_t k_opad[NOISE_HMAC_BLOCK_SIZE];
    uint8_t k_hash[NOISE_HASH_SIZE];
    mbedtls_sha256_context ctx;

    /* If key is longer than block size, hash it */
    if (key_len > NOISE_HMAC_KEY_MAX_LEN) {
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, key, key_len);
        mbedtls_sha256_finish(&ctx, k_hash);
        mbedtls_sha256_free(&ctx);
        key = k_hash;
        key_len = NOISE_HASH_SIZE;
    }

    /* Prepare pads */
    memset(k_ipad, 0x36, NOISE_HMAC_BLOCK_SIZE);
    memset(k_opad, 0x5c, NOISE_HMAC_BLOCK_SIZE);
    for (size_t i = 0; i < key_len; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

    /* Inner hash: SHA256(k_ipad || data) */
    uint8_t inner[NOISE_HASH_SIZE];
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_ipad, NOISE_HMAC_BLOCK_SIZE);
    if (data_len > 0 && data != NULL) {
        mbedtls_sha256_update(&ctx, data, data_len);
    }
    mbedtls_sha256_finish(&ctx, inner);
    mbedtls_sha256_free(&ctx);

    /* Outer hash: SHA256(k_opad || inner) */
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_opad, NOISE_HMAC_BLOCK_SIZE);
    mbedtls_sha256_update(&ctx, inner, NOISE_HASH_SIZE);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);

    /* Clear sensitive data */
    mbedtls_platform_zeroize(k_ipad, sizeof(k_ipad));
    mbedtls_platform_zeroize(k_opad, sizeof(k_opad));
    mbedtls_platform_zeroize(inner, sizeof(inner));
#endif
}

/* ============================================================================
 * Noise Protocol Primitives
 * ============================================================================ */

/**
 * @brief SHA256 hash
 */
static void noise_hash(const uint8_t *data, size_t len, uint8_t *out)
{
#if USE_PSA_CRYPTO
    size_t hash_len = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, data, len,
                                            out, NOISE_HASH_SIZE, &hash_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_compute failed: %d", (int)status);
        memset(out, 0, NOISE_HASH_SIZE);
    }
#else
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
#endif
}

/**
 * @brief Noise HKDF function (Section 4.3 of Noise spec)
 *
 * Note: This is NOT RFC 5869 HKDF, but Noise's own variant.
 */
static void noise_hkdf(const uint8_t *ck, const uint8_t *ikm, size_t ikm_len,
                       uint8_t *out1, uint8_t *out2, uint8_t *out3)
{
    /* temp_key = HMAC(ck, ikm) */
    uint8_t temp_key[NOISE_HASH_SIZE];
    hmac_sha256(ck, NOISE_HASH_SIZE, ikm, ikm_len, temp_key);

    /* output1 = HMAC(temp_key, 0x01) */
    uint8_t one = 0x01;
    hmac_sha256(temp_key, NOISE_HASH_SIZE, &one, 1, out1);

    if (out2) {
        /* output2 = HMAC(temp_key, output1 || 0x02) */
        uint8_t buf[NOISE_HASH_SIZE + 1];
        memcpy(buf, out1, NOISE_HASH_SIZE);
        buf[NOISE_HASH_SIZE] = 0x02;
        hmac_sha256(temp_key, NOISE_HASH_SIZE, buf, NOISE_HASH_SIZE + 1, out2);

        if (out3) {
            /* output3 = HMAC(temp_key, output2 || 0x03) */
            memcpy(buf, out2, NOISE_HASH_SIZE);
            buf[NOISE_HASH_SIZE] = 0x03;
            hmac_sha256(temp_key, NOISE_HASH_SIZE, buf, NOISE_HASH_SIZE + 1, out3);
        }
    }

    mbedtls_platform_zeroize(temp_key, sizeof(temp_key));
}

/**
 * @brief MixKey operation (Section 4.3)
 */
static void noise_mix_key(esphome_noise_ctx_t *ctx, const uint8_t *ikm, size_t ikm_len)
{
    uint8_t temp_k[NOISE_HASH_SIZE];
    noise_hkdf(ctx->ck, ikm, ikm_len, ctx->ck, temp_k, NULL);
    memcpy(ctx->k, temp_k, NOISE_HASH_SIZE);
    ctx->n = 0;
    ctx->has_key = true;
    mbedtls_platform_zeroize(temp_k, sizeof(temp_k));
}

/**
 * @brief MixHash operation
 */
static void noise_mix_hash(esphome_noise_ctx_t *ctx, const uint8_t *data, size_t len)
{
#if USE_PSA_CRYPTO
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t hash_len = 0;

    psa_status_t status = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_setup failed: %d", (int)status);
        return;
    }

    status = psa_hash_update(&op, ctx->h, NOISE_HASH_SIZE);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_update (h) failed: %d", (int)status);
        psa_hash_abort(&op);
        return;
    }

    status = psa_hash_update(&op, data, len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_update (data) failed: %d", (int)status);
        psa_hash_abort(&op);
        return;
    }

    status = psa_hash_finish(&op, ctx->h, NOISE_HASH_SIZE, &hash_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_finish failed: %d", (int)status);
    }
#else
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    mbedtls_sha256_update(&sha_ctx, ctx->h, NOISE_HASH_SIZE);
    mbedtls_sha256_update(&sha_ctx, data, len);
    mbedtls_sha256_finish(&sha_ctx, ctx->h);
    mbedtls_sha256_free(&sha_ctx);
#endif
}

/**
 * @brief MixKeyAndHash operation for PSK
 */
static void noise_mix_key_and_hash(esphome_noise_ctx_t *ctx, const uint8_t *ikm, size_t ikm_len)
{
    uint8_t temp_h[NOISE_HASH_SIZE], temp_k[NOISE_HASH_SIZE];
    noise_hkdf(ctx->ck, ikm, ikm_len, ctx->ck, temp_h, temp_k);
    noise_mix_hash(ctx, temp_h, NOISE_HASH_SIZE);
    memcpy(ctx->k, temp_k, NOISE_HASH_SIZE);
    ctx->n = 0;
    ctx->has_key = true;
    mbedtls_platform_zeroize(temp_h, sizeof(temp_h));
    mbedtls_platform_zeroize(temp_k, sizeof(temp_k));
}

/**
 * @brief Build nonce for ChaCha20-Poly1305
 *
 * Format: [4 zero bytes][8-byte counter, little-endian]
 */
static void noise_build_nonce(uint64_t n, uint8_t *nonce)
{
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (n >> (i * 8)) & 0xff;
    }
}

/**
 * @brief Encrypt with AEAD (ChaCha20-Poly1305)
 */
static esp_err_t noise_encrypt_aead(const uint8_t *key, uint64_t nonce,
                                     const uint8_t *ad, size_t ad_len,
                                     const uint8_t *plaintext, size_t plaintext_len,
                                     uint8_t *ciphertext, uint8_t *tag)
{
    uint8_t nonce_bytes[12];
    noise_build_nonce(nonce, nonce_bytes);

#if USE_PSA_CRYPTO
    psa_status_t status;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    size_t output_len = 0;

    /* Ensure PSA Crypto is initialized (idempotent) */
    status = psa_crypto_init();
    if (status != PSA_SUCCESS && status != PSA_ERROR_ALREADY_EXISTS &&
        status != PSA_ERROR_BAD_STATE) {
        ESP_LOGE(TAG, "psa_crypto_init failed in encrypt: %d", (int)status);
        return ESP_FAIL;
    }

    /* Debug: verify key is valid */
    if (key == NULL) {
        ESP_LOGE(TAG, "Encrypt key is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    /* Import ChaCha20-Poly1305 key */
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CHACHA20_POLY1305);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_CHACHA20);
    psa_set_key_bits(&attributes, 256);

    status = psa_import_key(&attributes, key, 32, &key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key for encrypt failed: %d (key_type=0x%04x, bits=%d)",
                 (int)status, PSA_KEY_TYPE_CHACHA20, 256);
        return ESP_FAIL;
    }

    /* PSA AEAD outputs ciphertext || tag concatenated */
    uint8_t *output = mem_alloc(plaintext_len + 16, MEM_CAP_DEFAULT);
    if (output == NULL) {
        psa_destroy_key(key_id);
        ESP_LOGE(TAG, "Failed to allocate AEAD output buffer");
        return ESP_ERR_NO_MEM;
    }

    status = psa_aead_encrypt(key_id, PSA_ALG_CHACHA20_POLY1305,
                               nonce_bytes, 12,
                               ad, ad_len,
                               plaintext, plaintext_len,
                               output, plaintext_len + 16, &output_len);
    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        mem_ng_free(output);
        ESP_LOGE(TAG, "psa_aead_encrypt failed: %d", (int)status);
        return ESP_FAIL;
    }

    /* Split output into ciphertext and tag */
    memcpy(ciphertext, output, plaintext_len);
    memcpy(tag, output + plaintext_len, 16);
    mem_ng_free(output);

    return ESP_OK;
#else
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);

    int ret = mbedtls_chachapoly_setkey(&ctx, key);
    if (ret != 0) {
        mbedtls_chachapoly_free(&ctx);
        ESP_LOGE(TAG, "ChaCha20-Poly1305 setkey failed: %d", ret);
        return ESP_FAIL;
    }

    ret = mbedtls_chachapoly_encrypt_and_tag(&ctx, plaintext_len,
                                               nonce_bytes,
                                               ad, ad_len,
                                               plaintext,
                                               ciphertext,
                                               tag);
    mbedtls_chachapoly_free(&ctx);

    if (ret != 0) {
        ESP_LOGE(TAG, "ChaCha20-Poly1305 encrypt failed: %d", ret);
        return ESP_FAIL;
    }

    return ESP_OK;
#endif
}

/**
 * @brief Decrypt with AEAD (ChaCha20-Poly1305)
 */
static esp_err_t noise_decrypt_aead(const uint8_t *key, uint64_t nonce,
                                     const uint8_t *ad, size_t ad_len,
                                     const uint8_t *ciphertext, size_t ciphertext_len,
                                     const uint8_t *tag,
                                     uint8_t *plaintext)
{
    uint8_t nonce_bytes[12];
    noise_build_nonce(nonce, nonce_bytes);

#if USE_PSA_CRYPTO
    psa_status_t status;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    size_t output_len = 0;

    /* Ensure PSA Crypto is initialized (idempotent) */
    status = psa_crypto_init();
    if (status != PSA_SUCCESS && status != PSA_ERROR_ALREADY_EXISTS &&
        status != PSA_ERROR_BAD_STATE) {
        ESP_LOGE(TAG, "psa_crypto_init failed in decrypt: %d", (int)status);
        return ESP_FAIL;
    }

    /* Debug: verify key is valid */
    if (key == NULL) {
        ESP_LOGE(TAG, "Decrypt key is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    /* Import ChaCha20-Poly1305 key */
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CHACHA20_POLY1305);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_CHACHA20);
    psa_set_key_bits(&attributes, 256);

    ESP_LOGD(TAG, "Importing decrypt key (first 4 bytes: %02x %02x %02x %02x)",
             key[0], key[1], key[2], key[3]);

    status = psa_import_key(&attributes, key, 32, &key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key for decrypt failed: %d (key_type=0x%04x, bits=%d)",
                 (int)status, PSA_KEY_TYPE_CHACHA20, 256);
        return ESP_FAIL;
    }

    /* PSA AEAD expects ciphertext || tag concatenated as input */
    uint8_t *input = mem_alloc(ciphertext_len + 16, MEM_CAP_DEFAULT);
    if (input == NULL) {
        psa_destroy_key(key_id);
        ESP_LOGE(TAG, "Failed to allocate AEAD input buffer");
        return ESP_ERR_NO_MEM;
    }

    memcpy(input, ciphertext, ciphertext_len);
    memcpy(input + ciphertext_len, tag, 16);

    status = psa_aead_decrypt(key_id, PSA_ALG_CHACHA20_POLY1305,
                               nonce_bytes, 12,
                               ad, ad_len,
                               input, ciphertext_len + 16,
                               plaintext, ciphertext_len, &output_len);
    psa_destroy_key(key_id);
    mem_ng_free(input);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_aead_decrypt failed: %d", (int)status);
        return ESP_ERR_INVALID_CRC;  /* MAC verification failed */
    }

    return ESP_OK;
#else
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);

    int ret = mbedtls_chachapoly_setkey(&ctx, key);
    if (ret != 0) {
        mbedtls_chachapoly_free(&ctx);
        ESP_LOGE(TAG, "ChaCha20-Poly1305 setkey failed: %d", ret);
        return ESP_FAIL;
    }

    ret = mbedtls_chachapoly_auth_decrypt(&ctx, ciphertext_len,
                                            nonce_bytes,
                                            ad, ad_len,
                                            tag,
                                            ciphertext,
                                            plaintext);
    mbedtls_chachapoly_free(&ctx);

    if (ret != 0) {
        ESP_LOGE(TAG, "ChaCha20-Poly1305 decrypt/verify failed: %d", ret);
        return ESP_ERR_INVALID_CRC;  /* MAC verification failed */
    }

    return ESP_OK;
#endif
}

/**
 * @brief EncryptAndHash operation
 */
static esp_err_t noise_encrypt_and_hash(esphome_noise_ctx_t *ctx,
                                         const uint8_t *plaintext, size_t plaintext_len,
                                         uint8_t *ciphertext)
{
    if (!ctx->has_key) {
        /* No key yet, just copy plaintext */
        if (plaintext_len > 0 && plaintext != NULL) {
            memcpy(ciphertext, plaintext, plaintext_len);
        }
        noise_mix_hash(ctx, ciphertext, plaintext_len);
        return ESP_OK;
    }

    uint8_t tag[ESPHOME_NOISE_MAC_SIZE];
    /* Use empty buffer for NULL plaintext (for empty payload encryption) */
    static const uint8_t empty_buf[1] = {0};
    const uint8_t *pt = (plaintext != NULL) ? plaintext : empty_buf;

    esp_err_t ret = noise_encrypt_aead(ctx->k, ctx->n, ctx->h, NOISE_HASH_SIZE,
                                        pt, plaintext_len,
                                        ciphertext, tag);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Append tag after ciphertext */
    memcpy(ciphertext + plaintext_len, tag, ESPHOME_NOISE_MAC_SIZE);
    noise_mix_hash(ctx, ciphertext, plaintext_len + ESPHOME_NOISE_MAC_SIZE);
    ctx->n++;

    return ESP_OK;
}

/**
 * @brief DecryptAndHash operation
 */
static esp_err_t noise_decrypt_and_hash(esphome_noise_ctx_t *ctx,
                                         const uint8_t *ciphertext, size_t ciphertext_len,
                                         uint8_t *plaintext, size_t *plaintext_len)
{
    if (!ctx->has_key) {
        /* No key yet, just copy ciphertext */
        memcpy(plaintext, ciphertext, ciphertext_len);
        *plaintext_len = ciphertext_len;
        noise_mix_hash(ctx, ciphertext, ciphertext_len);
        return ESP_OK;
    }

    if (ciphertext_len < ESPHOME_NOISE_MAC_SIZE) {
        ESP_LOGE(TAG, "Ciphertext too short for MAC");
        return ESP_ERR_INVALID_SIZE;
    }

    size_t data_len = ciphertext_len - ESPHOME_NOISE_MAC_SIZE;
    const uint8_t *tag = ciphertext + data_len;

    esp_err_t ret = noise_decrypt_aead(ctx->k, ctx->n, ctx->h, NOISE_HASH_SIZE,
                                        ciphertext, data_len,
                                        tag, plaintext);
    if (ret != ESP_OK) {
        return ret;
    }

    noise_mix_hash(ctx, ciphertext, ciphertext_len);
    *plaintext_len = data_len;
    ctx->n++;

    return ESP_OK;
}

/**
 * @brief Split operation - derive transport keys
 */
static void noise_split(esphome_noise_ctx_t *ctx)
{
    /* For server (responder): first key is for decryption, second for encryption */
    noise_hkdf(ctx->ck, NULL, 0, ctx->decrypt_key, ctx->encrypt_key, NULL);
    ctx->encrypt_nonce = 0;
    ctx->decrypt_nonce = 0;
}

/* ============================================================================
 * Initialization Functions
 * ============================================================================ */

/**
 * @brief Initialize protocol name hash
 *
 * Protocol name: "Noise_NNpsk0_25519_ChaChaPoly_SHA256"
 */
static void noise_init_symmetric_state(esphome_noise_ctx_t *ctx)
{
    static const char PROTOCOL_NAME[] = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";

    /* h = HASH(protocol_name) - protocol name is > 32 bytes */
    noise_hash((const uint8_t *)PROTOCOL_NAME, strlen(PROTOCOL_NAME), ctx->h);

    /* ck = h */
    memcpy(ctx->ck, ctx->h, NOISE_HASH_SIZE);

    /* Initialize cipher state */
    memset(ctx->k, 0, NOISE_HASH_SIZE);
    ctx->n = 0;
    ctx->has_key = false;
}

esp_err_t esphome_noise_init(void)
{
#if USE_PSA_CRYPTO
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        /* PSA_ERROR_BAD_STATE (-137) = already initialized, OK
         * PSA_ERROR_ALREADY_EXISTS (-129) = also OK in some implementations */
        if (status != PSA_ERROR_BAD_STATE && status != PSA_ERROR_ALREADY_EXISTS) {
            ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "PSA Crypto already initialized (status=%d)", (int)status);
    }
    ESP_LOGI(TAG, "Noise Protocol encryption initialized (PSA Crypto API)");
#else
    ESP_LOGI(TAG, "Noise Protocol encryption initialized (mbedTLS legacy)");
#endif
    return ESP_OK;
}

esphome_noise_ctx_t *esphome_noise_create(const char *psk_base64,
                                           const char *device_name,
                                           const char *mac_address)
{
    esphome_noise_ctx_t *ctx = mem_ng_calloc(1, sizeof(esphome_noise_ctx_t), MEM_CAP_DEFAULT);
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate Noise context");
        return NULL;
    }

    ctx->state = ESPHOME_NOISE_STATE_INIT;

    /* Decode PSK if provided */
    if (psk_base64 && strlen(psk_base64) > 0) {
        esp_err_t ret = esphome_noise_decode_key(psk_base64, ctx->psk);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Invalid PSK Base64 encoding");
            mem_ng_free(ctx);
            return NULL;
        }
        ctx->has_psk = true;
        ESP_LOGI(TAG, "PSK configured for Noise encryption");
    } else {
        /* No PSK - use zero key */
        memset(ctx->psk, 0, NOISE_HASH_SIZE);
        ctx->has_psk = false;
        ESP_LOGW(TAG, "No PSK configured - using empty key");
    }

    /* Copy device info */
    if (device_name) {
        strncpy(ctx->device_name, device_name, ESPHOME_NOISE_MAX_DEVICE_NAME - 1);
    }
    if (mac_address) {
        strncpy(ctx->mac_address, mac_address, ESPHOME_NOISE_MAC_STR_LEN - 1);
    }

    return ctx;
}

void esphome_noise_destroy(esphome_noise_ctx_t *ctx)
{
    if (ctx) {
        /* Securely wipe sensitive data */
        mbedtls_platform_zeroize(ctx, sizeof(esphome_noise_ctx_t));
        mem_ng_free(ctx);
    }
}

esp_err_t esphome_noise_reset(esphome_noise_ctx_t *ctx)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Preserve PSK and device info, reset everything else */
    uint8_t psk_backup[NOISE_HASH_SIZE];
    bool has_psk = ctx->has_psk;
    char device_name[ESPHOME_NOISE_MAX_DEVICE_NAME];
    char mac_address[ESPHOME_NOISE_MAC_STR_LEN];

    memcpy(psk_backup, ctx->psk, NOISE_HASH_SIZE);
    strncpy(device_name, ctx->device_name, ESPHOME_NOISE_MAX_DEVICE_NAME - 1);
    device_name[ESPHOME_NOISE_MAX_DEVICE_NAME - 1] = '\0';
    strncpy(mac_address, ctx->mac_address, ESPHOME_NOISE_MAC_STR_LEN - 1);
    mac_address[ESPHOME_NOISE_MAC_STR_LEN - 1] = '\0';

    /* Clear context */
    mbedtls_platform_zeroize(ctx, sizeof(esphome_noise_ctx_t));

    /* Restore preserved data */
    memcpy(ctx->psk, psk_backup, NOISE_HASH_SIZE);
    ctx->has_psk = has_psk;
    strncpy(ctx->device_name, device_name, ESPHOME_NOISE_MAX_DEVICE_NAME - 1);
    ctx->device_name[ESPHOME_NOISE_MAX_DEVICE_NAME - 1] = '\0';
    strncpy(ctx->mac_address, mac_address, ESPHOME_NOISE_MAC_STR_LEN - 1);
    ctx->mac_address[ESPHOME_NOISE_MAC_STR_LEN - 1] = '\0';
    ctx->state = ESPHOME_NOISE_STATE_INIT;

    mbedtls_platform_zeroize(psk_backup, sizeof(psk_backup));

    return ESP_OK;
}

/* ============================================================================
 * Handshake Functions
 * ============================================================================ */

bool esphome_noise_is_hello(const uint8_t *data, size_t len)
{
    /* Noise Hello uses 0x00 indicator with varint length encoding */
    return len >= 1 && data[0] == ESPHOME_NOISE_HELLO_INDICATOR;
}

esp_err_t esphome_noise_process_hello(esphome_noise_ctx_t *ctx,
                                       const uint8_t *client_data, size_t client_len,
                                       uint8_t *response, size_t response_size,
                                       size_t *response_len)
{
    if (!ctx || !response || !response_len) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Client Hello payload may be empty or contain flags - no marker to verify */
    ESP_LOGI(TAG, "Received Noise Hello from client (%zu bytes payload)", client_len);

    /* Build SERVER_HELLO response:
     * - Byte 0: 0x01 = Noise encryption protocol selected
     * - Bytes 1+: device_name (null-terminated)
     * - Final: mac_address (null-terminated, 12 hex chars) */
    size_t name_len = strlen(ctx->device_name);
    size_t mac_len = strlen(ctx->mac_address);
    size_t total_len = 1 + name_len + 1 + mac_len + 1;

    if (total_len > response_size) {
        ESP_LOGE(TAG, "Response buffer too small for Hello");
        return ESP_ERR_NO_MEM;
    }

    size_t pos = 0;
    response[pos++] = ESPHOME_NOISE_PROTO_SELECTED;
    memcpy(response + pos, ctx->device_name, name_len);
    pos += name_len;
    response[pos++] = '\0';
    memcpy(response + pos, ctx->mac_address, mac_len);
    pos += mac_len;
    response[pos++] = '\0';

    *response_len = pos;
    ctx->state = ESPHOME_NOISE_STATE_HELLO_SENT;

    ESP_LOGI(TAG, "Sent Hello response: device='%s', mac='%s'",
             ctx->device_name, ctx->mac_address);

    return ESP_OK;
}

esp_err_t esphome_noise_process_handshake(esphome_noise_ctx_t *ctx,
                                           const uint8_t *handshake_data, size_t handshake_len,
                                           uint8_t *response, size_t response_size,
                                           size_t *response_len)
{
    if (!ctx || !handshake_data || !response || !response_len) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Processing handshake message (%zu bytes)", handshake_len);

    /*
     * NN Pattern (responder):
     *   <- e
     *   -> e, ee
     *
     * With psk0:
     *   <- psk, e
     *   -> e, ee
     */

    /* Initialize symmetric state */
    noise_init_symmetric_state(ctx);

    /* MixHash(prologue) */
    noise_mix_hash(ctx, (const uint8_t *)ESPHOME_NOISE_PROLOGUE, ESPHOME_NOISE_PROLOGUE_LEN);

    /* PSK0: MixKeyAndHash(psk) before processing 'e' */
    noise_mix_key_and_hash(ctx, ctx->psk, 32);

    /* Client handshake format:
     * [0x00 preamble][32B ephemeral key][encrypted payload + 16B MAC]
     * Preamble: 0x00 = handshake message, non-zero = error from client
     * Minimum: 1 (preamble) + 32 (ephemeral) + 16 (MAC) = 49 bytes */
    const size_t PREAMBLE_SIZE = 1;
    const size_t EPHEMERAL_SIZE = 32;
    const size_t MIN_HANDSHAKE_SIZE = PREAMBLE_SIZE + EPHEMERAL_SIZE + ESPHOME_NOISE_MAC_SIZE;

    if (handshake_len < MIN_HANDSHAKE_SIZE) {
        ESP_LOGE(TAG, "Handshake too short: need %zu bytes, got %zu",
                 MIN_HANDSHAKE_SIZE, handshake_len);
        goto error;
    }

    /* Check preamble: 0x00 = handshake message, non-zero = error from client */
    if (handshake_data[0] != ESPHOME_NOISE_HANDSHAKE_OK) {
        ESP_LOGE(TAG, "Invalid handshake preamble: 0x%02x (expected 0x%02x)",
                 handshake_data[0], ESPHOME_NOISE_HANDSHAKE_OK);
        goto error;
    }

    /* Read client ephemeral public key (re) - starts after preamble byte */
    const size_t key_offset = PREAMBLE_SIZE;
    const size_t encrypted_offset = PREAMBLE_SIZE + EPHEMERAL_SIZE;

    memcpy(ctx->re, handshake_data + key_offset, EPHEMERAL_SIZE);

    /* MixHash(re) */
    noise_mix_hash(ctx, ctx->re, EPHEMERAL_SIZE);

    /* MixKey(re) - required after MixHash(re) because PSK set has_key=true */
    noise_mix_key(ctx, ctx->re, EPHEMERAL_SIZE);

    /* Decrypt payload (encrypted empty payload + MAC) */
    if (handshake_len > encrypted_offset) {
        size_t encrypted_len = handshake_len - encrypted_offset;
        uint8_t *decrypted = mem_alloc(encrypted_len, MEM_CAP_DEFAULT);
        if (!decrypted) {
            ESP_LOGE(TAG, "Failed to allocate decrypt buffer");
            goto error;
        }

        size_t decrypted_len;
        esp_err_t ret = noise_decrypt_and_hash(ctx, handshake_data + encrypted_offset,
                                                encrypted_len, decrypted, &decrypted_len);
        mem_ng_free(decrypted);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to decrypt handshake payload");
            goto error;
        }
    }

    /* Generate server ephemeral keypair */
    x25519_keypair(ctx->e_public, ctx->e_private);

    /* === Build response === */
    /* Response format: [0x00 success][32B ephemeral][encrypted empty payload + 16B MAC] */

    if (response_size < 1 + 32 + 16) {
        ESP_LOGE(TAG, "Response buffer too small");
        goto error;
    }

    size_t pos = 0;

    /* Success marker */
    response[pos++] = ESPHOME_NOISE_HANDSHAKE_OK;

    /* Write server ephemeral (e) */
    memcpy(response + pos, ctx->e_public, 32);
    pos += 32;

    /* MixHash(e) */
    noise_mix_hash(ctx, ctx->e_public, 32);

    /* ESPHome-specific: MixKey(e) after MixHash(e) */
    noise_mix_key(ctx, ctx->e_public, 32);

    /* Compute DH: ee = DH(e, re) */
    uint8_t shared_secret[32];
    x25519_shared_secret(shared_secret, ctx->e_private, ctx->re);

    /* MixKey(ee) */
    noise_mix_key(ctx, shared_secret, 32);
    mbedtls_platform_zeroize(shared_secret, sizeof(shared_secret));

    /* Encrypt empty payload (just produces MAC) */
    esp_err_t ret = noise_encrypt_and_hash(ctx, NULL, 0, response + pos);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to encrypt handshake response");
        goto error;
    }
    pos += 16;  /* Just MAC, no plaintext */

    *response_len = pos;

    /* Split to derive transport keys */
    noise_split(ctx);

    ctx->state = ESPHOME_NOISE_STATE_READY;
    ESP_LOGI(TAG, "Noise handshake complete - encryption ready");

    return ESP_OK;

error:
    /* Send error response */
    if (response_size >= NOISE_HANDSHAKE_MIN_RESPONSE_SIZE) {
        response[0] = 0x01;  /* Error marker */
        const char *err_msg = "Handshake failed";
        size_t err_len = strlen(err_msg);
        memcpy(response + 1, err_msg, err_len);
        *response_len = 1 + err_len;
    } else {
        *response_len = 0;
    }
    ctx->state = ESPHOME_NOISE_STATE_ERROR;
    return ESP_FAIL;
}

/* ============================================================================
 * Transport Functions
 * ============================================================================ */

esp_err_t esphome_noise_encrypt(esphome_noise_ctx_t *ctx,
                                 uint16_t msg_type,
                                 const uint8_t *payload, size_t payload_len,
                                 uint8_t *output, size_t output_size,
                                 size_t *output_len)
{
    if (!ctx || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->state != ESPHOME_NOISE_STATE_READY) {
        ESP_LOGE(TAG, "Cannot encrypt: not in READY state (state=%d)", ctx->state);
        return ESP_ERR_INVALID_STATE;
    }

    /* Inner frame: [2B msg_type BE][2B payload_len BE][payload] */
    size_t inner_len = 4 + payload_len;
    if (inner_len > ESPHOME_NOISE_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "Payload too large: %zu > %d", inner_len, ESPHOME_NOISE_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Output: [0x01 indicator][2B encrypted_len BE][encrypted inner + 16B MAC] */
    size_t encrypted_len = inner_len + ESPHOME_NOISE_MAC_SIZE;
    size_t total_output = ESPHOME_NOISE_FRAME_HEADER_SIZE + encrypted_len;

    if (total_output > output_size) {
        ESP_LOGE(TAG, "Output buffer too small: need %zu, have %zu", total_output, output_size);
        return ESP_ERR_NO_MEM;
    }

    /* Build inner frame */
    uint8_t *inner = mem_alloc(inner_len, MEM_CAP_DEFAULT);
    if (!inner) {
        return ESP_ERR_NO_MEM;
    }

    inner[0] = (msg_type >> 8) & 0xFF;   /* Big-endian msg_type */
    inner[1] = msg_type & 0xFF;
    inner[2] = (payload_len >> 8) & 0xFF;  /* Big-endian payload_len */
    inner[3] = payload_len & 0xFF;
    if (payload && payload_len > 0) {
        memcpy(inner + 4, payload, payload_len);
    }

    /* Write Noise frame header: [0x01 indicator][2B length BE] */
    output[0] = ESPHOME_FRAME_NOISE;
    output[1] = (encrypted_len >> 8) & 0xFF;
    output[2] = encrypted_len & 0xFF;

    /* Encrypt inner frame */
    uint8_t tag[ESPHOME_NOISE_MAC_SIZE];
    esp_err_t ret = noise_encrypt_aead(ctx->encrypt_key, ctx->encrypt_nonce,
                                        NULL, 0,  /* No AAD for transport */
                                        inner, inner_len,
                                        output + ESPHOME_NOISE_FRAME_HEADER_SIZE, tag);
    mem_ng_free(inner);

    if (ret != ESP_OK) {
        return ret;
    }

    /* Append MAC */
    memcpy(output + ESPHOME_NOISE_FRAME_HEADER_SIZE + inner_len, tag,
           ESPHOME_NOISE_MAC_SIZE);

    ctx->encrypt_nonce++;
    *output_len = total_output;

    return ESP_OK;
}

esp_err_t esphome_noise_decrypt(esphome_noise_ctx_t *ctx,
                                 const uint8_t *ciphertext, size_t ciphertext_len,
                                 uint16_t *msg_type,
                                 uint8_t *payload, size_t payload_size,
                                 size_t *payload_len)
{
    if (!ctx || !ciphertext || !msg_type || !payload || !payload_len) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->state != ESPHOME_NOISE_STATE_READY) {
        ESP_LOGE(TAG, "Cannot decrypt: not in READY state (state=%d)", ctx->state);
        return ESP_ERR_INVALID_STATE;
    }

    /* Ciphertext format: encrypted([2B msg_type BE][2B payload_len BE][payload]) + 16B MAC */
    if (ciphertext_len < ESPHOME_NOISE_MAC_SIZE + 4) {
        ESP_LOGE(TAG, "Ciphertext too short: %zu", ciphertext_len);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t data_len = ciphertext_len - ESPHOME_NOISE_MAC_SIZE;
    const uint8_t *tag = ciphertext + data_len;

    /* Allocate buffer for decrypted inner frame */
    uint8_t *inner = mem_alloc(data_len, MEM_CAP_DEFAULT);
    if (!inner) {
        return ESP_ERR_NO_MEM;
    }

    /* Decrypt */
    esp_err_t ret = noise_decrypt_aead(ctx->decrypt_key, ctx->decrypt_nonce,
                                        NULL, 0,  /* No AAD for transport */
                                        ciphertext, data_len,
                                        tag, inner);
    if (ret != ESP_OK) {
        mem_ng_free(inner);
        ESP_LOGE(TAG, "Decryption failed - MAC mismatch");
        return ret;
    }

    ctx->decrypt_nonce++;

    /* Parse inner frame */
    if (data_len < 4) {
        mem_ng_free(inner);
        ESP_LOGE(TAG, "Decrypted data too short for header");
        return ESP_ERR_INVALID_SIZE;
    }

    *msg_type = (inner[0] << 8) | inner[1];
    uint16_t inner_payload_len = (inner[2] << 8) | inner[3];

    if (inner_payload_len + 4 > data_len) {
        mem_ng_free(inner);
        ESP_LOGE(TAG, "Invalid payload length in decrypted frame");
        return ESP_ERR_INVALID_SIZE;
    }

    if (inner_payload_len > payload_size) {
        mem_ng_free(inner);
        ESP_LOGE(TAG, "Payload buffer too small: need %u, have %zu",
                 inner_payload_len, payload_size);
        return ESP_ERR_NO_MEM;
    }

    if (inner_payload_len > 0) {
        memcpy(payload, inner + 4, inner_payload_len);
    }
    *payload_len = inner_payload_len;

    mem_ng_free(inner);
    return ESP_OK;
}

/* ============================================================================
 * State Query Functions
 * ============================================================================ */

esphome_noise_state_t esphome_noise_get_state(const esphome_noise_ctx_t *ctx)
{
    return ctx ? ctx->state : ESPHOME_NOISE_STATE_ERROR;
}

bool esphome_noise_is_ready(const esphome_noise_ctx_t *ctx)
{
    return ctx && ctx->state == ESPHOME_NOISE_STATE_READY;
}

const char *esphome_noise_state_name(esphome_noise_state_t state)
{
    switch (state) {
        case ESPHOME_NOISE_STATE_INIT:           return "INIT";
        case ESPHOME_NOISE_STATE_HELLO_WAIT:     return "HELLO_WAIT";
        case ESPHOME_NOISE_STATE_HELLO_SENT:     return "HELLO_SENT";
        case ESPHOME_NOISE_STATE_HANDSHAKE_WAIT: return "HANDSHAKE_WAIT";
        case ESPHOME_NOISE_STATE_HANDSHAKE_DONE: return "HANDSHAKE_DONE";
        case ESPHOME_NOISE_STATE_READY:          return "READY";
        case ESPHOME_NOISE_STATE_ERROR:          return "ERROR";
        default:                                  return "UNKNOWN";
    }
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

esp_err_t esphome_noise_decode_key(const char *base64_key, uint8_t *key_out)
{
    if (!base64_key || !key_out) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t olen;
    int ret = mbedtls_base64_decode(key_out, 32, &olen,
                                     (const unsigned char *)base64_key,
                                     strlen(base64_key));
    if (ret != 0 || olen != 32) {
        ESP_LOGE(TAG, "Invalid Base64 key: ret=%d, len=%zu", ret, olen);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t esphome_noise_generate_key(char *base64_out, size_t base64_size)
{
    /* 32 bytes -> 44 base64 chars + null */
    if (!base64_out || base64_size < NOISE_BASE64_ENCODED_MIN_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Generate 32 random bytes */
    uint8_t key[NOISE_DH_OUTPUT_SIZE];
    esp_fill_random(key, NOISE_DH_OUTPUT_SIZE);

    /* Encode to Base64 */
    size_t olen;
    int ret = mbedtls_base64_encode((unsigned char *)base64_out, base64_size, &olen,
                                     key, NOISE_DH_OUTPUT_SIZE);

    mbedtls_platform_zeroize(key, sizeof(key));

    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 encode failed: %d", ret);
        return ESP_FAIL;
    }

    base64_out[olen] = '\0';
    return ESP_OK;
}
