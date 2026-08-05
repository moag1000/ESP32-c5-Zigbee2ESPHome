# Installation Guide

<!-- staleness-banner -->
> **Stand 2026-08-05.** Die BLE-Erwaehnung betrifft abgeschalteten Code.
>
> Aktuell gepflegt wird `CLAUDE.md` im Projektwurzelverzeichnis.


This guide walks you through setting up the development environment, building the firmware, and flashing it to your ESP32-C5 device.

> **Toolchain as of 2026-07-31: ESP-IDF v6.0.2.** If a version below says v6.0,
> read v6.0.2.
>
> The virtualenv that `install.sh` creates is named after the **host** Python
> version (currently `idf6.0_py3.14_env`). Upgrading the system Python
> invalidates it. The failure is confusing: `export.sh` still exits 0, but
> `idf.py` is then not on the PATH, and the export log contains
> `ERROR: ESP-IDF Python virtual environment ... not found`. Fix it by re-running
> `./install.sh esp32c5` — that pulls roughly 3.4GB into `~/.espressif`.
>
> Bluetooth is disabled in this firmware (`CONFIG_BT_ENABLED=n`); any BLE setup
> step below can be skipped.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Install ESP-IDF](#install-esp-idf)
- [Install ESP-Zigbee-SDK](#install-esp-zigbee-sdk)
- [Clone Project](#clone-project)
- [Environment Setup](#environment-setup)
- [Build Firmware](#build-firmware)
- [Flash Firmware](#flash-firmware)
- [Initial Configuration](#initial-configuration)
- [Verify Installation](#verify-installation)

## Prerequisites

### System Requirements

| Requirement | Specification |
|-------------|---------------|
| **Operating System** | Linux (Ubuntu 20.04+), macOS (10.15+), Windows 10/11 with WSL2 |
| **Python** | 3.8 or newer |
| **Git** | 2.20 or newer |
| **Disk Space** | 4 GB minimum for ESP-IDF and project |
| **RAM** | 4 GB minimum, 8 GB recommended |
| **USB Port** | USB 2.0 or higher for programming |

### Software Dependencies

The following will be installed automatically by ESP-IDF:
- CMake 3.16 or newer
- Ninja build system
- Cross-compiler toolchain (RISC-V)
- Python packages (requirements.txt)

### Supported Platforms

**Linux (Recommended):**
- Ubuntu 20.04 LTS or newer
- Debian 10 or newer
- Fedora 32 or newer
- Other distributions with equivalent packages

**macOS:**
- macOS 10.15 (Catalina) or newer
- Xcode Command Line Tools

**Windows:**
- Windows 10/11 with WSL2 (Ubuntu 20.04+ recommended)
- Native Windows support via ESP-IDF installer (experimental)

## Install ESP-IDF

ESP-IDF (Espressif IoT Development Framework) is required to build firmware for ESP32 chips.

### Quick Installation (Linux/macOS)

```bash
# Install prerequisites
# Ubuntu/Debian:
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip \
    python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util \
    libusb-1.0-0

# macOS (requires Homebrew):
brew install cmake ninja dfu-util python3

# Create ESP directory
mkdir -p ~/esp
cd ~/esp

# Clone ESP-IDF v6.0
git clone -b v6.0 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6
cd esp-idf-v6

# Install ESP-IDF tools for ESP32-C5
./install.sh esp32c5

# Set up environment (add to ~/.bashrc or ~/.zshrc for persistence)
. ./export.sh
```

### Detailed Installation Steps

#### 1. Install System Dependencies

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install -y \
    git wget flex bison gperf python3 python3-pip python3-venv \
    cmake ninja-build ccache libffi-dev libssl-dev dfu-util \
    libusb-1.0-0 pkg-config
```

**macOS:**
```bash
# Install Homebrew if not already installed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install dependencies
brew install cmake ninja dfu-util python3 ccache
```

**Windows (WSL2):**
```powershell
# In PowerShell (Admin), install WSL2
wsl --install -d Ubuntu-20.04

# Restart computer, then in WSL terminal:
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip \
    python3-venv cmake ninja-build ccache libffi-dev libssl-dev \
    dfu-util libusb-1.0-0
```

#### 2. Clone ESP-IDF v6.0

```bash
# Create ESP directory
mkdir -p ~/esp
cd ~/esp

# Clone ESP-IDF v6.0
git clone -b v6.0 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6
cd esp-idf-v6
```

#### 3. Install ESP-IDF Tools

```bash
cd ~/esp/esp-idf-v6

# Install tools for ESP32-C5 target
./install.sh esp32c5

# This will download and install:
# - RISC-V toolchain (riscv32-esp-elf)
# - OpenOCD debugger
# - Python packages
# - Other required tools
```

#### 4. Set Up Environment Variables

**Temporary (current session only):**
```bash
cd ~/esp/esp-idf-v6
. ./export.sh
```

**Permanent (recommended):**

Add to `~/.bashrc` (Linux) or `~/.zshrc` (macOS):
```bash
# ESP-IDF v6.0 Environment
alias get_idf='. $HOME/esp/esp-idf-v6/export.sh'

# Automatically activate (optional)
# . $HOME/esp/esp-idf-v6/export.sh
```

Reload shell configuration:
```bash
source ~/.bashrc  # or ~/.zshrc for macOS
```

#### 5. Verify ESP-IDF Installation

```bash
# Activate environment
get_idf  # or '. ~/esp/esp-idf-v6/export.sh'

# Verify idf.py command
idf.py --version

# Should output something like:
# ESP-IDF v6.0

# Verify target support
idf.py --list-targets | grep esp32c5
```

## Install ESP-Zigbee-SDK

The ESP-Zigbee-SDK provides Zigbee 3.0 protocol stack libraries.

### Installation Steps

```bash
# Navigate to ESP directory
cd ~/esp

# Clone ESP-Zigbee-SDK
git clone --recursive https://github.com/espressif/esp-zigbee-sdk.git
cd esp-zigbee-sdk

# Checkout latest release or stay on master
git checkout master
git submodule update --init --recursive

# Set environment variable (add to ~/.bashrc or ~/.zshrc)
export ESP_ZIGBEE_SDK_PATH=$HOME/esp/esp-zigbee-sdk
```

### Permanent Environment Variable

Add to `~/.bashrc` or `~/.zshrc`:
```bash
# ESP-Zigbee-SDK Path
export ESP_ZIGBEE_SDK_PATH=$HOME/esp/esp-zigbee-sdk
```

Reload configuration:
```bash
source ~/.bashrc  # or ~/.zshrc
```

### Verify Zigbee SDK Installation

```bash
# Check environment variable
echo $ESP_ZIGBEE_SDK_PATH

# Should output: /home/username/esp/esp-zigbee-sdk

# Verify SDK structure
ls $ESP_ZIGBEE_SDK_PATH/components/
# Should show: esp-zigbee-lib, zboss_stack, etc.
```

## Clone Project

### Clone from Git

```bash
# Navigate to projects directory
cd ~/projects  # or your preferred location

# Clone repository
git clone https://github.com/yourusername/esp32-c5-zigbee2mqtt.git
cd esp32-c5-zigbee2mqtt

# Verify project structure
ls -la
# Should show: main/, CMakeLists.txt, README.md, etc.
```

### Download as ZIP (Alternative)

If you don't have Git or prefer to download:

1. Visit: https://github.com/yourusername/esp32-c5-zigbee2mqtt
2. Click "Code" → "Download ZIP"
3. Extract to desired location
4. Navigate to extracted directory

## Environment Setup

### Automated Setup Script

The project includes a setup script to configure the environment:

```bash
cd ~/projects/esp32-c5-zigbee2mqtt

# Source the setup script (must be sourced, not executed)
source ./scripts/setup_env.sh

# This script will:
# 1. Activate ESP-IDF environment
# 2. Verify ESP_ZIGBEE_SDK_PATH is set
# 3. Set IDF_TARGET to esp32c5
# 4. Display environment information
```

### Manual Setup

If the automated script doesn't work:

```bash
# Activate ESP-IDF v6.0 environment
. ~/esp/esp-idf-v6/export.sh

# Set Zigbee SDK path (if not already in ~/.bashrc)
export ESP_ZIGBEE_SDK_PATH=$HOME/esp/esp-zigbee-sdk

# Set target to ESP32-C5
cd ~/projects/esp32-c5-zigbee2mqtt
idf.py set-target esp32c5

# This creates sdkconfig file with ESP32-C5 defaults
```

### Verify Environment

```bash
# Check IDF is activated
which idf.py
# Should show: ~/esp/esp-idf-v6/tools/idf.py

# Check Zigbee SDK
echo $ESP_ZIGBEE_SDK_PATH
# Should show: /home/username/esp/esp-zigbee-sdk

# Check target
idf.py --version
# Should mention esp32c5 target
```

## Build Firmware

### Configure Project

Before building, configure the project settings:

```bash
cd ~/projects/esp32-c5-zigbee2mqtt

# Open configuration menu
idf.py menuconfig
```

Navigate to `ESP32-C5 Zigbee2MQTT Gateway Configuration` and set:

**WiFi Configuration:**
- WiFi SSID: Your network name
- WiFi Password: Your network password

**MQTT Configuration:**
- MQTT Broker URL: e.g., `mqtt://192.168.1.100`
- MQTT Username: (if required)
- MQTT Password: (if required)

**Zigbee Configuration:**
- Zigbee Channel: 11-26 (default 11, avoid WiFi interference)
- Maximum Zigbee Children: 20-50 depending on needs

Press `S` to save, then `Q` to quit.

### Build Project

```bash
# Full build
idf.py build

# This will:
# 1. Generate build configuration
# 2. Compile all source files
# 3. Link binaries
# 4. Generate flashable images
# 5. Show memory usage summary

# Build takes 2-5 minutes on first run
# Subsequent builds are much faster (incremental)
```

### Build Output

Successful build output:
```
Project build complete. To flash, run:
 idf.py flash
or
 python -m esptool --chip esp32c5 --port /dev/ttyUSB0 --baud 460800 ...

Build time: XX.XXs
```

Firmware files location: `build/esp32-c5-zigbee2mqtt.bin`

### Troubleshooting Build Issues

**Missing Zigbee SDK:**
```
Error: ESP_ZIGBEE_SDK_PATH not set
Solution: export ESP_ZIGBEE_SDK_PATH=$HOME/esp/esp-zigbee-sdk
```

**Wrong Target:**
```
Error: This project is configured for esp32 target
Solution: idf.py set-target esp32c5
```

**Submodule Issues:**
```
Error: Cannot find required component
Solution: git submodule update --init --recursive
```

**Out of Memory During Build:**
```
Solution: Close other applications, or use:
idf.py build -j1  # Single-threaded build
```

## Flash Firmware

### Automatic Flash

```bash
# Connect ESP32-C5 via USB
# Auto-detect port and flash
idf.py flash

# Or use helper script
./scripts/flash.sh

# Flash with monitoring (recommended)
idf.py flash monitor
```

### Manual Port Selection

```bash
# List available ports
ls /dev/ttyUSB*   # Linux
ls /dev/cu.*      # macOS

# Flash to specific port
idf.py -p /dev/ttyUSB0 flash

# Or with script
./scripts/flash.sh /dev/ttyUSB0
```

### Flash Options

```bash
# Fast flash (921600 baud, risky on some systems)
idf.py -p /dev/ttyUSB0 -b 921600 flash

# Safe flash (slower but more reliable)
idf.py -p /dev/ttyUSB0 -b 115200 flash

# Erase flash before flashing (factory reset)
idf.py -p /dev/ttyUSB0 erase-flash flash

# Flash only app (skip bootloader and partition table)
idf.py flash app
```

### Flash Process

The flash process includes:
1. **Bootloader** (0x0000): ESP32-C5 bootloader
2. **Partition Table** (0x8000): Partition layout
3. **NVS** (0x9000): Non-volatile storage
4. **PHY Init** (0xF000): RF calibration data
5. **Application** (0x10000): Main firmware

Typical flash time: 30-60 seconds

### Entering Download Mode

If flashing fails, manually enter download mode:

1. Hold **BOOT** button
2. Press and release **RESET** button
3. Release **BOOT** button
4. Run `idf.py flash`

## Initial Configuration

### Monitor Serial Output

```bash
# Monitor serial output
idf.py monitor

# Or use script
./scripts/monitor.sh

# Exit monitor: Ctrl+]
```

### First Boot Sequence

After flashing, the device will:
1. Boot and initialize NVS
2. Connect to WiFi (using configured SSID/password)
3. Connect to MQTT broker
4. Initialize Zigbee coordinator
5. Start MQTT bridge
6. Report system ready

### Expected Boot Log

```
ESP32-C5 Zigbee2MQTT Gateway
Version: v1.0.0
Chip: esp32c5, Cores: 1
Flash size: 8 MB
Free heap: 280000 bytes

[INIT] NVS initialized successfully
[INIT] Event loop initialized successfully
[INIT] Configuration Manager initialized
[WIFI] Connecting to WiFi...
[WIFI] WiFi connected! IP: 192.168.1.100
[MQTT] MQTT Client ID: esp32c5_zigbee_gateway_aabbcc
[MQTT] MQTT connected successfully!
[ZIGBEE] Zigbee coordinator initialized successfully
[ZIGBEE] Zigbee network formed, PAN ID: 0x1A62
[CORE] MQTT bridge initialized successfully
System Initialization Complete
ESP32-C5 Zigbee2MQTT Gateway Ready
```

## Verify Installation

### Check System Status

Via serial monitor, verify:
- WiFi connection successful
- MQTT connection established
- Zigbee coordinator running
- Free heap > 50KB

### Test MQTT Connection

Subscribe to MQTT topics:
```bash
# On your MQTT broker or another machine
mosquitto_sub -h localhost -t "zigbee2mqtt/#" -v

# Should see bridge state message:
zigbee2mqtt/bridge/state online
```

### Test Zigbee Pairing

```bash
# Publish permit join command
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/permit_join" \
    -m '{"value":true}'

# Check for response
zigbee2mqtt/bridge/response/permit_join {"data":{"value":true},"status":"ok"}
```

### Verify Firmware Version

Check serial output or MQTT:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/info" -C 1
```

Should show:
```json
{
  "version": "v1.0.0",
  "coordinator": {
    "type": "ESP32-C5",
    "ieee_address": "0x00124b0024xxxxxx"
  }
}
```

## Common Installation Issues

### ESP-IDF Installation Fails

**Issue**: `./install.sh` fails with Python errors

**Solution**:
```bash
# Update Python pip
python3 -m pip install --upgrade pip

# Retry installation
./install.sh esp32c5
```

### USB Device Not Found

**Issue**: Cannot detect ESP32-C5 on USB

**Solutions**:
1. Install CP210x driver
2. Check USB cable (must support data)
3. Add user to dialout group (Linux):
   ```bash
   sudo usermod -a -G dialout $USER
   # Log out and back in
   ```

### Flash Permission Denied (Linux)

**Issue**: `Permission denied: /dev/ttyUSB0`

**Solution**:
```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER

# Or create udev rule (see Hardware Setup guide)
sudo nano /etc/udev/rules.d/99-esp32.rules
```

### Build Fails with "No Space Left"

**Issue**: Build fails with disk space errors

**Solution**:
```bash
# Clean build directory
idf.py fullclean

# Remove old builds
rm -rf build/

# Check disk space
df -h
```

### WiFi/MQTT Connection Fails

**Issue**: Device boots but doesn't connect

**Solutions**:
1. Verify SSID/password in menuconfig
2. Check MQTT broker is reachable
3. Use static IP instead of DHCP if needed
4. Check router logs for connection attempts
5. Monitor serial output for error messages

## Next Steps

After successful installation:
1. Read [Configuration Guide](CONFIGURATION.md) for advanced settings
2. Follow [Usage Guide](USAGE.md) to pair Zigbee devices
3. Set up [Home Assistant Integration](USAGE.md#home-assistant-integration)
4. Review [Troubleshooting Guide](TROUBLESHOOTING.md) if issues occur

## Updating Firmware

### OTA Updates

After initial installation, updates can be done over-the-air:
```bash
# Via MQTT command
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/ota_check" -m '{}'
```

See [Usage Guide](USAGE.md#ota-updates) for details.

### Manual Updates

To manually update firmware:
```bash
# Pull latest code
git pull origin main

# Rebuild
idf.py build

# Flash update
idf.py flash
```

## Development Environment (Optional)

For development work, consider installing:

**IDE/Editor:**
- Visual Studio Code with ESP-IDF extension
- CLion with ESP-IDF plugin
- Vim/Emacs with language server

**Debugging Tools:**
- ESP-Prog JTAG debugger
- Logic analyzer for protocol debugging
- Wireshark for network analysis

See [Development Guide](DEVELOPMENT.md) for details.
