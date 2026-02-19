/**
 * @file ble_inkbird.c
 * @brief Inkbird BLE Temperature/Humidity Sensor Parser Implementation
 *
 * Inkbird sensors use multiple data formats depending on the model.
 *
 * Format 1 (Service Data UUID 0xFFF0, 7 bytes):
 *   [0-1]   Temperature (signed 16-bit, 0.01°C, little-endian)
 *   [2-3]   Humidity (unsigned 16-bit, 0.01%, little-endian)
 *   [4-5]   External Temp (signed 16-bit, 0.01°C, little-endian, optional)
 *   [6]     Sensor flags / battery indicator
 *
 * Format 2 (Manufacturer Data, 6 bytes):
 *   [0-1]   Temperature (signed 16-bit, 0.01°C, little-endian)
 *   [2-3]   Humidity (unsigned 16-bit, 0.01%, little-endian)
 *   [4-5]   External Temp or reserved
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_inkbird.h"
#include "../ble_constants.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BLE_INKBIRD";

/* ============================================================================
 * Private Constants
 * ============================================================================ */

/** @brief BLE AD type for 16-bit service data */
#define BLE_AD_TYPE_SERVICE_DATA_16     0x16

/** @brief BLE AD type for complete local name */
#define BLE_AD_TYPE_COMPLETE_NAME       0x09

/** @brief BLE AD type for short local name */
#define BLE_AD_TYPE_SHORT_NAME          0x08

/** @brief BLE AD type for manufacturer data */
#define BLE_AD_TYPE_MANUFACTURER_DATA   0xFF

/** @brief Minimum service data length */
#define MIN_INKBIRD_SERVICE_DATA_LEN    4

/** @brief Minimum manufacturer data length */
#define MIN_INKBIRD_MFR_DATA_LEN        6

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static const uint8_t *find_service_data(const uint8_t *adv_data, size_t adv_len,
                                         uint16_t uuid, size_t *out_len);
static const uint8_t *find_manufacturer_data(const uint8_t *adv_data, size_t adv_len,
                                              size_t *out_len);
static const char *find_device_name(const uint8_t *adv_data, size_t adv_len, size_t *name_len);
static bool is_inkbird_name(const char *name, size_t len);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

ble_reg_device_type_t ble_inkbird_detect(const ble_registry_adv_data_t *adv)
{
    BLE_VALIDATE_ADV_OR_RETURN_UNKNOWN(adv);

    /* Method 1: Check for Inkbird service UUID 0xFFF0 */
    size_t svc_len = 0;
    const uint8_t *svc_data = find_service_data(adv->adv_data, adv->adv_len,
                                                 BLE_INKBIRD_SERVICE_UUID, &svc_len);

    if (svc_data != NULL && svc_len >= MIN_INKBIRD_SERVICE_DATA_LEN) {
        ESP_LOGD(TAG, "Detected Inkbird via service UUID 0xFFF0 (len: %d)", svc_len);
        return BLE_REG_DEVICE_TYPE_INKBIRD;
    }

    /* Method 2: Check device name prefix */
    size_t name_len = 0;
    const char *name = find_device_name(adv->adv_data, adv->adv_len, &name_len);

    if (name != NULL && is_inkbird_name(name, name_len)) {
        ESP_LOGD(TAG, "Detected Inkbird via device name: %.*s", (int)name_len, name);
        return BLE_REG_DEVICE_TYPE_INKBIRD;
    }

    return BLE_REG_DEVICE_TYPE_UNKNOWN;
}

