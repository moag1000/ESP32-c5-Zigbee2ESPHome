#!/usr/bin/env python3
"""Combine two z2m extracts, keeping the richer entry per device identity.

Why this exists
---------------
data/converters was produced by a version of z2m_converter_extract.py that no
longer exists in this tree: it decodes 5875 entries with exposes where the
current one manages 4399, and it holds 1761 device identities the current one
does not reach.

The current extractor is better in the other direction. After the fingerprint
and legacy-datapoint fixes it reaches 6614 identities against the old run's
6338 — 1878 of them absent from the shipped database entirely, including every
Tuya device recognised by a tuya.fingerprint() manufacturer list.

Neither run is a superset of the other, so replacing one with the other trades
one set of missing devices for another. This combines them: same key, keep
whichever entry carries more decoded content, and never drop an identity that
either run found.

It is a bridge, not a destination. When the current extractor covers what the
old run did, one extraction will produce everything and this tool becomes
unnecessary.

Usage:
    python3 tools/merge_z2m_extracts.py --inputs data/converters build/new_extract \\
        --output build/combined
"""

import argparse
import collections
import json
import os
import re
import sys
from datetime import date



# The firmware interns m/mf/v/d and its pool rejects anything at or beyond
# STRING_INTERN_MAX_LENGTH (128); a rejection returns NULL silently, and a NULL
# manufacturer in the index used to be a load access fault on the next compare.
# Some upstream manufacturer strings are also NUL-padded, and JSON-escaping wrote
# the literal six characters \u0000 into the files. Both are cleaned here as
# well as in the extractor, because this tool also reads the older shipped
# extract, which still carries them.
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


def load_extract(path):
    """(manufacturer, model) -> device dict, for one extract directory."""
    devices = {}
    if not os.path.isdir(path):
        sys.exit(f"not a directory: {path}")
    for name in sorted(os.listdir(path)):
        if not name.endswith(".json") or name == "index.json":
            continue
        try:
            with open(os.path.join(path, name), encoding="utf-8") as fh:
                payload = json.load(fh)
        except (OSError, ValueError) as exc:
            print(f"  skipping {name}: {exc}", file=sys.stderr)
            continue
        for dev in payload.get("devices", []):
            dev = sanitise_device(dev)
            devices[(dev.get("mf"), dev.get("m"))] = dev
    return devices


def richness(dev):
    """How much decoded content an entry carries.

    Both entries under one key describe the same device, so more decoded
    content is strictly better — there is no risk of preferring a wrong device,
    only of preferring a less complete description of the right one.
    """
    return (
        1 if dev.get("tuya_dp") else 0,
        len(dev.get("e") or []),
        len(dev.get("fz") or []) + len(dev.get("tz") or []),
        1 if (dev.get("d") or "").strip() else 0,
    )


def manufacturer_to_filename(manufacturer):
    name = re.sub(r"[^a-z0-9_]", "_", (manufacturer or "unknown").lower())
    name = re.sub(r"_+", "_", name).strip("_")
    return (name or "unknown") + ".json"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inputs", nargs="+", required=True,
                    help="extract directories, later ones win ties")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    combined = {}
    for path in args.inputs:
        found = load_extract(path)
        added = replaced = 0
        for key, dev in found.items():
            if key not in combined:
                combined[key] = dev
                added += 1
            elif richness(dev) > richness(combined[key]):
                combined[key] = dev
                replaced += 1
        print(f"{path}: {len(found)} identities, {added} new, {replaced} richer")

    # Group by output file, not by manufacturer: the filename lowercases and
    # strips punctuation, so distinct manufacturers can share one — writing per
    # manufacturer truncates all but the last.
    by_file = collections.defaultdict(list)
    for (manuf, _model), dev in sorted(combined.items(), key=lambda kv: (kv[0][0] or "", kv[0][1] or "")):
        by_file[manufacturer_to_filename(manuf)].append((manuf, dev))

    os.makedirs(args.output, exist_ok=True)
    manuf_info = {}
    total = 0
    for filename, members in sorted(by_file.items()):
        devices = [dev for _manuf, dev in members]
        counts = collections.Counter(manuf for manuf, _dev in members)
        for manuf, count in counts.items():
            manuf_info[manuf] = {"file": filename, "count": count}
        total += len(devices)
        with open(os.path.join(args.output, filename), "w", encoding="utf-8") as fh:
            json.dump({"devices": devices}, fh, separators=(",", ":"), ensure_ascii=False)

    index = {
        "version": 2,
        "sources": {"z2m": {"ts": date.today().isoformat(), "version": "combined"}},
        "manufacturers": manuf_info,
        "total_devices": total,
    }
    with open(os.path.join(args.output, "index.json"), "w", encoding="utf-8") as fh:
        json.dump(index, fh, separators=(",", ":"), ensure_ascii=False)

    print(f"\n{total} devices, {len(manuf_info)} manufacturers, "
          f"{len(by_file)} files -> {args.output}")


if __name__ == "__main__":
    main()
