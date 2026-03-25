#!/usr/bin/env python3
"""
convert_db.py — Convert z2m_raw.json + zha_raw.json → LittleFS JSON files

Main conversion pipeline:
1. Load raw extracted databases
2. Deduplicate (z2m preferred, ZHA fills gaps)
3. Map z2m fz/tz names and cluster names → our fn_registry names
4. Convert exposes → our zb_expose_t JSON format
5. Set quirk_flags from device characteristics
6. Split by manufacturer prefix → files ≤100KB
7. Generate index.json v2

Usage:
    python3 convert_db.py [--z2m data/z2m_raw.json] [--zha data/zha_raw.json] [--output data/output/]
"""

import argparse
import json
import os
import re
import sys
from collections import defaultdict
from datetime import date
from pathlib import Path

# ============================================================================
# z2m fz/tz converter name → our fn_registry name mapping
# ============================================================================

FZ_NAME_MAP = {
    # Standard ZCL
    "fz.on_off": "fz_on_off",
    "fz.brightness": "fz_brightness",
    "fz.color_colortemp": ["fz_color_temp", "fz_color_xy"],
    "fz.temperature": "fz_temperature",
    "fz.humidity": "fz_humidity",
    "fz.pressure": "fz_pressure",
    "fz.illuminance": "fz_illuminance",
    "fz.battery": "fz_battery",
    "fz.ias_contact_alarm_1": "fz_ias_zone_status",
    "fz.ias_contact_alarm_2": "fz_ias_zone_status",
    "fz.ias_occupancy_alarm_1": "fz_ias_zone_status",
    "fz.ias_occupancy_alarm_2": "fz_ias_zone_status",
    "fz.ias_water_leak_alarm_1": "fz_water_leak",
    "fz.ias_water_leak_alarm_2": "fz_water_leak",
    "fz.ias_smoke_alarm_1": "fz_smoke_alarm",
    "fz.ias_smoke_alarm_2": "fz_smoke_alarm",
    "fz.ias_gas_alarm_1": "fz_ias_zone_status",
    "fz.ias_gas_alarm_2": "fz_ias_zone_status",
    "fz.ias_carbon_monoxide_alarm_1": "fz_ias_zone_status",
    "fz.ias_carbon_monoxide_alarm_2": "fz_ias_zone_status",
    "fz.ias_vibration_alarm_1": "fz_ias_zone_status",
    "fz.ias_zone_alarm_1": "fz_ias_zone_status",
    "fz.ias_zone_alarm_2": "fz_ias_zone_status",
    "fz.occupancy": "fz_occupancy",
    "fz.electrical_measurement": "fz_electrical_power",
    "fz.metering": "fz_metering",
    "fz.thermostat": "fz_thermostat_local_temp",
    "fz.thermostat_local_temperature": "fz_thermostat_local_temp",
    "fz.thermostat_system_mode": "fz_thermostat_system_mode",
    "fz.thermostat_running_state": "fz_thermostat_running_state",
    "fz.cover_position_tilt": ["fz_cover_position", "fz_cover_tilt"],
    "fz.cover_position_via_brightness": "fz_cover_position",
    "fz.lock": "fz_lock_state",
    "fz.fan": "fz_fan_mode",
    "fz.fan_mode": "fz_fan_mode",
    "fz.power_on_behavior": "fz_power_on_behavior",
    "fz.power_outage_memory": "fz_power_on_behavior",
    "fz.smoke_alarm": "fz_smoke_alarm",
    "fz.water_leak": "fz_water_leak",
    "fz.tamper": "fz_ias_zone_status",
    # Vendor-specific
    "fz.xiaomi_basic": "fz_xiaomi_ff01",
    "fz.aqara_opple": "fz_aqara_opple",
    "fz.ikea_air_purifier": "fz_ikea_air_purifier",
    "fz.philips_hue_motion": "fz_philips_hue_motion",
    # Tuya
    "tuya.fz.datapoints": "fz_tuya_dp",
    "fz.tuya_data_point_convert": "fz_tuya_dp",
    "fz.tuya_switch": "fz_tuya_dp",
    "fz.tuya_thermostat": "fz_tuya_dp",
    "fz.tuya_dimmer": "fz_tuya_dp",
    # Commands (action-based)
    "fz.command_on": "fz_on_off",
    "fz.command_off": "fz_on_off",
    "fz.command_toggle": "fz_on_off",
    "fz.command_move": "fz_brightness",
    "fz.command_step": "fz_brightness",
    "fz.command_stop": "fz_brightness",
    # Level config — metadata, not state
    "fz.level_config": None,
    # Additional command converters → map to existing handlers
    "fz.command_move_to_level": "fz_brightness",
    "fz.command_move_to_color_temp": "fz_color_temp",
    "fz.command_move_to_color": "fz_color_xy",
    "fz.command_recall": None,       # scene recall — handled elsewhere
    "fz.command_step_color_temp": "fz_color_temp",
    "fz.command_step_color_temperature": "fz_color_temp",
    "fz.command_move_color_temperature": "fz_color_temp",
    "fz.command_color_loop_set": "fz_color_xy",
    # Hue/saturation command converters
    "fz.command_move_hue": "fz_color_hs",
    "fz.command_move_to_hue": "fz_color_hs",
    "fz.command_move_to_saturation": "fz_color_hs",
    "fz.command_move_to_hue_and_saturation": "fz_color_hs",
    "fz.command_enhanced_move_to_hue_and_saturation": "fz_color_hs",
    "fz.command_step_hue": "fz_color_hs",
    "fz.command_step_saturation": "fz_color_hs",
    "fz.command_stop_move_step": None,  # stop command — no state change
    # Lock events
    "fz.lock_operation_event": "fz_door_lock_event",
    "fz.lock_pin_code_response": "fz_door_lock_pin_code",
    "fz.lock_programming_event": "fz_door_lock_event",
    "fz.lock_user_status_response": "fz_door_lock_event",
    # HVAC
    "fz.hvac_user_interface": None,    # config, not state
    "fz.thermostat_weekly_schedule": None,
    # Tuya variants → generic Tuya DP handler
    "tuya.fz.power_on_behavior_1": "fz_power_on_behavior",
    "tuya.fz.power_on_behavior_2": "fz_power_on_behavior",
    "tuya.fz.power_outage_memory": "fz_power_on_behavior",
    "tuya.fz.brightness": "fz_brightness",
    "tuya.fz.indicator_mode": "fz_tuya_dp",
    "tuya.fz.switch_type": "fz_tuya_dp",
    "tuya.fz.on_off_countdown": "fz_tuya_dp",
    "tuya.fz.backlight_mode_off_on": "fz_tuya_dp",
    # No-op / ignored
    "fz.ignore_basic_report": None,
    "fz.ignore_tuya_set_time": None,
    "fz.ignore_occupancy_report": None,
    "fz.ignore_temperature_report": None,
    "fz.ignore_humidity_report": None,
    "fz.identify": None,
    "fz.linkquality_from_basic": None,
}