esp_err_t ble_inkbird_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse extended data first */
    ble_inkbird_data_t ink_data;
    esp_err_t ret = ble_inkbird_parse_extended(adv, &ink_data);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Map to standard sensor data structure */
    memset(out_data, 0, sizeof(ble_parsed_data_t));
    ble_sensor_data_t *sensor = &out_data->sensor;

    sensor->temperature = ink_data.temperature;
    sensor->humidity = ink_data.has_humidity ? ink_data.humidity : 0.0f;
    sensor->battery = 0;  /* Inkbird doesn't report battery in advertisement */
    sensor->rssi = adv->rssi;
    sensor->last_seen = adv->timestamp;

    /* Validate standard sensor data */
    if (!ble_inkbird_validate_data(sensor)) {
        ESP_LOGW(TAG, "Data validation failed: T=%.2f H=%.2f",
                 sensor->temperature, sensor->humidity);
        return ESP_ERR_INVALID_RESPONSE;
    }

    sensor->valid = true;

    ESP_LOGD(TAG, "Parsed %s: T=%.2f C, H=%.1f%%",
             ble_inkbird_get_model_name(ink_data.model),
             sensor->temperature, sensor->humidity);

    return ESP_OK;
}

esp_err_t ble_inkbird_parse_extended(const ble_registry_adv_data_t *adv, ble_inkbird_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (adv->adv_data == NULL || adv->adv_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Initialize output */
    memset(out_data, 0, sizeof(ble_inkbird_data_t));

    /* Try service data first (preferred) */
    size_t svc_len = 0;
    const uint8_t *svc_data = find_service_data(adv->adv_data, adv->adv_len,
                                                 BLE_INKBIRD_SERVICE_UUID, &svc_len);

    if (svc_data != NULL && svc_len >= MIN_INKBIRD_SERVICE_DATA_LEN) {
        /* Temperature: signed 16-bit, 0.01°C, little-endian */
        int16_t temp_raw = (int16_t)(svc_data[0] | (svc_data[1] << 8));
        out_data->temperature = temp_raw / BLE_INKBIRD_TEMP_HUM_SCALE;

        /* Humidity: unsigned 16-bit, 0.01%, little-endian */
        uint16_t hum_raw = svc_data[2] | (svc_data[3] << 8);
        out_data->humidity = hum_raw / BLE_INKBIRD_TEMP_HUM_SCALE;
        out_data->has_humidity = true;

        /* External temperature probe (if present) */
        if (svc_len >= 6) {
            int16_t ext_temp_raw = (int16_t)(svc_data[4] | (svc_data[5] << 8));
            /* Check for invalid marker (0x8000 or 0x7FFF often used) */
            if (ext_temp_raw != (int16_t)0x8000 && ext_temp_raw != 0x7FFF) {
                out_data->external_temp = ext_temp_raw / BLE_INKBIRD_TEMP_HUM_SCALE;
                out_data->has_external_temp = true;
            }
        }

        out_data->model = out_data->has_external_temp ?
                          BLE_INKBIRD_MODEL_ITH_20R :
                          (out_data->has_humidity ? BLE_INKBIRD_MODEL_IBS_TH2 : BLE_INKBIRD_MODEL_IBS_TH1);
        out_data->valid = true;

        ESP_LOGD(TAG, "Service data: T=%.2f H=%.1f ExtT=%.2f",
                 out_data->temperature, out_data->humidity, out_data->external_temp);

        return ESP_OK;
    }

    /* Fall back to manufacturer data */
    size_t mfr_len = 0;
    const uint8_t *mfr_data = find_manufacturer_data(adv->adv_data, adv->adv_len, &mfr_len);

    if (mfr_data != NULL && mfr_len >= MIN_INKBIRD_MFR_DATA_LEN) {
        /* Skip first 2 bytes (manufacturer ID) */
        const uint8_t *data = mfr_data + 2;
        size_t data_len = mfr_len - 2;

        if (data_len >= 4) {
            /* Temperature: signed 16-bit, 0.01°C, little-endian */
            int16_t temp_raw = (int16_t)(data[0] | (data[1] << 8));
            out_data->temperature = temp_raw / BLE_INKBIRD_TEMP_HUM_SCALE;

            /* Humidity: unsigned 16-bit, 0.01%, little-endian */
            uint16_t hum_raw = data[2] | (data[3] << 8);
            out_data->humidity = hum_raw / BLE_INKBIRD_TEMP_HUM_SCALE;
            out_data->has_humidity = true;

            out_data->model = BLE_INKBIRD_MODEL_IBS_TH2;
            out_data->valid = true;

            ESP_LOGD(TAG, "Manufacturer data: T=%.2f H=%.1f",
                     out_data->temperature, out_data->humidity);

            return ESP_OK;
        }
    }

    ESP_LOGD(TAG, "No valid Inkbird data found");
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_inkbird_validate_data(const ble_sensor_data_t *data)
{
    if (data == NULL) {
        return false;
    }

    /* Check temperature range */
    if (data->temperature < BLE_INKBIRD_TEMP_MIN ||
        data->temperature > BLE_INKBIRD_TEMP_MAX) {
        return false;
    }

    /* Check humidity range (if present) */
    if (data->humidity > 0 &&
        (data->humidity < BLE_INKBIRD_HUMIDITY_MIN ||
         data->humidity > BLE_INKBIRD_HUMIDITY_MAX)) {
        return false;
    }

    return true;
}

const char *ble_inkbird_get_model_name(ble_inkbird_model_t model)
{
    switch (model) {
        case BLE_INKBIRD_MODEL_IBS_TH1:
            return "IBS-TH1";
        case BLE_INKBIRD_MODEL_IBS_TH2:
            return "IBS-TH2";
        case BLE_INKBIRD_MODEL_ITH_20R:
            return "ITH-20R";
        default:
            return "Inkbird";
    }
}

/* ============================================================================
 * Private Function Implementation
 * ============================================================================ */

/**
 * @brief Find 16-bit service data in advertisement
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

        if (field_type == BLE_AD_TYPE_SERVICE_DATA_16 && field_len >= 3) {
            uint16_t found_uuid = adv_data[offset + 2] | (adv_data[offset + 3] << 8);
            if (found_uuid == uuid) {
                *out_len = field_len - 3;
                return &adv_data[offset + 4];
            }
        }

        offset += field_len + 1;
    }

    return NULL;
}

/**
 * @brief Find manufacturer data in advertisement (any manufacturer)
 */
static const uint8_t *find_manufacturer_data(const uint8_t *adv_data, size_t adv_len,
                                              size_t *out_len)
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

        if (field_type == BLE_AD_TYPE_MANUFACTURER_DATA && field_len >= 3) {
            *out_len = field_len - 1; /* Subtract type byte */
            return &adv_data[offset + 2];
        }

        offset += field_len + 1;
    }

    return NULL;
}

