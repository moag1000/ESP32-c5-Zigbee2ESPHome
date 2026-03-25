#!/usr/bin/env python3
"""
validate_converter_db.py — Schema + fn-Name Validation for Converter DB

Validates converter DB JSON files against the expected schema and checks
that all referenced function names exist in the firmware's fn_registry.

Usage:
    python tools/validate_converter_db.py data/converters_merged/
    python tools/validate_converter_db.py my_device.json
    python tools/validate_converter_db.py data/converters_merged/ --stats

Exit codes:
    0 = all valid
    1 = validation errors found
"""

import json
import os
import re
import sys
import argparse
from pathlib import Path
from difflib import get_close_matches

# Known function names — extracted from zb_converter_fn_registry.c
KNOWN_FZ = {
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
    "fz_power_factor", "fz_power_on_behavior", "fz_pressure", "fz_smoke_alarm",
    "fz_soil_moisture", "fz_temperature", "fz_thermostat_local_temp",
    "fz_thermostat_running_state", "fz_thermostat_setpoint",
    "fz_thermostat_system_mode", "fz_thermostat_weekly_schedule",
    "fz_tuya_dp", "fz_uint16", "fz_vibration_action", "fz_vibration_angle",
    "fz_vibration_strength", "fz_voc", "fz_voltage", "fz_water_leak",
    "fz_xiaomi_ff01", "fz_xiaomi_voltage",
}

KNOWN_TZ = {
    "tz_aqara_opple", "tz_brightness", "tz_color_hs", "tz_color_temp",
    "tz_color_xy", "tz_cover_command", "tz_cover_position", "tz_cover_tilt",
    "tz_door_lock_pin", "tz_effect", "tz_fan_mode", "tz_generic_write_attr",
    "tz_ias_zone_enroll_response", "tz_identify", "tz_identify_effect",
    "tz_lock_command", "tz_on_off", "tz_power_on_behavior",
    "tz_thermostat_setpoint", "tz_thermostat_system_mode",
    "tz_thermostat_weekly_schedule", "tz_tuya_command", "tz_tuya_dp",
    "tz_warning", "tz_xiaomi_sensitivity",
}

ALL_FN = KNOWN_FZ | KNOWN_TZ

# Valid expose types
VALID_EXPOSE_TYPES = {
    "light", "switch", "sensor", "binary_sensor", "cover", "lock",
    "climate", "fan", "select", "number", "button", "text",
}

# Valid DP types for tuya_dp
VALID_DP_TYPES = {"bool", "int", "str", "enum", "bitmap", "raw"}

# Valid quirk action types
VALID_QUIRK_ACTIONS = {"init", "bind", "report", "custom_bind", "custom_report"}


class ValidationError:
    def __init__(self, file, device_idx, field, message, suggestion=None):
        self.file = file
        self.device_idx = device_idx
        self.field = field
        self.message = message
        self.suggestion = suggestion

    def __str__(self):
        loc = f"{self.file}"
        if self.device_idx is not None:
            loc += f" device[{self.device_idx}]"
        s = f"ERROR: {loc}.{self.field} — {self.message}"
        if self.suggestion:
            s += f"\n  {self.suggestion}"
        return s


class ValidationWarning:
    def __init__(self, file, device_idx, field, message):
        self.file = file
        self.device_idx = device_idx
        self.field = field
        self.message = message

    def __str__(self):
        loc = f"{self.file}"
        if self.device_idx is not None:
            loc += f" device[{self.device_idx}]"
        return f"WARN: {loc}.{self.field} — {self.message}"


def suggest_fn(name, known_set):
    """Find close matches for a function name."""
    matches = get_close_matches(name, known_set, n=3, cutoff=0.6)
    if matches:
        return f"Did you mean: {', '.join(matches)}?"
    return None