TZ_NAME_MAP = {
    "tz.on_off": "tz_on_off",
    "tz.light_onoff_brightness": ["tz_on_off", "tz_brightness"],
    "tz.light_onoff_brightness_colortemp": ["tz_on_off", "tz_brightness", "tz_color_temp"],
    "tz.light_onoff_brightness_colortemp_color": ["tz_on_off", "tz_brightness", "tz_color_temp", "tz_color_xy"],
    "tz.light_onoff_brightness_color": ["tz_on_off", "tz_brightness", "tz_color_hs"],
    "tz.cover_position_tilt": ["tz_cover_position", "tz_cover_tilt"],
    "tz.cover_state": "tz_cover_command",
    "tz.cover_via_brightness": "tz_cover_position",
    "tz.thermostat_occupied_heating_setpoint": "tz_thermostat_setpoint",
    "tz.thermostat_system_mode": "tz_thermostat_system_mode",
    "tz.lock": "tz_lock_command",
    "tz.fan_mode": "tz_fan_mode",
    "tz.power_on_behavior": "tz_power_on_behavior",
    "tz.power_outage_memory": "tz_power_on_behavior",
    "tz.warning": "tz_warning",
    "tz.identify": None,       # handled generically by firmware
    # Tuya
    "tuya.tz.datapoints": "tz_tuya_dp",
    "tz.tuya_data_point_test": "tz_tuya_dp",
    "tz.tuya_switch_state": "tz_tuya_dp",
    "tz.tuya_thermostat": "tz_tuya_dp",
    "tz.tuya_dimmer_state": "tz_tuya_dp",
    "tz.tuya_dimmer_level": "tz_tuya_dp",
    # Color
    "tz.light_colortemp": "tz_color_temp",
    "tz.light_color": "tz_color_hs",
    "tz.effect": None,         # handled generically by firmware
    # No-ops — firmware handles internally
    "tz.ignore_transition": None,
    "tz.ignore_rate": None,
    "tz.level_config": None,
    "tz.light_color_mode": None,
    "tz.light_color_options": None,
    # Light move/step → map to existing handlers
    "tz.light_brightness_move": "tz_brightness",
    "tz.light_brightness_step": "tz_brightness",
    "tz.light_colortemp_move": "tz_color_temp",
    "tz.light_colortemp_step": "tz_color_temp",
    "tz.light_colortemp_startup": "tz_color_temp",
    "tz.light_hue_saturation_move": "tz_color_hs",
    "tz.light_hue_saturation_step": "tz_color_hs",
    # Lock advanced
    "tz.lock_auto_relock_time": "tz_lock_command",
    "tz.lock_sound_volume": "tz_lock_command",
    # Tuya variants
    "tuya.tz.power_on_behavior_1": "tz_power_on_behavior",
    "tuya.tz.power_on_behavior_2": "tz_power_on_behavior",
    "tuya.tz.backlight_indicator_mode_1": "tz_tuya_dp",
    "tuya.tz.backlight_indicator_mode_2": "tz_tuya_dp",
    # Combined color + color_temp
    "tz.light_color_colortemp": ["tz_color_hs", "tz_color_temp"],
    # Electrical measurement / metering readback (not commands, but tz entries exist)
    "tz.currentsummdelivered": None,
    "tz.currentsummreceived": None,
    "tz.electrical_measurement_power": None,
    "tz.acvoltage": None,
    "tz.accurrent": None,
    "tz.frequency": None,
    "tz.powerfactor": None,
    "tz.metering_power": None,
    # Thermostat extra
    "tz.thermostat_local_temperature": None,       # read-only, not a command
    "tz.thermostat_local_temperature_calibration": "tz_thermostat_setpoint",
    "tz.thermostat_running_state": None,           # read-only status
    "tz.thermostat_control_sequence_of_operation": "tz_thermostat_system_mode",
    "tz.thermostat_unoccupied_heating_setpoint": "tz_thermostat_setpoint",
    "tz.thermostat_occupied_cooling_setpoint": "tz_thermostat_setpoint",
    "tz.thermostat_keypad_lockout": None,
    "tz.thermostat_weekly_schedule": None,
    "tz.thermostat_max_heat_setpoint_limit": None,
    "tz.thermostat_min_heat_setpoint_limit": None,
    "tz.thermostat_running_mode": None,
    # Lock extra
    "tz.pincode_lock": "tz_lock_command",
    "tz.lock_userstatus": "tz_lock_command",
    # Tuya extra
    "tuya.tz.do_not_disturb": "tz_tuya_dp",
    "tuya.tz.on_off_countdown": "tz_tuya_dp",
    "tuya.tz.switch_type": "tz_tuya_dp",
    # Xiaomi/Aqara special
    "tz.xiaomi_sensitivity": "tz_xiaomi_sensitivity",
    # Generic / handled by firmware — skip to save space
    "tz.factory_reset": None,
    "tz.scene_rename": None,
    "tz.write": None,
    "tz.command": None,
    "tz.scene_store": None,
    "tz.scene_recall": None,
    "tz.scene_add": None,
    "tz.scene_remove": None,
    "tz.scene_remove_all": None,
    "tz.scene_membership": None,
    "tz.read": None,
    "tz.ota_update": None,
    "tz.zcl_command": None,
}

# z2m cluster string name → cluster ID
CLUSTER_NAME_TO_ID = {
    "genOnOff": 0x0006,
    "genLevelCtrl": 0x0008,
    "lightingColorCtrl": 0x0300,
    "msTemperatureMeasurement": 0x0402,
    "msRelativeHumidity": 0x0405,
    "msPressureMeasurement": 0x0403,
    "msIlluminanceMeasurement": 0x0400,
    "msOccupancySensing": 0x0406,
    "genPowerCfg": 0x0001,
    "ssIasZone": 0x0500,
    "haElectricalMeasurement": 0x0B04,
    "seMetering": 0x0702,
    "hvacThermostat": 0x0201,
    "hvacFanCtrl": 0x0202,
    "closuresWindowCovering": 0x0102,
    "closuresDoorLock": 0x0101,
    "manuSpecificTuya": 0xEF00,
    "genBasic": 0x0000,
    "genIdentify": 0x0003,
    "genGroups": 0x0004,
    "genScenes": 0x0005,
    "genAnalogInput": 0x000C,
    "genMultistateInput": 0x0012,
    "genBinaryInput": 0x000F,
    "msCO2": 0x040D,
    "pm25Measurement": 0x042A,
    "genDeviceTempCfg": 0x0002,
    "ssIasWd": 0x0502,
    "genDiagnostics": 0x0B05,
}

