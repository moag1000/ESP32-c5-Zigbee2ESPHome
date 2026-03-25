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

    for mf_name, devices in sorted(merged_devices.items()):
        cleaned = [clean_device(d) for d in devices]
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
