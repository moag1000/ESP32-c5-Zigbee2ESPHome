#!/usr/bin/env python3
"""
ZHA Quirks Transpiler

Parses zhaquirks Python sources using AST analysis and generates compact
JSON definitions for the ESP32-C5 runtime converter loader.

Handles both V1 (CustomDevice) and V2 (QuirkBuilder/TuyaQuirkBuilder) quirk
patterns. Extracts signature/replacement cluster lists and maps them to the
same fz_/tz_ C function names used by the z2m transpiler.

Usage:
    python3 zhaquirks_transpiler.py --source tools/zhaquirks/zhaquirks --output data/converters_zhaquirks/
    python3 zhaquirks_transpiler.py --source tools/zhaquirks/zhaquirks --report
    python3 zhaquirks_transpiler.py --help
"""

import argparse
import ast
import json
import os
import re
import sys
from collections import defaultdict
from datetime import date
from pathlib import Path

# ============================================================================
# Well-known ZCL cluster IDs (symbolic name -> numeric ID)
# ============================================================================

ZCL_CLUSTER_IDS = {
    # General
    "Basic": 0,
    "PowerConfiguration": 1,
    "DeviceTemperature": 2,
    "Identify": 3,
    "Groups": 4,
    "Scenes": 5,
    "OnOff": 6,
    "OnOffConfiguration": 7,
    "LevelControl": 8,
    "Alarms": 9,
    "Time": 10,
    "Commissioning": 11,
    "AnalogInput": 12,
    "AnalogOutput": 13,
    "AnalogValue": 14,
    "BinaryInput": 15,
    "BinaryOutput": 16,
    "BinaryValue": 17,
    "MultistateInput": 18,
    "MultistateOutput": 19,
    "MultistateValue": 20,
    "Ota": 25,
    "PollControl": 32,
    "GreenPowerProxy": 33,
    # Closures
    "DoorLock": 257,
    "WindowCovering": 258,
    # HVAC
    "Thermostat": 513,
    "FanControl": 514,
    "Fan": 514,
    "UserInterface": 516,
    # Lighting
    "ColorControl": 768,
    "Color": 768,
    "BallastConfiguration": 769,
    "Ballast": 769,
    # Measurement
    "IlluminanceMeasurement": 1024,
    "IlluminanceLevelSensing": 1025,
    "TemperatureMeasurement": 1026,
    "PressureMeasurement": 1027,
    "FlowMeasurement": 1028,
    "RelativeHumidity": 1029,
    "OccupancySensing": 1030,
    "SoilMoisture": 1032,
    "PM25": 1066,
    "PM": 1066,
    "CarbonDioxideConcentration": 1037,
    "FormaldehydeConcentration": 1067,
    "VOCMeasurement": 1070,
    # Security
    "IasZone": 1280,
    "IasAce": 1281,
    "IasWd": 1282,
    # Smart Energy
    "Metering": 1794,
    # Home Automation
    "ElectricalMeasurement": 2820,
    "Diagnostic": 2821,
    # Lighting Link
    "LightLink": 4096,
    # Tuya
    "TuyaManufCluster": 0xEF00,
    "TuyaManufacturerWindowCover": 0xEF00,
    "TuyaWindowCoverControl": 0xEF00,
    "TuyaManufClusterAttributes": 0xEF00,
    "TuyaMCUCluster": 0xEF00,
    "TuyaOnOffNM": 0xEF00,
    "TuyaNewManufCluster": 0xEF00,
    "TuyaLocalCluster": 0xEF00,
    "TuyaZBExternalSwitchTypeCluster": 0xEF00,
    "TuyaZBE000Cluster": 0xE000,
    "TuyaZBElectricalMeasurement": 2820,
    "TuyaZBMeteringCluster": 1794,
    "TuyaZBOnOffAttributeCluster": 6,
    # Xiaomi/Aqara
    "XiaomiAqaraE1Cluster": 0xFCC0,
    "XiaomiCustomCluster": 0xFCC0,
    "OppleCluster": 0xFCC0,
    "AqaraOppleCluster": 0xFCC0,
    "XiaomiPowerConfigurationCluster": 1,
    "BasicCluster": 0,
    # IKEA
    "IkeaCluster": 0xFC7C,
    "VOCIndex": 0xFC7E,
    # Philips/Signify
    "PhilipsBasicCluster": 0,
    "PhilipsRemoteCluster": 0xFC00,
    "HueOccupancy": 1030,
    # Schneider
    "SchneiderCluster": 0xFF17,
    # Legrand
    "LegrandCluster": 0xFC01,
    # Danfoss
    "DanfossThermostatCluster": 513,
    "DanfossUserInterfaceCluster": 516,
    "DanfossDiagnosticCluster": 2821,
    # Generic custom clusters
    "CustomBasicCluster": 0,
    "CustomPowerConfigurationCluster": 1,
    # Centralite
    "CentraliteAccelCluster": 0xFC02,
    # Sonoff
    "SonoffCluster": 0xFC11,
}

# Also map class.cluster_id references via import patterns
CLUSTER_CLASS_ALIASES = {
    "PowerConfigurationCluster": 1,
    "DoublingPowerConfigClusterIKEA": 1,
    "DoublingPowerConfigurationCluster": 1,
    "MotionWithReset": 1280,
    "MotionOnEvent": 1280,
    "OccupancyWithReset": 1030,
    "OccupancyOnEvent": 1030,
    "PhilipsOccupancySensing": 1030,
    "PhilipsBasicCluster": 0,
    "VOCIndex": 0xFC7E,
    "TuyaLocalCluster": 0xEF00,
    "TuyaWindowCover": 258,
    "TuyaManufacturerLevelControl": 0xEF00,
    "TuyaReplacementCluster": 0xEF00,
    "ScenesCluster": 5,
    "EventableCluster": None,  # Ignore
    "LocalDataCluster": None,  # Ignore
    "GroupBoundCluster": None,  # Ignore
    "NoReplyMixin": None,  # Ignore
    # Additional cluster class aliases for custom clusters
    "SmartThingsAccelCluster": 0xFC02,
    "SmartThingsHumidity": 0xFC45,
    "HueOccupancySensing": 1030,
    "IkeaPowerConfig": 1,
    "SonoffPresenceSensor": 0xFC11,
    "AqaraOppleCluster": 0xFCC0,
    "HueMotion": 1280,
    "NodOnPilotWireCluster": 0xFC00,
    "DevelcoPowerConfiguration": 1,
}


# ============================================================================
# Cluster -> fz/tz/expose Mapping (same as z2m transpiler)
# ============================================================================

CLUSTER_FZ_MAP = {
    6: [{"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"}],
    8: [{"c": 8, "a": 0, "k": "brightness", "fn": "fz_brightness"}],
    768: [
        {"c": 768, "a": 7, "k": "color_temp", "fn": "fz_color_temp"},
        {"c": 768, "a": 0, "k": "color_hue", "fn": "fz_color_hs"},
        {"c": 768, "a": 3, "k": "color_x", "fn": "fz_color_xy"},
    ],
    1026: [{"c": 1026, "a": 0, "k": "temperature", "fn": "fz_temperature"}],
    1029: [{"c": 1029, "a": 0, "k": "humidity", "fn": "fz_humidity"}],
    1027: [{"c": 1027, "a": 0, "k": "pressure", "fn": "fz_pressure"}],
    1: [{"c": 1, "a": 33, "k": "battery", "fn": "fz_battery"}],
    1024: [{"c": 1024, "a": 0, "k": "illuminance", "fn": "fz_illuminance"}],
    1030: [{"c": 1030, "a": 0, "k": "occupancy", "fn": "fz_occupancy"}],
    1280: [{"c": 1280, "a": 2, "k": "alarm", "fn": "fz_ias_zone_status"}],
    2820: [
        {"c": 2820, "a": 1291, "k": "power", "fn": "fz_electrical_power"},
        {"c": 2820, "a": 1288, "k": "current", "fn": "fz_current"},
        {"c": 2820, "a": 1285, "k": "voltage", "fn": "fz_voltage"},
    ],
    1794: [{"c": 1794, "a": 0, "k": "energy", "fn": "fz_metering"}],
    513: [
        {"c": 513, "a": 0, "k": "local_temperature", "fn": "fz_thermostat_local_temp"},
        {"c": 513, "a": 28, "k": "system_mode", "fn": "fz_thermostat_system_mode"},
        {"c": 513, "a": 41, "k": "running_state", "fn": "fz_thermostat_running_state"},
    ],
    258: [
        {"c": 258, "a": 8, "k": "position", "fn": "fz_cover_position"},
        {"c": 258, "a": 9, "k": "tilt", "fn": "fz_cover_tilt"},
    ],
    257: [{"c": 257, "a": 0, "k": "state", "fn": "fz_lock_state"}],
    514: [{"c": 514, "a": 0, "k": "fan_mode", "fn": "fz_fan_mode"}],
    0xEF00: [{"c": 61184, "a": 65535, "k": "tuya_dp", "fn": "fz_tuya_dp"}],
    0xFCC0: [{"c": 0xFCC0, "a": 0, "k": "aqara_opple", "fn": "fz_aqara_opple"}],
    1037: [{"c": 1037, "a": 0, "k": "co2", "fn": "fz_co2"}],
    1066: [{"c": 1066, "a": 0, "k": "pm25", "fn": "fz_pm25"}],
    1070: [{"c": 1070, "a": 0, "k": "voc", "fn": "fz_voc"}],
    1032: [{"c": 1032, "a": 0, "k": "soil_moisture", "fn": "fz_soil_moisture"}],
    2: [{"c": 2, "a": 0, "k": "device_temperature", "fn": "fz_device_temperature"}],
    18: [{"c": 18, "a": 85, "k": "action", "fn": "fz_multistate_input"}],
    12: [{"c": 12, "a": 85, "k": "analog_value", "fn": "fz_analog_input"}],
    1282: [{"c": 1282, "a": 0, "k": "warning", "fn": "fz_ias_wd"}],
}

