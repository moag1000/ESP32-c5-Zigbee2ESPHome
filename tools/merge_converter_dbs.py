#!/usr/bin/env python3
"""
Merge Converter Databases

Merges z2m (pri=2) and zhaquirks (pri=1) converter databases into a unified
set of per-manufacturer JSON files with a unified index.

Usage:
    python3 merge_converter_dbs.py \
        --z2m data/converters/ \
        --zhaquirks data/converters_zhaquirks/ \
        --output data/converters_merged/
"""

import argparse
import copy
import datetime
import subprocess
import json
import os
import re
import sys
from collections import defaultdict
from datetime import date
from pathlib import Path


# ============================================================================
# Helpers
# ============================================================================


# Last gate before the database is shipped. The firmware interns m/mf/v/d and
# its pool rejects anything at or beyond STRING_INTERN_MAX_LENGTH (128) — a
# rejection returns NULL silently, and a NULL manufacturer in the index was once
# a load access fault on the next comparison. Some upstream manufacturer strings
# are NUL-padded, and JSON-escaping wrote the literal characters \u0000 into the
# files; the shipped database was cleaned of those by hand once and they came
# straight back with the next extraction.
#
# Both extractors clean their own output now. This runs anyway, because it is
# the one place every device passes through no matter which source it came from.
_INTERN_LIMIT = 127


def sanitise_string(value):
    if not isinstance(value, str):
        return value
    value = value.replace("\\u0000", "").replace("\u0000", "")
    value = "".join(ch for ch in value if ch >= " " or ch == "\t")
    value = value.strip()
    return value[:_INTERN_LIMIT].rstrip() if len(value) > _INTERN_LIMIT else value


def sanitise_device(dev):
    for key in ("m", "mf", "v", "d"):
        if key in dev:
            dev[key] = sanitise_string(dev[key])
    return dev



# Devices that take Tuya datapoint writes as sendData (0x04) rather than
# dataRequest (0x00).
#
# This is a local finding, not something upstream records: the _TZ3210 Fingerbot
# Plus was measured to answer only to 0x04 and to ignore 0x00. That was once the
# default for every Tuya device in the tree, which left a NEO siren
# acknowledging every command and doing nothing at all. The default now matches
# zigbee-herdsman-converters, and the exception is carried per device.
#
# Two of these have a compiled-in converter in
# main/zigbee/converter/converters/conv_tuya_fingerbot.c and never reach the
# database path; the rest do, and would silently stop working without this.
ZB_QUIRK_TUYA_CMD_SENDDATA = 1 << 10

TUYA_SENDDATA_MANUFACTURERS = {
    "_TZ3210_7vgttna6",
    "_TZ3210_a04acm9s",
    "_TZ3210_cm9mbpr1",
    "_TZ3210_dse8ogfy",
    "_TZ3210_j4pdtz9v",
}


def apply_local_quirks(dev):
    """Flags this project measured that upstream does not record."""
    if dev.get("mf") in TUYA_SENDDATA_MANUFACTURERS:
        dev["q"] = (dev.get("q") or 0) | ZB_QUIRK_TUYA_CMD_SENDDATA
    return dev



# Melody names for the NEO NAS-AB02B2 siren and its rebadges (MOES sells the
# same hardware as ZSS-LO-SLA-U-EN).
#
# zigbee-herdsman-converters builds this device's melody list as bare numbers —
# Array.from(Array(18).keys()).map((x) => (x + 1).toString()) — so the converter
# database gets "1" through "18" and Home Assistant shows a dropdown of digits.
# The names below are from the zigbee2mqtt device page for NAS-AB02B2, which
# documents what each number actually plays. They are transcribed from that
# page, not inferred from the hardware.
#
# https://www.zigbee2mqtt.io/devices/NAS-AB02B2.html
NEO_SIREN_MANUFACTURERS = {
    "_TZE200_t1blo2bj",
    "_TZE204_t1blo2bj",
    "_TZE204_q76rtoa9",
}

NEO_SIREN_MELODIES = [
    "Fuer Elise", "Big Ben", "Ring Ring", "Lone Ranger", "Turkish March",
    "High Pitch Siren", "Red Alert", "Cricket", "Beep Beep", "Dogs",
    "Police", "Chime", "Phone Ring", "Firetruck", "Clock Chime",
    "Alarm Clock", "Psycho", "Doorbell",
]


