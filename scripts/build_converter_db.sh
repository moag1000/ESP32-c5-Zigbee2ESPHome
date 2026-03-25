#!/bin/bash
# build_converter_db.sh — Run transpiler pipeline to build merged converter DB
#
# Usage:
#   ./scripts/build_converter_db.sh              # Full pipeline: z2m + zhaquirks + merge
#   ./scripts/build_converter_db.sh --z2m-only   # Only run z2m transpiler
#   ./scripts/build_converter_db.sh --zha-only   # Only run zhaquirks transpiler
#   ./scripts/build_converter_db.sh --merge-only  # Only run merge (assumes z2m + zha already done)
#
# Source directories:
#   z2m:       tools/zhc/src/devices/   (zigbee-herdsman-converters TypeScript)
#   zhaquirks: tools/zhaquirks/zhaquirks/  (zha-device-handlers Python)
#
# Output directories:
#   z2m:       data/converters/
#   zhaquirks: data/converters_zhaquirks/
#   merged:    data/converters_merged/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TOOLS_DIR="$PROJECT_DIR/tools"

# Source directories
Z2M_SOURCE="$TOOLS_DIR/zhc/src/devices"
ZHA_SOURCE="$TOOLS_DIR/zhaquirks/zhaquirks"

# Output directories
Z2M_OUTPUT="$PROJECT_DIR/data/converters"
ZHA_OUTPUT="$PROJECT_DIR/data/converters_zhaquirks"
MERGED_OUTPUT="$PROJECT_DIR/data/converters_merged"

# Parse args
DO_Z2M=true
DO_ZHA=true
DO_MERGE=true

for arg in "$@"; do
    case "$arg" in
        --z2m-only)   DO_ZHA=false; DO_MERGE=false ;;
        --zha-only)   DO_Z2M=false; DO_MERGE=false ;;
        --merge-only) DO_Z2M=false; DO_ZHA=false ;;
        --help|-h)
            echo "Usage: $0 [--z2m-only|--zha-only|--merge-only]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg"
            exit 1
            ;;
    esac
done

echo "=== Converter DB Pipeline ==="
echo ""

# Step 1: z2m transpiler
if $DO_Z2M; then
    echo "--- Step 1: z2m Transpiler ---"
    if [ ! -d "$Z2M_SOURCE" ]; then
        echo "Error: z2m source not found at $Z2M_SOURCE"
        echo "  Clone zigbee-herdsman-converters into tools/zhc/"
        exit 1
    fi
    python3 "$TOOLS_DIR/z2m_converter_extract.py" \
        --source "$Z2M_SOURCE" \
        --output "$Z2M_OUTPUT"
    echo ""
fi

# Step 2: zhaquirks transpiler
if $DO_ZHA; then
    echo "--- Step 2: zhaquirks Transpiler ---"
    if [ ! -d "$ZHA_SOURCE" ]; then
        echo "Error: zhaquirks source not found at $ZHA_SOURCE"
        echo "  Clone zha-device-handlers into tools/zhaquirks/"
        exit 1
    fi
    python3 "$TOOLS_DIR/zhaquirks_transpiler.py" \
        --source "$ZHA_SOURCE" \
        --output "$ZHA_OUTPUT"
    echo ""
fi

# Step 3: Merge
if $DO_MERGE; then
    echo "--- Step 3: Merge ---"
    if [ ! -d "$Z2M_OUTPUT" ] || [ ! -f "$Z2M_OUTPUT/index.json" ]; then
        echo "Error: z2m output not found at $Z2M_OUTPUT"
        echo "  Run without --merge-only first, or run --z2m-only"
        exit 1
    fi
    if [ ! -d "$ZHA_OUTPUT" ] || [ ! -f "$ZHA_OUTPUT/index.json" ]; then
        echo "Error: zhaquirks output not found at $ZHA_OUTPUT"
        echo "  Run without --merge-only first, or run --zha-only"
        exit 1
    fi
    python3 "$TOOLS_DIR/merge_converter_dbs.py" \
        --z2m "$Z2M_OUTPUT" \
        --zhaquirks "$ZHA_OUTPUT" \
        --output "$MERGED_OUTPUT"
    echo ""
fi

# Statistics
echo "=== Statistics ==="
if [ -d "$Z2M_OUTPUT" ] && [ -f "$Z2M_OUTPUT/index.json" ]; then
    Z2M_COUNT=$(python3 -c "import json; d=json.load(open('$Z2M_OUTPUT/index.json')); print(d.get('total_devices', '?'))")
    Z2M_SIZE=$(du -sh "$Z2M_OUTPUT" | cut -f1)
    echo "  z2m:       $Z2M_COUNT devices ($Z2M_SIZE)"
fi
if [ -d "$ZHA_OUTPUT" ] && [ -f "$ZHA_OUTPUT/index.json" ]; then
    ZHA_COUNT=$(python3 -c "import json; d=json.load(open('$ZHA_OUTPUT/index.json')); print(d.get('total_devices', '?'))")
    ZHA_SIZE=$(du -sh "$ZHA_OUTPUT" | cut -f1)
    echo "  zhaquirks: $ZHA_COUNT devices ($ZHA_SIZE)"
fi
if [ -d "$MERGED_OUTPUT" ] && [ -f "$MERGED_OUTPUT/index.json" ]; then
    MERGED_COUNT=$(python3 -c "import json; d=json.load(open('$MERGED_OUTPUT/index.json')); print(d.get('total_devices', '?'))")
    MERGED_SIZE=$(du -sh "$MERGED_OUTPUT" | cut -f1)
    echo "  merged:    $MERGED_COUNT devices ($MERGED_SIZE)"

    # Tuya DP stats
    DP_STATS=$(python3 -c "
import json, os
dp_devs = 0; dp_total = 0
for f in os.listdir('$MERGED_OUTPUT'):
    if not f.endswith('.json') or f == 'index.json': continue
    for d in json.load(open(os.path.join('$MERGED_OUTPUT', f))).get('devices', []):
        dp = d.get('tuya_dp')
        if dp and isinstance(dp, dict):
            dp_devs += 1; dp_total += len(dp)
print(f'{dp_devs} devices with DP maps ({dp_total} total DPs)')
")
    echo "  tuya_dp:   $DP_STATS"
fi

echo ""
echo "=== Done ==="
