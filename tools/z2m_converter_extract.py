#!/usr/bin/env python3
"""
Z2M Converter Database Extractor

Parses zigbee-herdsman-converters TypeScript sources and generates
compact JSON definitions for the ESP32-C5 runtime converter loader.

Usage:
    python3 z2m_converter_extract.py --source /path/to/zhc/src/devices --output data/converters/
    python3 z2m_converter_extract.py --source /path/to/zhc/src/devices --report
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
# ModernExtend → fz_/tz_ mapping
# ============================================================================

MODERN_EXTEND_MAP = {
    # m.onOff()
    "onOff": {
        "fz": [{"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"}],
        "tz": [{"k": "state", "c": 6, "fn": "tz_on_off"}],
        "e": [{"t": 1, "f": 0, "p": "state", "ac": 3}],  # ZB_EXPOSE_SWITCH
    },
    # m.light() — base (on/off + brightness)
    "light": {
        "fz": [
            {"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"},
            {"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"},
        ],
        "tz": [
            {"k": "state", "c": 6, "fn": "tz_on_off"},
            {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        ],
        "e": [{"t": 0, "f": 3, "p": "state", "ac": 3}],  # ZB_EXPOSE_LIGHT, BRIGHTNESS
        "is_light": True,  # Flag: apply colorTemp/color detection on args
    },
    # m.temperature()
    "temperature": {
        "fz": [{"c": 1026, "a": 0, "k": "temperature", "fn": "fz_temperature"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "temperature", "u": "\u00b0C", "sc": "measurement", "p": "temperature", "ac": 1}],
    },
    # m.humidity()
    "humidity": {
        "fz": [{"c": 1029, "a": 0, "k": "humidity", "fn": "fz_humidity"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "humidity", "u": "%", "sc": "measurement", "p": "humidity", "ac": 1}],
    },
    # m.pressure()
    "pressure": {
        "fz": [{"c": 1027, "a": 0, "k": "pressure", "fn": "fz_pressure"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "pressure", "u": "hPa", "sc": "measurement", "p": "pressure", "ac": 1}],
    },
    # m.illuminance()
    "illuminance": {
        "fz": [{"c": 1024, "a": 0, "k": "illuminance", "fn": "fz_illuminance"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "illuminance", "u": "lx", "sc": "measurement", "p": "illuminance", "ac": 1}],
    },
    # m.occupancy()
    "occupancy": {
        "fz": [{"c": 1030, "a": 0, "k": "occupancy", "fn": "fz_occupancy"}],
        "tz": [],
        "e": [{"t": 3, "f": 0, "dc": "motion", "p": "occupancy", "ac": 1}],
    },
    # m.battery()
    "battery": {
        "fz": [{"c": 1, "a": 33, "k": "battery", "fn": "fz_battery"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "battery", "u": "%", "sc": "measurement", "p": "battery", "ac": 1}],
    },
    # m.iasZoneAlarm()
    "iasZoneAlarm": {
        "fz": [{"c": 1280, "a": 2, "k": "alarm", "fn": "fz_ias_zone_status"}],
        "tz": [],
        "e": [{"t": 3, "f": 0, "p": "alarm", "ac": 1}],
    },
    # m.electricalMeasurements()
    "electricalMeasurements": {
        "fz": [
            {"c": 2820, "a": 1291, "k": "power", "fn": "fz_electrical_power"},
            {"c": 2820, "a": 1288, "k": "current", "fn": "fz_current"},
            {"c": 2820, "a": 1285, "k": "voltage", "fn": "fz_voltage"},
        ],
        "tz": [],
        "e": [
            {"t": 2, "f": 0, "dc": "power", "u": "W", "sc": "measurement", "p": "power", "ac": 1},
            {"t": 2, "f": 0, "dc": "current", "u": "A", "sc": "measurement", "p": "current", "ac": 1},
            {"t": 2, "f": 0, "dc": "voltage", "u": "V", "sc": "measurement", "p": "voltage", "ac": 1},
        ],
    },
    # m.electricityMeter() — same as electricalMeasurements
    "electricityMeter": {
        "fz": [
            {"c": 2820, "a": 1291, "k": "power", "fn": "fz_electrical_power"},
            {"c": 2820, "a": 1288, "k": "current", "fn": "fz_current"},
            {"c": 2820, "a": 1285, "k": "voltage", "fn": "fz_voltage"},
            {"c": 1794, "a": 0, "k": "energy", "fn": "fz_metering"},
        ],
        "tz": [],
        "e": [
            {"t": 2, "f": 0, "dc": "power", "u": "W", "sc": "measurement", "p": "power", "ac": 1},
            {"t": 2, "f": 0, "dc": "current", "u": "A", "sc": "measurement", "p": "current", "ac": 1},
            {"t": 2, "f": 0, "dc": "voltage", "u": "V", "sc": "measurement", "p": "voltage", "ac": 1},
            {"t": 2, "f": 0, "dc": "energy", "u": "kWh", "sc": "total_increasing", "p": "energy", "ac": 1},
        ],
    },
    # m.metering()
    "metering": {
        "fz": [{"c": 1794, "a": 0, "k": "energy", "fn": "fz_metering"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "energy", "u": "kWh", "sc": "total_increasing", "p": "energy", "ac": 1}],
    },
    # m.windowCovering()
    "windowCovering": {
        "fz": [
            {"c": 258, "a": 8, "k": "position", "fn": "fz_cover_position"},
            {"c": 258, "a": 9, "k": "tilt", "fn": "fz_cover_tilt"},
        ],
        "tz": [
            {"k": "position", "c": 258, "fn": "tz_cover_position"},
            {"k": "tilt", "c": 258, "fn": "tz_cover_tilt"},
        ],
        "e": [{"t": 4, "f": 96, "p": "position", "ac": 3}],  # ZB_EXPOSE_COVER, POSITION|TILT
    },
    # m.lock()
    "lock": {
        "fz": [{"c": 257, "a": 0, "k": "state", "fn": "fz_lock_state"}],
        "tz": [{"k": "state", "c": 257, "fn": "tz_lock_command"}],
        "e": [{"t": 5, "f": 0, "p": "state", "ac": 3}],
    },
    # m.thermostat()
    "thermostat": {
        "fz": [
            {"c": 513, "a": 0, "k": "local_temperature", "fn": "fz_thermostat_local_temp"},
            {"c": 513, "a": 28, "k": "system_mode", "fn": "fz_thermostat_system_mode"},
            {"c": 513, "a": 41, "k": "running_state", "fn": "fz_thermostat_running_state"},
        ],
        "tz": [
            {"k": "occupied_heating_setpoint", "c": 513, "fn": "tz_thermostat_setpoint"},
            {"k": "system_mode", "c": 513, "fn": "tz_thermostat_system_mode"},
        ],
        "e": [{"t": 6, "f": 0, "p": "local_temperature", "ac": 1}],
    },
    # m.fanMode()
    "fanMode": {
        "fz": [{"c": 514, "a": 0, "k": "fan_mode", "fn": "fz_fan_mode"}],
        "tz": [{"k": "fan_mode", "c": 514, "fn": "tz_fan_mode"}],
        "e": [{"t": 7, "f": 0, "p": "fan_mode", "ac": 3}],
    },
    # m.co2()
    "co2": {
        "fz": [{"c": 1037, "a": 0, "k": "co2", "fn": "fz_co2"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "carbon_dioxide", "u": "ppm", "sc": "measurement", "p": "co2", "ac": 1}],
    },
    # m.vocMeasurement()
    "vocMeasurement": {
        "fz": [{"c": 1070, "a": 0, "k": "voc", "fn": "fz_voc"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "u": "ppb", "sc": "measurement", "p": "voc", "ac": 1}],
    },
    # m.smokeAlarm()
    "smokeAlarm": {
        "fz": [{"c": 1280, "a": 2, "k": "smoke", "fn": "fz_smoke_alarm"}],
        "tz": [],
        "e": [{"t": 3, "f": 0, "dc": "smoke", "p": "smoke", "ac": 1}],
    },
    # m.waterLeak()
    "waterLeak": {
        "fz": [{"c": 1280, "a": 2, "k": "water_leak", "fn": "fz_water_leak"}],
        "tz": [],
        "e": [{"t": 3, "f": 0, "dc": "moisture", "p": "water_leak", "ac": 1}],
    },
    # m.powerOnBehavior()
    "powerOnBehavior": {
        "fz": [{"c": 6, "a": 16387, "k": "power_on_behavior", "fn": "fz_power_on_behavior"}],
        "tz": [{"k": "power_on_behavior", "c": 6, "fn": "tz_power_on_behavior"}],
        "e": [{"t": 8, "f": 0, "p": "power_on_behavior", "ac": 3}],  # SELECT
    },
    # m.pm25Measurement()
    "pm25Measurement": {
        "fz": [{"c": 1066, "a": 0, "k": "pm25", "fn": "fz_pm25"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "pm25", "u": "µg/m³", "sc": "measurement", "p": "pm25", "ac": 1}],
    },
    # m.soilMoisture()
    "soilMoisture": {
        "fz": [{"c": 1032, "a": 0, "k": "soil_moisture", "fn": "fz_soil_moisture"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "moisture", "u": "%", "sc": "measurement", "p": "soil_moisture", "ac": 1}],
    },
    # m.deviceTemperature()
    "deviceTemperature": {
        "fz": [{"c": 2, "a": 0, "k": "device_temperature", "fn": "fz_device_temperature"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "temperature", "u": "°C", "sc": "measurement", "p": "device_temperature", "ac": 1}],
    },
    # m.multistateInput()
    "multistateInput": {
        "fz": [{"c": 18, "a": 85, "k": "action", "fn": "fz_multistate_input"}],
        "tz": [],
        "e": [{"t": 8, "f": 0, "p": "action", "ac": 1}],
    },
    # m.analogInput()
    "analogInput": {
        "fz": [{"c": 12, "a": 85, "k": "analog_value", "fn": "fz_analog_input"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "p": "analog_value", "ac": 1}],
    },
    # m.colorTemperature() — shorthand for light + colorTemp
    "colorTemperature": {
        "fz": [
            {"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"},
            {"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"},
            {"c": 768, "a": 7, "k": "color_temp", "fn": "fz_color_temp"},
        ],
        "tz": [
            {"k": "state", "c": 6, "fn": "tz_on_off"},
            {"k": "brightness", "c": 8, "fn": "tz_brightness"},
            {"k": "color_temp", "c": 768, "fn": "tz_color_temp"},
        ],
        "e": [{"t": 0, "f": 5, "p": "state", "ac": 3}],  # ZB_EXPOSE_LIGHT, BRIGHTNESS|COLOR_TEMP
    },
    # m.iasWarning()
    "iasWarning": {
        "fz": [{"c": 1282, "a": 0, "k": "warning", "fn": "fz_ias_wd"}],
        "tz": [{"k": "warning", "c": 1282, "fn": "tz_warning"}],
        "e": [{"t": 1, "f": 0, "p": "warning", "ac": 3}],
    },
    # m.doorLock()
    "doorLock": {
        "fz": [
            {"c": 257, "a": 0, "k": "state", "fn": "fz_lock_state"},
            {"c": 257, "a": 0, "k": "lock_action", "fn": "fz_door_lock_event"},
        ],
        "tz": [{"k": "state", "c": 257, "fn": "tz_lock_command"}],
        "e": [{"t": 5, "f": 0, "p": "state", "ac": 3}],
    },
    # m.electricalFrequency()
    "electricalFrequency": {
        "fz": [{"c": 2820, "a": 768, "k": "frequency", "fn": "fz_electrical_frequency"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "frequency", "u": "Hz", "sc": "measurement", "p": "frequency", "ac": 1}],
    },
    # ---------------------------------------------------------------
    # Generic ModernExtend that create generic entities from arguments
    # m.numeric() -> sensor or number entity (generic)
    # m.binary() -> binary_sensor or switch entity (generic)
    # m.enumLookup() -> select entity (generic)
    # These use fz_generic_attr / tz_generic_write_attr as a fallback
    # ---------------------------------------------------------------
    "numeric": {
        "fz": [{"c": 0, "a": 0, "k": "numeric_value", "fn": "fz_generic_attr"}],
        "tz": [{"k": "numeric_value", "c": 0, "fn": "tz_generic_write_attr"}],
        "e": [{"t": 2, "f": 0, "p": "numeric_value", "ac": 1}],
    },
    "binary": {
        "fz": [{"c": 0, "a": 0, "k": "binary_value", "fn": "fz_generic_attr"}],
        "tz": [{"k": "binary_value", "c": 0, "fn": "tz_generic_write_attr"}],
        "e": [{"t": 3, "f": 0, "p": "binary_value", "ac": 1}],
    },
    "enumLookup": {
        "fz": [{"c": 0, "a": 0, "k": "enum_value", "fn": "fz_generic_attr"}],
        "tz": [{"k": "enum_value", "c": 0, "fn": "tz_generic_write_attr"}],
        "e": [{"t": 8, "f": 0, "p": "enum_value", "ac": 3}],
    },
    # m.binaryWithOnOffCommand() - binary sensor with on/off command
    "binaryWithOnOffCommand": {
        "fz": [{"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"}],
        "tz": [{"k": "state", "c": 6, "fn": "tz_on_off"}],
        "e": [{"t": 1, "f": 0, "p": "state", "ac": 3}],
    },
    # m.switchActions() — power on behavior variant
    "switchActions": {
        "fz": [{"c": 6, "a": 16387, "k": "power_on_behavior", "fn": "fz_power_on_behavior"}],
        "tz": [{"k": "power_on_behavior", "c": 6, "fn": "tz_power_on_behavior"}],
        "e": [],
    },
    # ---------------------------------------------------------------
    # Tuya-specific ModernExtend patterns
    # ---------------------------------------------------------------
    "tuyaBase": {
        "fz": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
        "tz": [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
        "e": [],
    },
    "tuyaOnOff": {
        "fz": [
            {"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"},
            {"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"},
        ],
        "tz": [
            {"k": "state", "c": 6, "fn": "tz_on_off"},
            {"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"},
        ],
        "e": [{"t": 1, "f": 0, "p": "state", "ac": 3}],
    },
    "tuyaLight": {
        "fz": [
            {"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"},
            {"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"},
        ],
        "tz": [
            {"k": "state", "c": 6, "fn": "tz_on_off"},
            {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        ],
        "e": [{"t": 0, "f": 3, "p": "state", "ac": 3}],
        "is_light": True,
    },
    "tuyaMagicPacket": {
        "fz": [],
        "tz": [],
        "e": [],
    },
    "tuyaMcuCluster": {
        "fz": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
        "tz": [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
        "e": [],
    },
    # Tuya DP-based entities (dpBinary, dpNumeric, dpEnumLookup, dpAction, etc.)
    "dpBinary": {
        "fz": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
        "tz": [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
        "e": [{"t": 3, "f": 0, "p": "dp_binary", "ac": 1}],
    },
    "dpNumeric": {
        "fz": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
        "tz": [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
        "e": [{"t": 2, "f": 0, "p": "dp_numeric", "ac": 1}],
    },
    "dpEnumLookup": {
        "fz": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
        "tz": [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
        "e": [{"t": 8, "f": 0, "p": "dp_enum", "ac": 3}],
    },
    "dpAction": {
        "fz": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
        "tz": [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
        "e": [{"t": 8, "f": 0, "p": "action", "ac": 1}],
    },
    "dpTemperature": {
        "fz": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
        "tz": [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
        "e": [{"t": 2, "f": 0, "dc": "temperature", "u": "°C", "p": "temperature", "ac": 1}],
    },
    "dpLight": {
        "fz": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
        "tz": [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
        "e": [{"t": 0, "f": 3, "p": "state", "ac": 3}],
        "is_light": True,
    },
    "tuyaWeatherForecast": {
        "fz": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "p": "weather", "ac": 1}],
    },
    "electricityMeasurementPoll": {
        "fz": [
            {"c": 2820, "a": 1291, "k": "power", "fn": "fz_electrical_power"},
            {"c": 2820, "a": 1288, "k": "current", "fn": "fz_current"},
            {"c": 2820, "a": 1285, "k": "voltage", "fn": "fz_voltage"},
        ],
        "tz": [],
        "e": [
            {"t": 2, "f": 0, "dc": "power", "u": "W", "sc": "measurement", "p": "power", "ac": 1},
            {"t": 2, "f": 0, "dc": "current", "u": "A", "sc": "measurement", "p": "current", "ac": 1},
            {"t": 2, "f": 0, "dc": "voltage", "u": "V", "sc": "measurement", "p": "voltage", "ac": 1},
        ],
    },
    "combineActions": {
        "fz": [],
        "tz": [],
        "e": [{"t": 8, "f": 0, "p": "action", "ac": 1}],
    },
    # ---------------------------------------------------------------
    # Vendor-specific light wrappers (all are light variants)
    # These all wrap m.light() with vendor-specific customizations
    # ---------------------------------------------------------------
    "ikeaLight": {
        "fz": [
            {"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"},
            {"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"},
        ],
        "tz": [
            {"k": "state", "c": 6, "fn": "tz_on_off"},
            {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        ],
        "e": [{"t": 0, "f": 3, "p": "state", "ac": 3}],
        "is_light": True,
    },
    "ledvanceLight": {
        "fz": [
            {"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"},
            {"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"},
        ],
        "tz": [
            {"k": "state", "c": 6, "fn": "tz_on_off"},
            {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        ],
        "e": [{"t": 0, "f": 3, "p": "state", "ac": 3}],
        "is_light": True,
    },
    "gledoptoLight": {
        "fz": [
            {"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"},
            {"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"},
        ],
        "tz": [
            {"k": "state", "c": 6, "fn": "tz_on_off"},
            {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        ],
        "e": [{"t": 0, "f": 3, "p": "state", "ac": 3}],
        "is_light": True,
    },
    "sengledLight": {
        "fz": [
            {"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"},
            {"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"},
        ],
        "tz": [
            {"k": "state", "c": 6, "fn": "tz_on_off"},
            {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        ],
        "e": [{"t": 0, "f": 3, "p": "state", "ac": 3}],
        "is_light": True,
    },
    "mullerLichtLight": {
        "fz": [
            {"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"},
            {"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"},
        ],
        "tz": [
            {"k": "state", "c": 6, "fn": "tz_on_off"},
            {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        ],
        "e": [{"t": 0, "f": 3, "p": "state", "ac": 3}],
        "is_light": True,
    },
    "lumiLight": {
        "fz": [
            {"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"},
            {"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"},
        ],
        "tz": [
            {"k": "state", "c": 6, "fn": "tz_on_off"},
            {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        ],
        "e": [{"t": 0, "f": 3, "p": "state", "ac": 3}],
        "is_light": True,
    },
    # ---------------------------------------------------------------
    # Vendor-specific on/off wrappers
    # ---------------------------------------------------------------
    "lumiOnOff": {
        "fz": [{"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"}],
        "tz": [{"k": "state", "c": 6, "fn": "tz_on_off"}],
        "e": [{"t": 1, "f": 0, "p": "state", "ac": 3}],
    },
    "lumiPower": {
        "fz": [
            {"c": 2820, "a": 1291, "k": "power", "fn": "fz_electrical_power"},
            {"c": 1794, "a": 0, "k": "energy", "fn": "fz_metering"},
        ],
        "tz": [],
        "e": [
            {"t": 2, "f": 0, "dc": "power", "u": "W", "sc": "measurement", "p": "power", "ac": 1},
            {"t": 2, "f": 0, "dc": "energy", "u": "kWh", "sc": "total_increasing", "p": "energy", "ac": 1},
        ],
    },
    "lumiElectricityMeter": {
        "fz": [
            {"c": 2820, "a": 1291, "k": "power", "fn": "fz_electrical_power"},
            {"c": 2820, "a": 1288, "k": "current", "fn": "fz_current"},
            {"c": 2820, "a": 1285, "k": "voltage", "fn": "fz_voltage"},
            {"c": 1794, "a": 0, "k": "energy", "fn": "fz_metering"},
        ],
        "tz": [],
        "e": [
            {"t": 2, "f": 0, "dc": "power", "u": "W", "sc": "measurement", "p": "power", "ac": 1},
            {"t": 2, "f": 0, "dc": "current", "u": "A", "sc": "measurement", "p": "current", "ac": 1},
            {"t": 2, "f": 0, "dc": "voltage", "u": "V", "sc": "measurement", "p": "voltage", "ac": 1},
            {"t": 2, "f": 0, "dc": "energy", "u": "kWh", "sc": "total_increasing", "p": "energy", "ac": 1},
        ],
    },
    # ---------------------------------------------------------------
    # Vendor-specific action/button patterns
    # ---------------------------------------------------------------
    "lumiAction": {
        "fz": [{"c": 18, "a": 85, "k": "action", "fn": "fz_multistate_input"}],
        "tz": [],
        "e": [{"t": 8, "f": 0, "p": "action", "ac": 1}],
    },
    "ikeaBattery": {
        "fz": [{"c": 1, "a": 33, "k": "battery", "fn": "fz_battery"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "battery", "u": "%", "sc": "measurement", "p": "battery", "ac": 1}],
    },
    "ikeaVoc": {
        "fz": [{"c": 1070, "a": 0, "k": "voc_index", "fn": "fz_ikea_voc_index"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "u": "ppb", "sc": "measurement", "p": "voc_index", "ac": 1}],
    },
    "ikeaAirPurifier": {
        "fz": [{"c": 514, "a": 0, "k": "fan_mode", "fn": "fz_ikea_air_purifier"}],
        "tz": [{"k": "fan_mode", "c": 514, "fn": "tz_fan_mode"}],
        "e": [{"t": 7, "f": 0, "p": "fan_mode", "ac": 3}],
    },
    "tradfriOccupancy": {
        "fz": [{"c": 1030, "a": 0, "k": "occupancy", "fn": "fz_occupancy"}],
        "tz": [],
        "e": [{"t": 3, "f": 0, "dc": "motion", "p": "occupancy", "ac": 1}],
    },
    "lockExtend": {
        "fz": [{"c": 257, "a": 0, "k": "state", "fn": "fz_lock_state"}],
        "tz": [{"k": "state", "c": 257, "fn": "tz_lock_command"}],
        "e": [{"t": 5, "f": 0, "p": "state", "ac": 3}],
    },
    # ---------------------------------------------------------------
    # Philips-specific patterns
    # ---------------------------------------------------------------
    "contact": {
        "fz": [{"c": 1280, "a": 2, "k": "contact", "fn": "fz_ias_zone_status"}],
        "tz": [],
        "e": [{"t": 3, "f": 0, "dc": "door", "p": "contact", "ac": 1}],
    },
    # ---------------------------------------------------------------
    # No-op / config-only extends — they exist in definitions but don't
    # produce fz/tz converters (they configure endpoints, clusters, etc.)
    # We map them to empty results so they don't block FULL_MATCH
    # ---------------------------------------------------------------
}

# Extends that are "no-op" (config/infrastructure only, no fz/tz/e needed)
# These are recognized as valid so they don't block FULL_MATCH status.
NOOP_EXTENDS = {
    "linkquality", "identify", "deviceEndpoints", "deviceAddCustomCluster",
    "forcePowerSource", "quirkCheckinInterval", "quirkAddEndpointCluster",
    "forceDeviceType", "bindCluster", "poll",
    # Command routing (no direct fz/tz, handled by Zigbee stack)
    "commandsOnOff", "commandsLevelCtrl", "commandsColorCtrl",
    "commandsWindowCovering", "commandsScenes",
    # Vendor-specific config-only extends
    "lumiZigbeeOTA", "lumiPreventReset", "lumiLedDisabledNight",
    "lumiFlipIndicatorLight",
    "addCustomClusterManuSpecificIkeaUnknown",
    "addCustomClusterManuSpecificIkeaAirPurifier",
    "addCustomClusterManuSpecificIkeaSmartPlug",
    "addCustomClusterManuSpecificIkeaVocIndexMeasurement",
    "ikeaConfigureGenPollCtrl", "ikeaConfigureRemote", "ikeaConfigureStyrbar",
    "ikeaModernExtend",
    "addCustomClusterManuSpecificPhilipsContact",
    "addCustomClusterManuSpecificDevelcoGenBasic",
    "readGenBasicPrimaryVersions",
    "addCustomClusterEwelink",
    "addVisaConfigurationCluster",
    "clusterManuSpecificOrviboPowerOnBehavior",
    "orviboSwitchPowerOnBehavior",
    "externalSwitchType", "indicatorMode",
    # Vendor-specific OTA
    "sonoffExtend",
    # Tuya config-only
    "tuyaWeatherForecast",
    # Generic config
    "ikeaDotsClick", "ikeaArrowClick", "ikeaMediaCommands",
    "ikeaBilresaDouble", "styrbarCommandOn",
    "tradfriCommandsLevelCtrl", "tradfriCommandsOnOff",
    "tradfriRequestedBrightness",
}

# Additional options that add converters to m.light()
LIGHT_OPTIONS = {
    "colorTemp": {
        "fz": [{"c": 768, "a": 7, "k": "color_temp", "fn": "fz_color_temp"}],
        "tz": [{"k": "color_temp", "c": 768, "fn": "tz_color_temp"}],
        "feature_add": 2,  # ZB_FEATURE_COLOR_TEMP
    },
    "color": {
        "fz": [
            {"c": 768, "a": 0, "k": "color_hue", "fn": "fz_color_hs"},
            {"c": 768, "a": 1, "k": "color_saturation", "fn": "fz_color_hs"},
            {"c": 768, "a": 3, "k": "color_x", "fn": "fz_color_xy"},
            {"c": 768, "a": 4, "k": "color_y", "fn": "fz_color_xy"},
        ],
        "tz": [
            {"k": "color", "c": 768, "fn": "tz_color_hs"},
            {"k": "color_xy", "c": 768, "fn": "tz_color_xy"},
        ],
        "feature_add": 12,  # ZB_FEATURE_COLOR_XY | ZB_FEATURE_COLOR_HS
    },
}

# Legacy fz.* → C function mapping
LEGACY_FZ_MAP = {
    "fz.on_off": [{"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"}],
    "fz.brightness": [{"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"}],
    "fz.color_colortemp": [
        {"c": 768, "a": 7, "k": "color_temp", "fn": "fz_color_temp"},
        {"c": 768, "a": 0, "k": "color_hue", "fn": "fz_color_hs"},
        {"c": 768, "a": 1, "k": "color_saturation", "fn": "fz_color_hs"},
    ],
    "fz.temperature": [{"c": 1026, "a": 0, "k": "temperature", "fn": "fz_temperature"}],
    "fz.humidity": [{"c": 1029, "a": 0, "k": "humidity", "fn": "fz_humidity"}],
    "fz.pressure": [{"c": 1027, "a": 0, "k": "pressure", "fn": "fz_pressure"}],
    "fz.illuminance": [{"c": 1024, "a": 0, "k": "illuminance", "fn": "fz_illuminance"}],
    "fz.occupancy": [{"c": 1030, "a": 0, "k": "occupancy", "fn": "fz_occupancy"}],
    "fz.battery": [{"c": 1, "a": 33, "k": "battery", "fn": "fz_battery"}],
    "fz.ias_zone_alarm_1": [{"c": 1280, "a": 2, "k": "alarm", "fn": "fz_ias_zone_status"}],
    "fz.ias_zone_alarm_2": [{"c": 1280, "a": 2, "k": "alarm", "fn": "fz_ias_zone_status"}],
    "fz.electrical_measurement": [{"c": 2820, "a": 1291, "k": "power", "fn": "fz_electrical_power"}],
    "fz.metering": [{"c": 1794, "a": 0, "k": "energy", "fn": "fz_metering"}],
    "fz.thermostat": [{"c": 513, "a": 0, "k": "local_temperature", "fn": "fz_thermostat_local_temp"}],
    "fz.cover_position_tilt": [{"c": 258, "a": 8, "k": "position", "fn": "fz_cover_position"}],
    "fz.lock": [{"c": 257, "a": 0, "k": "state", "fn": "fz_lock_state"}],
    "fz.power_on_behavior": [{"c": 6, "a": 16387, "k": "power_on_behavior", "fn": "fz_power_on_behavior"}],
    "fz.thermostat_local_temperature": [{"c": 513, "a": 0, "k": "local_temperature", "fn": "fz_thermostat_local_temp"}],
    "fz.thermostat_system_mode": [{"c": 513, "a": 28, "k": "system_mode", "fn": "fz_thermostat_system_mode"}],
    "fz.thermostat_running_state": [{"c": 513, "a": 41, "k": "running_state", "fn": "fz_thermostat_running_state"}],
    "fz.fan_mode": [{"c": 514, "a": 0, "k": "fan_mode", "fn": "fz_fan_mode"}],
    "fz.cover_position_via_brightness": [{"c": 258, "a": 8, "k": "position", "fn": "fz_cover_position"}],
    "fz.smoke_alarm": [{"c": 1280, "a": 2, "k": "smoke", "fn": "fz_smoke_alarm"}],
    "fz.water_leak": [{"c": 1280, "a": 2, "k": "water_leak", "fn": "fz_water_leak"}],
    "fz.ias_water_leak_alarm_1": [{"c": 1280, "a": 2, "k": "water_leak", "fn": "fz_water_leak"}],
    "fz.ias_smoke_alarm_1": [{"c": 1280, "a": 2, "k": "smoke", "fn": "fz_smoke_alarm"}],
    "fz.tuya_data_point_convert": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
    "fz.lighting_color_state_colortemp": [
        {"c": 768, "a": 7, "k": "color_temp", "fn": "fz_color_temp"},
        {"c": 768, "a": 0, "k": "color_hue", "fn": "fz_color_hs"},
        {"c": 768, "a": 1, "k": "color_saturation", "fn": "fz_color_hs"},
    ],
    "fz.ias_contact_alarm_1": [{"c": 1280, "a": 2, "k": "contact", "fn": "fz_ias_zone_status"}],
    "fz.ias_contact_alarm_2": [{"c": 1280, "a": 2, "k": "contact", "fn": "fz_ias_zone_status"}],
    "fz.ias_gas_alarm_1": [{"c": 1280, "a": 2, "k": "gas", "fn": "fz_ias_zone_status"}],
    "fz.ias_gas_alarm_2": [{"c": 1280, "a": 2, "k": "gas", "fn": "fz_ias_zone_status"}],
    "fz.ias_carbon_monoxide_alarm_1": [{"c": 1280, "a": 2, "k": "carbon_monoxide", "fn": "fz_ias_zone_status"}],
    "fz.ias_carbon_monoxide_alarm_2": [{"c": 1280, "a": 2, "k": "carbon_monoxide", "fn": "fz_ias_zone_status"}],
    "fz.ias_vibration_alarm_1": [{"c": 1280, "a": 2, "k": "vibration", "fn": "fz_ias_zone_status"}],
    "fz.tamper": [{"c": 1280, "a": 2, "k": "tamper", "fn": "fz_ias_zone_status"}],
    "fz.ignore_basic_report": [],
    "fz.command_on": [{"c": 6, "a": 0, "k": "action", "fn": "fz_on_off"}],
    "fz.command_off": [{"c": 6, "a": 0, "k": "action", "fn": "fz_on_off"}],
    "fz.command_toggle": [{"c": 6, "a": 0, "k": "action", "fn": "fz_on_off"}],
    "fz.command_move": [{"c": 8, "a": 0, "k": "action", "fn": "fz_brightness"}],
    "fz.command_step": [{"c": 8, "a": 0, "k": "action", "fn": "fz_brightness"}],
    "fz.command_stop": [{"c": 8, "a": 0, "k": "action", "fn": "fz_brightness"}],
    "fz.command_move_to_level": [{"c": 8, "a": 0, "k": "action", "fn": "fz_brightness"}],
    "fz.command_move_to_color_temp": [{"c": 768, "a": 7, "k": "action", "fn": "fz_color_temp"}],
    "fz.command_move_to_color": [{"c": 768, "a": 0, "k": "action", "fn": "fz_color_hs"}],
    "fz.command_move_hue": [{"c": 768, "a": 0, "k": "action", "fn": "fz_color_hs"}],
    "fz.command_step_color_temperature": [{"c": 768, "a": 7, "k": "action", "fn": "fz_color_temp"}],
    "fz.tuya_switch": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
    "fz.tuya_thermostat": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
    "fz.ignore_tuya_set_time": [],
    "fz.tuya_dimmer": [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
    "fz.tuya_relay_din_led_indicator": [],
    "fz.power_outage_memory": [{"c": 6, "a": 16387, "k": "power_on_behavior", "fn": "fz_power_on_behavior"}],
    "fz.lighting_ballast_configuration": [],
    "fz.level_config": [],
    "fz.color_colortemp": [
        {"c": 768, "a": 7, "k": "color_temp", "fn": "fz_color_temp"},
        {"c": 768, "a": 0, "k": "color_hue", "fn": "fz_color_hs"},
    ],
    "fz.identify": [],
    "fz.linkquality_from_basic": [],
}

# Legacy tz.* → C function mapping
LEGACY_TZ_MAP = {
    "tz.on_off": [{"k": "state", "c": 6, "fn": "tz_on_off"}],
    "tz.light_onoff_brightness": [
        {"k": "state", "c": 6, "fn": "tz_on_off"},
        {"k": "brightness", "c": 8, "fn": "tz_brightness"},
    ],
    "tz.light_onoff_brightness_colortemp": [
        {"k": "state", "c": 6, "fn": "tz_on_off"},
        {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        {"k": "color_temp", "c": 768, "fn": "tz_color_temp"},
    ],
    "tz.light_onoff_brightness_colortemp_color": [
        {"k": "state", "c": 6, "fn": "tz_on_off"},
        {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        {"k": "color_temp", "c": 768, "fn": "tz_color_temp"},
        {"k": "color", "c": 768, "fn": "tz_color_hs"},
    ],
    "tz.light_onoff_brightness_color": [
        {"k": "state", "c": 6, "fn": "tz_on_off"},
        {"k": "brightness", "c": 8, "fn": "tz_brightness"},
        {"k": "color", "c": 768, "fn": "tz_color_hs"},
    ],
    "tz.thermostat_occupied_heating_setpoint": [{"k": "occupied_heating_setpoint", "c": 513, "fn": "tz_thermostat_setpoint"}],
    "tz.thermostat_system_mode": [{"k": "system_mode", "c": 513, "fn": "tz_thermostat_system_mode"}],
    "tz.cover_position_tilt": [{"k": "position", "c": 258, "fn": "tz_cover_position"}],
    "tz.lock": [{"k": "state", "c": 257, "fn": "tz_lock_command"}],
    "tz.power_on_behavior": [{"k": "power_on_behavior", "c": 6, "fn": "tz_power_on_behavior"}],
    "tz.fan_mode": [{"k": "fan_mode", "c": 514, "fn": "tz_fan_mode"}],
    "tz.cover_state": [{"k": "state", "c": 258, "fn": "tz_cover_command"}],
    "tz.tuya_data_point_test": [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
    "tz.warning": [{"k": "warning", "c": 1282, "fn": "tz_warning"}],
    "tz.identify": [{"k": "identify", "c": 3, "fn": "tz_identify"}],
    "tz.light_colortemp": [{"k": "color_temp", "c": 768, "fn": "tz_color_temp"}],
    "tz.light_color": [{"k": "color", "c": 768, "fn": "tz_color_hs"}],
    "tz.thermostat_local_temperature": [{"k": "local_temperature", "c": 513, "fn": "tz_thermostat_setpoint"}],
    "tz.thermostat_running_state": [{"k": "running_state", "c": 513, "fn": "tz_thermostat_system_mode"}],
    "tz.thermostat_local_temperature_calibration": [{"k": "local_temperature_calibration", "c": 513, "fn": "tz_thermostat_setpoint"}],
    "tz.tuya_switch_state": [{"k": "state", "c": 61184, "fn": "tz_tuya_dp"}],
    "tz.tuya_thermostat": [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
    "tz.tuya_dimmer_state": [{"k": "state", "c": 61184, "fn": "tz_tuya_dp"}],
    "tz.tuya_dimmer_level": [{"k": "brightness", "c": 61184, "fn": "tz_tuya_dp"}],
    "tz.power_outage_memory": [{"k": "power_on_behavior", "c": 6, "fn": "tz_power_on_behavior"}],
    "tz.light_brightness_move": [{"k": "brightness", "c": 8, "fn": "tz_brightness"}],
    "tz.effect": [{"k": "effect", "c": 6, "fn": "tz_effect"}],
    "tz.cover_via_brightness": [{"k": "position", "c": 258, "fn": "tz_cover_position"}],
}

# ============================================================================
# ESP32 Function Registries — used for compatibility filtering
# These must match the functions in zb_converter_fn_registry.c
# ============================================================================

ESP32_FZ_REGISTRY = {
    "fz_analog_input", "fz_aqara_opple", "fz_battery", "fz_brightness",
    "fz_co2", "fz_color_enhance_hs", "fz_color_hs", "fz_color_temp",
    "fz_color_xy", "fz_cover_position", "fz_cover_tilt", "fz_current",
    "fz_device_temperature", "fz_diagnostics_lqi", "fz_diagnostics_rssi",
    "fz_door_lock_event", "fz_door_lock_pin_code", "fz_door_lock_user_status",
    "fz_electrical_frequency", "fz_electrical_power", "fz_fan_mode",
    "fz_generic_attr", "fz_humidity", "fz_ias_wd", "fz_ias_zone_enrollment",
    "fz_ias_zone_status", "fz_ikea_air_purifier", "fz_ikea_voc_index",
    "fz_illuminance", "fz_lock_state", "fz_metering", "fz_multistate_input",
    "fz_occupancy", "fz_on_off", "fz_philips_hue_motion", "fz_pm25",
    "fz_power_factor", "fz_power_on_behavior", "fz_pressure",
    "fz_smoke_alarm", "fz_soil_moisture", "fz_temperature",
    "fz_thermostat_local_temp", "fz_thermostat_running_state",
    "fz_thermostat_setpoint", "fz_thermostat_system_mode",
    "fz_thermostat_weekly_schedule", "fz_tuya_dp", "fz_uint16",
    "fz_vibration_action", "fz_vibration_angle", "fz_vibration_strength",
    "fz_voc", "fz_voltage", "fz_water_leak", "fz_xiaomi_ff01",
    "fz_xiaomi_voltage",
}

ESP32_TZ_REGISTRY = {
    "tz_aqara_opple", "tz_brightness", "tz_color_hs", "tz_color_temp",
    "tz_color_xy", "tz_cover_command", "tz_cover_position", "tz_cover_tilt",
    "tz_door_lock_pin", "tz_effect", "tz_fan_mode", "tz_generic_write_attr",
    "tz_ias_zone_enroll_response", "tz_identify", "tz_identify_effect",
    "tz_lock_command", "tz_on_off", "tz_power_on_behavior",
    "tz_thermostat_setpoint", "tz_thermostat_system_mode",
    "tz_thermostat_weekly_schedule", "tz_tuya_dp", "tz_warning",
    "tz_xiaomi_sensitivity",
}


def check_device_compat(dev):
    """Check if all fz/tz functions in the device are in the ESP32 registries.

    Generic fallback functions (fz_generic_attr, tz_generic_write_attr) are
    always considered compatible.
    """
    always_compat = {"fz_generic_attr", "tz_generic_write_attr"}
    for entry in dev.get("fz", []):
        fn = entry.get("fn", "")
        if fn and fn not in ESP32_FZ_REGISTRY and fn not in always_compat:
            return False
    for entry in dev.get("tz", []):
        fn = entry.get("fn", "")
        if fn and fn not in ESP32_TZ_REGISTRY and fn not in always_compat:
            return False
    return True


def _extract_func_name(call_str):
    """Extract the base function name from an extend call string.

    Handles patterns like:
      m.light(...)           -> 'light'
      philips.m.light(...)   -> 'light'
      tuya.modernExtend.tuyaBase(...)  -> 'tuyaBase'
      ikeaLight(...)         -> 'ikeaLight'
      ledvanceLight(...)     -> 'ledvanceLight'
      addCustomClusterManuSpecificIkeaUnknown() -> 'addCustomClusterManuSpecificIkeaUnknown'

    Returns (func_name, args_str) or (None, None) if no match.
    """
    s = call_str.strip()

    # Match: optional.prefix.chain.funcName( ... )
    # The function name is everything after the last dot before the opening paren
    # OR the entire name if no dots before the paren
    match = re.match(r'(?:[\w.]*\.)?(\w+)\s*\((.*)\)\s*$', s, re.DOTALL)
    if not match:
        return None, None
    return match.group(1), match.group(2).strip()


# ============================================================================
# Tuya DP extraction from extend call arguments
# ============================================================================

# Maps dpXxx function names to default DP type strings
TUYA_DP_FUNC_TYPE = {
    "dpBinary": "bool",
    "dpNumeric": "int",
    "dpEnumLookup": "enum",
    "dpAction": "enum",
    "dpString": "str",
    "dpTemperature": "int",
    "dpLight": None,  # composite, handled specially
}


def _extract_tuya_dp_from_args(func_name, args_str):
    """Extract Tuya DP info from a dpXxx() extend call's arguments.

    Parses TypeScript object literal patterns like:
        {name: 'state', dp: 1, type: tuya.dataTypes.bool}
        {name: 'temperature', dp: 16, scale: 0.1, unit: '°C'}
        {name: 'mode', dp: 2, lookup: {auto: 0, heat: 1, cool: 2}}

    Returns a dict of {dp_id_str: {k, t, ...}} or None.
    """
    if not args_str:
        return None

    # Handle dpLight specially: it has sub-objects for state, brightness, colorTemp
    if func_name == "dpLight":
        return _extract_tuya_dp_light(args_str)

    # Extract dp: N
    dp_match = re.search(r'\bdp:\s*(\d+)', args_str)
    if not dp_match:
        return None
    dp_id = dp_match.group(1)

    # Extract name: 'xxx' or name: "xxx"
    name_match = re.search(r"""\bname:\s*['"]([^'"]+)['"]""", args_str)
    if not name_match:
        # Some dpAction calls don't have name, use "action" as default
        if func_name == "dpAction":
            name = "action"
        else:
            return None
    else:
        name = name_match.group(1)

    entry = {"k": name}

    # Determine type
    default_type = TUYA_DP_FUNC_TYPE.get(func_name, "int")
    # Check for explicit type: tuya.dataTypes.xxx or dataTypes.xxx
    type_match = re.search(r'(?:tuya\.)?dataTypes\.(\w+)', args_str)
    if type_match:
        ts_type = type_match.group(1).lower()
        type_map = {
            "bool": "bool", "boolean": "bool",
            "number": "int", "value": "int",
            "string": "str",
            "enum": "enum",
            "bitmap": "bitmap",
            "raw": "raw",
        }
        entry["t"] = type_map.get(ts_type, default_type)
    elif default_type:
        entry["t"] = default_type

    # Extract scale (number or array [fromMin, fromMax, toMin, toMax])
    scale_match = re.search(r'\bscale:\s*(\d+(?:\.\d+)?)\b', args_str)
    if scale_match:
        scale_val = float(scale_match.group(1))
        if scale_val != 0 and scale_val != 1:
            # z2m uses scale as divisor, we store as multiplier (1/scale)
            entry["scale"] = 1.0 / scale_val if scale_val > 0 else 0
    # Also check for array scale: scale: [0, 254, 0, 1000]
    scale_arr_match = re.search(r'\bscale:\s*\[([^\]]+)\]', args_str)
    if scale_arr_match and "scale" not in entry:
        parts = scale_arr_match.group(1).split(",")
        if len(parts) == 4:
            try:
                from_min, from_max, to_min, to_max = [float(p.strip()) for p in parts]
                if from_max != 0:
                    # scale as ratio: toRange/fromRange
                    entry["scale"] = (to_max - to_min) / (from_max - from_min)
            except ValueError:
                pass

    # Extract valueMin/valueMax or min/max
    for min_key in ("valueMin", "min"):
        m = re.search(rf'\b{min_key}:\s*(-?\d+(?:\.\d+)?)', args_str)
        if m:
            entry["min"] = float(m.group(1))
            break
    for max_key in ("valueMax", "max"):
        m = re.search(rf'\b{max_key}:\s*(-?\d+(?:\.\d+)?)', args_str)
        if m:
            entry["max"] = float(m.group(1))
            break

    # Extract lookup: {auto: 0, heat: 1, ...} for enum types
    lookup_match = re.search(r'\blookup:\s*\{', args_str)
    if lookup_match:
        start = lookup_match.end() - 1
        end = find_matching_brace(args_str, start)
        if end > start:
            lookup_str = args_str[start + 1:end]
            enum_map = _parse_js_lookup(lookup_str)
            if enum_map:
                entry["v"] = enum_map
                entry["t"] = "enum"

    # Clean up: remove default/useless values
    if "scale" in entry:
        s = entry["scale"]
        if isinstance(s, float) and (s == 1.0 or s == 0.0):
            del entry["scale"]
        elif isinstance(s, float):
            # Round for cleaner output
            entry["scale"] = round(s, 6)
    if "min" in entry and "max" in entry:
        if entry["min"] == 0 and entry["max"] == 0:
            del entry["min"]
            del entry["max"]

    return {dp_id: entry}


def _extract_tuya_dp_light(args_str):
    """Extract DP mappings from dpLight({state: {dp: 7}, brightness: {dp: 8, scale: ...}}).

    Returns dict with DP entries for state, brightness, colorTemp sub-objects.
    """
    result = {}

    # Parse state sub-object
    state_match = re.search(r'\bstate:\s*\{', args_str)
    if state_match:
        start = state_match.end() - 1
        end = find_matching_brace(args_str, start)
        if end > start:
            sub = args_str[start + 1:end]
            dp_m = re.search(r'\bdp:\s*(\d+)', sub)
            if dp_m:
                result[dp_m.group(1)] = {"k": "state", "t": "bool"}

    # Parse brightness sub-object
    bright_match = re.search(r'\bbrightness:\s*\{', args_str)
    if bright_match:
        start = bright_match.end() - 1
        end = find_matching_brace(args_str, start)
        if end > start:
            sub = args_str[start + 1:end]
            dp_m = re.search(r'\bdp:\s*(\d+)', sub)
            if dp_m:
                entry = {"k": "brightness", "t": "int"}
                # Check for scale array
                scale_arr = re.search(r'\bscale:\s*\[([^\]]+)\]', sub)
                if scale_arr:
                    parts = scale_arr.group(1).split(",")
                    if len(parts) == 4:
                        try:
                            from_min, from_max, to_min, to_max = [float(p.strip()) for p in parts]
                            if from_max != 0:
                                entry["scale"] = round((to_max - to_min) / (from_max - from_min), 6)
                        except ValueError:
                            pass
                result[dp_m.group(1)] = entry

    # Parse colorTemp sub-object
    ct_match = re.search(r'\bcolorTemp(?:erature)?:\s*\{', args_str)
    if ct_match:
        start = ct_match.end() - 1
        end = find_matching_brace(args_str, start)
        if end > start:
            sub = args_str[start + 1:end]
            dp_m = re.search(r'\bdp:\s*(\d+)', sub)
            if dp_m:
                result[dp_m.group(1)] = {"k": "color_temp", "t": "int"}

    return result if result else None


def _parse_js_lookup(lookup_str):
    """Parse JavaScript object literal body into {name: value} dict.

    Handles:
        auto: 0, heat: 1, cool: 2
        'auto': 0, 'heat': 1
        "auto": 0, "heat": 1
        scene_1: tuya.enum(0), scene_2: tuya.enum(1)
    """
    result = {}
    # Match: key: value pairs where key can be quoted or unquoted
    pattern = re.compile(
        r"""(?:['"]?(\w+)['"]?)\s*:\s*"""  # key (optionally quoted)
        r"""(?:tuya\.enum\()?"""            # optional tuya.enum( wrapper
        r"""(-?\d+)"""                       # numeric value
        r"""\)?"""                           # optional closing paren
    )
    for m in pattern.finditer(lookup_str):
        name = m.group(1)
        val = int(m.group(2))
        if 0 <= val <= 255:
            result[name] = val

    return result if result else None


def parse_extend_call(call_str):
    """Parse a single ModernExtend call like 'm.light({colorTemp: {range: [250, 454]}})'.

    Handles any prefix (m., philips.m., tuya.modernExtend., bare function names).
    """
    func_name, args_str = _extract_func_name(call_str)
    if func_name is None:
        return None, "NO_MATCH", None

    # Check no-op extends first
    if func_name in NOOP_EXTENDS:
        return {"fz": [], "tz": [], "e": []}, "FULL_MATCH", None

    if func_name not in MODERN_EXTEND_MAP:
        return None, "UNKNOWN_EXTEND", None

    mapping = MODERN_EXTEND_MAP[func_name]
    import copy
    result = {
        "fz": copy.deepcopy(mapping["fz"]),
        "tz": copy.deepcopy(mapping["tz"]),
        "e": copy.deepcopy(mapping["e"]),
    }

    # Extract Tuya DP data from dpXxx functions
    tuya_dp = None
    if func_name in TUYA_DP_FUNC_TYPE:
        tuya_dp = _extract_tuya_dp_from_args(func_name, args_str)

    # Handle light options (colorTemp, color) for any light-like extend
    is_light = mapping.get("is_light", False)
    if is_light and args_str:
        if "colorTemp" in args_str or "color_temp" in args_str:
            opt = LIGHT_OPTIONS["colorTemp"]
            result["fz"].extend(opt["fz"])
            result["tz"].extend(opt["tz"])
            if result["e"]:
                result["e"][0]["f"] = result["e"][0].get("f", 0) | opt["feature_add"]
        if (re.search(r'\bcolor\b\s*:', args_str) or
                "colorXY" in args_str or
                "color: true" in args_str or
                re.search(r'\bcolor\b\s*[,})\]]', args_str)):
            opt = LIGHT_OPTIONS["color"]
            result["fz"].extend(opt["fz"])
            result["tz"].extend(opt["tz"])
            if result["e"]:
                result["e"][0]["f"] = result["e"][0].get("f", 0) | opt["feature_add"]

    return result, "FULL_MATCH", tuya_dp


def _skip_string(content, i, quote, length):
    """Skip past a string literal starting at position i (after the opening quote)."""
    while i < length:
        c = content[i]
        if c == '\\':
            i += 2
            continue
        if c == quote:
            return i  # position of closing quote
        i += 1
    return i


def _skip_template_literal(content, i, length):
    """Skip past a template literal starting at position i (after the opening backtick)."""
    while i < length:
        c = content[i]
        if c == '\\':
            i += 2
            continue
        if c == '`':
            return i  # position of closing backtick
        if c == '$' and i + 1 < length and content[i + 1] == '{':
            # Template expression: ${...} — find matching }
            i += 2  # skip ${
            tmpl_depth = 1
            while i < length and tmpl_depth > 0:
                tc = content[i]
                if tc == '\\':
                    i += 2
                    continue
                if tc == '{':
                    tmpl_depth += 1
                elif tc == '}':
                    tmpl_depth -= 1
                    if tmpl_depth == 0:
                        break
                elif tc == '"' or tc == "'":
                    i = _skip_string(content, i + 1, tc, length)
                elif tc == '`':
                    i = _skip_template_literal(content, i + 1, length)
                i += 1
            continue
        i += 1
    return i


def find_matching_brace(content, start):
    """Find the closing brace matching the opening brace at 'start'.

    Properly handles:
    - Nested braces
    - Single-quoted strings, double-quoted strings
    - Template literal strings (backtick) with ${} interpolation
    - Line comments (//) and block comments (/* */)
    - Escaped characters
    """
    depth = 0
    i = start
    length = len(content)
    while i < length:
        ch = content[i]
        # Line comment: skip to end of line
        if ch == '/' and i + 1 < length:
            if content[i + 1] == '/':
                i += 2
                while i < length and content[i] != '\n':
                    i += 1
                i += 1  # skip the newline
                continue
            if content[i + 1] == '*':
                i += 2
                while i < length - 1 and not (content[i] == '*' and content[i + 1] == '/'):
                    i += 1
                i += 2  # skip */
                continue
        if ch == '\\':
            i += 2
            continue
        if ch == '"' or ch == "'":
            i = _skip_string(content, i + 1, ch, length) + 1
            continue
        if ch == '`':
            i = _skip_template_literal(content, i + 1, length) + 1
            continue
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def extract_bracket_content(text, keyword):
    """Extract content of keyword: [...] handling nested brackets, strings, and comments."""
    match = re.search(rf'{keyword}:\s*\[', text)
    if not match:
        return None
    start = match.end() - 1
    depth = 0
    i = start
    length = len(text)
    while i < length:
        ch = text[i]
        # Line and block comments
        if ch == '/' and i + 1 < length:
            if text[i + 1] == '/':
                i += 2
                while i < length and text[i] != '\n':
                    i += 1
                i += 1
                continue
            if text[i + 1] == '*':
                i += 2
                while i < length - 1 and not (text[i] == '*' and text[i + 1] == '/'):
                    i += 1
                i += 2
                continue
        if ch == '\\':
            i += 2
            continue
        if ch == '"' or ch == "'":
            i = _skip_string(text, i + 1, ch, length) + 1
            continue
        if ch == '`':
            i = _skip_template_literal(text, i + 1, length) + 1
            continue
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
        i += 1
    return None


def _find_definitions_array(content):
    """Find the start of the definitions array in the file.

    Returns the index after the opening '[' of:
      export const definitions: ... = [
    or None if not found.
    """
    match = re.search(r'export\s+const\s+definitions\s*(?::\s*[\w\[\]<>,\s|]+)?\s*=\s*\[', content)
    if match:
        return match.end()
    return None


def _split_definitions_array(content, array_start):
    """Split the top-level definitions array into individual definition blocks.

    Starting from array_start (right after the '['), finds each top-level
    object literal {...} in the array and returns a list of (block_text, start_pos) tuples.
    """
    definitions = []
    i = array_start
    length = len(content)

    while i < length:
        # Skip whitespace and commas
        while i < length and content[i] in ' \t\n\r,':
            i += 1

        if i >= length:
            break

        # Check for end of array
        if content[i] == ']':
            break

        # Skip single-line comments
        if content[i] == '/' and i + 1 < length and content[i + 1] == '/':
            while i < length and content[i] != '\n':
                i += 1
            continue

        # Skip multi-line comments
        if content[i] == '/' and i + 1 < length and content[i + 1] == '*':
            i += 2
            while i < length - 1 and not (content[i] == '*' and content[i + 1] == '/'):
                i += 1
            i += 2
            continue

        # Skip hashbang regions (#region, #endregion)
        if content[i] == '#':
            while i < length and content[i] != '\n':
                i += 1
            continue

        # Expect opening brace
        if content[i] != '{':
            i += 1
            continue

        brace_start = i
        end = find_matching_brace(content, brace_start)
        if end < 0:
            break  # Unmatched brace, stop

        block = content[brace_start:end + 1]
        definitions.append((block, brace_start))
        i = end + 1

    return definitions


def extract_definitions_from_file(filepath):
    """Extract device definitions from a single TypeScript file.

    Uses a two-phase approach:
    1. Find the `export const definitions = [...]` array
    2. Split it into top-level {} blocks
    3. Extract model/vendor/description/extend from each block
    """
    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
    except Exception as e:
        print(f"  Warning: Cannot read {filepath}: {e}", file=sys.stderr)
        return []

    devices = []
    file_manuf = extract_manufacturer_from_file(filepath, content)

    # Phase 1: Find the definitions array
    array_start = _find_definitions_array(content)
    if array_start is None:
        # Fallback: try the old model-search approach for files without a standard definitions array
        return _extract_definitions_fallback(filepath, content, file_manuf)

    # Phase 2: Split into definition blocks
    def_blocks = _split_definitions_array(content, array_start)

    # Phase 3: Parse each block
    for block, block_pos in def_blocks:
        devs = _parse_definition_block(block, file_manuf)
        devices.extend(devs)

    return devices


def _parse_definition_block(block, file_manuf):
    """Parse a single definition block and return a list of device dicts.

    Usually returns 1 device, but may return 0 if the block has no model/vendor.
    """
    devices = []

    # Extract model
    model_match = re.search(r"model:\s*['\"]([^'\"]+)['\"]", block)
    if not model_match:
        return devices

    model = model_match.group(1)

    # Extract vendor and description
    vendor_match = re.search(r"vendor:\s*['\"]([^'\"]+)['\"]", block)
    desc_match = re.search(r"description:\s*['\"]([^'\"]+)['\"]", block)

    # If vendor is missing, try to derive from file manufacturer or spread templates
    if vendor_match:
        vendor = vendor_match.group(1)
    elif file_manuf:
        vendor = file_manuf
    else:
        return devices

    description = desc_match.group(1) if desc_match else ""

    # Find manufacturer from zigbeeModel/fingerprint or file default
    manuf_match = re.search(
        r"(?:zigbeeModel|fingerprint).*?manufacturerName.*?['\"]([^'\"]+)['\"]",
        block, re.DOTALL
    )
    manufacturer = manuf_match.group(1) if manuf_match else file_manuf

    device = {
        "m": model,
        "mf": manufacturer,
        "v": vendor,
        "d": description,
        "fz": [],
        "tz": [],
        "e": [],
        "q": 0,
        "_coverage": "NO_MATCH",
    }

    # Check for ModernExtend: extend: [m.xxx(), ...]
    extends_str = extract_bracket_content(block, "extend")
    collected_tuya_dp = {}
    if extends_str:
        calls = split_extend_calls(extends_str)
        all_matched = True
        any_matched = False

        for call in calls:
            call = call.strip()
            if not call:
                continue
            result, status, tuya_dp = parse_extend_call(call)
            if result is not None:
                device["fz"].extend(result["fz"])
                device["tz"].extend(result["tz"])
                device["e"].extend(result["e"])
                any_matched = True
                if tuya_dp:
                    collected_tuya_dp.update(tuya_dp)
            else:
                all_matched = False

        if any_matched:
            device["_coverage"] = "FULL_MATCH" if all_matched else "PARTIAL"

    # Store collected Tuya DP mappings
    if collected_tuya_dp:
        device["tuya_dp"] = collected_tuya_dp

    # Check for legacy fromZigbee/toZigbee (also check if extend gave us nothing)
    if not device["fz"]:
        fz_str = extract_bracket_content(block, "fromZigbee")
        if fz_str:
            # Match standard fz.xxx, tuya.fz.xxx, lumi.fromZigbee.xxx, fzLocal.xxx, etc.
            fz_refs = re.findall(r'(fz\.\w+)', fz_str)
            any_mapped = False
            all_mapped = True
            for ref in fz_refs:
                if ref in LEGACY_FZ_MAP:
                    device["fz"].extend(LEGACY_FZ_MAP[ref])
                    any_mapped = True
                else:
                    all_mapped = False

            # Also check for tuya.fz.* patterns
            tuya_fz_refs = re.findall(r'tuya\.fz\.(\w+)', fz_str)
            for ref in tuya_fz_refs:
                if f"fz.{ref}" in LEGACY_FZ_MAP:
                    device["fz"].extend(LEGACY_FZ_MAP[f"fz.{ref}"])
                    any_mapped = True
                elif ref in ("power_outage_memory", "power_on_behavior"):
                    device["fz"].extend(LEGACY_FZ_MAP.get("fz.power_on_behavior", []))
                    any_mapped = True
                elif ref in ("indicator_mode",):
                    any_mapped = True  # No-op, but recognized
                else:
                    all_mapped = False

            if any_mapped:
                if device["_coverage"] == "NO_MATCH":
                    device["_coverage"] = "FULL_MATCH" if all_mapped else "PARTIAL"
                elif device["_coverage"] == "FULL_MATCH" and not all_mapped:
                    device["_coverage"] = "PARTIAL"

    if not device["tz"]:
        tz_str = extract_bracket_content(block, "toZigbee")
        if tz_str:
            tz_refs = re.findall(r'(tz\.\w+)', tz_str)
            for ref in tz_refs:
                if ref in LEGACY_TZ_MAP:
                    device["tz"].extend(LEGACY_TZ_MAP[ref])
            # Also check for tuya.tz.* patterns
            tuya_tz_refs = re.findall(r'tuya\.tz\.(\w+)', tz_str)
            for ref in tuya_tz_refs:
                if f"tz.{ref}" in LEGACY_TZ_MAP:
                    device["tz"].extend(LEGACY_TZ_MAP[f"tz.{ref}"])
                elif ref.startswith("power_on_behavior"):
                    device["tz"].extend(LEGACY_TZ_MAP.get("tz.power_on_behavior", []))
                elif ref.startswith("backlight_indicator"):
                    pass  # No-op
                else:
                    pass  # Unknown tuya tz

    # Deduplicate fz/tz entries
    device["fz"] = deduplicate_entries(device["fz"])
    device["tz"] = deduplicate_entries(device["tz"])

    # Include if we got converter info, OR always include with at least model/vendor info
    if device["fz"] or device["e"]:
        devices.append(device)
    elif vendor and model:
        # Include minimal entry for identification purposes
        device["_coverage"] = "PARTIAL" if device["_coverage"] == "NO_MATCH" else device["_coverage"]
        # Add a generic fallback based on what the block contains
        if 'fromZigbee:' in block or 'extend:' in block:
            device["_coverage"] = "PARTIAL"
        devices.append(device)

    return devices


def _extract_definitions_fallback(filepath, content, file_manuf):
    """Fallback extraction using model: search for files without standard definitions array."""
    devices = []
    model_pattern = re.compile(r"model:\s*['\"]([^'\"]+)['\"]")

    for model_match in model_pattern.finditer(content):
        model = model_match.group(1)
        pos = model_match.start()

        # Walk backwards from model: to find the opening '{' of the definition
        brace_pos = -1
        i = pos - 1
        while i >= 0:
            ch = content[i]
            if ch == '{':
                brace_pos = i
                break
            elif ch == '}':
                # Skip backwards over a nested block
                depth = 1
                i -= 1
                while i >= 0 and depth > 0:
                    if content[i] == '}':
                        depth += 1
                    elif content[i] == '{':
                        depth -= 1
                    i -= 1
                continue
            i -= 1

        if brace_pos < 0:
            continue

        end = find_matching_brace(content, brace_pos)
        if end < 0:
            continue
        block = content[brace_pos:end + 1]

        devs = _parse_definition_block(block, file_manuf)
        devices.extend(devs)

    return devices


def extract_manufacturer_from_file(filepath, content):
    """Try to determine default manufacturer from file content."""
    # Common patterns at the top of files
    patterns = [
        r"manufacturerName:\s*\[?['\"]([^'\"]+)['\"]",
        r"vendor:\s*['\"]([^'\"]+)['\"]",
    ]
    for pat in patterns:
        m = re.search(pat, content[:2000])
        if m:
            return m.group(1)

    # Fallback: derive from filename
    stem = Path(filepath).stem.lower()
    manuf_map = {
        "ikea": "IKEA of Sweden",
        "philips": "Philips",
        "signify": "Signify Netherlands B.V.",
        "xiaomi": "LUMI",
        "aqara": "LUMI",
        "sonoff": "SONOFF",
        "tuya": "_TZE200",
        "lidl": "LIDL Silvercrest",
        "innr": "innr",
        "osram": "OSRAM",
        "ledvance": "LEDVANCE",
        "gledopto": "GLEDOPTO",
        "müller_licht": "Muller Licht",
    }
    for key, val in manuf_map.items():
        if key in stem:
            return val
    return stem.upper()


def split_extend_calls(s):
    """Split extend array contents by top-level commas, respecting parens/braces/brackets and strings."""
    calls = []
    depth = 0
    current = []
    in_string = False
    string_char = None
    i = 0
    length = len(s)

    while i < length:
        ch = s[i]

        if in_string:
            current.append(ch)
            if ch == '\\' and i + 1 < length:
                current.append(s[i + 1])
                i += 2
                continue
            if ch == string_char:
                if string_char != '`':
                    in_string = False
                else:
                    in_string = False
            i += 1
            continue

        if ch in ('"', "'", '`'):
            in_string = True
            string_char = ch
            current.append(ch)
        elif ch in '([{':
            depth += 1
            current.append(ch)
        elif ch in ')]}':
            depth -= 1
            current.append(ch)
        elif ch == ',' and depth == 0:
            calls.append(''.join(current))
            current = []
        else:
            current.append(ch)
        i += 1

    if current:
        calls.append(''.join(current))
    return calls


def deduplicate_entries(entries):
    """Remove duplicate fz/tz entries based on fn+k combination."""
    seen = set()
    result = []
    for entry in entries:
        key = (entry.get("fn", ""), entry.get("k", ""))
        if key not in seen:
            seen.add(key)
            result.append(entry)
    return result


def clean_expose(e):
    """Remove None/empty values from expose dict for compact JSON."""
    return {k: v for k, v in e.items() if v is not None and v != ""}


def clean_device(dev):
    """Remove internal fields and clean up device for output."""
    compat = check_device_compat(dev)
    out = {
        "m": dev["m"],
        "mf": dev.get("mf", ""),
        "v": dev.get("v", ""),
        "d": dev.get("d", ""),
        "src": "z2m",
        "pri": 2,
        "fz": dev["fz"],
        "tz": dev["tz"],
        "e": [clean_expose(e) for e in dev.get("e", [])],
        "q": dev.get("q", 0),
        "_compat": compat,
    }
    # Pass through tuya_dp if present
    if dev.get("tuya_dp"):
        out["tuya_dp"] = dev["tuya_dp"]
    # Remove empty arrays
    if not out["tz"]:
        del out["tz"]
    if not out["e"]:
        del out["e"]
    if out["q"] == 0:
        del out["q"]
    return out


def group_by_manufacturer(devices):
    """Group devices by manufacturer, creating filename mapping."""
    groups = defaultdict(list)
    for dev in devices:
        manuf = dev.get("mf", "unknown")
        groups[manuf].append(dev)
    return groups


def manufacturer_to_filename(manufacturer):
    """Convert manufacturer name to a safe filename."""
    name = manufacturer.lower()
    name = re.sub(r'[^a-z0-9_]', '_', name)
    name = re.sub(r'_+', '_', name).strip('_')
    if not name:
        name = "unknown"
    return name + ".json"


def main():
    parser = argparse.ArgumentParser(description="Z2M Converter DB Extractor")
    parser.add_argument("--source", required=True, help="Path to zhc/src/devices/ directory")
    parser.add_argument("--output", help="Output directory for JSON files")
    parser.add_argument("--report", action="store_true", help="Print coverage report")
    parser.add_argument("--min-coverage", choices=["FULL_MATCH", "PARTIAL", "NO_MATCH"],
                        default="PARTIAL", help="Minimum coverage to include in output")
    args = parser.parse_args()

    source_dir = Path(args.source)
    if not source_dir.is_dir():
        print(f"Error: {source_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    # Scan all .ts files
    ts_files = sorted(source_dir.glob("*.ts"))
    if not ts_files:
        print(f"Error: No .ts files found in {source_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Scanning {len(ts_files)} TypeScript files...")

    all_devices = []
    stats = {"FULL_MATCH": 0, "PARTIAL": 0, "NO_MATCH": 0, "UNKNOWN_EXTEND": 0}

    for ts_file in ts_files:
        devices = extract_definitions_from_file(ts_file)
        for dev in devices:
            coverage = dev.get("_coverage", "NO_MATCH")
            if coverage in stats:
                stats[coverage] += 1
            else:
                stats["NO_MATCH"] += 1
        all_devices.extend(devices)
        if devices:
            print(f"  {ts_file.name}: {len(devices)} definitions")

    total = len(all_devices)
    print(f"\nTotal: {total} device definitions extracted")
    print(f"  FULL_MATCH: {stats['FULL_MATCH']} ({stats['FULL_MATCH']*100//max(total,1)}%)")
    print(f"  PARTIAL:    {stats['PARTIAL']} ({stats['PARTIAL']*100//max(total,1)}%)")
    print(f"  NO_MATCH:   {stats['NO_MATCH']} ({stats['NO_MATCH']*100//max(total,1)}%)")

    if args.report:
        print("\n--- Detailed Coverage Report ---")
        for dev in sorted(all_devices, key=lambda d: d.get("_coverage", "")):
            cov = dev.get("_coverage", "NO_MATCH")
            print(f"  [{cov:10s}] {dev['mf']:30s} {dev['m']:30s} ({len(dev['fz'])} fz, {len(dev['tz'])} tz)")
        return

    if not args.output:
        print("No --output specified, dry run complete.")
        return

    # Filter by minimum coverage
    coverage_levels = {"FULL_MATCH": 3, "PARTIAL": 2, "NO_MATCH": 1}
    min_level = coverage_levels.get(args.min_coverage, 2)
    filtered = [d for d in all_devices
                if coverage_levels.get(d.get("_coverage", "NO_MATCH"), 1) >= min_level]

    # When FULL_MATCH is requested, also filter by ESP32 compatibility
    if args.min_coverage == "FULL_MATCH":
        before_compat = len(filtered)
        filtered = [d for d in filtered if check_device_compat(d)]
        compat_removed = before_compat - len(filtered)
        if compat_removed > 0:
            print(f"  Removed {compat_removed} devices with unsupported fz/tz functions")

    print(f"\nOutputting {len(filtered)} devices (min coverage: {args.min_coverage})")

    # Group by manufacturer
    groups = group_by_manufacturer(filtered)

    # Create output directory
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Generate per-manufacturer files
    manuf_info = {}
    total_devices = 0

    for manuf, devices in sorted(groups.items()):
        filename = manufacturer_to_filename(manuf)

        cleaned = [clean_device(d) for d in devices]
        total_devices += len(cleaned)

        out_path = out_dir / filename
        with open(out_path, 'w') as f:
            json.dump({"devices": cleaned}, f, separators=(',', ':'), ensure_ascii=False)

        manuf_info[manuf] = {"file": filename, "count": len(cleaned)}
        print(f"  {filename}: {len(cleaned)} devices")

    # Generate index.json (v2 format)
    index = {
        "version": 2,
        "sources": {
            "z2m": {"ts": date.today().isoformat(), "commit": "unknown"},
        },
        "manufacturers": manuf_info,
        "total_devices": total_devices,
    }

    index_path = out_dir / "index.json"
    with open(index_path, 'w') as f:
        json.dump(index, f, separators=(',', ':'), ensure_ascii=False)

    print(f"\nGenerated {len(manuf_info)} manufacturer files + index.json")
    print(f"Total: {total_devices} devices in {out_dir}")

    # Size report
    total_bytes = sum(f.stat().st_size for f in out_dir.iterdir() if f.is_file())
    print(f"Total size: {total_bytes:,} bytes ({total_bytes/1024:.1f} KB)")


if __name__ == "__main__":
    main()
