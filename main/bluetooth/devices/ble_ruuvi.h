/**
 * @file ble_ruuvi.h
 * @brief Ruuvi Tag BLE Environmental Sensor Parser
 *
 * Parses BLE advertisements from Ruuvi Tag sensors.
 * Supports RAWv1 (Data Format 3) and RAWv2 (Data Format 5).
 *
 * Data extracted:
 * - Temperature (-163.84 to +163.83 °C)
 * - Humidity (0-100%)
 * - Barometric pressure (500-1155.36 hPa)
 * - Battery voltage (1.6-3.647V)
 * - Acceleration (X, Y, Z)
 * - TX power
 * - Movement counter
 * - Measurement sequence
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_RUUVI_H
#define BLE_RUUVI_H

#include "ble_device_registry.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Ruuvi Innovations manufacturer ID (little-endian: 0x9904) */
#define BLE_RUUVI_MANUFACTURER_ID       0x0499

/** @brief RAWv1 data format identifier */
#define BLE_RUUVI_FORMAT_RAWV1          3

/** @brief RAWv2 data format identifier */
#define BLE_RUUVI_FORMAT_RAWV2          5

/** @brief RAWv1 payload length (after manufacturer ID) */
#define BLE_RUUVI_RAWV1_LEN             14

/** @brief RAWv2 payload length without MAC (after manufacturer ID) */
#define BLE_RUUVI_RAWV2_LEN             18

/** @brief RAWv2 payload length with MAC (after manufacturer ID) */
#define BLE_RUUVI_RAWV2_WITH_MAC_LEN    24

/** @brief Minimum valid temperature in Celsius */
#define BLE_RUUVI_TEMP_MIN              (-163.84f)

/** @brief Maximum valid temperature in Celsius */
#define BLE_RUUVI_TEMP_MAX              (163.83f)

/** @brief Minimum valid humidity percentage */
#define BLE_RUUVI_HUMIDITY_MIN          (0.0f)

/** @brief Maximum valid humidity percentage */
#define BLE_RUUVI_HUMIDITY_MAX          (100.0f)

/** @brief Minimum valid pressure in hPa */
#define BLE_RUUVI_PRESSURE_MIN          (500.0f)

/** @brief Maximum valid pressure in hPa */
#define BLE_RUUVI_PRESSURE_MAX          (1155.36f)

/** @brief Minimum valid battery voltage in V */
#define BLE_RUUVI_BATTERY_MIN           (1.6f)

/** @brief Maximum valid battery voltage in V */
#define BLE_RUUVI_BATTERY_MAX           (3.647f)

/** @brief Invalid temperature marker (0x8000) */
#define BLE_RUUVI_INVALID_TEMP          0x8000

/** @brief Invalid humidity marker (0xFFFF) */
#define BLE_RUUVI_INVALID_HUMIDITY      0xFFFF

/** @brief Invalid pressure marker (0xFFFF) */
#define BLE_RUUVI_INVALID_PRESSURE      0xFFFF

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief Extended Ruuvi sensor data structure
 *
 * Contains all data fields available from Ruuvi tags including
 * acceleration, TX power, and sequence counters not present in
 * the standard ble_sensor_data_t.
 */
typedef struct {
    float temperature;          /**< Temperature in Celsius (-163.84 to +163.83) */
    float humidity;             /**< Relative humidity in percent (0-100) */
    float pressure;             /**< Barometric pressure in hPa (500-1155.36) */
    float battery_voltage;      /**< Battery voltage in V (1.6-3.647) */
    int8_t tx_power;            /**< TX power in dBm (-40 to +20) */
    int16_t accel_x;            /**< Acceleration X in mG (-32000 to +32000) */
    int16_t accel_y;            /**< Acceleration Y in mG (-32000 to +32000) */
    int16_t accel_z;            /**< Acceleration Z in mG (-32000 to +32000) */
    uint8_t movement_counter;   /**< Movement counter (0-254, 255=invalid) */
    uint16_t measurement_seq;   /**< Measurement sequence number */
    uint8_t data_format;        /**< Data format version (3 or 5) */
    bool valid;                 /**< Data validity flag */
} ble_ruuvi_data_t;

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Detect if advertisement is from Ruuvi Tag
 *
 * Checks for Ruuvi manufacturer ID (0x0499) and valid data format
 * in advertisement data.
 *
 * @param[in] adv Advertisement data to analyze
 * @return
 *      - BLE_REG_DEVICE_TYPE_RUUVI_TAG if detected
 *      - BLE_REG_DEVICE_TYPE_UNKNOWN otherwise
 */
ble_reg_device_type_t ble_ruuvi_detect(const ble_registry_adv_data_t *adv);

/**
 * @brief Parse Ruuvi Tag advertisement data
 *
 * Extracts temperature, humidity, and battery percentage from
 * Ruuvi's RAWv1 or RAWv2 format. Maps to standard ble_sensor_data_t
 * for compatibility with the device registry.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed sensor data (uses sensor union member)
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if format not recognized
 *      - ESP_ERR_INVALID_RESPONSE if data validation fails
 */
esp_err_t ble_ruuvi_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data);

/**
 * @brief Parse Ruuvi Tag advertisement with extended data
 *
 * Extracts all available data fields including acceleration,
 * TX power, and sequence counters.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Extended Ruuvi data structure
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if format not recognized
 *      - ESP_ERR_INVALID_RESPONSE if data validation fails
 */
esp_err_t ble_ruuvi_parse_extended(const ble_registry_adv_data_t *adv, ble_ruuvi_data_t *out_data);

/**
 * @brief Check if Ruuvi sensor data is valid
 *
 * Validates temperature, humidity, and pressure are within expected ranges.
 *
 * @param[in] data Sensor data to validate
 * @return true if data is valid, false otherwise
 */
bool ble_ruuvi_validate_data(const ble_sensor_data_t *data);

/**
 * @brief Check if extended Ruuvi data is valid
 *
 * Validates all data fields against Ruuvi specifications.
 *
 * @param[in] data Extended Ruuvi data to validate
 * @return true if data is valid, false otherwise
 */
bool ble_ruuvi_validate_extended(const ble_ruuvi_data_t *data);

/**
 * @brief Get data format name from Ruuvi advertisement
 *
 * @param[in] adv Advertisement data
 * @return Format name string ("RAWv1", "RAWv2", or "unknown")
 */
const char *ble_ruuvi_get_format_name(const ble_registry_adv_data_t *adv);

/**
 * @brief Convert battery voltage to percentage estimate
 *
 * Maps battery voltage (1.6V-3.0V) to percentage (0-100%).
 * Uses linear interpolation based on typical CR2477 discharge curve.
 *
 * @param[in] voltage Battery voltage in V
 * @return Estimated battery percentage (0-100)
 */
uint8_t ble_ruuvi_voltage_to_percent(float voltage);

#ifdef __cplusplus
}
#endif

#endif /* BLE_RUUVI_H */
