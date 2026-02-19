#!/bin/bash

###############################################################################
# ESP32-C5 Zigbee2MQTT Gateway - Environment Setup Script
#
# This script helps set up the development environment by:
# 1. Checking for ESP-IDF installation
# 2. Cloning ESP-Zigbee-SDK if not present
# 3. Setting up environment variables
#
# Usage: source ./scripts/setup_env.sh
#        (Note: Must be sourced, not executed)
###############################################################################

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}ESP32-C5 Zigbee2MQTT Gateway Setup${NC}"
echo -e "${GREEN}========================================${NC}"

# Check if script is being sourced
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo -e "${RED}Error: This script must be sourced, not executed!${NC}"
    echo -e "${YELLOW}Usage: source ./scripts/setup_env.sh${NC}"
    exit 1
fi

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Step 1: Check ESP-IDF installation
echo -e "${YELLOW}[1/3] Checking ESP-IDF installation...${NC}"

# Try to find ESP-IDF if not already set
if [ -z "$IDF_PATH" ]; then
    # Common ESP-IDF v6 locations (v6 preferred)
    POSSIBLE_IDF_PATHS=(
        "$HOME/esp/esp-idf-v6"
        "$HOME/esp/esp-idf"
        "$HOME/esp-idf"
        "/opt/esp-idf"
        "/usr/local/esp-idf"
    )

    for idf_path in "${POSSIBLE_IDF_PATHS[@]}"; do
        if [ -d "$idf_path" ] && [ -f "$idf_path/export.sh" ]; then
            export IDF_PATH="$idf_path"
            echo -e "${GREEN}ESP-IDF found at: $IDF_PATH${NC}"
            break
        fi
    done
fi

if [ -z "$IDF_PATH" ] || [ ! -d "$IDF_PATH" ]; then
    echo -e "${RED}ESP-IDF not found!${NC}"
    echo -e "${YELLOW}Please install ESP-IDF v6.0:${NC}"
    echo -e "${YELLOW}  git clone -b v6.0 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6${NC}"
    echo -e "${YELLOW}  cd ~/esp/esp-idf-v6${NC}"
    echo -e "${YELLOW}  ./install.sh esp32c5${NC}"
    echo -e "${YELLOW}  source export.sh${NC}"
    return 1
else
    echo -e "${GREEN}ESP-IDF found at: $IDF_PATH${NC}"

    # Check IDF version
    IDF_VERSION=$(cd "$IDF_PATH" && git describe --tags 2>/dev/null || echo 'unknown')
    echo -e "${GREEN}IDF Version: $IDF_VERSION${NC}"

    # Source ESP-IDF export script to ensure environment is set up
    if [ -f "$IDF_PATH/export.sh" ]; then
        echo -e "${YELLOW}Sourcing ESP-IDF environment...${NC}"
        source "$IDF_PATH/export.sh"
    fi
fi

# Step 2: Check ESP-Zigbee-SDK
echo -e "${YELLOW}[2/3] Checking ESP-Zigbee-SDK...${NC}"

if [ -z "$ESP_ZIGBEE_SDK_PATH" ]; then
    # Try common locations
    POSSIBLE_PATHS=(
        "$HOME/esp/esp-zigbee-sdk"
        "$PROJECT_DIR/../esp-zigbee-sdk"
        "/opt/esp-zigbee-sdk"
    )

    SDK_FOUND=false
    for path in "${POSSIBLE_PATHS[@]}"; do
        if [ -d "$path" ]; then
            export ESP_ZIGBEE_SDK_PATH="$path"
            SDK_FOUND=true
            echo -e "${GREEN}ESP-Zigbee-SDK found at: $ESP_ZIGBEE_SDK_PATH${NC}"
            break
        fi
    done

    if [ "$SDK_FOUND" = false ]; then
        # Check if running interactively
        if [ -t 0 ]; then
            echo -e "${YELLOW}ESP-Zigbee-SDK not found. Would you like to clone it? (y/n)${NC}"
            read -r response

            if [[ "$response" =~ ^[Yy]$ ]]; then
                CLONE_DIR="$HOME/esp/esp-zigbee-sdk"
                echo -e "${YELLOW}Cloning ESP-Zigbee-SDK to $CLONE_DIR...${NC}"
                mkdir -p "$HOME/esp"
                git clone --recursive https://github.com/espressif/esp-zigbee-sdk.git "$CLONE_DIR"

                if [ $? -eq 0 ]; then
                    export ESP_ZIGBEE_SDK_PATH="$CLONE_DIR"
                    echo -e "${GREEN}ESP-Zigbee-SDK cloned successfully!${NC}"
                else
                    echo -e "${RED}Failed to clone ESP-Zigbee-SDK${NC}"
                    return 1
                fi
            else
                echo -e "${YELLOW}Please clone ESP-Zigbee-SDK manually:${NC}"
                echo -e "${YELLOW}  git clone --recursive https://github.com/espressif/esp-zigbee-sdk.git${NC}"
                echo -e "${YELLOW}Then set: export ESP_ZIGBEE_SDK_PATH=/path/to/esp-zigbee-sdk${NC}"
                return 1
            fi
        else
            # Non-interactive mode: warn and continue (SDK may be fetched via idf_component.yml)
            echo -e "${YELLOW}ESP-Zigbee-SDK not found (non-interactive mode).${NC}"
            echo -e "${YELLOW}Note: SDK components may be fetched automatically via idf_component.yml${NC}"
            echo -e "${YELLOW}Or set ESP_ZIGBEE_SDK_PATH manually before building.${NC}"
        fi
    fi
else
    echo -e "${GREEN}ESP-Zigbee-SDK found at: $ESP_ZIGBEE_SDK_PATH${NC}"
fi

# Step 3: Summary and next steps
echo -e "${YELLOW}[3/3] Environment Setup Complete${NC}"
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Environment Summary:${NC}"
echo -e "${GREEN}  IDF_PATH: $IDF_PATH${NC}"
echo -e "${GREEN}  ESP_ZIGBEE_SDK_PATH: $ESP_ZIGBEE_SDK_PATH${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "${YELLOW}To persist these settings, add to your ~/.bashrc or ~/.zshrc:${NC}"
echo -e "${YELLOW}  export IDF_PATH=$IDF_PATH${NC}"
echo -e "${YELLOW}  export ESP_ZIGBEE_SDK_PATH=$ESP_ZIGBEE_SDK_PATH${NC}"
echo -e "${YELLOW}  alias get_idf='source \$IDF_PATH/export.sh'${NC}"
echo ""
echo -e "${GREEN}Next steps:${NC}"
echo -e "${GREEN}  1. cd $PROJECT_DIR${NC}"
echo -e "${GREEN}  2. ./scripts/build.sh${NC}"
echo -e "${GREEN}  3. ./scripts/flash.sh${NC}"
echo -e "${GREEN}  4. ./scripts/monitor.sh${NC}"
echo ""
