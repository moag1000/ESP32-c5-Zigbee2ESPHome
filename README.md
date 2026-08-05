# ESP32-C5 Zigbee Gateway (ESPHome Native API + MQTT)

> ⚠️ **EXPERIMENTAL PROJECT**: This is a hobbyist/learning project. Espressif **recommends dual-SoC solutions** (e.g., ESP32-S3 + ESP32-H2) for production Zigbee gateways due to better reliability and lower packet loss on single-RF-path systems.
>
> **Best for**: Learning, experimentation, proof-of-concept
> **Not recommended for**: Production deployments, mission-critical applications

> 🔴 **Bluetooth is disabled** (`CONFIG_BT_ENABLED=n`, as of 2026-07-31). The
> ESP32-C5 could not hold stable GATT connections under WiFi + Zigbee
> coexistence load. The BLE source tree is still present but is not compiled;
> turning it back on means re-enabling `CONFIG_BT_ENABLED` and restoring the
> NimBLE block in `sdkconfig.defaults`. Every BLE section below describes code
> that currently does not run.

A **Zigbee Coordinator** with an **ESPHome Native API** bridge for Home Assistant,
built on the ESP32-C5 SoC.

## Overview

This project implements a Zigbee gateway on the ESP32-C5 microcontroller:
- **Zigbee 3.0**: Coordinator for Zigbee devices (lights, sensors, switches)
- **WiFi 6**: Dual-band connectivity with ESPHome Native API (primary) + MQTT bridge (secondary)
- **Bluetooth LE**: implemented but **currently disabled** — see the banner above

### Key Features

#### Zigbee Gateway
- **Zigbee 3.0 Coordinator**: Manages up to 20-30 Zigbee devices (conservative tested limits)
- **MQTT Bridge**: Bi-directional communication with Home Assistant
- **Home Assistant Discovery**: Automatic device discovery via MQTT
- **ZCL Cluster Support**: On/Off, Level, Color, Temperature, Humidity, Occupancy

#### Bluetooth Gateway — DISABLED
Implemented but not compiled (`CONFIG_BT_ENABLED=n`). Kept for reference:
- **BLE Passive Scanner**: Beacon tracking, presence detection (iBeacon, Eddystone)
- **BLE Active Proxy**: GATT connections to BLE sensors (Xiaomi, Govee)
- **Device Tracking**: RSSI-based presence detection for 50+ BLE devices
- **Supported Devices**: Xiaomi LYWSD03MMC, Govee H5075, iBeacons, BLE trackers

#### ESPHome Integration (primary HA path)
- **ESPHome Native API**: Full compatibility with Home Assistant ESPHome, port 6053, Noise encryption
- **Auto-Discovery**: Appears as ESPHome device in Home Assistant
- **Sub-Devices**: Zigbee devices appear as sub-devices of the gateway (ESPHome 2025.7.0+)
- **Entity Support**: Sensors, Binary Sensors, Switches, Lights, Covers, Fans, Climate, Locks
- **ESPHome OTA**: firmware updates over the ESPHome protocol on port 3232
- **Service Calls**: framework is in place, but only `test_service` is registered so far

#### Converter Database
- Device definitions are **loaded at runtime** from a JSON database in LittleFS,
  not compiled into the firmware
- Transpiled from zigbee-herdsman-converters (MIT, Koen Kanters) and
  zha-device-handlers (Apache-2.0) by the host tooling. Those two projects
  are what turns a paired address into a named device with working
  controls — see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
  tools in `tools/`
- The database can be replaced at runtime over MQTT
  (`zigbee2mqtt/bridge/request/converter_db/update`)

#### mmWave Presence
- S3KM1110 24GHz FMCW radar over UART, 16 distance gates (~70cm each)
- Enabled with `CONFIG_MMWAVE_SENSOR_ENABLE`

#### System Features
- **WiFi Connectivity**: Dual-band WiFi 6 (2.4GHz + 5GHz, 5GHz preferred via `CONFIG_WIFI_PREFER_5GHZ`)
- **WiFi Coexistence**: Hardware coexistence for WiFi/BT/Zigbee on 2.4GHz
- **OTA Updates**: Over-the-air firmware updates
- **Memory Optimized**: Efficient PSRAM management for single-core architecture
- **Runtime Configuration**: MQTT-based + NVS persistent configuration

## Hardware Requirements

### ESP32-C5 Specifications
- **SoC**: ESP32-C5 (RISC-V @ 240MHz, single-core)
- **RAM**: 384KB SRAM + 8MB PSRAM
- **Flash**: 16MB (required for OTA plus the LittleFS converter database)
- **Wireless**:
  - WiFi 6 (2.4/5GHz dual-band)
  - Zigbee 3.0 (IEEE 802.15.4 @ 2.4GHz)
  - Bluetooth 5.0 LE (2.4GHz) — present in silicon, disabled in firmware
