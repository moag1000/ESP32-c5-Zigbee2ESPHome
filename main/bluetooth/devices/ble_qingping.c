/**
 * @file ble_qingping.c
 * @brief Qingping BLE Sensor Parser Implementation
 *
 * Mi Beacon Frame Structure (Service Data for UUID 0xFE95):
 *   [0-1]   Service UUID (0xFE95, little-endian) - stripped by find_service_data
 *   [0-1]   Frame Control (little-endian)
 *   [2-3]   Device Type (little-endian)
 *   [4]     Frame Counter
 *   [5-10]  MAC Address (optional, if Frame Control bit 4 set)
 *   [...]   Capability (optional, if Frame Control bit 5 set)
 *   [...]   Event Objects (if Frame Control bit 6 set)
 *
 * Mi Beacon Object Structure:
 *   [0-1]   Object Type (little-endian)
 *   [2]     Object Length
 *   [3+]    Object Data
 *
 * Object Types:
 *   0x1004: Temperature (2 bytes, 0.1°C, little-endian signed)
 *   0x1006: Humidity (2 bytes, 0.1%, little-endian unsigned)
 *   0x100A: Battery (1 byte, percentage)
 *   0x100D: Temp+Humidity (4 bytes: temp 2 bytes + humidity 2 bytes)
 *   0x1008: Pressure (3 bytes, 0.1 hPa, little-endian)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_qingping.h"
#include "../ble_constants.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BLE_QINGPING";

/* ============================================================================
 * Private Constants
 * ============================================================================ */

/** @brief BLE AD type for 16-bit service data */
#define BLE_AD_TYPE_SERVICE_DATA_16     0x16

/** @brief Minimum service data length for Mi Beacon */
#define MIN_MI_BEACON_LEN               5

/** @brief Mi Beacon object header size (type + length) */
#define MI_OBJECT_HEADER_SIZE           3

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static const uint8_t *find_service_data(const uint8_t *adv_data, size_t adv_len,
                                         uint16_t uuid, size_t *out_len);
static ble_qingping_model_t device_type_to_model(uint16_t device_type);
static esp_err_t parse_mi_objects(const uint8_t *data, size_t len, ble_qingping_data_t *out);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

ble_reg_device_type_t ble_qingping_detect(const ble_registry_adv_data_t *adv)
{
    BLE_VALIDATE_ADV_OR_RETURN_UNKNOWN(adv);

    /* Look for Mi Beacon service data */
    size_t svc_len = 0;
    const uint8_t *svc_data = find_service_data(adv->adv_data, adv->adv_len,
                                                 BLE_QINGPING_SERVICE_UUID, &svc_len);

    if (svc_data == NULL || svc_len < MIN_MI_BEACON_LEN) {
        return BLE_REG_DEVICE_TYPE_UNKNOWN;
    }

    /* Extract device type from Mi Beacon header */
    uint16_t device_type = svc_data[2] | (svc_data[3] << 8);
    ble_qingping_model_t model = device_type_to_model(device_type);

    if (model != BLE_QINGPING_MODEL_UNKNOWN) {
        ESP_LOGD(TAG, "Detected Qingping %s (device type: 0x%04X, len: %d)",
                 ble_qingping_get_model_name(model), device_type, svc_len);
        return BLE_REG_DEVICE_TYPE_QINGPING;
    }

    return BLE_REG_DEVICE_TYPE_UNKNOWN;
}

esp_err_t ble_qingping_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse extended data first */
    ble_qingping_data_t qp_data;
    esp_err_t ret = ble_qingping_parse_extended(adv, &qp_data);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Map to standard sensor data structure */
    memset(out_data, 0, sizeof(ble_parsed_data_t));
    ble_sensor_data_t *sensor = &out_data->sensor;

    sensor->temperature = qp_data.temperature;
    sensor->humidity = qp_data.humidity;
    sensor->battery = qp_data.battery;
    sensor->rssi = adv->rssi;
    sensor->last_seen = adv->timestamp;

    /* Validate standard sensor data */
    if (!ble_qingping_validate_data(sensor)) {
        ESP_LOGW(TAG, "Data validation failed: T=%.2f H=%.2f",
                 sensor->temperature, sensor->humidity);
        return ESP_ERR_INVALID_RESPONSE;
    }

    sensor->valid = true;

    ESP_LOGD(TAG, "Parsed %s: T=%.2f C, H=%.1f%%, Batt=%d%%",
             ble_qingping_get_model_name(qp_data.model),
             sensor->temperature, sensor->humidity, sensor->battery);

    return ESP_OK;
}

