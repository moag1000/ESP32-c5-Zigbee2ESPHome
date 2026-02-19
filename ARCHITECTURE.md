# ESP32-C5 Unified Gateway - Architecture Overview

> **Detailed Documentation**: For comprehensive architecture details including FreeRTOS task structure, memory layout, synchronization points, and component details, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

High-level architecture overview for the ESP32-C5 Unified Gateway (Zigbee2MQTT + Bluetooth + ESPHome API).

## System Overview

The ESP32-C5 Unified Gateway is a production-ready firmware (~18,000 lines of code planned) that bridges **Zigbee**, **Bluetooth LE**, and **WiFi** networks to MQTT-based and ESPHome-compatible home automation systems. It runs on the ESP32-C5 SoC, leveraging its tri-radio capabilities (WiFi 6 + Zigbee + Bluetooth 5.0 LE).

## Key Features

### Zigbee Gateway (Phase 1-9, Complete)
- **Zigbee 3.0 Coordinator**: Manages 30-50 Zigbee devices
- **MQTT Bridge**: Bi-directional communication with Home Assistant
- **Home Assistant Discovery**: Automatic device discovery via MQTT
- **OTA Updates**: Over-the-air firmware updates

### Bluetooth Gateway (Phase 10-14, Planned) 🔵
- **BLE Passive Scanner**: Beacon tracking, presence detection (50+ devices)
- **BLE Active Proxy**: GATT connections to sensors (Xiaomi, Govee)
- **ESPHome Native API**: Full Home Assistant ESPHome compatibility
- **Device Tracking**: RSSI-based presence detection

### System Features
- **WiFi Connectivity**: Dual-band WiFi 6 (2.4GHz + 5GHz, **5GHz preferred**)
- **WiFi Coexistence**: Hardware coexistence for WiFi/BT/Zigbee on 2.4GHz
- **Memory Optimized**: PSRAM management for single-core architecture
- **Runtime Configuration**: MQTT-based + NVS persistent configuration
- **System Monitoring**: Health monitoring, uptime tracking, diagnostics

## High-Level Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    Application Layer                          │
│   ┌────────────┐  ┌────────────┐  ┌──────────────────────┐  │
│   │   MQTT     │  │  Zigbee    │  │  Home Assistant      │  │
│   │  Bridge    │  │ Coordinator│  │   Discovery (MQTT)   │  │
│   │  (Z+B)     │  │            │  │                      │  │
│   └────────────┘  └────────────┘  └──────────────────────┘  │
│                                                                │
│   🔵 NEW LAYER: Bluetooth + ESPHome API                       │
│   ┌────────────┐  ┌────────────┐  ┌──────────────────────┐  │
│   │ BLE Scanner│  │ BLE Proxy  │  │   ESPHome Native     │  │
│   │  (Passive) │  │  (Active)  │  │   API Server (6053)  │  │
│   └────────────┘  └────────────┘  └──────────────────────┘  │
├──────────────────────────────────────────────────────────────┤
│                      Core Layer                               │
│   ┌────────┐  ┌────────┐  ┌────────┐  ┌──────────────────┐ │
│   │ Device │  │Command │  │ Config │  │   System          │ │
│   │  State │  │Handler │  │Manager │  │   Monitor         │ │
│   │ (Z+B)  │  │ (Z+B)  │  │ (Z+B)  │  │  (Memory/CPU)     │ │
│   └────────┘  └────────┘  └────────┘  └──────────────────┘ │
├──────────────────────────────────────────────────────────────┤
│                   Protocol Layer                              │
│   ┌──────────┐  ┌──────────┐  ┌─────────┐  ┌────────────┐  │
│   │   WiFi   │  │   MQTT   │  │ Zigbee  │  │ Bluetooth  │  │
│   │ Manager  │  │  Client  │  │  Stack  │  │  LE Stack  │  │
│   │ (5GHz!)  │  │          │  │ (2.4GHz)│  │  (2.4GHz)  │  │
│   └──────────┘  └──────────┘  └─────────┘  └────────────┘  │
├──────────────────────────────────────────────────────────────┤
│            WiFi/BT/Zigbee Coexistence Layer                   │
│   ESP-IDF Coexistence API (Hardware Arbitration)             │
└──────────────────────────────────────────────────────────────┘
                 ESP32-C5 Hardware
    (RISC-V CPU @ 240MHz, WiFi 6, Zigbee, BLE 5.0, PSRAM)
