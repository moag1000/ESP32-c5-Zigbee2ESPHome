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
from pathlib import Path

# ============================================================================
# ModernExtend → fz_/tz_ mapping
# ============================================================================

MODERN_EXTEND_MAP = {
    # m.onOff()
    "onOff": {
        "fz": [{"c": 6, "a": 0, "k": "state", "fn": "fz_on_off"}],
        "tz": [{"k": "state", "c": 6, "fn": "tz_on_off"}],
        "e": [{"t": 1, "f": 0}],  # ZB_EXPOSE_SWITCH
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
        "e": [{"t": 0, "f": 3}],  # ZB_EXPOSE_LIGHT, BRIGHTNESS
    },
    # m.temperature()
    "temperature": {
        "fz": [{"c": 1026, "a": 0, "k": "temperature", "fn": "fz_temperature"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "temperature", "u": "\u00b0C", "sc": "measurement"}],
    },
    # m.humidity()
    "humidity": {
        "fz": [{"c": 1029, "a": 0, "k": "humidity", "fn": "fz_humidity"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "humidity", "u": "%", "sc": "measurement"}],
    },
    # m.pressure()
    "pressure": {
        "fz": [{"c": 1027, "a": 0, "k": "pressure", "fn": "fz_pressure"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "pressure", "u": "hPa", "sc": "measurement"}],
    },
    # m.illuminance()
    "illuminance": {
        "fz": [{"c": 1024, "a": 0, "k": "illuminance", "fn": "fz_illuminance"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "illuminance", "u": "lx", "sc": "measurement"}],
    },
    # m.occupancy()
    "occupancy": {
        "fz": [{"c": 1030, "a": 0, "k": "occupancy", "fn": "fz_occupancy"}],
        "tz": [],
        "e": [{"t": 3, "f": 0, "dc": "motion"}],
    },
    # m.battery()
    "battery": {
        "fz": [{"c": 1, "a": 33, "k": "battery", "fn": "fz_battery"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "battery", "u": "%", "sc": "measurement"}],
    },
    # m.iasZoneAlarm()
    "iasZoneAlarm": {
        "fz": [{"c": 1280, "a": 2, "k": "alarm", "fn": "fz_ias_zone_status"}],
        "tz": [],
        "e": [{"t": 3, "f": 0}],
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
            {"t": 2, "f": 0, "dc": "power", "u": "W", "sc": "measurement"},
            {"t": 2, "f": 0, "dc": "current", "u": "A", "sc": "measurement"},
            {"t": 2, "f": 0, "dc": "voltage", "u": "V", "sc": "measurement"},
        ],
    },
    # m.metering()
    "metering": {
        "fz": [{"c": 1794, "a": 0, "k": "energy", "fn": "fz_metering"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "energy", "u": "kWh", "sc": "total_increasing"}],
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
        "e": [{"t": 4, "f": 96}],  # ZB_EXPOSE_COVER, POSITION|TILT
    },
    # m.lock()
    "lock": {
        "fz": [{"c": 257, "a": 0, "k": "state", "fn": "fz_lock_state"}],
        "tz": [{"k": "state", "c": 257, "fn": "tz_lock_command"}],
        "e": [{"t": 5, "f": 0}],
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
        "e": [{"t": 6, "f": 0}],
    },
    # m.fanMode()
    "fanMode": {
        "fz": [{"c": 514, "a": 0, "k": "fan_mode", "fn": "fz_fan_mode"}],
        "tz": [{"k": "fan_mode", "c": 514, "fn": "tz_fan_mode"}],
        "e": [{"t": 7, "f": 0}],
    },
    # m.co2()
    "co2": {
        "fz": [{"c": 1037, "a": 0, "k": "co2", "fn": "fz_co2"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "dc": "carbon_dioxide", "u": "ppm", "sc": "measurement"}],
    },
    # m.vocMeasurement()
    "vocMeasurement": {
        "fz": [{"c": 1070, "a": 0, "k": "voc", "fn": "fz_voc"}],
        "tz": [],
        "e": [{"t": 2, "f": 0, "u": "ppb", "sc": "measurement"}],
    },
    # m.smokeAlarm()
    "smokeAlarm": {
        "fz": [{"c": 1280, "a": 2, "k": "smoke", "fn": "fz_smoke_alarm"}],
        "tz": [],
        "e": [{"t": 3, "f": 0, "dc": "smoke"}],
    },
    # m.waterLeak()
    "waterLeak": {
        "fz": [{"c": 1280, "a": 2, "k": "water_leak", "fn": "fz_water_leak"}],
        "tz": [],
        "e": [{"t": 3, "f": 0, "dc": "moisture"}],
    },
    # m.powerOnBehavior()
    "powerOnBehavior": {
        "fz": [{"c": 6, "a": 16387, "k": "power_on_behavior", "fn": "fz_power_on_behavior"}],
        "tz": [{"k": "power_on_behavior", "c": 6, "fn": "tz_power_on_behavior"}],
        "e": [{"t": 8, "f": 0}],  # SELECT
    },
    # m.linkquality() — no converter needed, handled automatically
    "linkquality": {
        "fz": [],
        "tz": [],
        "e": [],
    },
    # m.identify() — no converter needed
    "identify": {
        "fz": [],
        "tz": [],
        "e": [],
    },
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
    "tz.thermostat_occupied_heating_setpoint": [{"k": "occupied_heating_setpoint", "c": 513, "fn": "tz_thermostat_setpoint"}],
    "tz.thermostat_system_mode": [{"k": "system_mode", "c": 513, "fn": "tz_thermostat_system_mode"}],
    "tz.cover_position_tilt": [{"k": "position", "c": 258, "fn": "tz_cover_position"}],
    "tz.lock": [{"k": "state", "c": 257, "fn": "tz_lock_command"}],
    "tz.power_on_behavior": [{"k": "power_on_behavior", "c": 6, "fn": "tz_power_on_behavior"}],
    "tz.fan_mode": [{"k": "fan_mode", "c": 514, "fn": "tz_fan_mode"}],
}