CLUSTER_TZ_MAP = {
    6: [{"k": "state", "c": 6, "fn": "tz_on_off"}],
    8: [{"k": "brightness", "c": 8, "fn": "tz_brightness"}],
    768: [
        {"k": "color_temp", "c": 768, "fn": "tz_color_temp"},
        {"k": "color", "c": 768, "fn": "tz_color_hs"},
    ],
    258: [
        {"k": "position", "c": 258, "fn": "tz_cover_position"},
        {"k": "tilt", "c": 258, "fn": "tz_cover_tilt"},
    ],
    257: [{"k": "state", "c": 257, "fn": "tz_lock_command"}],
    513: [
        {"k": "occupied_heating_setpoint", "c": 513, "fn": "tz_thermostat_setpoint"},
        {"k": "system_mode", "c": 513, "fn": "tz_thermostat_system_mode"},
    ],
    514: [{"k": "fan_mode", "c": 514, "fn": "tz_fan_mode"}],
    0xEF00: [{"k": "tuya_dp", "c": 61184, "fn": "tz_tuya_dp"}],
    0xFCC0: [{"k": "aqara_opple", "c": 0xFCC0, "fn": "tz_aqara_opple"}],
    1282: [{"k": "warning", "c": 1282, "fn": "tz_warning"}],
}

# Cluster -> expose mapping
CLUSTER_EXPOSE_MAP = {
    6: [{"t": 1, "f": 0, "p": "state", "ac": 3}],  # SWITCH
    8: [{"t": 0, "f": 3, "p": "state", "ac": 3}],  # LIGHT + BRIGHTNESS
    768: [{"t": 0, "f": 15, "p": "state", "ac": 3}],  # LIGHT + COLOR
    1026: [{"t": 2, "f": 0, "dc": "temperature", "u": "\u00b0C", "sc": "measurement", "p": "temperature", "ac": 1}],
    1029: [{"t": 2, "f": 0, "dc": "humidity", "u": "%", "sc": "measurement", "p": "humidity", "ac": 1}],
    1027: [{"t": 2, "f": 0, "dc": "pressure", "u": "hPa", "sc": "measurement", "p": "pressure", "ac": 1}],
    1: [{"t": 2, "f": 0, "dc": "battery", "u": "%", "sc": "measurement", "p": "battery", "ac": 1}],
    1024: [{"t": 2, "f": 0, "dc": "illuminance", "u": "lx", "sc": "measurement", "p": "illuminance", "ac": 1}],
    1030: [{"t": 3, "f": 0, "dc": "motion", "p": "occupancy", "ac": 1}],
    1280: [{"t": 3, "f": 0, "p": "alarm", "ac": 1}],
    2820: [
        {"t": 2, "f": 0, "dc": "power", "u": "W", "sc": "measurement", "p": "power", "ac": 1},
        {"t": 2, "f": 0, "dc": "current", "u": "A", "sc": "measurement", "p": "current", "ac": 1},
        {"t": 2, "f": 0, "dc": "voltage", "u": "V", "sc": "measurement", "p": "voltage", "ac": 1},
    ],
    1794: [{"t": 2, "f": 0, "dc": "energy", "u": "kWh", "sc": "total_increasing", "p": "energy", "ac": 1}],
    513: [{"t": 6, "f": 0, "p": "local_temperature", "ac": 1}],  # CLIMATE
    258: [{"t": 4, "f": 96, "p": "position", "ac": 3}],  # COVER
    257: [{"t": 5, "f": 0, "p": "state", "ac": 3}],  # LOCK
    514: [{"t": 7, "f": 0, "p": "fan_mode", "ac": 3}],  # FAN
    1037: [{"t": 2, "f": 0, "dc": "carbon_dioxide", "u": "ppm", "sc": "measurement", "p": "co2", "ac": 1}],
    1066: [{"t": 2, "f": 0, "dc": "pm25", "u": "\u00b5g/m\u00b3", "sc": "measurement", "p": "pm25", "ac": 1}],
    1070: [{"t": 2, "f": 0, "u": "ppb", "sc": "measurement", "p": "voc", "ac": 1}],
    1032: [{"t": 2, "f": 0, "dc": "moisture", "u": "%", "sc": "measurement", "p": "soil_moisture", "ac": 1}],
    2: [{"t": 2, "f": 0, "dc": "temperature", "u": "\u00b0C", "sc": "measurement", "p": "device_temperature", "ac": 1}],
}

# Clusters that don't map to fz/tz (utility clusters)
UTILITY_CLUSTERS = {0, 3, 4, 5, 7, 9, 10, 25, 32, 33, 2821, 4096, 0xFC57}

# Quirk flags
ZB_QUIRK_TUYA_DEVICE = 16
ZB_QUIRK_XIAOMI_INIT = 32
ZB_QUIRK_CUSTOM_REPORTING = 256
ZB_QUIRK_SKIP_CONFIG = 512

# ============================================================================
# ZHA DeviceType -> implied clusters mapping
# ============================================================================

# Maps ZHA DeviceType values to the standard ZCL clusters that are expected
# on those device types. Used to infer clusters when replaces_endpoint()
# specifies a device_type but no explicit cluster replacements.
DEVICE_TYPE_CLUSTERS = {
    # Lights
    "ON_OFF_LIGHT": {6},
    "DIMMABLE_LIGHT": {6, 8},
    "COLOR_DIMMABLE_LIGHT": {6, 8, 768},
    "COLOR_TEMPERATURE_LIGHT": {6, 8, 768},
    "EXTENDED_COLOR_LIGHT": {6, 8, 768},
    # Switches/Outlets
    "ON_OFF_OUTPUT": {6},
    "ON_OFF_SWITCH": {6},
    "ON_OFF_PLUG_IN_UNIT": {6},
    "SMART_PLUG": {6},
    "DIMMABLE_PLUG_IN_UNIT": {6, 8},
    "DIMMER_SWITCH": {6, 8},
    # Covers
    "WINDOW_COVERING": {258},
    "WINDOW_COVERING_DEVICE": {258},
    "WINDOW_COVERING_CONTROLLER": {258},
    "SHADE": {258},
    "SHADE_CONTROLLER": {258},
    # HVAC
    "THERMOSTAT": {513},
    # Sensors
    "OCCUPANCY_SENSOR": {1030},
    "TEMPERATURE_SENSOR": {1026},
    "LIGHT_SENSOR": {1024},
    # IAS
    "IAS_ZONE": {1280},
    "IAS_ANCILLARY_CONTROL": {1281},
    "IAS_WARNING": {1282},
    # Door Lock
    "DOOR_LOCK": {257},
    "DOOR_LOCK_CONTROLLER": {257},
    # Fan
    "FAN": {514},
    # Remote controls (have basic capabilities)
    "REMOTE_CONTROL": set(),
    "NON_COLOR_CONTROLLER": set(),
    "COLOR_CONTROLLER": set(),
    "NON_COLOR_SCENE_CONTROLLER": set(),
    "COLOR_SCENE_CONTROLLER": set(),
}

# ============================================================================
# ESP32 Function Registries (same as z2m transpiler)
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
    """Check if all fz/tz functions are in ESP32 registries."""
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


# ============================================================================
# AST Helpers
# ============================================================================

