/**
 * @file ble_xiaomi.c
 * @brief Xiaomi LYWSD03MMC BLE Sensor Parser Implementation
 *
 * Supports ATC and pvvx custom firmware advertisement formats.
 *
 * ATC Format (13 bytes service data):
 *   [0-5]  MAC address (reversed)
 *   [6-7]  Temperature (int16, 0.01 C resolution)
 *   [8]    Humidity (uint8, 1% resolution)
 *   [9]    Battery (uint8, 1% resolution)
 *   [10-11] Battery voltage (uint16, mV)
 *   [12]   Frame counter
 *
 * pvvx Format (17 bytes service data):
 *   [0-5]  MAC address
 *   [6-7]  Temperature (int16, 0.01 C resolution)
 *   [8-9]  Humidity (uint16, 0.01% resolution)
 *   [10-11] Battery voltage (uint16, mV)
 *   [12]   Battery (uint8, 1% resolution)
 *   [13]   Frame counter
 *   [14]   Flags
 *   [15-16] Reserved
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_xiaomi.h"
#include "../ble_constants.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BLE_XIAOMI";

/* ============================================================================
 * Private Types and Constants
 * ============================================================================ */

/** @brief BLE AD type for 16-bit service data */
#define BLE_AD_TYPE_SERVICE_DATA_16     0x16

/** @brief Minimum service data length for ATC format */
#define MIN_ATC_SERVICE_DATA_LEN        13

/** @brief Minimum service data length for pvvx format */
#define MIN_PVVX_SERVICE_DATA_LEN       15

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static const uint8_t *find_service_data(const uint8_t *adv_data, size_t adv_len,
                                         uint16_t uuid, size_t *out_len);
static esp_err_t parse_atc_format(const uint8_t *data, size_t len,
                                   ble_sensor_data_t *sensor);
static esp_err_t parse_pvvx_format(const uint8_t *data, size_t len,
                                    ble_sensor_data_t *sensor);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

ble_reg_device_type_t ble_xiaomi_detect(const ble_registry_adv_data_t *adv)
{
    BLE_VALIDATE_ADV_OR_RETURN_UNKNOWN(adv);

    /* Look for Environmental Sensing service data (UUID 0x181A) */
    size_t service_len = 0;
    const uint8_t *service_data = find_service_data(adv->adv_data, adv->adv_len,
                                                     BLE_XIAOMI_SERVICE_UUID, &service_len);

    if (service_data == NULL) {
        return BLE_REG_DEVICE_TYPE_UNKNOWN;
    }

    /* Check for valid ATC or pvvx format length */
    if (service_len >= MIN_ATC_SERVICE_DATA_LEN) {
        ESP_LOGD(TAG, "Detected Xiaomi LYWSD03MMC (service data len: %d)", service_len);
        return BLE_REG_DEVICE_TYPE_XIAOMI_LYWSD03MMC;
    }

    return BLE_REG_DEVICE_TYPE_UNKNOWN;
}

esp_err_t ble_xiaomi_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (adv->adv_data == NULL || adv->adv_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find service data */
    size_t service_len = 0;
    const uint8_t *service_data = find_service_data(adv->adv_data, adv->adv_len,
                                                     BLE_XIAOMI_SERVICE_UUID, &service_len);

    if (service_data == NULL) {
        ESP_LOGD(TAG, "No Environmental Sensing service data found");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Initialize output */
    memset(out_data, 0, sizeof(ble_parsed_data_t));
    ble_sensor_data_t *sensor = &out_data->sensor;
    sensor->rssi = adv->rssi;
    sensor->last_seen = adv->timestamp;

    esp_err_t ret;

    /* Determine format by length and parse */
    if (service_len >= MIN_PVVX_SERVICE_DATA_LEN) {
        /* Try pvvx format first (longer format) */
        ret = parse_pvvx_format(service_data, service_len, sensor);
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "Parsed pvvx format");
        }
    } else if (service_len >= MIN_ATC_SERVICE_DATA_LEN) {
        /* Try ATC format */
        ret = parse_atc_format(service_data, service_len, sensor);
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "Parsed ATC format");
        }
    } else {
        ESP_LOGW(TAG, "Service data too short: %d bytes", service_len);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (ret != ESP_OK) {
        return ret;
    }

    /* Validate parsed data */
    if (!ble_xiaomi_validate_data(sensor)) {
        ESP_LOGW(TAG, "Data validation failed: T=%.2f H=%.2f",
                 sensor->temperature, sensor->humidity);
        return ESP_ERR_INVALID_RESPONSE;
    }

    sensor->valid = true;

    ESP_LOGD(TAG, "Parsed: T=%.2f C, H=%.1f%%, Batt=%d%%",
             sensor->temperature, sensor->humidity, sensor->battery);

    return ESP_OK;
}

