/**
 * @file zb_cluster_electrical.h
 * @brief Electrical Measurement (0x0B04) and Metering (0x0702) Cluster APIs
 *
 * Provides support for electrical measurement and smart metering devices.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_CLUSTER_ELECTRICAL_H
#define ZB_CLUSTER_ELECTRICAL_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types moved from zb_device_handler_types.h
 * ============================================================================ */

/** @brief Maximum electrical measurement devices tracked */
#define ZB_STATE_MAX_ELECTRICAL             16

/** @brief Maximum metering (smart meter) devices tracked */
#define ZB_STATE_MAX_METERING               16

/* ============================================================================
 * Electrical Measurement Cluster (0x0B04) Types
 * ============================================================================ */

/**
 * @brief Electrical Measurement Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT    0x0B04

/**
 * @brief Electrical Measurement Cluster Attribute IDs
 */
/* Basic Information */
#define ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_TYPE_ID              0x0000  /**< MeasurementType (bitmap32) */

/* AC (Non-phase specific) */
#define ZB_ZCL_ATTR_ELECTRICAL_AC_FREQUENCY_ID                  0x0300  /**< AC Frequency (uint16, Hz * 10) */
#define ZB_ZCL_ATTR_ELECTRICAL_AC_FREQUENCY_MIN_ID              0x0301  /**< AC Frequency Min */
#define ZB_ZCL_ATTR_ELECTRICAL_AC_FREQUENCY_MAX_ID              0x0302  /**< AC Frequency Max */

/* AC Single Phase or Phase A */
#define ZB_ZCL_ATTR_ELECTRICAL_RMS_VOLTAGE_ID                   0x0505  /**< RMS Voltage (uint16, V * 10) */
#define ZB_ZCL_ATTR_ELECTRICAL_RMS_VOLTAGE_MIN_ID               0x0506  /**< RMS Voltage Min */
#define ZB_ZCL_ATTR_ELECTRICAL_RMS_VOLTAGE_MAX_ID               0x0507  /**< RMS Voltage Max */
#define ZB_ZCL_ATTR_ELECTRICAL_RMS_CURRENT_ID                   0x0508  /**< RMS Current (uint16, A * 1000 = mA) */
#define ZB_ZCL_ATTR_ELECTRICAL_RMS_CURRENT_MIN_ID               0x0509  /**< RMS Current Min */
#define ZB_ZCL_ATTR_ELECTRICAL_RMS_CURRENT_MAX_ID               0x050A  /**< RMS Current Max */
#define ZB_ZCL_ATTR_ELECTRICAL_ACTIVE_POWER_ID                  0x050B  /**< Active Power (int16, W * 10) */
#define ZB_ZCL_ATTR_ELECTRICAL_ACTIVE_POWER_MIN_ID              0x050C  /**< Active Power Min */
#define ZB_ZCL_ATTR_ELECTRICAL_ACTIVE_POWER_MAX_ID              0x050D  /**< Active Power Max */
#define ZB_ZCL_ATTR_ELECTRICAL_REACTIVE_POWER_ID                0x050E  /**< Reactive Power (int16, VAR * 10) */
#define ZB_ZCL_ATTR_ELECTRICAL_APPARENT_POWER_ID                0x050F  /**< Apparent Power (uint16, VA * 10) */
#define ZB_ZCL_ATTR_ELECTRICAL_POWER_FACTOR_ID                  0x0510  /**< Power Factor (int8, -100 to 100) */

/* Divisors/Multipliers for correct scaling */
#define ZB_ZCL_ATTR_ELECTRICAL_AC_VOLTAGE_MULTIPLIER_ID         0x0600  /**< AC Voltage Multiplier */
#define ZB_ZCL_ATTR_ELECTRICAL_AC_VOLTAGE_DIVISOR_ID            0x0601  /**< AC Voltage Divisor */
#define ZB_ZCL_ATTR_ELECTRICAL_AC_CURRENT_MULTIPLIER_ID         0x0602  /**< AC Current Multiplier */
#define ZB_ZCL_ATTR_ELECTRICAL_AC_CURRENT_DIVISOR_ID            0x0603  /**< AC Current Divisor */
#define ZB_ZCL_ATTR_ELECTRICAL_AC_POWER_MULTIPLIER_ID           0x0604  /**< AC Power Multiplier */
#define ZB_ZCL_ATTR_ELECTRICAL_AC_POWER_DIVISOR_ID              0x0605  /**< AC Power Divisor */