def resolve_cluster_id(node, file_cluster_ids=None):
    """Try to resolve a cluster ID from an AST node.

    Returns int cluster_id or None if unresolvable.
    """
    if file_cluster_ids is None:
        file_cluster_ids = {}

    # Literal integer: e.g. 0x0006, 258, 0xEF00
    if isinstance(node, ast.Constant) and isinstance(node.value, int):
        return node.value

    # Negative number (rare but handle)
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        if isinstance(node.operand, ast.Constant) and isinstance(node.operand.value, int):
            return -node.operand.value

    # Name reference: e.g. TUYA_CLUSTER_ID, IKEA_CLUSTER_ID
    if isinstance(node, ast.Name):
        name = node.id
        # Check known constants
        known = {
            "TUYA_CLUSTER_ID": 0xEF00,
            "TUYA_CLUSTER_E000_ID": 0xE000,
            "TUYA_CLUSTER_E001_ID": 0xE001,
            "IKEA_CLUSTER_ID": 0xFC7C,
            "WWAH_CLUSTER_ID": 0xFC57,
            "MANUFACTURER_SPECIFIC_PROFILE_ID": None,
            "IKEA_SHORTCUT_CLUSTER_V1_ID": 0xFC7F,
            "IKEA_MATTER_SWITCH_CLUSTER_ID": 0xFC80,
        }
        if name in known:
            return known[name]
        # Check file-level cluster IDs
        if name in file_cluster_ids:
            return file_cluster_ids[name]
        # Check ZCL class names
        if name in ZCL_CLUSTER_IDS:
            return ZCL_CLUSTER_IDS[name]
        # Check alias names (custom cluster classes)
        if name in CLUSTER_CLASS_ALIASES:
            return CLUSTER_CLASS_ALIASES[name]
        return None

    # Attribute reference: e.g. Basic.cluster_id, OnOff.cluster_id,
    # WindowCovering.cluster_id, TuyaManufCluster.cluster_id
    if isinstance(node, ast.Attribute):
        if node.attr == "cluster_id" and isinstance(node.value, ast.Name):
            class_name = node.value.id
            if class_name in ZCL_CLUSTER_IDS:
                return ZCL_CLUSTER_IDS[class_name]
            if class_name in CLUSTER_CLASS_ALIASES:
                return CLUSTER_CLASS_ALIASES[class_name]
            if class_name in file_cluster_ids:
                return file_cluster_ids[class_name]
        # zha.PROFILE_ID etc -- not cluster IDs
        return None

    return None


def extract_list_elements(node, file_cluster_ids=None):
    """Extract cluster IDs from an AST List node.

    Elements can be:
    - Integer literals (0x0006)
    - Name references (Basic.cluster_id)
    - Class references used as replacement clusters (PowerConfigurationCluster)
    """
    if not isinstance(node, ast.List):
        return []

    result = []
    for elt in node.elts:
        cid = resolve_cluster_id(elt, file_cluster_ids)
        if cid is not None:
            result.append(cid)
        elif isinstance(elt, ast.Name):
            # May be a custom cluster class used in replacement
            name = elt.id
            if name in file_cluster_ids:
                cid = file_cluster_ids[name]
                if cid is not None:
                    result.append(cid)
    return result


def extract_string(node, constants=None):
    """Extract string from AST Constant or JoinedStr (f-string).

    For f-strings like f" {LEGRAND}", tries to resolve variable references
    using the provided constants dict.
    """
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.JoinedStr) and constants:
        # Try to evaluate simple f-strings by concatenating parts
        parts = []
        for value in node.values:
            if isinstance(value, ast.Constant) and isinstance(value.value, str):
                parts.append(value.value)
            elif isinstance(value, ast.FormattedValue):
                # The inner value is a Name or expression
                inner = value.value
                if isinstance(inner, ast.Name) and inner.id in constants:
                    val = constants[inner.id]
                    if isinstance(val, str):
                        parts.append(val)
                    else:
                        return None  # Can't resolve
                else:
                    return None  # Can't resolve
            else:
                return None
        return "".join(parts)
    return None


def extract_models_info(node, file_constants=None):
    """Extract (manufacturer, model) tuples from MODELS_INFO list.

    The pattern is:
        MODELS_INFO: [("mfr", "model"), ...]
    or
        MODELS_INFO: [(IKEA, "model"), ...]
    """
    if file_constants is None:
        file_constants = {}

    if not isinstance(node, (ast.List, ast.Tuple)):
        return []

    result = []
    for elt in node.elts:
        if isinstance(elt, (ast.Tuple, ast.List)) and len(elt.elts) >= 2:
            mfr_node = elt.elts[0]
            model_node = elt.elts[1]

            mfr = extract_string(mfr_node, file_constants)
            if mfr is None and isinstance(mfr_node, ast.Name):
                mfr = file_constants.get(mfr_node.id, mfr_node.id)

            model = extract_string(model_node, file_constants)
            if model is None and isinstance(model_node, ast.Name):
                model = file_constants.get(model_node.id, model_node.id)

            if mfr and model:
                result.append((mfr, model))

    return result


def find_dict_key(dict_node, key_name):
    """Find the value for a given key in an AST Dict node.

    Keys can be Name (MODELS_INFO) or Constant (1, "endpoints").
    """
    if not isinstance(dict_node, ast.Dict):
        return None

    for k, v in zip(dict_node.keys, dict_node.values):
        if isinstance(k, ast.Name) and k.id == key_name:
            return v
        if isinstance(k, ast.Constant) and k.value == key_name:
            return v
    return None


def extract_int_key_dict(dict_node):
    """Extract {int: value} entries from a Dict node (endpoint dicts)."""
    if not isinstance(dict_node, ast.Dict):
        return {}

    result = {}
    for k, v in zip(dict_node.keys, dict_node.values):
        if isinstance(k, ast.Constant) and isinstance(k.value, int):
            result[k.value] = v
    return result


# ============================================================================
# File-level constant extraction
# ============================================================================

def extract_file_constants(tree):
    """Extract module-level string and int constants from the AST.

    Returns dict of name -> value for constants like:
        IKEA = "IKEA of Sweden"
        LUMI = "LUMI"
        CENTRALITE = "CentraLite"
        IKEA_CLUSTER_ID = 0xFC7C
    """
    constants = {}
    cluster_ids = {}

    for node in ast.iter_child_nodes(tree):
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            if isinstance(target, ast.Name):
                name = target.id
                value = node.value

                if isinstance(value, ast.Constant):
                    if isinstance(value.value, str):
                        constants[name] = value.value
                    elif isinstance(value.value, int):
                        constants[name] = value.value
                        # If it looks like a cluster ID constant
                        if "CLUSTER" in name or value.value > 255:
                            cluster_ids[name] = value.value

    return constants, cluster_ids


def extract_file_cluster_classes(tree, file_cluster_ids):
    """Find custom cluster classes defined in the file and map class name -> cluster_id.

    Patterns:
        class MyCluster(CustomCluster):
            cluster_id = 0xE002

        class MyCluster(CustomCluster, OnOff):
            ...  # inherits cluster_id from OnOff
    """
    for node in ast.iter_child_nodes(tree):
        if not isinstance(node, ast.ClassDef):
            continue

        # Check if it inherits from a known cluster class
        parent_cluster_id = None
        for base in node.bases:
            if isinstance(base, ast.Name):
                if base.id in ZCL_CLUSTER_IDS:
                    parent_cluster_id = ZCL_CLUSTER_IDS[base.id]
                elif base.id in CLUSTER_CLASS_ALIASES:
                    # Only update if the alias resolves to an actual ID
                    # (None means "ignore this base class")
                    alias_val = CLUSTER_CLASS_ALIASES[base.id]
                    if alias_val is not None:
                        parent_cluster_id = alias_val
                elif base.id in file_cluster_ids:
                    parent_cluster_id = file_cluster_ids[base.id]
            elif isinstance(base, ast.Attribute):
                if base.attr in ZCL_CLUSTER_IDS:
                    parent_cluster_id = ZCL_CLUSTER_IDS[base.attr]

        # Check for explicit cluster_id assignment in class body
        explicit_id = None
        for item in node.body:
            if isinstance(item, ast.Assign):
                for t in item.targets:
                    if isinstance(t, ast.Name) and t.id == "cluster_id":
                        if isinstance(item.value, ast.Constant) and isinstance(item.value.value, int):
                            explicit_id = item.value.value
            elif isinstance(item, ast.AnnAssign):
                if isinstance(item.target, ast.Name) and item.target.id == "cluster_id":
                    if item.value and isinstance(item.value, ast.Constant) and isinstance(item.value.value, int):
                        explicit_id = item.value.value

        cid = explicit_id if explicit_id is not None else parent_cluster_id
        if cid is not None:
            file_cluster_ids[node.name] = cid