def parse_extend_call(call_str):
    """Parse a single ModernExtend call like 'm.light({colorTemp: {range: [250, 454]}})' """
    # Extract function name
    match = re.match(r'm\.(\w+)\s*\((.*)\)\s*$', call_str.strip(), re.DOTALL)
    if not match:
        return None, "NO_MATCH"

    func_name = match.group(1)
    args_str = match.group(2).strip()

    if func_name not in MODERN_EXTEND_MAP:
        return None, "UNKNOWN_EXTEND"

    result = {
        "fz": list(MODERN_EXTEND_MAP[func_name]["fz"]),
        "tz": list(MODERN_EXTEND_MAP[func_name]["tz"]),
        "e": list(MODERN_EXTEND_MAP[func_name]["e"]),
    }

    # Handle light options
    if func_name == "light" and args_str:
        if "colorTemp" in args_str or "color_temp" in args_str:
            opt = LIGHT_OPTIONS["colorTemp"]
            result["fz"].extend(opt["fz"])
            result["tz"].extend(opt["tz"])
            if result["e"]:
                result["e"][0]["f"] = result["e"][0].get("f", 0) | opt["feature_add"]
        if re.search(r'\bcolor\b\s*:', args_str) or "colorXY" in args_str or "color: true" in args_str:
            opt = LIGHT_OPTIONS["color"]
            result["fz"].extend(opt["fz"])
            result["tz"].extend(opt["tz"])
            if result["e"]:
                result["e"][0]["f"] = result["e"][0].get("f", 0) | opt["feature_add"]

    return result, "FULL_MATCH"