def apply_melody_names(dev):
    """Give the siren's melody datapoint its documented names."""
    if dev.get("mf") not in NEO_SIREN_MANUFACTURERS:
        return dev

    names = {name: i + 1 for i, name in enumerate(NEO_SIREN_MELODIES)}

    dps = dev.get("tuya_dp") or {}
    entry = dps.get("21")
    if entry and entry.get("k") == "melody":
        entry["t"] = "enum"
        entry["v"] = names

    for expose in dev.get("e") or []:
        if expose.get("p") == "melody":
            expose["sel"] = {"v": list(NEO_SIREN_MELODIES)}

    return dev



def strip_build_only_fields(dev):
    """Drop fields the firmware never reads.

    "src" records which upstream project an entry came from and "pri" orders
    entries while merging; neither is looked at by zb_converter_loader.c. On a
    database of 8321 devices they are about 137 KB of a 7036 KB partition —
    space that matters now that datapoint maps are extracted, and that buys
    nothing on the device. Both stay in the intermediate extracts, where the
    merge does use them.
    """
    dev.pop("src", None)
    dev.pop("pri", None)
    return dev



# ---------------------------------------------------------------------------
# Shared behaviour profiles
# ---------------------------------------------------------------------------
#
# 8321 devices share 1962 distinct behaviours: the same converters, exposes and
# datapoint maps repeated for every manufacturer id that sells the same
# hardware. Written out per device that is 5844 KB; written once and referenced
# it is 1732 KB, and the database had reached 96.4 % of its partition.
#
# A device keeps everything that identifies it — model, manufacturer, vendor,
# description — and points at a profile for what it does:
#
#     {"m":"TS0601","mf":"_TZE204_t1blo2bj","v":"NEO","d":"Alarm","p":417}
#
# Profiles live in profiles_N.json, N = id / PROFILES_PER_FILE, so the loader
# can find the file from the id without an extra index. They are split because
# a single file would be 1732 KB and the loader reads at most 512 KB at once.
PROFILE_KEYS = ("fz", "tz", "e", "tuya_dp", "quirks", "q")
PROFILES_PER_FILE = 256


def _db_revision():
    """Date of this build, plus the short commit if the tree is a git checkout."""
    stamp = datetime.date.today().isoformat()
    try:
        rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=5).stdout.strip()
        if rev:
            return f"{stamp}-{rev}"
    except Exception:
        pass
    return stamp


def build_profiles(devices):
    """Split behaviour out of the devices. Returns (profiles, devices)."""
    profiles = []
    by_signature = {}

    for dev in devices:
        body = {k: dev[k] for k in PROFILE_KEYS if k in dev}
        signature = json.dumps(body, sort_keys=True, separators=(",", ":"))
        pid = by_signature.get(signature)
        if pid is None:
            pid = len(profiles)
            by_signature[signature] = pid
            profiles.append(body)
        for k in PROFILE_KEYS:
            dev.pop(k, None)
        dev["p"] = pid

    return profiles, devices


def write_profiles(out_dir, profiles):
    """One file per PROFILES_PER_FILE profiles, named by the range they cover."""
    written = 0
    for start in range(0, len(profiles), PROFILES_PER_FILE):
        chunk = profiles[start:start + PROFILES_PER_FILE]
        name = f"profiles_{start // PROFILES_PER_FILE}.json"
        with open(os.path.join(out_dir, name), "w", encoding="utf-8") as fh:
            json.dump({"first": start, "profiles": chunk}, fh,
                      separators=(",", ":"), ensure_ascii=False)
        written += 1
    return written


def normalize_manufacturer(mf):
    """Normalize manufacturer name to lowercase for matching."""
    return mf.strip().lower()


def manufacturer_to_filename(manufacturer):
    """Convert manufacturer name to a safe filename."""
    name = manufacturer.lower()
    name = re.sub(r'[^a-z0-9_]', '_', name)
    name = re.sub(r'_+', '_', name).strip('_')
    if not name:
        name = "unknown"
    return name + ".json"