/**
 * @brief Electrical Measurement state structure
 *
 * Contains raw values from the device. Use helper functions to get
 * properly scaled values in standard units.
 */
typedef struct {
    /* Raw measurement values */
    uint16_t rms_voltage;           /**< RMS Voltage raw value (V * 10 typically) */
    uint16_t rms_current;           /**< RMS Current raw value (mA typically) */
    int16_t active_power;           /**< Active Power raw value (W * 10 typically) */
    int16_t reactive_power;         /**< Reactive Power raw value (VAR * 10 typically) */
    uint16_t apparent_power;        /**< Apparent Power raw value (VA * 10 typically) */
    int8_t power_factor;            /**< Power Factor (-100 to 100, percentage) */
    uint16_t ac_frequency;          /**< AC Frequency raw value (Hz * 10 typically) */

    /* Scaling factors (read once during device interview) */
    uint16_t voltage_multiplier;    /**< Voltage multiplier (default 1) */
    uint16_t voltage_divisor;       /**< Voltage divisor (default 10) */
    uint16_t current_multiplier;    /**< Current multiplier (default 1) */
    uint16_t current_divisor;       /**< Current divisor (default 1000) */
    uint16_t power_multiplier;      /**< Power multiplier (default 1) */
    uint16_t power_divisor;         /**< Power divisor (default 10) */

    /* Status flags */
    bool scaling_factors_read;      /**< True if scaling factors have been read */
    bool is_valid;                  /**< True if measurements are valid */
} zb_electrical_state_t;

/**
 * @brief Electrical Measurement state change callback type
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current electrical measurement state
 */
typedef void (*zb_electrical_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                          const zb_electrical_state_t *state);

/* ============================================================================
 * Metering Cluster (0x0702) Types - Smart Energy
 * ============================================================================ */

/**
 * @brief Metering Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_METERING                      0x0702

/**
 * @brief Metering Cluster Attribute IDs - Reading Information Set
 */
#define ZB_ZCL_ATTR_METERING_CURRENT_SUMM_DELIVERED_ID      0x0000  /**< CurrentSummationDelivered (uint48) */
#define ZB_ZCL_ATTR_METERING_CURRENT_SUMM_RECEIVED_ID       0x0001  /**< CurrentSummationReceived (uint48) */
#define ZB_ZCL_ATTR_METERING_CURRENT_MAX_DEMAND_DELIV_ID    0x0002  /**< CurrentMaxDemandDelivered (uint48) */
#define ZB_ZCL_ATTR_METERING_CURRENT_MAX_DEMAND_RECV_ID     0x0003  /**< CurrentMaxDemandReceived (uint48) */

/**
 * @brief Metering Cluster Attribute IDs - TOU (Time of Use) Information
 */
#define ZB_ZCL_ATTR_METERING_CURRENT_TIER1_SUMM_DELIV_ID    0x0100  /**< CurrentTier1SummationDelivered (uint48) */
#define ZB_ZCL_ATTR_METERING_CURRENT_TIER2_SUMM_DELIV_ID    0x0102  /**< CurrentTier2SummationDelivered (uint48) */

/**
 * @brief Metering Cluster Attribute IDs - Meter Status
 */
#define ZB_ZCL_ATTR_METERING_STATUS_ID                      0x0200  /**< Status (bitmap8) */
#define ZB_ZCL_ATTR_METERING_REMAINING_BATTERY_LIFE_ID      0x0201  /**< RemainingBatteryLife (uint8, %) */
#define ZB_ZCL_ATTR_METERING_HOURS_IN_OPERATION_ID          0x0202  /**< HoursInOperation (uint24) */

