#!/bin/bash

###############################################################################
# ESP32-C5 Zigbee2MQTT Gateway - Monitor Script
#
# This script opens the serial monitor to view logs from the device.
# It filters logs by project-specific tags for easier debugging.
#
# Usage: ./scripts/monitor.sh [PORT]
#        ./scripts/monitor.sh /dev/ttyUSB0 - Monitor specific port
###############################################################################

set -e  # Exit on error

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}ESP32-C5 Zigbee2MQTT Gateway - Monitor${NC}"
echo -e "${GREEN}========================================${NC}"

# Check if ESP-IDF environment is set up
if [ -z "$IDF_PATH" ]; then
    echo -e "${RED}Error: ESP-IDF environment not set up!${NC}"
    echo -e "${YELLOW}Please run: source \$IDF_PATH/export.sh${NC}"
    exit 1
fi

# Change to project directory
cd "$PROJECT_DIR"

# Determine port
PORT="$1"

if [ -z "$PORT" ]; then
    # Try to auto-detect port
    if [ "$(uname)" == "Darwin" ]; then
        # macOS
        PORT=$(ls /dev/cu.usb* 2>/dev/null | head -n 1)
    else
        # Linux
        PORT=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n 1)
    fi

    if [ -z "$PORT" ]; then
        echo -e "${RED}Error: No USB device detected!${NC}"
        echo -e "${YELLOW}Please specify port: ./scripts/monitor.sh /dev/ttyUSB0${NC}"
        exit 1
    fi

    echo -e "${GREEN}Auto-detected port: $PORT${NC}"
else
    echo -e "${GREEN}Using port: $PORT${NC}"
fi

echo -e "${YELLOW}Press Ctrl+] to exit monitor${NC}"
echo -e "${GREEN}Starting monitor...${NC}"
echo ""

# Start monitor with filtering for our log tags
# Filter: MAIN, ZIGBEE, MQTT, WIFI, CORE, OTA
idf.py -p "$PORT" monitor