bool ble_xiaomi_validate_data(const ble_sensor_data_t *data)
{
    if (data == NULL) {
        return false;
    }

    /* Check temperature range */
    if (data->temperature < BLE_XIAOMI_TEMP_MIN ||
        data->temperature > BLE_XIAOMI_TEMP_MAX) {
        return false;
    }

    /* Check humidity range */
    if (data->humidity < BLE_XIAOMI_HUMIDITY_MIN ||
        data->humidity > BLE_XIAOMI_HUMIDITY_MAX) {
        return false;
    }

    /* Battery should be 0-100% */
    if (data->battery > BLE_BATTERY_PERCENTAGE_MAX) {
        return false;
    }

    return true;
}

const char *ble_xiaomi_get_firmware_type(const ble_registry_adv_data_t *adv)
{
    if (adv == NULL || adv->adv_data == NULL) {
        return "unknown";
    }

    size_t service_len = 0;
    const uint8_t *service_data = find_service_data(adv->adv_data, adv->adv_len,
                                                     BLE_XIAOMI_SERVICE_UUID, &service_len);

    if (service_data == NULL) {
        return "unknown";
    }

    if (service_len >= MIN_PVVX_SERVICE_DATA_LEN) {
        return "pvvx";
    } else if (service_len >= MIN_ATC_SERVICE_DATA_LEN) {
        return "ATC";
    }

    return "unknown";
}

/* ============================================================================
 * Private Function Implementation
 * ============================================================================ */

/**
 * @brief Find 16-bit UUID service data in advertisement
 *
 * @param[in] adv_data Advertisement data
 * @param[in] adv_len Advertisement data length
 * @param[in] uuid 16-bit service UUID to find
 * @param[out] out_len Output length (excluding UUID bytes)
 * @return Pointer to service data after UUID, or NULL if not found
 */
static const uint8_t *find_service_data(const uint8_t *adv_data, size_t adv_len,
                                         uint16_t uuid, size_t *out_len)
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

        /* Check for 16-bit service data */
        if (field_type == BLE_AD_TYPE_SERVICE_DATA_16 && field_len >= 3) {
            /* Extract UUID (little-endian) */
            uint16_t found_uuid = adv_data[offset + 2] | (adv_data[offset + 3] << 8);
            if (found_uuid == uuid) {
                *out_len = field_len - 3; /* Subtract type (1) and UUID (2) */
                return &adv_data[offset + 4]; /* Skip length, type, and UUID */
            }
        }

        offset += field_len + 1;
    }

    return NULL;
}

/**
 * @brief Parse ATC firmware format
 *
 * @param[in] data Service data (after UUID)
 * @param[in] len Data length
 * @param[out] sensor Output sensor data
 * @return ESP_OK on success
 */
static esp_err_t parse_atc_format(const uint8_t *data, size_t len,
                                   ble_sensor_data_t *sensor)
{
    if (len < BLE_XIAOMI_ATC_ADV_LEN - 2) { /* -2 for UUID already stripped */
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * ATC format (after UUID):
     * [0-5]  MAC (reversed)
     * [6-7]  Temperature (big-endian int16, 0.01 C)
     * [8]    Humidity (%)
     * [9]    Battery (%)
     * [10-11] Battery voltage (big-endian uint16, mV)
     * [12]   Frame counter
     */

    /* Temperature: big-endian int16 in 0.01 C units */
    int16_t temp_raw = (int16_t)((data[6] << 8) | data[7]);
    sensor->temperature = temp_raw / BLE_XIAOMI_TEMP_SCALE;

    /* Humidity: direct percentage */
    sensor->humidity = (float)data[8];

    /* Battery: direct percentage */
    sensor->battery = data[9];

    return ESP_OK;
}

/**
 * @brief Parse pvvx firmware format
 *
 * @param[in] data Service data (after UUID)
 * @param[in] len Data length
 * @param[out] sensor Output sensor data
 * @return ESP_OK on success
 */
static esp_err_t parse_pvvx_format(const uint8_t *data, size_t len,
                                    ble_sensor_data_t *sensor)
{
    if (len < MIN_PVVX_SERVICE_DATA_LEN - 2) { /* -2 for UUID already stripped */
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * pvvx format (after UUID):
     * [0-5]  MAC
     * [6-7]  Temperature (little-endian int16, 0.01 C)
     * [8-9]  Humidity (little-endian uint16, 0.01%)
     * [10-11] Battery voltage (little-endian uint16, mV)
     * [12]   Battery (%)
     * [13]   Frame counter
     * [14]   Flags
     */

    /* Temperature: little-endian int16 in 0.01 C units */
    int16_t temp_raw = (int16_t)(data[6] | (data[7] << 8));
    sensor->temperature = temp_raw / BLE_XIAOMI_TEMP_SCALE;

    /* Humidity: little-endian uint16 in 0.01% units */
    uint16_t hum_raw = data[8] | (data[9] << 8);
    sensor->humidity = hum_raw / BLE_XIAOMI_HUM_SCALE_PVVX;

    /* Battery: direct percentage */
    sensor->battery = data[12];

    return ESP_OK;
}
