/**
 * @file esphome_protocol.h
 * @brief ESPHome Native API Protocol Handler
 *
 * Implements Protobuf-compatible wire format encoding and decoding
 * for ESPHome Native API messages.
 *
 * Wire Format:
 * - Byte 0: 0x00 (null byte, message start marker)
 * - Bytes 1-N: Message length as varint
 * - Bytes N+1-M: Message type as varint
 * - Bytes M+1-end: Message payload (protobuf-encoded)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_PROTOCOL_H
#define ESPHOME_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esphome_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Protobuf Wire Types
 * ============================================================================ */

/**
 * @brief Protobuf wire types
 */
typedef enum {
    PROTOBUF_WIRE_VARINT     = 0,  /**< Varint (int32, int64, uint32, uint64, bool, enum) */
    PROTOBUF_WIRE_64BIT      = 1,  /**< 64-bit (fixed64, sfixed64, double) */
    PROTOBUF_WIRE_LEN        = 2,  /**< Length-delimited (string, bytes, embedded messages) */
    PROTOBUF_WIRE_32BIT      = 5,  /**< 32-bit (fixed32, sfixed32, float) */
} protobuf_wire_type_t;

/* ============================================================================
 * Message Buffer Structure
 * ============================================================================ */

/**
 * @brief Protocol buffer for encoding/decoding messages
 */
typedef struct {
    uint8_t *data;      /**< Buffer data pointer */
    size_t size;        /**< Total buffer size */
    size_t position;    /**< Current read/write position */
    bool overflow;      /**< Overflow flag */
} esphome_buffer_t;

/* ============================================================================
 * Decoded Message Structure
 * ============================================================================ */

/**
 * @brief Decoded message header
 */
typedef struct {
    uint32_t length;            /**< Message payload length */
    esphome_msg_type_t type;    /**< Message type */
    uint8_t *payload;           /**< Pointer to payload data */
    size_t payload_len;         /**< Payload length */
} esphome_message_t;

/* ============================================================================
 * Buffer Management Functions
 * ============================================================================ */

/**
 * @brief Initialize a protocol buffer for writing
 *
 * @param[out] buf Buffer structure to initialize
 * @param[in] data Data buffer pointer
 * @param[in] size Size of data buffer
 */
void esphome_buffer_init(esphome_buffer_t *buf, uint8_t *data, size_t size);

/**
 * @brief Reset buffer position to beginning
 *
 * @param[in,out] buf Buffer to reset
 */
void esphome_buffer_reset(esphome_buffer_t *buf);

/**
 * @brief Get remaining bytes available in buffer
 *
 * @param[in] buf Buffer to check
 * @return Number of bytes remaining
 */
size_t esphome_buffer_remaining(const esphome_buffer_t *buf);

/**
 * @brief Check if buffer has overflowed
 *
 * @param[in] buf Buffer to check
 * @return true if overflow occurred
 */
bool esphome_buffer_overflow(const esphome_buffer_t *buf);

/* ============================================================================
 * Varint Encoding/Decoding
 * ============================================================================ */

/**
 * @brief Encode a varint into buffer
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] value Value to encode
 * @return ESP_OK on success, ESP_ERR_NO_MEM if buffer full
 */
esp_err_t esphome_encode_varint(esphome_buffer_t *buf, uint64_t value);

/**
 * @brief Decode a varint from buffer
 *
 * @param[in,out] buf Buffer to read from
 * @param[out] value Decoded value
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE if insufficient data
 */
esp_err_t esphome_decode_varint(esphome_buffer_t *buf, uint64_t *value);

/**
 * @brief Get encoded varint size
 *
 * @param[in] value Value to check
 * @return Number of bytes needed to encode value
 */
size_t esphome_varint_size(uint64_t value);

/* ============================================================================
 * Protobuf Field Encoding
 * ============================================================================ */

/**
 * @brief Encode a protobuf field tag
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] wire_type Wire type
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_tag(esphome_buffer_t *buf, uint32_t field_number,
                             protobuf_wire_type_t wire_type);

/**
 * @brief Decode a protobuf field tag
 *
 * @param[in,out] buf Buffer to read from
 * @param[out] field_number Decoded field number
 * @param[out] wire_type Decoded wire type
 * @return ESP_OK on success
 */
esp_err_t esphome_decode_tag(esphome_buffer_t *buf, uint32_t *field_number,
                             protobuf_wire_type_t *wire_type);

/**
 * @brief Encode a boolean field
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] value Boolean value
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_bool(esphome_buffer_t *buf, uint32_t field_number, bool value);

/**
 * @brief Encode a uint32 field
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] value Uint32 value
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_uint32(esphome_buffer_t *buf, uint32_t field_number, uint32_t value);

/**
 * @brief Encode a fixed32 field (wire type 5, 4-byte little-endian)
 *
 * Used for protobuf `fixed32` fields such as entity keys.
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] value Fixed32 value
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_fixed32(esphome_buffer_t *buf, uint32_t field_number, uint32_t value);

/**
 * @brief Encode a int32 field
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] value Int32 value
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_int32(esphome_buffer_t *buf, uint32_t field_number, int32_t value);

/**
 * @brief Encode a float field (fixed32)
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] value Float value
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_float(esphome_buffer_t *buf, uint32_t field_number, float value);

/**
 * @brief Encode a string field
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] value String value (null-terminated)
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_string(esphome_buffer_t *buf, uint32_t field_number, const char *value);

/**
 * @brief Encode raw bytes field
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] data Bytes to encode
 * @param[in] len Length of bytes
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_bytes(esphome_buffer_t *buf, uint32_t field_number,
                               const uint8_t *data, size_t len);

/**
 * @brief Encode a fixed64 field (8 bytes, little-endian)
 *
 * Used for BLE addresses and other 64-bit fixed values.
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] value 64-bit value
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_fixed64(esphome_buffer_t *buf, uint32_t field_number, uint64_t value);

/**
 * @brief Encode a sint32 field (ZigZag encoding)
 *
 * Used for signed values like RSSI.
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] value Signed 32-bit value
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_sint32(esphome_buffer_t *buf, uint32_t field_number, int32_t value);

/**
 * @brief Encode a uint64 field (varint)
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number
 * @param[in] value 64-bit value
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_uint64(esphome_buffer_t *buf, uint32_t field_number, uint64_t value);

/* ============================================================================
 * Protobuf Field Decoding
 * ============================================================================ */

