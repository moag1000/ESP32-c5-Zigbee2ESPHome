/**
 * @file zb_cluster_measurement.h
 * @brief Measurement Cluster APIs
 *
 * Provides support for:
 * - Illuminance Measurement (0x0400)
 * - Pressure Measurement (0x0403)
 * - PM2.5 Measurement (0x042A)
 *
 * Note: Temperature (0x0402) and Humidity (0x0405) are handled by
 * converter modules as they have simpler implementations.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_CLUSTER_MEASUREMENT_H
#define ZB_CLUSTER_MEASUREMENT_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types moved from zb_device_handler_types.h
 * ============================================================================ */

/** @brief Maximum illuminance sensor devices tracked */
#define ZB_STATE_MAX_ILLUMINANCE            16

/** @brief Maximum pressure sensor devices tracked */
#define ZB_STATE_MAX_PRESSURE               16

/** @brief Maximum PM2.5 sensor devices tracked */
#define ZB_STATE_MAX_PM25                   16

/* ============================================================================
 * Illuminance Measurement Cluster (0x0400) Types
 * ============================================================================ */

/**
 * @brief Illuminance Measurement Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT   0x0400

/**
 * @brief Illuminance Measurement Cluster Attribute IDs
 */
#define ZB_ZCL_ATTR_ILLUMINANCE_MEASURED_VALUE_ID   0x0000  /**< MeasuredValue (uint16) */
#define ZB_ZCL_ATTR_ILLUMINANCE_MIN_MEASURED_ID     0x0001  /**< MinMeasuredValue (uint16) */
#define ZB_ZCL_ATTR_ILLUMINANCE_MAX_MEASURED_ID     0x0002  /**< MaxMeasuredValue (uint16) */
#define ZB_ZCL_ATTR_ILLUMINANCE_TOLERANCE_ID        0x0003  /**< Tolerance (uint16) */
#define ZB_ZCL_ATTR_ILLUMINANCE_LIGHT_SENSOR_TYPE_ID 0x0004 /**< LightSensorType (enum8) */

/**
 * @brief Illuminance measurement invalid value
 *
 * Value 0x0000 indicates that illuminance is too low to be measured.
 * Value 0xFFFF indicates that illuminance is invalid/not configured.
 */
#define ZB_ZCL_ILLUMINANCE_MEASURED_VALUE_INVALID   0xFFFF
#define ZB_ZCL_ILLUMINANCE_MEASURED_VALUE_TOO_LOW   0x0000

/**
 * @brief Illuminance measurement state structure
 */
typedef struct {
    uint16_t measured_value;    /**< Measured illuminance value (log10(lux) * 10000 + 1) */
    uint16_t min_measured;      /**< Minimum measurable value */
    uint16_t max_measured;      /**< Maximum measurable value */
    uint16_t tolerance;         /**< Tolerance (optional) */
    uint8_t light_sensor_type;  /**< Light sensor type (0=photodiode, 1=CMOS) */
} zb_illuminance_state_t;

/**
 * @brief Illuminance state change callback type
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current illuminance measurement state
 */
typedef void (*zb_illuminance_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                          const zb_illuminance_state_t *state);

/* ============================================================================
 * Pressure Measurement Cluster (0x0403) Types
 * ============================================================================ */

/**
 * @brief Pressure Measurement Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT      0x0403

/**
 * @brief Pressure Measurement Cluster Attribute IDs
 */
#define ZB_ZCL_ATTR_PRESSURE_MEASURED_VALUE_ID      0x0000  /**< MeasuredValue (int16) in 10 Pa units */
#define ZB_ZCL_ATTR_PRESSURE_MIN_MEASURED_ID        0x0001  /**< MinMeasuredValue (int16) */
#define ZB_ZCL_ATTR_PRESSURE_MAX_MEASURED_ID        0x0002  /**< MaxMeasuredValue (int16) */
#define ZB_ZCL_ATTR_PRESSURE_TOLERANCE_ID           0x0003  /**< Tolerance (uint16) */
#define ZB_ZCL_ATTR_PRESSURE_SCALED_VALUE_ID        0x0010  /**< ScaledValue (int16) */
#define ZB_ZCL_ATTR_PRESSURE_MIN_SCALED_VALUE_ID    0x0011  /**< MinScaledValue (int16) */
#define ZB_ZCL_ATTR_PRESSURE_MAX_SCALED_VALUE_ID    0x0012  /**< MaxScaledValue (int16) */
#define ZB_ZCL_ATTR_PRESSURE_SCALED_TOLERANCE_ID    0x0013  /**< ScaledTolerance (uint16) */
#define ZB_ZCL_ATTR_PRESSURE_SCALE_ID               0x0014  /**< Scale (int8) - 10^Scale multiplier */

/**
 * @brief Pressure measurement invalid value
 * @note 0x8000 in int16_t representation is -32768
 */
