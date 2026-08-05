# Configuration Guide

<!-- staleness-banner -->
> **Stand 2026-08-05.** Die BLE-Optionen sind wirkungslos, solange `CONFIG_BT_ENABLED=n` gilt (Default).
>
> Aktuell gepflegt wird `CLAUDE.md` im Projektwurzelverzeichnis.


This guide covers configuration options for the ESP32-C5 gateway, including compile-time (Kconfig), runtime (NVS), and MQTT-based configuration.

> ⚠️ Last reviewed 2026-07-31.
>
> **How the config chain actually resolves.** `CMakeLists.txt` sets
> `SDKCONFIG_DEFAULTS` to **two** files: `sdkconfig.defaults`, then
> `sdkconfig.local`. The local file overrides the defaults and is gitignored
> (it holds WiFi and MQTT credentials in plaintext — do not commit it). Editing
> `sdkconfig.defaults` has no effect once a generated `sdkconfig` exists: delete
> `sdkconfig` and rebuild, then check `build/config/sdkconfig.h`.
> `idf.py reconfigure` does not reliably regenerate it.
>
> **All `CONFIG_BT_*` and `CONFIG_BLE_*` options below are inert.** Bluetooth is
> disabled project-wide (`CONFIG_BT_ENABLED=n`) and `CONFIG_BT_SCANNER_ENABLED`
> now carries `depends on BT_ENABLED`, so it can no longer be switched on by
> itself from `sdkconfig.local`.
>
> Missing from this document: `CONFIG_MMWAVE_SENSOR_ENABLE` and the related
> `MMWAVE_GPIO_TX` / `MMWAVE_GPIO_RX` / `MMWAVE_DEFAULT_MAX_GATE` /
> `MMWAVE_DEFAULT_ABSENCE_DELAY` options.

## Table of Contents

