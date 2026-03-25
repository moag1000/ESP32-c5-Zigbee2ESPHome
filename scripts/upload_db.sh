#!/bin/bash
# upload_db.sh — Build LittleFS image from converter DB and flash to ESP32-C5
#
# Usage:
#   ./scripts/upload_db.sh                  # Build image from merged DB + flash
#   ./scripts/upload_db.sh --full           # Full pipeline: transpile → merge → image → flash
#   ./scripts/upload_db.sh --image-only     # Build image only (no flash)
#   ./scripts/upload_db.sh --flash-only     # Flash existing converters.bin
#
# The default source is data/converters_merged/ (output of build_converter_db.sh).
# Use --full to run the transpiler pipeline first.
#
# Prerequisites:
#   - ESP-IDF v6.0 sourced (for parttool.py)
#   - Python 3
#   - littlefs-python (pip install littlefs-python) OR mklittlefs

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
MERGED_DIR="$PROJECT_DIR/data/converters_merged"
IMAGE_FILE="$PROJECT_DIR/data/converters.bin"
PARTITION_NAME="spiffs"
FLASH_PORT="${FLASH_PORT:-/dev/cu.usbmodem5B5E1260731}"

# Parse args
DO_TRANSPILE=false
DO_IMAGE=true
DO_FLASH=true

for arg in "$@"; do
    case "$arg" in
        --full)        DO_TRANSPILE=true ;;
        --image-only)  DO_FLASH=false ;;
        --flash-only)  DO_IMAGE=false ;;
        --no-flash)    DO_FLASH=false ;;
        --help|-h)
            echo "Usage: $0 [--full|--image-only|--flash-only|--no-flash]"
            echo ""
            echo "  (default)      Build LittleFS image from data/converters_merged/ and flash"
            echo "  --full         Run transpiler pipeline first (build_converter_db.sh)"
            echo "  --image-only   Build LittleFS image only, don't flash"
            echo "  --flash-only   Flash existing data/converters.bin"
            echo "  --no-flash     Alias for --image-only"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg"
            exit 1
            ;;
    esac
done

echo "=== ESP32-C5 Converter DB Upload ==="
echo ""

# Step 1: Run transpiler pipeline (optional)
if $DO_TRANSPILE; then
    echo "--- Step 1: Run transpiler pipeline ---"
    "$SCRIPT_DIR/build_converter_db.sh"
    echo ""
fi

# Verify merged DB exists
if $DO_IMAGE; then
    if [ ! -d "$MERGED_DIR" ] || [ ! -f "$MERGED_DIR/index.json" ]; then
        echo "Error: Merged converter DB not found at $MERGED_DIR"
        echo "  Run: ./scripts/build_converter_db.sh"
        echo "  Or:  ./scripts/upload_db.sh --full"
        exit 1
    fi

    FILE_COUNT=$(ls "$MERGED_DIR"/*.json 2>/dev/null | wc -l | tr -d ' ')
    DIR_SIZE=$(du -sh "$MERGED_DIR" | cut -f1)
    echo "Source: $MERGED_DIR ($FILE_COUNT files, $DIR_SIZE)"
    echo ""
fi

# Step 2: Build LittleFS image
if $DO_IMAGE; then
    echo "--- Step 2: Build LittleFS image ---"

    # Find mklittlefs tool
    MKLITTLEFS=""
    if command -v mklittlefs &>/dev/null; then
        MKLITTLEFS="mklittlefs"
    elif [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/tools/mklittlefs/mklittlefs" ]; then
        MKLITTLEFS="$IDF_PATH/tools/mklittlefs/mklittlefs"
    elif [ -f "$HOME/.espressif/tools/mklittlefs/mklittlefs" ]; then
        MKLITTLEFS="$HOME/.espressif/tools/mklittlefs/mklittlefs"
    fi

    if [ -z "$MKLITTLEFS" ]; then
        # Use python-based littlefs image creation
        if ! python3 -c "import littlefs" 2>/dev/null; then
            echo "Error: Neither mklittlefs nor littlefs-python found."
            echo "Install with: pip install littlefs-python"
            exit 1
        fi

        python3 - "$MERGED_DIR" "$IMAGE_FILE" <<'PYEOF'
import sys
import os
from littlefs import LittleFS

src_dir = sys.argv[1]
out_file = sys.argv[2]

# 6.87MB partition = 6,946,816 bytes (from partitions.csv)
PARTITION_SIZE = 6946816
BLOCK_SIZE = 4096
BLOCK_COUNT = PARTITION_SIZE // BLOCK_SIZE

fs = LittleFS(block_size=BLOCK_SIZE, block_count=BLOCK_COUNT)
fs.mkdir("/converters")

total_bytes = 0
file_count = 0

for fname in sorted(os.listdir(src_dir)):
    if not fname.endswith(".json"):
        continue
    src_path = os.path.join(src_dir, fname)
    dst_path = f"/converters/{fname}"

    with open(src_path, 'rb') as f:
        data = f.read()

    with fs.open(dst_path, 'wb') as f:
        f.write(data)

    total_bytes += len(data)
    file_count += 1

with open(out_file, 'wb') as f:
    f.write(fs.context.buffer)

print(f"LittleFS image: {file_count} files, {total_bytes:,} bytes data")
print(f"Image size: {os.path.getsize(out_file):,} bytes ({os.path.getsize(out_file)/1024/1024:.1f} MB)")
PYEOF
    else
        # Use mklittlefs tool
        PART_SIZE=6946816  # 6.87MB (from partitions.csv)
        BLOCK_SIZE=4096

        STAGING_DIR="$PROJECT_DIR/data/.staging"
        rm -rf "$STAGING_DIR"
        mkdir -p "$STAGING_DIR/converters"
        cp "$MERGED_DIR"/*.json "$STAGING_DIR/converters/"

        "$MKLITTLEFS" -c "$STAGING_DIR" -s "$PART_SIZE" -b "$BLOCK_SIZE" "$IMAGE_FILE"

        rm -rf "$STAGING_DIR"
    fi

    if [ -f "$IMAGE_FILE" ]; then
        IMAGE_SIZE=$(stat -f%z "$IMAGE_FILE" 2>/dev/null || stat -c%s "$IMAGE_FILE" 2>/dev/null)
        echo "Image: $IMAGE_FILE ($IMAGE_SIZE bytes)"
    else
        echo "Error: Failed to create image"
        exit 1
    fi
    echo ""
fi

# Step 3: Flash to device
if $DO_FLASH; then
    echo "--- Step 3: Flash converter DB to ESP32-C5 ---"

    if [ ! -f "$IMAGE_FILE" ]; then
        echo "Error: $IMAGE_FILE not found. Run without --flash-only first."
        exit 1
    fi

    # Use parttool.py to write the spiffs partition
    PARTTOOL=""
    if command -v parttool.py &>/dev/null; then
        PARTTOOL="parttool.py"
    elif [ -n "${IDF_PATH:-}" ]; then
        PARTTOOL="python3 $IDF_PATH/components/partition_table/parttool.py"
    fi

    if [ -z "$PARTTOOL" ]; then
        echo "Error: parttool.py not found. Source ESP-IDF first:"
        echo "  source ~/esp/esp-idf-v6/export.sh"
        exit 1
    fi

    echo "Flashing to partition '$PARTITION_NAME' via $FLASH_PORT..."
    $PARTTOOL --port "$FLASH_PORT" \
        write_partition --partition-name="$PARTITION_NAME" \
        --input "$IMAGE_FILE"
    echo "Flash complete!"
    echo ""
fi

echo "=== Done ==="
