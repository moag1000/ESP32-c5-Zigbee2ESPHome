/**
 * @file ble_switchbot.c
 * @brief SwitchBot BLE Device Parser Implementation
 *
 * SwitchBot devices use service data with UUID 0xFD3D.
 *
 * Service Data Format (after UUID):
 *   [0]     Device type byte (H=Bot, c=Curtain, T=Meter, etc.)
 *   [1+]    Device-specific data
 *
 * Meter/Meter Plus Service Data (6 bytes):
 *   [0]     Device type ('T' or 'i')
 *   [1]     Status byte (bit 7: temp sign, bit 6: temp alert, bits 0-5: reserved)
 *   [2]     Temperature integer (bits 6-0) + fractional flag (bit 7)
 *   [3]     Temperature decimal + humidity high bits
 *   [4]     Humidity (bits 7-1) + humidity decimal flag (bit 0)
 *   [5]     Battery level (0-100)
 *
 * Bot Service Data (3+ bytes):
 *   [0]     Device type ('H')
 *   [1]     Status (bit 7: mode, bit 6: state)
 *   [2]     Battery level
 *
 * Curtain Service Data (5+ bytes):
 *   [0]     Device type ('c')
 *   [1]     Status (bit 7: calibrated, bit 6: moving)
 *   [2]     Battery
 *   [3]     Position (0-100, 0=open)
 *   [4]     Light level
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_switchbot.h"
#include "../ble_constants.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BLE_SWITCHBOT";

/* ============================================================================
 * Private Constants
 * ============================================================================ */

/** @brief BLE AD type for 16-bit service data */
#define BLE_AD_TYPE_SERVICE_DATA_16     0x16

/** @brief BLE AD type for manufacturer data */
#define BLE_AD_TYPE_MANUFACTURER_DATA   0xFF

/** @brief Minimum service data length */
#define MIN_SWITCHBOT_SERVICE_DATA_LEN  2

/** @brief Meter service data length */
#define SWITCHBOT_METER_DATA_LEN        6

/** @brief Bot service data length */
#define SWITCHBOT_BOT_DATA_LEN          3

/** @brief Curtain service data length */
#define SWITCHBOT_CURTAIN_DATA_LEN      5

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static const uint8_t *find_service_data(const uint8_t *adv_data, size_t adv_len,
                                         uint16_t uuid, size_t *out_len);
static const uint8_t *find_manufacturer_data(const uint8_t *adv_data, size_t adv_len,
                                              uint16_t company_id, size_t *out_len);
static ble_switchbot_model_t device_type_to_model(uint8_t device_type);
static esp_err_t parse_meter_data(const uint8_t *data, size_t len, ble_switchbot_data_t *out);
static esp_err_t parse_bot_data(const uint8_t *data, size_t len, ble_switchbot_data_t *out);
static esp_err_t parse_curtain_data(const uint8_t *data, size_t len, ble_switchbot_data_t *out);
static esp_err_t parse_motion_data(const uint8_t *data, size_t len, ble_switchbot_data_t *out);
static esp_err_t parse_contact_data(const uint8_t *data, size_t len, ble_switchbot_data_t *out);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

ble_reg_device_type_t ble_switchbot_detect(const ble_registry_adv_data_t *adv)
{
    BLE_VALIDATE_ADV_OR_RETURN_UNKNOWN(adv);

    /* Method 1: Check for SwitchBot service UUID 0xFD3D */
    size_t svc_len = 0;
    const uint8_t *svc_data = find_service_data(adv->adv_data, adv->adv_len,
                                                 BLE_SWITCHBOT_SERVICE_UUID, &svc_len);

    if (svc_data != NULL && svc_len >= MIN_SWITCHBOT_SERVICE_DATA_LEN) {
        uint8_t device_type = svc_data[0];
        ble_switchbot_model_t model = device_type_to_model(device_type);

        if (model != BLE_SWITCHBOT_MODEL_UNKNOWN) {
            ESP_LOGD(TAG, "Detected SwitchBot %s via service data (type: 0x%02X, len: %d)",
                     ble_switchbot_get_model_name(model), device_type, svc_len);

            /* Map to registry device types */
            switch (model) {
                case BLE_SWITCHBOT_MODEL_METER:
                case BLE_SWITCHBOT_MODEL_METER_PLUS:
                    return BLE_REG_DEVICE_TYPE_SWITCHBOT_METER;
                case BLE_SWITCHBOT_MODEL_BOT:
                    return BLE_REG_DEVICE_TYPE_SWITCHBOT_BOT;
                case BLE_SWITCHBOT_MODEL_CURTAIN:
                    return BLE_REG_DEVICE_TYPE_SWITCHBOT_CURTAIN;
                default:
                    return BLE_REG_DEVICE_TYPE_SWITCHBOT_METER;  /* Default to meter */
            }
        }
    }

    /* Method 2: Check for SwitchBot manufacturer ID */
    size_t mfr_len = 0;
    const uint8_t *mfr_data = find_manufacturer_data(adv->adv_data, adv->adv_len,
                                                      BLE_SWITCHBOT_COMPANY_ID, &mfr_len);

    if (mfr_data != NULL && mfr_len >= MIN_SWITCHBOT_SERVICE_DATA_LEN) {
        ESP_LOGD(TAG, "Detected SwitchBot via manufacturer ID (len: %d)", mfr_len);
        return BLE_REG_DEVICE_TYPE_SWITCHBOT_METER;  /* Default */
    }

    return BLE_REG_DEVICE_TYPE_UNKNOWN;
}