# ============================================================================
# Expose type mapping: z2m type → our zb_expose_type_t enum value
# ============================================================================

Z2M_TYPE_MAP = {
    "light": 0,             # ZB_EXPOSE_LIGHT
    "switch": 1,            # ZB_EXPOSE_SWITCH
    "numeric": 2,           # ZB_EXPOSE_SENSOR (default, may become NUMBER if writable)
    "binary": 3,            # ZB_EXPOSE_BINARY_SENSOR (default, may become SWITCH if writable)
    "cover": 4,             # ZB_EXPOSE_COVER
    "lock": 5,              # ZB_EXPOSE_LOCK
    "climate": 6,           # ZB_EXPOSE_CLIMATE
    "fan": 7,               # ZB_EXPOSE_FAN
    "enum": 8,              # ZB_EXPOSE_SELECT
    "text": 11,             # ZB_EXPOSE_TEXT
    "composite": None,      # Flatten to sub-exposes
}

# ZHA expose type → our type
ZHA_TYPE_MAP = {
    "sensor": 2,            # ZB_EXPOSE_SENSOR
    "binary_sensor": 3,     # ZB_EXPOSE_BINARY_SENSOR
    "switch": 1,            # ZB_EXPOSE_SWITCH
    "light": 0,             # ZB_EXPOSE_LIGHT
    "light_color": 0,       # ZB_EXPOSE_LIGHT (with color features)
    "cover": 4,             # ZB_EXPOSE_COVER
    "lock": 5,              # ZB_EXPOSE_LOCK
    "climate": 6,           # ZB_EXPOSE_CLIMATE
    "fan": 7,               # ZB_EXPOSE_FAN
    "tuya": None,           # Tuya cluster — skip (handled by fz_tuya_dp)
}

# Feature flags
ZB_FEATURE_BRIGHTNESS = 1 << 0
ZB_FEATURE_COLOR_TEMP = 1 << 1
ZB_FEATURE_COLOR_XY   = 1 << 2
ZB_FEATURE_COLOR_HS   = 1 << 3
ZB_FEATURE_TRANSITION = 1 << 4
ZB_FEATURE_POSITION   = 1 << 5
ZB_FEATURE_TILT       = 1 << 6

# ============================================================================
# Device class inference from property name / unit
# ============================================================================

DEVICE_CLASS_MAP = {
    "temperature": "temperature",
    "humidity": "humidity",
    "pressure": "atmospheric_pressure",
    "battery": "battery",
    "illuminance": "illuminance",
    "illuminance_lux": "illuminance",
    "power": "power",
    "energy": "energy",
    "voltage": "voltage",
    "current": "current",
    "co2": "carbon_dioxide",
    "voc": "volatile_organic_compounds",
    "pm25": "pm25",
    "formaldehyde": "volatile_organic_compounds",
    "soil_moisture": "moisture",
    "contact": "door",
    "occupancy": "motion",
    "water_leak": "moisture",
    "smoke": "smoke",
    "vibration": "vibration",
    "gas": "gas",
    "carbon_monoxide": "carbon_monoxide",
    "tamper": "tamper",
    "action": None,  # Actions don't have a device class
}

STATE_CLASS_MAP = {
    "temperature": "measurement",
    "humidity": "measurement",
    "pressure": "measurement",
    "battery": "measurement",
    "illuminance": "measurement",
    "power": "measurement",
    "voltage": "measurement",
    "current": "measurement",
    "co2": "measurement",
    "voc": "measurement",
    "pm25": "measurement",
    "energy": "total_increasing",
    "soil_moisture": "measurement",
}

ICON_MAP = {
    "temperature": "mdi:thermometer",
    "humidity": "mdi:water-percent",
    "pressure": "mdi:gauge",
    "battery": "mdi:battery",
    "illuminance": "mdi:brightness-5",
    "power": "mdi:flash",
    "energy": "mdi:lightning-bolt",
    "voltage": "mdi:sine-wave",
    "current": "mdi:current-ac",
    "co2": "mdi:molecule-co2",
    "voc": "mdi:air-filter",
    "pm25": "mdi:air-filter",
    "soil_moisture": "mdi:water",
    "linkquality": "mdi:signal",
}


def infer_device_class(prop_name, unit=None):
    """Infer HA device_class from property name and unit."""
    if prop_name in DEVICE_CLASS_MAP:
        return DEVICE_CLASS_MAP[prop_name]
    if unit:
        unit_map = {
            "°C": "temperature", "°F": "temperature",
            "hPa": "atmospheric_pressure", "mbar": "atmospheric_pressure",
            "lx": "illuminance",
            "W": "power", "kW": "power",
            "kWh": "energy", "Wh": "energy",
            "V": "voltage",
            "A": "current", "mA": "current",
            "ppm": "carbon_dioxide",
            "ppb": "volatile_organic_compounds",
            "µg/m³": "pm25",
            "%": None,  # Too ambiguous
        }
        return unit_map.get(unit)
    return None


def infer_state_class(prop_name):
    """Infer HA state_class from property name."""
    return STATE_CLASS_MAP.get(prop_name)


def infer_icon(prop_name):
    """Infer MDI icon from property name."""
    return ICON_MAP.get(prop_name)


# ============================================================================
# z2m fz converter name → fz entries with cluster/attr
# ============================================================================

