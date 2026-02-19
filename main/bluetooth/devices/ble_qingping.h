/**
 * @file ble_qingping.h
 * @brief Qingping BLE Sensor Parser (Mi Beacon Format)
 *
 * Parses BLE advertisements from Qingping CGG1/CGP1S sensors using
 * the Mi Beacon format (Service UUID 0xFE95).
 *
 * Supported models:
 * - CGG1: Temperature and humidity sensor
 * - CGP1S: Temperature, humidity, and air pressure sensor
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_QINGPING_H
#define BLE_QINGPING_H

#include "ble_device_registry.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Mi Beacon service UUID (Xiaomi ecosystem) */
#define BLE_QINGPING_SERVICE_UUID           0xFE95

/** @brief Qingping device type ID in Mi Beacon header */
#define BLE_QINGPING_DEVICE_TYPE_CGG1       0x0B48
#define BLE_QINGPING_DEVICE_TYPE_CGG1_ENC   0x0B47
#define BLE_QINGPING_DEVICE_TYPE_CGDK2      0x066F
#define BLE_QINGPING_DEVICE_TYPE_CGP1S      0x0C47

/** @brief Mi Beacon frame control flags */
#define BLE_MI_FRAME_CTRL_HAS_MAC           0x0010
#define BLE_MI_FRAME_CTRL_HAS_CAPABILITY    0x0020
#define BLE_MI_FRAME_CTRL_HAS_EVENT         0x0040
#define BLE_MI_FRAME_CTRL_HAS_CUSTOM        0x0080
#define BLE_MI_FRAME_CTRL_HAS_SUBTITLE      0x0100
#define BLE_MI_FRAME_CTRL_ENCRYPTED         0x0008
#define BLE_MI_FRAME_CTRL_BINDING           0x0200

/** @brief Mi Beacon object types */
#define BLE_MI_OBJECT_TEMPERATURE           0x1004  /**< Temperature (2 bytes, 0.1°C) */
#define BLE_MI_OBJECT_HUMIDITY              0x1006  /**< Humidity (2 bytes, 0.1%) */
#define BLE_MI_OBJECT_BATTERY               0x100A  /**< Battery (1 byte, %) */
#define BLE_MI_OBJECT_TEMP_HUMIDITY         0x100D  /**< Combined temp+humidity (4 bytes) */
#define BLE_MI_OBJECT_PRESSURE              0x1008  /**< Pressure (3 bytes, 0.1 hPa) */

/** @brief Minimum valid temperature in Celsius */
#define BLE_QINGPING_TEMP_MIN               (-40.0f)

/** @brief Maximum valid temperature in Celsius */
#define BLE_QINGPING_TEMP_MAX               (60.0f)

/** @brief Minimum valid humidity percentage */
#define BLE_QINGPING_HUMIDITY_MIN           (0.0f)

/** @brief Maximum valid humidity percentage */
#define BLE_QINGPING_HUMIDITY_MAX           (100.0f)

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief Qingping device model enumeration
 */
typedef enum {
    BLE_QINGPING_MODEL_UNKNOWN = 0,
    BLE_QINGPING_MODEL_CGG1,            /**< CGG1 - Temp/Humidity */
    BLE_QINGPING_MODEL_CGG1_ENC,        /**< CGG1 with encryption */
    BLE_QINGPING_MODEL_CGDK2,           /**< CGDK2 - Temp/Humidity LCD */
    BLE_QINGPING_MODEL_CGP1S,           /**< CGP1S - Temp/Humidity/Pressure */
} ble_qingping_model_t;

/**
 * @brief Extended Qingping sensor data
 */
typedef struct {
    float temperature;              /**< Temperature in Celsius */
    float humidity;                 /**< Relative humidity in percent */
    float pressure;                 /**< Barometric pressure in hPa (0 if not available) */
    uint8_t battery;                /**< Battery level in percent (0-100) */
    ble_qingping_model_t model;     /**< Detected device model */
    uint8_t frame_counter;          /**< Mi Beacon frame counter */
    bool has_temperature;           /**< Temperature data present */
    bool has_humidity;              /**< Humidity data present */
    bool has_pressure;              /**< Pressure data present */
    bool has_battery;               /**< Battery data present */
    bool encrypted;                 /**< Data is encrypted (not parsed) */
    bool valid;                     /**< Data validity flag */
} ble_qingping_data_t;

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Detect if advertisement is from Qingping sensor
 *
 * Checks for Mi Beacon service UUID (0xFE95) and Qingping device type
 * in advertisement data.
 *
 * @param[in] adv Advertisement data to analyze
 * @return
 *      - BLE_REG_DEVICE_TYPE_QINGPING if detected
 *      - BLE_REG_DEVICE_TYPE_UNKNOWN otherwise
 */
ble_reg_device_type_t ble_qingping_detect(const ble_registry_adv_data_t *adv);

/**
 * @brief Parse Qingping sensor advertisement data
 *
 * Extracts temperature, humidity, and battery from Mi Beacon format.
 * Maps to standard ble_sensor_data_t for registry compatibility.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed sensor data (uses sensor union member)
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if format not recognized or encrypted
 *      - ESP_ERR_INVALID_RESPONSE if data validation fails
 */
esp_err_t ble_qingping_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data);

/**
 * @brief Parse Qingping advertisement with extended data
 *
 * Extracts all available data fields including pressure (for CGP1S).
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Extended Qingping data structure
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if format not recognized or encrypted
 *      - ESP_ERR_INVALID_RESPONSE if data validation fails
 */
esp_err_t ble_qingping_parse_extended(const ble_registry_adv_data_t *adv, ble_qingping_data_t *out_data);

/**
 * @brief Check if Qingping sensor data is valid
 *
 * Validates temperature and humidity are within expected ranges.
 *
 * @param[in] data Sensor data to validate
 * @return true if data is valid, false otherwise
 */
bool ble_qingping_validate_data(const ble_sensor_data_t *data);

/**
 * @brief Get model name from Qingping device type
 *
 * @param[in] model Device model enumeration
 * @return Model name string ("CGG1", "CGP1S", etc.)
 */
const char *ble_qingping_get_model_name(ble_qingping_model_t model);

/**
 * @brief Get model from advertisement
 *
 * @param[in] adv Advertisement data
 * @return Detected model enumeration
 */
ble_qingping_model_t ble_qingping_get_model(const ble_registry_adv_data_t *adv);

#ifdef __cplusplus
}
#endif

#endif /* BLE_QINGPING_H */