def device_key(dev):
    """Create a unique key for a device based on manufacturer + model."""
    mf = normalize_manufacturer(dev.get("mf", ""))
    m = dev.get("m", "").strip()
    return (mf, m)


def deep_merge_dict(base, overlay):
    """Deep-merge overlay dict into base dict. overlay wins on conflicts."""
    result = copy.deepcopy(base)
    for key, val in overlay.items():
        if key in result and isinstance(result[key], dict) and isinstance(val, dict):
            result[key] = deep_merge_dict(result[key], val)
        else:
            result[key] = copy.deepcopy(val)
    return result


def deduplicate_fz_tz(entries):
    """Remove duplicate fz/tz entries based on fn+k combination."""
    seen = set()
    result = []
    for entry in entries:
        key = (entry.get("fn", ""), entry.get("k", ""))
        if key not in seen:
            seen.add(key)
            result.append(entry)
    return result


def clean_device(dev):
    """Remove internal fields (_compat, _coverage) from a device for output."""
    out = {}
    for k, v in dev.items():
        if k.startswith("_"):
            continue
        out[k] = v
    return out


# ============================================================================
# Source loading
# ============================================================================

def load_source(source_dir):
    """Load all manufacturer JSON files from a source directory.

    Scans ALL .json files (not just index-referenced ones) to avoid missing
    devices when the index is truncated to MAX_INDEX_ENTRIES.

    Returns:
        (index_dict, {normalized_mf: [devices...]}, source_meta)
    """
    source_path = Path(source_dir)

    if not source_path.is_dir():
        return None, {}, {}

    # Load index for metadata (optional)
    index = {}
    source_meta = {}
    index_path = source_path / "index.json"
    if index_path.is_file():
        with open(index_path, 'r', encoding='utf-8') as f:
            index = json.load(f)
        sources = index.get("sources", {})
        for src_name, meta in sources.items():
            source_meta[src_name] = meta

    # Scan ALL .json files in directory (not just index-referenced)
    all_devices = defaultdict(list)  # normalized_mf -> [devices]
    loaded_files = 0
    total_devices = 0

    for filepath in sorted(source_path.glob("*.json")):
        if filepath.name == "index.json":
            continue

        try:
            with open(filepath, 'r', encoding='utf-8') as f:
                data = json.load(f)
        except (json.JSONDecodeError, OSError) as e:
            print(f"  Warning: Cannot read {filepath}: {e}", file=sys.stderr)
            continue

        devices = data.get("devices", [])
        if not devices:
            continue

        loaded_files += 1

        for dev in devices:
            if not dev.get("m"):
                continue
            norm_mf = normalize_manufacturer(dev.get("mf", ""))
            all_devices[norm_mf].append(dev)
            total_devices += 1

    print(f"  Loaded {loaded_files} files, {total_devices} devices from {source_path}")
    return index, dict(all_devices), source_meta


# ============================================================================
# Merge logic
# ============================================================================

def merge_device(z2m_dev, zhq_dev):
    """Merge a z2m device with a zhaquirks device.

    Strategy: keep z2m definition as base, merge quirks section from zhaquirks,
    append additional fz/tz entries, set pri=1 (quirks override wins).
    """
    merged = copy.deepcopy(z2m_dev)

    # Quirks override wins on priority
    merged["pri"] = 1
    merged["src"] = "merged"

    # Merge quirks section from zhaquirks
    zhq_quirks = zhq_dev.get("quirks", {})
    if zhq_quirks:
        base_quirks = merged.get("quirks", {})
        merged["quirks"] = deep_merge_dict(base_quirks, zhq_quirks)

    # Append additional fz entries from zhaquirks (deduplicate)
    zhq_fz = zhq_dev.get("fz", [])
    if zhq_fz:
        combined_fz = list(merged.get("fz", [])) + zhq_fz
        merged["fz"] = deduplicate_fz_tz(combined_fz)

    # Append additional tz entries from zhaquirks (deduplicate)
    zhq_tz = zhq_dev.get("tz", [])
    if zhq_tz:
        combined_tz = list(merged.get("tz", [])) + zhq_tz
        merged["tz"] = deduplicate_fz_tz(combined_tz)

    # Copy over any zhaquirks-specific fields not in z2m
    for key in zhq_dev:
        if key not in merged and not key.startswith("_"):
            merged[key] = copy.deepcopy(zhq_dev[key])

    return merged


