#!/bin/bash

###############################################################################
# run_tests.sh - Build and Run Tests for ESP32-C5 Zigbee2MQTT Gateway
#
# This script builds the test suite and runs it on the target device.
# It parses the output to determine pass/fail status.
#
# Usage:
#   ./scripts/run_tests.sh [OPTIONS]
#
# Options:
#   -p PORT    Serial port (default: /dev/ttyUSB0)
#   -b         Build only, don't flash or monitor
#   -c         Clean build directory before building
#   -v         Verbose output
#   -h         Show help
#
# Copyright (c) 2026
# License: Apache License 2.0
###############################################################################

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default configuration
SERIAL_PORT="${SERIAL_PORT:-/dev/ttyUSB0}"
BUILD_ONLY=false
CLEAN_BUILD=false
VERBOSE=false
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TEST_DIR="$PROJECT_DIR/tests"
BUILD_DIR="$TEST_DIR/build"

# Parse command line arguments
while getopts "p:bcvh" opt; do
    case $opt in
        p)
            SERIAL_PORT="$OPTARG"
            ;;
        b)
            BUILD_ONLY=true
            ;;
        c)
            CLEAN_BUILD=true
            ;;
        v)
            VERBOSE=true
            ;;
        h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -p PORT    Serial port (default: /dev/ttyUSB0)"
            echo "  -b         Build only, don't flash or monitor"
            echo "  -c         Clean build directory before building"
            echo "  -v         Verbose output"
            echo "  -h         Show help"
            exit 0
            ;;
        \?)
            echo "Invalid option: -$OPTARG" >&2
            exit 1
            ;;
    esac
done

# Functions
print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

# Check if IDF_PATH is set
if [ -z "$IDF_PATH" ]; then
    print_error "IDF_PATH is not set. Please run: source \$IDF_PATH/export.sh"
    exit 1
fi

print_header "ESP32-C5 Zigbee2MQTT Gateway Test Suite"

# Navigate to test directory
cd "$TEST_DIR"

# Clean build if requested
if [ "$CLEAN_BUILD" = true ]; then
    print_info "Cleaning build directory..."
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
        print_success "Build directory cleaned"
    fi
fi

# Set target
print_info "Setting target to esp32c5..."
idf.py set-target esp32c5

# Build tests
print_header "Building Test Suite"
if [ "$VERBOSE" = true ]; then
    idf.py build
else
    idf.py build > /dev/null 2>&1
fi

if [ $? -eq 0 ]; then
    print_success "Build completed successfully"
else
    print_error "Build failed"
    exit 1
fi

# Exit if build only
if [ "$BUILD_ONLY" = true ]; then
    print_info "Build only mode - exiting"
    exit 0
fi

# Flash and monitor
print_header "Flashing and Running Tests"
print_info "Using serial port: $SERIAL_PORT"

# Create a temporary file for test output
TEMP_OUTPUT=$(mktemp)
trap "rm -f $TEMP_OUTPUT" EXIT

print_info "Flashing firmware..."
idf.py -p "$SERIAL_PORT" flash > /dev/null 2>&1

if [ $? -ne 0 ]; then
    print_error "Flash failed"
    exit 1
fi

print_success "Flash completed"
print_info "Starting test monitor..."
print_info "Waiting for test results... (this may take a minute)"

# Monitor output and capture results
# We'll wait up to 120 seconds for test completion
timeout 120 idf.py -p "$SERIAL_PORT" monitor 2>&1 | tee "$TEMP_OUTPUT" &
MONITOR_PID=$!

# Wait for monitor to complete or timeout
wait $MONITOR_PID 2>/dev/null
MONITOR_EXIT=$?

# Parse test results
print_header "Test Results"

if grep -q "ALL TESTS PASSED" "$TEMP_OUTPUT"; then
    print_success "All tests passed!"

    # Extract statistics
    TOTAL=$(grep "Total Tests:" "$TEMP_OUTPUT" | awk '{print $3}')
    PASSED=$(grep "Passed:" "$TEMP_OUTPUT" | grep -v "Suite:" | awk '{print $2}')

    echo ""
    echo "  Total Tests: $TOTAL"
    echo "  Passed:      $PASSED"
    echo ""

    exit 0

elif grep -q "TEST(S) FAILED" "$TEMP_OUTPUT"; then
    FAILED=$(grep "TEST(S) FAILED" "$TEMP_OUTPUT" | awk '{print $2}')
    print_error "$FAILED test(s) failed!"

    # Extract statistics
    TOTAL=$(grep "Total Tests:" "$TEMP_OUTPUT" | awk '{print $3}')
    PASSED=$(grep "Passed:" "$TEMP_OUTPUT" | grep -v "Suite:" | awk '{print $2}')
    FAILED_COUNT=$(grep "FAILED:" "$TEMP_OUTPUT" | grep -v "Suite:" | awk '{print $2}')

    echo ""
    echo "  Total Tests: $TOTAL"
    echo "  Passed:      $PASSED"
    echo "  Failed:      $FAILED_COUNT"
    echo ""

    # Show failed tests
    print_info "Failed tests:"
    grep "Test FAILED:" "$TEMP_OUTPUT" | sed 's/^/  /'

    exit 1

else
    print_error "Could not determine test results (timeout or error)"
    print_info "Check the output above for details"
    exit 1
fi
