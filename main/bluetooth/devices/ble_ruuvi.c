/**
 * @file ble_ruuvi.c
 * @brief Ruuvi Tag BLE Environmental Sensor Parser Implementation
 *
 * Ruuvi RAWv1 (Data Format 3) - 14 bytes after manufacturer ID:
 *   [0]     Data format (0x03)
 *   [1]     Humidity (0.5% resolution, 0-200 = 0-100%)
 *   [2]     Temperature integer (unsigned, bit 7 = sign)
 *   [3]     Temperature fraction (0.01 C units)
 *   [4]     Pressure MSB (hPa + 50000, unsigned 16-bit)
 *   [5]     Pressure LSB
 *   [6-7]   Acceleration X (signed 16-bit, mG, big-endian)
 *   [8-9]   Acceleration Y
 *   [10-11] Acceleration Z
 *   [12-13] Battery voltage (mV, big-endian)
 *
 * Ruuvi RAWv2 (Data Format 5) - 18 bytes (or 24 with MAC):
 *   [0]     Data format (0x05)
 *   [1-2]   Temperature (0.005 C resolution, signed 16-bit, big-endian)
 *   [3-4]   Humidity (0.0025% resolution, unsigned 16-bit, big-endian)
 *   [5-6]   Pressure (1 Pa resolution, offset 50000, unsigned 16-bit, big-endian)
 *   [7-8]   Acceleration X (mG, signed 16-bit, big-endian)
 *   [9-10]  Acceleration Y
 *   [11-12] Acceleration Z
 *   [13-14] Power info (bits 15-11: TX power, bits 10-0: battery mV)
 *   [15]    Movement counter
 *   [16-17] Measurement sequence (big-endian)
 *   [18-23] MAC address (optional, in extended format)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_ruuvi.h"
#include "../ble_constants.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BLE_RUUVI";

/* ============================================================================
 * Private Constants
 * ============================================================================ */

/** @brief BLE AD type for manufacturer specific data */
#define BLE_AD_TYPE_MANUFACTURER_DATA   0xFF

/** @brief Minimum data length for RAWv1 detection (format byte + some data) */
#define MIN_RUUVI_DATA_LEN              5

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static const uint8_t *find_manufacturer_data(const uint8_t *adv_data, size_t adv_len,
                                              uint16_t manufacturer_id, size_t *out_len);
static esp_err_t parse_rawv1(const uint8_t *data, size_t len, ble_ruuvi_data_t *out);
static esp_err_t parse_rawv2(const uint8_t *data, size_t len, ble_ruuvi_data_t *out);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

ble_reg_device_type_t ble_ruuvi_detect(const ble_registry_adv_data_t *adv)
{
    BLE_VALIDATE_ADV_OR_RETURN_UNKNOWN(adv);

    /* Look for Ruuvi manufacturer data */
    size_t mfr_len = 0;
    const uint8_t *mfr_data = find_manufacturer_data(adv->adv_data, adv->adv_len,
                                                      BLE_RUUVI_MANUFACTURER_ID, &mfr_len);

    if (mfr_data == NULL || mfr_len < MIN_RUUVI_DATA_LEN) {
        return BLE_REG_DEVICE_TYPE_UNKNOWN;
    }

    /* Check data format byte */
    uint8_t format = mfr_data[0];
    if (format == BLE_RUUVI_FORMAT_RAWV1 && mfr_len >= BLE_RUUVI_RAWV1_LEN) {
        ESP_LOGD(TAG, "Detected Ruuvi Tag RAWv1 (len: %d)", mfr_len);
        return BLE_REG_DEVICE_TYPE_RUUVI_TAG;
    }

    if (format == BLE_RUUVI_FORMAT_RAWV2 && mfr_len >= BLE_RUUVI_RAWV2_LEN) {
        ESP_LOGD(TAG, "Detected Ruuvi Tag RAWv2 (len: %d)", mfr_len);
        return BLE_REG_DEVICE_TYPE_RUUVI_TAG;
    }

    return BLE_REG_DEVICE_TYPE_UNKNOWN;
}