def merge_sources(z2m_devices, zhq_devices):
    """Merge two device maps into one.

    Args:
        z2m_devices: {normalized_mf: [devices...]} from z2m
        zhq_devices: {normalized_mf: [devices...]} from zhaquirks

    Returns:
        merged_devices: {original_mf: [devices...]}
        stats: dict with merge statistics
    """
    stats = {"z2m_only": 0, "zhq_only": 0, "merged": 0, "total": 0}

    # Build lookup: (norm_mf, model) -> device for each source
    z2m_lookup = {}
    z2m_mf_names = {}  # normalized -> original manufacturer name
    for norm_mf, devices in z2m_devices.items():
        for dev in devices:
            key = device_key(dev)
            z2m_lookup[key] = dev
            # Keep the original (non-normalized) manufacturer name
            z2m_mf_names[norm_mf] = dev.get("mf", norm_mf)

    zhq_lookup = {}
    zhq_mf_names = {}
    for norm_mf, devices in zhq_devices.items():
        for dev in devices:
            key = device_key(dev)
            zhq_lookup[key] = dev
            zhq_mf_names[norm_mf] = dev.get("mf", norm_mf)

    # Collect all device keys
    all_keys = set(z2m_lookup.keys()) | set(zhq_lookup.keys())

    # Merge into per-manufacturer groups
    merged_by_mf = defaultdict(list)  # original_mf -> [devices]

    for key in all_keys:
        norm_mf, model = key
        in_z2m = key in z2m_lookup
        in_zhq = key in zhq_lookup

        if in_z2m and in_zhq:
            # Both sources: merge
            dev = merge_device(z2m_lookup[key], zhq_lookup[key])
            stats["merged"] += 1
        elif in_z2m:
            # z2m only
            dev = copy.deepcopy(z2m_lookup[key])
            stats["z2m_only"] += 1
        else:
            # zhaquirks only
            dev = copy.deepcopy(zhq_lookup[key])
            stats["zhq_only"] += 1

        stats["total"] += 1

        # Use the original manufacturer name (prefer z2m)
        orig_mf = z2m_mf_names.get(norm_mf) or zhq_mf_names.get(norm_mf, norm_mf)
        merged_by_mf[orig_mf].append(dev)

    # Sort devices within each manufacturer by pri ascending
    for mf in merged_by_mf:
        merged_by_mf[mf].sort(key=lambda d: (d.get("pri", 99), d.get("m", "")))

    return dict(merged_by_mf), stats


# ============================================================================
# Output
# ============================================================================

MAX_FILE_SIZE = 100 * 1024  # 100KB per file
BUNDLE_THRESHOLD = 4096     # Files smaller than this get bundled
BUNDLE_TARGET_SIZE = 64 * 1024  # Target size for catch-all bundles


