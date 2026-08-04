#!/usr/bin/env bash
#
# Build the test suite, run it on the board, put the gateway back.
#
# The restore is the point. The test binary shares the gateway's partition
# layout so it only replaces the app in ota_0, but leaving the board running
# tests instead of the gateway is not something to rely on remembering. This
# script restores it even when the tests fail or you interrupt it.
#
# Usage:
#   scripts/test.sh                 # autodetect port
#   scripts/test.sh /dev/cu.usbmodemXXXX
#   scripts/test.sh --no-restore    # leave the test app on the board
#
# Exit status is the test result: 0 all passed, 1 something failed.

set -o pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v6/export.sh}"
CAPTURE_SECONDS="${CAPTURE_SECONDS:-50}"

RESTORE=1
PORT=""
for arg in "$@"; do
    case "$arg" in
        --no-restore) RESTORE=0 ;;
        /dev/*)       PORT="$arg" ;;
        *)            echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done

log()  { printf '\n=== %s ===\n' "$1"; }
fail() { printf 'FAILED: %s\n' "$1" >&2; exit 1; }

# --- environment -------------------------------------------------------------

[ -f "$IDF_EXPORT" ] || fail "ESP-IDF export script not found at $IDF_EXPORT
Set IDF_EXPORT to point at it, or run ./install.sh esp32c5 in your IDF checkout."

# shellcheck disable=SC1090
source "$IDF_EXPORT" >/dev/null 2>&1
command -v idf.py >/dev/null || fail "idf.py not on PATH after sourcing $IDF_EXPORT.
The virtualenv is named after the host Python version, so a Python upgrade
invalidates it — re-run install.sh esp32c5."

# --- port --------------------------------------------------------------------

if [ -z "$PORT" ]; then
    # Pick the single usbmodem device if there is exactly one.
    # Plain globbing rather than mapfile: macOS ships bash 3.2, which has no
    # mapfile, and this script should run with the shell that is actually there.
    PORTS=""
    PORT_COUNT=0
    for candidate in /dev/cu.usbmodem*; do
        [ -e "$candidate" ] || continue
        PORTS="$PORTS  $candidate
"
        PORT_COUNT=$((PORT_COUNT + 1))
        PORT="$candidate"
    done
    case "$PORT_COUNT" in
        0) fail "No /dev/cu.usbmodem* found. Is the board plugged in?" ;;
        1) : ;;  # PORT already set
        *) PORT=""; fail "Several serial ports found, pass one explicitly:
$PORTS" ;;
    esac
fi
echo "Port: $PORT"

# --- always put the gateway back --------------------------------------------

restore_gateway() {
    [ "$RESTORE" -eq 1 ] || { echo "Leaving the test app on the board (--no-restore)."; return; }
    log "Restoring gateway firmware"

    # Retry: a board that has just been reset over USB-Serial/JTAG is not
    # always ready for the next connection straight away, and leaving the test
    # firmware on the gateway is the worst outcome this script can produce.
    local attempt
    for attempt in 1 2 3; do
        if idf.py -C "$PROJECT_DIR" -p "$PORT" flash >/dev/null 2>&1; then
            echo "Gateway restored."
            return
        fi
        echo "Restore attempt $attempt failed, retrying..." >&2
        sleep 3
    done

    # Make this impossible to miss: the board is left running the tests.
    cat >&2 <<EOM

################################################################################
#  GATEWAY NOT RESTORED — the board is still running the TEST firmware.
#
#  Three attempts failed. The board may need to be power-cycled: unplug the
#  USB cable, plug it back in, then run
#
#      idf.py -C $PROJECT_DIR -p $PORT flash
#
#  Until then the gateway is not doing its job: no Zigbee, no MQTT, no
#  Home Assistant. Pairings and the converter database are untouched — the
#  test app shares the partition layout and only occupies ota_0.
################################################################################

EOM
}
trap restore_gateway EXIT

# --- build & flash -----------------------------------------------------------

log "Building test suite"
idf.py -C "$PROJECT_DIR/tests" build || fail "test build"

log "Building gateway (so the restore does not rebuild under a trap)"
idf.py -C "$PROJECT_DIR" build >/dev/null || fail "gateway build"

log "Flashing tests to $PORT"
idf.py -C "$PROJECT_DIR/tests" -p "$PORT" flash >/dev/null || fail "test flash"

# --- run ---------------------------------------------------------------------

log "Running tests (${CAPTURE_SECONDS}s capture)"
OUTPUT="$(mktemp)"
python - "$PORT" "$CAPTURE_SECONDS" "$OUTPUT" <<'PY'
import sys, time, serial
port, seconds, path = sys.argv[1], float(sys.argv[2]), sys.argv[3]
ser = serial.Serial(port, 115200, timeout=0.2)
# DTR/RTS toggle: reset into the freshly flashed app.
ser.setDTR(False); ser.setRTS(True); time.sleep(0.1)
ser.setRTS(False); ser.setDTR(False)
buf = b""
deadline = time.time() + seconds
while time.time() < deadline:
    buf += ser.read(4096)
    if b"ALL TESTS PASSED" in buf or b"TEST(S) FAILED" in buf:
        break          # summary reached, no need to wait out the window
ser.close()
open(path, "wb").write(buf)
PY

# Strip ANSI colour so grep sees plain text.
RESULT="$(sed 's/\x1b\[[0-9;]*m//g' "$OUTPUT")"

log "Results"
echo "$RESULT" | grep -E "Suite:|Passed: |Total Tests|FAILED:" || true

if echo "$RESULT" | grep -q "ALL TESTS PASSED"; then
    echo
    echo "PASS"
    rm -f "$OUTPUT"
    exit 0
fi

echo
echo "Test output kept at: $OUTPUT"
if echo "$RESULT" | grep -q "TEST(S) FAILED"; then
    echo "$RESULT" | grep -E "Test FAILED|assert failed" | head -20
    fail "tests reported failures"
fi
fail "no test summary within ${CAPTURE_SECONDS}s — raise CAPTURE_SECONDS or check the board"
