# Bluetooth Gateway Guide

Complete guide to the Bluetooth LE Gateway functionality of the ESP32-C5 Unified Gateway.

> **Status**: ✅ Phase 10-14 Complete
> **Requires**: `CONFIG_BT_SCANNER_ENABLED=y`

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Supported Devices](#supported-devices)
- [Configuration](#configuration)
- [Device Pairing](#device-pairing)
- [MQTT Integration](#mqtt-integration)
- [Presence Detection](#presence-detection)
- [Performance Considerations](#performance-considerations)
- [Troubleshooting](#troubleshooting)

## Overview

The Bluetooth Gateway extends the ESP32-C5 Unified Gateway with full Bluetooth LE support, enabling:

### Passive Scanning (BLE Scanner)
- **iBeacon** tracking for presence detection
- **Eddystone** beacon support
- **RSSI-based** distance estimation
- **Low overhead**: ~5% CPU usage

### Active Connections (BLE Proxy)
- **Xiaomi LYWSD03MMC** temperature/humidity sensors
- **Govee H5075** sensors
- **GATT connections** to BLE sensors
- **Higher overhead**: ~15% CPU usage per active connection

### Key Features
- **Dual Mode**: Passive scanning + active GATT connections
- **MQTT Publishing**: All BLE data published to MQTT topics
- **Home Assistant**: Auto-discovery via MQTT and ESPHome API
- **Coexistence**: Operates alongside WiFi and Zigbee with hardware arbitration
- **Memory Efficient**: BLE device registry offloaded to PSRAM

## Architecture

### BLE Stack Components

```
┌─────────────────────────────────────────────┐
│           Application Layer                  │
│  ┌──────────────┐      ┌──────────────┐    │
│  │ BLE Scanner  │      │  BLE Proxy   │    │
│  │  (Passive)   │      │  (Active)    │    │
│  └──────────────┘      └──────────────┘    │
│         │                      │             │
│         ├──────────────────────┤             │
│         ▼                      ▼             │
│  ┌──────────────────────────────────────┐  │
│  │      BLE Device Handler              │  │
│  │  (Registry, Tracking, State)         │  │
│  └──────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│           Protocol Layer                     │
│  ┌──────────────────────────────────────┐  │
│  │    Bluetooth LE Stack (Nimble)       │  │
│  │  GAP, GATT, SM (Security Manager)    │  │
│  └──────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│      Coexistence Arbitration                │
│  WiFi (5GHz) ◄─► BT (2.4GHz) ◄─► Zigbee     │
└─────────────────────────────────────────────┘
```

### Task Architecture

| Task | Priority | Stack | Purpose |
|------|----------|-------|---------|
| `ble_scan_task` | 5 | 4096 | Passive BLE scanning |
| `ble_proxy_task` | 5 | 4096 | Active GATT connections |
| `bt_device_handler` | 4 | 3072 | Device registry management |

### Memory Footprint

**With Bluetooth Enabled**:
- BLE Stack: ~40 KB
- Device Registry (50 devices): ~20 KB (PSRAM)
- Task Stacks: +10 KB
- **Total Overhead**: ~50 KB SRAM + 20 KB PSRAM

## Supported Devices

### Temperature/Humidity Sensors

#### Xiaomi LYWSD03MMC
- **Type**: Active GATT
- **Measures**: Temperature, Humidity, Battery
- **Update Interval**: 60-120 seconds
- **Range**: ~10 meters
- **Pairing**: Required (via MQTT)
- **Purchase**: ~$5 USD

**MQTT Topic**:
```
zigbee2mqtt/bluetooth/{device_name}
```

**Message Format**:
```json
{
  "temperature": 21.5,
  "humidity": 45.2,
  "battery": 87,
  "rssi": -68,
  "last_seen": "2026-01-23T10:30:20Z"
}
```

#### Govee H5075
- **Type**: Active GATT
- **Measures**: Temperature, Humidity
- **Update Interval**: 30-60 seconds
- **Range**: ~15 meters
- **Pairing**: Auto-discovery
- **Purchase**: ~$10 USD

### Beacons

#### iBeacon
- **Type**: Passive scan
- **Use Case**: Presence detection
- **Update Interval**: Continuous (scan-based)
- **Range**: Configurable (RSSI threshold)
- **Pairing**: Not required

**MQTT Topic**:
```
zigbee2mqtt/bluetooth/ibeacon/{uuid}
```

**Message Format**:
```json
{
  "uuid": "FDA50693-A4E2-4FB1-AFCF-C6EB07647825",
  "major": 100,
  "minor": 1,
  "rssi": -65,
  "distance": 2.5,
  "state": "home",
  "last_seen": "2026-01-23T10:30:30Z"
}
```

#### Eddystone
- **Type**: Passive scan
- **Use Case**: Presence detection, proximity
- **Update Interval**: Continuous
- **Range**: Configurable
- **Pairing**: Not required

### Device Support Matrix

| Device | Type | Temp | Humidity | Battery | Presence | Pairing |
|--------|------|------|----------|---------|----------|---------|
| Xiaomi LYWSD03MMC | Active | ✅ | ✅ | ✅ | ❌ | Required |
| Govee H5075 | Active | ✅ | ✅ | ❌ | ❌ | Auto |
| iBeacon | Passive | ❌ | ❌ | ❌ | ✅ | None |
| Eddystone | Passive | ❌ | ❌ | ❌ | ✅ | None |

## Configuration

### Enable Bluetooth Gateway

```bash
idf.py menuconfig
```

Navigate to: `Bluetooth Gateway Configuration`

#### Essential Settings

**CONFIG_ENABLE_BLUETOOTH_GATEWAY**
- Enable Bluetooth LE Gateway functionality
- Default: `false` (Zigbee-only mode)
- **Set to**: `true`

**CONFIG_BT_SCANNER_ENABLED**
- Enable passive BLE scanner
- Default: `true`
- Recommended: `true` (low overhead)

**CONFIG_BT_PROXY_ENABLED**
- Enable active BLE proxy (GATT)
- Default: `true`
- Note: Higher CPU usage (~15% per device)

**CONFIG_BT_MAX_TRACKED_DEVICES**
- Maximum BLE devices to track
- Default: `50`
- Range: 10-100
- Recommendation: 50 for balanced setup

#### Scan Configuration

**CONFIG_BT_SCAN_INTERVAL_MS**
- BLE scan interval
- Default: `1000` ms
- Range: 100-10000 ms
- **Presence detection**: 1000ms
- **Low power**: 2000-5000ms

**CONFIG_BT_SCAN_WINDOW_MS**
- BLE scan window duration
- Default: `100` ms
- Must be ≤ scan interval
- Recommendation: 10% of interval

**CONFIG_BT_RSSI_THRESHOLD**
- Minimum RSSI for detection
- Default: `-80` dBm
- Range: -100 to -30
- **Close range**: -50 to -70 dBm
- **Long range**: -80 to -90 dBm

### Coexistence Configuration

**CONFIG_WIFI_PREFER_5GHZ**
- **CRITICAL**: Set to `true`
- Frees 2.4 GHz for BT and Zigbee
- Fallback to 2.4 GHz if unavailable

**CONFIG_COEXISTENCE_MODE**
- Hardware arbitration mode
- Default: `1` (Balanced)
- Options: 0=WiFi, 1=Balanced, 2=BT, 3=Zigbee

**CONFIG_COEXISTENCE_BT_SCAN_DUTY_CYCLE**
- BT scan duty cycle during coexistence
- Default: `50`%
- Range: 10-90%
- Lower = more time for WiFi/Zigbee

## Device Pairing

### Passive Devices (Beacons)

No pairing required. Devices automatically discovered when in range:

```bash
# Watch for discoveries
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bridge/event"

# Output:
{
  "type": "bluetooth_discovered",
  "data": {
    "mac": "E7:2E:00:AB:CD:EF",
    "device_type": "ibeacon",
    "rssi": -72
  }
}
```

### Active Devices (Sensors)

#### Xiaomi LYWSD03MMC Pairing

1. **Ensure device is advertising**:
   - Remove and reinsert battery
   - Device should show on LCD

2. **Pair via MQTT**:
```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bluetooth/request/pair" -m '{
  "device_type": "xiaomi_lywsd03mmc",
  "mac": "A4:C1:38:12:34:56",
  "friendly_name": "bedroom_temp"
}'
```

3. **Verify pairing**:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bedroom_temp"

# Should start receiving data within 60 seconds
{
  "temperature": 21.5,
  "humidity": 45.2,
  "battery": 87
}
```

#### Govee H5075 Pairing

Auto-discovered and paired automatically. No manual pairing required.

## MQTT Integration

### Topic Structure

All Bluetooth devices publish under `zigbee2mqtt/bluetooth/` namespace:

```
zigbee2mqtt/bluetooth/
├── bridge/
│   ├── state              # BLE bridge status
│   ├── devices            # List of BLE devices
│   └── event              # Discovery/connection events
├── request/
│   ├── pair               # Pair device
│   ├── unpair             # Unpair device
│   └── scan               # Start/stop scan
├── {device_name}          # Active sensor data
└── ibeacon/{uuid}         # iBeacon presence
```

### Bridge Status

```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bridge/state"
```

Response:
```json
{
  "state": "online",
  "scanner_active": true,
  "proxy_active": true,
  "tracked_devices": 12
}
```

### Device List

```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bridge/devices"
```

Response:
```json
[
  {
    "mac": "A4:C1:38:12:34:56",
    "friendly_name": "bedroom_temp",
    "device_type": "xiaomi_lywsd03mmc",
    "rssi": -68,
    "last_seen": "2026-01-23T10:30:15Z",
    "battery": 87
  }
]
```

## Presence Detection

### iBeacon-Based Presence

iBeacons can trigger presence automations in Home Assistant:

**Use Cases**:
- Room occupancy detection
- Arrival/departure triggers
- Proximity-based actions

**Configuration**:
```bash
idf.py menuconfig
# Bluetooth Gateway → RSSI Threshold: -75 (room-level detection)
# Bluetooth Gateway → Device Timeout: 300 seconds
```

**MQTT Topic**:
```
zigbee2mqtt/bluetooth/ibeacon/{uuid}
```

**States**:
- `state: "home"` - Device detected (RSSI > threshold)
- `state: "away"` - Device not seen for timeout period

**Home Assistant Automation Example**:
```yaml
automation:
  - alias: "Welcome Home"
    trigger:
      platform: mqtt
      topic: zigbee2mqtt/bluetooth/ibeacon/FDA50693-A4E2-4FB1-AFCF-C6EB07647825
    condition:
      condition: template
      value_template: "{{ trigger.payload_json.state == 'home' }}"
    action:
      service: light.turn_on
      target:
        entity_id: light.hallway
```

## Performance Considerations

### CPU Usage

**Baseline (Zigbee-only)**: 38%
**With BLE Scanner**: 43% (+5%)
**With 5 Active GATT**: 58% (+20%)
**With 10 Active GATT**: 73% (+35%)

**Recommendations**:
- Limit active GATT connections to 5-10 devices
- Use passive scanning for presence detection
- Increase scan interval if CPU usage high

### Memory Usage

**Free Heap**:
- Zigbee-only: 120 KB
- Zigbee + BT: 60 KB (⚠️ WARNING threshold)

**Device Limits**:
- Zigbee: 50 → 30 devices (with BT enabled)
- Bluetooth: 50 devices (PSRAM)

### Power Consumption

**Current Draw**:
- Zigbee-only: 150-250 mA
- Zigbee + BT: 200-300 mA

**Optimization**:
- Use 5 GHz WiFi (reduces 2.4 GHz activity)
- Increase BT scan interval
- Reduce scan duty cycle

## Troubleshooting

### BLE Scanner Not Discovering Devices

**Check**:
```bash
idf.py monitor
# Look for: I (xxx) BLE_SCANNER: BLE scanner started
```

**Solutions**:
1. Enable BLE scanner in menuconfig
2. Lower RSSI threshold: `-90` dBm
3. Verify Bluetooth stack initialized (check logs)

### Cannot Connect to Xiaomi Sensor

**Common Causes**:
- Device already paired to Mi Home app
- Device out of range (RSSI < -90)
- Too many active GATT connections

**Solutions**:
1. Unbind from Mi Home app
2. Move device closer (RSSI > -70)
3. Reduce `CONFIG_BT_MAX_TRACKED_DEVICES`

### High Packet Loss with BT Enabled

**Check coexistence**:
```bash
idf.py menuconfig
# WiFi Coexistence → Prefer 5GHz: [*]
# WiFi Coexistence → Coexistence Mode: 1 (Balanced)
```

**Solutions**:
1. Use WiFi 5 GHz (CRITICAL)
2. Reduce BT scan duty cycle: 30%
3. Change Zigbee channel: 20 or 25

## Next Steps

- [ESPHome API Guide](ESPHOME_API.md) - ESPHome integration
- [Coexistence Guide](COEXISTENCE.md) - WiFi/BT/Zigbee optimization
- [BLE Devices](BLE_DEVICES.md) - Detailed device list
- [Configuration Guide](CONFIGURATION.md) - Full configuration reference
