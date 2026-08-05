/**
 * @file esphome_noise.h
 * @brief ESPHome Noise Protocol Encryption
 *
 * Implements Noise Protocol encryption for ESPHome Native API.
 * Protocol: Noise_NNpsk0_25519_ChaChaPoly_SHA256
 *
 * ESPHome-specific modifications from standard Noise:
 * - Pre-handshake Hello phase (0x01 negotiation)
 * - Fixed prologue: "NoiseAPIInit\x00\x00"
 * - Handshake frame with 0x00 preamble byte
 * - Big-endian length fields in encrypted frames
 * - PSK derived from Base64-encoded encryption key
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_NOISE_H
#define ESPHOME_NOISE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Noise Protocol prologue for ESPHome */
#define ESPHOME_NOISE_PROLOGUE          "NoiseAPIInit\x00\x00"

/** @brief Noise prologue length */
#define ESPHOME_NOISE_PROLOGUE_LEN      14

/** @brief X25519 key size in bytes */
#define ESPHOME_NOISE_KEY_SIZE          32

/** @brief ChaCha20-Poly1305 nonce size */
#define ESPHOME_NOISE_NONCE_SIZE        12

/** @brief ChaCha20-Poly1305 MAC/tag size */
#define ESPHOME_NOISE_MAC_SIZE          16

/** @brief Buffer size for a Base64 PSK: 44 characters plus terminator. */
#define ESPHOME_NOISE_PSK_BASE64_LEN    48

/** @brief SHA256 hash size */
#define ESPHOME_NOISE_HASH_SIZE         32

/** @brief Maximum encrypted frame payload (limited by 16-bit length field) */
#define ESPHOME_NOISE_MAX_PAYLOAD       65535

/** @brief Maximum handshake message size */
#define ESPHOME_NOISE_MAX_HANDSHAKE     128

/** @brief Server Hello protocol selection: Noise encryption chosen */
#define ESPHOME_NOISE_PROTO_SELECTED    0x01

/** @brief Noise Hello indicator byte (same as all Noise frames) */
#define ESPHOME_NOISE_HELLO_INDICATOR   0x01

/** @brief Handshake success marker */
#define ESPHOME_NOISE_HANDSHAKE_OK      0x00

/** @brief Maximum device name length in Hello */
#define ESPHOME_NOISE_MAX_DEVICE_NAME   64

/** @brief MAC address string length (12 hex chars + null) */
#define ESPHOME_NOISE_MAC_STR_LEN       13

/* ============================================================================
 * Handshake States
 * ============================================================================ */

/**
 * @brief Noise handshake state machine
 */
typedef enum {
    ESPHOME_NOISE_STATE_INIT = 0,           /**< Not started */
    ESPHOME_NOISE_STATE_HELLO_WAIT,         /**< Waiting for Hello from client */
    ESPHOME_NOISE_STATE_HELLO_SENT,         /**< Hello response sent, waiting for handshake */
    ESPHOME_NOISE_STATE_HANDSHAKE_WAIT,     /**< Waiting for handshake message 1 */
    ESPHOME_NOISE_STATE_HANDSHAKE_DONE,     /**< Handshake message 2 sent */
    ESPHOME_NOISE_STATE_READY,              /**< Transport ready for encrypted data */
    ESPHOME_NOISE_STATE_ERROR,              /**< Handshake failed */
} esphome_noise_state_t;

/* ============================================================================
 * Noise Context Structure
 * ============================================================================ */

/**
 * @brief Noise Protocol context for a client connection
 *
 * Holds all state needed for the Noise handshake and encrypted transport.
 */