# Map z2m cluster names to (cluster_id, attr_id, key) tuples for fz entries
FZ_CLUSTER_MAP = {
    "fz_on_off": [{"c": 0x0006, "a": 0x0000, "k": "state"}],
    "fz_brightness": [{"c": 0x0008, "a": 0x0000, "k": "brightness"}],
    "fz_color_temp": [{"c": 0x0300, "a": 0x0007, "k": "color_temp"}],
    "fz_color_xy": [{"c": 0x0300, "a": 0x0003, "k": "color_x"}, {"c": 0x0300, "a": 0x0004, "k": "color_y"}],
    "fz_color_hs": [{"c": 0x0300, "a": 0x0000, "k": "color_hue"}, {"c": 0x0300, "a": 0x0001, "k": "color_saturation"}],
    "fz_temperature": [{"c": 0x0402, "a": 0x0000, "k": "temperature"}],
    "fz_humidity": [{"c": 0x0405, "a": 0x0000, "k": "humidity"}],
    "fz_pressure": [{"c": 0x0403, "a": 0x0000, "k": "pressure"}],
    "fz_illuminance": [{"c": 0x0400, "a": 0x0000, "k": "illuminance"}],
    "fz_occupancy": [{"c": 0x0406, "a": 0x0000, "k": "occupancy"}],
    "fz_battery": [{"c": 0x0001, "a": 0x0021, "k": "battery"}],
    "fz_ias_zone_status": [{"c": 0x0500, "a": 0x0002, "k": "alarm"}],
    "fz_water_leak": [{"c": 0x0500, "a": 0x0002, "k": "water_leak"}],
    "fz_smoke_alarm": [{"c": 0x0500, "a": 0x0002, "k": "smoke"}],
    "fz_electrical_power": [{"c": 0x0B04, "a": 0x050B, "k": "power"}],
    "fz_metering": [{"c": 0x0702, "a": 0x0000, "k": "energy"}],
    "fz_thermostat_local_temp": [{"c": 0x0201, "a": 0x0000, "k": "local_temperature"}],
    "fz_thermostat_system_mode": [{"c": 0x0201, "a": 0x001C, "k": "system_mode"}],
    "fz_thermostat_running_state": [{"c": 0x0201, "a": 0x0029, "k": "running_state"}],
    "fz_cover_position": [{"c": 0x0102, "a": 0x0008, "k": "position"}],
    "fz_cover_tilt": [{"c": 0x0102, "a": 0x0009, "k": "tilt"}],
    "fz_lock_state": [{"c": 0x0101, "a": 0x0000, "k": "state"}],
    "fz_door_lock_event": [{"c": 0x0101, "a": 0xFFFF, "k": "lock_action"}],
    "fz_door_lock_pin_code": [{"c": 0x0101, "a": 0xFFFF, "k": "pin_code"}],
    "fz_fan_mode": [{"c": 0x0202, "a": 0x0000, "k": "fan_mode"}],
    "fz_power_on_behavior": [{"c": 0x0006, "a": 0x4003, "k": "power_on_behavior"}],
    "fz_voltage": [{"c": 0x0B04, "a": 0x0505, "k": "voltage"}],
    "fz_current": [{"c": 0x0B04, "a": 0x0508, "k": "current"}],
    "fz_tuya_dp": [{"c": 0xEF00, "a": 0xFFFF, "k": "tuya_dp"}],
    "fz_xiaomi_ff01": [{"c": 0x0000, "a": 0xFF01, "k": "xiaomi_ff01"}],
    "fz_aqara_opple": [{"c": 0xFCC0, "a": 0xFFFF, "k": "aqara_opple"}],
    "fz_ikea_air_purifier": [{"c": 0xFC7D, "a": 0xFFFF, "k": "air_purifier"}],
    "fz_philips_hue_motion": [{"c": 0xFC00, "a": 0xFFFF, "k": "hue_motion"}],
    "fz_generic_attr": [{"c": 0x0000, "a": 0xFFFF, "k": "generic"}],
}

TZ_CLUSTER_MAP = {
    "tz_on_off": {"k": "state", "c": 0x0006},
    "tz_brightness": {"k": "brightness", "c": 0x0008},
    "tz_color_temp": {"k": "color_temp", "c": 0x0300},
    "tz_color_xy": {"k": "color_xy", "c": 0x0300},
    "tz_color_hs": {"k": "color", "c": 0x0300},
    "tz_thermostat_setpoint": {"k": "occupied_heating_setpoint", "c": 0x0201},
    "tz_thermostat_system_mode": {"k": "system_mode", "c": 0x0201},
    "tz_cover_position": {"k": "position", "c": 0x0102},
    "tz_cover_tilt": {"k": "tilt", "c": 0x0102},
    "tz_cover_command": {"k": "state", "c": 0x0102},
    "tz_lock_command": {"k": "state", "c": 0x0101},
    "tz_fan_mode": {"k": "fan_mode", "c": 0x0202},
    "tz_power_on_behavior": {"k": "power_on_behavior", "c": 0x0006},
    "tz_tuya_dp": {"k": "tuya_dp", "c": 0xEF00},
    "tz_tuya_command": {"k": "tuya_cmd", "c": 0xEF00},
    "tz_warning": {"k": "warning", "c": 0x0502},
    "tz_identify": {"k": "identify", "c": 0x0003},
    "tz_effect": {"k": "effect", "c": 0x0006},
    "tz_generic_write_attr": {"k": "generic", "c": 0x0000},
    "tz_xiaomi_sensitivity": {"k": "sensitivity", "c": 0x0101},
}


# ============================================================================
# Convert z2m raw device → our format
# ============================================================================

def map_fz_name(z2m_name):
    """Map z2m fz converter name to our fn_registry name(s)."""
    if z2m_name in FZ_NAME_MAP:
        result = FZ_NAME_MAP[z2m_name]
        if result is None:
            return []
        if isinstance(result, list):
            return result
        return [result]
    # Unknown — use generic fallback
    return ["fz_generic_attr"]


def map_tz_name(z2m_name):
    """Map z2m tz converter name to our fn_registry name(s)."""
    if z2m_name in TZ_NAME_MAP:
        result = TZ_NAME_MAP[z2m_name]
        if result is None:
            return []
        if isinstance(result, list):
            return result
        return [result]
    # Unknown — skip (don't waste space with generic fallbacks)
    return []


def resolve_cluster_id(cluster_name):
    """Resolve a z2m cluster name to cluster ID."""
    if isinstance(cluster_name, int):
        return cluster_name
    if isinstance(cluster_name, str):
        if cluster_name.startswith('0x') or cluster_name.isdigit():
            return int(cluster_name, 0)
        return CLUSTER_NAME_TO_ID.get(cluster_name, 0)
    return 0


