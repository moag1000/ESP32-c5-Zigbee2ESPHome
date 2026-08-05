# ESP32-C5 Zigbee → ESPHome converter

That is the whole idea: a **Zigbee 3.0 coordinator** that converts its paired
devices into **ESPHome native API** entities, on a single ESP32-C5.

No MQTT broker. No zigbee2mqtt. No ZHA. One device, one integration.

## What this does differently

Everything else in this space goes one of two ways: an ESP32 that *becomes* a
Zigbee device for someone else's coordinator ([ESPHome's zigbee
component](https://esphome.io/components/zigbee/),
[luar123/zigbee_esphome](https://github.com/luar123/zigbee_esphome)), or a
coordinator that publishes to **MQTT** (zigbee2mqtt, ZHA). This is the
combination nobody else builds: the coordinator runs here, and the devices it
pairs show up as **ESPHome sub-devices** in Home Assistant.

**The ESP32-C5 is the reason it is worth doing.** Zigbee lives on 2.4 GHz. Every
2.4 GHz-only gateway — ESP32-C6, ESP32-H2, and any USB stick in a Pi on 2.4 GHz
Wi-Fi — competes with itself for the band. The C5 is dual-band, so the uplink
sits on 5 GHz while Zigbee keeps 2.4 GHz:

    Wi-Fi   channel 64, 5 GHz, -41 dBm
    Zigbee  channel 25, 2.4 GHz

C6 and H2 cannot do this at all.

What follows from that:

- **No broker in the path.** Nothing to install, nothing to keep running,
  one less thing that can be down.
- **Zigbee does not depend on the uplink.** The coordinator comes up before
  Wi-Fi and MQTT. With the network gone, paired devices stay reachable.
- **Devices are real HA devices**, with entities sorted into Controls,
  Configuration and Diagnostic — not MQTT topics plus discovery payloads.
- **6801 device definitions loaded at runtime** from LittleFS, replaceable
  without recompiling.

## Status, honestly

Hobby project, one board, one developer. Espressif recommends dual-SoC designs
(ESP32-S3 + ESP32-H2) for production Zigbee gateways, and that advice stands:
single-RF-path systems see more packet loss.

**Use it for** learning, experimenting, and a small network you are willing to
tinker with. **Do not use it for** anything you need to just work.

zigbee2mqtt and ZHA are vastly more mature — thousands of contributors, years of
field testing, far more devices. This is a different shape, not a better one.

Measured on hardware (ESP32-C5, ESP-IDF v6.0.2):

| | |
|---|---|
| Wi-Fi association | 25-27 s from boot, 0 failed attempts |
| Devices in converter DB | 6801, from 1360 manufacturers |
| Internal heap, steady state | 122 KB free (65 KB with BLE on) |
| Internal heap, low-water mark | 121 KB |
| Test suite | 119 tests, on-device |

Known gaps: GATT connections are untested for want of a device; the C5's Wi-Fi
scan returns nothing useful, so anything scan-based (the captive portal's
network list) is unreliable; 47 % of flash and a good deal of the codebase have
never been reviewed.

## Features

**Zigbee** — 3.0 coordinator, device interview, converter binding, groups,
direct binding, network topology and heal, availability tracking, backup and
restore. Scenes, Touchlink and Zigbee OTA are complete but off by default
(`CONFIG_ZB_SCENES_ENABLE`, `CONFIG_ZB_TOUCHLINK_ENABLE`, `CONFIG_ZB_OTA_ENABLE`).

**Home Assistant** — ESPHome native API on port 6053 with Noise encryption,
sub-devices, 15 entity types, entity categories, OTA on port 3232, and the
service calls `permit_join`, `remove_device`, `reconfigure_device`.

**Bluetooth** — NimBLE scanner, passive or active, switchable from Home
Assistant at runtime. Re-enabled after the original reason for disabling it
turned out to be a Wi-Fi coexistence bug rather than a BLE problem.

**MQTT** — secondary. Bridge management, diagnostics, converter database
updates. Not required for Home Assistant.

**Other** — S3KM1110 mmWave presence over UART, captive portal for Wi-Fi setup,
crash reporting, LED status, performance metrics.

## Credit where it is due

This gateway would be a Zigbee adapter without two projects that did the hard
part — knowing what a device *is*:

- **[zigbee-herdsman-converters](https://github.com/Koenkk/zigbee-herdsman-converters)**
  (MIT, Koen Kanters) — thousands of devices described by hundreds of people,
  each entry usually somebody buying hardware and working out what its vendor
  actually did. It is why a paired address becomes a "Fingerbot Plus" with a
  mode selector. **[Sponsor Koen](https://github.com/sponsors/Koenkk)** — if
  this project is useful to you, that is where the money should go first.
- **[zha-device-handlers](https://github.com/zigpy/zha-device-handlers)**
  (Apache-2.0) — covers what the z2m set does not.
- **[ESPHome](https://github.com/esphome/esphome)** — the native API's wire
  format, message numbering and entity model are theirs.

Full notices in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

<!-- If you want your own funding link, add a .github/FUNDING.yml and reference
     it here. Left out deliberately rather than guessed at. -->

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

### Memory budget (320 KB usable SRAM)

Measured on hardware 2026-08-05, steady state with Wi-Fi, MQTT and Zigbee up:

| Configuration | Free internal heap | Low-water mark |
|---|---|---|
| Zigbee only | **122 KB** | 121 KB |
| Zigbee + BLE | **65 KB** | 64 KB |

The low-water mark used to be 30 KB regardless of the steady-state figure,
because the tightest moment is early boot rather than operation — loading the
converter index briefly took 158 KB. That allocation now goes to PSRAM, along
with the entity table and all of cJSON.

PSRAM is 8 MB and still barely used, so it is the place to move anything large
that is not touched from an ISR:

```bash
riscv32-esp-elf-nm --print-size --size-sort --radix=d build/*.elf \
  | awk '$3=="b" || $3=="B" {print $2, $4}' | sort -rn | head -20
```

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
