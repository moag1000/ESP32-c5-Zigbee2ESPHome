/**
 * @file esphome_protocol.c
 * @brief ESPHome Native API Protocol Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_protocol.h"
#include <string.h>
#include "esp_log.h"

/* Log tag */
static const char *TAG = "ESPHOME_PROTO";

/* ============================================================================
 * Buffer Management Functions
 * ============================================================================ */

/**
 * @brief Initialize a protocol buffer for writing
 *
 * Sets up a buffer structure for protobuf encoding operations.
 * The buffer tracks position and overflow state during encoding.
 *
 * @param[out] buf      Pointer to buffer structure to initialize
 * @param[in]  data     Pointer to underlying byte array for storage
 * @param[in]  size     Size of the byte array in bytes
 *
 * @note If buf is NULL, this function returns without action
 */
void esphome_buffer_init(esphome_buffer_t *buf, uint8_t *data, size_t size)
{
    if (!buf) {
        return;
    }
    buf->data = data;
    buf->size = size;
    buf->position = 0;
    buf->overflow = false;
}

/**
 * @brief Reset buffer position to beginning
 *
 * Resets the buffer's position pointer to the start and clears
 * the overflow flag, allowing the buffer to be reused.
 *
 * @param[in,out] buf  Pointer to buffer structure to reset
 *
 * @note If buf is NULL, this function returns without action
 * @note The underlying data array is not cleared
 */
void esphome_buffer_reset(esphome_buffer_t *buf)
{
    if (!buf) {
        return;
    }
    buf->position = 0;
    buf->overflow = false;
}

/**
 * @brief Get remaining bytes available in buffer
 *
 * Calculates how many bytes can still be written to the buffer
 * before it would overflow.
 *
 * @param[in] buf  Pointer to buffer structure to query
 *
 * @return Number of bytes remaining (0 if buf is NULL or exhausted)
 */
size_t esphome_buffer_remaining(const esphome_buffer_t *buf)
{
    if (!buf || buf->position >= buf->size) {
        return 0;
    }
    return buf->size - buf->position;
}

/**
 * @brief Check if buffer has overflowed
 *
 * Returns whether a write operation has attempted to exceed
 * the buffer's capacity. Once set, the overflow flag persists
 * until the buffer is reset.
 *
 * @param[in] buf  Pointer to buffer structure to check
 *
 * @return true if buffer has overflowed or buf is NULL
 * @return false if no overflow has occurred
 */
bool esphome_buffer_overflow(const esphome_buffer_t *buf)
{
    return buf ? buf->overflow : true;
}

/* ============================================================================
 * Internal Buffer Write/Read Helpers
 * ============================================================================ */

/**
 * @brief Write a single byte to buffer
 */
static esp_err_t buffer_write_byte(esphome_buffer_t *buf, uint8_t byte)
{
    if (!buf || !buf->data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buf->position >= buf->size) {
        buf->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    buf->data[buf->position++] = byte;
    return ESP_OK;
}

/**
 * @brief Write multiple bytes to buffer
 */
static esp_err_t buffer_write_bytes(esphome_buffer_t *buf, const uint8_t *data, size_t len)
{
    if (!buf || !buf->data || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buf->position + len > buf->size) {
        buf->overflow = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(&buf->data[buf->position], data, len);
    buf->position += len;
    return ESP_OK;
}

/**
 * @brief Read a single byte from buffer
 */
static esp_err_t buffer_read_byte(esphome_buffer_t *buf, uint8_t *byte)
{
    if (!buf || !buf->data || !byte) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buf->position >= buf->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    *byte = buf->data[buf->position++];
    return ESP_OK;
}

/**
 * @brief Read multiple bytes from buffer
 */
static esp_err_t buffer_read_bytes(esphome_buffer_t *buf, uint8_t *data, size_t len)
{
    if (!buf || !buf->data || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buf->position + len > buf->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, &buf->data[buf->position], len);
    buf->position += len;
    return ESP_OK;
}

/* ============================================================================
 * Varint Encoding/Decoding
 * ============================================================================ */

/**
 * @brief Encode a varint into buffer
 *
 * Encodes an unsigned 64-bit integer using protobuf variable-length
 * encoding. Each byte uses 7 bits for data and 1 bit to indicate
 * if more bytes follow.
 *
 * @param[in,out] buf    Pointer to buffer for writing
 * @param[in]     value  Unsigned integer value to encode
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if buf is NULL
 * @return ESP_ERR_NO_MEM if buffer overflows
 */
esp_err_t esphome_encode_varint(esphome_buffer_t *buf, uint64_t value)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }

    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;  /* More bytes to follow */
        }
        esp_err_t ret = buffer_write_byte(buf, byte);
        if (ret != ESP_OK) {
            return ret;
        }
    } while (value != 0);

    return ESP_OK;
}

