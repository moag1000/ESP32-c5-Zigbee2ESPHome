/**
 * @file ble_govee.h
 * @brief Govee H5075 BLE Thermometer/Hygrometer Parser
 *
 * Parses BLE advertisements from Govee H5075 temperature and
 * humidity sensors. Extracts temperature, humidity, and battery level.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_GOVEE_H
#define BLE_GOVEE_H

#include "ble_device_registry.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Govee manufacturer ID (little-endian: 0x88EC) */
#define BLE_GOVEE_MANUFACTURER_ID       0xEC88

/** @brief Govee H5075 manufacturer data length */
#define BLE_GOVEE_H5075_DATA_LEN        9

/** @brief Minimum valid temperature in Celsius */
#define BLE_GOVEE_TEMP_MIN              (-20.0f)

/** @brief Maximum valid temperature in Celsius */
#define BLE_GOVEE_TEMP_MAX              (60.0f)

/** @brief Minimum valid humidity percentage */
#define BLE_GOVEE_HUMIDITY_MIN          (0.0f)

/** @brief Maximum valid humidity percentage */
#define BLE_GOVEE_HUMIDITY_MAX          (100.0f)

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Detect if advertisement is from Govee H5075
 *
 * Checks for Govee manufacturer ID (0xEC88) in advertisement data.
 *
 * @param[in] adv Advertisement data to analyze
 * @return
 *      - BLE_REG_DEVICE_TYPE_GOVEE_H5075 if detected
 *      - BLE_REG_DEVICE_TYPE_UNKNOWN otherwise
 */
ble_reg_device_type_t ble_govee_detect(const ble_registry_adv_data_t *adv);

/**
 * @brief Parse Govee H5075 advertisement data
 *
 * Extracts temperature, humidity, and battery percentage
 * from Govee's proprietary manufacturer data format.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed sensor data
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if format not recognized
 *      - ESP_ERR_INVALID_RESPONSE if data validation fails
 */
esp_err_t ble_govee_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data);

/**
 * @brief Check if Govee sensor data is valid
 *
 * Validates temperature and humidity are within expected ranges.
 *
 * @param[in] data Sensor data to validate
 * @return true if data is valid, false otherwise
 */
bool ble_govee_validate_data(const ble_sensor_data_t *data);

/**
 * @brief Get Govee model name from advertisement
 *
 * @param[in] adv Advertisement data
 * @return Model name string ("H5075" or "unknown")
 */
const char *ble_govee_get_model(const ble_registry_adv_data_t *adv);

#ifdef __cplusplus
}
#endif

#endif /* BLE_GOVEE_H */
