#!/usr/bin/env bash
#
# Build a publishable release image.
#
# The point of this script is that a normal build is NOT publishable: the
# firmware compiles CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD, the MQTT credentials
# and CONFIG_ESPHOME_NOISE_PSK into the binary, and those come from
# sdkconfig.local. Flashing your own build to somebody else's board hands them
# your Wi-Fi password in cleartext.
#
# So this builds from a copy of the config with every credential blanked, then
# greps the result for them before producing anything. If a secret turns up in
# the image the script stops.
#
# Output in build_release/:
#   esp32c5-zigbee-esphome-<ver>-merged.bin   single file, flash at 0x0
#   esp32c5-zigbee-esphome-<ver>-app.bin      app only, for updating firmware
#   converters-<ver>.bin                      LittleFS device database
#   SHA256SUMS
#
# Usage: scripts/release.sh [version]

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v6/export.sh}"
VERSION="${1:-$(git -C "$PROJECT_DIR" describe --tags --always 2>/dev/null || echo dev)}"
OUT="$PROJECT_DIR/build_release"
WORK="$PROJECT_DIR/build_relwork"

fail() { printf 'FAILED: %s\n' "$1" >&2; exit 1; }

[ -f "$IDF_EXPORT" ] || fail "ESP-IDF export script not found at $IDF_EXPORT"
# shellcheck disable=SC1090
source "$IDF_EXPORT" >/dev/null 2>&1
command -v idf.py >/dev/null || fail "idf.py not on PATH after sourcing $IDF_EXPORT"

cd "$PROJECT_DIR"
[ -f sdkconfig ] || fail "No sdkconfig yet — run a normal build first."

echo "=== Config without credentials ==="
mkdir -p "$WORK"
CFG="$WORK/sdkconfig.release"
grep -vE '^CONFIG_(WIFI_SSID|WIFI_PASSWORD|MQTT_USERNAME|MQTT_PASSWORD|MQTT_BROKER_URL|ESPHOME_NOISE_PSK)=' \
    sdkconfig > "$CFG"
cat >> "$CFG" <<'EOF'
CONFIG_WIFI_SSID=""
CONFIG_WIFI_PASSWORD=""
CONFIG_MQTT_USERNAME=""
CONFIG_MQTT_PASSWORD=""
CONFIG_MQTT_BROKER_URL=""
CONFIG_ESPHOME_NOISE_PSK=""
EOF

echo "=== Converter database image ==="
rm -rf build/lfs_root && mkdir -p build/lfs_root/converters
cp data/converters_merged/*.json build/lfs_root/converters/

echo "=== Build ==="
idf.py -B "$WORK/build" -DSDKCONFIG="$CFG" build >/dev/null || fail "release build"

echo "=== Refusing to ship secrets ==="
python3 - "$PROJECT_DIR" "$WORK/build" <<'PY' || exit 1
import re, sys, pathlib
proj, build = sys.argv[1], sys.argv[2]
# Read the secrets from the DEVELOPER's config, then prove they are absent.
cfg = pathlib.Path(proj, "build", "config", "sdkconfig.h")
if not cfg.is_file():
    print("  no developer config to compare against — cannot verify, stopping")
    sys.exit(1)
text = cfg.read_text()
keys = ["CONFIG_WIFI_SSID", "CONFIG_WIFI_PASSWORD", "CONFIG_MQTT_USERNAME",
        "CONFIG_MQTT_PASSWORD", "CONFIG_MQTT_BROKER_URL", "CONFIG_ESPHOME_NOISE_PSK"]
secrets = {}
for k in keys:
    m = re.search(r'#define %s "([^"]*)"' % k, text)
    if m and len(m.group(1)) >= 4:
        secrets[k] = m.group(1)
bad = False
for img in pathlib.Path(build).glob("*.bin"):
    data = img.read_bytes()
    for k, v in secrets.items():
        if v.encode() in data:
            print(f"  {img.name}: contains {k} — STOPPING")
            bad = True
print("  checked %d image(s) against %d configured secrets: %s"
      % (len(list(pathlib.Path(build).glob('*.bin'))), len(secrets),
         "FAIL" if bad else "clean"))
sys.exit(1 if bad else 0)
PY

echo "=== Merged image ==="
mkdir -p "$OUT"
NAME="esp32c5-zigbee-esphome-$VERSION"
python -m esptool --chip esp32c5 merge-bin -o "$OUT/$NAME-merged.bin" \
    --flash-mode dio --flash-size 16MB --flash-freq 80m \
    0x2000   "$WORK/build/bootloader/bootloader.bin" \
    0x8000   "$WORK/build/partition_table/partition-table.bin" \
    0xf000   "$WORK/build/ota_data_initial.bin" \
    0x20000  "$WORK/build/esp32_c5_zigbee2mqtt.bin" \
    0x921000 "$WORK/build/spiffs.bin" >/dev/null || fail "merge-bin"

cp "$WORK/build/esp32_c5_zigbee2mqtt.bin" "$OUT/$NAME-app.bin"
cp "$WORK/build/spiffs.bin"               "$OUT/converters-$VERSION.bin"

( cd "$OUT" && shasum -a 256 ./*.bin > SHA256SUMS )

echo
echo "Release artefacts in build_release/:"
ls -la "$OUT" | tail -n +2
echo
echo "Flash with:"
echo "  esptool --chip esp32c5 -p <port> write-flash 0x0 $NAME-merged.bin"