```

## Core Components

### Application Layer

- **MQTT Bridge**: Central coordinator between Zigbee/Bluetooth and MQTT protocols
- **Zigbee Coordinator**: Manages Zigbee network formation, device pairing, and communication
- **BLE Scanner** 🔵: Passive scanning for beacons, presence detection (Phase 10)
- **BLE Proxy** 🔵: Active GATT connections to BLE sensors (Phase 10)
- **ESPHome API Server** 🔵: Native API server for Home Assistant ESPHome integration (Phase 10)
- **Home Assistant Discovery**: Publishes MQTT and ESPHome discovery messages

### Core Layer

- **Device State Publisher**: Manages and publishes Zigbee device states to MQTT
- **Command Handler**: Processes MQTT commands and forwards to Zigbee devices
- **Config Manager**: Centralized configuration with NVS persistence and MQTT-based updates
- **System Monitor**: Health monitoring, memory tracking, uptime, and diagnostics

### Protocol Layer

- **WiFi Manager**: WiFi connection, reconnection, and state management
- **MQTT Client**: MQTT protocol implementation, connection handling, pub/sub
- **Zigbee Stack**: IEEE 802.15.4 / Zigbee 3.0 protocol (ESP-Zigbee-SDK)

## Data Flow

### Device State Updates (Zigbee → MQTT)

```
Zigbee Device → Zigbee Coordinator → Zigbee Callbacks →
MQTT Bridge → Device State Publisher → MQTT Client →
MQTT Broker → Home Assistant
```

### Device Control (MQTT → Zigbee)

```
Home Assistant → MQTT Broker → MQTT Client → MQTT Bridge →
Command Handler → Zigbee Device Handler → Zigbee Coordinator →
Zigbee Device
```

## Memory Architecture

### Memory Budget (384 KB SRAM)

#### Zigbee Only Configuration
- FreeRTOS Kernel: ~32 KB
- WiFi Stack: ~50 KB
- Zigbee Stack: ~80 KB
- MQTT Buffers: ~20 KB
- Task Stacks: ~30 KB
- Device Tables: ~100 KB (50 Zigbee devices)
- Static Data: ~22 KB
- **Free Heap**: ~50 KB (minimum target) ✅

#### Zigbee + Bluetooth Configuration 🔵 (Phase 10-14)
- FreeRTOS Kernel: ~32 KB
- WiFi Stack: ~55 KB (+5KB for coexistence)
- Zigbee Stack: ~80 KB
- **Bluetooth LE Stack: ~40 KB** 🔵
- **ESPHome API: ~35 KB** 🔵
- MQTT Buffers: ~20 KB
- Task Stacks: ~40 KB (+10KB for BT tasks)
- Device Tables: ~42 KB (30 Zigbee + 50 BT → PSRAM offloaded)
- Static Data: ~30 KB
- **Free Heap**: ~40-60 KB (WARNING threshold) ⚠️

### PSRAM Usage

8 MB external PSRAM available for:
- OTA firmware download buffers
- Large JSON documents
- Temporary storage

## Key Design Decisions

1. **Single-Core Optimization**: Optimized for ESP32-C5's single RISC-V core
2. **Asynchronous Event-Driven**: Non-blocking operations using ESP-IDF event loop
3. **Centralized Bridge**: Single MQTT bridge coordinates all communication
4. **Three-Tier Configuration**: Kconfig defaults → NVS persistence → MQTT runtime
5. **Home Assistant Discovery**: Zero-configuration user experience
6. **Rate Limiting**: Prevent MQTT broker flooding
7. **Modular Design**: Clear separation of concerns for maintainability

## Performance Characteristics

- **MQTT messages/sec**: 50-100 (typical)
- **Zigbee commands/sec**: 10-20 (typical)
- **Device capacity**: 50 devices (tested), 100 (theoretical)
- **MQTT → Zigbee latency**: 50-200ms (typical)
- **Zigbee → MQTT latency**: 100-500ms (typical)

## Technology Stack

- **Platform**: ESP-IDF v5.0+ (master branch)
- **Zigbee**: ESP-Zigbee-SDK
- **RTOS**: FreeRTOS (built into ESP-IDF)
- **Language**: C (ISO C11)
- **Build System**: CMake
- **JSON Library**: cJSON

## Security

- Zigbee network encryption (AES-128)
- MQTTS/TLS support
- Secure boot support (optional)
- Flash encryption support (optional)
- Configuration protection

## Documentation

For detailed architecture information, see:
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**: Comprehensive architecture documentation
- **[docs/API_REFERENCE.md](docs/API_REFERENCE.md)**: Complete API reference
- **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)**: Development guide
- **[docs/MEMORY_OPTIMIZATION.md](docs/MEMORY_OPTIMIZATION.md)**: Memory management details

## Getting Started

See the following guides:
- [Hardware Setup](docs/HARDWARE_SETUP.md)
- [Installation](docs/INSTALLATION.md)
- [Configuration](docs/CONFIGURATION.md)
- [Usage](docs/USAGE.md)

## License

Apache License 2.0 - See [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.