esp_err_t ble_ruuvi_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (adv->adv_data == NULL || adv->adv_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse extended data first */
    ble_ruuvi_data_t ruuvi_data;
    esp_err_t ret = ble_ruuvi_parse_extended(adv, &ruuvi_data);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Map to standard sensor data structure */
    memset(out_data, 0, sizeof(ble_parsed_data_t));
    ble_sensor_data_t *sensor = &out_data->sensor;

    sensor->temperature = ruuvi_data.temperature;
    sensor->humidity = ruuvi_data.humidity;
    sensor->battery = ble_ruuvi_voltage_to_percent(ruuvi_data.battery_voltage);
    sensor->rssi = adv->rssi;
    sensor->last_seen = adv->timestamp;

    /* Validate standard sensor data */
    if (!ble_ruuvi_validate_data(sensor)) {
        ESP_LOGW(TAG, "Data validation failed: T=%.2f H=%.2f",
                 sensor->temperature, sensor->humidity);
        return ESP_ERR_INVALID_RESPONSE;
    }

    sensor->valid = true;

    ESP_LOGD(TAG, "Parsed: T=%.2f C, H=%.1f%%, P=%.1f hPa, Batt=%.2fV (%d%%)",
             sensor->temperature, sensor->humidity, ruuvi_data.pressure,
             ruuvi_data.battery_voltage, sensor->battery);

    return ESP_OK;
}