/**
 * @brief Metering Cluster Attribute IDs - Formatting
 */
#define ZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID             0x0300  /**< UnitOfMeasure (enum8) */
#define ZB_ZCL_ATTR_METERING_MULTIPLIER_ID                  0x0301  /**< Multiplier (uint24) */
#define ZB_ZCL_ATTR_METERING_DIVISOR_ID                     0x0302  /**< Divisor (uint24) */
#define ZB_ZCL_ATTR_METERING_SUMMATION_FORMATTING_ID        0x0303  /**< SummationFormatting (bitmap8) */
#define ZB_ZCL_ATTR_METERING_DEMAND_FORMATTING_ID           0x0304  /**< DemandFormatting (bitmap8) */
#define ZB_ZCL_ATTR_METERING_METERING_DEVICE_TYPE_ID        0x0306  /**< MeteringDeviceType (bitmap8) */

/**
 * @brief Metering Cluster Attribute IDs - Historical Consumption
 */
#define ZB_ZCL_ATTR_METERING_INSTANTANEOUS_DEMAND_ID        0x0400  /**< InstantaneousDemand (int24) - current power */
#define ZB_ZCL_ATTR_METERING_CURRENT_DAY_CONSUMPTION_ID     0x0401  /**< CurrentDayConsumption (uint24) */
#define ZB_ZCL_ATTR_METERING_PREVIOUS_DAY_CONSUMPTION_ID    0x0403  /**< PreviousDayConsumption (uint24) */

/**
 * @brief Metering Unit of Measure values (enum8)
 */
typedef enum {
    ZB_METERING_UNIT_KWH = 0x00,            /**< kWh (Kilowatt Hours) - Electricity */
    ZB_METERING_UNIT_CUBIC_METERS = 0x01,   /**< m3 (Cubic Meters) - Gas */
    ZB_METERING_UNIT_CUBIC_FEET = 0x02,     /**< ft3 (Cubic Feet) - Gas */
    ZB_METERING_UNIT_CCF = 0x03,            /**< ccf (100 Cubic Feet) */
    ZB_METERING_UNIT_US_GAL = 0x04,         /**< US gal (US Gallons) - Water */
    ZB_METERING_UNIT_IMP_GAL = 0x05,        /**< IMP gal (Imperial Gallons) - Water */
    ZB_METERING_UNIT_BTU = 0x06,            /**< BTU (British Thermal Units) */
    ZB_METERING_UNIT_LITERS = 0x07,         /**< L (Liters) - Water */
    ZB_METERING_UNIT_KPA_GAUGE = 0x08,      /**< kPA (Gauge) */
    ZB_METERING_UNIT_KPA_ABSOLUTE = 0x09,   /**< kPA (Absolute) */
    ZB_METERING_UNIT_MCF = 0x0A,            /**< mcf (1000 Cubic Feet) */
    ZB_METERING_UNIT_UNITLESS = 0x0B,       /**< Unitless */
    ZB_METERING_UNIT_MJ = 0x0C,             /**< MJ (Mega Joules) */
    ZB_METERING_UNIT_KVAR = 0x0D,           /**< kVAr (Kilo Volt Ampere Reactive) */
} zb_metering_unit_t;

/**
 * @brief Metering Device Type values (bitmap8)
 */
typedef enum {
    ZB_METERING_DEVICE_ELECTRIC = 0x00,     /**< Electric Metering */
    ZB_METERING_DEVICE_GAS = 0x01,          /**< Gas Metering */
    ZB_METERING_DEVICE_WATER = 0x02,        /**< Water Metering */
    ZB_METERING_DEVICE_THERMAL = 0x03,      /**< Thermal Metering (heat) */
    ZB_METERING_DEVICE_PRESSURE = 0x04,     /**< Pressure Metering */
    ZB_METERING_DEVICE_HEAT = 0x05,         /**< Heat Metering */
    ZB_METERING_DEVICE_COOLING = 0x06,      /**< Cooling Metering */
} zb_metering_device_type_t;

