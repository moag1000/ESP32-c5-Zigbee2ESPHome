# MQTT Protocol Reference

Complete reference for MQTT topics, message formats, and protocols used by the ESP32-C5 Unified Gateway (Zigbee2MQTT + Bluetooth).

## Table of Contents

- [Topic Structure](#topic-structure)
- [Bridge Topics](#bridge-topics)
- [Device Topics](#device-topics)
- [Message Formats](#message-formats)
- [Home Assistant Discovery](#home-assistant-discovery)
- [Examples](#examples)

## Topic Structure

### Base Topic

Default: `zigbee2mqtt`

All topics use this base prefix. Configurable via `CONFIG_MQTT_BASE_TOPIC`.

### Topic Hierarchy

```
zigbee2mqtt/
├── bridge/
│   ├── state                    # Bridge online/offline
│   ├── info                     # Bridge information
│   ├── devices                  # Device list (Zigbee)
│   ├── event                    # Bridge events
│   ├── logging                  # Log messages
│   ├── system                   # System statistics
│   ├── request/
│   │   ├── permit_join          # Enable Zigbee pairing
│   │   ├── device/remove        # Remove device
│   │   ├── device/rename        # Rename device
│   │   ├── health_check         # System health
│   │   ├── restart              # Restart gateway
│   │   ├── config_get           # Get configuration
│   │   ├── config_set           # Set configuration
│   │   ├── ota_check            # Check for updates
│   │   └── ota_install          # Install update
│   └── response/
│       ├── permit_join          # Permit join response
│       ├── device/remove        # Remove response
│       └── ...                  # Corresponding responses
│
├── bluetooth/ 🔵                # Bluetooth namespace (Phase 10-14)
│   ├── bridge/
│   │   ├── state                # BLE bridge state
│   │   ├── devices              # BLE device list
│   │   └── event                # BLE events (discovered, connected)
│   ├── request/
│   │   ├── pair                 # Pair BLE device
│   │   ├── unpair               # Unpair BLE device
│   │   └── scan                 # Start/stop scan
│   ├── {ble_device_name}        # BLE device state
│   └── ibeacon/{uuid}           # iBeacon presence
│
├── {zigbee_device_name}         # Zigbee device state (publish)
├── {zigbee_device_name}/set     # Zigbee device commands (subscribe)
├── {zigbee_device_name}/get     # Request device state (subscribe)
└── {zigbee_device_name}/availability  # Device availability
```

## Bridge Topics

### bridge/state

**Direction**: Publish (retained)
**QoS**: 1

**Payload**:
```
online
```
or
```
offline
```

**Description**: Indicates bridge connection status. Published as LWT (Last Will and Testament).

### bridge/info

**Direction**: Publish (retained)
**QoS**: 1

**Payload**:
```json
{
  "version": "v1.0.0",
  "coordinator": {
    "type": "ESP32-C5",
    "ieee_address": "0x00124b0024xxxxxx",
    "meta": {
      "pan_id": "0x1A62",
      "channel": 11,
      "permit_join": false
    }
  },
  "network": {
    "pan_id": "0x1A62",
    "channel": 11,
    "extended_pan_id": "0xdddddddddddddddd"
  },
  "devices": 8,
  "uptime": 3600
}
```

**Published**: On startup, every 5 minutes

### bridge/devices

**Direction**: Publish (retained)
**QoS**: 1

**Payload**:
```json
[
  {
    "ieee_address": "0x00158d0001a2b3c4",
    "friendly_name": "living_room_light",
    "type": "Router",
    "network_address": 4660,
    "supported": true,
    "definition": {
      "model": "TRADFRI bulb E27",
      "vendor": "IKEA",
      "description": "TRADFRI LED bulb E27 806 lumen, dimmable, white spectrum",
      "exposes": [
        {
          "type": "light",
          "features": ["state", "brightness", "color_temp"]
        }
      ]
    },
    "power_source": "Mains (single phase)",
    "date_code": "20210215"
  }
]
```

**Published**: On startup, when devices join/leave

### bridge/event

**Direction**: Publish
**QoS**: 1

**Payloads**:

**Device Joined**:
```json
{
  "type": "device_joined",
  "data": {
    "friendly_name": "0x00158d0001a2b3c4",
    "ieee_address": "0x00158d0001a2b3c4"
  }
}
```

**Device Left**:
```json
{
  "type": "device_leave",
  "data": {
    "ieee_address": "0x00158d0001a2b3c4",
    "friendly_name": "living_room_light"
  }
}
```

**Device Announced**:
```json
{
  "type": "device_announce",
  "data": {
    "ieee_address": "0x00158d0001a2b3c4",
    "friendly_name": "living_room_light"
  }
}
```

### bridge/system

**Direction**: Publish
**QoS**: 1

**Payload**:
```json
{
  "uptime": 7200,
  "uptime_formatted": "2h 0m 0s",
  "free_heap": 82000,
  "min_free_heap": 75000,
  "free_psram": 7900000,
  "heap_fragmentation": 8,
  "wifi_connected": true,
  "wifi_rssi": -42,
  "mqtt_connected": true,
  "zigbee_devices": 8,
  "cpu_usage": 12,
  "health": "good"
}
```

**Published**: Every 5 minutes (configurable)

### bridge/request/permit_join

**Direction**: Subscribe
**QoS**: 1

**Request**:
```json
{
  "value": true,
  "time": 254
}
```

**Parameters**:
- `value`: `true` to enable, `false` to disable
- `time`: Duration in seconds (0-254, optional, default: 254)

**Response** on `bridge/response/permit_join`:
```json
{
  "data": {
    "value": true,
    "time": 254
  },
  "status": "ok"
}
```

### bridge/request/device/remove

**Direction**: Subscribe
**QoS**: 1

**Request**:
```json
{
  "id": "living_room_light"
}
```
or
```json
{
  "id": "0x00158d0001a2b3c4"
}
```

**Response**:
```json
{
  "data": {
    "id": "living_room_light",
    "ieee_address": "0x00158d0001a2b3c4"
  },
  "status": "ok"
}
```

**Error Response**:
```json
{
  "data": {},
  "status": "error",
  "error": "Device not found"
}
```

### bridge/request/device/rename

**Direction**: Subscribe
**QoS**: 1

**Request**:
```json
{
  "from": "0x00158d0001a2b3c4",
  "to": "bedroom_light"
}
```

**Response**:
```json
{
  "data": {
    "from": "0x00158d0001a2b3c4",
    "to": "bedroom_light"
  },
  "status": "ok"
}
```

### bridge/request/health_check

**Direction**: Subscribe
**QoS**: 1

**Request**:
```json
{}
```

**Response**:
```json
{
  "data": {
    "health": "good",
    "uptime": 3600,
    "free_heap": 85000,
    "wifi_rssi": -45,
    "mqtt_connected": true,
    "zigbee_devices": 8,
    "checks": {
      "memory": "ok",
      "wifi": "ok",
      "mqtt": "ok",
      "zigbee": "ok"
    }
  },
  "status": "ok"
}
```

## Bluetooth Topics 🔵

> **Note**: Available in Phase 10-14. Requires `CONFIG_ENABLE_BLUETOOTH_GATEWAY=y`.

### zigbee2mqtt/bluetooth/bridge/state

**Direction**: Publish
**QoS**: 1

```json
{
  "state": "online",
  "scanner_active": true,
  "proxy_active": true,
  "tracked_devices": 12
}
```

### zigbee2mqtt/bluetooth/bridge/devices

**Direction**: Publish
**QoS**: 1

```json
[
  {
    "mac": "A4:C1:38:12:34:56",
    "friendly_name": "bedroom_temp",
    "device_type": "xiaomi_lywsd03mmc",
    "rssi": -68,
    "last_seen": "2026-01-23T10:30:15Z",
    "battery": 87
  },
  {
    "mac": "E7:2E:00:AB:CD:EF",
    "friendly_name": "living_room_beacon",
    "device_type": "ibeacon",
    "rssi": -72,
    "last_seen": "2026-01-23T10:30:18Z"
  }
]
```

### zigbee2mqtt/bluetooth/bridge/event

**Direction**: Publish
**QoS**: 1

**Device Discovered**:
```json
{
  "type": "bluetooth_discovered",
  "data": {
    "mac": "A4:C1:38:12:34:56",
    "rssi": -65,
    "device_type": "xiaomi_lywsd03mmc",
    "name": "LYWSD03MMC"
  }
}
```

**Device Connected**:
```json
{
  "type": "bluetooth_connected",
  "data": {
    "mac": "A4:C1:38:12:34:56",
    "friendly_name": "bedroom_temp"
  }
}
```

### zigbee2mqtt/bluetooth/request/pair

**Direction**: Subscribe
**QoS**: 1

```json
{
  "device_type": "xiaomi_lywsd03mmc",
  "mac": "A4:C1:38:12:34:56",
  "friendly_name": "bedroom_temp"
}
```

### zigbee2mqtt/bluetooth/{device_name}

**Direction**: Publish
**QoS**: 1

**Xiaomi LYWSD03MMC**:
```json
{
  "temperature": 21.5,
  "humidity": 45.2,
  "battery": 87,
  "rssi": -68,
  "last_seen": "2026-01-23T10:30:20Z"
}
```

**Govee H5075**:
```json
{
  "temperature": 22.3,
  "humidity": 52.1,
  "battery": 95,
  "rssi": -72,
  "last_seen": "2026-01-23T10:30:25Z"
}
```

### zigbee2mqtt/bluetooth/ibeacon/{uuid}

**Direction**: Publish
**QoS**: 1

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

## Zigbee Device Topics

### {device}/state (Publish)

**Direction**: Publish
**QoS**: 1

**Light Example**:
```json
{
  "state": "ON",
  "brightness": 128,
  "color_temp": 250,
  "linkquality": 120,
  "last_seen": "2026-01-23T10:30:00Z"
}
```

**Sensor Example**:
```json
{
  "temperature": 22.5,
  "humidity": 45.2,
  "battery": 95,
  "voltage": 3000,
  "linkquality": 105,
  "last_seen": "2026-01-23T10:30:05Z"
}
```

**Switch/Plug Example**:
```json
{
  "state": "ON",
  "power": 15.5,
  "energy": 0.25,
  "linkquality": 130,
  "last_seen": "2026-01-23T10:30:10Z"
}
```

### {device}/set (Subscribe)

**Direction**: Subscribe
**QoS**: 1

**Light Control**:
```json
{
  "state": "ON",
  "brightness": 200,
  "color_temp": 300,
  "transition": 1
}
```

**Switch Control**:
```json
{
  "state": "OFF"
}
```

**Toggle**:
```json
{
  "state": "TOGGLE"
}
```

**Response**: Updated state published to `{device}` topic

### {device}/get (Subscribe)

**Direction**: Subscribe
**QoS**: 1

**Request**:
```json
{}
```
or
```json
{
  "state": ""
}
```

**Response**: Current state published to `{device}` topic

### {device}/availability

**Direction**: Publish (retained)
**QoS**: 1

**Payload**:
```
online
```
or
```
offline
```

**Description**: Device reachability status

## Message Formats

### Standard Response Format

All bridge requests follow this response format:

**Success**:
```json
{
  "data": { /* response data */ },
  "status": "ok"
}
```

**Error**:
```json
{
  "data": {},
  "status": "error",
  "error": "Error description"
}
```

### Device State Schema

Common fields across devices:

| Field | Type | Description |
|-------|------|-------------|
| `state` | string | "ON", "OFF", "TOGGLE" |
| `brightness` | number | 0-255 |
| `color_temp` | number | 150-500 (mireds) |
| `color` | object | `{"hue": 0-360, "saturation": 0-100}` or `{"r": 0-255, "g": 0-255, "b": 0-255}` |
| `linkquality` | number | 0-255 (signal strength) |
| `last_seen` | string | ISO8601 timestamp |
| `battery` | number | 0-100 (percentage) |
| `voltage` | number | Voltage in mV |
| `power` | number | Power consumption in watts |
| `energy` | number | Energy consumption in kWh |

## Home Assistant Discovery

### Discovery Topic Format

```
homeassistant/{component}/{node_id}/{object_id}/config
```

Example:
```
homeassistant/light/esp32c5_gateway/living_room_light/config
```

### Light Discovery

```json
{
  "availability": [
    {
      "topic": "zigbee2mqtt/bridge/state",
      "value_template": "{{ value }}"
    },
    {
      "topic": "zigbee2mqtt/living_room_light/availability",
      "value_template": "{{ value }}"
    }
  ],
  "availability_mode": "all",
  "brightness": true,
  "brightness_scale": 255,
  "color_mode": true,
  "command_topic": "zigbee2mqtt/living_room_light/set",
  "device": {
    "identifiers": ["zigbee2mqtt_0x00158d0001a2b3c4"],
    "manufacturer": "IKEA",
    "model": "TRADFRI bulb E27",
    "name": "living_room_light",
    "sw_version": "2.3.014",
    "via_device": "zigbee2mqtt_bridge"
  },
  "name": "Living Room Light",
  "object_id": "living_room_light",
  "schema": "json",
  "state_topic": "zigbee2mqtt/living_room_light",
  "supported_color_modes": ["color_temp"],
  "unique_id": "0x00158d0001a2b3c4_light_zigbee2mqtt"
}
```

### Sensor Discovery

```json
{
  "availability": [
    {
      "topic": "zigbee2mqtt/bridge/state"
    },
    {
      "topic": "zigbee2mqtt/bedroom_sensor/availability"
    }
  ],
  "availability_mode": "all",
  "device": {
    "identifiers": ["zigbee2mqtt_0x00158d0002b3c4d5"],
    "manufacturer": "Xiaomi",
    "model": "Temperature and Humidity Sensor",
    "name": "bedroom_sensor",
    "via_device": "zigbee2mqtt_bridge"
  },
  "device_class": "temperature",
  "name": "Bedroom Temperature",
  "state_class": "measurement",
  "state_topic": "zigbee2mqtt/bedroom_sensor",
  "unique_id": "0x00158d0002b3c4d5_temperature_zigbee2mqtt",
  "unit_of_measurement": "°C",
  "value_template": "{{ value_json.temperature }}"
}
```

## Examples

### Example 1: Turn On Light

```bash
# Publish command
mosquitto_pub -h localhost -t "zigbee2mqtt/living_room_light/set" \
  -m '{"state":"ON","brightness":200}'

# Gateway receives command
# Gateway sends Zigbee command to device
# Device responds

# Gateway publishes state
# Topic: zigbee2mqtt/living_room_light
# Payload: {"state":"ON","brightness":200,"linkquality":120}
```

### Example 2: Device Pairing

```bash
# 1. Enable pairing
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/permit_join" \
  -m '{"value":true}'

# Response:
# Topic: zigbee2mqtt/bridge/response/permit_join
# Payload: {"data":{"value":true,"time":254},"status":"ok"}

# 2. Put device in pairing mode (device-specific)

# 3. Device joins network
# Topic: zigbee2mqtt/bridge/event
# Payload: {"type":"device_joined","data":{"friendly_name":"0x00158d0001a2b3c4",...}}

# 4. Device list updated
# Topic: zigbee2mqtt/bridge/devices
# Payload: [array of all devices including new one]

# 5. Home Assistant discovery (if enabled)
# Topic: homeassistant/light/esp32c5_gateway/0x00158d0001a2b3c4/config
# Payload: {HA discovery JSON}
```

### Example 3: Monitor All Events

```bash
# Subscribe to all topics
mosquitto_sub -h localhost -t "zigbee2mqtt/#" -v

# Output shows all gateway activity:
# zigbee2mqtt/bridge/state online
# zigbee2mqtt/bridge/devices [...]
# zigbee2mqtt/living_room_light {"state":"ON",...}
# zigbee2mqtt/bedroom_sensor {"temperature":22.5,...}
```

### Example 4: Health Check

```bash
# Request health check
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/health_check" -m '{}'

# Subscribe to response
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/response/health_check" -C 1

# Output:
# {"data":{"health":"good","uptime":3600,...},"status":"ok"}
```

### Example 5: Configuration Change

```bash
# Get current config
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_get" -m '{}'

# Change Zigbee channel
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
  -m '{"zigbee_channel":20}'

# Restart to apply
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/restart" -m '{}'
```

## QoS Recommendations

| Topic Type | QoS | Retained | Reason |
|------------|-----|----------|--------|
| State updates | 1 | No | Guaranteed delivery, latest value |
| Commands | 1 | No | Must not miss commands |
| Bridge state | 1 | Yes | Show last known state |
| Availability | 1 | Yes | Show last known availability |
| Discovery | 0 | Yes | One-time configuration |
| Events | 1 | No | Important notifications |
| Logs | 0 | No | High volume, best effort |

## MQTT Best Practices

1. **Use QoS 1** for important messages
2. **Retain** state messages for client reconnection
3. **Use LWT** for offline detection
4. **Subscribe with wildcards** (`#`) sparingly
5. **Keep payloads small** (<4KB)
6. **Use JSON** for structured data
7. **Include timestamps** in state messages
8. **Handle disconnections** gracefully
9. **Rate limit** publications to avoid broker overload
10. **Use unique client IDs** per gateway

## Related Documentation

- [Usage Guide](USAGE.md) - Using MQTT topics
- [Configuration](CONFIGURATION.md) - MQTT broker configuration
- [API Reference](API_REFERENCE.md) - MQTT client API
