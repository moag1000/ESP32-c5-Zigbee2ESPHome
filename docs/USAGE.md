# Usage Guide

<!-- staleness-banner -->
> **Stand 2026-08-05.** Die BLE-Erwaehnung betrifft abgeschalteten Code.
>
> Aktuell gepflegt wird `CLAUDE.md` im Projektwurzelverzeichnis.


This guide covers daily operation of the ESP32-C5 Unified Gateway (Zigbee2MQTT + Bluetooth + ESPHome API), including device pairing, MQTT control, Home Assistant integration, and OTA updates.

## Table of Contents

- [Gateway Operation](#gateway-operation)
- [Pairing Zigbee Devices](#pairing-zigbee-devices)
- [Device Control](#device-control)
- [Home Assistant Integration](#home-assistant-integration)
- [MQTT Topics Reference](#mqtt-topics-reference)
- [Bridge Requests](#bridge-requests)
- [OTA Updates](#ota-updates)
- [Monitoring and Diagnostics](#monitoring-and-diagnostics)

## Gateway Operation

### Starting the Gateway

After flashing and initial configuration, the gateway starts automatically on power-up.

**Boot Sequence:**
1. System initialization
2. WiFi connection (5GHz preferred if Bluetooth enabled)
3. MQTT connection
4. Zigbee coordinator start
5. Bluetooth stack initialization 🔵 (if enabled)
6. ESPHome API server start 🔵 (if enabled)
7. MQTT bridge activation
8. Ready for device pairing (Zigbee and Bluetooth)

### Gateway States

| State | Description | LED Indicator |
|-------|-------------|---------------|
| **Starting** | Initializing subsystems | Flashing Yellow |
| **Connecting** | WiFi/MQTT connection | Flashing Yellow |
| **Online** | Fully operational | Solid Green |
| **Pairing** | Accepting new devices | Flashing Blue |
| **Error** | System error | Flashing Red |

### Checking Gateway Status

#### Via MQTT

Subscribe to bridge state:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/state"

# Output: online
```

View detailed info:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/info" -C 1

# Output:
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
    "channel": 11
  },
  "devices": 5,
  "uptime": 3600
}
```

#### Via Serial Monitor

```bash
idf.py monitor

# Or
./scripts/monitor.sh
```

Check logs for system status and errors.

## Pairing Zigbee Devices

### Enable Pairing Mode

#### Method 1: MQTT Command

```bash
# Enable pairing for 254 seconds
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/permit_join" \
  -m '{"value": true}'

# Response on zigbee2mqtt/bridge/response/permit_join:
{
  "data": {
    "value": true,
    "time": 254
  },
  "status": "ok"
}
```

#### Method 2: Timed Pairing

```bash
# Enable pairing for 60 seconds
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/permit_join" \
  -m '{"value": true, "time": 60}'
```

#### Method 3: Disable Pairing

```bash
# Close network to new devices
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/permit_join" \
  -m '{"value": false}'
```

### Pairing Process

1. **Enable pairing mode** (as above)
2. **Put device in pairing mode**:
   - Light bulbs: Power cycle 5-6 times
   - Sensors: Hold button 5-10 seconds
   - Switches: Hold button or follow manufacturer instructions
3. **Wait for device to join** (10-60 seconds)
4. **Verify device joined**:

```bash
# Watch for join event
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/event"

# Output:
{
  "type": "device_joined",
  "data": {
    "friendly_name": "0x00158d0001a2b3c4",
    "ieee_address": "0x00158d0001a2b3c4"
  }
}
```

### Supported Device Types

| Device Type | Examples | Features |
|-------------|----------|----------|
| **Lights** | Philips Hue, IKEA Tradfri | On/Off, Brightness, Color |
| **Sensors** | Temperature, Motion, Door | Read sensor values |
| **Switches** | Smart buttons, Remotes | Trigger actions |
| **Plugs** | Smart outlets | On/Off control, Power monitoring |

### Device Naming

After pairing, devices get default names like `0x00158d0001a2b3c4`. Rename for easier identification:

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/device/rename" -m '{
  "from": "0x00158d0001a2b3c4",
  "to": "living_room_light"
}'

# Response:
{
  "data": {
    "from": "0x00158d0001a2b3c4",
    "to": "living_room_light"
  },
  "status": "ok"
}
```

## Pairing Bluetooth Devices 🔵

> **Note**: Bluetooth features available in Phase 10-14. Requires `CONFIG_ENABLE_BLUETOOTH_GATEWAY=y`.

### Bluetooth Device Auto-Discovery

The gateway automatically discovers nearby Bluetooth devices when BLE scanner is enabled:

```bash
# Watch for BLE device discoveries
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bridge/event"

# Output:
{
  "type": "bluetooth_discovered",
  "data": {
    "mac": "A4:C1:38:XX:XX:XX",
    "rssi": -65,
    "device_type": "xiaomi_lywsd03mmc",
    "name": "LYWSD03MMC"
  }
}
```

### Supported Bluetooth Devices

| Device Type | Model | Features | Mode |
|-------------|-------|----------|------|
| **Temperature/Humidity** | Xiaomi LYWSD03MMC | Temp, Humidity, Battery | Active GATT |
| **Temperature/Humidity** | Govee H5075 | Temp, Humidity | Active GATT |
| **Beacons** | iBeacon | Presence detection | Passive scan |
| **Beacons** | Eddystone | Presence detection | Passive scan |

### Pairing Active BLE Devices

For devices requiring GATT connections (Xiaomi, Govee):

```bash
# Pair Xiaomi LYWSD03MMC
mosquitto_pub -h localhost -t "zigbee2mqtt/bluetooth/request/pair" -m '{
  "device_type": "xiaomi_lywsd03mmc",
  "mac": "A4:C1:38:XX:XX:XX",
  "friendly_name": "bedroom_temp_sensor"
}'

# Response:
{
  "status": "ok",
  "data": {
    "mac": "A4:C1:38:XX:XX:XX",
    "paired": true
  }
}
```

### Bluetooth Device States

BLE devices publish to separate topics:

```bash
# Subscribe to all BLE devices
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/#"

# Example output for Xiaomi sensor:
zigbee2mqtt/bluetooth/bedroom_temp_sensor {
  "temperature": 21.5,
  "humidity": 45.2,
  "battery": 87,
  "rssi": -68
}
```

## Device Control

### Controlling Lights

#### Turn On/Off

```bash
# Turn on
mosquitto_pub -h localhost -t "zigbee2mqtt/living_room_light/set" \
  -m '{"state": "ON"}'

# Turn off
mosquitto_pub -h localhost -t "zigbee2mqtt/living_room_light/set" \
  -m '{"state": "OFF"}'

# Toggle
mosquitto_pub -h localhost -t "zigbee2mqtt/living_room_light/set" \
  -m '{"state": "TOGGLE"}'
```

#### Brightness Control

```bash
# Set brightness (0-255)
mosquitto_pub -h localhost -t "zigbee2mqtt/living_room_light/set" \
  -m '{"state": "ON", "brightness": 128}'

# Dim to 25%
mosquitto_pub -h localhost -t "zigbee2mqtt/living_room_light/set" \
  -m '{"brightness": 64}'
```

#### Color Control

```bash
# Set color (hue/saturation)
mosquitto_pub -h localhost -t "zigbee2mqtt/living_room_light/set" \
  -m '{"color": {"hue": 120, "saturation": 100}}'

# Set color temperature (warm to cool: 150-500)
mosquitto_pub -h localhost -t "zigbee2mqtt/living_room_light/set" \
  -m '{"color_temp": 250}'

# Set RGB color
mosquitto_pub -h localhost -t "zigbee2mqtt/living_room_light/set" \
  -m '{"color": {"r": 255, "g": 0, "b": 0}}'
```

### Reading Sensor Data

Subscribe to sensor updates:

```bash
# Temperature sensor
mosquitto_sub -h localhost -t "zigbee2mqtt/bedroom_sensor"

# Output (periodic updates):
{
  "temperature": 22.5,
  "humidity": 45.2,
  "battery": 95,
  "linkquality": 120
}
```

### Controlling Switches/Plugs

```bash
# Turn on smart plug
mosquitto_pub -h localhost -t "zigbee2mqtt/kitchen_plug/set" \
  -m '{"state": "ON"}'

# Turn off
mosquitto_pub -h localhost -t "zigbee2mqtt/kitchen_plug/set" \
  -m '{"state": "OFF"}'
```

### Device State Monitoring

Subscribe to all devices:

```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/#" -v

# Shows all device states and updates
```

Subscribe to specific device:

```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/living_room_light"
```

## Home Assistant Integration

The gateway integrates with Home Assistant through two methods:
1. **MQTT Discovery** - For Zigbee and Bluetooth devices (via MQTT)
2. **ESPHome Native API** 🔵 - For the gateway itself as an ESPHome device

### Method 1: MQTT Discovery (Zigbee + Bluetooth Devices)

The gateway supports Home Assistant MQTT discovery for automatic device integration.

#### Enable Discovery

Already enabled by default. Verify in configuration:

```bash
idf.py menuconfig
# Gateway Configuration → Enable Home Assistant Discovery: [*]
```

Or via MQTT:

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
  "ha_discovery_enabled": true
}'
```

### Adding Gateway to Home Assistant

#### 1. Configure MQTT Integration

In Home Assistant `configuration.yaml`:

```yaml
mqtt:
  broker: localhost  # or IP of MQTT broker
  port: 1883
  username: !secret mqtt_username  # if required
  password: !secret mqtt_password  # if required
  discovery: true
  discovery_prefix: homeassistant
```

#### 2. Restart Home Assistant

```bash
# In Home Assistant
Developer Tools → YAML Configuration → Restart
```

#### 3. Devices Appear Automatically

- Navigate to: **Settings** → **Devices & Services** → **MQTT**
- Devices should appear automatically after pairing

### Manual Device Addition (if needed)

If automatic discovery doesn't work, add devices manually:

#### Light Example

```yaml
# configuration.yaml
light:
  - platform: mqtt
    name: "Living Room Light"
    state_topic: "zigbee2mqtt/living_room_light"
    command_topic: "zigbee2mqtt/living_room_light/set"
    brightness_state_topic: "zigbee2mqtt/living_room_light"
    brightness_command_topic: "zigbee2mqtt/living_room_light/set"
    brightness_scale: 255
    payload_on: "ON"
    payload_off: "OFF"
    json_attributes_topic: "zigbee2mqtt/living_room_light"
    schema: json
```

#### Sensor Example

```yaml
# configuration.yaml
sensor:
  - platform: mqtt
    name: "Bedroom Temperature"
    state_topic: "zigbee2mqtt/bedroom_sensor"
    unit_of_measurement: "°C"
    value_template: "{{ value_json.temperature }}"

  - platform: mqtt
    name: "Bedroom Humidity"
    state_topic: "zigbee2mqtt/bedroom_sensor"
    unit_of_measurement: "%"
    value_template: "{{ value_json.humidity }}"
```

### Method 2: ESPHome Native API Integration 🔵

> **Note**: Available in Phase 10-14. Requires `CONFIG_ESPHOME_API_ENABLED=y`.

The gateway can also appear as an ESPHome device in Home Assistant, providing:
- Gateway diagnostics (uptime, memory, WiFi signal)
- Device tracker entities for BLE presence detection
- Gateway control switches

#### Adding Gateway as ESPHome Device

1. **Enable ESPHome API** (if not already):

```bash
idf.py menuconfig
# ESPHome API Configuration → Enable ESPHome API: [*]
# ESPHome API Configuration → Device Name: esp32-c5-unified-gateway
# ESPHome API Configuration → Friendly Name: ESP32-C5 Unified Gateway
```

2. **In Home Assistant**:
   - Navigate to: **Settings** → **Devices & Services** → **Add Integration**
   - Search for: **ESPHome**
   - Enter gateway IP address (e.g., `192.168.1.100`)
   - Enter API password (if configured)
   - Click **Submit**

3. **Gateway appears with entities**:
   - **Sensor**: `sensor.esp32_c5_unified_gateway_uptime`
   - **Sensor**: `sensor.esp32_c5_unified_gateway_free_memory`
   - **Sensor**: `sensor.esp32_c5_unified_gateway_wifi_signal`
   - **Device Tracker**: `device_tracker.bedroom_phone` (BLE presence)
   - **Switch**: `switch.esp32_c5_unified_gateway_permit_join`

#### ESPHome YAML (Reference Only)

The gateway implements ESPHome Native API protocol but does NOT use ESPHome framework. However, it presents itself as:

```yaml
# Equivalent ESPHome configuration (for reference)
esphome:
  name: esp32-c5-unified-gateway
  friendly_name: ESP32-C5 Unified Gateway

api:
  encryption:
    key: "your_api_password"

wifi:
  ssid: "YourSSID"
  password: "YourPassword"

sensor:
  - platform: uptime
    name: "Uptime"
  - platform: template
    name: "Free Memory"
    lambda: 'return esp_get_free_heap_size();'
```

### Home Assistant Automations

#### Motion-Activated Light

```yaml
# automations.yaml
automation:
  - alias: "Motion Light On"
    trigger:
      platform: mqtt
      topic: zigbee2mqtt/hallway_motion
      payload: '{"occupancy": true}'
    action:
      service: light.turn_on
      target:
        entity_id: light.hallway_light

  - alias: "Motion Light Off"
    trigger:
      platform: mqtt
      topic: zigbee2mqtt/hallway_motion
      payload: '{"occupancy": false}'
    action:
      service: light.turn_off
      target:
        entity_id: light.hallway_light
```

#### Temperature-Based Fan Control

```yaml
automation:
  - alias: "Fan On When Hot"
    trigger:
      platform: mqtt
      topic: zigbee2mqtt/bedroom_sensor
    condition:
      condition: template
      value_template: "{{ trigger.payload_json.temperature > 25 }}"
    action:
      service: switch.turn_on
      target:
        entity_id: switch.bedroom_fan
```

### Home Assistant Dashboard

Create a dashboard for Zigbee devices:

```yaml
# ui-lovelace.yaml
views:
  - title: Zigbee Devices
    cards:
      - type: entities
        title: Lights
        entities:
          - light.living_room_light
          - light.bedroom_light
          - light.kitchen_light

      - type: entities
        title: Sensors
        entities:
          - sensor.bedroom_temperature
          - sensor.bedroom_humidity
          - binary_sensor.door_sensor

      - type: button
        name: Enable Pairing
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: zigbee2mqtt/bridge/request/permit_join
            payload: '{"value": true}'
```

## MQTT Topics Reference

### Bridge Topics

| Topic | Direction | Description |
|-------|-----------|-------------|
| `zigbee2mqtt/bridge/state` | Publish | Bridge online/offline state |
| `zigbee2mqtt/bridge/info` | Publish | Bridge information and stats |
| `zigbee2mqtt/bridge/devices` | Publish | List of all devices |
| `zigbee2mqtt/bridge/event` | Publish | Bridge events (join, leave) |
| `zigbee2mqtt/bridge/logging` | Publish | Log messages (if enabled) |
| `zigbee2mqtt/bridge/request/#` | Subscribe | Bridge command requests |
| `zigbee2mqtt/bridge/response/#` | Publish | Command responses |

### Device Topics

| Topic Pattern | Direction | Description |
|---------------|-----------|-------------|
| `zigbee2mqtt/{device}` | Publish | Device state updates |
| `zigbee2mqtt/{device}/set` | Subscribe | Device control commands |
| `zigbee2mqtt/{device}/get` | Subscribe | Request device state |
| `zigbee2mqtt/{device}/availability` | Publish | Device online/offline |

### System Topics

| Topic | Direction | Description |
|-------|-----------|-------------|
| `zigbee2mqtt/bridge/system` | Publish | System statistics |
| `zigbee2mqtt/bridge/health` | Publish | Health check results |
| `zigbee2mqtt/bridge/config` | Publish | Configuration info |

## Bridge Requests

Bridge requests are sent to `zigbee2mqtt/bridge/request/{command}` topics.

### Available Commands

#### permit_join

Enable/disable device pairing:

```bash
# Enable pairing
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/permit_join" \
  -m '{"value": true, "time": 254}'

# Disable pairing
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/permit_join" \
  -m '{"value": false}'
```

#### device/remove

Remove device from network:

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/device/remove" \
  -m '{"id": "living_room_light"}'

# Or by IEEE address
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/device/remove" \
  -m '{"id": "0x00158d0001a2b3c4"}'
```

#### device/rename

Rename device:

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/device/rename" \
  -m '{"from": "0x00158d0001a2b3c4", "to": "bedroom_light"}'
```

#### health_check

Request system health check:

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/health_check" \
  -m '{}'

# Response on zigbee2mqtt/bridge/response/health_check:
{
  "data": {
    "health": "good",
    "uptime": 3600,
    "free_heap": 85000,
    "wifi_rssi": -45,
    "mqtt_connected": true
  },
  "status": "ok"
}
```

#### restart

Restart gateway:

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/restart" \
  -m '{}'
```

#### config_get

Get current configuration:

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_get" \
  -m '{}'
```

#### config_set

Update configuration:

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
  -m '{"zigbee_channel": 20}'
```

## OTA Updates

### Check for Updates

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/ota_check" \
  -m '{}'

# Response:
{
  "data": {
    "current_version": "v1.0.0",
    "available_version": "v1.1.0",
    "update_available": true
  },
  "status": "ok"
}
```

### Install Update

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/ota_install" \
  -m '{}'

# Gateway will:
# 1. Download firmware
# 2. Verify integrity
# 3. Install update
# 4. Reboot automatically

# Monitor progress:
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/ota/#"
```

### OTA Progress Topics

| Topic | Description |
|-------|-------------|
| `zigbee2mqtt/bridge/ota/state` | OTA state (checking, downloading, installing) |
| `zigbee2mqtt/bridge/ota/progress` | Download progress (0-100%) |

### Rollback Update

If update causes issues:

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/ota_rollback" \
  -m '{}'
```

## Monitoring and Diagnostics

### System Statistics

Subscribe to system stats:

```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/system"

# Output (periodic):
{
  "uptime": 7200,
  "free_heap": 82000,
  "min_free_heap": 75000,
  "wifi_rssi": -42,
  "wifi_connected": true,
  "mqtt_connected": true,
  "zigbee_devices": 8,
  "cpu_usage": 12
}
```

### Device List

Get all registered devices:

```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/devices" -C 1

# Output:
[
  {
    "ieee_address": "0x00158d0001a2b3c4",
    "friendly_name": "living_room_light",
    "type": "Router",
    "model": "TRADFRI bulb E27",
    "manufacturer": "IKEA",
    "supported": true
  },
  ...
]
```

### Network Map (Future Feature)

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/networkmap" \
  -m '{"type": "raw"}'

# Returns network topology
```

### Enable Debug Logging

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
  -m '{"log_level": 4}'  # 4 = Debug level

# Subscribe to logs
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/logging"
```

### Serial Console Monitoring

For low-level debugging:

```bash
idf.py monitor

# Or
./scripts/monitor.sh

# Shows ESP-IDF logs in real-time
```

## Best Practices

### Device Placement

- **Coordinator**: Central location in coverage area
- **Routers**: Powered devices extend network range
- **End devices**: Battery devices can be anywhere in range

### Network Health

- Monitor signal quality (`linkquality` in device messages)
- Ensure powered devices act as routers
- Add router devices to extend range
- Keep devices updated

### MQTT Broker

- Use persistent sessions for reliability
- Set appropriate QoS levels (QoS 1 recommended)
- Monitor broker resource usage
- Use authentication in production

### Security

- Disable pairing mode when not adding devices
- Use strong WiFi/MQTT passwords
- Enable MQTTS for encrypted communication
- Regularly update firmware

## Troubleshooting Quick Reference

| Issue | Quick Fix |
|-------|-----------|
| Device won't pair | Enable pairing mode, power cycle device |
| Device offline | Check batteries, check signal strength |
| Commands not working | Verify MQTT connection, check topic names |
| Poor performance | Check WiFi/Zigbee channel interference |
| Gateway offline | Check power, WiFi credentials, MQTT broker |

For detailed troubleshooting, see [Troubleshooting Guide](TROUBLESHOOTING.md).

## Next Steps

- [Troubleshooting Guide](TROUBLESHOOTING.md) - Solve common issues
- [API Reference](API_REFERENCE.md) - Developer API documentation
- [MQTT Protocol](MQTT_PROTOCOL.md) - Detailed MQTT protocol reference
