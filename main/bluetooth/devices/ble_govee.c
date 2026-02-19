/**
 * @file ble_govee.c
 * @brief Govee H5075 BLE Thermometer/Hygrometer Parser Implementation
 *
 * Govee H5075 Manufacturer Data Format:
 *   [0-1]  Manufacturer ID (0xEC88, little-endian)
 *   [2]    Unknown (usually 0x00)
 *   [3-5]  Encoded temperature and humidity (3 bytes)
 *          Value = (temp_raw * 1000) + humidity_raw
 *          Where temp_raw is in 0.01 C and humidity_raw is in 0.01%
 *   [6]    Battery percentage
 *   [7-8]  Unknown/reserved
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_govee.h"
#include "../ble_constants.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BLE_GOVEE";

/* ============================================================================
 * Private Types and Constants
 * ============================================================================ */

/** @brief BLE AD type for manufacturer specific data */
#define BLE_AD_TYPE_MANUFACTURER_DATA   0xFF

/** @brief Minimum manufacturer data length for H5075 */
#define MIN_GOVEE_DATA_LEN              7

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static const uint8_t *find_manufacturer_data(const uint8_t *adv_data, size_t adv_len,
                                              uint16_t manufacturer_id, size_t *out_len);
static void decode_govee_temp_humidity(uint32_t encoded, float *temp, float *humidity);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

ble_reg_device_type_t ble_govee_detect(const ble_registry_adv_data_t *adv)
{
    BLE_VALIDATE_ADV_OR_RETURN_UNKNOWN(adv);

    /* Look for Govee manufacturer data */
    size_t mfr_len = 0;
    const uint8_t *mfr_data = find_manufacturer_data(adv->adv_data, adv->adv_len,
                                                      BLE_GOVEE_MANUFACTURER_ID, &mfr_len);

    if (mfr_data == NULL) {
        return BLE_REG_DEVICE_TYPE_UNKNOWN;
    }

    /* Check for valid H5075 data length */
    if (mfr_len >= MIN_GOVEE_DATA_LEN - 2) { /* -2 for manufacturer ID */
        ESP_LOGD(TAG, "Detected Govee H5075 (mfr data len: %d)", mfr_len);
        return BLE_REG_DEVICE_TYPE_GOVEE_H5075;
    }

    return BLE_REG_DEVICE_TYPE_UNKNOWN;
}

