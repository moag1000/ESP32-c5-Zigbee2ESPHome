#!/usr/bin/env python3
"""
zha_extract.py — Extract device definitions from ZHA device handlers (zhaquirks)

Parses ZHA quirk classes and derives exposes from replacement cluster definitions.

Usage:
    pip install zha-quirks  # or: pip install zhaquirks
    python3 zha_extract.py [--output scripts/data/zha_raw.json]
"""

import argparse
import importlib
import inspect
import json
import os
import pkgutil
import sys
from datetime import datetime
from pathlib import Path

# ---------------------------------------------------------------------------
# Cluster ID → expose type mapping
# ---------------------------------------------------------------------------

# Standard ZCL cluster IDs
CLUSTER_EXPOSE_MAP = {
    # Measurement clusters → sensors
    0x0402: {"type": "sensor", "property": "temperature", "device_class": "temperature", "unit": "°C"},
    0x0405: {"type": "sensor", "property": "humidity", "device_class": "humidity", "unit": "%"},
    0x0403: {"type": "sensor", "property": "pressure", "device_class": "atmospheric_pressure", "unit": "hPa"},
    0x0400: {"type": "sensor", "property": "illuminance", "device_class": "illuminance", "unit": "lx"},
    0x0406: {"type": "binary_sensor", "property": "occupancy", "device_class": "motion"},
    0x042A: {"type": "sensor", "property": "pm25", "device_class": "pm25", "unit": "µg/m³"},
    0x040D: {"type": "sensor", "property": "co2", "device_class": "carbon_dioxide", "unit": "ppm"},
    0x042E: {"type": "sensor", "property": "voc", "unit": "ppb"},

    # On/Off → switch
    0x0006: {"type": "switch", "property": "state"},

    # Level control → light (combined with on/off)
    0x0008: {"type": "light", "property": "brightness"},

    # Color control → light features
    0x0300: {"type": "light_color", "property": "color"},

    # Window covering → cover
    0x0102: {"type": "cover", "property": "position"},

    # Door lock → lock
    0x0101: {"type": "lock", "property": "state"},

    # Thermostat → climate
    0x0201: {"type": "climate", "property": "local_temperature"},

    # Fan control → fan
    0x0202: {"type": "fan", "property": "fan_mode"},

    # IAS Zone → binary sensor (type depends on zone type)
    0x0500: {"type": "binary_sensor", "property": "alarm"},

    # Power configuration → battery sensor
    0x0001: {"type": "sensor", "property": "battery", "device_class": "battery", "unit": "%"},

    # Electrical measurement → power sensors
    0x0B04: {"type": "sensor", "property": "power", "device_class": "power", "unit": "W"},

    # Metering → energy sensor
    0x0702: {"type": "sensor", "property": "energy", "device_class": "energy", "unit": "kWh"},

    # Tuya cluster
    0xEF00: {"type": "tuya", "property": "tuya_dp"},
}


def derive_exposes_from_clusters(clusters):
    """Derive expose definitions from a set of cluster IDs."""
    exposes = []
    has_on_off = False
    has_level = False
    has_color = False

    cluster_ids = set()
    if isinstance(clusters, dict):
        cluster_ids = set(clusters.keys())
    elif isinstance(clusters, (list, set)):
        cluster_ids = set(clusters)

    for cid in sorted(cluster_ids):
        cid_int = int(cid) if not isinstance(cid, int) else cid
        if cid_int == 0x0006:
            has_on_off = True
        elif cid_int == 0x0008:
            has_level = True
        elif cid_int == 0x0300:
            has_color = True
        elif cid_int in CLUSTER_EXPOSE_MAP:
            expose = dict(CLUSTER_EXPOSE_MAP[cid_int])
            exposes.append(expose)

    # Combine on/off + level + color into proper light/switch expose
    if has_on_off:
        if has_level:
            expose = {"type": "light", "property": "state"}
            features = ["brightness"]
            if has_color:
                features.extend(["color_temp", "color_xy"])
            expose["features"] = features
            exposes.insert(0, expose)
        else:
            exposes.insert(0, {"type": "switch", "property": "state"})

    return exposes