/**
 * @brief Decode a varint from buffer
 *
 * Decodes a protobuf variable-length encoded integer from the buffer.
 * Advances the buffer position past the decoded value.
 *
 * @param[in,out] buf    Pointer to buffer to read from
 * @param[out]    value  Pointer to store decoded value
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if buf or value is NULL
 * @return ESP_ERR_INVALID_SIZE if buffer has insufficient data
 * @return ESPHOME_ERR_INVALID_MESSAGE if varint exceeds 64 bits
 */
esp_err_t esphome_decode_varint(esphome_buffer_t *buf, uint64_t *value)
{
    if (!buf || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    *value = 0;
    uint8_t byte;
    int shift = 0;

    do {
        if (shift >= 64) {
            ESP_LOGE(TAG, "Varint too long");
            return ESPHOME_ERR_INVALID_MESSAGE;
        }

        esp_err_t ret = buffer_read_byte(buf, &byte);
        if (ret != ESP_OK) {
            return ret;
        }

        *value |= ((uint64_t)(byte & 0x7F)) << shift;
        shift += 7;
    } while (byte & 0x80);

    return ESP_OK;
}

/**
 * @brief Get encoded varint size
 */
size_t esphome_varint_size(uint64_t value)
{
    size_t size = 1;
    while (value > 0x7F) {
        value >>= 7;
        size++;
    }
    return size;
}

/* ============================================================================
 * Protobuf Field Encoding
 * ============================================================================ */

/**
 * @brief Encode a protobuf field tag
 *
 * Encodes a protobuf field tag consisting of field number and wire type.
 * The tag format is: (field_number << 3) | wire_type
 *
 * @param[in,out] buf           Pointer to buffer for writing
 * @param[in]     field_number  Protobuf field number (1-based)
 * @param[in]     wire_type     Wire type for the field
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if buf is NULL
 * @return ESP_ERR_NO_MEM if buffer overflows
 */
esp_err_t esphome_encode_tag(esphome_buffer_t *buf, uint32_t field_number,
                             protobuf_wire_type_t wire_type)
{
    uint32_t tag = (field_number << 3) | wire_type;
    return esphome_encode_varint(buf, tag);
}

/**
 * @brief Decode a protobuf field tag
 */
esp_err_t esphome_decode_tag(esphome_buffer_t *buf, uint32_t *field_number,
                             protobuf_wire_type_t *wire_type)
{
    if (!field_number || !wire_type) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t tag;
    esp_err_t ret = esphome_decode_varint(buf, &tag);
    if (ret != ESP_OK) {
        return ret;
    }

    *wire_type = (protobuf_wire_type_t)(tag & 0x07);
    *field_number = tag >> 3;

    return ESP_OK;
}

/**
 * @brief Encode a boolean field
 *
 * Encodes a boolean value as a protobuf field with VARINT wire type.
 * The value is encoded as 0 (false) or 1 (true).
 *
 * @param[in,out] buf           Pointer to buffer for writing
 * @param[in]     field_number  Protobuf field number (1-based)
 * @param[in]     value         Boolean value to encode
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if buf is NULL
 * @return ESP_ERR_NO_MEM if buffer overflows
 */
esp_err_t esphome_encode_bool(esphome_buffer_t *buf, uint32_t field_number, bool value)
{
    esp_err_t ret = esphome_encode_tag(buf, field_number, PROTOBUF_WIRE_VARINT);
    if (ret != ESP_OK) {
        return ret;
    }
    return esphome_encode_varint(buf, value ? 1 : 0);
}

/**
 * @brief Encode a uint32 field
 */
esp_err_t esphome_encode_uint32(esphome_buffer_t *buf, uint32_t field_number, uint32_t value)
{
    esp_err_t ret = esphome_encode_tag(buf, field_number, PROTOBUF_WIRE_VARINT);
    if (ret != ESP_OK) {
        return ret;
    }
    return esphome_encode_varint(buf, value);
}

/**
 * @brief Encode a fixed32 field (wire type 5, 4-byte little-endian)
 */
esp_err_t esphome_encode_fixed32(esphome_buffer_t *buf, uint32_t field_number, uint32_t value)
{
    esp_err_t ret = esphome_encode_tag(buf, field_number, PROTOBUF_WIRE_32BIT);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t bytes[4] = {
        (uint8_t)(value & 0xFF),
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)((value >> 16) & 0xFF),
        (uint8_t)((value >> 24) & 0xFF)
    };

    return buffer_write_bytes(buf, bytes, 4);
}