def build_fz_entries(z2m_device):
    """Build fz entries from z2m raw device fromZigbee array."""
    entries = []
    seen_keys = set()

    # Detect Xiaomi/Aqara vendor for unnamed converter mapping
    vendor = z2m_device.get("vendor", "")
    is_xiaomi = vendor in ("Xiaomi", "Aqara", "LUMI")

    for fz in z2m_device.get("fromZigbee", []):
        name = fz.get("name", "")
        cluster = fz.get("cluster", "")

        # Special case: unnamed Xiaomi fz converters → map by cluster
        if not name and is_xiaomi:
            if cluster == "genBasic":
                name = "fz.xiaomi_basic"   # → fz_xiaomi_ff01
            elif cluster in ("manuSpecificLumi", "65472"):
                name = "fz.aqara_opple"    # → fz_aqara_opple
            elif cluster == "closuresDoorLock":
                # Aqara vibration sensor: proprietary attrs on DoorLock cluster
                vibration_fz = [
                    {"c": 0x0101, "a": 0x0055, "k": "action",   "fn": "fz_vibration_action"},
                    {"c": 0x0101, "a": 0x0503, "k": "angle",    "fn": "fz_uint16"},
                    {"c": 0x0101, "a": 0x0505, "k": "strength", "fn": "fz_vibration_strength"},
                    {"c": 0x0101, "a": 0x0508, "k": "angle_x",  "fn": "fz_vibration_angle"},
                ]
                for vfz in vibration_fz:
                    vkey = (vfz["c"], vfz["k"])
                    if vkey not in seen_keys:
                        seen_keys.add(vkey)
                        entries.append(vfz)
                continue  # skip normal processing for this fz entry

        our_names = map_fz_name(name)
        for our_name in our_names:
            if our_name in FZ_CLUSTER_MAP:
                for entry_template in FZ_CLUSTER_MAP[our_name]:
                    key = (entry_template["c"], entry_template["k"])
                    if key not in seen_keys:
                        seen_keys.add(key)
                        entries.append({
                            "c": entry_template["c"],
                            "a": entry_template["a"],
                            "k": entry_template["k"],
                            "fn": our_name,
                        })
            else:
                # Use cluster info from z2m if available
                cid = resolve_cluster_id(cluster)
                key = (cid, our_name)
                if key not in seen_keys:
                    seen_keys.add(key)
                    entries.append({
                        "c": cid,
                        "a": 0xFFFF,
                        "k": our_name.replace("fz_", ""),
                        "fn": our_name,
                    })

    return entries


def build_tz_entries(z2m_device):
    """Build tz entries from z2m raw device toZigbee array."""
    entries = []
    seen_keys = set()

    # Detect Xiaomi/Aqara vendor for unnamed converter mapping
    vendor = z2m_device.get("vendor", "")
    is_xiaomi = vendor in ("Xiaomi", "Aqara", "LUMI")

    for tz in z2m_device.get("toZigbee", []):
        name = tz.get("name", "")
        keys = tz.get("key", [])

        # Special case: unnamed Xiaomi tz converters → map by key
        if not name and is_xiaomi:
            if "sensitivity" in keys:
                name = "tz.xiaomi_sensitivity"  # will be mapped below

        our_names = map_tz_name(name)
        for our_name in our_names:
            if our_name in TZ_CLUSTER_MAP:
                template = TZ_CLUSTER_MAP[our_name]
                entry_key = template["k"]
                # If z2m provides key names, use the first matching one
                if keys:
                    for k in keys:
                        if k == entry_key or entry_key == "generic":
                            entry_key = k
                            break
                    else:
                        entry_key = keys[0]  # Use first key from z2m

                key = (entry_key, our_name)
                if key not in seen_keys:
                    seen_keys.add(key)
                    entries.append({
                        "k": entry_key,
                        "c": template["c"],
                        "fn": our_name,
                    })

    return entries


def convert_z2m_expose(expose, depth=0):
    """Convert a z2m expose object to our compact JSON format."""
    if depth > 3:
        return []

    z2m_type = expose.get("type", "")

    # Composite types: flatten features into separate exposes
    if z2m_type == "composite":
        results = []
        for feature in expose.get("features", []):
            results.extend(convert_z2m_expose(feature, depth + 1))
        return results

    # For light/climate/fan/cover/lock: produce one expose with features metadata
    if z2m_type in ("light", "cover", "lock", "climate", "fan"):
        result = convert_complex_expose(expose)
        return [result] if result else []

    # Simple types
    our_type = Z2M_TYPE_MAP.get(z2m_type)
    if our_type is None:
        return []

    access = expose.get("access", 1)

    # Determine final type based on access (writable numeric → NUMBER, etc.)
    if z2m_type == "numeric":
        our_type = 9 if (access & 2) else 2  # NUMBER vs SENSOR
    elif z2m_type == "binary":
        our_type = 1 if (access & 2) else 3  # SWITCH vs BINARY_SENSOR

    prop = expose.get("property", expose.get("name", ""))
    result = {"t": our_type, "ac": access}

    if expose.get("name"):
        result["n"] = expose["name"]
    if prop:
        result["p"] = prop

    # Device class
    dc = expose.get("device_class") or infer_device_class(prop, expose.get("unit"))
    if dc:
        result["dc"] = dc

    # Unit
    if expose.get("unit"):
        result["u"] = expose["unit"]

    # State class
    sc = infer_state_class(prop)
    if sc:
        result["sc"] = sc

    # Skip icon and description to save space — firmware infers these from device_class

    # Type-specific data
    if our_type == 9:  # NUMBER
        num = {}
        if expose.get("value_min") is not None:
            num["min"] = expose["value_min"]
        if expose.get("value_max") is not None:
            num["max"] = expose["value_max"]
        if expose.get("value_step") is not None:
            num["step"] = expose["value_step"]
        if num:
            result["num"] = num
    elif our_type == 8:  # SELECT
        if expose.get("values"):
            vals = expose["values"]
            # Limit to 16 values max to save space
            if len(vals) > 16:
                vals = vals[:16]
            result["sel"] = {"v": vals}
    elif our_type == 3:  # BINARY_SENSOR
        if expose.get("value_on") is not None or expose.get("value_off") is not None:
            result["bin"] = {
                "von": str(expose.get("value_on", "true")),
                "voff": str(expose.get("value_off", "false")),
            }

    return [result]


def convert_complex_expose(expose):
    """Convert light/cover/climate/fan/lock expose with features."""
    z2m_type = expose.get("type", "")
    our_type = Z2M_TYPE_MAP.get(z2m_type)
    if our_type is None:
        return None

    features = expose.get("features", [])
    feature_mask = 0
    access = expose.get("access", 3)

    # Analyze features for feature flags
    for feat in features:
        feat_name = feat.get("name", feat.get("property", ""))
        if feat_name == "brightness":
            feature_mask |= ZB_FEATURE_BRIGHTNESS
        elif feat_name in ("color_temp", "color_temp_startup"):
            feature_mask |= ZB_FEATURE_COLOR_TEMP
        elif feat_name in ("color_xy", "color"):
            feature_mask |= ZB_FEATURE_COLOR_XY
        elif feat_name in ("color_hs",):
            feature_mask |= ZB_FEATURE_COLOR_HS
        elif feat_name == "position":
            feature_mask |= ZB_FEATURE_POSITION
        elif feat_name == "tilt":
            feature_mask |= ZB_FEATURE_TILT

    prop = expose.get("property", expose.get("name", "state"))
    result = {
        "t": our_type,
        "f": feature_mask,
        "p": prop,
        "ac": access,
    }

    if expose.get("name"):
        result["n"] = expose["name"]

    return result