def extract_quirk_info(quirk_class):
    """Extract device info from a ZHA quirk class."""
    info = {
        "quirk_class": f"{quirk_class.__module__}.{quirk_class.__name__}",
    }

    # Get signature (manufacturer + model) from class attributes
    signature = getattr(quirk_class, 'signature', None)
    if signature and isinstance(signature, dict):
        if 'manufacturer' in signature:
            info['manufacturer'] = signature['manufacturer']
        if 'model' in signature:
            info['model'] = signature['model']
        # Extract input/output clusters from signature endpoints
        if 'endpoints' in signature:
            sig_clusters = set()
            for ep_id, ep_data in signature['endpoints'].items():
                if isinstance(ep_data, dict):
                    for cid in ep_data.get('input_clusters', []):
                        sig_clusters.add(cid)
            info['_sig_clusters'] = sig_clusters

    # Get replacement info
    replacement = getattr(quirk_class, 'replacement', None)
    if replacement and isinstance(replacement, dict):
        if 'manufacturer' in replacement:
            info['manufacturer'] = replacement.get('manufacturer', info.get('manufacturer'))
        if 'model' in replacement:
            info['model'] = replacement.get('model', info.get('model'))

        # Extract replacement clusters
        repl_clusters = set()
        if 'endpoints' in replacement:
            for ep_id, ep_data in replacement['endpoints'].items():
                if isinstance(ep_data, dict):
                    for cid in ep_data.get('input_clusters', []):
                        if isinstance(cid, int):
                            repl_clusters.add(cid)
                        elif isinstance(cid, type):
                            # It's a cluster class, get its cluster_id
                            cid_val = getattr(cid, 'cluster_id', None)
                            if cid_val is not None:
                                repl_clusters.add(cid_val)
        info['_repl_clusters'] = repl_clusters

    return info


def process_quirk(quirk_class):
    """Process a single quirk class and return a device dict."""
    try:
        info = extract_quirk_info(quirk_class)
    except Exception as e:
        return None

    manufacturer = info.get('manufacturer')
    model = info.get('model')
    if not manufacturer or not model:
        return None

    # Derive exposes from replacement clusters (preferred) or signature clusters
    clusters = info.get('_repl_clusters', set()) or info.get('_sig_clusters', set())
    exposes = derive_exposes_from_clusters(clusters)

    device = {
        "model": model,
        "manufacturer": manufacturer,
        "quirk_class": info.get('quirk_class', ''),
        "exposes": exposes,
    }

    return device


def discover_quirks():
    """Discover all ZHA quirk classes."""
    quirks = []

    try:
        import zhaquirks
    except ImportError:
        print("Error: zhaquirks not installed. Install with: pip install zha-quirks", file=sys.stderr)
        sys.exit(1)

    # zhaquirks uses a registry
    try:
        from zhaquirks import _DEVICE_REGISTRY
        if _DEVICE_REGISTRY:
            for manufacturer, models in _DEVICE_REGISTRY.items():
                if isinstance(models, dict):
                    for model, quirk_list in models.items():
                        if isinstance(quirk_list, list):
                            quirks.extend(quirk_list)
                        else:
                            quirks.append(quirk_list)
            print(f"Found {len(quirks)} quirks in _DEVICE_REGISTRY", file=sys.stderr)
            return quirks
    except (ImportError, AttributeError):
        pass

    # Fallback: scan all submodules for CustomDevice subclasses
    try:
        from zhaquirks import CustomDevice
    except ImportError:
        from zigpy.quirks import CustomDevice

    base_path = Path(zhaquirks.__file__).parent
    print(f"Scanning {base_path} for quirk classes...", file=sys.stderr)

    for importer, modname, ispkg in pkgutil.walk_packages(
        path=[str(base_path)],
        prefix='zhaquirks.',
        onerror=lambda name: None
    ):
        try:
            mod = importlib.import_module(modname)
            for name, obj in inspect.getmembers(mod, inspect.isclass):
                if (issubclass(obj, CustomDevice) and
                    obj is not CustomDevice and
                    hasattr(obj, 'signature')):
                    quirks.append(obj)
        except Exception:
            continue

    print(f"Found {len(quirks)} quirk classes via module scan", file=sys.stderr)
    return quirks


def main():
    parser = argparse.ArgumentParser(description="ZHA Device Handler Extractor")
    parser.add_argument("--output", default=os.path.join(os.path.dirname(__file__), "data", "zha_raw.json"),
                        help="Output file path")
    args = parser.parse_args()

    print("Discovering ZHA quirks...", file=sys.stderr)
    quirk_classes = discover_quirks()

    devices = []
    seen = set()

    for quirk in quirk_classes:
        if isinstance(quirk, type):
            device = process_quirk(quirk)
        else:
            # It might be an instance, try its class
            device = process_quirk(type(quirk)) if hasattr(quirk, 'signature') else None

        if device is None:
            continue

        # Deduplicate by (manufacturer, model)
        key = (device['manufacturer'], device['model'])
        if key in seen:
            continue
        seen.add(key)
        devices.append(device)

    output = {
        "_meta": {
            "source": "zha-device-handlers (zhaquirks)",
            "extracted": datetime.now().isoformat(),
            "count": len(devices),
        },
        "devices": devices,
    }

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, 'w') as f:
        json.dump(output, f, separators=(',', ':'), ensure_ascii=False)

    size_kb = os.path.getsize(args.output) / 1024
    print(f"\nWritten {len(devices)} ZHA devices to {args.output} ({size_kb:.1f} KB)", file=sys.stderr)

    # Summary
    with_exposes = sum(1 for d in devices if d['exposes'])
    print(f"  With exposes: {with_exposes}", file=sys.stderr)
    print(f"  Without exposes: {len(devices) - with_exposes}", file=sys.stderr)


if __name__ == "__main__":
    main()