def validate_fz_entry(entry, file, dev_idx, fz_idx, errors, warnings):
    """Validate a single from_zigbee entry."""
    prefix = f"fz[{fz_idx}]"

    # fn (function name) — required
    fn = entry.get("fn")
    if not fn or not isinstance(fn, str):
        errors.append(ValidationError(file, dev_idx, f"{prefix}.fn", "Missing or invalid function name"))
        return

    if fn not in KNOWN_FZ:
        suggestion = suggest_fn(fn, KNOWN_FZ)
        errors.append(ValidationError(file, dev_idx, f"{prefix}.fn",
                                       f'Unknown function "{fn}"',
                                       suggestion or f"Available fz_* functions: {', '.join(sorted(KNOWN_FZ)[:10])}..."))

    # k (json_key) — required
    key = entry.get("k")
    if not key or not isinstance(key, str):
        errors.append(ValidationError(file, dev_idx, f"{prefix}.k", "Missing json_key"))

    # c (cluster_id) — required, must be int
    cluster = entry.get("c")
    if cluster is None:
        errors.append(ValidationError(file, dev_idx, f"{prefix}.c", "Missing cluster_id"))
    elif not isinstance(cluster, int):
        errors.append(ValidationError(file, dev_idx, f"{prefix}.c", f"cluster_id must be integer, got {type(cluster).__name__}"))

    # a (attr_id) — required, must be int
    attr = entry.get("a")
    if attr is None:
        errors.append(ValidationError(file, dev_idx, f"{prefix}.a", "Missing attr_id"))
    elif not isinstance(attr, int):
        errors.append(ValidationError(file, dev_idx, f"{prefix}.a", f"attr_id must be integer, got {type(attr).__name__}"))


def validate_tz_entry(entry, file, dev_idx, tz_idx, errors, warnings):
    """Validate a single to_zigbee entry."""
    prefix = f"tz[{tz_idx}]"

    fn = entry.get("fn")
    if not fn or not isinstance(fn, str):
        errors.append(ValidationError(file, dev_idx, f"{prefix}.fn", "Missing or invalid function name"))
        return

    if fn not in KNOWN_TZ:
        suggestion = suggest_fn(fn, KNOWN_TZ)
        errors.append(ValidationError(file, dev_idx, f"{prefix}.fn",
                                       f'Unknown function "{fn}"',
                                       suggestion or f"Available tz_* functions: {', '.join(sorted(KNOWN_TZ)[:10])}..."))

    key = entry.get("k")
    if not key or not isinstance(key, str):
        errors.append(ValidationError(file, dev_idx, f"{prefix}.k", "Missing json_key"))

    cluster = entry.get("c")
    if cluster is None:
        errors.append(ValidationError(file, dev_idx, f"{prefix}.c", "Missing cluster_id"))


def validate_expose(expose, file, dev_idx, exp_idx, errors, warnings):
    """Validate a single expose entry."""
    prefix = f"e[{exp_idx}]"

    etype = expose.get("type")
    if not etype or etype not in VALID_EXPOSE_TYPES:
        # "None" is a common transpiler artifact — downgrade to warning
        if etype == "None" or etype is None:
            warnings.append(ValidationWarning(file, dev_idx, f"{prefix}.type",
                                               "Expose type is None (transpiler artifact)"))
        else:
            errors.append(ValidationError(file, dev_idx, f"{prefix}.type",
                                           f'Invalid expose type "{etype}". Valid: {", ".join(sorted(VALID_EXPOSE_TYPES))}'))

    # Check for select with values
    if etype == "select":
        values = expose.get("values")
        if not values or not isinstance(values, list):
            warnings.append(ValidationWarning(file, dev_idx, f"{prefix}.values",
                                               "Select expose without values list"))

    # Check for number with min/max
    if etype == "number":
        if "min" not in expose and "max" not in expose:
            warnings.append(ValidationWarning(file, dev_idx, prefix,
                                               "Number expose without min/max range"))


