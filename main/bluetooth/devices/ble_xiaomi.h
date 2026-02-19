/**
 * @file ble_xiaomi.h
 * @brief Xiaomi LYWSD03MMC BLE Sensor Parser (ATC/pvvx Firmware)
 *
 * Parses BLE advertisements from Xiaomi LYWSD03MMC thermometers
 * running custom ATC or pvvx firmware. Extracts temperature,
 * humidity, battery level, and signal strength.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_XIAOMI_H
#define BLE_XIAOMI_H

#include "ble_device_registry.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Xiaomi Environmental Sensing Service UUID */
#define BLE_XIAOMI_SERVICE_UUID         0x181A

/** @brief ATC firmware advertisement data length */
#define BLE_XIAOMI_ATC_ADV_LEN          13

/** @brief pvvx firmware advertisement data length (custom format) */
#define BLE_XIAOMI_PVVX_ADV_LEN         17

/** @brief Minimum valid temperature in Celsius */
#define BLE_XIAOMI_TEMP_MIN             (-40.0f)

/** @brief Maximum valid temperature in Celsius */
#define BLE_XIAOMI_TEMP_MAX             (85.0f)

/** @brief Minimum valid humidity percentage */
#define BLE_XIAOMI_HUMIDITY_MIN         (0.0f)

/** @brief Maximum valid humidity percentage */
#define BLE_XIAOMI_HUMIDITY_MAX         (100.0f)

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Detect if advertisement is from Xiaomi LYWSD03MMC
 *
 * Checks for Environmental Sensing service UUID (0x181A) and
 * validates advertisement format for ATC or pvvx firmware.
 *
 * @param[in] adv Advertisement data to analyze
 * @return
 *      - BLE_REG_DEVICE_TYPE_XIAOMI_LYWSD03MMC if detected
 *      - BLE_REG_DEVICE_TYPE_UNKNOWN otherwise
 */
ble_reg_device_type_t ble_xiaomi_detect(const ble_registry_adv_data_t *adv);

/**
 * @brief Parse Xiaomi LYWSD03MMC advertisement data
 *
 * Supports both ATC and pvvx custom firmware formats.
 * Extracts temperature, humidity, and battery percentage.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed sensor data
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if format not recognized
 *      - ESP_ERR_INVALID_RESPONSE if data validation fails
 */
esp_err_t ble_xiaomi_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data);

/**
 * @brief Check if Xiaomi sensor data is valid
 *
 * Validates temperature and humidity are within expected ranges.
 *
 * @param[in] data Sensor data to validate
 * @return true if data is valid, false otherwise
 */
bool ble_xiaomi_validate_data(const ble_sensor_data_t *data);

/**
 * @brief Get Xiaomi firmware type string
 *
 * @param[in] adv Advertisement data
 * @return "ATC", "pvvx", or "unknown"
 */
const char *ble_xiaomi_get_firmware_type(const ble_registry_adv_data_t *adv);

#ifdef __cplusplus
}
#endif

#endif /* BLE_XIAOMI_H */