esp_err_t ble_govee_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (adv->adv_data == NULL || adv->adv_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find manufacturer data */
    size_t mfr_len = 0;
    const uint8_t *mfr_data = find_manufacturer_data(adv->adv_data, adv->adv_len,
                                                      BLE_GOVEE_MANUFACTURER_ID, &mfr_len);

    if (mfr_data == NULL) {
        ESP_LOGD(TAG, "No Govee manufacturer data found");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (mfr_len < MIN_GOVEE_DATA_LEN - 2) {
        ESP_LOGW(TAG, "Manufacturer data too short: %d bytes", mfr_len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Initialize output */
    memset(out_data, 0, sizeof(ble_parsed_data_t));
    ble_sensor_data_t *sensor = &out_data->sensor;
    sensor->rssi = adv->rssi;
    sensor->last_seen = adv->timestamp;

    /*
     * Parse Govee H5075 format (after manufacturer ID):
     * [0]    Unknown byte
     * [1-3]  Encoded temp/humidity (big-endian 24-bit)
     * [4]    Battery percentage
     */

    /* Extract 24-bit encoded value (big-endian) */
    uint32_t encoded = ((uint32_t)mfr_data[1] << 16) |
                       ((uint32_t)mfr_data[2] << 8) |
                       ((uint32_t)mfr_data[3]);

    /* Decode temperature and humidity */
    decode_govee_temp_humidity(encoded, &sensor->temperature, &sensor->humidity);

    /* Battery percentage */
    sensor->battery = mfr_data[4];

    /* Validate parsed data */
    if (!ble_govee_validate_data(sensor)) {
        ESP_LOGW(TAG, "Data validation failed: T=%.2f H=%.2f",
                 sensor->temperature, sensor->humidity);
        return ESP_ERR_INVALID_RESPONSE;
    }

    sensor->valid = true;

    ESP_LOGD(TAG, "Parsed: T=%.2f C, H=%.1f%%, Batt=%d%%",
             sensor->temperature, sensor->humidity, sensor->battery);

    return ESP_OK;
}

bool ble_govee_validate_data(const ble_sensor_data_t *data)
{
    if (data == NULL) {
        return false;
    }

    /* Check temperature range */
    if (data->temperature < BLE_GOVEE_TEMP_MIN ||
        data->temperature > BLE_GOVEE_TEMP_MAX) {
        return false;
    }

    /* Check humidity range */
    if (data->humidity < BLE_GOVEE_HUMIDITY_MIN ||
        data->humidity > BLE_GOVEE_HUMIDITY_MAX) {
        return false;
    }

    /* Battery should be 0-100% */
    if (data->battery > BLE_BATTERY_PERCENTAGE_MAX) {
        return false;
    }

    return true;
}

const char *ble_govee_get_model(const ble_registry_adv_data_t *adv)
{
    if (adv == NULL || adv->adv_data == NULL) {
        return "unknown";
    }

    size_t mfr_len = 0;
    const uint8_t *mfr_data = find_manufacturer_data(adv->adv_data, adv->adv_len,
                                                      BLE_GOVEE_MANUFACTURER_ID, &mfr_len);

    if (mfr_data != NULL && mfr_len >= MIN_GOVEE_DATA_LEN - 2) {
        /* H5075 is the most common model with this format */
        return "H5075";
    }

    return "unknown";
}

/* ============================================================================
 * Private Function Implementation
 * ============================================================================ */

/**
 * @brief Find manufacturer specific data in advertisement
 *
 * @param[in] adv_data Advertisement data
 * @param[in] adv_len Advertisement data length
 * @param[in] manufacturer_id Manufacturer ID to find
 * @param[out] out_len Output length (excluding manufacturer ID)
 * @return Pointer to data after manufacturer ID, or NULL if not found
 */
static const uint8_t *find_manufacturer_data(const uint8_t *adv_data, size_t adv_len,
                                              uint16_t manufacturer_id, size_t *out_len)
{
    if (adv_data == NULL || adv_len < 4 || out_len == NULL) {
        return NULL;
    }

    size_t offset = 0;
    while (offset < adv_len) {
        uint8_t field_len = adv_data[offset];
        if (field_len == 0 || offset + field_len > adv_len) {
            break;
        }

        uint8_t field_type = adv_data[offset + 1];

        /* Check for manufacturer specific data */
        if (field_type == BLE_AD_TYPE_MANUFACTURER_DATA && field_len >= 3) {
            /* Extract manufacturer ID (little-endian) */
            uint16_t found_id = adv_data[offset + 2] | (adv_data[offset + 3] << 8);
            if (found_id == manufacturer_id) {
                *out_len = field_len - 3; /* Subtract type (1) and ID (2) */
                return &adv_data[offset + 4]; /* Skip length, type, and ID */
            }
        }

        offset += field_len + 1;
    }

    return NULL;
}

/**
 * @brief Decode Govee encoded temperature and humidity
 *
 * Govee encodes temp and humidity in a single 24-bit value:
 * encoded = (temp_in_0.01C * 1000) + humidity_in_0.01%
 *
 * Example: 25.5C and 60.5% humidity:
 *   encoded = (2550 * 1000) + 6050 = 2556050
 *
 * To decode:
 *   humidity_raw = encoded % 1000
 *   temp_raw = encoded / 1000
 *
 * @param[in] encoded 24-bit encoded value
 * @param[out] temp Temperature in Celsius
 * @param[out] humidity Humidity in percent
 */
static void decode_govee_temp_humidity(uint32_t encoded, float *temp, float *humidity)
{
    /* Handle negative temperatures (sign bit in MSB) */
    bool negative = (encoded & 0x800000) != 0;
    if (negative) {
        encoded = encoded & 0x7FFFFF; /* Clear sign bit */
    }

    /* Decode humidity (last 3 digits, in 0.01% units) */
    uint32_t humidity_raw = encoded % BLE_GOVEE_ENCODING_MODULO;
    *humidity = humidity_raw / BLE_GOVEE_HUM_DIVISOR;

    /* Decode temperature (remaining digits, in 0.01 C units) */
    uint32_t temp_raw = encoded / BLE_GOVEE_ENCODING_MODULO;
    *temp = temp_raw / BLE_GOVEE_TEMP_DIVISOR;

    if (negative) {
        *temp = -(*temp);
    }
}