def extract_imported_constants(tree, source_dir):
    """Extract constants from import statements.

    Handles patterns like:
        from zhaquirks.ikea import IKEA, IKEA_CLUSTER_ID
        from zhaquirks.centralite import CENTRALITE
    """
    constants = {}
    cluster_ids = {}

    # Well-known vendor constants
    vendor_constants = {
        "IKEA": "IKEA of Sweden",
        "LUMI": "LUMI",
        "CENTRALITE": "CentraLite",
        "PHILIPS": "Philips",
        "SIGNIFY": "Signify Netherlands B.V.",
        "OSRAM": "OSRAM",
        "LEDVANCE": "LEDVANCE",
        "INNR": "innr",
        "SONOFF": "SONOFF",
        "BOSCH": "Bosch",
        "SCHNEIDER": "Schneider Electric",
        "SE_MANUF_NAME": "Schneider Electric",
        "LEGRAND": "Legrand",
        "TUYA": "Tuya",
        "DANFOSS": "Danfoss",
        "EUROTRONIC": "Eurotronic",
        "HEIMAN": "Heiman",
        "DEVELCO": "Develco Products A/S",
        "LINKIND": "Linkind",
        "NIMLY": "Onesti Products AS",
        "AQARA": "Aqara",
        "CANDEO": "Candeo",
        "FRIENT": "frient A/S",
        "ELKO": "ELKO",
        "FEIBIT": "FeiBit",
        "GLEDOPTO": "GLEDOPTO",
        "HIVEHOME": "HiveHome.com",
        "ILUMINIZE": "iluminize",
        "IMAGIC": "iMagic by GreatStar",
        "INSTA": "Insta GmbH",
        "KONKE": "Konke",
        "LINXURA": "Linxura",
        "LIXEE": "LiXee",
        "ORVIBO": "\u6b27\u745e\u535a",
        "ORVIBO_LATIN": "ORVIBO",
        "PAULMANN": "Paulmann LichtGmbH",
        "PAULMANN_VARIANT": "Paulmann Licht GmbH",
        "PHILIO": "Philio",
        "PLAID_SYSTEMS": "PLAID SYSTEMS",
        "SAMJIN": "Samjin",
        "SERCOMM": "Sercomm Corp.",
        "SINOPE": "Sinope Technologies",
        "SMART_THINGS": "SmartThings",
        "THIRD_REALITY": "Third Reality, Inc",
        "TRUST": "Trust",
        "WAXMAN": "WAXMAN",
        "ZEN": "Zen Within",
        "ZUNZUNBEE": "zunzunbee",
        "COMPUTIME": "Computime",
        "BITRON": "Bitron Home",
    }

    # Well-known cluster ID constants
    known_cluster_constants = {
        "IKEA_CLUSTER_ID": 0xFC7C,
        "WWAH_CLUSTER_ID": 0xFC57,
        "TUYA_CLUSTER_ID": 0xEF00,
        "TUYA_CLUSTER_E000_ID": 0xE000,
        "TUYA_CLUSTER_E001_ID": 0xE001,
        "IKEA_SHORTCUT_CLUSTER_V1_ID": 0xFC7F,
        "IKEA_MATTER_SWITCH_CLUSTER_ID": 0xFC80,
    }

    for node in ast.iter_child_nodes(tree):
        if isinstance(node, ast.ImportFrom):
            if node.names:
                for alias in node.names:
                    name = alias.asname or alias.name
                    if name in vendor_constants:
                        constants[name] = vendor_constants[name]
                    if name in known_cluster_constants:
                        cluster_ids[name] = known_cluster_constants[name]
                        constants[name] = known_cluster_constants[name]

    return constants, cluster_ids


# ============================================================================
# V1 Quirk Extraction (CustomDevice subclasses)
# ============================================================================

def extract_v1_quirks(tree, file_constants, file_cluster_ids, vendor_name, filepath):
    """Extract device definitions from V1 CustomDevice classes.

    Each class has:
    - signature dict with MODELS_INFO and ENDPOINTS
    - replacement dict with ENDPOINTS containing cluster lists
    """
    devices = []

    for node in ast.iter_child_nodes(tree):
        if not isinstance(node, ast.ClassDef):
            continue

        # Check if it inherits from CustomDevice (or a subclass thereof)
        is_custom_device = False
        for base in node.bases:
            base_name = None
            if isinstance(base, ast.Name):
                base_name = base.id
            elif isinstance(base, ast.Attribute):
                base_name = base.attr

            if base_name in (
                "CustomDevice", "QuickInitDevice",
                "XiaomiQuickInitDevice", "QuickInitDevice",
                "TuyaWindowCover", "TuyaSmartRemoteOnOffCluster",
                "TuyaDimmerSwitch", "TuyaSwitch",
                "BaseEnchantedDevice", "EnchantedDevice",
                "Tuya6ButtonTriggers", "Tuya4ButtonTriggers",
                "TuyaSingleSwitch", "TuyaDoubleSwitch",
                "TuyaTripleSwitch", "TuyaQuadSwitch",
                "TuyaSingleNoNeutralSwitch", "TuyaDoubleNoNeutralSwitch",
                "TuyaTripleNoNeutralSwitch",
            ):
                is_custom_device = True
                break
            # Broader heuristic: if not a known non-device class
            if base_name and base_name not in (
                "CustomCluster", "LocalDataCluster", "EventableCluster",
                "GroupBoundCluster", "NoReplyMixin", "Bus",
                "DoublingPowerConfigurationCluster", "PowerConfigurationCluster",
                "MotionWithReset", "MotionOnEvent", "OccupancyWithReset",
                "OccupancyOnEvent", "_Motion", "_Occupancy",
                # Known non-device helper/mixin classes
                "BaseAttributeDefs", "BaseCommandDefs", "ZCLAttributeDef",
                "ZCLCommandDef",
            ):
                # Check if the class has both 'signature' and 'replacement' attributes
                has_sig = False
                has_repl = False
                for item in node.body:
                    if isinstance(item, ast.Assign):
                        for t in item.targets:
                            if isinstance(t, ast.Name):
                                if t.id == "signature":
                                    has_sig = True
                                elif t.id == "replacement":
                                    has_repl = True
                if has_sig:
                    is_custom_device = True

        if not is_custom_device:
            continue

        # Extract signature and replacement dicts
        sig_node = None
        repl_node = None
        desc = ""

        for item in node.body:
            if isinstance(item, ast.Assign):
                for t in item.targets:
                    if isinstance(t, ast.Name):
                        if t.id == "signature" and isinstance(item.value, ast.Dict):
                            sig_node = item.value
                        elif t.id == "replacement" and isinstance(item.value, ast.Dict):
                            repl_node = item.value

            # Docstring
            if isinstance(item, ast.Expr) and isinstance(item.value, ast.Constant):
                if isinstance(item.value.value, str) and not desc:
                    desc = item.value.value.strip()

        if sig_node is None:
            continue

        # Extract models_info from signature
        models_info_node = find_dict_key(sig_node, "MODELS_INFO")
        if models_info_node is None:
            # Try the string key version
            models_info_node = find_dict_key(sig_node, "models_info")

        models = []
        if models_info_node is not None:
            models = extract_models_info(models_info_node, file_constants)

        # Fallback: try MODEL + optional MANUFACTURER keys
        # Pattern: MODEL: "TS130F" (no manufacturer) or
        #          MODEL: "model", MANUFACTURER: "manufacturer"
        if not models:
            model_node = find_dict_key(sig_node, "MODEL")
            if model_node is None:
                model_node = find_dict_key(sig_node, "model")
            manuf_node = find_dict_key(sig_node, "MANUFACTURER")
            if manuf_node is None:
                manuf_node = find_dict_key(sig_node, "manufacturer")

            if model_node:
                model_str = extract_string(model_node, file_constants)
                if model_str is None and isinstance(model_node, ast.Name):
                    model_str = file_constants.get(model_node.id)
                manuf_str = ""
                if manuf_node:
                    manuf_str = extract_string(manuf_node, file_constants) or ""
                    if not manuf_str and isinstance(manuf_node, ast.Name):
                        manuf_str = file_constants.get(manuf_node.id, "")
                if model_str:
                    models.append((manuf_str, model_str))

        if not models:
            continue

        # Extract clusters from replacement (preferred) or signature endpoints
        all_input_clusters = set()
        all_output_clusters = set()

        target_node = repl_node if repl_node is not None else sig_node
        endpoints_node = find_dict_key(target_node, "ENDPOINTS")
        if endpoints_node is None:
            endpoints_node = find_dict_key(target_node, "endpoints")

        if endpoints_node and isinstance(endpoints_node, ast.Dict):
            ep_dict = extract_int_key_dict(endpoints_node)
            for ep_id, ep_node in ep_dict.items():
                if isinstance(ep_node, ast.Dict):
                    input_node = find_dict_key(ep_node, "INPUT_CLUSTERS")
                    if input_node is None:
                        input_node = find_dict_key(ep_node, "input_clusters")
                    output_node = find_dict_key(ep_node, "OUTPUT_CLUSTERS")
                    if output_node is None:
                        output_node = find_dict_key(ep_node, "output_clusters")

                    if input_node:
                        all_input_clusters.update(extract_list_elements(input_node, file_cluster_ids))
                    if output_node:
                        all_output_clusters.update(extract_list_elements(output_node, file_cluster_ids))

        # If replacement had no endpoints, try signature
        if not all_input_clusters and repl_node is not None:
            endpoints_node = find_dict_key(sig_node, "ENDPOINTS")
            if endpoints_node is None:
                endpoints_node = find_dict_key(sig_node, "endpoints")
            if endpoints_node and isinstance(endpoints_node, ast.Dict):
                ep_dict = extract_int_key_dict(endpoints_node)
                for ep_id, ep_node in ep_dict.items():
                    if isinstance(ep_node, ast.Dict):
                        input_node = find_dict_key(ep_node, "INPUT_CLUSTERS")
                        if input_node is None:
                            input_node = find_dict_key(ep_node, "input_clusters")
                        if input_node:
                            all_input_clusters.update(extract_list_elements(input_node, file_cluster_ids))

        if not all_input_clusters:
            continue

        # Build fz/tz/expose from cluster IDs
        for manufacturer, model in models:
            dev = build_device_from_clusters(
                model=model,
                manufacturer=manufacturer,
                vendor=vendor_name,
                description=desc,
                input_clusters=all_input_clusters,
                output_clusters=all_output_clusters,
            )
            if dev:
                devices.append(dev)

    return devices


# ============================================================================
# V2 Quirk Extraction (QuirkBuilder / TuyaQuirkBuilder)
# ============================================================================