def convert_zha_expose(expose):
    """Convert a ZHA-derived expose to our format."""
    zha_type = expose.get("type", "")
    our_type = ZHA_TYPE_MAP.get(zha_type)
    if our_type is None:
        return None

    prop = expose.get("property", "")
    result = {"t": our_type, "p": prop, "ac": 1}

    if expose.get("device_class"):
        result["dc"] = expose["device_class"]
    if expose.get("unit"):
        result["u"] = expose["unit"]

    sc = infer_state_class(prop)
    if sc:
        result["sc"] = sc

    # ZHA light with features
    if zha_type == "light":
        result["f"] = ZB_FEATURE_BRIGHTNESS
        result["ac"] = 3
        if expose.get("features"):
            for feat in expose["features"]:
                if feat == "color_temp":
                    result["f"] |= ZB_FEATURE_COLOR_TEMP
                elif feat in ("color_xy", "color"):
                    result["f"] |= ZB_FEATURE_COLOR_XY

    elif zha_type == "switch":
        result["ac"] = 3

    elif zha_type == "cover":
        result["f"] = ZB_FEATURE_POSITION
        result["ac"] = 3

    elif zha_type in ("lock", "fan", "climate"):
        result["ac"] = 3

    return result


def determine_quirk_flags(z2m_device):
    """Determine quirk flags from device characteristics."""
    flags = 0

    # Check for Tuya
    fp_list = z2m_device.get("fingerprints", [])
    for fp in fp_list:
        manuf = fp.get("manufacturerName", "") or ""
        if manuf.startswith("_TZ") or manuf.startswith("_TH"):
            flags |= 0x10  # ZB_QUIRK_TUYA_DEVICE

    meta = z2m_device.get("meta", {})
    if meta:
        if meta.get("tuyaDatapoints"):
            flags |= 0x10 | 0x40  # ZB_QUIRK_TUYA_DEVICE | ZB_QUIRK_TUYA_MCU_INIT
        if meta.get("multiEndpoint"):
            flags |= 0x200  # ZB_QUIRK_MULTI_ENDPOINT

    return flags


# z2m "vendor" name → actual Zigbee Basic cluster manufacturer string.
# Many z2m vendors use human-readable names that differ from what the
# device actually reports.  The firmware stores the Zigbee string from
# the interview, so the converter DB must use the same string or the
# loader's find_filename() won't locate the right JSON file.
VENDOR_TO_ZIGBEE_MFR = {
    "Aqara": "LUMI",
    "Xiaomi": "LUMI",
    "IKEA": "IKEA of Sweden",
    "Sonoff": "eWeLink",
    "Innr": "innr",
    "Third Reality": "Third Reality, Inc",
    "Sengled": "Sengled",
    "SmartThings": "SmartThings",
    "Keen Home": "Keen Home Inc",
    "Develco": "Develco Products A/S",
    "NodOn": "NodOn",
    "Hive": "Computime",
}


def get_manufacturer_from_device(device):
    """Get the primary manufacturer name from a device.

    Priority:
    1. Fingerprint manufacturerName (exact Zigbee string)
    2. Direct manufacturer field
    3. Vendor name → Zigbee manufacturer mapping
    4. Raw vendor name (fallback)
    """
    # Check fingerprints first (most specific)
    for fp in device.get("fingerprints", []):
        mn = fp.get("manufacturerName")
        if mn:
            return mn
    # Check direct manufacturer field
    mf = device.get("manufacturer", "")
    if mf:
        return mf
    # Map z2m vendor name to Zigbee manufacturer string
    vendor = device.get("vendor", "")
    return VENDOR_TO_ZIGBEE_MFR.get(vendor, vendor)


def convert_z2m_device(z2m_device):
    """Convert a z2m raw device to our output format.

    Returns a list of device entries. For devices with fingerprint modelIDs
    different from the z2m model name, generates entries for BOTH the z2m
    model AND each unique fingerprint modelID. This ensures the firmware
    finds the converter regardless of which model string the device reports.
    """
    model = z2m_device.get("model", "")
    if not model:
        return []

    manufacturer = get_manufacturer_from_device(z2m_device)

    # Build fz/tz entries
    fz_entries = build_fz_entries(z2m_device)
    tz_entries = build_tz_entries(z2m_device)

    # Convert exposes
    our_exposes = []
    for exp in z2m_device.get("exposes", []):
        our_exposes.extend(convert_z2m_expose(exp))

    # Quirk flags
    quirk_flags = determine_quirk_flags(z2m_device)

    def make_entry(m, mf):
        result = {
            "m": m,
            "mf": mf,
            "v": z2m_device.get("vendor", ""),
            "d": z2m_device.get("description", ""),
            "fz": list(fz_entries),
            "tz": list(tz_entries),
            "e": list(our_exposes),
        }
        if quirk_flags:
            result["q"] = quirk_flags
        # Strip empty arrays for compactness
        if not result["fz"]:
            del result["fz"]
        if not result["tz"]:
            del result["tz"]
        if not result["e"]:
            del result["e"]
        return result

    # Use fingerprint modelIDs as primary keys (what firmware actually sees).
    # The z2m marketing model name (e.g. "DJT11LM") is NOT what the device
    # reports — the Zigbee modelID (e.g. "lumi.vibration.aq1") is.
    fingerprints = z2m_device.get("fingerprints", [])
    fp_model_ids = set()
    for fp in fingerprints:
        mid = fp.get("modelID", "")
        if mid:
            fp_model_ids.add(mid)

    if not fp_model_ids:
        # No fingerprints — use z2m model name as fallback
        fp_model_ids.add(model)

    # Generate one entry per unique fingerprint modelID
    results = []
    for mid in fp_model_ids:
        # Use fingerprint-specific manufacturer if available
        fp_mfr = manufacturer
        for fp in fingerprints:
            if fp.get("modelID") == mid and fp.get("manufacturerName"):
                fp_mfr = fp["manufacturerName"]
                break
        results.append(make_entry(mid, fp_mfr))

    return results