/**
 * @brief Find device name in advertisement
 */
static const char *find_device_name(const uint8_t *adv_data, size_t adv_len, size_t *name_len)
{
    if (adv_data == NULL || adv_len < 2 || name_len == NULL) {
        return NULL;
    }

    size_t offset = 0;
    while (offset < adv_len) {
        uint8_t field_len = adv_data[offset];
        if (field_len == 0 || offset + field_len > adv_len) {
            break;
        }

        uint8_t field_type = adv_data[offset + 1];

        if ((field_type == BLE_AD_TYPE_COMPLETE_NAME ||
             field_type == BLE_AD_TYPE_SHORT_NAME) && field_len >= 2) {
            *name_len = field_len - 1;
            return (const char *)&adv_data[offset + 2];
        }

        offset += field_len + 1;
    }

    return NULL;
}

/**
 * @brief Check if device name matches Inkbird patterns
 */
static bool is_inkbird_name(const char *name, size_t len)
{
    if (name == NULL || len < 3) {
        return false;
    }

    /* Check various Inkbird name prefixes (case-insensitive first char check) */
    if (len >= 3) {
        /* sps, tps prefixes (lowercase) */
        if ((name[0] == 's' || name[0] == 't') && name[1] == 'p' && name[2] == 's') {
            return true;
        }
    }

    if (len >= 3) {
        /* IBS prefix */
        if (name[0] == 'I' && name[1] == 'B' && name[2] == 'S') {
            return true;
        }
        /* ITH prefix */
        if (name[0] == 'I' && name[1] == 'T' && name[2] == 'H') {
            return true;
        }
    }

    /* Full "Inkbird" name check */
    if (len >= 7) {
        if (strncasecmp(name, "Inkbird", 7) == 0) {
            return true;
        }
    }

    return false;
}
