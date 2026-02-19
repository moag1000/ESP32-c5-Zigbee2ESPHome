/**
 * @file ble_beacon.c
 * @brief iBeacon and Eddystone BLE Beacon Parser Implementation
 *
 * iBeacon Format (Manufacturer Specific Data):
 *   [0-1]  Apple Company ID (0x004C, little-endian)
 *   [2]    iBeacon type (0x02)
 *   [3]    Data length (0x15 = 21 bytes)
 *   [4-19] Proximity UUID (16 bytes, big-endian)
 *   [20-21] Major value (big-endian)
 *   [22-23] Minor value (big-endian)
 *   [24]   TX Power (signed int8, calibrated at 1m)
 *
 * Eddystone-UID Format (Service Data for UUID 0xFEAA):
 *   [0]    Frame type (0x00 for UID)
 *   [1]    TX Power (signed int8)
 *   [2-11] Namespace (10 bytes)
 *   [12-17] Instance (6 bytes)
 *   [18-19] RFU (reserved)
 *
 * Eddystone-TLM Format (Service Data for UUID 0xFEAA):
 *   [0]    Frame type (0x20 for TLM)
 *   [1]    Version (0x00)
 *   [2-3]  Battery voltage (big-endian, mV)
 *   [4-5]  Beacon temperature (big-endian, 8.8 fixed point)
 *   [6-9]  Advertisement count (big-endian)
 *   [10-13] Time since boot (big-endian, 100ms units)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_beacon.h"
#include "../ble_constants.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "BLE_BEACON";

/* ============================================================================
 * Private Types and Constants
 * ============================================================================ */

/** @brief BLE AD type for manufacturer specific data */
#define BLE_AD_TYPE_MANUFACTURER_DATA   0xFF

/** @brief BLE AD type for 16-bit service data */
#define BLE_AD_TYPE_SERVICE_DATA_16     0x16

/** @brief iBeacon total manufacturer data length (including company ID) */
#define IBEACON_MFR_DATA_LEN            25

/** @brief Eddystone-UID service data length (excluding UUID) */
#define EDDYSTONE_UID_DATA_LEN          18

/** @brief Eddystone-TLM service data length (excluding UUID) */
#define EDDYSTONE_TLM_DATA_LEN          12

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static const uint8_t *find_manufacturer_data(const uint8_t *adv_data, size_t adv_len,
                                              uint16_t manufacturer_id, size_t *out_len);
static const uint8_t *find_service_data(const uint8_t *adv_data, size_t adv_len,
                                         uint16_t uuid, size_t *out_len);
static ble_eddystone_type_t get_eddystone_frame_type(const uint8_t *service_data, size_t len);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

ble_reg_device_type_t ble_ibeacon_detect(const ble_registry_adv_data_t *adv)
{
    BLE_VALIDATE_ADV_OR_RETURN_UNKNOWN(adv);

    /* Look for Apple manufacturer data */
    size_t mfr_len = 0;
    const uint8_t *mfr_data = find_manufacturer_data(adv->adv_data, adv->adv_len,
                                                      BLE_IBEACON_MANUFACTURER_ID, &mfr_len);

    if (mfr_data == NULL || mfr_len < IBEACON_MFR_DATA_LEN - 2) {
        return BLE_REG_DEVICE_TYPE_UNKNOWN;
    }

    /* Check iBeacon type and length indicators */
    if (mfr_data[0] == BLE_IBEACON_TYPE && mfr_data[1] == BLE_IBEACON_DATA_LEN) {
        ESP_LOGD(TAG, "Detected iBeacon");
        return BLE_REG_DEVICE_TYPE_IBEACON;
    }

    return BLE_REG_DEVICE_TYPE_UNKNOWN;
}