#define ZB_ZCL_PRESSURE_MEASURED_VALUE_INVALID      ((int16_t)0x8000)

/**
 * @brief Pressure measurement state structure
 */
typedef struct {
    int16_t measured_value;     /**< Measured pressure in 10 Pa (1 kPa) units */
    int16_t min_measured;       /**< Minimum measurable value */
    int16_t max_measured;       /**< Maximum measurable value */
    uint16_t tolerance;         /**< Tolerance (optional) */
    int16_t scaled_value;       /**< Scaled value (optional, higher resolution) */
    int16_t min_scaled_value;   /**< Minimum scaled value */
    int16_t max_scaled_value;   /**< Maximum scaled value */
    uint16_t scaled_tolerance;  /**< Scaled tolerance */
    int8_t scale;               /**< Scale factor (10^scale) */
    bool has_scaled;            /**< True if scaled attributes are valid */
} zb_pressure_state_t;

/**
 * @brief Pressure state change callback type
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current pressure measurement state
 */
typedef void (*zb_pressure_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                        const zb_pressure_state_t *state);

/* ============================================================================
 * PM2.5 Measurement Cluster (0x042A) Types
 * ============================================================================ */

/**
 * @brief PM2.5 Measurement Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_PM25_MEASUREMENT          0x042A

/**
 * @brief PM2.5 Measurement Cluster Attribute IDs
 */
#define ZB_ZCL_ATTR_PM25_MEASURED_VALUE_ID          0x0000  /**< MeasuredValue (float/single) */
#define ZB_ZCL_ATTR_PM25_MIN_MEASURED_ID            0x0001  /**< MinMeasuredValue (float/single) */
#define ZB_ZCL_ATTR_PM25_MAX_MEASURED_ID            0x0002  /**< MaxMeasuredValue (float/single) */
#define ZB_ZCL_ATTR_PM25_TOLERANCE_ID               0x0003  /**< Tolerance (float/single) */

/**
 * @brief PM2.5 measurement invalid value (NaN)
 */
#define ZB_ZCL_PM25_MEASURED_VALUE_INVALID          0x7FC00000  /**< IEEE 754 quiet NaN */

/**
 * @brief PM2.5 measurement state structure
 */
typedef struct {
    float measured_value;   /**< Measured PM2.5 concentration in ug/m3 */
    float min_measured;     /**< Minimum measurable value */
    float max_measured;     /**< Maximum measurable value */
    float tolerance;        /**< Measurement tolerance */
    bool is_valid;          /**< True if measurement is valid (not NaN) */
} zb_pm25_state_t;

/**
 * @brief PM2.5 state change callback type
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current PM2.5 measurement state
 */
typedef void (*zb_pm25_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                    const zb_pm25_state_t *state);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

esp_err_t zb_cluster_measurement_init(void);
esp_err_t zb_cluster_measurement_deinit(void);
void zb_cluster_measurement_clear_all(void);
void zb_cluster_measurement_remove_device(uint16_t short_addr);

/* ============================================================================
 * Illuminance (0x0400) Public API
 * ============================================================================ */

esp_err_t zb_illuminance_register_callback(zb_illuminance_state_cb_t callback);
bool zb_device_has_illuminance(uint16_t short_addr);
esp_err_t zb_illuminance_read_value(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_illuminance_handle_report(uint16_t short_addr, uint8_t endpoint,
                                        uint16_t attr_id, void *value, size_t value_len);
esp_err_t zb_illuminance_get_state(uint16_t short_addr, zb_illuminance_state_t *state);
float zb_illuminance_to_lux(uint16_t raw_value);

/* ============================================================================
 * Pressure (0x0403) Public API
 * ============================================================================ */

esp_err_t zb_pressure_register_callback(zb_pressure_state_cb_t callback);
bool zb_device_has_pressure(uint16_t short_addr);
esp_err_t zb_pressure_read_value(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_pressure_handle_report(uint16_t short_addr, uint8_t endpoint,
                                     uint16_t attr_id, void *value, size_t value_len);
esp_err_t zb_pressure_get_state(uint16_t short_addr, zb_pressure_state_t *state);
float zb_pressure_to_hpa(const zb_pressure_state_t *state);

/* ============================================================================
 * PM2.5 (0x042A) Public API
 * ============================================================================ */

esp_err_t zb_pm25_register_callback(zb_pm25_state_cb_t callback);
bool zb_device_has_pm25(uint16_t short_addr);
esp_err_t zb_pm25_read_value(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_pm25_handle_report(uint16_t short_addr, uint8_t endpoint,
                                 uint16_t attr_id, void *value, size_t value_len);
esp_err_t zb_pm25_get_state(uint16_t short_addr, zb_pm25_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CLUSTER_MEASUREMENT_H */