def _extract_cluster_id_from_kwarg(call, kwarg_name, file_cluster_ids):
    """Extract a cluster_id from a keyword argument in a call node."""
    for kw in call.keywords:
        if kw.arg == kwarg_name:
            cid = resolve_cluster_id(kw.value, file_cluster_ids)
            if cid is not None:
                return cid
            # Try Name resolution for common cluster classes
            if isinstance(kw.value, ast.Attribute) and kw.value.attr == "cluster_id":
                if isinstance(kw.value.value, ast.Name):
                    cls_name = kw.value.value.id
                    cid = ZCL_CLUSTER_IDS.get(cls_name) or CLUSTER_CLASS_ALIASES.get(cls_name)
                    if cid is not None:
                        return cid
                    cid = file_cluster_ids.get(cls_name)
                    if cid is not None:
                        return cid
    return None


def _extract_device_type_name(call):
    """Extract device_type name from keyword args like device_type=zha.DeviceType.DIMMABLE_LIGHT."""
    for kw in call.keywords:
        if kw.arg == "device_type":
            if isinstance(kw.value, ast.Attribute):
                return kw.value.attr
    return None


# ============================================================================
# Tuya DP extraction from TuyaQuirkBuilder method chains
# ============================================================================

# Maps tuya_xxx method suffix to default DP type
TUYA_METHOD_DP_TYPE = {
    "onoff": "bool", "on_off": "bool", "switch": "bool",
    "temperature": "int", "humidity": "int", "illuminance": "int",
    "battery": "int", "battery_pct": "int", "soil_moisture": "int",
    "metering": "int", "energy": "int", "power": "int",
    "co2": "int", "pm25": "int", "voc": "int", "formaldehyde": "int",
    "number": "int", "sensor": "int",
    "dimmer": "int", "brightness": "int",
    "contact": "bool", "door": "bool",
    "occupancy": "bool", "motion": "bool",
    "smoke": "bool", "gas": "bool",
    "vibration": "bool",
    "enum": "enum",
    "cover": "int",
    "thermostat": "int", "trv": "int",
}

# Maps tuya_xxx method suffix to default semantic key
TUYA_METHOD_DEFAULT_KEY = {
    "onoff": "state", "on_off": "state", "switch": "state",
    "temperature": "temperature", "humidity": "humidity",
    "illuminance": "illuminance", "battery": "battery",
    "battery_pct": "battery", "soil_moisture": "soil_moisture",
    "metering": "energy", "energy": "energy", "power": "power",
    "co2": "co2", "pm25": "pm25", "voc": "voc",
    "formaldehyde": "formaldehyde",
    "contact": "contact", "door": "contact",
    "occupancy": "occupancy", "motion": "occupancy",
    "smoke": "smoke", "gas": "gas",
    "vibration": "vibration",
    "cover": "position",
    "dimmer": "brightness", "brightness": "brightness",
}


def _extract_tuya_dp_from_call(call, method_suffix):
    """Extract DP mapping from a single tuya_xxx() call.

    Returns (dp_id_str, entry_dict) or (None, None).
    """
    dp_id = None
    key = None
    scale = None

    # Extract keyword arguments
    for kw in call.keywords:
        if kw.arg == "dp" or kw.arg == "dp_id":
            if isinstance(kw.value, ast.Constant) and isinstance(kw.value.value, int):
                dp_id = kw.value.value
        elif kw.arg == "attribute_name":
            if isinstance(kw.value, ast.Constant) and isinstance(kw.value.value, str):
                key = kw.value.value
        elif kw.arg == "name":
            if isinstance(kw.value, ast.Constant) and isinstance(kw.value.value, str):
                if key is None:
                    key = kw.value.value
        elif kw.arg == "scale":
            if isinstance(kw.value, ast.Constant) and isinstance(kw.value.value, (int, float)):
                scale = kw.value.value

    # Also check positional args: tuya_switch(dp=1) or tuya_switch(1)
    if dp_id is None and call.args:
        for arg in call.args:
            if isinstance(arg, ast.Constant) and isinstance(arg.value, int):
                dp_id = arg.value
                break

    if dp_id is None or dp_id <= 0 or dp_id > 255:
        return None, None

    # Determine key and type from method suffix
    if key is None:
        key = TUYA_METHOD_DEFAULT_KEY.get(method_suffix, method_suffix)
    dp_type = TUYA_METHOD_DP_TYPE.get(method_suffix, "int")

    entry = {"k": key, "t": dp_type}
    if scale is not None and scale != 0 and scale != 1:
        # zhaquirks scale is typically a divisor (e.g., 10 means value/10)
        entry["scale"] = round(1.0 / scale, 6) if scale > 0 else 0

    return str(dp_id), entry


def _extract_chain_data(chain, file_constants, file_cluster_ids):
    """Extract cluster and method data from a QuirkBuilder method chain.

    Returns a dict with the extracted cluster sets, flags, and models.
    This is shared between direct QuirkBuilder chains and .clone() chains.
    """
    data = {
        "models": [],
        "replaced_clusters": set(),
        "removed_clusters": set(),
        "device_type_clusters": set(),
        "entity_clusters": set(),
        "tuya_clusters": set(),
        "tuya_dp": {},
        "is_registered": False,
        "has_skip_config": False,
        "has_entity_methods": False,
        "has_removes": False,
        "is_tuya_builder": False,
    }

    for call in chain:
        method_name = _get_method_name(call)
        if not method_name:
            continue

        if method_name in ("applies_to", "also_applies_to") and len(call.args) >= 2:
            mfr = _eval_string_arg(call.args[0], file_constants)
            model = _eval_string_arg(call.args[1], file_constants)
            if mfr and model:
                data["models"].append((mfr, model))

        elif method_name == "replaces" and len(call.args) >= 1:
            cls_name = _get_name(call.args[0])
            if cls_name:
                cid = (file_cluster_ids.get(cls_name)
                       or ZCL_CLUSTER_IDS.get(cls_name)
                       or CLUSTER_CLASS_ALIASES.get(cls_name))
                if cid is not None:
                    data["replaced_clusters"].add(cid)

        elif method_name == "adds" and len(call.args) >= 1:
            cls_name = _get_name(call.args[0])
            if cls_name:
                cid = (file_cluster_ids.get(cls_name)
                       or ZCL_CLUSTER_IDS.get(cls_name)
                       or CLUSTER_CLASS_ALIASES.get(cls_name))
                if cid is not None:
                    data["replaced_clusters"].add(cid)

        elif method_name == "removes":
            data["has_removes"] = True
            cid = None
            if call.args:
                cid = resolve_cluster_id(call.args[0], file_cluster_ids)
            if cid is None:
                cid = _extract_cluster_id_from_kwarg(call, "cluster_id", file_cluster_ids)
            if cid is not None:
                data["removed_clusters"].add(cid)

        elif method_name == "replace_cluster_occurrences" and len(call.args) >= 1:
            cls_name = _get_name(call.args[0])
            if cls_name:
                cid = (file_cluster_ids.get(cls_name)
                       or ZCL_CLUSTER_IDS.get(cls_name)
                       or CLUSTER_CLASS_ALIASES.get(cls_name))
                if cid is not None:
                    data["replaced_clusters"].add(cid)

        elif method_name == "replaces_endpoint":
            dt_name = _extract_device_type_name(call)
            if dt_name and dt_name in DEVICE_TYPE_CLUSTERS:
                data["device_type_clusters"].update(DEVICE_TYPE_CLUSTERS[dt_name])

        elif method_name == "adds_endpoint":
            dt_name = _extract_device_type_name(call)
            if dt_name and dt_name in DEVICE_TYPE_CLUSTERS:
                data["device_type_clusters"].update(DEVICE_TYPE_CLUSTERS[dt_name])

        elif method_name == "sensor":
            data["has_entity_methods"] = True
            cid = _extract_cluster_id_from_kwarg(call, "cluster_id", file_cluster_ids)
            if cid is not None:
                data["entity_clusters"].add(cid)

        elif method_name == "binary_sensor":
            data["has_entity_methods"] = True
            cid = _extract_cluster_id_from_kwarg(call, "cluster_id", file_cluster_ids)
            if cid is not None:
                data["entity_clusters"].add(cid)

        elif method_name == "switch":
            data["has_entity_methods"] = True
            cid = _extract_cluster_id_from_kwarg(call, "cluster_id", file_cluster_ids)
            if cid is not None:
                data["entity_clusters"].add(cid)
            else:
                data["entity_clusters"].add(6)  # default: OnOff

        elif method_name == "number":
            data["has_entity_methods"] = True
            cid = _extract_cluster_id_from_kwarg(call, "cluster_id", file_cluster_ids)
            if cid is not None:
                data["entity_clusters"].add(cid)

        elif method_name == "enum":
            data["has_entity_methods"] = True
            cid = _extract_cluster_id_from_kwarg(call, "cluster_id", file_cluster_ids)
            if cid is not None:
                data["entity_clusters"].add(cid)

        elif method_name in ("write_attr_button", "command_button"):
            data["has_entity_methods"] = True
            cid = _extract_cluster_id_from_kwarg(call, "cluster_id", file_cluster_ids)
            if cid is not None:
                data["entity_clusters"].add(cid)

        elif method_name == "change_entity_metadata":
            cid = _extract_cluster_id_from_kwarg(call, "cluster_id", file_cluster_ids)
            if cid is not None and cid not in UTILITY_CLUSTERS:
                data["entity_clusters"].add(cid)

        elif method_name == "prevent_default_entity_creation":
            cid = _extract_cluster_id_from_kwarg(call, "cluster_id", file_cluster_ids)
            if cid is not None and cid not in UTILITY_CLUSTERS:
                data["entity_clusters"].add(cid)

        elif method_name == "skip_configuration":
            data["has_skip_config"] = True

        elif method_name == "add_to_registry":
            data["is_registered"] = True

        elif method_name.startswith("tuya_"):
            tuya_name = method_name[5:]
            data["tuya_clusters"].add(0xEF00)

            # Extract DP mapping from call kwargs
            dp_id_str, dp_entry = _extract_tuya_dp_from_call(call, tuya_name)
            if dp_id_str and dp_entry:
                data["tuya_dp"][dp_id_str] = dp_entry

            if tuya_name in ("temperature", "soil_moisture"):
                data["replaced_clusters"].add(1026)
            elif tuya_name == "humidity":
                data["replaced_clusters"].add(1029)
            elif tuya_name in ("illuminance",):
                data["replaced_clusters"].add(1024)
            elif tuya_name in ("battery", "battery_pct"):
                data["replaced_clusters"].add(1)
            elif tuya_name in ("contact", "door"):
                data["replaced_clusters"].add(1280)
            elif tuya_name in ("occupancy", "motion"):
                data["replaced_clusters"].add(1030)
            elif tuya_name in ("smoke", "gas", "ias"):
                data["replaced_clusters"].add(1280)
            elif tuya_name in ("switch", "on_off", "onoff"):
                data["replaced_clusters"].add(6)
            elif tuya_name in ("dimmer", "brightness"):
                data["replaced_clusters"].add(8)
            elif tuya_name in ("cover",):
                data["replaced_clusters"].add(258)
            elif tuya_name in ("thermostat", "trv"):
                data["replaced_clusters"].add(513)
            elif tuya_name in ("vibration",):
                pass
            elif tuya_name in ("co2",):
                data["replaced_clusters"].add(1037)
            elif tuya_name in ("pm25",):
                data["replaced_clusters"].add(1066)
            elif tuya_name in ("voc", "formaldehyde"):
                data["replaced_clusters"].add(1070)
            elif tuya_name in ("metering", "energy", "power"):
                data["replaced_clusters"].add(1794)
            elif tuya_name in ("electrical_conductivity",):
                data["replaced_clusters"].add(2820)

    return data