/**
 * @brief Encode an int32 field
 */
esp_err_t esphome_encode_int32(esphome_buffer_t *buf, uint32_t field_number, int32_t value)
{
    esp_err_t ret = esphome_encode_tag(buf, field_number, PROTOBUF_WIRE_VARINT);
    if (ret != ESP_OK) {
        return ret;
    }
    /* Encode as unsigned using ZigZag encoding for negative values would be better,
     * but ESPHome uses simple cast */
    return esphome_encode_varint(buf, (uint64_t)(uint32_t)value);
}

/**
 * @brief Encode a float field (fixed32)
 */
esp_err_t esphome_encode_float(esphome_buffer_t *buf, uint32_t field_number, float value)
{
    esp_err_t ret = esphome_encode_tag(buf, field_number, PROTOBUF_WIRE_32BIT);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Write float as 4 bytes (little-endian) */
    uint32_t bits;
    memcpy(&bits, &value, sizeof(float));

    uint8_t bytes[4] = {
        (uint8_t)(bits & 0xFF),
        (uint8_t)((bits >> 8) & 0xFF),
        (uint8_t)((bits >> 16) & 0xFF),
        (uint8_t)((bits >> 24) & 0xFF)
    };

    return buffer_write_bytes(buf, bytes, 4);
}

/**
 * @brief Encode a string field
 */
esp_err_t esphome_encode_string(esphome_buffer_t *buf, uint32_t field_number, const char *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(value);

    esp_err_t ret = esphome_encode_tag(buf, field_number, PROTOBUF_WIRE_LEN);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esphome_encode_varint(buf, len);
    if (ret != ESP_OK) {
        return ret;
    }

    if (len > 0) {
        return buffer_write_bytes(buf, (const uint8_t *)value, len);
    }

    return ESP_OK;
}

/**
 * @brief Encode raw bytes field
 */
esp_err_t esphome_encode_bytes(esphome_buffer_t *buf, uint32_t field_number,
                               const uint8_t *data, size_t len)
{
    esp_err_t ret = esphome_encode_tag(buf, field_number, PROTOBUF_WIRE_LEN);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esphome_encode_varint(buf, len);
    if (ret != ESP_OK) {
        return ret;
    }

    if (len > 0 && data) {
        return buffer_write_bytes(buf, data, len);
    }

    return ESP_OK;
}

/**
 * @brief Encode a fixed64 field (8 bytes, little-endian)
 */
esp_err_t esphome_encode_fixed64(esphome_buffer_t *buf, uint32_t field_number, uint64_t value)
{
    esp_err_t ret = esphome_encode_tag(buf, field_number, PROTOBUF_WIRE_64BIT);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Write 8 bytes in little-endian order */
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) {
        bytes[i] = (value >> (i * 8)) & 0xFF;
    }

    return buffer_write_bytes(buf, bytes, 8);
}

/**
 * @brief Encode a sint32 field (ZigZag encoding)
 *
 * ZigZag encoding maps signed values to unsigned:
 * -1 -> 1, 1 -> 2, -2 -> 3, 2 -> 4, etc.
 */
esp_err_t esphome_encode_sint32(esphome_buffer_t *buf, uint32_t field_number, int32_t value)
{
    esp_err_t ret = esphome_encode_tag(buf, field_number, PROTOBUF_WIRE_VARINT);
    if (ret != ESP_OK) {
        return ret;
    }

    /* ZigZag encoding: (n << 1) ^ (n >> 31) */
    uint32_t zigzag = ((uint32_t)value << 1) ^ ((uint32_t)((int32_t)value >> 31));
    return esphome_encode_varint(buf, zigzag);
}