esp_err_t ble_qingping_parse_extended(const ble_registry_adv_data_t *adv, ble_qingping_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (adv->adv_data == NULL || adv->adv_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find Mi Beacon service data */
    size_t svc_len = 0;
    const uint8_t *svc_data = find_service_data(adv->adv_data, adv->adv_len,
                                                 BLE_QINGPING_SERVICE_UUID, &svc_len);

    if (svc_data == NULL || svc_len < MIN_MI_BEACON_LEN) {
        ESP_LOGD(TAG, "No Mi Beacon service data found");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Initialize output */
    memset(out_data, 0, sizeof(ble_qingping_data_t));

    /* Parse Mi Beacon header */
    uint16_t frame_ctrl = svc_data[0] | (svc_data[1] << 8);
    uint16_t device_type = svc_data[2] | (svc_data[3] << 8);
    out_data->frame_counter = svc_data[4];
    out_data->model = device_type_to_model(device_type);

    /* Check if encrypted */
    if (frame_ctrl & BLE_MI_FRAME_CTRL_ENCRYPTED) {
        ESP_LOGD(TAG, "Encrypted Mi Beacon data (not supported)");
        out_data->encrypted = true;
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Calculate object data offset */
    size_t offset = 5;  /* After frame_ctrl (2) + device_type (2) + counter (1) */

    /* Skip MAC address if present */
    if (frame_ctrl & BLE_MI_FRAME_CTRL_HAS_MAC) {
        offset += 6;
    }

    /* Skip capability if present */
    if (frame_ctrl & BLE_MI_FRAME_CTRL_HAS_CAPABILITY) {
        if (offset < svc_len) {
            offset += 1;  /* Capability is 1 byte */
        }
    }

    /* Check if we have event/object data */
    if (!(frame_ctrl & BLE_MI_FRAME_CTRL_HAS_EVENT)) {
        ESP_LOGD(TAG, "No event data in Mi Beacon");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (offset >= svc_len) {
        ESP_LOGW(TAG, "Mi Beacon data truncated");
        return ESP_ERR_INVALID_SIZE;
    }

    /* Parse Mi Beacon objects */
    esp_err_t ret = parse_mi_objects(&svc_data[offset], svc_len - offset, out_data);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Require at least temperature or humidity */
    if (!out_data->has_temperature && !out_data->has_humidity) {
        ESP_LOGD(TAG, "No temperature or humidity data found");
        return ESP_ERR_NOT_SUPPORTED;
    }

    out_data->valid = true;
    return ESP_OK;
}

bool ble_qingping_validate_data(const ble_sensor_data_t *data)
{
    if (data == NULL) {
        return false;
    }

    /* Check temperature range */
    if (data->temperature < BLE_QINGPING_TEMP_MIN ||
        data->temperature > BLE_QINGPING_TEMP_MAX) {
        return false;
    }

    /* Check humidity range */
    if (data->humidity < BLE_QINGPING_HUMIDITY_MIN ||
        data->humidity > BLE_QINGPING_HUMIDITY_MAX) {
        return false;
    }

    /* Battery should be 0-100% */
    if (data->battery > BLE_BATTERY_PERCENTAGE_MAX) {
        return false;
    }

    return true;
}

const char *ble_qingping_get_model_name(ble_qingping_model_t model)
{
    switch (model) {
        case BLE_QINGPING_MODEL_CGG1:
            return "CGG1";
        case BLE_QINGPING_MODEL_CGG1_ENC:
            return "CGG1 (enc)";
        case BLE_QINGPING_MODEL_CGDK2:
            return "CGDK2";
        case BLE_QINGPING_MODEL_CGP1S:
            return "CGP1S";
        default:
            return "unknown";
    }
}

ble_qingping_model_t ble_qingping_get_model(const ble_registry_adv_data_t *adv)
{
    if (adv == NULL || adv->adv_data == NULL) {
        return BLE_QINGPING_MODEL_UNKNOWN;
    }

    size_t svc_len = 0;
    const uint8_t *svc_data = find_service_data(adv->adv_data, adv->adv_len,
                                                 BLE_QINGPING_SERVICE_UUID, &svc_len);

    if (svc_data == NULL || svc_len < MIN_MI_BEACON_LEN) {
        return BLE_QINGPING_MODEL_UNKNOWN;
    }

    uint16_t device_type = svc_data[2] | (svc_data[3] << 8);
    return device_type_to_model(device_type);
}

/* ============================================================================
 * Private Function Implementation
 * ============================================================================ */

/**
 * @brief Find 16-bit service data in advertisement
 *
 * @param[in] adv_data Advertisement data
 * @param[in] adv_len Advertisement data length
 * @param[in] uuid 16-bit service UUID to find
 * @param[out] out_len Output data length (excluding UUID)
 * @return Pointer to data after UUID, or NULL if not found
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
 * @brief Convert Mi Beacon device type to Qingping model
 *
 * @param[in] device_type Mi Beacon device type ID
 * @return Qingping model enumeration
 */
static ble_qingping_model_t device_type_to_model(uint16_t device_type)
{
    switch (device_type) {
        case BLE_QINGPING_DEVICE_TYPE_CGG1:
            return BLE_QINGPING_MODEL_CGG1;
        case BLE_QINGPING_DEVICE_TYPE_CGG1_ENC:
            return BLE_QINGPING_MODEL_CGG1_ENC;
        case BLE_QINGPING_DEVICE_TYPE_CGDK2:
            return BLE_QINGPING_MODEL_CGDK2;
        case BLE_QINGPING_DEVICE_TYPE_CGP1S:
            return BLE_QINGPING_MODEL_CGP1S;
        default:
            return BLE_QINGPING_MODEL_UNKNOWN;
    }
}

/**
 * @brief Parse Mi Beacon objects from data
 *
 * @param[in] data Object data
 * @param[in] len Data length
 * @param[out] out Parsed Qingping data
 * @return ESP_OK on success
 */
static esp_err_t parse_mi_objects(const uint8_t *data, size_t len, ble_qingping_data_t *out)
{
    size_t offset = 0;

    while (offset + MI_OBJECT_HEADER_SIZE <= len) {
        /* Object type (2 bytes, little-endian) */
        uint16_t obj_type = data[offset] | (data[offset + 1] << 8);
        uint8_t obj_len = data[offset + 2];
        offset += MI_OBJECT_HEADER_SIZE;

        /* Check we have enough data */
        if (offset + obj_len > len) {
            ESP_LOGW(TAG, "Object data truncated: type=0x%04X, len=%d", obj_type, obj_len);
            break;
        }

        const uint8_t *obj_data = &data[offset];

        switch (obj_type) {
            case BLE_MI_OBJECT_TEMPERATURE:
                if (obj_len >= 2) {
                    /* Temperature: signed 16-bit, 0.1°C resolution, little-endian */
                    int16_t temp_raw = (int16_t)(obj_data[0] | (obj_data[1] << 8));
                    out->temperature = temp_raw / 10.0f;
                    out->has_temperature = true;
                    ESP_LOGD(TAG, "Object: Temperature = %.1f C", out->temperature);
                }
                break;

            case BLE_MI_OBJECT_HUMIDITY:
                if (obj_len >= 2) {
                    /* Humidity: unsigned 16-bit, 0.1% resolution, little-endian */
                    uint16_t hum_raw = obj_data[0] | (obj_data[1] << 8);
                    out->humidity = hum_raw / 10.0f;
                    out->has_humidity = true;
                    ESP_LOGD(TAG, "Object: Humidity = %.1f%%", out->humidity);
                }
                break;

            case BLE_MI_OBJECT_BATTERY:
                if (obj_len >= 1) {
                    /* Battery: single byte percentage */
                    out->battery = obj_data[0];
                    out->has_battery = true;
                    ESP_LOGD(TAG, "Object: Battery = %d%%", out->battery);
                }
                break;

            case BLE_MI_OBJECT_TEMP_HUMIDITY:
                if (obj_len >= 4) {
                    /* Combined temp+humidity: temp 2 bytes + humidity 2 bytes */
                    int16_t temp_raw = (int16_t)(obj_data[0] | (obj_data[1] << 8));
                    uint16_t hum_raw = obj_data[2] | (obj_data[3] << 8);
                    out->temperature = temp_raw / 10.0f;
                    out->humidity = hum_raw / 10.0f;
                    out->has_temperature = true;
                    out->has_humidity = true;
                    ESP_LOGD(TAG, "Object: Temp+Humidity = %.1f C, %.1f%%",
                             out->temperature, out->humidity);
                }
                break;

            case BLE_MI_OBJECT_PRESSURE:
                if (obj_len >= 3) {
                    /* Pressure: 3 bytes, 0.1 hPa, little-endian (24-bit unsigned) */
                    uint32_t press_raw = obj_data[0] | (obj_data[1] << 8) | (obj_data[2] << 16);
                    out->pressure = press_raw / 10.0f;
                    out->has_pressure = true;
                    ESP_LOGD(TAG, "Object: Pressure = %.1f hPa", out->pressure);
                }
                break;

            default:
                ESP_LOGD(TAG, "Unknown Mi object: type=0x%04X, len=%d", obj_type, obj_len);
                break;
        }

        offset += obj_len;
    }

    return ESP_OK;
}