def _merge_chain_data(base, clone):
    """Merge base quirk data with clone chain data.

    The clone inherits all cluster data from the base, plus its own additions.
    Models come only from the clone chain (base quirk has no models typically).
    """
    merged_dp = dict(base.get("tuya_dp", {}))
    merged_dp.update(clone.get("tuya_dp", {}))
    merged = {
        "models": list(clone["models"]),  # models from clone only
        "replaced_clusters": set(base["replaced_clusters"]) | clone["replaced_clusters"],
        "removed_clusters": set(base["removed_clusters"]) | clone["removed_clusters"],
        "device_type_clusters": set(base["device_type_clusters"]) | clone["device_type_clusters"],
        "entity_clusters": set(base["entity_clusters"]) | clone["entity_clusters"],
        "tuya_clusters": set(base["tuya_clusters"]) | clone["tuya_clusters"],
        "tuya_dp": merged_dp,
        "is_registered": clone["is_registered"],
        "has_skip_config": base["has_skip_config"] or clone["has_skip_config"],
        "has_entity_methods": base["has_entity_methods"] or clone["has_entity_methods"],
        "has_removes": base["has_removes"] or clone["has_removes"],
        "is_tuya_builder": base["is_tuya_builder"] or clone["is_tuya_builder"],
    }
    return merged


def _chain_data_to_devices(data, vendor_name):
    """Convert extracted chain data into device definitions.

    Returns a list of device dicts ready for output.
    """
    devices = []
    models = data["models"]
    is_tuya_builder = data["is_tuya_builder"]
    has_skip_config = data["has_skip_config"]
    has_entity_methods = data["has_entity_methods"]
    has_removes = data["has_removes"]

    # Combine all cluster info
    input_clusters = (data["replaced_clusters"] | data["tuya_clusters"]
                      | data["device_type_clusters"] | data["entity_clusters"])
    if is_tuya_builder and 0xEF00 not in input_clusters:
        input_clusters.add(0xEF00)

    removed_clusters = data["removed_clusters"]

    # If we have .removes() but no positive cluster info, infer the device
    # type from what was removed
    if has_removes and not input_clusters:
        non_utility_removed = {c for c in removed_clusters if c not in UTILITY_CLUSTERS}
        if non_utility_removed:
            if 768 in non_utility_removed:
                input_clusters.add(6)
                input_clusters.add(8)
            if 8 in non_utility_removed:
                input_clusters.add(6)
            if 6 in non_utility_removed:
                pass

    functional_clusters = {c for c in input_clusters if c not in UTILITY_CLUSTERS}

    if not functional_clusters and not has_entity_methods:
        quirk_flags = 0
        if has_skip_config:
            quirk_flags |= ZB_QUIRK_SKIP_CONFIG
        for manufacturer, model in models:
            devices.append({
                "m": model,
                "mf": manufacturer,
                "v": vendor_name,
                "d": "",
                "src": "zhaquirks",
                "pri": 1,
                "fz": [],
                "tz": [],
                "e": [],
                "q": quirk_flags,
                "_coverage": "V2_TRIGGERS_ONLY",
            })
        return devices

    tuya_dp = data.get("tuya_dp", {})

    for manufacturer, model in models:
        dev = build_device_from_clusters(
            model=model,
            manufacturer=manufacturer,
            vendor=vendor_name,
            description="",
            input_clusters=input_clusters,
            output_clusters=set(),
            allow_empty=has_entity_methods,
        )
        if dev:
            if has_skip_config:
                dev["q"] = dev.get("q", 0) | ZB_QUIRK_SKIP_CONFIG
            if tuya_dp:
                dev["tuya_dp"] = dict(tuya_dp)
            dev["_coverage"] = "V2_MATCH"
            devices.append(dev)

    return devices


def extract_v2_quirks(tree, file_constants, file_cluster_ids, vendor_name, filepath):
    """Extract device definitions from V2 QuirkBuilder chains.

    Handles three patterns:
    1. Direct QuirkBuilder chains with add_to_registry():
        QuirkBuilder("Manufacturer", "Model")
            .replaces(CustomCluster)
            .add_to_registry()

    2. TuyaQuirkBuilder chains:
        TuyaQuirkBuilder("_TZE200_xxx", "TS0601")
            .tuya_temperature(dp_id=1)
            .add_to_registry()

    3. Clone patterns (base_quirk assigned, then cloned):
        base_quirk = QuirkBuilder().replaces(CustomCluster)
        base_quirk.clone().applies_to("Mfr", "Model").add_to_registry()
    """
    devices = []

    # ---------------------------------------------------------------
    # Pass 1: Collect base quirk assignments for .clone() support
    # ---------------------------------------------------------------
    # Look for: variable = QuirkBuilder()...chain... (without add_to_registry)
    # or: variable = (QuirkBuilder()...chain...)
    base_quirks = {}  # variable_name -> chain_data

    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        if len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name):
            continue

        # The value could be the chain call directly or wrapped in parens
        value = node.value
        if not isinstance(value, ast.Call):
            continue

        chain = _unwrap_call_chain(value)
        if not chain:
            continue

        root_call = chain[0]
        root_name = _get_call_name(root_call)

        if root_name not in ("QuirkBuilder", "TuyaQuirkBuilder"):
            continue

        # Check that this chain does NOT have add_to_registry
        # (that means it's a base quirk, not a standalone registration)
        has_register = False
        for call in chain[1:]:
            if _get_method_name(call) == "add_to_registry":
                has_register = True
                break

        if has_register:
            # This is a normal chain that happens to be assigned; it will be
            # picked up in pass 2 as well. Skip base quirk collection.
            continue

        # Extract chain data from the base quirk
        chain_data = _extract_chain_data(chain[1:], file_constants, file_cluster_ids)
        chain_data["is_tuya_builder"] = (root_name == "TuyaQuirkBuilder")

        # Also extract constructor models (some base quirks have mfr/model)
        mfr = None
        model = None
        if len(root_call.args) >= 2:
            mfr = _eval_string_arg(root_call.args[0], file_constants)
            model = _eval_string_arg(root_call.args[1], file_constants)
        for kw in root_call.keywords:
            if kw.arg == "manufacturer" and mfr is None:
                mfr = _eval_string_arg(kw.value, file_constants)
            elif kw.arg == "model" and model is None:
                model = _eval_string_arg(kw.value, file_constants)
        if mfr and model:
            chain_data["models"].append((mfr, model))

        base_quirks[target.id] = chain_data

    # ---------------------------------------------------------------
    # Pass 2: Process expression statements (direct chains + clones)
    # ---------------------------------------------------------------
    for node in ast.walk(tree):
        if not isinstance(node, ast.Expr):
            continue
        if not isinstance(node.value, ast.Call):
            continue

        chain = _unwrap_call_chain(node.value)
        if not chain:
            continue

        root_call = chain[0]
        root_name = _get_call_name(root_call)

        # Check for .clone() pattern: variable.clone()...
        clone_base_name = None
        if root_name == "clone":
            # The root call is something.clone() -- check if "something" is a
            # known base quirk variable
            func = root_call.func
            if isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name):
                clone_base_name = func.value.id

        if root_name in ("QuirkBuilder", "TuyaQuirkBuilder"):
            # Direct QuirkBuilder chain
            models = []
            is_tuya_builder = root_name == "TuyaQuirkBuilder"

            mfr = None
            model = None
            if len(root_call.args) >= 2:
                mfr = _eval_string_arg(root_call.args[0], file_constants)
                model = _eval_string_arg(root_call.args[1], file_constants)
            for kw in root_call.keywords:
                if kw.arg == "manufacturer" and mfr is None:
                    mfr = _eval_string_arg(kw.value, file_constants)
                elif kw.arg == "model" and model is None:
                    model = _eval_string_arg(kw.value, file_constants)
            if mfr and model:
                models.append((mfr, model))

            chain_data = _extract_chain_data(chain[1:], file_constants, file_cluster_ids)
            chain_data["is_tuya_builder"] = is_tuya_builder
            chain_data["models"] = models + chain_data["models"]

            if not chain_data["is_registered"] or not chain_data["models"]:
                continue

            devices.extend(_chain_data_to_devices(chain_data, vendor_name))

        elif clone_base_name and clone_base_name in base_quirks:
            # .clone() pattern -- merge base quirk data with clone chain data
            base_data = base_quirks[clone_base_name]

            clone_data = _extract_chain_data(chain[1:], file_constants, file_cluster_ids)
            clone_data["is_tuya_builder"] = base_data["is_tuya_builder"]

            merged = _merge_chain_data(base_data, clone_data)

            if not merged["is_registered"] or not merged["models"]:
                continue

            devices.extend(_chain_data_to_devices(merged, vendor_name))

    return devices


