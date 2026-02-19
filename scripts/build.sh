#!/bin/bash

###############################################################################
# ESP32-C5 Zigbee2MQTT Gateway - Build Script
#
# This script builds the project using ESP-IDF build system.
# It checks for ESP-IDF environment and sets the target to esp32c5.
#
# Usage: ./scripts/build.sh [clean]
#        ./scripts/build.sh clean - Clean build directory before building
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
echo -e "${GREEN}ESP32-C5 Zigbee2MQTT Gateway - Build${NC}"
echo -e "${GREEN}========================================${NC}"

# Check if ESP-IDF environment is set up
if [ -z "$IDF_PATH" ]; then
    echo -e "${RED}Error: ESP-IDF environment not set up!${NC}"
    echo -e "${YELLOW}Please run: source \$IDF_PATH/export.sh${NC}"
    echo -e "${YELLOW}Or run: ./scripts/setup_env.sh${NC}"
    exit 1
fi

echo -e "${GREEN}ESP-IDF Path: $IDF_PATH${NC}"
echo -e "${GREEN}IDF Version: $(cd $IDF_PATH && git describe --tags 2>/dev/null || echo 'unknown')${NC}"

# Check if ESP-Zigbee-SDK path is set
if [ -z "$ESP_ZIGBEE_SDK_PATH" ]; then
    echo -e "${YELLOW}Warning: ESP_ZIGBEE_SDK_PATH not set!${NC}"
    echo -e "${YELLOW}Please set it to your ESP-Zigbee-SDK installation path.${NC}"
    echo -e "${YELLOW}Example: export ESP_ZIGBEE_SDK_PATH=~/esp/esp-zigbee-sdk${NC}"
fi

# Change to project directory
cd "$PROJECT_DIR"

# Set target to ESP32-C5
echo -e "${GREEN}Setting target to esp32c5...${NC}"
idf.py set-target esp32c5

# Clean build if requested
if [ "$1" == "clean" ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    idf.py fullclean
fi

# Build the project
echo -e "${GREEN}Building project...${NC}"
idf.py build

# Check build status
if [ $? -eq 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Build completed successfully!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Binary location: build/${PROJECT_NAME}.bin${NC}"
    echo -e "${GREEN}To flash: ./scripts/flash.sh${NC}"
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}Build failed!${NC}"
    echo -e "${RED}========================================${NC}"
    exit 1
fi