def validate_tuya_dp(tuya_dp, file, dev_idx, errors, warnings):
    """Validate tuya_dp mapping."""
    if not isinstance(tuya_dp, dict):
        errors.append(ValidationError(file, dev_idx, "tuya_dp", "Must be an object"))
        return

    for dp_id_str, dp_def in tuya_dp.items():
        prefix = f"tuya_dp.{dp_id_str}"

        # DP ID must be numeric string
        try:
            dp_id = int(dp_id_str)
            if dp_id < 1 or dp_id > 255:
                warnings.append(ValidationWarning(file, dev_idx, prefix, f"DP ID {dp_id} out of range (1-255), will be skipped"))
                continue
        except ValueError:
            errors.append(ValidationError(file, dev_idx, prefix, f'DP ID "{dp_id_str}" must be a number'))
            continue

        if not isinstance(dp_def, dict):
            errors.append(ValidationError(file, dev_idx, prefix, "DP entry must be an object"))
            continue

        # k (key) — required
        key = dp_def.get("k")
        if not key or not isinstance(key, str):
            errors.append(ValidationError(file, dev_idx, f"{prefix}.k", "Missing semantic key"))

        # t (type) — required
        dp_type = dp_def.get("t")
        if not dp_type or dp_type not in VALID_DP_TYPES:
            errors.append(ValidationError(file, dev_idx, f"{prefix}.t",
                                           f'Invalid DP type "{dp_type}". Valid: {", ".join(VALID_DP_TYPES)}'))

        # scale — optional, must be number
        scale = dp_def.get("scale")
        if scale is not None and not isinstance(scale, (int, float)):
            errors.append(ValidationError(file, dev_idx, f"{prefix}.scale", "Must be a number"))

        # v (enum values) — optional, must be object
        values = dp_def.get("v")
        if values is not None:
            if not isinstance(values, dict):
                errors.append(ValidationError(file, dev_idx, f"{prefix}.v", "Enum values must be an object"))


def validate_quirks(quirks, file, dev_idx, errors, warnings):
    """Validate quirks section."""
    if not isinstance(quirks, dict):
        errors.append(ValidationError(file, dev_idx, "quirks", "Must be an object"))
        return

    for action_type, action_list in quirks.items():
        if action_type not in VALID_QUIRK_ACTIONS:
            warnings.append(ValidationWarning(file, dev_idx, f"quirks.{action_type}",
                                               f"Unknown quirk action type"))


def validate_device(device, file, dev_idx, errors, warnings):
    """Validate a single device definition."""
    # m (model) — required
    model = device.get("m")
    if not model or not isinstance(model, str):
        errors.append(ValidationError(file, dev_idx, "m", "Missing or invalid model"))

    # mf (manufacturer) — optional but recommended
    mfr = device.get("mf")
    if not mfr:
        warnings.append(ValidationWarning(file, dev_idx, "mf", "No manufacturer specified"))

    # fz (from_zigbee) — optional array
    fz = device.get("fz")
    if fz is not None:
        if not isinstance(fz, list):
            errors.append(ValidationError(file, dev_idx, "fz", "Must be an array"))
        else:
            for i, entry in enumerate(fz):
                validate_fz_entry(entry, file, dev_idx, i, errors, warnings)

    # tz (to_zigbee) — optional array
    tz = device.get("tz")
    if tz is not None:
        if not isinstance(tz, list):
            errors.append(ValidationError(file, dev_idx, "tz", "Must be an array"))
        else:
            for i, entry in enumerate(tz):
                validate_tz_entry(entry, file, dev_idx, i, errors, warnings)

    # e (exposes) — optional array
    exposes = device.get("e")
    if exposes is not None:
        if not isinstance(exposes, list):
            errors.append(ValidationError(file, dev_idx, "e", "Must be an array"))
        else:
            for i, expose in enumerate(exposes):
                validate_expose(expose, file, dev_idx, i, errors, warnings)

    # tuya_dp — optional
    tuya_dp = device.get("tuya_dp")
    if tuya_dp is not None:
        validate_tuya_dp(tuya_dp, file, dev_idx, errors, warnings)

    # quirks — optional
    quirks = device.get("quirks")
    if quirks is not None:
        validate_quirks(quirks, file, dev_idx, errors, warnings)

    # No fz AND no tz → warning (device does nothing)
    if not fz and not tz:
        warnings.append(ValidationWarning(file, dev_idx, "fz/tz",
                                           f"Device '{model}' has no from_zigbee or to_zigbee entries"))