def extract_definitions_from_file(filepath):
    """Extract device definitions from a single TypeScript file."""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
    except Exception as e:
        print(f"  Warning: Cannot read {filepath}: {e}", file=sys.stderr)
        return []

    devices = []

    # Pattern: definition blocks — look for objects with model: '...'
    # This handles both array definitions and standalone definitions

    # Find all definition-like blocks
    # Pattern matches objects with model field
    def_pattern = re.compile(
        r'\{\s*'
        r'(?:(?:zigbeeModel|fingerprint).*?,\s*)?'  # optional zigbeeModel/fingerprint
        r'model:\s*[\'"]([^\'"]+)[\'"]'               # model (capture group 1)
        r'.*?'                                          # anything between
        r'vendor:\s*[\'"]([^\'"]+)[\'"]'               # vendor (capture group 2)
        r'.*?'                                          # anything between
        r'description:\s*[\'"]([^\'"]+)[\'"]'          # description (capture group 3)
        r'(.*?)'                                        # rest of definition (capture group 4)
        r'\}',
        re.DOTALL
    )

    # Also need manufacturer — look for zigbeeModel patterns
    manuf_pattern = re.compile(
        r'(?:zigbeeModel|fingerprint).*?manufacturerName.*?[\'"]([^\'"]+)[\'"]',
        re.DOTALL
    )

    # Global manufacturer from filename or common patterns
    file_manuf = extract_manufacturer_from_file(filepath, content)

    for match in def_pattern.finditer(content):
        model = match.group(1)
        vendor = match.group(2)
        description = match.group(3)
        rest = match.group(4)

        # Try to find manufacturer near this definition
        block_start = max(0, match.start() - 500)
        block = content[block_start:match.end()]

        manuf_match = manuf_pattern.search(block)
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
        extend_match = re.search(r'extend:\s*\[(.*?)\]', rest, re.DOTALL)
        if extend_match:
            extends_str = extend_match.group(1)
            # Split by top-level commas (handling nested parens)
            calls = split_extend_calls(extends_str)
            all_matched = True
            any_matched = False

            for call in calls:
                call = call.strip()
                if not call:
                    continue
                result, status = parse_extend_call(call)
                if result:
                    device["fz"].extend(result["fz"])
                    device["tz"].extend(result["tz"])
                    device["e"].extend(result["e"])
                    any_matched = True
                else:
                    all_matched = False

            if any_matched:
                device["_coverage"] = "FULL_MATCH" if all_matched else "PARTIAL"

        # Check for legacy fromZigbee/toZigbee
        if not device["fz"]:
            fz_match = re.search(r'fromZigbee:\s*\[(.*?)\]', rest, re.DOTALL)
            if fz_match:
                fz_refs = re.findall(r'(fz\.\w+)', fz_match.group(1))
                any_mapped = False
                all_mapped = True
                for ref in fz_refs:
                    if ref in LEGACY_FZ_MAP:
                        device["fz"].extend(LEGACY_FZ_MAP[ref])
                        any_mapped = True
                    else:
                        all_mapped = False

                if any_mapped:
                    device["_coverage"] = "FULL_MATCH" if all_mapped else "PARTIAL"

        if not device["tz"]:
            tz_match = re.search(r'toZigbee:\s*\[(.*?)\]', rest, re.DOTALL)
            if tz_match:
                tz_refs = re.findall(r'(tz\.\w+)', tz_match.group(1))
                for ref in tz_refs:
                    if ref in LEGACY_TZ_MAP:
                        device["tz"].extend(LEGACY_TZ_MAP[ref])

        # Deduplicate fz/tz entries
        device["fz"] = deduplicate_entries(device["fz"])
        device["tz"] = deduplicate_entries(device["tz"])

        if device["fz"] or device["e"]:  # Only include if we got something useful
            devices.append(device)

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
    """Split extend array contents by top-level commas, respecting parens."""
    calls = []
    depth = 0
    current = []
    for ch in s:
        if ch in '([{':
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
    out = {
        "m": dev["m"],
        "mf": dev.get("mf", ""),
        "v": dev.get("v", ""),
        "d": dev.get("d", ""),
        "fz": dev["fz"],
        "tz": dev["tz"],
        "e": [clean_expose(e) for e in dev.get("e", [])],
        "q": dev.get("q", 0),
    }
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

    print(f"\nOutputting {len(filtered)} devices (min coverage: {args.min_coverage})")

    # Group by manufacturer
    groups = group_by_manufacturer(filtered)

    # Create output directory
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Generate per-manufacturer files
    index_files = {}
    total_devices = 0

    for manuf, devices in sorted(groups.items()):
        filename = manufacturer_to_filename(manuf)
        index_files[manuf] = filename

        cleaned = [clean_device(d) for d in devices]
        total_devices += len(cleaned)

        out_path = out_dir / filename
        with open(out_path, 'w') as f:
            json.dump({"devices": cleaned}, f, separators=(',', ':'), ensure_ascii=False)

        print(f"  {filename}: {len(cleaned)} devices")

    # Generate index.json
    index = {
        "v": 1,
        "z2m": "2.0.0",
        "files": index_files,
        "count": total_devices,
    }

    index_path = out_dir / "index.json"
    with open(index_path, 'w') as f:
        json.dump(index, f, separators=(',', ':'), ensure_ascii=False)

    print(f"\nGenerated {len(index_files)} manufacturer files + index.json")
    print(f"Total: {total_devices} devices in {out_dir}")

    # Size report
    total_bytes = sum(f.stat().st_size for f in out_dir.iterdir() if f.is_file())
    print(f"Total size: {total_bytes:,} bytes ({total_bytes/1024:.1f} KB)")


if __name__ == "__main__":
    main()