ble_reg_device_type_t ble_eddystone_detect(const ble_registry_adv_data_t *adv)
{
    BLE_VALIDATE_ADV_OR_RETURN_UNKNOWN(adv);

    /* Look for Eddystone service data */
    size_t service_len = 0;
    const uint8_t *service_data = find_service_data(adv->adv_data, adv->adv_len,
                                                     BLE_EDDYSTONE_SERVICE_UUID, &service_len);

    if (service_data == NULL || service_len < 2) {
        return BLE_REG_DEVICE_TYPE_UNKNOWN;
    }

    /* Determine frame type */
    ble_eddystone_type_t frame_type = get_eddystone_frame_type(service_data, service_len);

    switch (frame_type) {
        case BLE_EDDYSTONE_TYPE_UID:
            ESP_LOGD(TAG, "Detected Eddystone-UID");
            return BLE_REG_DEVICE_TYPE_EDDYSTONE_UID;
        case BLE_EDDYSTONE_TYPE_TLM:
            ESP_LOGD(TAG, "Detected Eddystone-TLM");
            return BLE_REG_DEVICE_TYPE_EDDYSTONE_TLM;
        default:
            return BLE_REG_DEVICE_TYPE_UNKNOWN;
    }
}

ble_reg_device_type_t ble_beacon_detect(const ble_registry_adv_data_t *adv)
{
    ble_reg_device_type_t type;

    /* Try iBeacon first */
    type = ble_ibeacon_detect(adv);
    if (type != BLE_REG_DEVICE_TYPE_UNKNOWN) {
        return type;
    }

    /* Try Eddystone */
    type = ble_eddystone_detect(adv);
    if (type != BLE_REG_DEVICE_TYPE_UNKNOWN) {
        return type;
    }

    return BLE_REG_DEVICE_TYPE_UNKNOWN;
}

esp_err_t ble_ibeacon_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find Apple manufacturer data */
    size_t mfr_len = 0;
    const uint8_t *mfr_data = find_manufacturer_data(adv->adv_data, adv->adv_len,
                                                      BLE_IBEACON_MANUFACTURER_ID, &mfr_len);

    if (mfr_data == NULL || mfr_len < IBEACON_MFR_DATA_LEN - 2) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Verify iBeacon header */
    if (mfr_data[0] != BLE_IBEACON_TYPE || mfr_data[1] != BLE_IBEACON_DATA_LEN) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Initialize output */
    memset(out_data, 0, sizeof(ble_parsed_data_t));
    ble_beacon_data_t *beacon = &out_data->beacon;

    /*
     * Parse iBeacon data (after type and length bytes):
     * [2-17]  Proximity UUID (16 bytes)
     * [18-19] Major (big-endian)
     * [20-21] Minor (big-endian)
     * [22]    TX Power (signed)
     */

    /* Copy UUID */
    memcpy(beacon->uuid, &mfr_data[2], 16);

    /* Major (big-endian) */
    beacon->major = (mfr_data[18] << 8) | mfr_data[19];

    /* Minor (big-endian) */
    beacon->minor = (mfr_data[20] << 8) | mfr_data[21];

    /* TX Power (signed int8) */
    beacon->tx_power = (int8_t)mfr_data[22];

    /* Current RSSI */
    beacon->rssi = adv->rssi;

    /* Calculate estimated distance */
    beacon->distance = ble_beacon_estimate_distance(adv->rssi, beacon->tx_power);

    /* Determine presence */
    beacon->present = (adv->rssi >= BLE_BEACON_PRESENCE_RSSI_THRESHOLD);

    /* Timestamp */
    beacon->last_seen = adv->timestamp;

    char uuid_str[37];
    ble_ibeacon_uuid_to_string(beacon->uuid, uuid_str, sizeof(uuid_str));
    ESP_LOGD(TAG, "iBeacon: UUID=%s Major=%d Minor=%d TxPwr=%d RSSI=%d Dist=%.2fm",
             uuid_str, beacon->major, beacon->minor, beacon->tx_power,
             beacon->rssi, beacon->distance);

    return ESP_OK;
}