/**
 * @brief Encode a uint64 field (varint)
 */
esp_err_t esphome_encode_uint64(esphome_buffer_t *buf, uint32_t field_number, uint64_t value)
{
    esp_err_t ret = esphome_encode_tag(buf, field_number, PROTOBUF_WIRE_VARINT);
    if (ret != ESP_OK) {
        return ret;
    }

    return esphome_encode_varint(buf, value);
}

/* ============================================================================
 * Protobuf Field Decoding
 * ============================================================================ */

/**
 * @brief Decode a boolean from current buffer position
 */
esp_err_t esphome_decode_bool(esphome_buffer_t *buf, bool *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t v;
    esp_err_t ret = esphome_decode_varint(buf, &v);
    if (ret != ESP_OK) {
        return ret;
    }

    *value = (v != 0);
    return ESP_OK;
}

/**
 * @brief Decode a uint32 from current buffer position
 */
esp_err_t esphome_decode_uint32(esphome_buffer_t *buf, uint32_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t v;
    esp_err_t ret = esphome_decode_varint(buf, &v);
    if (ret != ESP_OK) {
        return ret;
    }

    *value = (uint32_t)v;
    return ESP_OK;
}

/**
 * @brief Decode a fixed32 from current buffer position (4-byte little-endian)
 */
esp_err_t esphome_decode_fixed32(esphome_buffer_t *buf, uint32_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t bytes[4];
    esp_err_t ret = buffer_read_bytes(buf, bytes, 4);
    if (ret != ESP_OK) {
        return ret;
    }

    *value = (uint32_t)bytes[0] |
             ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) |
             ((uint32_t)bytes[3] << 24);
    return ESP_OK;
}

/**
 * @brief Decode a float from current buffer position (fixed32)
 */
esp_err_t esphome_decode_float(esphome_buffer_t *buf, float *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t bytes[4];
    esp_err_t ret = buffer_read_bytes(buf, bytes, 4);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t bits = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
    memcpy(value, &bits, sizeof(float));

    return ESP_OK;
}

/**
 * @brief Decode a string from current buffer position
 *
 * Includes buffer bounds validation to prevent overflow attacks.
 */
