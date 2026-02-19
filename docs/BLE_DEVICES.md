# Supported BLE Devices

Comprehensive list of supported Bluetooth LE devices for the ESP32-C5 Unified Gateway.

> **Status**: ✅ Phase 10-14 Complete
> **Requires**: `CONFIG_BT_SCANNER_ENABLED=y`

## Table of Contents

- [Overview](#overview)
- [Temperature & Humidity Sensors](#temperature--humidity-sensors)
- [Beacons](#beacons)
- [Device Compatibility](#device-compatibility)
- [Pairing Instructions](#pairing-instructions)
- [MQTT Topics](#mqtt-topics)
- [Home Assistant Integration](#home-assistant-integration)

## Overview

The gateway supports two categories of BLE devices:

### Active Devices (GATT Connections)
- Require pairing
- Periodic data reads via GATT
- Higher CPU overhead (~15% per device)
- Limited to ~10 concurrent connections

### Passive Devices (Beacon Scanning)
- No pairing required
- Continuous presence detection
- Low CPU overhead (~5% total)
- Unlimited device tracking

## Temperature & Humidity Sensors

### Xiaomi LYWSD03MMC

![Xiaomi LYWSD03MMC](https://via.placeholder.com/150?text=LYWSD03MMC)

**Type**: Active GATT
**Manufacturer**: Xiaomi
**Model**: LYWSD03MMC
**Price**: ~$5 USD

#### Features
- ✅ Temperature (°C/°F)
- ✅ Humidity (%)
- ✅ Battery level
- ✅ E-Ink display
- ✅ Temperature/humidity history
- ⚠️ Requires pairing

#### Specifications
| Parameter | Value |
|-----------|-------|
| Temperature Range | -9.9°C to 60°C |
| Temperature Accuracy | ±0.1°C |
| Humidity Range | 0-99% |
| Humidity Accuracy | ±1% |
| Battery | CR2032 (1 year+) |
| Bluetooth | BLE 4.2 |
| Update Interval | 60-120 seconds |
| Range | ~10 meters |

#### Pairing Instructions

1. **Insert Battery**:
   - Remove battery cover
   - Insert CR2032 battery
   - Display should turn on

2. **Verify Advertising**:
   - Device shows on LCD
   - Bluetooth icon visible

3. **Pair via MQTT**:
```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bluetooth/request/pair" -m '{
  "device_type": "xiaomi_lywsd03mmc",
  "mac": "A4:C1:38:XX:XX:XX",
  "friendly_name": "bedroom_temp"
}'
```

4. **Wait for Response**:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/response/pair"

# Should return:
{
  "status": "ok",
  "data": {
    "mac": "A4:C1:38:XX:XX:XX",
    "paired": true
  }
}
```

5. **Verify Data**:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bedroom_temp"

# Should receive within 60-120 seconds:
{
  "temperature": 21.5,
  "humidity": 45.2,
  "battery": 87,
  "rssi": -68
}
```

#### MQTT Topic
```
zigbee2mqtt/bluetooth/bedroom_temp
```

#### Message Format
```json
{
  "temperature": 21.5,
  "humidity": 45.2,
  "battery": 87,
  "voltage": 3000,
  "rssi": -68,
  "last_seen": "2026-01-23T10:30:20Z"
}
```

#### Home Assistant
```yaml
sensor:
  - platform: mqtt
    name: "Bedroom Temperature"
    state_topic: "zigbee2mqtt/bluetooth/bedroom_temp"
    value_template: "{{ value_json.temperature }}"
    unit_of_measurement: "°C"
    device_class: temperature

  - platform: mqtt
    name: "Bedroom Humidity"
    state_topic: "zigbee2mqtt/bluetooth/bedroom_temp"
    value_template: "{{ value_json.humidity }}"
    unit_of_measurement: "%"
    device_class: humidity
```

#### Troubleshooting

**Problem**: Device not discovered
- **Solution**: Check battery inserted correctly
- **Solution**: Press button to wake device
- **Solution**: Move closer to gateway (< 5m)

**Problem**: Pairing fails
- **Solution**: Remove from Mi Home app first
- **Solution**: Factory reset: hold button 5 seconds
- **Solution**: Check MAC address correct

**Problem**: Data not updating
- **Solution**: Check battery level (replace if < 20%)
- **Solution**: Verify RSSI > -80 dBm
- **Solution**: Restart gateway

---

### Govee H5075

![Govee H5075](https://via.placeholder.com/150?text=H5075)

**Type**: Active GATT
**Manufacturer**: Govee
**Model**: H5075
**Price**: ~$10 USD

#### Features
- ✅ Temperature (°C/°F)
- ✅ Humidity (%)
- ✅ LCD display
- ✅ Mobile app support
- ✅ Auto-discovery (no pairing)
- ✅ 20-day data history

#### Specifications
| Parameter | Value |
|-----------|-------|
| Temperature Range | -20°C to 60°C |
| Temperature Accuracy | ±0.3°C |
| Humidity Range | 0-99% |
| Humidity Accuracy | ±3% |
| Battery | AAA x2 (3-6 months) |
| Bluetooth | BLE 5.0 |
| Update Interval | 30-60 seconds |
| Range | ~15 meters |

#### Setup Instructions

1. **Insert Batteries**:
   - Open battery cover
   - Insert 2x AAA batteries
   - Display should turn on

2. **Auto-Discovery**:
   - Gateway automatically discovers device
   - No pairing required

3. **Verify Discovery**:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bridge/event"

# Look for:
{
  "type": "bluetooth_discovered",
  "data": {
    "mac": "E3:5E:CC:XX:XX:XX",
    "device_type": "govee_h5075",
    "name": "GVH5075_XXXX"
  }
}
```

4. **Name Device** (Optional):
```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bluetooth/request/rename" -m '{
  "mac": "E3:5E:CC:XX:XX:XX",
  "friendly_name": "living_room_temp"
}'
```

#### MQTT Topic
```
zigbee2mqtt/bluetooth/living_room_temp
```

#### Message Format
```json
{
  "temperature": 22.3,
  "humidity": 52.1,
  "battery": 95,
  "rssi": -72,
  "last_seen": "2026-01-23T10:30:25Z"
}
```

---

## Beacons

### iBeacon

![iBeacon](https://via.placeholder.com/150?text=iBeacon)

**Type**: Passive Scan
**Standard**: Apple iBeacon
**Use Case**: Presence detection

#### Features
- ✅ Presence detection
- ✅ RSSI-based distance
- ✅ Room-level tracking
- ✅ No pairing required
- ✅ Low power (1+ year battery)

#### Specifications
| Parameter | Value |
|-----------|-------|
| Standard | iBeacon (Apple) |
| Bluetooth | BLE 4.0+ |
| Broadcast Interval | 100-1000ms (configurable) |
| Range | 1-70 meters (power dependent) |
| Battery | CR2032 / CR2477 (1-3 years) |
| Detection | Continuous (scan-based) |

#### Setup Instructions

1. **Configure iBeacon**:
   - Set UUID (unique identifier)
   - Set Major/Minor values
   - Set transmit power (-20 to +4 dBm)

2. **Enable on Gateway**:
   - No configuration needed
   - Automatically detected

3. **Verify Detection**:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/ibeacon/#"

# Output:
zigbee2mqtt/bluetooth/ibeacon/FDA50693-A4E2-4FB1-AFCF-C6EB07647825
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

#### MQTT Topic
```
zigbee2mqtt/bluetooth/ibeacon/{uuid}
```

#### Message Format
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

**States**:
- `home`: RSSI > threshold (device present)
- `away`: Not seen for timeout period

#### Home Assistant
```yaml
device_tracker:
  - platform: mqtt
    name: "Phone Presence"
    state_topic: "zigbee2mqtt/bluetooth/ibeacon/FDA50693-A4E2-4FB1-AFCF-C6EB07647825"
    value_template: "{{ value_json.state }}"
    payload_home: "home"
    payload_not_home: "away"
```

#### Recommended Beacons
- **Estimote Proximity Beacon**: $25, configurable, long range
- **Chipolo One**: $25, replaceable battery
- **Tile**: $25-35, non-replaceable battery
- **Generic CR2032 beacons**: $5-10, basic functionality

#### Distance Estimation

RSSI to distance formula:
```
distance (meters) = 10 ^ ((measured_power - rssi) / (10 * n))

where:
  measured_power = RSSI at 1 meter (calibrated)
  rssi = current RSSI
  n = path loss exponent (2.0 for free space, 3-4 indoors)
```

**Accuracy**: ±1-3 meters (environmental factors)

---

### Eddystone

**Type**: Passive Scan
**Standard**: Google Eddystone
**Use Case**: Presence detection, URLs

#### Features
- ✅ Multiple frame types (UID, URL, TLM)
- ✅ Presence detection
- ✅ Telemetry data (battery, temperature)
- ✅ No pairing required

#### Specifications
| Parameter | Value |
|-----------|-------|
| Standard | Eddystone (Google) |
| Frame Types | UID, URL, TLM, EID |
| Bluetooth | BLE 4.0+ |
| Range | 1-70 meters |
| Detection | Continuous |

#### MQTT Topic
```
zigbee2mqtt/bluetooth/eddystone/{namespace_id}
```

#### Message Format
```json
{
  "namespace": "EDD1EBEAC04E5DEFA017",
  "instance": "123456789012",
  "rssi": -70,
  "distance": 3.2,
  "state": "home",
  "battery": 2850,
  "temperature": 22,
  "last_seen": "2026-01-23T10:30:35Z"
}
```

---

## Device Compatibility

### Compatibility Matrix

| Device | Type | Temp | Humidity | Battery | Presence | Pairing | CPU | Memory |
|--------|------|------|----------|---------|----------|---------|-----|--------|
| **Xiaomi LYWSD03MMC** | Active | ✅ | ✅ | ✅ | ❌ | Required | 15% | 2KB |
| **Govee H5075** | Active | ✅ | ✅ | ✅ | ❌ | Auto | 15% | 2KB |
| **iBeacon** | Passive | ❌ | ❌ | ❌ | ✅ | None | 5%* | 400B |
| **Eddystone** | Passive | ❌ | ❌ | ❌ | ✅ | None | 5%* | 400B |

*5% total for all passive devices

### Future Support (Roadmap)

See [BACKLOG.md](../BACKLOG.md) for planned device support:
- **BT-003**: Ruuvi Tag sensors (P1, 4-6h)
- **BT-004**: Generic BLE temperature sensors (P2, 6-8h)
- **BT-007**: Switchbot devices (P2, 8-12h)
- **BT-009**: BLE presence detection (multiple beacons) (P1, 6-8h)

## Pairing Instructions

### General Pairing Flow

1. **Discovery**:
```bash
# Enable BLE scanning
mosquitto_pub -h localhost -t "zigbee2mqtt/bluetooth/request/scan" -m '{"enable": true}'

# Watch for devices
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bridge/event"
```

2. **Note MAC Address**:
```json
{
  "type": "bluetooth_discovered",
  "data": {
    "mac": "A4:C1:38:12:34:56",
    "device_type": "xiaomi_lywsd03mmc"
  }
}
```

3. **Pair**:
```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bluetooth/request/pair" -m '{
  "device_type": "xiaomi_lywsd03mmc",
  "mac": "A4:C1:38:12:34:56",
  "friendly_name": "bedroom_temp"
}'
```

4. **Verify**:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bedroom_temp"
```

### Unpairing Devices

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bluetooth/request/unpair" -m '{
  "mac": "A4:C1:38:12:34:56"
}'
```

## MQTT Topics

### Topic Structure

```
zigbee2mqtt/bluetooth/
├── bridge/
│   ├── state
│   ├── devices
│   └── event
├── request/
│   ├── pair
│   ├── unpair
│   ├── rename
│   └── scan
├── {device_name}
├── ibeacon/{uuid}
└── eddystone/{namespace}
```

### Topic Reference

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `bluetooth/bridge/state` | Publish | BLE bridge status |
| `bluetooth/bridge/devices` | Publish | List of BLE devices |
| `bluetooth/bridge/event` | Publish | Discovery, connection events |
| `bluetooth/request/pair` | Subscribe | Pair device |
| `bluetooth/request/unpair` | Subscribe | Unpair device |
| `bluetooth/request/rename` | Subscribe | Rename device |
| `bluetooth/request/scan` | Subscribe | Start/stop scan |
| `bluetooth/{device}` | Publish | Device state (sensors) |
| `bluetooth/ibeacon/{uuid}` | Publish | iBeacon presence |
| `bluetooth/eddystone/{ns}` | Publish | Eddystone presence |

## Home Assistant Integration

### Auto-Discovery

Devices automatically discovered via MQTT Discovery:

```yaml
# Auto-generated by gateway
sensor:
  - platform: mqtt
    name: "Bedroom Temperature"
    state_topic: "zigbee2mqtt/bluetooth/bedroom_temp"
    value_template: "{{ value_json.temperature }}"
    unit_of_measurement: "°C"
    device_class: temperature
    unique_id: "bt_bedroom_temp_temperature"
```

### Manual Configuration

If auto-discovery fails:

```yaml
# configuration.yaml
sensor:
  - platform: mqtt
    name: "Bedroom Temperature"
    state_topic: "zigbee2mqtt/bluetooth/bedroom_temp"
    value_template: "{{ value_json.temperature }}"
    unit_of_measurement: "°C"
    device_class: temperature

  - platform: mqtt
    name: "Bedroom Humidity"
    state_topic: "zigbee2mqtt/bluetooth/bedroom_temp"
    value_template: "{{ value_json.humidity }}"
    unit_of_measurement: "%"
    device_class: humidity

  - platform: mqtt
    name: "Bedroom Sensor Battery"
    state_topic: "zigbee2mqtt/bluetooth/bedroom_temp"
    value_template: "{{ value_json.battery }}"
    unit_of_measurement: "%"
    device_class: battery

device_tracker:
  - platform: mqtt
    name: "Phone Presence"
    state_topic: "zigbee2mqtt/bluetooth/ibeacon/FDA50693-A4E2-4FB1-AFCF-C6EB07647825"
    value_template: "{{ value_json.state }}"
    payload_home: "home"
    payload_not_home: "away"
```

## Next Steps

- [Bluetooth Gateway Guide](BLUETOOTH_GATEWAY.md) - BLE gateway setup
- [ESPHome API Guide](ESPHOME_API.md) - ESPHome integration
- [Coexistence Guide](COEXISTENCE.md) - WiFi/BT/Zigbee optimization
- [Usage Guide](USAGE.md) - Daily operation