esp_err_t ble_eddystone_uid_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find Eddystone service data */
    size_t service_len = 0;
    const uint8_t *service_data = find_service_data(adv->adv_data, adv->adv_len,
                                                     BLE_EDDYSTONE_SERVICE_UUID, &service_len);

    if (service_data == NULL || service_len < EDDYSTONE_UID_DATA_LEN) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Verify UID frame type */
    if (service_data[0] != BLE_EDDYSTONE_FRAME_UID) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Initialize output */
    memset(out_data, 0, sizeof(ble_parsed_data_t));
    ble_beacon_data_t *beacon = &out_data->beacon;

    /*
     * Parse Eddystone-UID:
     * [0]     Frame type (0x00)
     * [1]     TX Power
     * [2-11]  Namespace (10 bytes)
     * [12-17] Instance (6 bytes)
     */

    /* TX Power */
    beacon->tx_power = (int8_t)service_data[1];

    /* Copy Namespace to first 10 bytes of UUID field */
    memcpy(beacon->uuid, &service_data[2], BLE_EDDYSTONE_NAMESPACE_LEN);

    /* Store Instance in remaining 6 bytes */
    memcpy(&beacon->uuid[10], &service_data[12], BLE_EDDYSTONE_INSTANCE_LEN);

    /* Use minor to store instance as 16-bit value (last 2 bytes) */
    beacon->minor = (service_data[16] << 8) | service_data[17];

    /* Current RSSI */
    beacon->rssi = adv->rssi;

    /* Calculate distance */
    beacon->distance = ble_beacon_estimate_distance(adv->rssi, beacon->tx_power);

    /* Presence */
    beacon->present = (adv->rssi >= BLE_BEACON_PRESENCE_RSSI_THRESHOLD);

    /* Timestamp */
    beacon->last_seen = adv->timestamp;

    char ns_str[21];
    ble_eddystone_namespace_to_string(beacon->uuid, ns_str, sizeof(ns_str));
    ESP_LOGD(TAG, "Eddystone-UID: NS=%s TxPwr=%d RSSI=%d Dist=%.2fm",
             ns_str, beacon->tx_power, beacon->rssi, beacon->distance);

    return ESP_OK;
}

esp_err_t ble_eddystone_tlm_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find Eddystone service data */
    size_t service_len = 0;
    const uint8_t *service_data = find_service_data(adv->adv_data, adv->adv_len,
                                                     BLE_EDDYSTONE_SERVICE_UUID, &service_len);

    if (service_data == NULL || service_len < EDDYSTONE_TLM_DATA_LEN) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Verify TLM frame type */
    if (service_data[0] != BLE_EDDYSTONE_FRAME_TLM) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Initialize output */
    memset(out_data, 0, sizeof(ble_parsed_data_t));
    ble_eddystone_tlm_t *tlm = &out_data->tlm;

    /*
     * Parse Eddystone-TLM:
     * [0]     Frame type (0x20)
     * [1]     Version
     * [2-3]   Battery voltage (big-endian, mV)
     * [4-5]   Temperature (big-endian, 8.8 fixed point)
     * [6-9]   Advertisement count (big-endian)
     * [10-13] Uptime (big-endian, 100ms units)
     */

    /* Battery voltage (mV) */
    tlm->battery_mv = (service_data[2] << 8) | service_data[3];

    /* Temperature (8.8 fixed point: integer part in [4], fraction in [5]) */
    int8_t temp_int = (int8_t)service_data[4];
    uint8_t temp_frac = service_data[5];
    tlm->temperature = temp_int + (temp_frac / BLE_BEACON_TEMP_FRAC_DIVISOR);

    /* Advertisement count */
    tlm->adv_count = ((uint32_t)service_data[6] << 24) |
                     ((uint32_t)service_data[7] << 16) |
                     ((uint32_t)service_data[8] << 8) |
                     (uint32_t)service_data[9];

    /* Uptime in 100ms units */
    tlm->uptime_100ms = ((uint32_t)service_data[10] << 24) |
                        ((uint32_t)service_data[11] << 16) |
                        ((uint32_t)service_data[12] << 8) |
                        (uint32_t)service_data[13];

    /* Timestamp */
    tlm->last_seen = adv->timestamp;

    ESP_LOGD(TAG, "Eddystone-TLM: Batt=%dmV Temp=%.1fC AdvCnt=%lu Uptime=%lus",
             tlm->battery_mv, tlm->temperature,
             (unsigned long)tlm->adv_count,
             (unsigned long)(tlm->uptime_100ms / 10));

    return ESP_OK;
}