def convert_zha_device(zha_device):
    """Convert a ZHA raw device to our output format."""
    model = zha_device.get("model", "")
    manufacturer = zha_device.get("manufacturer", "")
    if not model or not manufacturer:
        return None

    # Convert exposes from cluster-derived types
    our_exposes = []
    for exp in zha_device.get("exposes", []):
        converted = convert_zha_expose(exp)
        if converted:
            our_exposes.append(converted)

    result = {
        "m": model,
        "mf": manufacturer,
        "v": manufacturer,
        "d": f"ZHA quirk: {zha_device.get('quirk_class', '')}",
        "e": our_exposes,
    }

    if not result["e"]:
        del result["e"]

    return result


# ============================================================================
# Manufacturer grouping and file splitting
# ============================================================================

VENDOR_FILE_MAP = {
    # Major brands — vendor name → filename
    "Philips": "philips.json",
    "Signify Netherlands B.V.": "philips.json",
    "Tuya": "tuya_branded.json",
    "Aqara": "xiaomi.json",
    "Innr": "innr.json",
    "IKEA": "ikea.json",
    "Gledopto": "gledopto.json",
    "Schneider Electric": "schneider.json",
    "Namron": "namron.json",
    "Sunricher": "sunricher.json",
    "LEDVANCE": "osram.json",
    "HEIMAN": "heiman.json",
    "OSRAM": "osram.json",
    "ADEO": "adeo.json",
    "ORVIBO": "orvibo.json",
    "ShinaSystem": "shinasystem.json",
    "SONOFF": "sonoff.json",
    "Nue / 3A": "nue.json",
    "Feibit": "feibit.json",
    "Paulmann": "paulmann.json",
    "Iluminize": "iluminize.json",
    "EFEKTA": "efekta.json",
    "Hive": "hive.json",
    "Legrand": "legrand.json",
    "ROBB": "robb.json",
    "Sengled": "sengled.json",
    "SmartThings": "smartthings.json",
    "Develco": "develco.json",
    "Third Reality": "thirdreality.json",
    "Yale": "yale.json",
    "Müller Licht": "muller_licht.json",
    "Sylvania": "sylvania.json",
    "Moes": "moes.json",
    "SMaBiT (Bitron Video)": "smabit.json",
    "Sinopé": "sinope.json",
    "Aurora Lighting": "aurora.json",
    "Immax": "immax.json",
    "Custom devices (DiY)": "diy.json",
    "Dawon DNS": "dawon.json",
    "Bosch": "bosch.json",
    "YOKIS": "yokis.json",
    "Danfoss": "danfoss.json",
    "Eurotronic": "eurotronic.json",
    "Centralite": "centralite.json",
    "Zemismart": "zemismart.json",
    "Xiaomi": "xiaomi.json",
    "LUMI": "xiaomi.json",
    "eWeLink": "sonoff.json",
    "TuYa": "tuya_branded.json",
    "Dresden Elektronik": "dresden.json",
    "LIDL Silvercrest": "lidl.json",
    "Ubisys": "ubisys.json",
    "Candeo": "candeo.json",
}

MAX_FILE_SIZE = 100 * 1024  # 100KB per file


def vendor_to_filename(vendor):
    """Convert a vendor name to a safe filename."""
    name = vendor.lower()
    name = re.sub(r'[^a-z0-9_]', '_', name)
    name = re.sub(r'_+', '_', name).strip('_')
    if not name:
        name = "other"
    return name + ".json"


def get_file_for_vendor(vendor):
    """Determine the output filename for a vendor."""
    if not vendor:
        return "other.json"

    # Direct match
    if vendor in VENDOR_FILE_MAP:
        return VENDOR_FILE_MAP[vendor]

    # Tuya-like vendor names
    if vendor.lower() in ("tuya", "tuyatec"):
        return "tuya_branded.json"

    # Generate from vendor name
    return vendor_to_filename(vendor)


def split_oversized_files(file_groups, max_size=MAX_FILE_SIZE):
    """Split file groups that would exceed max_size."""
    result = {}

    for filename, devices in file_groups.items():
        # Estimate size: ~300 bytes per device on average
        estimated_size = len(json.dumps({"devices": devices}, separators=(',', ':')))
        if estimated_size <= max_size:
            result[filename] = devices
        else:
            # Split into multiple files
            base, ext = os.path.splitext(filename)
            part = 1
            current = []
            current_size = 0

            for dev in devices:
                dev_size = len(json.dumps(dev, separators=(',', ':'))) + 1
                if current_size + dev_size > max_size and current:
                    result[f"{base}_{part}{ext}"] = current
                    current = []
                    current_size = 0
                    part += 1
                current.append(dev)
                current_size += dev_size

            if current:
                if part == 1:
                    result[filename] = current
                else:
                    result[f"{base}_{part}{ext}"] = current

    return result


# ============================================================================
# Built-in converter models to exclude
# ============================================================================

def load_builtin_models():
    """Load (manufacturer, model) pairs of built-in converters to exclude from the DB.

    Returns a set of (manufacturer, model) tuples. Only excludes a z2m device
    when BOTH manufacturer and model match a built-in definition. This prevents
    over-exclusion (e.g. generic TS0001 switch vs Fingerbot TS0001).
    """
    builtin = set()
    conv_dir = Path(__file__).parent.parent / "main" / "zigbee" / "converter" / "converters"
    if conv_dir.exists():
        for f in conv_dir.glob("conv_*.c"):
            try:
                content = f.read_text(encoding='utf-8', errors='replace')
                # Extract (manufacturer, model) pairs from converter definitions
                # Pattern: .manufacturer = "...", ... .model = "..."
                # They appear in struct initializers, so find each def block
                for block in re.finditer(
                    r'\.manufacturer\s*=\s*"([^"]+)"[^}]*?\.model\s*=\s*"([^"]+)"',
                    content, re.DOTALL
                ):
                    builtin.add((block.group(1), block.group(2)))
            except Exception:
                pass
    return builtin