esp_err_t esphome_decode_string(esphome_buffer_t *buf, char *value, size_t max_len)
{
    if (!buf || !value || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t len;
    esp_err_t ret = esphome_decode_varint(buf, &len);
    if (ret != ESP_OK) {
        return ret;
    }

    /* CRITICAL: Validate buffer has enough bytes before reading */
    if (len > esphome_buffer_remaining(buf)) {
        ESP_LOGE(TAG, "String length %llu exceeds buffer remaining %zu",
                 len, esphome_buffer_remaining(buf));
        return ESP_ERR_INVALID_SIZE;
    }

    /* Track original length for buffer position update */
    uint64_t original_len = len;

    /* Truncate if necessary, but still consume all bytes from buffer */
    if (len >= max_len) {
        ESP_LOGW(TAG, "String too long (%llu >= %zu), truncating", len, max_len);
        len = max_len - 1;
    }

    if (len > 0) {
        ret = buffer_read_bytes(buf, (uint8_t *)value, len);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    value[len] = '\0';

    /* Skip any remaining bytes if we truncated */
    if (original_len > len) {
        size_t skip = (size_t)(original_len - len);
        if (buf->position + skip <= buf->size) {
            buf->position += skip;
        }
    }

    return ESP_OK;
}

/**
 * @brief Decode raw bytes from current buffer position
 */
esp_err_t esphome_decode_bytes(esphome_buffer_t *buf, uint8_t *data, size_t max_len,
                                size_t *actual_len)
{
    if (!buf || !data || !actual_len || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t len;
    esp_err_t ret = esphome_decode_varint(buf, &len);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Validate buffer has enough bytes */
    if (len > esphome_buffer_remaining(buf)) {
        ESP_LOGE(TAG, "Bytes length %llu exceeds buffer remaining %zu",
                 len, esphome_buffer_remaining(buf));
        return ESP_ERR_INVALID_SIZE;
    }

    /* Track how much to actually copy */
    size_t copy_len = (len > max_len) ? max_len : (size_t)len;
    *actual_len = copy_len;

    if (copy_len > 0) {
        ret = buffer_read_bytes(buf, data, copy_len);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    /* Skip any remaining bytes if buffer was too small */
    if (len > copy_len) {
        size_t skip = (size_t)(len - copy_len);
        if (buf->position + skip <= buf->size) {
            buf->position += skip;
        }
    }

    return ESP_OK;
}

/**
 * @brief Decode a fixed64 from current buffer position (8 bytes, little-endian)
 */
esp_err_t esphome_decode_fixed64(esphome_buffer_t *buf, uint64_t *value)
{
    if (!buf || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t bytes[8];
    esp_err_t ret = buffer_read_bytes(buf, bytes, 8);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Read 8 bytes in little-endian order */
    *value = 0;
    for (int i = 0; i < 8; i++) {
        *value |= ((uint64_t)bytes[i]) << (i * 8);
    }

    return ESP_OK;
}

/**
 * @brief Skip a field value in buffer
 */
esp_err_t esphome_skip_field(esphome_buffer_t *buf, protobuf_wire_type_t wire_type)
{
    switch (wire_type) {
        case PROTOBUF_WIRE_VARINT: {
            uint64_t dummy;
            return esphome_decode_varint(buf, &dummy);
        }
        case PROTOBUF_WIRE_64BIT: {
            if (esphome_buffer_remaining(buf) < 8) {
                return ESP_ERR_INVALID_SIZE;
            }
            buf->position += 8;
            return ESP_OK;
        }
        case PROTOBUF_WIRE_LEN: {
            uint64_t len;
            esp_err_t ret = esphome_decode_varint(buf, &len);
            if (ret != ESP_OK) {
                return ret;
            }
            if (esphome_buffer_remaining(buf) < len) {
                return ESP_ERR_INVALID_SIZE;
            }
            buf->position += len;
            return ESP_OK;
        }
        case PROTOBUF_WIRE_32BIT: {
            if (esphome_buffer_remaining(buf) < 4) {
                return ESP_ERR_INVALID_SIZE;
            }
            buf->position += 4;
            return ESP_OK;
        }
        default:
            ESP_LOGE(TAG, "Unknown wire type: %d", wire_type);
            return ESPHOME_ERR_INVALID_MESSAGE;
    }
}

/* ============================================================================
 * Message Framing Functions
 * ============================================================================ */

/**
 * @brief Encode message frame header
 */
esp_err_t esphome_encode_frame_header(esphome_buffer_t *buf, esphome_msg_type_t msg_type,
                                       size_t payload_len)
{
    /* Write null byte marker */
    esp_err_t ret = buffer_write_byte(buf, 0x00);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Calculate and write total length (type varint size + payload) */
    size_t type_size = esphome_varint_size((uint64_t)msg_type);
    size_t total_len = type_size + payload_len;

    ret = esphome_encode_varint(buf, total_len);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Write message type */
    ret = esphome_encode_varint(buf, (uint64_t)msg_type);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief Decode message frame from buffer
 */
esp_err_t esphome_decode_frame(const uint8_t *data, size_t data_len,
                               esphome_message_t *msg, size_t *bytes_consumed)
{
    if (!data || !msg || !bytes_consumed || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, (uint8_t *)data, data_len);

    /* Check for null byte marker */
    uint8_t marker;
    esp_err_t ret = buffer_read_byte(&buf, &marker);
    if (ret != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (marker != 0x00) {
        ESP_LOGE(TAG, "Invalid frame marker: 0x%02X", marker);
        return ESPHOME_ERR_INVALID_MESSAGE;
    }

    /* Read total length */
    uint64_t total_len;
    ret = esphome_decode_varint(&buf, &total_len);
    if (ret != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Check if we have enough data */
    size_t header_size = buf.position;
    if (data_len < header_size + total_len) {
        /* Incomplete message, need more data */
        return ESP_ERR_INVALID_SIZE;
    }

    /* Read message type */
    uint64_t msg_type;
    ret = esphome_decode_varint(&buf, &msg_type);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t type_size = buf.position - header_size;
    size_t payload_len = total_len - type_size;

    msg->length = (uint32_t)total_len;
    msg->type = (esphome_msg_type_t)msg_type;
    msg->payload = (payload_len > 0) ? (uint8_t *)&data[buf.position] : NULL;
    msg->payload_len = payload_len;

    *bytes_consumed = header_size + total_len;

    ESP_LOGD(TAG, "Decoded frame: type=%d, payload_len=%zu", msg->type, payload_len);

    return ESP_OK;
}

/* ============================================================================
 * Message Building Helpers
 * ============================================================================ */

/**
 * @brief Build a complete message with frame
 */
esp_err_t esphome_build_message(esphome_msg_type_t msg_type, const uint8_t *payload,
                                 size_t payload_len, uint8_t *output, size_t output_size,
                                 size_t *output_len)
{
    if (!output || !output_len || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, output, output_size);

    /* Write frame header */
    esp_err_t ret = esphome_encode_frame_header(&buf, msg_type, payload_len);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Write payload if present */
    if (payload_len > 0 && payload) {
        ret = buffer_write_bytes(&buf, payload, payload_len);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    *output_len = buf.position;
    return ESP_OK;
}

/**
 * @brief Build an empty message (no payload)
 */
esp_err_t esphome_build_empty_message(esphome_msg_type_t msg_type, uint8_t *output,
                                       size_t output_size, size_t *output_len)
{
    return esphome_build_message(msg_type, NULL, 0, output, output_size, output_len);
}

/* ============================================================================
 * Protocol Utilities
 * ============================================================================ */

/**
 * @brief Get message type name for logging
 */
const char *esphome_msg_type_name(esphome_msg_type_t type)
{
    switch (type) {
        /* Core handshake */
        case ESPHOME_MSG_HELLO_REQUEST:               return "HelloRequest";
        case ESPHOME_MSG_HELLO_RESPONSE:              return "HelloResponse";
        case ESPHOME_MSG_CONNECT_REQUEST:             return "ConnectRequest";
        case ESPHOME_MSG_CONNECT_RESPONSE:            return "ConnectResponse";
        case ESPHOME_MSG_DISCONNECT_REQUEST:          return "DisconnectRequest";
        case ESPHOME_MSG_DISCONNECT_RESPONSE:         return "DisconnectResponse";
        case ESPHOME_MSG_PING_REQUEST:                return "PingRequest";
        case ESPHOME_MSG_PING_RESPONSE:               return "PingResponse";

        /* Device info */
        case ESPHOME_MSG_DEVICE_INFO_REQUEST:         return "DeviceInfoRequest";
        case ESPHOME_MSG_DEVICE_INFO_RESPONSE:        return "DeviceInfoResponse";

        /* Entity list */
        case ESPHOME_MSG_LIST_ENTITIES_REQUEST:       return "ListEntitiesRequest";
        case ESPHOME_MSG_LIST_ENTITIES_DONE_RESPONSE: return "ListEntitiesDone";
        case ESPHOME_MSG_LIST_ENTITIES_BINARY_SENSOR: return "ListEntitiesBinarySensor";
        case ESPHOME_MSG_LIST_ENTITIES_COVER:         return "ListEntitiesCover";
        case ESPHOME_MSG_LIST_ENTITIES_FAN:           return "ListEntitiesFan";
        case ESPHOME_MSG_LIST_ENTITIES_LIGHT:         return "ListEntitiesLight";
        case ESPHOME_MSG_LIST_ENTITIES_SENSOR:        return "ListEntitiesSensor";
        case ESPHOME_MSG_LIST_ENTITIES_SWITCH:        return "ListEntitiesSwitch";
        case ESPHOME_MSG_LIST_ENTITIES_TEXT_SENSOR:   return "ListEntitiesTextSensor";
        case ESPHOME_MSG_LIST_ENTITIES_CLIMATE:       return "ListEntitiesClimate";
        case ESPHOME_MSG_LIST_ENTITIES_NUMBER:        return "ListEntitiesNumber";
        case ESPHOME_MSG_LIST_ENTITIES_SELECT:        return "ListEntitiesSelect";
        case ESPHOME_MSG_LIST_ENTITIES_BUTTON:        return "ListEntitiesButton";

        /* State subscription */
        case ESPHOME_MSG_SUBSCRIBE_STATES_REQUEST:    return "SubscribeStatesRequest";
        case ESPHOME_MSG_BINARY_SENSOR_STATE:         return "BinarySensorState";
        case ESPHOME_MSG_COVER_STATE:                 return "CoverState";
        case ESPHOME_MSG_FAN_STATE:                   return "FanState";
        case ESPHOME_MSG_LIGHT_STATE:                 return "LightState";
        case ESPHOME_MSG_SENSOR_STATE:                return "SensorState";
        case ESPHOME_MSG_SWITCH_STATE:                return "SwitchState";
        case ESPHOME_MSG_TEXT_SENSOR_STATE:           return "TextSensorState";
        case ESPHOME_MSG_CLIMATE_STATE:               return "ClimateState";
        case ESPHOME_MSG_NUMBER_STATE:                return "NumberState";
        case ESPHOME_MSG_SELECT_STATE:                return "SelectState";

        /* Commands */
        case ESPHOME_MSG_COVER_COMMAND:               return "CoverCommand";
        case ESPHOME_MSG_FAN_COMMAND:                 return "FanCommand";
        case ESPHOME_MSG_LIGHT_COMMAND:               return "LightCommand";
        case ESPHOME_MSG_SWITCH_COMMAND:              return "SwitchCommand";
        case ESPHOME_MSG_CLIMATE_COMMAND:             return "ClimateCommand";
        case ESPHOME_MSG_NUMBER_COMMAND:              return "NumberCommand";
        case ESPHOME_MSG_SELECT_COMMAND:              return "SelectCommand";
        case ESPHOME_MSG_BUTTON_COMMAND:              return "ButtonCommand";

        /* Logs */
        case ESPHOME_MSG_SUBSCRIBE_LOGS_REQUEST:      return "SubscribeLogsRequest";
        case ESPHOME_MSG_SUBSCRIBE_LOGS_RESPONSE:     return "SubscribeLogsResponse";

        /* Time */
        case ESPHOME_MSG_GET_TIME_REQUEST:            return "GetTimeRequest";
        case ESPHOME_MSG_GET_TIME_RESPONSE:           return "GetTimeResponse";

        /* Services */
        case ESPHOME_MSG_LIST_SERVICES_REQUEST:       return "ListServicesRequest";
        case ESPHOME_MSG_LIST_SERVICES_RESPONSE:      return "ListServicesResponse";
        case ESPHOME_MSG_EXECUTE_SERVICE:             return "ExecuteService";

        /* Bluetooth proxy */
        case ESPHOME_MSG_SUBSCRIBE_BLE_ADVERTISEMENTS:  return "SubscribeBLEAdvertisements";
        case ESPHOME_MSG_BLE_ADVERTISEMENT_RESPONSE:    return "BLEAdvertisementResponse";
        case ESPHOME_MSG_BLE_DEVICE_REQUEST:            return "BLEDeviceRequest";
        case ESPHOME_MSG_BLE_DEVICE_CONNECTION_RESP:    return "BLEDeviceConnectionResp";
        case ESPHOME_MSG_BLE_GATT_GET_SERVICES_REQ:     return "BLEGATTGetServicesReq";
        case ESPHOME_MSG_BLE_GATT_GET_SERVICES_RESP:    return "BLEGATTGetServicesResp";
        case ESPHOME_MSG_BLE_GATT_SERVICES_DONE:        return "BLEGATTServicesDone";
        case ESPHOME_MSG_BLE_GATT_READ_REQUEST:         return "BLEGATTReadRequest";
        case ESPHOME_MSG_BLE_GATT_READ_RESPONSE:        return "BLEGATTReadResponse";
        case ESPHOME_MSG_BLE_GATT_WRITE_REQUEST:        return "BLEGATTWriteRequest";
        case ESPHOME_MSG_BLE_GATT_WRITE_RESPONSE:       return "BLEGATTWriteResponse";
        case ESPHOME_MSG_BLE_GATT_NOTIFY_REQUEST:       return "BLEGATTNotifyRequest";
        case ESPHOME_MSG_BLE_GATT_NOTIFY_DATA_RESP:     return "BLEGATTNotifyDataResp";
        case ESPHOME_MSG_SUBSCRIBE_BLE_CONNECTIONS_FREE: return "SubscribeBLEConnectionsFree";
        case ESPHOME_MSG_BLE_CONNECTIONS_FREE_RESP:     return "BLEConnectionsFreeResp";
        case ESPHOME_MSG_BLE_DEVICE_PAIRING_RESPONSE:   return "BLEDevicePairingResponse";
        case ESPHOME_MSG_UNSUBSCRIBE_BLE_ADVERTISEMENTS: return "UnsubscribeBLEAdvertisements";
        case ESPHOME_MSG_BLE_RAW_ADVERTISEMENTS_RESP:   return "BLERawAdvertisementsResp";
        case ESPHOME_MSG_BLE_SCANNER_STATE_RESPONSE:    return "BLEScannerStateResponse";
        case ESPHOME_MSG_BLE_SCANNER_SET_MODE_REQUEST:  return "BLEScannerSetModeRequest";

        default:                                        return "Unknown";
    }
}

/**
 * @brief Initialize protocol module
 */
esp_err_t esphome_protocol_init(void)
{
    ESP_LOGI(TAG, "ESPHome protocol module initialized");
    return ESP_OK;
}

/**
 * @brief Test protocol encoding/decoding
 */
esp_err_t esphome_protocol_test(void)
{
    ESP_LOGI(TAG, "=== ESPHome Protocol Test ===");

    uint8_t buffer[ESPHOME_BUFFER_MEDIUM];
    esphome_buffer_t buf;

    /* Test varint encoding/decoding */
    ESP_LOGI(TAG, "Testing varint encoding...");

    uint64_t test_values[] = {0, 1, 127, 128, 300, 16383, 16384, 0x7FFFFFFF, 0xFFFFFFFF};
    size_t num_tests = sizeof(test_values) / sizeof(test_values[0]);

    for (size_t i = 0; i < num_tests; i++) {
        esphome_buffer_init(&buf, buffer, sizeof(buffer));

        esp_err_t ret = esphome_encode_varint(&buf, test_values[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Varint encode failed for %llu", test_values[i]);
            return ESP_FAIL;
        }

        size_t encoded_len = buf.position;
        esphome_buffer_init(&buf, buffer, encoded_len);

        uint64_t decoded;
        ret = esphome_decode_varint(&buf, &decoded);
        if (ret != ESP_OK || decoded != test_values[i]) {
            ESP_LOGE(TAG, "Varint decode failed: expected %llu, got %llu",
                    test_values[i], decoded);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Varint tests passed");

    /* Test string encoding/decoding */
    ESP_LOGI(TAG, "Testing string encoding...");
    esphome_buffer_init(&buf, buffer, sizeof(buffer));

    const char *test_str = "Hello ESPHome!";
    esp_err_t ret = esphome_encode_string(&buf, 1, test_str);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "String encode failed");
        return ESP_FAIL;
    }

    size_t str_len = buf.position;
    esphome_buffer_init(&buf, buffer, str_len);

    /* Skip tag */
    uint32_t field_num;
    protobuf_wire_type_t wire_type;
    ret = esphome_decode_tag(&buf, &field_num, &wire_type);
    if (ret != ESP_OK || field_num != 1 || wire_type != PROTOBUF_WIRE_LEN) {
        ESP_LOGE(TAG, "Tag decode failed");
        return ESP_FAIL;
    }

    char decoded_str[ESPHOME_STRING_BUFFER_MEDIUM];
    ret = esphome_decode_string(&buf, decoded_str, sizeof(decoded_str));
    if (ret != ESP_OK || strcmp(decoded_str, test_str) != 0) {
        ESP_LOGE(TAG, "String decode failed: expected '%s', got '%s'", test_str, decoded_str);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "String tests passed");

    /* Test message framing */
    ESP_LOGI(TAG, "Testing message framing...");

    uint8_t payload[] = {0x0A, 0x05, 'H', 'e', 'l', 'l', 'o'};
    uint8_t output[ESPHOME_STRING_BUFFER_MEDIUM];
    size_t output_len;

    ret = esphome_build_message(ESPHOME_MSG_HELLO_RESPONSE, payload, sizeof(payload),
                                output, sizeof(output), &output_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Message build failed");
        return ESP_FAIL;
    }

    esphome_message_t msg;
    size_t consumed;
    ret = esphome_decode_frame(output, output_len, &msg, &consumed);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Frame decode failed");
        return ESP_FAIL;
    }

    if (msg.type != ESPHOME_MSG_HELLO_RESPONSE || msg.payload_len != sizeof(payload)) {
        ESP_LOGE(TAG, "Message decode mismatch: type=%d, payload_len=%zu",
                msg.type, msg.payload_len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Message framing tests passed");

    ESP_LOGI(TAG, "=== All Protocol Tests PASSED ===");
    return ESP_OK;
}
