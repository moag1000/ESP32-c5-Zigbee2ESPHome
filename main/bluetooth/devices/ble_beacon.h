/**
 * @file ble_beacon.h
 * @brief iBeacon and Eddystone BLE Beacon Parser
 *
 * Parses BLE advertisements from iBeacon and Eddystone beacons.
 * Supports distance estimation and presence detection.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_BEACON_H
#define BLE_BEACON_H

#include "ble_device_registry.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Apple iBeacon manufacturer ID */
#define BLE_IBEACON_MANUFACTURER_ID     0x004C

/** @brief iBeacon type indicator */
#define BLE_IBEACON_TYPE                0x02

/** @brief iBeacon data length indicator */
#define BLE_IBEACON_DATA_LEN            0x15

/** @brief Eddystone Service UUID */
#define BLE_EDDYSTONE_SERVICE_UUID      0xFEAA

/** @brief Eddystone-UID frame type */
#define BLE_EDDYSTONE_FRAME_UID         0x00

/** @brief Eddystone-URL frame type */
#define BLE_EDDYSTONE_FRAME_URL         0x10

/** @brief Eddystone-TLM frame type */
#define BLE_EDDYSTONE_FRAME_TLM         0x20

/** @brief Eddystone-EID frame type */
#define BLE_EDDYSTONE_FRAME_EID         0x30

/** @brief Eddystone UID Namespace length */
#define BLE_EDDYSTONE_NAMESPACE_LEN     10

/** @brief Eddystone UID Instance length */
#define BLE_EDDYSTONE_INSTANCE_LEN      6

/** @brief RSSI threshold for presence detection (dBm) */
#define BLE_BEACON_PRESENCE_RSSI_THRESHOLD  (-85)

/** @brief Path loss exponent for indoor environments */
#define BLE_BEACON_PATH_LOSS_EXPONENT   2.0f

/** @brief Reference distance for TX power calibration (1 meter) */
#define BLE_BEACON_REFERENCE_DISTANCE   1.0f

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief Eddystone frame type enumeration
 */
typedef enum {
    BLE_EDDYSTONE_TYPE_UID = 0,     /**< UID frame */
    BLE_EDDYSTONE_TYPE_URL,         /**< URL frame */
    BLE_EDDYSTONE_TYPE_TLM,         /**< TLM (telemetry) frame */
    BLE_EDDYSTONE_TYPE_EID,         /**< EID (encrypted ID) frame */
    BLE_EDDYSTONE_TYPE_UNKNOWN
} ble_eddystone_type_t;

/**
 * @brief Beacon presence state
 */
typedef enum {
    BLE_BEACON_PRESENCE_UNKNOWN = 0,
    BLE_BEACON_PRESENCE_AWAY,
    BLE_BEACON_PRESENCE_NEAR,
    BLE_BEACON_PRESENCE_IMMEDIATE
} ble_beacon_presence_t;

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Detect if advertisement is an iBeacon
 *
 * @param[in] adv Advertisement data to analyze
 * @return
 *      - BLE_REG_DEVICE_TYPE_IBEACON if detected
 *      - BLE_REG_DEVICE_TYPE_UNKNOWN otherwise
 */
ble_reg_device_type_t ble_ibeacon_detect(const ble_registry_adv_data_t *adv);

/**
 * @brief Detect if advertisement is an Eddystone beacon
 *
 * @param[in] adv Advertisement data to analyze
 * @return
 *      - BLE_REG_DEVICE_TYPE_EDDYSTONE_UID for UID frames
 *      - BLE_REG_DEVICE_TYPE_EDDYSTONE_TLM for TLM frames
 *      - BLE_REG_DEVICE_TYPE_UNKNOWN otherwise
 */
ble_reg_device_type_t ble_eddystone_detect(const ble_registry_adv_data_t *adv);

/**
 * @brief Detect any beacon type
 *
 * @param[in] adv Advertisement data to analyze
 * @return Detected beacon type or BLE_REG_DEVICE_TYPE_UNKNOWN
 */
ble_reg_device_type_t ble_beacon_detect(const ble_registry_adv_data_t *adv);

/**
 * @brief Parse iBeacon advertisement data
 *
 * Extracts UUID, Major, Minor, TX Power, and calculates distance.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed beacon data
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if not an iBeacon
 */
esp_err_t ble_ibeacon_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data);

/**
 * @brief Parse Eddystone-UID advertisement data
 *
 * Extracts Namespace (10 bytes) and Instance (6 bytes).
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed beacon data
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if not an Eddystone-UID
 */
esp_err_t ble_eddystone_uid_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data);

/**
 * @brief Parse Eddystone-TLM advertisement data
 *
 * Extracts battery voltage, temperature, advertisement count, and uptime.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed TLM data
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if not an Eddystone-TLM
 */
esp_err_t ble_eddystone_tlm_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data);

/**
 * @brief Generic beacon parser (auto-detects type)
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed data
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_beacon_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data);

/**
 * @brief Estimate distance from beacon based on RSSI
 *
 * Uses log-distance path loss model:
 * distance = 10 ^ ((txPower - rssi) / (10 * n))
 *
 * @param[in] rssi Measured RSSI in dBm
 * @param[in] tx_power Calibrated TX power at 1m in dBm
 * @return Estimated distance in meters
 */
float ble_beacon_estimate_distance(int8_t rssi, int8_t tx_power);

/**
 * @brief Determine presence state based on RSSI
 *
 * @param[in] rssi Measured RSSI in dBm
 * @return Presence state (IMMEDIATE, NEAR, AWAY, or UNKNOWN)
 */
ble_beacon_presence_t ble_beacon_get_presence(int8_t rssi);

/**
 * @brief Get presence state as string
 *
 * @param[in] presence Presence state
 * @return Human-readable presence string
 */
const char *ble_beacon_presence_to_string(ble_beacon_presence_t presence);

/**
 * @brief Format iBeacon UUID as string
 *
 * @param[in] uuid 16-byte UUID
 * @param[out] buf Output buffer (minimum 37 bytes)
 * @param[in] buf_len Buffer length
 * @return Pointer to buf on success, NULL on error
 */
char *ble_ibeacon_uuid_to_string(const uint8_t uuid[16], char *buf, size_t buf_len);

/**
 * @brief Format Eddystone Namespace as hex string
 *
 * @param[in] namespace 10-byte namespace
 * @param[out] buf Output buffer (minimum 21 bytes)
 * @param[in] buf_len Buffer length
 * @return Pointer to buf on success, NULL on error
 */
char *ble_eddystone_namespace_to_string(const uint8_t namespace[10], char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_BEACON_H */