esp_err_t ble_switchbot_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse extended data first */
    ble_switchbot_data_t sb_data;
    esp_err_t ret = ble_switchbot_parse_extended(adv, &sb_data);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Map to standard sensor data structure */
    memset(out_data, 0, sizeof(ble_parsed_data_t));
    ble_sensor_data_t *sensor = &out_data->sensor;

    sensor->battery = sb_data.battery;
    sensor->rssi = adv->rssi;
    sensor->last_seen = adv->timestamp;

    /* Map device-specific data to sensor format */
    if (sb_data.has_temperature) {
        sensor->temperature = sb_data.temperature;
        sensor->humidity = sb_data.humidity;
    }

    /* Validate sensor data if it's a meter */
    if (sb_data.model == BLE_SWITCHBOT_MODEL_METER ||
        sb_data.model == BLE_SWITCHBOT_MODEL_METER_PLUS) {
        if (!ble_switchbot_validate_data(sensor)) {
            ESP_LOGW(TAG, "Data validation failed: T=%.2f H=%.2f",
                     sensor->temperature, sensor->humidity);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    sensor->valid = true;

    ESP_LOGD(TAG, "Parsed %s: T=%.2f C, H=%.1f%%, Batt=%d%%",
             ble_switchbot_get_model_name(sb_data.model),
             sensor->temperature, sensor->humidity, sensor->battery);

    return ESP_OK;
}

esp_err_t ble_switchbot_parse_extended(const ble_registry_adv_data_t *adv, ble_switchbot_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (adv->adv_data == NULL || adv->adv_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Initialize output */
    memset(out_data, 0, sizeof(ble_switchbot_data_t));

    /* Find SwitchBot service data */
    size_t svc_len = 0;
    const uint8_t *svc_data = find_service_data(adv->adv_data, adv->adv_len,
                                                 BLE_SWITCHBOT_SERVICE_UUID, &svc_len);

    if (svc_data == NULL || svc_len < MIN_SWITCHBOT_SERVICE_DATA_LEN) {
        ESP_LOGD(TAG, "No SwitchBot service data found");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Determine device type */
    uint8_t device_type = svc_data[0];
    out_data->model = device_type_to_model(device_type);

    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;

    switch (out_data->model) {
        case BLE_SWITCHBOT_MODEL_METER:
        case BLE_SWITCHBOT_MODEL_METER_PLUS:
            ret = parse_meter_data(svc_data, svc_len, out_data);
            break;

        case BLE_SWITCHBOT_MODEL_BOT:
            ret = parse_bot_data(svc_data, svc_len, out_data);
            break;

        case BLE_SWITCHBOT_MODEL_CURTAIN:
            ret = parse_curtain_data(svc_data, svc_len, out_data);
            break;

        case BLE_SWITCHBOT_MODEL_MOTION:
            ret = parse_motion_data(svc_data, svc_len, out_data);
            break;

        case BLE_SWITCHBOT_MODEL_CONTACT:
            ret = parse_contact_data(svc_data, svc_len, out_data);
            break;

        default:
            ESP_LOGD(TAG, "Unknown SwitchBot device type: 0x%02X", device_type);
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (ret == ESP_OK) {
        out_data->valid = true;
    }

    return ret;
}

bool ble_switchbot_validate_data(const ble_sensor_data_t *data)
{
    if (data == NULL) {
        return false;
    }

    /* Check temperature range */
    if (data->temperature < BLE_SWITCHBOT_TEMP_MIN ||
        data->temperature > BLE_SWITCHBOT_TEMP_MAX) {
        return false;
    }

    /* Check humidity range */
    if (data->humidity < BLE_SWITCHBOT_HUMIDITY_MIN ||
        data->humidity > BLE_SWITCHBOT_HUMIDITY_MAX) {
        return false;
    }

    /* Battery should be 0-100% */
    if (data->battery > BLE_BATTERY_PERCENTAGE_MAX) {
        return false;
    }

    return true;
}

const char *ble_switchbot_get_model_name(ble_switchbot_model_t model)
{
    switch (model) {
        case BLE_SWITCHBOT_MODEL_BOT:
            return "Bot";
        case BLE_SWITCHBOT_MODEL_CURTAIN:
            return "Curtain";
        case BLE_SWITCHBOT_MODEL_METER:
            return "Meter";
        case BLE_SWITCHBOT_MODEL_METER_PLUS:
            return "Meter Plus";
        case BLE_SWITCHBOT_MODEL_MOTION:
            return "Motion";
        case BLE_SWITCHBOT_MODEL_CONTACT:
            return "Contact";
        case BLE_SWITCHBOT_MODEL_HUB_MINI:
            return "Hub Mini";
        case BLE_SWITCHBOT_MODEL_PLUG_MINI:
            return "Plug Mini";
        default:
            return "SwitchBot";
    }
}

ble_switchbot_model_t ble_switchbot_get_model(const ble_registry_adv_data_t *adv)
{
    if (adv == NULL || adv->adv_data == NULL) {
        return BLE_SWITCHBOT_MODEL_UNKNOWN;
    }

    size_t svc_len = 0;
    const uint8_t *svc_data = find_service_data(adv->adv_data, adv->adv_len,
                                                 BLE_SWITCHBOT_SERVICE_UUID, &svc_len);

    if (svc_data == NULL || svc_len < 1) {
        return BLE_SWITCHBOT_MODEL_UNKNOWN;
    }

    return device_type_to_model(svc_data[0]);
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
 * @brief Find manufacturer data with specific company ID
 */
static const uint8_t *find_manufacturer_data(const uint8_t *adv_data, size_t adv_len,
                                              uint16_t company_id, size_t *out_len)
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
            uint16_t found_id = adv_data[offset + 2] | (adv_data[offset + 3] << 8);
            if (found_id == company_id) {
                *out_len = field_len - 3;
                return &adv_data[offset + 4];
            }
        }

        offset += field_len + 1;
    }

    return NULL;
}

/**
 * @brief Convert device type byte to model enumeration
 */
static ble_switchbot_model_t device_type_to_model(uint8_t device_type)
{
    switch (device_type) {
        case BLE_SWITCHBOT_TYPE_BOT:
            return BLE_SWITCHBOT_MODEL_BOT;
        case BLE_SWITCHBOT_TYPE_CURTAIN:
            return BLE_SWITCHBOT_MODEL_CURTAIN;
        case BLE_SWITCHBOT_TYPE_METER:
            return BLE_SWITCHBOT_MODEL_METER;
        case BLE_SWITCHBOT_TYPE_METER_PLUS:
            return BLE_SWITCHBOT_MODEL_METER_PLUS;
        case BLE_SWITCHBOT_TYPE_MOTION:
            return BLE_SWITCHBOT_MODEL_MOTION;
        case BLE_SWITCHBOT_TYPE_CONTACT:
            return BLE_SWITCHBOT_MODEL_CONTACT;
        case BLE_SWITCHBOT_TYPE_HUB_MINI:
            return BLE_SWITCHBOT_MODEL_HUB_MINI;
        case BLE_SWITCHBOT_TYPE_PLUG_MINI:
            return BLE_SWITCHBOT_MODEL_PLUG_MINI;
        default:
            return BLE_SWITCHBOT_MODEL_UNKNOWN;
    }
}

/**
 * @brief Parse Meter/Meter Plus data
 */
static esp_err_t parse_meter_data(const uint8_t *data, size_t len, ble_switchbot_data_t *out)
{
    if (len < SWITCHBOT_METER_DATA_LEN) {
        ESP_LOGW(TAG, "Meter data too short: %d bytes", len);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Meter data format:
     * [0]   Device type ('T' or 'i')
     * [1]   Status: bit 7 = temp negative, bit 6 = temp alert
     * [2]   Temperature: bit 7 = has decimal, bits 6-0 = integer part
     * [3]   Temp decimal (upper 4 bits) + humidity bits
     * [4]   Humidity: bits 7-1 = value, bit 0 = has decimal
     * [5]   Battery percentage
     */

    bool temp_negative = (data[1] & 0x80) != 0;
    bool temp_has_decimal = (data[2] & 0x80) != 0;
    uint8_t temp_int = data[2] & 0x7F;
    uint8_t temp_dec = (data[3] >> 4) & 0x0F;

    out->temperature = (float)temp_int;
    if (temp_has_decimal) {
        out->temperature += temp_dec / BLE_SWITCHBOT_TEMP_DECIMAL_STEP;
    }
    if (temp_negative) {
        out->temperature = -out->temperature;
    }

    uint8_t humidity_int = (data[4] >> 1) & 0x7F;
    bool humidity_has_decimal = (data[4] & 0x01) != 0;
    out->humidity = (float)humidity_int;
    if (humidity_has_decimal) {
        out->humidity += BLE_SWITCHBOT_HUM_DECIMAL_STEP;  /* Humidity decimal is 0.5 */
    }

    out->battery = data[5];
    out->has_temperature = true;

    ESP_LOGD(TAG, "Meter: T=%.1f C, H=%.1f%%, Batt=%d%%",
             out->temperature, out->humidity, out->battery);

    return ESP_OK;
}

/**
 * @brief Parse Bot data
 */
static esp_err_t parse_bot_data(const uint8_t *data, size_t len, ble_switchbot_data_t *out)
{
    if (len < SWITCHBOT_BOT_DATA_LEN) {
        ESP_LOGW(TAG, "Bot data too short: %d bytes", len);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Bot data format:
     * [0]   Device type ('H')
     * [1]   Status: bit 7 = switch mode, bit 6 = state (on/off)
     * [2]   Battery percentage
     */

    out->bot_mode = (data[1] & 0x80) ? BLE_SWITCHBOT_BOT_MODE_SWITCH : BLE_SWITCHBOT_BOT_MODE_PRESS;
    out->bot_state = (data[1] & 0x40) != 0;
    out->battery = data[2] & 0x7F;  /* Lower 7 bits */
    out->has_bot_state = true;

    ESP_LOGD(TAG, "Bot: state=%s, mode=%s, batt=%d%%",
             out->bot_state ? "ON" : "OFF",
             out->bot_mode == BLE_SWITCHBOT_BOT_MODE_SWITCH ? "switch" : "press",
             out->battery);

    return ESP_OK;
}

/**
 * @brief Parse Curtain data
 */
static esp_err_t parse_curtain_data(const uint8_t *data, size_t len, ble_switchbot_data_t *out)
{
    if (len < SWITCHBOT_CURTAIN_DATA_LEN) {
        ESP_LOGW(TAG, "Curtain data too short: %d bytes", len);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Curtain data format:
     * [0]   Device type ('c')
     * [1]   Status: bit 7 = calibrated, bit 6 = moving
     * [2]   Battery percentage
     * [3]   Position (0-100, 0=fully open)
     * [4]   Light level
     */

    out->curtain_calibrated = (data[1] & 0x80) != 0;
    out->curtain_moving = (data[1] & 0x40) != 0;
    out->battery = data[2] & 0x7F;
    out->curtain_position = data[3] & 0x7F;  /* 0-100 */
    out->has_curtain_position = true;

    ESP_LOGD(TAG, "Curtain: pos=%d%%, moving=%d, calibrated=%d, batt=%d%%",
             out->curtain_position, out->curtain_moving,
             out->curtain_calibrated, out->battery);

    return ESP_OK;
}

/**
 * @brief Parse Motion sensor data
 */
static esp_err_t parse_motion_data(const uint8_t *data, size_t len, ble_switchbot_data_t *out)
{
    if (len < 3) {
        ESP_LOGW(TAG, "Motion data too short: %d bytes", len);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Motion data format:
     * [0]   Device type ('s')
     * [1]   Status: bit 7 = motion detected
     * [2]   Battery + brightness info
     */

    out->motion_detected = (data[1] & 0x80) != 0;
    out->battery = data[2] & 0x7F;
    out->motion_brightness = (data[1] >> 4) & 0x07;
    out->has_motion = true;

    ESP_LOGD(TAG, "Motion: detected=%d, brightness=%d, batt=%d%%",
             out->motion_detected, out->motion_brightness, out->battery);

    return ESP_OK;
}

/**
 * @brief Parse Contact sensor data
 */
static esp_err_t parse_contact_data(const uint8_t *data, size_t len, ble_switchbot_data_t *out)
{
    if (len < 3) {
        ESP_LOGW(TAG, "Contact data too short: %d bytes", len);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Contact data format:
     * [0]   Device type ('d')
     * [1]   Status: bit 7 = timeout, bit 6 = hall effect state (open/closed)
     * [2]   Battery percentage
     */

    out->contact_timeout = (data[1] & 0x80) != 0;
    out->contact_open = (data[1] & 0x40) != 0;
    out->battery = data[2] & 0x7F;
    out->has_contact = true;

    ESP_LOGD(TAG, "Contact: open=%d, timeout=%d, batt=%d%%",
             out->contact_open, out->contact_timeout, out->battery);

    return ESP_OK;
}