typedef struct esphome_noise_ctx {
    /* State machine */
    esphome_noise_state_t state;            /**< Current handshake state */

    /* Pre-shared key (PSK) */
    uint8_t psk[ESPHOME_NOISE_KEY_SIZE];    /**< Pre-shared key (from Base64 config) */
    /**
     * @brief True if a PSK was supplied to esphome_noise_create()
     *
     * Informational only — nothing in this module reads it, so it is NOT the
     * interlock it looks like. An empty PSK is refused earlier, by the
     * CONFIG_ESPHOME_NOISE_REQUIRE_PSK check in esphome_api_server.c, before a
     * context is ever created. Do not add a code path that relies on this flag
     * to decide whether encryption is trustworthy.
     */
    bool has_psk;

    /* Symmetric state (during handshake) */
    uint8_t ck[ESPHOME_NOISE_HASH_SIZE];    /**< Chaining key */
    uint8_t h[ESPHOME_NOISE_HASH_SIZE];     /**< Handshake hash */
    uint8_t k[ESPHOME_NOISE_KEY_SIZE];      /**< Current cipher key (handshake) */
    bool has_key;                           /**< True if cipher key is initialized */
    uint64_t n;                             /**< Nonce counter (handshake) */

    /* Ephemeral keys (server) */
    uint8_t e_private[ESPHOME_NOISE_KEY_SIZE];  /**< Server ephemeral private key */
    uint8_t e_public[ESPHOME_NOISE_KEY_SIZE];   /**< Server ephemeral public key */

    /* Remote ephemeral (client) */
    uint8_t re[ESPHOME_NOISE_KEY_SIZE];     /**< Client ephemeral public key */

    /* Transport cipher states (after handshake) */
    uint8_t encrypt_key[ESPHOME_NOISE_KEY_SIZE];  /**< Key for encrypting (server->client) */
    uint8_t decrypt_key[ESPHOME_NOISE_KEY_SIZE];  /**< Key for decrypting (client->server) */
    uint64_t encrypt_nonce;                 /**< Encryption nonce counter */
    uint64_t decrypt_nonce;                 /**< Decryption nonce counter */

    /* Device info (for Hello response) */
    char device_name[ESPHOME_NOISE_MAX_DEVICE_NAME];  /**< Device name */
    char mac_address[ESPHOME_NOISE_MAC_STR_LEN];      /**< MAC address (hex, uppercase) */
} esphome_noise_ctx_t;

/* ============================================================================
 * Initialization Functions
 * ============================================================================ */

/**
 * @brief Initialize Noise encryption module
 *
 * Must be called once at startup before using Noise encryption.
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_noise_init(void);

/**
 * @brief Create a new Noise context for a client connection
 *
 * Allocates and initializes a Noise context for handling encrypted
 * communication with a client.
 *
 * @param[in] psk_base64 Base64-encoded 32-byte pre-shared key (NULL for no PSK)
 * @param[in] device_name Device name for Hello response
 * @param[in] mac_address MAC address string (12 hex chars, uppercase)
 * @return Pointer to new context, or NULL on error
 */
esphome_noise_ctx_t *esphome_noise_create(const char *psk_base64,
                                           const char *device_name,
                                           const char *mac_address);

/**
 * @brief Destroy a Noise context
 *
 * Securely wipes and frees the context.
 *
 * @param[in] ctx Context to destroy
 */
void esphome_noise_destroy(esphome_noise_ctx_t *ctx);

/**
 * @brief Reset context for new connection (reuse memory)
 *
 * @param[in] ctx Context to reset
 * @return ESP_OK on success
 */
esp_err_t esphome_noise_reset(esphome_noise_ctx_t *ctx);

/* ============================================================================
 * Handshake Functions
 * ============================================================================ */

/**
 * @brief Check if incoming data is a Noise Hello request
 *
 * Checks if the first byte is the Noise Hello marker (0x01).
 *
 * @param[in] data Received data
 * @param[in] len Data length
 * @return true if this is a Noise Hello request
 */
bool esphome_noise_is_hello(const uint8_t *data, size_t len);

/**
 * @brief Process client Hello and generate server Hello response
 *
 * Called when client sends Hello (0x01 byte).
 * Generates server Hello response: 0x01 + device_name\0 + mac\0
 *
 * @param[in] ctx Noise context
 * @param[in] client_data Client Hello data (should be 0x01)
 * @param[in] client_len Client data length
 * @param[out] response Buffer for Hello response
 * @param[in] response_size Response buffer size
 * @param[out] response_len Actual response length
 * @return ESP_OK on success
 */
esp_err_t esphome_noise_process_hello(esphome_noise_ctx_t *ctx,
                                       const uint8_t *client_data, size_t client_len,
                                       uint8_t *response, size_t response_size,
                                       size_t *response_len);