def write_output(merged_devices, output_dir, z2m_meta, zhq_meta):
    """Write merged per-manufacturer files and index.json.

    Small manufacturers (< BUNDLE_THRESHOLD per-file) are consolidated into
    numbered catch-all files to reduce LittleFS inode overhead.
    """
    out_path = Path(output_dir)
    out_path.mkdir(parents=True, exist_ok=True)

    # Clean old output
    for old_file in out_path.glob("*.json"):
        old_file.unlink()

    manuf_info = {}
    total_devices = 0

    # Phase 1: measure file sizes, split into large vs small
    large_files = {}   # mf_name -> cleaned devices (write as own file)
    small_devs = []    # (mf_name, cleaned devices) to bundle

    # Clean everything first, then lift the shared behaviour out across all
    # manufacturers at once — the same profile is used by devices filed under
    # completely different names, so this cannot be done per file.
    cleaned_by_manuf = {}
    all_cleaned = []
    for mf_name, devices in sorted(merged_devices.items()):
        cleaned = [strip_build_only_fields(
                       apply_melody_names(apply_local_quirks(sanitise_device(clean_device(d)))))
                   for d in devices]
        cleaned_by_manuf[mf_name] = cleaned
        all_cleaned.extend(cleaned)

    profiles, _ = build_profiles(all_cleaned)
    profile_files = write_profiles(str(out_path), profiles)
    print(f"  {len(profiles)} shared profiles in {profile_files} files "
          f"(from {len(all_cleaned)} devices)")

    for mf_name, cleaned in cleaned_by_manuf.items():
        est_size = len(json.dumps({"devices": cleaned}, separators=(',', ':')))

        if est_size >= BUNDLE_THRESHOLD:
            large_files[mf_name] = cleaned
        else:
            small_devs.append((mf_name, cleaned))

    # Phase 2: write large files
    for mf_name, cleaned in large_files.items():
        filename = manufacturer_to_filename(mf_name)
        total_devices += len(cleaned)

        # Split oversized files
        file_json = json.dumps({"devices": cleaned}, separators=(',', ':'))
        if len(file_json) <= MAX_FILE_SIZE:
            filepath = out_path / filename
            filepath.write_text(file_json)
            manuf_info[mf_name] = {"file": filename, "count": len(cleaned)}
        else:
            base, ext = os.path.splitext(filename)
            part = 1
            current = []
            current_size = 0
            all_part_files = []  # track all split filenames
            for dev in cleaned:
                dev_size = len(json.dumps(dev, separators=(',', ':'))) + 1
                if current_size + dev_size > MAX_FILE_SIZE and current:
                    part_fn = f"{base}_{part}{ext}"
                    (out_path / part_fn).write_text(
                        json.dumps({"devices": current}, separators=(',', ':')))
                    all_part_files.append((part_fn, len(current)))
                    current = []
                    current_size = 0
                    part += 1
                current.append(dev)
                current_size += dev_size
            if current:
                part_fn = f"{base}_{part}{ext}" if part > 1 else filename
                (out_path / part_fn).write_text(
                    json.dumps({"devices": current}, separators=(',', ':')))
                all_part_files.append((part_fn, len(current)))

            # Index: first file as primary "file", all files in "files" array
            total_count = sum(c for _, c in all_part_files)
            manuf_info[mf_name] = {
                "file": all_part_files[0][0],
                "files": [fn for fn, _ in all_part_files],
                "count": total_count,
            }

    # Phase 3: bundle small files into catch-all groups
    bundle_num = 1
    bundle_devs = []
    bundle_size = 0

    for mf_name, cleaned in small_devs:
        total_devices += len(cleaned)
        chunk_size = len(json.dumps(cleaned, separators=(',', ':')))

        if bundle_size + chunk_size > BUNDLE_TARGET_SIZE and bundle_devs:
            # Write current bundle
            bundle_fn = f"_other_{bundle_num}.json"
            (out_path / bundle_fn).write_text(
                json.dumps({"devices": bundle_devs}, separators=(',', ':')))
            for d in bundle_devs:
                mf = d.get("mf", "")
                if mf and mf not in manuf_info:
                    manuf_info[mf] = {"file": bundle_fn, "count": 1}
            bundle_devs = []
            bundle_size = 0
            bundle_num += 1

        bundle_devs.extend(cleaned)
        bundle_size += chunk_size

    # Write final bundle
    if bundle_devs:
        bundle_fn = f"_other_{bundle_num}.json"
        (out_path / bundle_fn).write_text(
            json.dumps({"devices": bundle_devs}, separators=(',', ':')))
        for d in bundle_devs:
            mf = d.get("mf", "")
            if mf and mf not in manuf_info:
                manuf_info[mf] = {"file": bundle_fn, "count": 1}

    n_bundles = bundle_num if bundle_devs or bundle_num > 1 else 0
    n_large = len([f for f in out_path.glob("*.json") if f.name != "index.json" and not f.name.startswith("_other_")])
    print(f"  {n_large} manufacturer files + {n_bundles} catch-all bundles")

    # Build unified index
    today = date.today().isoformat()
    sources = {}

    # z2m source info
    if z2m_meta:
        sources["z2m"] = z2m_meta.get("z2m", {"ts": today, "commit": "unknown"})
    else:
        sources["z2m"] = {"ts": today, "commit": "unknown"}

    # zhaquirks source info
    if zhq_meta:
        sources["zhaquirks"] = zhq_meta.get("zhaquirks", {"ts": today, "commit": "unknown"})
    else:
        sources["zhaquirks"] = {"ts": today, "commit": "unknown"}

    index = {
        "version": 2,
        # A revision the gateway can compare against what it has installed.
        # "version" above is the schema, which has not changed since v2 and says
        # nothing about the contents; without this there is no way to answer
        # "is there a newer database than mine" other than downloading all of it.
        # ISO dates order lexicographically, so a plain string compare works.
        "db_revision": _db_revision(),
        "sources": sources,
        "manufacturers": manuf_info,
        "total_devices": total_devices,
    }

    index_path = out_path / "index.json"
    with open(index_path, 'w', encoding='utf-8') as f:
        json.dump(index, f, separators=(',', ':'), ensure_ascii=False)

    return len(manuf_info), total_devices


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Merge z2m and zhaquirks converter databases into a unified output."
    )
    parser.add_argument("--z2m", help="Path to z2m converter directory (with index.json)")
    parser.add_argument("--zhaquirks", help="Path to zhaquirks converter directory (with index.json)")
    parser.add_argument("--output", required=True, help="Output directory for merged files")
    args = parser.parse_args()

    z2m_path = args.z2m
    zhq_path = args.zhaquirks
    output_path = args.output

    # Validate at least one source exists
    z2m_exists = z2m_path and Path(z2m_path).is_dir()
    zhq_exists = zhq_path and Path(zhq_path).is_dir()

    if not z2m_exists and not zhq_exists:
        print("Error: At least one source directory must exist.", file=sys.stderr)
        sys.exit(1)

    # Load sources
    z2m_devices = {}
    z2m_meta = {}
    zhq_devices = {}
    zhq_meta = {}

    if z2m_exists:
        print(f"Loading z2m from {z2m_path}...")
        _, z2m_devices, z2m_meta = load_source(z2m_path)

    if zhq_exists:
        print(f"Loading zhaquirks from {zhq_path}...")
        _, zhq_devices, zhq_meta = load_source(zhq_path)

    # Handle single-source passthrough
    if not z2m_devices and not zhq_devices:
        print("Error: No devices found in any source.", file=sys.stderr)
        sys.exit(1)

    if not z2m_devices:
        print("No z2m devices, passing through zhaquirks only.")
        # Convert zhq_devices: normalize keys back to original mf names
        passthrough = defaultdict(list)
        for norm_mf, devices in zhq_devices.items():
            for dev in devices:
                orig_mf = dev.get("mf", norm_mf)
                passthrough[orig_mf].append(dev)
        merged_devices = dict(passthrough)
        stats = {"z2m_only": 0, "zhq_only": sum(len(v) for v in zhq_devices.values()),
                 "merged": 0, "total": sum(len(v) for v in zhq_devices.values())}
    elif not zhq_devices:
        print("No zhaquirks devices, passing through z2m only.")
        passthrough = defaultdict(list)
        for norm_mf, devices in z2m_devices.items():
            for dev in devices:
                orig_mf = dev.get("mf", norm_mf)
                passthrough[orig_mf].append(dev)
        merged_devices = dict(passthrough)
        stats = {"z2m_only": sum(len(v) for v in z2m_devices.values()), "zhq_only": 0,
                 "merged": 0, "total": sum(len(v) for v in z2m_devices.values())}
    else:
        print("Merging databases...")
        merged_devices, stats = merge_sources(z2m_devices, zhq_devices)

    # Print statistics
    print(f"\n--- Merge Statistics ---")
    print(f"  z2m only:       {stats['z2m_only']}")
    print(f"  zhaquirks only: {stats['zhq_only']}")
    print(f"  merged (both):  {stats['merged']}")
    print(f"  total:          {stats['total']}")

    # Write output
    print(f"\nWriting output to {output_path}...")
    n_files, n_devices = write_output(merged_devices, output_path, z2m_meta, zhq_meta)

    print(f"\nGenerated {n_files} manufacturer files + index.json")
    print(f"Total: {n_devices} devices in {output_path}")

    # Size report
    out_dir = Path(output_path)
    total_bytes = sum(f.stat().st_size for f in out_dir.iterdir() if f.is_file())
    print(f"Total size: {total_bytes:,} bytes ({total_bytes / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
