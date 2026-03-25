/**
 * @file zb_converter_std.h
 * @brief Standard Reusable Converter Function Declarations
 *
 * Provides standard fromZigbee (fz_*) and toZigbee (tz_*) converter
 * functions that can be shared across multiple device definitions.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef ZB_CONVERTER_STD_H
#define ZB_CONVERTER_STD_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * fromZigbee (fz_*) Converters: ZCL Attribute -> JSON
 *
 * Signature: esp_err_t fn(const void *raw, size_t len, cJSON *json, const char *key)
 * ============================================================================ */

/** @brief On/Off attribute -> "ON"/"OFF" string */
esp_err_t fz_on_off(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Brightness (uint8) -> number 0-254 */
esp_err_t fz_brightness(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Color temperature (uint16) -> mireds */
esp_err_t fz_color_temp(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Color XY (uint16) -> normalized 0.0-1.0 */
esp_err_t fz_color_xy(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Temperature measurement (int16) -> degrees C (raw/100) */
esp_err_t fz_temperature(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Humidity measurement (uint16) -> percent (raw/100) */
esp_err_t fz_humidity(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Pressure measurement (int16) -> hPa (raw/10) */
esp_err_t fz_pressure(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Illuminance measurement (uint16) -> lux via log formula */
esp_err_t fz_illuminance(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Occupancy sensing (uint8 bit0) -> boolean */
esp_err_t fz_occupancy(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Battery percentage (uint8) -> percent (raw/2) */
esp_err_t fz_battery(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief IAS Zone status (uint16 bitmask) -> boolean (bit0 = alarm1) */
esp_err_t fz_ias_zone_status(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief DJT11LM status type (uint16 on Door Lock 0x0101, attr 0x0055) -> action string + vibration bool */
esp_err_t fz_vibration_action(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief DJT11LM vibration strength (uint32 on Door Lock 0x0101, attr 0x0505) -> byte-swapped uint16 */
esp_err_t fz_vibration_strength(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief DJT11LM orientation (uint48 on Door Lock 0x0101, attr 0x0508) -> angle_x, angle_y, angle_z */
esp_err_t fz_vibration_angle(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Xiaomi 0xFF01 proprietary TLV blob -> battery (from voltage), voltage, device_temperature */
esp_err_t fz_xiaomi_ff01(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Simple uint16 pass-through -> number */
esp_err_t fz_uint16(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Electrical active power (int16) -> watts (raw/10) */
esp_err_t fz_electrical_power(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Metering current summation (uint48) -> kWh */
esp_err_t fz_metering(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Thermostat local temperature (int16) -> degrees C (raw/100) */
esp_err_t fz_thermostat_local_temp(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Cover position (uint8) -> 0-100 percent */
esp_err_t fz_cover_position(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Lock state (uint8) -> "LOCK"/"UNLOCK" string */
esp_err_t fz_lock_state(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Color Hue/Saturation (uint8) -> color object */
esp_err_t fz_color_hs(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Thermostat system mode (uint8 enum) -> mode string */
esp_err_t fz_thermostat_system_mode(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Thermostat running state (uint16 bitmask) -> state string */
esp_err_t fz_thermostat_running_state(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Fan mode (uint8 enum) -> mode string */
esp_err_t fz_fan_mode(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief RMS Current (uint16) -> amps (raw/1000) */
esp_err_t fz_current(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief RMS Voltage (uint16) -> volts (raw/10) */
esp_err_t fz_voltage(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief CO2 concentration (float) -> ppm */
esp_err_t fz_co2(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Cover tilt position (uint8) -> 0-100 percent */
esp_err_t fz_cover_tilt(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief VOC measurement (float) -> ppb */
esp_err_t fz_voc(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Power factor (int8) -> percentage */
esp_err_t fz_power_factor(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Smoke alarm IAS Zone status -> boolean */
esp_err_t fz_smoke_alarm(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Water leak IAS Zone status -> boolean */
esp_err_t fz_water_leak(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Power-on behavior (uint8 enum) -> string */
esp_err_t fz_power_on_behavior(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Xiaomi battery voltage (uint16) -> volts */
esp_err_t fz_xiaomi_voltage(const void *raw, size_t len, cJSON *json, const char *key);

/**
 * @brief Generic attribute converter — auto-converts based on ZCL data type
 *
 * Uses dispatch context (zb_converter_get_dispatch_ctx()) to read the ZCL
 * attribute data type. Handles: bool, uint8/16/24/32, int8/16/24/32,
 * float, and octet string.
 *
 * For temperature/humidity clusters (0x0402/0x0405), applies /100 scaling.
 * For pressure cluster (0x0403), applies /10 scaling.
 * For illuminance (0x0400), applies log10 formula.
 */
esp_err_t fz_generic_attr(const void *raw, size_t len, cJSON *json, const char *key);

/* --- std/zb_converter_std_hvac.c --- */

/** @brief Thermostat setpoint (int16) -> degrees C (raw/100) */
esp_err_t fz_thermostat_setpoint(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Thermostat weekly schedule -> JSON schedule object */
esp_err_t fz_thermostat_weekly_schedule(const void *raw, size_t len, cJSON *json, const char *key);

/* --- std/zb_converter_std_security.c --- */

/** @brief IAS zone enrollment status -> JSON "enrolled" bool */
esp_err_t fz_ias_zone_enrollment(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief IAS WD/Siren mode -> string */
esp_err_t fz_ias_wd(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Door lock user status -> string */
esp_err_t fz_door_lock_user_status(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Door lock PIN code response -> JSON */
esp_err_t fz_door_lock_pin_code(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Door lock operation event -> JSON object */
esp_err_t fz_door_lock_event(const void *raw, size_t len, cJSON *json, const char *key);

/* --- std/zb_converter_std_lighting.c --- */

/** @brief Enhanced color hue/saturation (uint16+uint8) -> color object */
esp_err_t fz_color_enhance_hs(const void *raw, size_t len, cJSON *json, const char *key);

/* --- std/zb_converter_std_electrical.c --- */

/** @brief AC frequency (uint16) -> Hz (raw/10) */
esp_err_t fz_electrical_frequency(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Diagnostics RSSI (int8) -> dBm */
esp_err_t fz_diagnostics_rssi(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Diagnostics LQI (uint8) -> 0-255 */
esp_err_t fz_diagnostics_lqi(const void *raw, size_t len, cJSON *json, const char *key);

/* --- std/zb_converter_std_tuya.c --- */

/** @brief Universal Tuya DP report parser -> JSON */
esp_err_t fz_tuya_dp(const void *raw, size_t len, cJSON *json, const char *key);

/* --- std/zb_converter_std_vendor.c --- */

/** @brief Aqara Opple cluster (0xFCC0) attribute -> JSON */
esp_err_t fz_aqara_opple(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief IKEA air purifier PM2.5/filter (0xFC7D) -> JSON */
esp_err_t fz_ikea_air_purifier(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief IKEA VOC index (0xFC7D) -> number */
esp_err_t fz_ikea_voc_index(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Philips Hue motion (0xFC00) sensitivity/LED -> JSON */
esp_err_t fz_philips_hue_motion(const void *raw, size_t len, cJSON *json, const char *key);

/* --- Remaining in zb_converter_std.c --- */

/** @brief PM2.5 concentration (0x042A) -> ug/m3 */
esp_err_t fz_pm25(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Soil moisture (0x0408) -> percent */
esp_err_t fz_soil_moisture(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Device temperature (cluster 0x0002) -> degrees C */
esp_err_t fz_device_temperature(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Multistate input (cluster 0x0012) -> number */
esp_err_t fz_multistate_input(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief Analog input (cluster 0x000C) -> float */
esp_err_t fz_analog_input(const void *raw, size_t len, cJSON *json, const char *key);

/* ============================================================================
 * toZigbee (tz_*) Converters: JSON Command -> ZCL
 *
 * Signature: esp_err_t fn(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
 * ============================================================================ */

/** @brief "ON"/"OFF"/"TOGGLE" -> ZCL on/off command */
esp_err_t tz_on_off(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief Number -> ZCL level control command */
esp_err_t tz_brightness(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief Mireds -> ZCL color temperature command */
esp_err_t tz_color_temp(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief {x,y} -> ZCL color XY command */
esp_err_t tz_color_xy(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief Degrees C -> ZCL thermostat setpoint write */
esp_err_t tz_thermostat_setpoint(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief 0-100 -> ZCL go to lift percentage */
esp_err_t tz_cover_position(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief "OPEN"/"CLOSE"/"STOP" -> ZCL cover commands */
esp_err_t tz_cover_command(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief "LOCK"/"UNLOCK" -> ZCL lock commands */
esp_err_t tz_lock_command(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief "low"/"medium"/"high" -> Xiaomi sensitivity (Basic cluster 0xFF0D, manuf 0x115F) */
esp_err_t tz_xiaomi_sensitivity(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief Retry pending write command if device just reported (sleepy device wake) */
void zb_converter_std_retry_pending(uint16_t short_addr);

/** @brief {hue, saturation} -> ZCL color hue/saturation command */
esp_err_t tz_color_hs(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief Mode string -> ZCL thermostat system mode write */
esp_err_t tz_thermostat_system_mode(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief Mode string -> ZCL fan mode write */
esp_err_t tz_fan_mode(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief 0-100 -> ZCL cover tilt percentage */
esp_err_t tz_cover_tilt(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief "off"/"on"/"toggle"/"previous" -> ZCL StartUpOnOff write */
esp_err_t tz_power_on_behavior(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/**
 * @brief Generic attribute write — JSON value -> ZCL Write Attribute
 *
 * Uses dispatch context to get cluster_id. Cross-references the converter's
 * from_zigbee entries to find the matching attr_id for the JSON key.
 * Auto-infers ZCL data type from the JSON value type and fz entry context.
 */
esp_err_t tz_generic_write_attr(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/* --- std/zb_converter_std_hvac.c --- */

/** @brief JSON schedule -> ZCL SetWeeklySchedule command */
esp_err_t tz_thermostat_weekly_schedule(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/* --- std/zb_converter_std_security.c --- */

/** @brief JSON {mode, strobe, duration} -> IAS WD Start Warning */
esp_err_t tz_warning(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief JSON {user_id, pin_code} -> Door Lock Set PIN Code */
esp_err_t tz_door_lock_pin(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief JSON {zone_id, response} -> IAS Zone Enroll Response */
esp_err_t tz_ias_zone_enroll_response(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/* --- std/zb_converter_std_lighting.c --- */

/** @brief Effect string -> ZCL Identify Trigger Effect command */
esp_err_t tz_effect(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/* --- std/zb_converter_std_tuya.c --- */

/** @brief Tuya command via registered driver -> cluster 0xEF00 command */
esp_err_t tz_tuya_command(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief Universal Tuya DP writer -> cluster 0xEF00 command */
esp_err_t tz_tuya_dp(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/* --- std/zb_converter_std_vendor.c --- */

/** @brief Number/string -> Aqara Opple manufacturer-specific write */
esp_err_t tz_aqara_opple(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief Number (seconds) -> ZCL Identify command */
esp_err_t tz_identify(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/** @brief Effect string -> ZCL Identify Trigger Effect command */
esp_err_t tz_identify_effect(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CONVERTER_STD_H */