- [Configuration Methods](#configuration-methods)
- [Kconfig Options](#kconfig-options)
- [Runtime Configuration](#runtime-configuration)
- [MQTT Configuration](#mqtt-configuration)
- [Configuration File Reference](#configuration-file-reference)
- [Advanced Configuration](#advanced-configuration)

## Configuration Methods

The gateway supports three configuration methods:

| Method | When | Persistence | Requires Rebuild | Use Case |
|--------|------|-------------|------------------|----------|
| **Kconfig** | Compile-time | In firmware | Yes | Default settings, development |
| **NVS** | Runtime | Flash memory | No | Persistent runtime changes |
| **MQTT** | Runtime | Saved to NVS | No | Remote configuration |

### Configuration Priority

Configuration values are loaded in this order:
1. **Kconfig defaults** (compile-time)
2. **NVS stored values** (overrides Kconfig)
3. **MQTT commands** (saves to NVS)

## Kconfig Options

Kconfig provides compile-time default configuration accessed via menuconfig.

### Access Menuconfig

```bash
cd ~/projects/esp32-c5-zigbee2mqtt
idf.py menuconfig
```

Navigate to: `ESP32-C5 Zigbee2MQTT Gateway Configuration`

### WiFi Configuration

Path: `WiFi Configuration`

#### CONFIG_WIFI_SSID
- **Type**: String
- **Default**: `myssid`
- **Description**: WiFi network name (SSID) to connect to
- **Example**: `MyHomeNetwork`

#### CONFIG_WIFI_PASSWORD
- **Type**: String
- **Default**: `mypassword`
- **Description**: WiFi password (WPA/WPA2/WPA3)
- **Security**: Stored in flash, consider using NVS for production

#### CONFIG_WIFI_MAXIMUM_RETRY
- **Type**: Integer
- **Default**: `5`
- **Range**: 1-20
- **Description**: Maximum connection retry attempts before giving up
- **Recommendation**: 5-10 for stable networks

#### CONFIG_WIFI_SCAN_METHOD
- **Type**: Integer
- **Default**: `0` (FAST)
- **Options**:
  - `0`: FAST - Connect to first matching SSID
  - `1`: ALL_CHANNEL - Scan all channels before connecting
- **Recommendation**: Use FAST for known networks

### MQTT Configuration

Path: `MQTT Configuration`

#### CONFIG_MQTT_BROKER_URL
- **Type**: String
- **Default**: `mqtt://mqtt.example.com`
- **Description**: MQTT broker URL
- **Supported Protocols**:
  - `mqtt://` - Standard MQTT (port 1883)
  - `mqtts://` - MQTT over TLS (port 8883)
  - `ws://` - MQTT over WebSocket (port 80/custom)
  - `wss://` - MQTT over secure WebSocket (port 443)
- **Examples**:
  ```
  mqtt://192.168.1.100
  mqtt://broker.hivemq.com
  mqtts://secure-broker.example.com
  ```

#### CONFIG_MQTT_BROKER_PORT
- **Type**: Integer
- **Default**: `1883`
- **Common Ports**:
  - `1883`: Standard MQTT
  - `8883`: MQTTS (TLS)
  - `8080`: WebSocket
- **Description**: MQTT broker port number

#### CONFIG_MQTT_USERNAME
- **Type**: String
- **Default**: Empty
- **Description**: MQTT broker username (optional)
- **Note**: Leave empty if broker doesn't require authentication

#### CONFIG_MQTT_PASSWORD
- **Type**: String
- **Default**: Empty
- **Description**: MQTT broker password (optional)

#### CONFIG_MQTT_CLIENT_ID
- **Type**: String
- **Default**: `esp32c5_zigbee_gateway`
- **Description**: MQTT client identifier
- **Important**: Must be unique per broker connection
- **Auto-generation**: Firmware appends MAC address suffix

#### CONFIG_MQTT_BASE_TOPIC
- **Type**: String
- **Default**: `zigbee2mqtt`
- **Description**: Base topic for all MQTT messages
- **Topic Structure**: `{base_topic}/{sub_topic}`
- **Example Topics**:
  ```
  zigbee2mqtt/bridge/state
  zigbee2mqtt/devices/light_1
  ```

#### CONFIG_MQTT_KEEPALIVE
- **Type**: Integer
- **Default**: `120` seconds
- **Range**: 30-300
- **Description**: MQTT keep-alive interval
- **Recommendation**: 120s for most use cases

#### CONFIG_MQTT_QOS
- **Type**: Integer
- **Default**: `1`
- **Range**: 0-2
- **Options**:
  - `0`: At most once (fire and forget)
  - `1`: At least once (guaranteed delivery)
  - `2`: Exactly once (guaranteed single delivery)
- **Recommendation**: Use QoS 1 for balance of reliability and performance

### Zigbee Configuration

Path: `Zigbee Configuration`

#### CONFIG_ZIGBEE_PAN_ID
- **Type**: Hexadecimal
- **Default**: `0x1A62`
- **Range**: `0x0000` - `0xFFFE`
- **Special**: `0xFFFF` = Auto-select random PAN ID
- **Description**: Personal Area Network identifier
- **Important**: Use unique PAN ID if multiple gateways nearby

#### CONFIG_ZIGBEE_CHANNEL
- **Type**: Integer
- **Default**: `11`
- **Range**: 11-26
- **Description**: Zigbee radio channel (2.4 GHz)
- **Channel Planning**:
  ```
  WiFi Channel 1 (2.412 GHz) → Zigbee 15, 20, 25
  WiFi Channel 6 (2.437 GHz) → Zigbee 11, 20, 25
  WiFi Channel 11 (2.462 GHz) → Zigbee 11, 15, 20
  ```
- **Recommendation**:
  - Use Zigbee channel 15, 20, or 25 to minimize WiFi interference
  - Scan your WiFi environment first
  - Avoid overlapping with WiFi

#### CONFIG_ZIGBEE_MAX_CHILDREN
- **Type**: Integer
- **Default**: `20`
- **Range**: 1-100
- **Description**: Maximum number of direct child devices
- **Memory**: Each device uses ~2KB RAM
- **Recommendation**:
  - Small network: 10-20 devices
  - Medium network: 20-50 devices
  - Large network: 50-100 devices (requires PSRAM optimization)

#### CONFIG_ZIGBEE_PERMIT_JOIN_DURATION
- **Type**: Integer
- **Default**: `180` seconds
- **Range**: 0-254 (255 = forever)
- **Description**: Default duration for device pairing mode
- **Security**: Don't use 255 (forever) in production

#### CONFIG_ZIGBEE_NETWORK_KEY_ENABLED
- **Type**: Boolean
- **Default**: `false`
- **Description**: Use custom network encryption key
- **Security**:
  - Disabled: Gateway generates random key (recommended)
  - Enabled: Use custom key (for multi-gateway setups)

#### CONFIG_ZIGBEE_NETWORK_KEY
- **Type**: String (hex)
- **Default**: `00112233445566778899AABBCCDDEEFF`
- **Format**: 32 hex characters (16 bytes)
- **Description**: 128-bit network encryption key
- **Requires**: `CONFIG_ZIGBEE_NETWORK_KEY_ENABLED=y`

### Bluetooth Gateway Configuration 🔵

Path: `Bluetooth Gateway Configuration`

> **Note**: Bluetooth features are optional and add ~75KB to memory footprint. Only enable if you need BLE device support alongside Zigbee.

#### CONFIG_ENABLE_BLUETOOTH_GATEWAY
- **Type**: Boolean
- **Default**: `false`
- **Description**: Enable Bluetooth LE gateway functionality
- **Impact**:
  - Memory: +75KB (BLE stack ~40KB + ESPHome API ~35KB)
  - Free heap: 120KB → 60KB (with Bluetooth enabled)
  - Zigbee device limit: 50 → 30 (recommended)
  - Flash: +~200KB code size
- **Requires**: 16MB flash recommended
- **Warning**: Tight memory constraints when enabled

#### CONFIG_BT_SCANNER_ENABLED
- **Type**: Boolean
- **Default**: `true`
- **Description**: Enable passive BLE scanner for beacons and presence detection
- **Requires**: `CONFIG_ENABLE_BLUETOOTH_GATEWAY=y`
- **Features**:
  - iBeacon tracking
  - Eddystone beacon support
  - RSSI-based presence detection
  - Low overhead (~5% CPU)

#### CONFIG_BT_PROXY_ENABLED
- **Type**: Boolean
- **Default**: `true`
- **Description**: Enable active BLE proxy for GATT sensor connections
- **Requires**: `CONFIG_ENABLE_BLUETOOTH_GATEWAY=y`
- **Features**:
  - Xiaomi LYWSD03MMC temperature/humidity sensors
  - Govee H5075 sensors
  - Active GATT connections
  - Higher overhead (~15% CPU)

#### CONFIG_BT_MAX_TRACKED_DEVICES
- **Type**: Integer
- **Default**: `50`
- **Range**: 10-100
- **Description**: Maximum number of tracked BLE devices
- **Memory**: Device registry offloaded to PSRAM
- **Note**: 50 BLE devices + 30 Zigbee devices = 80 total when Bluetooth enabled

#### CONFIG_BT_SCAN_INTERVAL_MS
- **Type**: Integer
- **Default**: `1000` milliseconds
- **Range**: 100-10000
- **Description**: BLE scan interval for passive scanner
- **Recommendations**:
  - Presence detection: 1000ms (balance of responsiveness and power)
  - Beacon tracking: 2000-5000ms (lower CPU usage)
  - High-frequency: 100-500ms (higher CPU usage)

#### CONFIG_BT_SCAN_WINDOW_MS
- **Type**: Integer
- **Default**: `100` milliseconds
- **Range**: 10-1000
- **Description**: BLE scan window duration
- **Constraint**: Must be ≤ scan_interval
- **Recommendation**: 10% of scan_interval for balance

#### CONFIG_BT_RSSI_THRESHOLD
- **Type**: Integer
- **Default**: `-80` dBm
- **Range**: -100 to -30
- **Description**: Minimum RSSI for device detection
- **Values**:
  - `-30` to `-50`: Very close (1-2 meters)
  - `-50` to `-70`: Medium range (5-10 meters)
  - `-70` to `-90`: Long range (10-30 meters)
  - `-90` to `-100`: Maximum sensitivity

#### CONFIG_BT_DEVICE_TIMEOUT_SEC
- **Type**: Integer
- **Default**: `300` seconds (5 minutes)
- **Range**: 60-3600
- **Description**: Mark device as away after this timeout
- **Use Case**: Presence detection timeout

### ESPHome API Configuration 🔵

Path: `ESPHome API Configuration`

> **Note**: ESPHome Native API enables full Home Assistant ESPHome integration without using the ESPHome framework (which is incompatible with ESP-IDF).

#### CONFIG_ESPHOME_API_ENABLED
- **Type**: Boolean
- **Default**: `true` (if Bluetooth enabled)
- **Description**: Enable ESPHome Native API server
- **Requires**: `CONFIG_ENABLE_BLUETOOTH_GATEWAY=y`
- **Port**: 6053 (standard ESPHome API port)
- **Protocol**: Protobuf over TCP
- **Features**:
  - Auto-discovery in Home Assistant
  - Entity support (Sensor, Binary Sensor, Switch, Device Tracker)
  - Service calls
  - OTA updates via ESPHome

#### CONFIG_ESPHOME_API_PORT
- **Type**: Integer
- **Default**: `6053`
- **Range**: 1024-65535
- **Description**: TCP port for ESPHome Native API
- **Standard**: 6053 (do not change unless necessary)

#### CONFIG_ESPHOME_API_PASSWORD
- **Type**: String
- **Default**: Empty (no password)
- **Description**: Optional password for API authentication
- **Security**: Recommended for production deployments
- **Note**: Home Assistant needs this password for connection

#### CONFIG_ESPHOME_DEVICE_NAME
- **Type**: String
- **Default**: `esp32-c5-unified-gateway`
- **Description**: Friendly device name for Home Assistant
- **Format**: Lowercase, hyphens allowed, no spaces
- **Example**: `living-room-gateway`

#### CONFIG_ESPHOME_FRIENDLY_NAME
- **Type**: String
- **Default**: `ESP32-C5 Unified Gateway`
- **Description**: Human-readable device name
- **Example**: `Living Room Gateway`

#### CONFIG_ESPHOME_DEVICE_AREA
- **Type**: String
- **Default**: Empty
- **Description**: Home Assistant area assignment
- **Example**: `Living Room`

#### CONFIG_ESPHOME_API_REBOOT_TIMEOUT_MS
- **Type**: Integer
- **Default**: `15000` milliseconds
- **Range**: 5000-300000
- **Description**: API disconnection timeout before reboot
- **Note**: 0 = disable auto-reboot

### WiFi Coexistence Configuration 🔵

Path: `WiFi Coexistence Configuration`

> **Important**: ESP32-C5 operates WiFi, Zigbee, and Bluetooth simultaneously on 2.4GHz. Proper coexistence configuration is CRITICAL for reliable operation.

#### CONFIG_WIFI_PREFER_5GHZ
- **Type**: Boolean
- **Default**: `true`
- **Description**: Prefer 5GHz WiFi band to minimize 2.4GHz interference
- **Recommendation**: **STRONGLY RECOMMENDED** when Bluetooth enabled
- **Impact**: Frees 2.4GHz spectrum for Zigbee and Bluetooth
- **Fallback**: Auto-fallback to 2.4GHz if 5GHz unavailable

#### CONFIG_COEXISTENCE_MODE
- **Type**: Integer
- **Default**: `1` (Balanced)
- **Description**: Hardware coexistence arbitration mode
- **Options**:
  - `0`: WiFi Priority - WiFi gets priority over BT/Zigbee (WiFi-critical use cases)
  - `1`: Balanced - Equal priority (RECOMMENDED for Zigbee+BT)
  - `2`: BT Priority - Bluetooth gets priority (BT audio use cases)
  - `3`: Zigbee Priority - Zigbee gets priority (Zigbee-critical use cases)
- **Recommendation**: Use Balanced (1) for tri-radio operation

#### CONFIG_COEXISTENCE_PREFERENCE
- **Type**: Integer
- **Default**: `2` (Prefer WiFi 5GHz)
- **Options**:
  - `0`: WiFi 2.4GHz only
  - `1`: WiFi dual-band (no preference)
  - `2`: WiFi 5GHz preferred (RECOMMENDED)
  - `3`: WiFi 5GHz only (strict)
- **Recommendation**: Use option 2 with CONFIG_WIFI_PREFER_5GHZ=y

#### CONFIG_ZIGBEE_CHANNEL_MASK
- **Type**: Hexadecimal bitmask
- **Default**: `0x07FFF800` (channels 11-26)
- **Description**: Allowed Zigbee channels bitmask
- **Recommendation**: Restrict to non-overlapping channels
- **Examples**:
  ```
  0x00008000 = Channel 15 only
  0x00100000 = Channel 20 only
  0x02000000 = Channel 25 only
  0x02108000 = Channels 15, 20, 25 (recommended)
  ```
- **WiFi Overlap**: Use channels 15, 20, or 25 to avoid WiFi 1, 6, 11

#### CONFIG_COEXISTENCE_BT_SCAN_DUTY_CYCLE
- **Type**: Integer
- **Default**: `50` percent
- **Range**: 10-90
- **Description**: Bluetooth scan duty cycle during coexistence
- **Lower Values**: Less BT scanning, more time for WiFi/Zigbee
- **Higher Values**: More BT scanning, potentially more interference
- **Recommendation**: 30-50% for balanced operation

#### CONFIG_COEXISTENCE_EXTERNAL_COEX
- **Type**: Boolean
- **Default**: `false`
- **Description**: Enable external coexistence GPIO pins
- **Use Case**: External WiFi modules or coexistence with non-ESP devices
- **Requires**: Hardware support and wiring

### Gateway Configuration

Path: `Gateway Configuration`

#### CONFIG_MAX_ZIGBEE_DEVICES
- **Type**: Integer
- **Default**: `50`
- **Range**: 1-200
- **Description**: Maximum total devices in device table
- **Memory**: Each device entry uses ~2KB
- **Note**: Different from `MAX_CHILDREN` (routing table vs device table)

#### CONFIG_DEVICE_DISCOVERY_INTERVAL
- **Type**: Integer
- **Default**: `60` seconds
- **Range**: 10-3600
- **Description**: Interval for device health checks and discovery

#### CONFIG_MQTT_PUBLISH_INTERVAL
- **Type**: Integer
- **Default**: `1000` milliseconds
- **Range**: 100-10000
- **Description**: Minimum interval between MQTT publishes
- **Purpose**: Rate limiting to prevent broker flooding

#### CONFIG_ENABLE_HOMEASSISTANT_DISCOVERY
- **Type**: Boolean
- **Default**: `true`
- **Description**: Enable automatic Home Assistant device discovery
- **Effect**: Publishes discovery messages to HA discovery topic

#### CONFIG_HOMEASSISTANT_DISCOVERY_PREFIX
- **Type**: String
- **Default**: `homeassistant`
- **Description**: Topic prefix for HA discovery
- **Standard**: Keep default unless HA configured differently

#### CONFIG_BRIDGE_AUTO_PUBLISH_DEVICE_LIST
- **Type**: Boolean
- **Default**: `true`
- **Description**: Auto-publish device list on join/leave events

#### CONFIG_BRIDGE_PUBLISH_INTERVAL
- **Type**: Integer
- **Default**: `300` seconds
- **Range**: 60-3600
- **Description**: Interval for publishing bridge info/statistics

#### CONFIG_ENABLE_BRIDGE_LOGGING_TO_MQTT
- **Type**: Boolean
- **Default**: `false`
- **Description**: Redirect ESP logs to MQTT topic
- **Warning**: Generates significant MQTT traffic
- **Use Case**: Remote debugging

### Debug and Logging

Path: `Debug and Logging`

#### CONFIG_ENABLE_VERBOSE_LOGGING
- **Type**: Boolean
- **Default**: `false`
- **Description**: Enable verbose debug logs for all modules
- **Impact**: Increases code size and serial output

#### CONFIG_LOG_MEMORY_STATISTICS
- **Type**: Boolean
- **Default**: `true`
- **Description**: Periodically log heap memory statistics

#### CONFIG_MEMORY_LOG_INTERVAL
- **Type**: Integer
- **Default**: `30` seconds
- **Range**: 10-300
- **Description**: Memory statistics logging interval

#### CONFIG_ENABLE_TASK_STATISTICS
- **Type**: Boolean
- **Default**: `false`
- **Description**: Enable FreeRTOS task monitoring
- **Impact**: Slight CPU overhead

### OTA Update Configuration

Path: `OTA Update Configuration`

#### CONFIG_OTA_FIRMWARE_URL
- **Type**: String
- **Default**: Empty
- **Description**: URL for OTA firmware updates
- **Examples**:
  ```
  http://192.168.1.100:8080/firmware.bin
  https://updates.example.com/esp32c5/latest.bin
  ```
- **Note**: Leave empty to disable OTA

#### CONFIG_OTA_CHECK_INTERVAL
- **Type**: Integer
- **Default**: `24` hours
- **Range**: 1-168 (1 week)
- **Description**: Automatic update check interval

#### CONFIG_OTA_SKIP_VERSION_CHECK
- **Type**: Boolean
- **Default**: `false`
- **Description**: Force update regardless of version
- **Warning**: Use with caution

## Runtime Configuration

Runtime configuration is stored in NVS (Non-Volatile Storage) and persists across reboots.

### Configuration Manager API

The gateway includes a configuration manager for runtime changes.

### View Current Configuration

```bash
# Via MQTT
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_get" -m '{}'

# Response on zigbee2mqtt/bridge/response/config_get:
{
  "wifi_ssid": "MyNetwork",
  "mqtt_broker_url": "mqtt://192.168.1.100",
  "mqtt_port": 1883,
  "zigbee_pan_id": "0x1A62",
  "zigbee_channel": 11,
  ...
}
```

### Change Configuration

```bash
# Via MQTT
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
  "wifi_ssid": "NewNetwork",
  "wifi_password": "NewPassword",
  "zigbee_channel": 20
}'

# Response:
{
  "status": "ok",
  "data": {
    "restart_required": true
  }
}

# Restart to apply changes
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/restart" -m '{}'
```

### Configuration Validation

The configuration manager validates all changes:
- String length limits
- Numeric ranges
- Format validation (URLs, hex values)
- Cross-field dependencies

Invalid configurations are rejected with error messages.

### Reset to Defaults

```bash
# Via MQTT
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_reset" -m '{}'

# Or via serial console (development only)
# Connect to serial monitor and enter:
# config_reset
```

## MQTT Configuration

Real-time configuration via MQTT topics.

### Configuration Topics

| Topic | Purpose | Access |
|-------|---------|--------|
| `zigbee2mqtt/bridge/request/config_get` | Get current config | Read |
| `zigbee2mqtt/bridge/request/config_set` | Set configuration | Write |
| `zigbee2mqtt/bridge/response/config_get` | Config response | Read |
| `zigbee2mqtt/bridge/response/config_set` | Set confirmation | Read |

### Configuration Fields

All configuration fields from `gateway_config_t` structure are accessible:

**Network:**
- `wifi_ssid` (string)
- `wifi_password` (string)
- `mqtt_broker_url` (string)
- `mqtt_port` (uint16)
- `mqtt_username` (string)
- `mqtt_password` (string)
- `mqtt_keepalive` (uint16)
- `mqtt_qos` (uint8)

**Zigbee:**
- `zigbee_pan_id` (uint16, hex string)
- `zigbee_channel` (uint8, 11-26)
- `zigbee_max_children` (uint8)

**Gateway:**
- `device_publish_interval_ms` (uint32)
- `ha_discovery_enabled` (bool)
- `bridge_logging_enabled` (bool)

**System:**
- `log_level` (uint8, 0-5)
- `ota_enabled` (bool)
- `ota_url` (string)

### Configuration Examples

#### Change WiFi Network

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
  "wifi_ssid": "GuestNetwork",
  "wifi_password": "guest123"
}'
```

#### Change MQTT Broker

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
  "mqtt_broker_url": "mqtt://10.0.0.50",
  "mqtt_port": 1883,
  "mqtt_username": "gateway",
  "mqtt_password": "secret"
}'
```

#### Change Zigbee Channel

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
  "zigbee_channel": 25
}'
```

#### Enable OTA Updates

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
  "ota_enabled": true,
  "ota_url": "http://192.168.1.100:8080/firmware.bin"
}'
```

## Configuration File Reference

### NVS Storage Structure

Configuration is stored in NVS partition under namespace `gateway_cfg`:

| Key | Type | Size | Description |
|-----|------|------|-------------|
| `config_ver` | uint32 | 4B | Config version for migration |
| `wifi_ssid` | string | 32B | WiFi SSID |
| `wifi_pass` | string | 64B | WiFi password |
| `mqtt_url` | string | 128B | MQTT broker URL |
| `mqtt_port` | uint16 | 2B | MQTT port |
| `mqtt_user` | string | 64B | MQTT username |
| `mqtt_pass` | string | 64B | MQTT password |
| `zb_panid` | uint16 | 2B | Zigbee PAN ID |
| `zb_channel` | uint8 | 1B | Zigbee channel |
| ... | ... | ... | ... |

Total storage: ~600 bytes per configuration

### Default Configuration

Default configuration (from Kconfig):

```c
{
  .wifi_ssid = "myssid",
  .wifi_password = "mypassword",
  .mqtt_broker_url = "mqtt://mqtt.example.com",
  .mqtt_port = 1883,
  .mqtt_username = "",
  .mqtt_password = "",
  .mqtt_client_id = "esp32c5_zigbee_gateway",
  .mqtt_keepalive = 120,
  .mqtt_qos = 1,
  .zigbee_pan_id = 0x1A62,
  .zigbee_channel = 11,
  .zigbee_max_children = 20,
  .zigbee_permit_join_on_boot = false,
  .device_publish_interval_ms = 1000,
  .ha_discovery_enabled = true,
  .bridge_logging_enabled = false,
  .log_level = 3, // ESP_LOG_INFO
  .ota_enabled = false,
  .ota_url = "",
  .config_version = 1
}
```

## Advanced Configuration

### Multi-Gateway Setup

When running multiple gateways:

1. **Unique PAN IDs**: Each gateway needs unique `zigbee_pan_id`
2. **Unique Client IDs**: Each gateway needs unique `mqtt_client_id`
3. **Unique Base Topics**: Consider different `mqtt_base_topic` per gateway
4. **Same Network Key**: Use same `zigbee_network_key` for device mobility

Example configuration for Gateway 2:
```bash
mosquitto_pub -h localhost -t "zigbee2mqtt_2/bridge/request/config_set" -m '{
  "zigbee_pan_id": "0x1A63",
  "mqtt_client_id": "esp32c5_gateway_2",
  "mqtt_base_topic": "zigbee2mqtt_2"
}'
```

### Channel Planning

**Zigbee + WiFi Coexistence:**

ESP32-C5 hardware supports both WiFi and Zigbee on 2.4 GHz simultaneously. For optimal performance:

1. **Check WiFi channels in use**:
   ```bash
   # Linux
   sudo iwlist wlan0 scan | grep Channel

   # Or use WiFi analyzer app
   ```

2. **Select non-overlapping Zigbee channel**:
   - If WiFi on channel 1: Use Zigbee 15, 20, or 25
   - If WiFi on channel 6: Use Zigbee 20 or 25
   - If WiFi on channel 11: Use Zigbee 15 or 20

3. **Update configuration**:
   ```bash
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
     "zigbee_channel": 20
   }'
   ```

### Security Hardening

#### 1. Change Default Passwords

```bash
idf.py menuconfig
# Change default WiFi password
# Change default MQTT credentials
```

#### 2. Enable MQTTS

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
  "mqtt_broker_url": "mqtts://secure-broker.example.com",
  "mqtt_port": 8883
}'
```

#### 3. Custom Network Key

```bash
idf.py menuconfig
# Enable: Zigbee Configuration → Use Custom Network Key
# Set: Zigbee Network Key → [your 32-char hex key]
```

#### 4. Disable Bridge Logging

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
  "bridge_logging_enabled": false
}'
```

### Bluetooth and ESPHome Configuration 🔵

#### Enable Bluetooth Gateway

```bash
idf.py menuconfig
# Enable: Bluetooth Gateway Configuration → Enable Bluetooth Gateway
# Enable: Bluetooth Gateway Configuration → BLE Scanner
# Enable: Bluetooth Gateway Configuration → BLE Proxy
# Set: Bluetooth Gateway Configuration → Max Tracked Devices: 50
```

**Impact Analysis**:
- Memory: 120KB free → 60KB free (WARNING level)
- Zigbee devices: Reduce from 50 to 30 recommended
- CPU load: 38% → 73% (tight but manageable)
- Flash: +200KB code size

#### Configure ESPHome API

```bash
idf.py menuconfig
# Enable: ESPHome API Configuration → Enable ESPHome API
# Set: ESPHome API Configuration → Device Name: "living-room-gateway"
# Set: ESPHome API Configuration → Friendly Name: "Living Room Gateway"
# Set: ESPHome API Configuration → API Password: (your secure password)
```

**Home Assistant Integration**:
1. Device will auto-discover via mDNS
2. Add integration in HA: Settings → Devices & Services → Add Integration → ESPHome
3. Enter IP address and password (if configured)

#### Configure WiFi Coexistence for Tri-Radio

```bash
idf.py menuconfig
# Enable: WiFi Coexistence → Prefer 5GHz WiFi (CRITICAL)
# Set: WiFi Coexistence → Coexistence Mode: 1 (Balanced)
# Set: WiFi Coexistence → Coexistence Preference: 2 (5GHz preferred)
# Set: Zigbee Configuration → Channel: 20 (or 15, 25)
# Set: Bluetooth Gateway → Scan Interval: 1000ms
# Set: Bluetooth Gateway → BT Scan Duty Cycle: 40%
```

**Channel Planning for Tri-Radio**:
- **WiFi**: 5GHz (mandatory for optimal performance)
- **Zigbee**: Channel 20 (2.450 GHz, avoids WiFi 2.4GHz channels 1/6/11)
- **Bluetooth**: Adaptive Frequency Hopping (AFH) on 2.4GHz

#### BLE Device Configuration Examples

**iBeacon Tracking**:
```bash
# No configuration needed, auto-detected
# Publishes to MQTT: zigbee2mqtt/bluetooth/ibeacon/{UUID}
```

**Xiaomi LYWSD03MMC Sensors**:
```bash
# Pair via MQTT
mosquitto_pub -h localhost -t "zigbee2mqtt/bluetooth/request/pair" -m '{
  "device_type": "xiaomi_lywsd03mmc",
  "mac": "A4:C1:38:XX:XX:XX"
}'
```

**Govee H5075 Sensors**:
```bash
# Auto-detected, published to MQTT
# Topic: zigbee2mqtt/bluetooth/govee_h5075_{MAC}
```

### Performance Tuning

#### Reduce MQTT Traffic

```bash
# Increase publish interval
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
  "device_publish_interval_ms": 5000,
  "bridge_logging_enabled": false
}'
```

#### Optimize Memory (Zigbee-Only)

```bash
idf.py menuconfig
# Gateway Configuration → Maximum Zigbee Devices: 50
# Zigbee Configuration → Maximum Zigbee Children: 20
# Bluetooth Gateway → Enable Bluetooth Gateway: No
```

#### Optimize Memory (Zigbee + Bluetooth)

```bash
idf.py menuconfig
# Gateway Configuration → Maximum Zigbee Devices: 30 (REDUCE from 50)
# Zigbee Configuration → Maximum Zigbee Children: 15 (REDUCE from 20)
# Bluetooth Gateway → Max Tracked Devices: 50
# WiFi Coexistence → Prefer 5GHz: Yes (CRITICAL)
```

#### Reduce BLE Scanning Overhead

```bash
idf.py menuconfig
# Bluetooth Gateway → BLE Scan Interval: 2000ms (increase from 1000ms)
# Bluetooth Gateway → BLE Scan Window: 100ms
# Bluetooth Gateway → BT Scan Duty Cycle: 30% (reduce from 50%)
```

#### Reduce Log Level

```bash
# Via MQTT
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -m '{
  "log_level": 2
}'

# Log levels: 0=None, 1=Error, 2=Warn, 3=Info, 4=Debug, 5=Verbose
```

## Configuration Backup and Restore

### Export Configuration

```bash
# Get full configuration
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/response/config_get" -C 1 > config_backup.json

# Or via serial
# config_export > config_backup.json
```

### Import Configuration

```bash
# Via MQTT
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_import" \
  -f config_backup.json

# Or multiple individual sets
cat config_backup.json | jq '.data' | \
  mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" -l
```

### Reset NVS (Factory Reset)

```bash
# Via serial monitor (development)
idf.py erase-flash

# Or via MQTT
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/factory_reset" -m '{}'
```

## Next Steps

- [Usage Guide](USAGE.md) - Learn how to use the configured gateway
- [Bluetooth Gateway Guide](BLUETOOTH_GATEWAY.md) - 🔵 BLE device setup and configuration
- [ESPHome API Guide](ESPHOME_API.md) - 🔵 ESPHome integration with Home Assistant
- [Coexistence Guide](COEXISTENCE.md) - 🔵 WiFi/BT/Zigbee optimization
- [Troubleshooting](TROUBLESHOOTING.md) - Solve configuration issues
- [API Reference](API_REFERENCE.md) - Configuration API details