esp_err_t ble_beacon_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data)
{
    if (adv == NULL || out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Try iBeacon */
    if (ble_ibeacon_detect(adv) == BLE_REG_DEVICE_TYPE_IBEACON) {
        return ble_ibeacon_parse(adv, out_data);
    }

    /* Try Eddystone */
    ble_reg_device_type_t type = ble_eddystone_detect(adv);
    if (type == BLE_REG_DEVICE_TYPE_EDDYSTONE_UID) {
        return ble_eddystone_uid_parse(adv, out_data);
    } else if (type == BLE_REG_DEVICE_TYPE_EDDYSTONE_TLM) {
        return ble_eddystone_tlm_parse(adv, out_data);
    }

    return ESP_ERR_NOT_SUPPORTED;
}

float ble_beacon_estimate_distance(int8_t rssi, int8_t tx_power)
{
    if (rssi == 0) {
        return -1.0f; /* Unknown */
    }

    /*
     * Log-distance path loss model:
     * RSSI = TxPower - 10 * n * log10(d)
     * Solving for d:
     * d = 10 ^ ((TxPower - RSSI) / (10 * n))
     *
     * Where n is the path loss exponent (typically 2.0 for free space,
     * 2.0-4.0 for indoor environments)
     */
    float ratio = (float)(tx_power - rssi) / (10.0f * BLE_BEACON_PATH_LOSS_EXPONENT);
    float distance = powf(10.0f, ratio);

    /* Clamp to reasonable range */
    if (distance < 0.0f) {
        distance = 0.0f;
    } else if (distance > BLE_BEACON_DISTANCE_MAX) {
        distance = BLE_BEACON_DISTANCE_MAX;
    }

    return distance;
}

ble_beacon_presence_t ble_beacon_get_presence(int8_t rssi)
{
    if (rssi >= BLE_RSSI_IMMEDIATE_THRESHOLD) {
        return BLE_BEACON_PRESENCE_IMMEDIATE;
    } else if (rssi >= BLE_RSSI_NEAR_THRESHOLD) {
        return BLE_BEACON_PRESENCE_NEAR;
    } else if (rssi >= BLE_RSSI_FAR_THRESHOLD) {
        return BLE_BEACON_PRESENCE_AWAY;
    } else {
        return BLE_BEACON_PRESENCE_UNKNOWN;
    }
}

const char *ble_beacon_presence_to_string(ble_beacon_presence_t presence)
{
    switch (presence) {
        case BLE_BEACON_PRESENCE_IMMEDIATE: return "immediate";
        case BLE_BEACON_PRESENCE_NEAR:      return "near";
        case BLE_BEACON_PRESENCE_AWAY:      return "away";
        case BLE_BEACON_PRESENCE_UNKNOWN:
        default:                             return "unknown";
    }
}

char *ble_ibeacon_uuid_to_string(const uint8_t uuid[16], char *buf, size_t buf_len)
{
    if (uuid == NULL || buf == NULL || buf_len < BLE_BEACON_UUID_BUFFER_SIZE) {
        return NULL;
    }

    /* Format: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX */
    snprintf(buf, buf_len,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5],
             uuid[6], uuid[7],
             uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);

    return buf;
}

char *ble_eddystone_namespace_to_string(const uint8_t namespace[10], char *buf, size_t buf_len)
{
    if (namespace == NULL || buf == NULL || buf_len < BLE_BEACON_NAMESPACE_BUFFER_SIZE) {
        return NULL;
    }

    /* Format: 20 hex characters */
    snprintf(buf, buf_len, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             namespace[0], namespace[1], namespace[2], namespace[3], namespace[4],
             namespace[5], namespace[6], namespace[7], namespace[8], namespace[9]);

    return buf;
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
 * @brief Get Eddystone frame type from service data
 *
 * @param[in] service_data Service data (after UUID)
 * @param[in] len Service data length
 * @return Eddystone frame type
 */
static ble_eddystone_type_t get_eddystone_frame_type(const uint8_t *service_data, size_t len)
{
    if (service_data == NULL || len < 1) {
        return BLE_EDDYSTONE_TYPE_UNKNOWN;
    }

    switch (service_data[0]) {
        case BLE_EDDYSTONE_FRAME_UID:
            if (len >= EDDYSTONE_UID_DATA_LEN) {
                return BLE_EDDYSTONE_TYPE_UID;
            }
            break;
        case BLE_EDDYSTONE_FRAME_URL:
            return BLE_EDDYSTONE_TYPE_URL;
        case BLE_EDDYSTONE_FRAME_TLM:
            if (len >= EDDYSTONE_TLM_DATA_LEN) {
                return BLE_EDDYSTONE_TYPE_TLM;
            }
            break;
        case BLE_EDDYSTONE_FRAME_EID:
            return BLE_EDDYSTONE_TYPE_EID;
    }

    return BLE_EDDYSTONE_TYPE_UNKNOWN;
}