esp_err_t ble_ruuvi_parse_extended(const ble_registry_adv_data_t *adv, ble_ruuvi_data_t *out_data)
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
                                                      BLE_RUUVI_MANUFACTURER_ID, &mfr_len);

    if (mfr_data == NULL || mfr_len < MIN_RUUVI_DATA_LEN) {
        ESP_LOGD(TAG, "No Ruuvi manufacturer data found");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Initialize output */
    memset(out_data, 0, sizeof(ble_ruuvi_data_t));
    out_data->data_format = mfr_data[0];

    esp_err_t ret;
    switch (out_data->data_format) {
        case BLE_RUUVI_FORMAT_RAWV1:
            if (mfr_len < BLE_RUUVI_RAWV1_LEN) {
                ESP_LOGW(TAG, "RAWv1 data too short: %d bytes", mfr_len);
                return ESP_ERR_INVALID_SIZE;
            }
            ret = parse_rawv1(mfr_data, mfr_len, out_data);
            break;

        case BLE_RUUVI_FORMAT_RAWV2:
            if (mfr_len < BLE_RUUVI_RAWV2_LEN) {
                ESP_LOGW(TAG, "RAWv2 data too short: %d bytes", mfr_len);
                return ESP_ERR_INVALID_SIZE;
            }
            ret = parse_rawv2(mfr_data, mfr_len, out_data);
            break;

        default:
            ESP_LOGW(TAG, "Unknown Ruuvi data format: 0x%02X", out_data->data_format);
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (ret != ESP_OK) {
        return ret;
    }

    /* Validate extended data */
    if (!ble_ruuvi_validate_extended(out_data)) {
        ESP_LOGW(TAG, "Extended data validation failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    out_data->valid = true;
    return ESP_OK;
}

bool ble_ruuvi_validate_data(const ble_sensor_data_t *data)
{
    if (data == NULL) {
        return false;
    }

    /* Check temperature range */
    if (data->temperature < BLE_RUUVI_TEMP_MIN ||
        data->temperature > BLE_RUUVI_TEMP_MAX) {
        return false;
    }

    /* Check humidity range */
    if (data->humidity < BLE_RUUVI_HUMIDITY_MIN ||
        data->humidity > BLE_RUUVI_HUMIDITY_MAX) {
        return false;
    }

    /* Battery should be 0-100% */
    if (data->battery > BLE_BATTERY_PERCENTAGE_MAX) {
        return false;
    }

    return true;
}

bool ble_ruuvi_validate_extended(const ble_ruuvi_data_t *data)
{
    if (data == NULL) {
        return false;
    }

    /* Check temperature range */
    if (data->temperature < BLE_RUUVI_TEMP_MIN ||
        data->temperature > BLE_RUUVI_TEMP_MAX) {
        return false;
    }

    /* Check humidity range */
    if (data->humidity < BLE_RUUVI_HUMIDITY_MIN ||
        data->humidity > BLE_RUUVI_HUMIDITY_MAX) {
        return false;
    }

    /* Check pressure range */
    if (data->pressure < BLE_RUUVI_PRESSURE_MIN ||
        data->pressure > BLE_RUUVI_PRESSURE_MAX) {
        return false;
    }

    /* Check battery voltage range */
    if (data->battery_voltage < BLE_RUUVI_BATTERY_MIN ||
        data->battery_voltage > BLE_RUUVI_BATTERY_MAX) {
        return false;
    }

    return true;
}

const char *ble_ruuvi_get_format_name(const ble_registry_adv_data_t *adv)
{
    if (adv == NULL || adv->adv_data == NULL) {
        return "unknown";
    }

    size_t mfr_len = 0;
    const uint8_t *mfr_data = find_manufacturer_data(adv->adv_data, adv->adv_len,
                                                      BLE_RUUVI_MANUFACTURER_ID, &mfr_len);

    if (mfr_data == NULL || mfr_len < 1) {
        return "unknown";
    }

    switch (mfr_data[0]) {
        case BLE_RUUVI_FORMAT_RAWV1:
            return "RAWv1";
        case BLE_RUUVI_FORMAT_RAWV2:
            return "RAWv2";
        default:
            return "unknown";
    }
}

uint8_t ble_ruuvi_voltage_to_percent(float voltage)
{
    /* Convert V to mV for calculation */
    int voltage_mv = (int)(voltage * BLE_VOLTAGE_TO_MV_SCALE);

    /* Clamp to valid range */
    if (voltage_mv <= BLE_RUUVI_BATTERY_EMPTY_MV) {
        return 0;
    }
    if (voltage_mv >= BLE_RUUVI_BATTERY_FULL_MV) {
        return BLE_BATTERY_PERCENTAGE_MAX;
    }

    /* Linear interpolation between empty and full */
    int range = BLE_RUUVI_BATTERY_FULL_MV - BLE_RUUVI_BATTERY_EMPTY_MV;
    int level = voltage_mv - BLE_RUUVI_BATTERY_EMPTY_MV;

    return (uint8_t)((level * BLE_BATTERY_PERCENTAGE_MAX) / range);
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
 * @brief Parse RAWv1 (Data Format 3) payload
 *
 * @param[in] data Raw manufacturer data (starting with format byte)
 * @param[in] len Data length
 * @param[out] out Parsed Ruuvi data
 * @return ESP_OK on success
 */
static esp_err_t parse_rawv1(const uint8_t *data, size_t len, ble_ruuvi_data_t *out)
{
    /* Format: [0]=fmt [1]=hum [2]=temp_int [3]=temp_frac [4-5]=press [6-13]=accel+batt */

    /* Humidity: value * 0.5% */
    out->humidity = data[1] * BLE_RUUVI_HUM_RESOLUTION_V1;

    /* Temperature: integer part with sign bit, fraction in 0.01 C */
    uint8_t temp_int = data[2] & 0x7F;  /* Lower 7 bits = magnitude */
    bool temp_negative = (data[2] & 0x80) != 0;
    float temp_frac = data[3] / BLE_RUUVI_TEMP_FRAC_SCALE_V1;
    out->temperature = temp_int + temp_frac;
    if (temp_negative) {
        out->temperature = -out->temperature;
    }

    /* Pressure: 16-bit value + 50000 Pa, convert to hPa */
    uint16_t pressure_raw = ((uint16_t)data[4] << 8) | data[5];
    out->pressure = (pressure_raw + BLE_RUUVI_PRESSURE_OFFSET_PA) / BLE_RUUVI_PRESSURE_SCALE;

    /* Acceleration X, Y, Z (signed 16-bit, big-endian, mG) */
    out->accel_x = (int16_t)(((uint16_t)data[6] << 8) | data[7]);
    out->accel_y = (int16_t)(((uint16_t)data[8] << 8) | data[9]);
    out->accel_z = (int16_t)(((uint16_t)data[10] << 8) | data[11]);

    /* Battery voltage (mV, big-endian) */
    uint16_t battery_mv = ((uint16_t)data[12] << 8) | data[13];
    out->battery_voltage = battery_mv / BLE_VOLTAGE_TO_MV_SCALE;

    /* RAWv1 doesn't have TX power, movement, or sequence */
    out->tx_power = 0;
    out->movement_counter = 0;
    out->measurement_seq = 0;

    ESP_LOGD(TAG, "RAWv1: T=%.2f H=%.1f P=%.1f Batt=%.2fV Accel=[%d,%d,%d]",
             out->temperature, out->humidity, out->pressure,
             out->battery_voltage, out->accel_x, out->accel_y, out->accel_z);

    return ESP_OK;
}

/**
 * @brief Parse RAWv2 (Data Format 5) payload
 *
 * @param[in] data Raw manufacturer data (starting with format byte)
 * @param[in] len Data length
 * @param[out] out Parsed Ruuvi data
 * @return ESP_OK on success
 */
static esp_err_t parse_rawv2(const uint8_t *data, size_t len, ble_ruuvi_data_t *out)
{
    /* Temperature: signed 16-bit, 0.005 C resolution, big-endian */
    int16_t temp_raw = (int16_t)(((uint16_t)data[1] << 8) | data[2]);
    if (temp_raw == (int16_t)BLE_RUUVI_INVALID_TEMP) {
        out->temperature = 0.0f;  /* Invalid marker */
    } else {
        out->temperature = temp_raw * BLE_RUUVI_TEMP_RESOLUTION_V2;
    }

    /* Humidity: unsigned 16-bit, 0.0025% resolution, big-endian */
    uint16_t hum_raw = ((uint16_t)data[3] << 8) | data[4];
    if (hum_raw == BLE_RUUVI_INVALID_HUMIDITY) {
        out->humidity = 0.0f;  /* Invalid marker */
    } else {
        out->humidity = hum_raw * BLE_RUUVI_HUM_RESOLUTION_V2;
    }

    /* Pressure: unsigned 16-bit, 1 Pa resolution, offset 50000, big-endian */
    uint16_t press_raw = ((uint16_t)data[5] << 8) | data[6];
    if (press_raw == BLE_RUUVI_INVALID_PRESSURE) {
        out->pressure = 0.0f;  /* Invalid marker */
    } else {
        out->pressure = (press_raw + BLE_RUUVI_PRESSURE_OFFSET_PA) / BLE_RUUVI_PRESSURE_SCALE;  /* Convert Pa to hPa */
    }

    /* Acceleration X, Y, Z (signed 16-bit, big-endian, mG) */
    out->accel_x = (int16_t)(((uint16_t)data[7] << 8) | data[8]);
    out->accel_y = (int16_t)(((uint16_t)data[9] << 8) | data[10]);
    out->accel_z = (int16_t)(((uint16_t)data[11] << 8) | data[12]);

    /* Power info: bits 15-11 = TX power, bits 10-0 = battery mV */
    uint16_t power_info = ((uint16_t)data[13] << 8) | data[14];
    uint8_t tx_power_raw = (power_info >> 11) & 0x1F;  /* 5 bits */
    uint16_t battery_mv = power_info & 0x07FF;          /* 11 bits */

    /* TX power: value * 2 - 40 = dBm */
    out->tx_power = (int8_t)((tx_power_raw * 2) - BLE_RUUVI_TX_POWER_OFFSET);

    /* Battery: add 1600 mV offset, convert to V */
    out->battery_voltage = (battery_mv + BLE_RUUVI_BATTERY_OFFSET_MV_V2) / BLE_VOLTAGE_TO_MV_SCALE;

    /* Movement counter */
    out->movement_counter = data[15];

    /* Measurement sequence (big-endian) */
    out->measurement_seq = ((uint16_t)data[16] << 8) | data[17];

    ESP_LOGD(TAG, "RAWv2: T=%.2f H=%.1f P=%.1f Batt=%.2fV TxPwr=%d Accel=[%d,%d,%d] Mov=%d Seq=%d",
             out->temperature, out->humidity, out->pressure,
             out->battery_voltage, out->tx_power,
             out->accel_x, out->accel_y, out->accel_z,
             out->movement_counter, out->measurement_seq);

    return ESP_OK;
}