# ============================================================================
# Main pipeline
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="Convert z2m + ZHA → LittleFS JSON")
    parser.add_argument("--z2m", default=os.path.join(os.path.dirname(__file__), "data", "z2m_raw.json"),
                        help="Path to z2m_raw.json")
    parser.add_argument("--zha", default=os.path.join(os.path.dirname(__file__), "data", "zha_raw.json"),
                        help="Path to zha_raw.json (optional)")
    parser.add_argument("--output", default=os.path.join(os.path.dirname(__file__), "data", "output"),
                        help="Output directory for LittleFS JSON files")
    parser.add_argument("--no-zha", action="store_true", help="Skip ZHA processing")
    args = parser.parse_args()

    # Load built-in (manufacturer, model) pairs to exclude
    builtin_models = load_builtin_models()
    if builtin_models:
        print(f"Found {len(builtin_models)} built-in converter (mfr, model) pairs to exclude")
        for mfr, model in sorted(builtin_models):
            print(f"  {mfr} / {model}")

    # --- Load z2m ---
    z2m_devices = []
    if os.path.exists(args.z2m):
        print(f"Loading z2m from {args.z2m}...")
        with open(args.z2m) as f:
            z2m_raw = json.load(f)
        z2m_raw_devices = z2m_raw.get("devices", [])
        print(f"  {len(z2m_raw_devices)} raw z2m devices")

        for dev in z2m_raw_devices:
            converted_list = convert_z2m_device(dev)
            for converted in converted_list:
                # Only exclude when both (manufacturer, model) match a built-in
                mfr_model = (converted.get("mf", ""), converted["m"])
                if mfr_model not in builtin_models:
                    z2m_devices.append(converted)

        print(f"  {len(z2m_devices)} converted z2m devices (after excluding built-in)")
    else:
        print(f"Warning: {args.z2m} not found, skipping z2m", file=sys.stderr)

    # --- Load ZHA ---
    zha_devices = []
    if not args.no_zha and os.path.exists(args.zha):
        print(f"Loading ZHA from {args.zha}...")
        with open(args.zha) as f:
            zha_raw = json.load(f)
        zha_raw_devices = zha_raw.get("devices", [])
        print(f"  {len(zha_raw_devices)} raw ZHA devices")

        for dev in zha_raw_devices:
            converted = convert_zha_device(dev)
            if converted:
                mfr_model = (converted.get("mf", ""), converted["m"])
                if mfr_model not in builtin_models:
                    zha_devices.append(converted)

        print(f"  {len(zha_devices)} converted ZHA devices")

    # --- Deduplicate (z2m preferred, ZHA fills gaps) ---
    z2m_keys = set()
    for dev in z2m_devices:
        z2m_keys.add((dev.get("mf", ""), dev["m"]))

    zha_unique = []
    for dev in zha_devices:
        key = (dev.get("mf", ""), dev["m"])
        if key not in z2m_keys:
            zha_unique.append(dev)

    all_devices = z2m_devices + zha_unique
    print(f"\nTotal: {len(all_devices)} devices ({len(z2m_devices)} z2m + {len(zha_unique)} ZHA-only)")

    # --- Group by vendor → file ---
    file_groups = defaultdict(list)

    for dev in all_devices:
        vendor = dev.get("v", "")
        filename = get_file_for_vendor(vendor)
        file_groups[filename].append(dev)

    # Split oversized files
    file_groups = split_oversized_files(dict(file_groups))

    # --- Build manufacturer→file index ---
    # For each device, collect all known manufacturerName strings and map to the file
    manuf_to_file = {}
    manuf_counts = defaultdict(int)

    for filename, devices in file_groups.items():
        for dev in devices:
            mf = dev.get("mf", "")
            if mf:
                manuf_to_file[mf] = filename
                manuf_counts[mf] += 1
            # Also add vendor name as a fallback mapping
            vendor = dev.get("v", "")
            if vendor and vendor not in manuf_to_file:
                manuf_to_file[vendor] = filename
                manuf_counts[vendor] += manuf_counts.get(vendor, 0)

    # --- Write output files ---
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Clean old output
    for old_file in out_dir.glob("*.json"):
        old_file.unlink()

    total_bytes = 0
    total_devices = 0

    for filename, devices in sorted(file_groups.items()):
        out_path = out_dir / filename
        data = {"devices": devices}
        json_str = json.dumps(data, separators=(',', ':'), ensure_ascii=False)
        out_path.write_text(json_str)

        size = len(json_str)
        total_bytes += size
        total_devices += len(devices)
        print(f"  {filename}: {len(devices)} devices ({size/1024:.1f} KB)")

    # --- Generate index.json v2 ---
    # Build index mapping manufacturerName → file
    manuf_index = {}
    for manuf, filename in manuf_to_file.items():
        manuf_index[manuf] = {
            "file": filename,
            "count": manuf_counts.get(manuf, 0),
        }

    # Check index limit — if >128, keep only entries with count >= 2 or known brands
    MAX_INDEX_ENTRIES = 256  # Increased from 128 (uses ~4KB extra BSS)
    if len(manuf_index) > MAX_INDEX_ENTRIES:
        print(f"\nWarning: {len(manuf_index)} index entries exceeds MAX_INDEX_ENTRIES={MAX_INDEX_ENTRIES}")
        print("  Keeping only entries with count >= 2 or known brands...")

        # Keep entries that have count >= 2 or are known Tuya prefixes
        filtered = {}
        for m, info in manuf_index.items():
            if info["count"] >= 2 or m.startswith("_T") or m in VENDOR_FILE_MAP:
                filtered[m] = info

        # If still too many, sort by count and keep top entries
        if len(filtered) > MAX_INDEX_ENTRIES:
            sorted_entries = sorted(filtered.items(), key=lambda x: x[1]["count"], reverse=True)
            filtered = dict(sorted_entries[:MAX_INDEX_ENTRIES])

        removed = len(manuf_index) - len(filtered)
        manuf_index = filtered
        print(f"  Reduced to {len(manuf_index)} entries (removed {removed})")

    index = {
        "version": 2,
        "total_devices": total_devices,
        "manufacturers": manuf_index,
    }

    index_path = out_dir / "index.json"
    index_json = json.dumps(index, separators=(',', ':'), ensure_ascii=False)
    index_path.write_text(index_json)
    total_bytes += len(index_json)

    print(f"\n--- Summary ---")
    print(f"  Files:        {len(file_groups)} + index.json")
    print(f"  Devices:      {total_devices}")
    print(f"  Manufacturers: {len(manuf_index)} in index")
    print(f"  Total size:   {total_bytes/1024:.1f} KB ({total_bytes/1024/1024:.2f} MB)")
    print(f"  Output dir:   {out_dir}")

    # Warn if total exceeds partition
    if total_bytes > 6.5 * 1024 * 1024:
        print(f"\n  WARNING: Total size exceeds 6.5MB LittleFS partition!")

    # Report expose coverage
    with_exposes = sum(1 for d in all_devices if d.get("e"))
    with_fz = sum(1 for d in all_devices if d.get("fz"))
    with_tz = sum(1 for d in all_devices if d.get("tz"))
    print(f"\n  With exposes: {with_exposes}/{total_devices} ({with_exposes*100//max(total_devices,1)}%)")
    print(f"  With fz:      {with_fz}/{total_devices} ({with_fz*100//max(total_devices,1)}%)")
    print(f"  With tz:      {with_tz}/{total_devices} ({with_tz*100//max(total_devices,1)}%)")


if __name__ == "__main__":
    main()
