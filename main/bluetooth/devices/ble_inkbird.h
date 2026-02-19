/**
 * @file ble_inkbird.h
 * @brief Inkbird BLE Temperature/Humidity Sensor Parser
 *
 * Parses BLE advertisements from Inkbird IBS-TH1/TH2 sensors.
 * These sensors broadcast temperature and humidity data in their
 * manufacturer-specific data.
 *
 * Supported models:
 * - IBS-TH1: Temperature only (internal probe)
 * - IBS-TH2: Temperature and humidity
 * - ITH-20R: With external temperature probe
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_INKBIRD_H
#define BLE_INKBIRD_H

#include "ble_device_registry.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/**
 * @brief Inkbird uses device name prefix for identification
 *
 * Inkbird devices don't have a consistent manufacturer ID but
 * advertise with names like "sps", "tps", "Inkbird" etc.
 */
#define BLE_INKBIRD_NAME_PREFIX_SPS         "sps"
#define BLE_INKBIRD_NAME_PREFIX_TPS         "tps"
#define BLE_INKBIRD_NAME_PREFIX_IBS         "IBS"
#define BLE_INKBIRD_NAME_PREFIX_ITH         "ITH"

/** @brief Inkbird service UUID (primary method) */
#define BLE_INKBIRD_SERVICE_UUID            0xFFF0

/** @brief Alternative manufacturer ID (some models) */
#define BLE_INKBIRD_MANUFACTURER_ID         0x0001

/** @brief Minimum valid temperature in Celsius */
#define BLE_INKBIRD_TEMP_MIN                (-40.0f)

/** @brief Maximum valid temperature in Celsius */
#define BLE_INKBIRD_TEMP_MAX                (100.0f)

/** @brief Minimum valid humidity percentage */
#define BLE_INKBIRD_HUMIDITY_MIN            (0.0f)

/** @brief Maximum valid humidity percentage */
#define BLE_INKBIRD_HUMIDITY_MAX            (100.0f)

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief Inkbird device model enumeration
 */
typedef enum {
    BLE_INKBIRD_MODEL_UNKNOWN = 0,
    BLE_INKBIRD_MODEL_IBS_TH1,          /**< IBS-TH1 - Temperature only */
    BLE_INKBIRD_MODEL_IBS_TH2,          /**< IBS-TH2 - Temp + Humidity */
    BLE_INKBIRD_MODEL_ITH_20R,          /**< ITH-20R - With external probe */
} ble_inkbird_model_t;

/**
 * @brief Extended Inkbird sensor data
 */
typedef struct {
    float temperature;              /**< Temperature in Celsius */
    float humidity;                 /**< Relative humidity in percent */
    float external_temp;            /**< External probe temperature (0 if not present) */
    ble_inkbird_model_t model;      /**< Detected device model */
    bool has_humidity;              /**< Humidity data present */
    bool has_external_temp;         /**< External temperature present */
    bool valid;                     /**< Data validity flag */
} ble_inkbird_data_t;

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Detect if advertisement is from Inkbird sensor
 *
 * Checks for Inkbird service UUID (0xFFF0) or device name prefix
 * in advertisement data.
 *
 * @param[in] adv Advertisement data to analyze
 * @return
 *      - BLE_REG_DEVICE_TYPE_INKBIRD if detected
 *      - BLE_REG_DEVICE_TYPE_UNKNOWN otherwise
 */
ble_reg_device_type_t ble_inkbird_detect(const ble_registry_adv_data_t *adv);

/**
 * @brief Parse Inkbird sensor advertisement data
 *
 * Extracts temperature and humidity from Inkbird's format.
 * Maps to standard ble_sensor_data_t for registry compatibility.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed sensor data (uses sensor union member)
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if format not recognized
 *      - ESP_ERR_INVALID_RESPONSE if data validation fails
 */
esp_err_t ble_inkbird_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data);

/**
 * @brief Parse Inkbird advertisement with extended data
 *
 * Extracts all available data fields including external temperature probe.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Extended Inkbird data structure
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if format not recognized
 */
esp_err_t ble_inkbird_parse_extended(const ble_registry_adv_data_t *adv, ble_inkbird_data_t *out_data);

/**
 * @brief Check if Inkbird sensor data is valid
 *
 * Validates temperature and humidity are within expected ranges.
 *
 * @param[in] data Sensor data to validate
 * @return true if data is valid, false otherwise
 */
bool ble_inkbird_validate_data(const ble_sensor_data_t *data);

/**
 * @brief Get model name from Inkbird model enumeration
 *
 * @param[in] model Device model enumeration
 * @return Model name string ("IBS-TH1", "IBS-TH2", etc.)
 */
const char *ble_inkbird_get_model_name(ble_inkbird_model_t model);

#ifdef __cplusplus
}
#endif

#endif /* BLE_INKBIRD_H */