def _unwrap_call_chain(node):
    """Unwrap a chained method call into a list of Call nodes.

    QuirkBuilder("a","b").applies_to("c","d").add_to_registry()
    becomes: [QuirkBuilder("a","b"), .applies_to("c","d"), .add_to_registry()]
    """
    chain = []
    current = node

    while isinstance(current, ast.Call):
        chain.append(current)
        func = current.func
        if isinstance(func, ast.Attribute):
            current = func.value
        else:
            break

    chain.reverse()
    return chain


def _get_call_name(call):
    """Get the function name from a Call node."""
    if isinstance(call.func, ast.Name):
        return call.func.id
    if isinstance(call.func, ast.Attribute):
        return call.func.attr
    return None


def _get_method_name(call):
    """Get method name from a method call in a chain."""
    if isinstance(call.func, ast.Attribute):
        return call.func.attr
    return None


def _get_name(node):
    """Get a simple name from a Name node or dotted attribute."""
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
        return node.value.id
    return None


def _eval_string_arg(node, constants):
    """Evaluate a string argument, resolving Name references via constants."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.Name) and node.id in constants:
        val = constants[node.id]
        if isinstance(val, str):
            return val
    # Handle f-strings like f" {LEGRAND}"
    if isinstance(node, ast.JoinedStr):
        return extract_string(node, constants)
    return None


# ============================================================================
# Device Construction
# ============================================================================

def build_device_from_clusters(model, manufacturer, vendor, description,
                                input_clusters, output_clusters,
                                allow_empty=False):
    """Build a device definition dict from cluster ID sets."""
    fz_entries = []
    tz_entries = []
    expose_entries = []
    quirk_flags = 0
    bind_clusters = []
    quirks = {}

    # Determine expose type: if we have both OnOff and LevelControl,
    # it's a LIGHT not a SWITCH
    has_on_off = 6 in input_clusters
    has_level = 8 in input_clusters
    has_color = 768 in input_clusters
    has_tuya = 0xEF00 in input_clusters
    has_aqara_opple = 0xFCC0 in input_clusters

    # Sort clusters for deterministic output
    for cid in sorted(input_clusters):
        if cid in UTILITY_CLUSTERS:
            continue

        # Special light cluster handling
        if cid == 6 and (has_level or has_color):
            # Don't add SWITCH expose -- LIGHT will cover it
            if cid in CLUSTER_FZ_MAP:
                fz_entries.extend(CLUSTER_FZ_MAP[cid])
            if cid in CLUSTER_TZ_MAP:
                tz_entries.extend(CLUSTER_TZ_MAP[cid])
            continue

        if cid == 8 and has_color:
            # Add fz/tz but not expose -- COLOR light expose covers it
            if cid in CLUSTER_FZ_MAP:
                fz_entries.extend(CLUSTER_FZ_MAP[cid])
            if cid in CLUSTER_TZ_MAP:
                tz_entries.extend(CLUSTER_TZ_MAP[cid])
            continue

        if cid in CLUSTER_FZ_MAP:
            fz_entries.extend(CLUSTER_FZ_MAP[cid])
        if cid in CLUSTER_TZ_MAP:
            tz_entries.extend(CLUSTER_TZ_MAP[cid])
        if cid in CLUSTER_EXPOSE_MAP:
            expose_entries.extend(CLUSTER_EXPOSE_MAP[cid])

        if cid not in UTILITY_CLUSTERS and cid in CLUSTER_FZ_MAP:
            bind_clusters.append(cid)

    # Quirk flags
    if has_tuya:
        quirk_flags |= ZB_QUIRK_TUYA_DEVICE
    if has_aqara_opple:
        quirk_flags |= ZB_QUIRK_XIAOMI_INIT

    # Build quirks section
    if bind_clusters:
        quirks["bind"] = sorted(set(bind_clusters))

    # Deduplicate
    fz_entries = deduplicate_entries(fz_entries)
    tz_entries = deduplicate_entries(tz_entries)
    expose_entries = deduplicate_exposes(expose_entries)

    if not fz_entries and not expose_entries and not allow_empty:
        return None

    dev = {
        "m": model,
        "mf": manufacturer,
        "v": vendor,
        "d": description,
        "src": "zhaquirks",
        "pri": 1,
        "fz": fz_entries,
        "tz": tz_entries,
        "e": expose_entries,
        "q": quirk_flags,
        "_coverage": "FULL_MATCH",
    }

    if quirks:
        dev["quirks"] = quirks

    return dev


def deduplicate_entries(entries):
    """Remove duplicate fz/tz entries by fn+k."""
    seen = set()
    result = []
    for entry in entries:
        key = (entry.get("fn", ""), entry.get("k", ""))
        if key not in seen:
            seen.add(key)
            result.append(entry)
    return result


def deduplicate_exposes(entries):
    """Remove duplicate expose entries by type+property."""
    seen = set()
    result = []
    for entry in entries:
        key = (entry.get("t", 0), entry.get("p", ""))
        if key not in seen:
            seen.add(key)
            result.append(entry)
    return result


# ============================================================================
# File Processing
# ============================================================================

def process_file(filepath, source_dir):
    """Process a single Python file and extract all quirk definitions."""
    try:
        with open(filepath, "r", encoding="utf-8", errors="replace") as f:
            source = f.read()
    except Exception as e:
        print(f"  Warning: Cannot read {filepath}: {e}", file=sys.stderr)
        return []

    try:
        tree = ast.parse(source, filename=str(filepath))
    except SyntaxError as e:
        print(f"  Warning: Syntax error in {filepath}: {e}", file=sys.stderr)
        return []

    # Determine vendor from directory name
    rel_path = filepath.relative_to(source_dir)
    parts = rel_path.parts
    vendor_name = parts[0] if len(parts) > 1 else rel_path.stem
    # Title-case the vendor name for display
    vendor_name = vendor_name.replace("_", " ").title()

    # Extract file-level constants
    file_constants, file_cluster_ids = extract_file_constants(tree)
    import_constants, import_clusters = extract_imported_constants(tree, source_dir)
    file_constants.update(import_constants)
    file_cluster_ids.update(import_clusters)

    # Load __init__.py from parent directories BEFORE extracting file-level
    # cluster classes, so that imported base classes are available for
    # inheritance resolution. E.g., IasZoneContactSwitchCluster(SmartThingsIasZone)
    # needs SmartThingsIasZone from __init__.py to resolve its cluster_id.
    init_dirs = []
    current = filepath.parent
    while current != source_dir and current != current.parent:
        init_dirs.append(current)
        current = current.parent

    for init_dir in reversed(init_dirs):  # Process parent first, then child
        vendor_init = init_dir / "__init__.py"
        if vendor_init.exists() and vendor_init != filepath:
            try:
                with open(vendor_init, "r", encoding="utf-8", errors="replace") as f:
                    init_source = f.read()
                init_tree = ast.parse(init_source, filename=str(vendor_init))
                init_constants, init_clusters = extract_file_constants(init_tree)
                # Also extract imported constants from __init__.py
                init_import_constants, init_import_clusters = extract_imported_constants(
                    init_tree, source_dir
                )
                init_constants.update(init_import_constants)
                init_clusters.update(init_import_clusters)
                # Don't overwrite file-level values
                for k, v in init_constants.items():
                    if k not in file_constants:
                        file_constants[k] = v
                for k, v in init_clusters.items():
                    if k not in file_cluster_ids:
                        file_cluster_ids[k] = v
                # Extract cluster classes from __init__.py
                extract_file_cluster_classes(init_tree, file_cluster_ids)
            except Exception:
                pass

    # Extract custom cluster classes defined in this file (AFTER loading
    # parent __init__.py so base classes are available)
    extract_file_cluster_classes(tree, file_cluster_ids)

    # Extract V1 quirks (CustomDevice classes)
    v1_devices = extract_v1_quirks(tree, file_constants, file_cluster_ids, vendor_name, filepath)

    # Extract V2 quirks (QuirkBuilder chains)
    v2_devices = extract_v2_quirks(tree, file_constants, file_cluster_ids, vendor_name, filepath)

    return v1_devices + v2_devices


# ============================================================================
# Output Formatting
# ============================================================================

def clean_expose(e):
    """Remove None/empty values from expose dict for compact JSON."""
    return {k: v for k, v in e.items() if v is not None and v != ""}


def clean_device(dev):
    """Remove internal fields and prepare device for JSON output."""
    compat = check_device_compat(dev)
    out = {
        "m": dev["m"],
        "mf": dev.get("mf", ""),
        "v": dev.get("v", ""),
        "d": dev.get("d", ""),
        "src": "zhaquirks",
        "pri": 1,
        "fz": dev["fz"],
        "tz": dev["tz"],
        "e": [clean_expose(e) for e in dev.get("e", [])],
        "q": dev.get("q", 0),
        "_compat": compat,
    }

    if dev.get("quirks"):
        out["quirks"] = dev["quirks"]
    if dev.get("tuya_dp"):
        out["tuya_dp"] = dev["tuya_dp"]

    # Remove empty arrays and zero values for compactness
    if not out["tz"]:
        del out["tz"]
    if not out["e"]:
        del out["e"]
    if out["q"] == 0:
        del out["q"]

    return out


def group_by_manufacturer(devices):
    """Group devices by manufacturer string."""
    groups = defaultdict(list)
    for dev in devices:
        manuf = dev.get("mf", "unknown")
        groups[manuf].append(dev)
    return groups


def manufacturer_to_filename(manufacturer):
    """Convert manufacturer name to safe filename."""
    name = manufacturer.lower()
    name = re.sub(r"[^a-z0-9_]", "_", name)
    name = re.sub(r"_+", "_", name).strip("_")
    if not name:
        name = "unknown"
    return name + ".json"


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="ZHA Quirks Transpiler - extracts device definitions from zhaquirks Python sources"
    )
    parser.add_argument(
        "--source", required=True,
        help="Path to zhaquirks/zhaquirks/ directory"
    )
    parser.add_argument(
        "--output",
        help="Output directory for JSON files"
    )
    parser.add_argument(
        "--report", action="store_true",
        help="Print detailed coverage report"
    )
    parser.add_argument(
        "--min-coverage",
        choices=["FULL_MATCH", "PARTIAL", "V2_MATCH", "V2_TRIGGERS_ONLY", "NO_MATCH"],
        default="V2_MATCH",
        help="Minimum coverage to include in output (default: V2_MATCH)"
    )
    args = parser.parse_args()

    source_dir = Path(args.source)
    if not source_dir.is_dir():
        print(f"Error: {source_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    # Collect all Python files in vendor subdirectories
    # We now include __init__.py files that contain device definitions
    py_files = []
    init_files_with_devices = set()

    # First pass: identify __init__.py files that contain device definitions
    for vendor_dir in sorted(source_dir.iterdir()):
        if not vendor_dir.is_dir():
            continue
        if vendor_dir.name == "__pycache__":
            continue
        for init_file in vendor_dir.rglob("__init__.py"):
            if "__pycache__" in str(init_file):
                continue
            try:
                with open(init_file, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
                # Check if __init__.py has device definitions (not just cluster defs)
                if ("add_to_registry" in content
                    or ("MODELS_INFO" in content and "signature" in content)
                    or ("CustomDevice" in content and "replacement" in content)):
                    init_files_with_devices.add(init_file)
            except Exception:
                pass

    # Second pass: collect all files
    for vendor_dir in sorted(source_dir.iterdir()):
        if not vendor_dir.is_dir():
            continue
        if vendor_dir.name == "__pycache__":
            continue
        if vendor_dir.name.startswith("_") or vendor_dir.name.startswith("."):
            if vendor_dir.name not in ("__pycache__",):
                # skip hidden dirs but not underscore vendor dirs
                if vendor_dir.name.startswith("."):
                    continue
        for py_file in sorted(vendor_dir.rglob("*.py")):
            if py_file.name == "__init__.py":
                # Only include __init__.py if it has device definitions
                if py_file in init_files_with_devices:
                    py_files.append(py_file)
                continue
            if "__pycache__" in str(py_file):
                continue
            py_files.append(py_file)

    if not py_files:
        print(f"Error: No Python files found in {source_dir}/*/", file=sys.stderr)
        sys.exit(1)

    print(f"Scanning {len(py_files)} Python files across zhaquirks vendor directories...")

    all_devices = []
    stats = defaultdict(int)
    files_with_devices = 0

    for py_file in py_files:
        devices = process_file(py_file, source_dir)
        for dev in devices:
            coverage = dev.get("_coverage", "NO_MATCH")
            stats[coverage] += 1
        if devices:
            files_with_devices += 1
            rel = py_file.relative_to(source_dir)
            print(f"  {rel}: {len(devices)} definitions")
        all_devices.extend(devices)

    total = len(all_devices)
    print(f"\nTotal: {total} device definitions extracted from {files_with_devices} files")
    for cov_type in sorted(stats.keys()):
        count = stats[cov_type]
        pct = count * 100 // max(total, 1)
        print(f"  {cov_type:20s}: {count:4d} ({pct}%)")

    # ESP32 compatibility check
    compat_count = sum(1 for d in all_devices if check_device_compat(d))
    print(f"\nESP32 compatible: {compat_count}/{total} ({compat_count * 100 // max(total, 1)}%)")

    if args.report:
        print("\n--- Detailed Coverage Report ---")
        for dev in sorted(all_devices, key=lambda d: (d.get("_coverage", ""), d.get("mf", ""), d.get("m", ""))):
            cov = dev.get("_coverage", "NO_MATCH")
            compat = "OK" if check_device_compat(dev) else "!!"
            print(
                f"  [{cov:20s}] {compat} {dev['mf']:30s} {dev['m']:30s} "
                f"({len(dev.get('fz', []))} fz, {len(dev.get('tz', []))} tz, "
                f"{len(dev.get('e', []))} exp)"
            )
        return

    if not args.output:
        print("\nNo --output specified, dry run complete.")
        return

    # Filter by minimum coverage
    coverage_levels = {
        "FULL_MATCH": 5,
        "V2_MATCH": 4,
        "PARTIAL": 3,
        "V2_TRIGGERS_ONLY": 2,
        "NO_MATCH": 1,
    }
    min_level = coverage_levels.get(args.min_coverage, 5)
    filtered = [
        d for d in all_devices
        if coverage_levels.get(d.get("_coverage", "NO_MATCH"), 1) >= min_level
    ]

    # Remove devices with no converters (triggers-only quirks)
    filtered = [d for d in filtered if d.get("fz") or d.get("e")]

    # Filter by ESP32 compatibility when at FULL_MATCH or V2_MATCH level
    if args.min_coverage in ("FULL_MATCH", "V2_MATCH"):
        before = len(filtered)
        filtered = [d for d in filtered if check_device_compat(d)]
        removed = before - len(filtered)
        if removed > 0:
            print(f"  Removed {removed} devices with unsupported fz/tz functions")

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
        with open(out_path, "w") as f:
            json.dump({"devices": cleaned}, f, separators=(",", ":"), ensure_ascii=False)

        manuf_info[manuf] = {"file": filename, "count": len(cleaned)}
        print(f"  {filename}: {len(cleaned)} devices")

    # Generate/update index.json
    index = {
        "version": 2,
        "sources": {
            "zhaquirks": {"ts": date.today().isoformat(), "commit": "unknown"},
        },
        "manufacturers": manuf_info,
        "total_devices": total_devices,
    }

    index_path = out_dir / "index.json"
    with open(index_path, "w") as f:
        json.dump(index, f, separators=(",", ":"), ensure_ascii=False)

    print(f"\nGenerated {len(manuf_info)} manufacturer files + index.json")
    print(f"Total: {total_devices} devices in {out_dir}")

    # Size report
    total_bytes = sum(f.stat().st_size for f in out_dir.iterdir() if f.is_file())
    print(f"Total size: {total_bytes:,} bytes ({total_bytes / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
