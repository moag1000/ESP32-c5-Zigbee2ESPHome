/**
 * @file zb_converter_fn_registry.c
 * @brief Function Name Registry — Binary Search Lookup
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_converter_fn_registry.h"
#include "zb_converter_std.h"
#include <string.h>

/* ============================================================================
 * fromZigbee function table — MUST be sorted alphabetically by name
 * ============================================================================ */

typedef struct {
    const char *name;
    fz_convert_fn_t fn;
} fz_entry_t;

static const fz_entry_t s_fz_table[] = {
    { "fz_analog_input",           fz_analog_input },
    { "fz_aqara_opple",            fz_aqara_opple },
    { "fz_battery",                fz_battery },
    { "fz_brightness",             fz_brightness },
    { "fz_co2",                    fz_co2 },
    { "fz_color_enhance_hs",      fz_color_enhance_hs },
    { "fz_color_hs",               fz_color_hs },
    { "fz_color_temp",             fz_color_temp },
    { "fz_color_xy",               fz_color_xy },
    { "fz_cover_position",         fz_cover_position },
    { "fz_cover_tilt",             fz_cover_tilt },
    { "fz_current",                fz_current },
    { "fz_device_temperature",     fz_device_temperature },
    { "fz_diagnostics_lqi",        fz_diagnostics_lqi },
    { "fz_diagnostics_rssi",       fz_diagnostics_rssi },
    { "fz_door_lock_event",        fz_door_lock_event },
    { "fz_door_lock_pin_code",     fz_door_lock_pin_code },
    { "fz_door_lock_user_status",  fz_door_lock_user_status },
    { "fz_electrical_frequency",   fz_electrical_frequency },
    { "fz_electrical_power",       fz_electrical_power },
    { "fz_fan_mode",               fz_fan_mode },
    { "fz_generic_attr",           fz_generic_attr },
    { "fz_humidity",               fz_humidity },
    { "fz_ias_wd",                 fz_ias_wd },
    { "fz_ias_zone_enrollment",    fz_ias_zone_enrollment },
    { "fz_ias_zone_status",        fz_ias_zone_status },
    { "fz_ikea_air_purifier",      fz_ikea_air_purifier },
    { "fz_ikea_voc_index",         fz_ikea_voc_index },
    { "fz_illuminance",            fz_illuminance },
    { "fz_lock_state",             fz_lock_state },
    { "fz_metering",               fz_metering },
    { "fz_multistate_input",       fz_multistate_input },
    { "fz_occupancy",              fz_occupancy },
    { "fz_on_off",                 fz_on_off },
    { "fz_philips_hue_motion",     fz_philips_hue_motion },
    { "fz_pm25",                   fz_pm25 },
    { "fz_power_factor",           fz_power_factor },
    { "fz_power_on_behavior",      fz_power_on_behavior },
    { "fz_pressure",               fz_pressure },
    { "fz_smoke_alarm",            fz_smoke_alarm },
    { "fz_soil_moisture",          fz_soil_moisture },
    { "fz_temperature",            fz_temperature },
    { "fz_thermostat_local_temp",  fz_thermostat_local_temp },
    { "fz_thermostat_running_state", fz_thermostat_running_state },
    { "fz_thermostat_setpoint",    fz_thermostat_setpoint },
    { "fz_thermostat_system_mode", fz_thermostat_system_mode },
    { "fz_thermostat_weekly_schedule", fz_thermostat_weekly_schedule },
    { "fz_tuya_dp",                fz_tuya_dp },
    { "fz_uint16",                 fz_uint16 },
    { "fz_vibration_action",       fz_vibration_action },
    { "fz_vibration_angle",        fz_vibration_angle },
    { "fz_vibration_strength",     fz_vibration_strength },
    { "fz_voc",                    fz_voc },
    { "fz_voltage",                fz_voltage },
    { "fz_water_leak",             fz_water_leak },
    { "fz_xiaomi_ff01",            fz_xiaomi_ff01 },
    { "fz_xiaomi_voltage",         fz_xiaomi_voltage },
};

#define FZ_TABLE_SIZE (sizeof(s_fz_table) / sizeof(s_fz_table[0]))

/* ============================================================================
 * toZigbee function table — MUST be sorted alphabetically by name
 * ============================================================================ */

typedef struct {
    const char *name;
    tz_convert_fn_t fn;
} tz_entry_t;

static const tz_entry_t s_tz_table[] = {
    { "tz_aqara_opple",            tz_aqara_opple },
    { "tz_brightness",             tz_brightness },
    { "tz_color_hs",               tz_color_hs },
    { "tz_color_temp",             tz_color_temp },
    { "tz_color_xy",               tz_color_xy },
    { "tz_cover_command",          tz_cover_command },
    { "tz_cover_position",         tz_cover_position },
    { "tz_cover_tilt",             tz_cover_tilt },
    { "tz_door_lock_pin",          tz_door_lock_pin },
    { "tz_effect",                 tz_effect },
    { "tz_fan_mode",               tz_fan_mode },
    { "tz_generic_write_attr",     tz_generic_write_attr },
    { "tz_ias_zone_enroll_response", tz_ias_zone_enroll_response },
    { "tz_identify",               tz_identify },
    { "tz_identify_effect",        tz_identify_effect },
    { "tz_lock_command",           tz_lock_command },
    { "tz_on_off",                 tz_on_off },
    { "tz_power_on_behavior",      tz_power_on_behavior },
    { "tz_thermostat_setpoint",    tz_thermostat_setpoint },
    { "tz_thermostat_system_mode", tz_thermostat_system_mode },
    { "tz_thermostat_weekly_schedule", tz_thermostat_weekly_schedule },
    { "tz_tuya_command",           tz_tuya_command },
    { "tz_tuya_dp",                tz_tuya_dp },
    { "tz_warning",                tz_warning },
    { "tz_xiaomi_sensitivity",     tz_xiaomi_sensitivity },
};

#define TZ_TABLE_SIZE (sizeof(s_tz_table) / sizeof(s_tz_table[0]))

/* ============================================================================
 * Binary Search Lookup
 * ============================================================================ */

fz_convert_fn_t zb_fn_registry_find_fz(const char *name)
{
    if (name == NULL) return NULL;

    int lo = 0, hi = (int)FZ_TABLE_SIZE - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, s_fz_table[mid].name);
        if (cmp == 0) return s_fz_table[mid].fn;
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

tz_convert_fn_t zb_fn_registry_find_tz(const char *name)
{
    if (name == NULL) return NULL;

    int lo = 0, hi = (int)TZ_TABLE_SIZE - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, s_tz_table[mid].name);
        if (cmp == 0) return s_tz_table[mid].fn;
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

size_t zb_fn_registry_fz_count(void)
{
    return FZ_TABLE_SIZE;
}

size_t zb_fn_registry_tz_count(void)
{
    return TZ_TABLE_SIZE;
}