/**
 * @brief Decode a boolean from current buffer position
 *
 * @param[in,out] buf Buffer to read from
 * @param[out] value Decoded boolean
 * @return ESP_OK on success
 */
esp_err_t esphome_decode_bool(esphome_buffer_t *buf, bool *value);

/**
 * @brief Decode a uint32 from current buffer position
 *
 * @param[in,out] buf Buffer to read from
 * @param[out] value Decoded uint32
 * @return ESP_OK on success
 */
esp_err_t esphome_decode_uint32(esphome_buffer_t *buf, uint32_t *value);

/**
 * @brief Decode a fixed32 from current buffer position (4-byte little-endian)
 *
 * Used for protobuf `fixed32` fields such as entity keys.
 *
 * @param[in,out] buf Buffer to read from
 * @param[out] value Decoded fixed32
 * @return ESP_OK on success
 */
esp_err_t esphome_decode_fixed32(esphome_buffer_t *buf, uint32_t *value);

/**
 * @brief Decode a float from current buffer position (fixed32)
 *
 * @param[in,out] buf Buffer to read from
 * @param[out] value Decoded float
 * @return ESP_OK on success
 */
esp_err_t esphome_decode_float(esphome_buffer_t *buf, float *value);

/**
 * @brief Decode a string from current buffer position
 *
 * @param[in,out] buf Buffer to read from
 * @param[out] value Output string buffer
 * @param[in] max_len Maximum string length
 * @return ESP_OK on success
 */
esp_err_t esphome_decode_string(esphome_buffer_t *buf, char *value, size_t max_len);

/**
 * @brief Decode raw bytes from current buffer position
 *
 * @param[in,out] buf Buffer to read from
 * @param[out] data Output buffer for bytes
 * @param[in] max_len Maximum bytes to read
 * @param[out] actual_len Actual bytes read
 * @return ESP_OK on success
 */
esp_err_t esphome_decode_bytes(esphome_buffer_t *buf, uint8_t *data, size_t max_len,
                                size_t *actual_len);

/**
 * @brief Decode a fixed64 from current buffer position
 *
 * @param[in,out] buf Buffer to read from
 * @param[out] value Decoded 64-bit value
 * @return ESP_OK on success
 */
esp_err_t esphome_decode_fixed64(esphome_buffer_t *buf, uint64_t *value);

/**
 * @brief Skip a field value in buffer
 *
 * @param[in,out] buf Buffer to read from
 * @param[in] wire_type Wire type of field to skip
 * @return ESP_OK on success
 */
esp_err_t esphome_skip_field(esphome_buffer_t *buf, protobuf_wire_type_t wire_type);

/* ============================================================================
 * Message Framing Functions
 * ============================================================================ */

/**
 * @brief Encode message frame header
 *
 * Writes the ESPHome message frame: 0x00 + length(varint) + type(varint)
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] msg_type Message type
 * @param[in] payload_len Payload length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_frame_header(esphome_buffer_t *buf, esphome_msg_type_t msg_type,
                                       size_t payload_len);

/**
 * @brief Decode message frame from buffer
 *
 * Parses the ESPHome message frame header and extracts message info.
 *
 * @param[in] data Raw data buffer
 * @param[in] data_len Length of data
 * @param[out] msg Decoded message structure
 * @param[out] bytes_consumed Number of bytes consumed from buffer
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_SIZE if incomplete message
 * @return ESPHOME_ERR_INVALID_MESSAGE if invalid format
 */
esp_err_t esphome_decode_frame(const uint8_t *data, size_t data_len,
                               esphome_message_t *msg, size_t *bytes_consumed);

/* ============================================================================
 * Message Building Helpers
 * ============================================================================ */

/**
 * @brief Build a complete message with frame
 *
 * Builds a complete ESPHome message including frame header.
 * Call this after encoding all payload fields.
 *
 * @param[in] msg_type Message type
 * @param[in] payload Encoded payload data
 * @param[in] payload_len Payload length
 * @param[out] output Output buffer for complete message
 * @param[in] output_size Size of output buffer
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_build_message(esphome_msg_type_t msg_type, const uint8_t *payload,
                                 size_t payload_len, uint8_t *output, size_t output_size,
                                 size_t *output_len);

/**
 * @brief Build an empty message (no payload)
 *
 * @param[in] msg_type Message type
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_build_empty_message(esphome_msg_type_t msg_type, uint8_t *output,
                                       size_t output_size, size_t *output_len);

/* ============================================================================
 * Protocol Utilities
 * ============================================================================ */

/**
 * @brief Get message type name for logging
 *
 * @param[in] type Message type
 * @return String name of message type
 */
const char *esphome_msg_type_name(esphome_msg_type_t type);

/**
 * @brief Initialize protocol module
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_protocol_init(void);

/**
 * @brief Test protocol encoding/decoding
 *
 * @return ESP_OK if tests pass
 */
esp_err_t esphome_protocol_test(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_PROTOCOL_H */