- **Coexistence**: Hardware support for simultaneous WiFi/Zigbee operation

### Development Board
- ESP32-C5-DevKitC-1 or compatible
- USB-C cable for programming and power
- Antenna (integrated or external)

## Software Requirements

### ESP-IDF

- **Version**: ESP-IDF v6.0.2 (Picolibc, C23, PSA Crypto)
- **Installation**: [ESP-IDF Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)

```bash
# Clone ESP-IDF v6.0.2
mkdir -p ~/esp
cd ~/esp
git clone -b v6.0.2 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6
cd esp-idf-v6

# Install ESP-IDF
./install.sh esp32c5

# Set up environment (add to ~/.bashrc or ~/.zshrc)
alias get_idf='source $HOME/esp/esp-idf-v6/export.sh'
```

The Python virtualenv that `install.sh` creates is named after the **host**
Python version (e.g. `idf6.0_py3.14_env`). Upgrading the system Python
invalidates it: `export.sh` still exits 0, but `idf.py` is then not on the PATH
and the log says `ESP-IDF Python virtual environment ... not found`. Re-run
`./install.sh esp32c5` to fix it.

### ESP-Zigbee-SDK
- **Repository**: [ESP-Zigbee-SDK](https://github.com/espressif/esp-zigbee-sdk)
- **Version**: Latest (ESP-IDF v6.0 compatible)
- ESP32-C5 supported via radio spinel interface

```bash
# Clone ESP-Zigbee-SDK
cd ~/esp
git clone --recursive https://github.com/espressif/esp-zigbee-sdk.git

# Set environment variable (add to ~/.bashrc or ~/.zshrc)
export ESP_ZIGBEE_SDK_PATH=$HOME/esp/esp-zigbee-sdk
```

## Quick Start

### 1. Environment Setup

```bash
# Clone this repository
git clone https://github.com/yourusername/esp32-c5-zigbee2mqtt.git
cd esp32-c5-zigbee2mqtt

# Run setup script (must be sourced)
source ./scripts/setup_env.sh
```

### 2. Configuration

Configure project settings using menuconfig:

```bash
idf.py menuconfig
```

Navigate to: `ESP32-C5 Zigbee2MQTT Gateway Configuration`

Configure:
- **WiFi**: SSID and password
- **MQTT**: Broker URL, credentials, topics
- **Zigbee**: PAN ID, channel, network settings

### 3. Build

```bash
./scripts/build.sh
```

### 4. Flash

```bash
# Auto-detect port and flash
./scripts/flash.sh

# Or specify port manually
./scripts/flash.sh /dev/ttyUSB0
```

### 5. Monitor

```bash
./scripts/monitor.sh
```

## Project Structure

```
esp32-c5-unified-gateway/
├── CMakeLists.txt              # Root build configuration
├── sdkconfig.defaults          # Default SDK configuration
├── partitions.csv              # Custom partition table (16MB with OTA)
├── README.md                   # This file
├── main/
│   ├── CMakeLists.txt          # Main component build config
│   ├── main.c                  # Application entry point
│   ├── Kconfig.projbuild       # Project configuration menu
│   ├── zigbee/                 # Zigbee coordinator (8 files)
│   ├── bluetooth/              # 🔵 BLE Scanner + Proxy (12 files)
│   ├── esphome/                # 🔵 ESPHome Native API (10 files)
│   ├── mqtt/                   # MQTT client (4 files)
│   ├── wifi/                   # WiFi manager with coexistence (4 files)
│   ├── core/                   # Core gateway logic (bridge, config, monitor)
│   ├── utils/                  # Utility functions (JSON, version)
│   └── ota/                    # OTA update implementation
├── components/                 # Custom components
├── docs/                       # Documentation (14 files)
│   ├── BLUETOOTH_GATEWAY.md    # 🔵 BLE Gateway Guide
│   ├── ESPHOME_API.md          # 🔵 ESPHome API Guide
│   ├── COEXISTENCE.md          # 🔵 WiFi/BT/Zigbee Coexistence
│   └── BLE_DEVICES.md          # 🔵 Supported BLE Devices
├── scripts/
│   ├── build.sh                # Build script
│   ├── flash.sh                # Flash script
│   ├── monitor.sh              # Serial monitor script
│   └── setup_env.sh            # Environment setup script
└── tests/
    ├── unit/                   # Unit tests (73 tests)
    └── integration/            # Integration tests (34 tests)
```

**Total**: ~60 source files, ~18,000 lines of code

## Memory Configuration

The partition table is optimized for 16MB flash with OTA support and Zigbee + Bluetooth:

| Partition    | Size    | Offset     | Purpose                          |
|--------------|---------|------------|----------------------------------|
| nvs          | 24KB    | 0x9000     | WiFi, MQTT, app data             |
| otadata      | 8KB     | 0xF000     | OTA state tracking               |
| phy_init     | 4KB     | 0x11000    | PHY initialization               |
| ota_0        | 4MB     | 0x20000    | Application slot 0 (active)      |
| ota_1        | 4MB     | 0x420000   | Application slot 1 (OTA target)  |
| zb_storage   | 1MB     | 0x820000   | Zigbee network data              |
| zb_fct       | 4KB     | 0x920000   | Zigbee factory data              |
| spiffs       | ~6.9MB  | 0x921000   | Logs, config files, data storage |

**Total**: 16MB (0x1000000) - Full flash utilization with dual OTA partitions

### Memory Budget (384KB SRAM)

| Configuration | Free Heap | Zigbee Devices | BT Devices | Status |
|---------------|-----------|----------------|------------|--------|
| **Zigbee Only** | ~120KB | 50 | 0 | ✅ Recommended |
| **Zigbee + BT** | ~60KB  | 30 | 50 | ⚠️ Tight but functional |

**Memory Target**:
- Zigbee Only: Minimum 50KB free heap
- With Bluetooth: Minimum 40KB free heap (WARNING threshold)

## Development Phases

### Phase 1-9: Zigbee2MQTT Core ✅ COMPLETE
- ✅ Phase 1: Project structure and build system
- ✅ Phase 2: Zigbee Coordinator (8 files, 2,260 LOC)
- ✅ Phase 3: WiFi + MQTT Client (8 files, 2,408 LOC)
- ✅ Phase 4: Zigbee-MQTT Bridge (12 files, 3,148 LOC)
- ✅ Phase 5: Memory & Performance Optimization
- ✅ Phase 6: Configuration System (NVS + MQTT)
- ✅ Phase 7: OTA Updates
- ✅ Phase 8: Testing Framework (107 tests)
- ✅ Phase 9: Comprehensive Documentation

### Phase 10-14: Bluetooth Gateway + ESPHome API ✅ COMPLETE
- ✅ Phase 10: BLE Stack + ESPHome API Foundation
  - Bluetooth LE Stack Integration (6 files)
  - ESPHome Native API Protocol (5 files)
  - BLE Scanner (passive) + BLE Proxy (active)
  - ESPHome API Server (Port 6053, Protobuf)

- ✅ Phase 11: BLE Device Support & Integration
  - Xiaomi LYWSD03MMC support
  - Govee H5075 support
  - iBeacon/Eddystone tracking
  - Bluetooth-MQTT Bridge

- ✅ Phase 12: Resource Management & Coexistence
  - Memory pool management (BT/Zigbee/MQTT)
  - CPU load balancing
  - WiFi Coexistence configuration (5GHz priority)
  - Dynamic priority adjustment

- ✅ Phase 13: Configuration & Integration
  - Kconfig extensions (BT/ESPHome options)
  - Config Manager updates
  - Partition table update

- ✅ Phase 14: Testing & Documentation
  - BT/Zigbee coexistence tests
  - ESPHome integration tests
  - Updated documentation (4 new files)

**Current Status**: Phase 1-14 complete (~34,000 LOC: ~12,000 Zigbee2MQTT + ~22,000 Bluetooth/ESPHome)

## Configuration

### WiFi Configuration
- Configure via menuconfig or Kconfig
- Supports WPA/WPA2/WPA3
- Auto-reconnect with exponential backoff

### MQTT Configuration
- Supports MQTT/MQTTS/WebSocket
- QoS levels 0, 1, 2
- Customizable topics and base path
- Username/password authentication

### Zigbee Configuration
- PAN ID: Configurable (default: 0x1A62)
- Channel: 11-26 (avoid WiFi interference, recommend 15/20/25)
- Max devices: 50 (Zigbee only) or 30 (with Bluetooth enabled)
- Network key: Random or custom

### Bluetooth Configuration 🔵 NEW
- BLE Scanner: Enabled/disabled via Kconfig (`CONFIG_BT_SCANNER_ENABLED`)
- BLE Proxy: Enabled/disabled via Kconfig (`CONFIG_BT_PROXY_ENABLED`)
- Max BLE devices: Up to 50 tracked devices
- Scan interval: 100-10000ms (default: 1000ms)
- Supported modes: Passive scanning + Active GATT proxy

### ESPHome API Configuration 🔵 NEW
- API Port: 6053 (standard ESPHome port)
- Password: Optional authentication
- Device Name: Configurable friendly name
- Auto-Discovery: mDNS-based discovery in Home Assistant
- Protocol: Native API over TCP (Protobuf)

### WiFi Coexistence 🔵 NEW
- **5GHz Preferred**: Configurable via `CONFIG_WIFI_PREFER_5GHZ` (default: enabled)
  - Uses RSSI adjustment (`CONFIG_WIFI_5GHZ_RSSI_ADJUSTMENT`, default: 10dB) to prefer 5GHz
  - Requires dual-band router with same SSID on both bands
- **Coexistence Mode**: WiFi Priority (recommended), Balanced, BT Priority, Zigbee Priority
- **Channel Planning**: Zigbee channels 15/20/25 avoid WiFi overlap

## Contributing

Contributions are welcome! Please follow these guidelines:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Follow ESP-IDF coding standards
4. Add tests for new features
5. Commit with clear messages
6. Push to your fork and submit a Pull Request

### Coding Standards

For detailed coding standards, see [docs/CODE_STYLE.md](docs/CODE_STYLE.md).

## Troubleshooting

### Build Issues
- Ensure ESP-IDF master branch is used (ESP32-C5 support)
- Check `ESP_ZIGBEE_SDK_PATH` environment variable
- Run `idf.py fullclean` and rebuild

### Flash Issues
- Check USB cable and port permissions
- Try different USB ports
- Reset device manually during flash

### Runtime Issues
- Check WiFi credentials in menuconfig
- Verify MQTT broker is reachable
- Monitor heap memory usage
- Review logs with `./scripts/monitor.sh`

## License

This project is licensed under the Apache License 2.0 - see below for details.

```
Copyright 2026 ESP32-C5 Zigbee2MQTT Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

## Resources

### ESP32 & Zigbee
- [ESP32-C5 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf)
- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [ESP-Zigbee-SDK Documentation](https://github.com/espressif/esp-zigbee-sdk)
- [Zigbee Specification](https://zigbeealliance.org/solution/zigbee/)

### Bluetooth & GATT
- [ESP-IDF Bluetooth API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/index.html)
- [ESP-IDF BLE GATT Server](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_gatts.html)
- [ESP-IDF BLE GATT Client](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_gattc.html)
- [Bluetooth SIG GATT Specifications](https://www.bluetooth.com/specifications/specs/gatt-specification-supplement/)
- [ESP32 BLE Examples](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth)
- [NimBLE Stack for ESP32](https://github.com/espressif/esp-nimble)

### ESPHome
- [ESPHome Documentation](https://esphome.io/)
- [ESPHome Native API](https://esphome.io/components/api.html)
- [ESPHome Native API Protocol (Protobuf)](https://github.com/esphome/aioesphome/tree/main/aioesphome)
- [ESPHome Developer Documentation](https://esphome.io/guides/contributing.html)
- [ESPHome Custom Components](https://esphome.io/custom/custom_component.html)

### Home Assistant Integration
- [Home Assistant MQTT Discovery](https://www.home-assistant.io/docs/mqtt/discovery/)
- [Home Assistant ESPHome Integration](https://www.home-assistant.io/integrations/esphome/)
- [Home Assistant Bluetooth Integration](https://www.home-assistant.io/integrations/bluetooth/)
- [Home Assistant Device Automation](https://www.home-assistant.io/docs/automation/trigger/#device-triggers)
- [Home Assistant ESP32 Bluetooth Proxy](https://esphome.io/components/bluetooth_proxy.html)

## Support

For issues, questions, or contributions:
- Open an issue on GitHub
- Join the ESP32 community forums
- Check existing documentation

## Acknowledgments

- Espressif Systems for ESP-IDF and ESP-Zigbee-SDK
- Zigbee Alliance for specifications
- Home Assistant community
- All contributors to this project

---

**Status**:
- ✅ Phase 1-9 Complete - Zigbee2MQTT fully functional (~12,000 LOC)
- ✅ Phase 10-14 Complete - Bluetooth Gateway + ESPHome API (~22,000 LOC)

**Device Support**:
- Zigbee Devices: 30-50 (depending on Bluetooth enablement)
- Bluetooth Devices: 50
- Total Capacity: Up to 80 devices with both protocols

**Recommended Setup**:
- WiFi: 5GHz (minimize 2.4GHz crowding)
- Zigbee Channel: 15, 20, or 25
- Memory: Enable Bluetooth only if needed (reduces available heap)

**Last Updated**: January 23, 2026 (Bluetooth Gateway documentation added)