/**
 * @brief Process client handshake message and generate response
 *
 * Processes the client's handshake message containing:
 * - Client ephemeral public key
 * - Encrypted payload (if any)
 *
 * Generates server handshake response containing:
 * - 0x00 success byte (or 0x01 + error message on failure)
 * - Server ephemeral public key
 * - Encrypted payload with MAC
 *
 * After successful completion, transport is ready for encrypted messages.
 *
 * @param[in] ctx Noise context
 * @param[in] handshake_data Client handshake data
 * @param[in] handshake_len Client data length
 * @param[out] response Buffer for handshake response
 * @param[in] response_size Response buffer size
 * @param[out] response_len Actual response length
 * @return ESP_OK on success, error code on failure
 */
esp_err_t esphome_noise_process_handshake(esphome_noise_ctx_t *ctx,
                                           const uint8_t *handshake_data, size_t handshake_len,
                                           uint8_t *response, size_t response_size,
                                           size_t *response_len);

/* ============================================================================
 * Transport Functions (after handshake)
 * ============================================================================ */

/**
 * @brief Encrypt a message for transport
 *
 * Encrypts an ESPHome message for sending to the client.
 * Output format: [2B length BE][encrypted data + 16B MAC]
 * Encrypted data contains: [2B msg_type BE][2B payload_len BE][payload]
 *
 * @param[in] ctx Noise context (must be in READY state)
 * @param[in] msg_type ESPHome message type
 * @param[in] payload Message payload (protobuf-encoded)
 * @param[in] payload_len Payload length
 * @param[out] output Buffer for encrypted message
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not in READY state
 * @return ESP_ERR_NO_MEM if output buffer too small
 */
esp_err_t esphome_noise_encrypt(esphome_noise_ctx_t *ctx,
                                 uint16_t msg_type,
                                 const uint8_t *payload, size_t payload_len,
                                 uint8_t *output, size_t output_size,
                                 size_t *output_len);

/**
 * @brief Decrypt a received message
 *
 * Decrypts an encrypted message from the client.
 * Input format: encrypted data + 16B MAC (length prefix already removed)
 * Decrypted data contains: [2B msg_type BE][2B payload_len BE][payload]
 *
 * @param[in] ctx Noise context (must be in READY state)
 * @param[in] ciphertext Encrypted data (including MAC)
 * @param[in] ciphertext_len Ciphertext length
 * @param[out] msg_type Decoded message type
 * @param[out] payload Buffer for decrypted payload
 * @param[in] payload_size Payload buffer size
 * @param[out] payload_len Actual payload length
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not in READY state
 * @return ESP_ERR_INVALID_CRC if MAC verification fails
 */
esp_err_t esphome_noise_decrypt(esphome_noise_ctx_t *ctx,
                                 const uint8_t *ciphertext, size_t ciphertext_len,
                                 uint16_t *msg_type,
                                 uint8_t *payload, size_t payload_size,
                                 size_t *payload_len);

/* ============================================================================
 * State Query Functions
 * ============================================================================ */

/**
 * @brief Get current handshake state
 *
 * @param[in] ctx Noise context
 * @return Current state
 */
esphome_noise_state_t esphome_noise_get_state(const esphome_noise_ctx_t *ctx);

/**
 * @brief Check if encryption is ready for transport
 *
 * @param[in] ctx Noise context
 * @return true if ready for encrypted communication
 */
bool esphome_noise_is_ready(const esphome_noise_ctx_t *ctx);

/**
 * @brief Get state name for logging
 *
 * @param[in] state Noise state
 * @return String name of state
 */
const char *esphome_noise_state_name(esphome_noise_state_t state);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * @brief Decode Base64 encryption key
 *
 * Decodes a Base64-encoded 32-byte key.
 *
 * @param[in] base64_key Base64-encoded key string
 * @param[out] key_out 32-byte output buffer
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if key is invalid
 */
esp_err_t esphome_noise_decode_key(const char *base64_key, uint8_t *key_out);

/**
 * @brief Generate a random Base64 encryption key
 *
 * Generates a cryptographically secure 32-byte key and encodes it as Base64.
 *
 * @param[out] base64_out Buffer for Base64 string (at least 45 bytes)
 * @param[in] base64_size Buffer size
 * @return ESP_OK on success
 */
esp_err_t esphome_noise_generate_key(char *base64_out, size_t base64_size);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_NOISE_H */