def validate_file(filepath, errors, warnings, stats):
    """Validate a single JSON file."""
    filename = os.path.basename(filepath)

    try:
        with open(filepath, 'r') as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        errors.append(ValidationError(filename, None, "JSON", f"Parse error: {e}"))
        return
    except Exception as e:
        errors.append(ValidationError(filename, None, "file", f"Read error: {e}"))
        return

    # index.json has special format
    if filename == "index.json":
        if not isinstance(data, dict):
            errors.append(ValidationError(filename, None, "root", "index.json must be an object"))
            return
        ver = data.get("version")
        if ver not in (1, 2):
            warnings.append(ValidationWarning(filename, None, "version", f"Unexpected version: {ver}"))
        mfrs = data.get("manufacturers")
        if not isinstance(mfrs, dict):
            errors.append(ValidationError(filename, None, "manufacturers", "Missing manufacturers object"))
        else:
            stats["index_manufacturers"] = len(mfrs)
        return

    # Regular device file
    if not isinstance(data, dict):
        errors.append(ValidationError(filename, None, "root", "Must be an object"))
        return

    devices = data.get("devices")
    if devices is None:
        # Single device format (custom quirk)
        validate_device(data, filename, None, errors, warnings)
        stats["devices"] += 1
        if data.get("tuya_dp"):
            stats["tuya_dp_devices"] += 1
        if data.get("quirks"):
            stats["quirk_devices"] += 1
        return

    if not isinstance(devices, list):
        errors.append(ValidationError(filename, None, "devices", "Must be an array"))
        return

    for i, device in enumerate(devices):
        if not isinstance(device, dict):
            errors.append(ValidationError(filename, i, "device", "Must be an object"))
            continue
        validate_device(device, filename, i, errors, warnings)
        stats["devices"] += 1
        if device.get("tuya_dp"):
            stats["tuya_dp_devices"] += 1
        if device.get("quirks"):
            stats["quirk_devices"] += 1

    stats["files"] += 1


def main():
    parser = argparse.ArgumentParser(
        description="Validate converter DB JSON files"
    )
    parser.add_argument("path", help="File or directory to validate")
    parser.add_argument("--stats", action="store_true", help="Show coverage statistics")
    parser.add_argument("--warnings", action="store_true", help="Show warnings (default: errors only)")
    parser.add_argument("--quiet", action="store_true", help="Only show summary")
    args = parser.parse_args()

    path = Path(args.path)
    errors = []
    warnings = []
    stats = {
        "files": 0,
        "devices": 0,
        "tuya_dp_devices": 0,
        "quirk_devices": 0,
        "index_manufacturers": 0,
    }

    if path.is_file():
        validate_file(str(path), errors, warnings, stats)
    elif path.is_dir():
        json_files = sorted(path.glob("*.json"))
        if not json_files:
            print(f"No JSON files found in {path}")
            sys.exit(1)
        for f in json_files:
            validate_file(str(f), errors, warnings, stats)
    else:
        print(f"Path not found: {path}")
        sys.exit(1)

    # Collect unknown function names for summary
    unknown_fz = set()
    unknown_tz = set()
    for e in errors:
        if ".fn" in e.field and "Unknown function" in e.message:
            fn_name = e.message.split('"')[1]
            if fn_name.startswith("fz_"):
                unknown_fz.add(fn_name)
            elif fn_name.startswith("tz_"):
                unknown_tz.add(fn_name)

    # Output
    if not args.quiet:
        for e in errors:
            print(e)
        if args.warnings:
            for w in warnings:
                print(w)

    # Summary
    print(f"\n{'='*60}")
    print(f"Validation Summary")
    print(f"{'='*60}")
    print(f"Files checked:      {stats['files']}")
    print(f"Devices checked:    {stats['devices']}")
    print(f"Errors:             {len(errors)}")
    print(f"Warnings:           {len(warnings)}")

    if args.stats or True:  # Always show basic stats
        print(f"\nCoverage:")
        print(f"  Devices with Tuya DP maps:  {stats['tuya_dp_devices']}")
        print(f"  Devices with quirks:        {stats['quirk_devices']}")
        if stats["index_manufacturers"] > 0:
            print(f"  Manufacturers in index:     {stats['index_manufacturers']}")

    if unknown_fz:
        print(f"\nUnknown fz_* functions ({len(unknown_fz)}):")
        for fn in sorted(unknown_fz)[:20]:
            print(f"  {fn}")
        if len(unknown_fz) > 20:
            print(f"  ... and {len(unknown_fz) - 20} more")

    if unknown_tz:
        print(f"\nUnknown tz_* functions ({len(unknown_tz)}):")
        for fn in sorted(unknown_tz)[:20]:
            print(f"  {fn}")
        if len(unknown_tz) > 20:
            print(f"  ... and {len(unknown_tz) - 20} more")

    if len(errors) > 0:
        print(f"\nResult: FAILED ({len(errors)} errors)")
        sys.exit(1)
    else:
        print(f"\nResult: PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
