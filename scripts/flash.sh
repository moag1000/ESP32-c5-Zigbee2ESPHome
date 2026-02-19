#!/bin/bash

###############################################################################
# ESP32-C5 Zigbee2MQTT Gateway - Flash Script
#
# This script flashes the compiled firmware to the ESP32-C5 device.
# It automatically detects the USB port and flashes the device.
#
# Usage: ./scripts/flash.sh [PORT]
#        ./scripts/flash.sh /dev/ttyUSB0 - Flash to specific port
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
echo -e "${GREEN}ESP32-C5 Zigbee2MQTT Gateway - Flash${NC}"
echo -e "${GREEN}========================================${NC}"

# Check if ESP-IDF environment is set up
if [ -z "$IDF_PATH" ]; then
    echo -e "${RED}Error: ESP-IDF environment not set up!${NC}"
    echo -e "${YELLOW}Please run: source \$IDF_PATH/export.sh${NC}"
    exit 1
fi

# Change to project directory
cd "$PROJECT_DIR"

# Check if build exists
if [ ! -d "build" ]; then
    echo -e "${RED}Error: Build directory not found!${NC}"
    echo -e "${YELLOW}Please run: ./scripts/build.sh${NC}"
    exit 1
fi

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
        echo -e "${YELLOW}Please specify port: ./scripts/flash.sh /dev/ttyUSB0${NC}"
        exit 1
    fi

    echo -e "${GREEN}Auto-detected port: $PORT${NC}"
else
    echo -e "${GREEN}Using port: $PORT${NC}"
fi

# Flash the device
echo -e "${GREEN}Flashing device...${NC}"
idf.py -p "$PORT" flash

# Check flash status
if [ $? -eq 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Flash completed successfully!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}To monitor: ./scripts/monitor.sh${NC}"
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}Flash failed!${NC}"
    echo -e "${RED}========================================${NC}"
    exit 1
fi