/**
 * @brief Metering state structure
 *
 * Stores raw values from the meter. Use helper functions to get formatted values.
 */
typedef struct {
    uint64_t current_summation_delivered;   /**< Total consumption delivered (raw uint48) */
    uint64_t current_summation_received;    /**< Total energy fed back (raw uint48) */
    int32_t instantaneous_demand;           /**< Current power/demand (raw int24) */
    uint32_t current_day_consumption;       /**< Today's consumption (raw uint24) */
    uint32_t previous_day_consumption;      /**< Yesterday's consumption (raw uint24) */
    uint8_t unit_of_measure;                /**< Unit of measure (enum8) */
    uint32_t multiplier;                    /**< Multiplier for formatting (uint24) */
    uint32_t divisor;                       /**< Divisor for formatting (uint24) */
    uint8_t summation_formatting;           /**< Summation formatting bits (bitmap8) */
    uint8_t demand_formatting;              /**< Demand formatting bits (bitmap8) */
    uint8_t metering_device_type;           /**< Device type (electric/gas/water) */
    uint8_t status;                         /**< Meter status (bitmap8) */
    uint8_t battery_percentage;             /**< Remaining battery life (0-100%) */
} zb_metering_state_t;

/**
 * @brief Metering state change callback type
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current metering state
 */
typedef void (*zb_metering_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                        const zb_metering_state_t *state);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

esp_err_t zb_cluster_electrical_init(void);
esp_err_t zb_cluster_electrical_deinit(void);
void zb_cluster_electrical_clear_all(void);
void zb_cluster_electrical_remove_device(uint16_t short_addr);

/* ============================================================================
 * Electrical Measurement (0x0B04) Public API
 * ============================================================================ */

esp_err_t zb_electrical_register_callback(zb_electrical_state_cb_t callback);
bool zb_device_has_electrical_measurement(uint16_t short_addr);
esp_err_t zb_electrical_read_values(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_electrical_read_scaling(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_electrical_handle_report(uint16_t short_addr, uint8_t endpoint,
                                       uint16_t attr_id, void *value, size_t value_len);
esp_err_t zb_electrical_get_state(uint16_t short_addr, zb_electrical_state_t *state);

float zb_electrical_get_voltage_v(const zb_electrical_state_t *state);
float zb_electrical_get_current_a(const zb_electrical_state_t *state);
float zb_electrical_get_power_w(const zb_electrical_state_t *state);
float zb_electrical_get_reactive_power_var(const zb_electrical_state_t *state);
float zb_electrical_get_apparent_power_va(const zb_electrical_state_t *state);
float zb_electrical_get_power_factor(const zb_electrical_state_t *state);
float zb_electrical_get_frequency_hz(const zb_electrical_state_t *state);

/* ============================================================================
 * Metering (0x0702) Public API
 * ============================================================================ */

esp_err_t zb_metering_register_callback(zb_metering_state_cb_t callback);
bool zb_device_has_metering(uint16_t short_addr);
esp_err_t zb_metering_read_values(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_metering_handle_report(uint16_t short_addr, uint8_t endpoint,
                                     uint16_t attr_id, void *value, size_t value_len);
esp_err_t zb_metering_get_state(uint16_t short_addr, zb_metering_state_t *state);

double zb_metering_get_total_energy(const zb_metering_state_t *state);
double zb_metering_get_power_w(const zb_metering_state_t *state);
double zb_metering_get_current_day_energy(const zb_metering_state_t *state);
double zb_metering_get_previous_day_energy(const zb_metering_state_t *state);
const char* zb_metering_get_unit_string(uint8_t unit);
uint64_t zb_metering_parse_uint48(const void *data, size_t len);
int32_t zb_metering_parse_int24(const void *data, size_t len);
uint32_t zb_metering_parse_uint24(const void *data, size_t len);
uint8_t zb_metering_get_decimal_places(uint8_t formatting);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CLUSTER_ELECTRICAL_H */
