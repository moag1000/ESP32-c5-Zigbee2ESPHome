# ESPHome Native API Guide

<!-- staleness-banner -->
> **Stand 2026-08-05.** Der BLE-Proxy ist inaktiv (BLE projektweit aus). Neu seit diesem Dokument:
> die Entity-Zustaende liegen in `esphome_entity_mirror.c` mit eigener
> Kapazitaet, nicht mehr als virtuelle Geraete in der Device-Registry.
>
> Aktuell gepflegt wird `CLAUDE.md` im Projektwurzelverzeichnis.


Complete guide to ESPHome Native API integration for the ESP32-C5 gateway.

> **Status**: active — this is the **primary** Home Assistant integration
> **Requires**: `CONFIG_ESPHOME_API_ENABLE=y`
>
> ⚠️ Last reviewed 2026-07-31; the body below is still the fork state from
> 2026-02-19 and is missing three things:
> - **BLE Proxy sections do not apply.** Bluetooth is disabled
>   (`CONFIG_BT_ENABLED=n`), so `bluetooth_proxy_feature_flags` is reported as
>   `0` and the Bluetooth MAC (field 18) is omitted from `DeviceInfoResponse`.
>   Home Assistant will not offer this device as a Bluetooth proxy.
> - **ESPHome OTA** exists: `esphome_ota.c`, ESPHome OTA protocol on port 3232,
>   started from `esphome_adapter_gateway.c`. Covers the gateway firmware
>   itself, not Zigbee sub-device OTA.
> - **Service calls**: `esphome_services.c` provides the registration and
>   dispatch framework, but only `test_service` is actually registered. There
>   are no `permit_join` / `remove` / `reconfigure` services yet.

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Configuration](#configuration)
- [Home Assistant Integration](#home-assistant-integration)
- [Available Entities](#available-entities)
- [Protocol Details](#protocol-details)
- [Security](#security)
- [Troubleshooting](#troubleshooting)

## Overview

The ESP32-C5 Unified Gateway implements the **ESPHome Native API** protocol, allowing it to appear as an ESPHome device in Home Assistant without using the ESPHome framework.

### What is ESPHome Native API?

ESPHome Native API is a **Protobuf-based TCP protocol** (port 6053) that enables:
- **Bidirectional communication** between ESP devices and Home Assistant
- **Entity discovery** (sensors, switches, binary sensors, device trackers)
- **Real-time updates** with push notifications
- **Service calls** from Home Assistant to device
- **OTA updates** via Home Assistant ESPHome integration

### Why Implement the Protocol Directly?

> **Important Clarification**: ESPHome framework **DOES support ESP-IDF** as a build framework (not Arduino-only).

**Why we implement the ESPHome Native API protocol manually**:
- ESP-Zigbee-SDK requires **direct ESP-IDF integration** with specific build configurations
- ESPHome framework doesn't support **ESP-Zigbee-SDK** integration (no Zigbee support)
- We need **full control** over Zigbee stack initialization, memory allocation, and task priorities
- Custom tri-radio coexistence management not available in ESPHome framework
- Implementing the protocol (~2,000-5,000 LOC estimated) gives maximum flexibility

**Solution**: Implement the **ESPHome Native API protocol** (Protobuf over TCP, port 6053) directly in our ESP-IDF project. This allows the gateway to communicate with Home Assistant's ESPHome integration while maintaining full control over the Zigbee and Bluetooth stacks.

### Key Benefits

✅ **Full Home Assistant ESPHome Integration**
- Gateway appears as native ESPHome device
- No custom components needed
- Automatic discovery via mDNS

✅ **Dual Integration Modes**
- **MQTT Discovery**: For Zigbee/Bluetooth devices
- **ESPHome API**: For gateway itself (diagnostics, presence)

✅ **Rich Entity Support**
- Sensors: uptime, free memory, WiFi signal, device counts
- Binary Sensors: connectivity status
- Switches: permit join, restart
- Device Trackers: BLE presence detection

## Architecture

### Protocol Stack

```
┌─────────────────────────────────────────────┐
│        Home Assistant ESPHome                │
│         Integration (Frontend)               │
└─────────────────────────────────────────────┘
                   │
                   │ ESPHome Native API
                   │ (Protobuf over TCP:6053)
                   ▼
┌─────────────────────────────────────────────┐
│      ESP32-C5 Unified Gateway               │
│  ┌──────────────────────────────────────┐  │
│  │   ESPHome API Server                 │  │
│  │   - TCP Server (port 6053)           │  │
│  │   - Protobuf Encoding/Decoding       │  │
│  │   - Entity Management                │  │
│  │   - Service Call Handling            │  │
│  └──────────────────────────────────────┘  │
│                   │                          │
│                   ▼                          │
│  ┌──────────────────────────────────────┐  │
│  │   Entity Providers                   │  │
│  │   - System Monitor (CPU, Memory)     │  │
│  │   - WiFi Manager (RSSI, IP)          │  │
│  │   - Zigbee Coordinator (Permit Join) │  │
│  │   - Bluetooth Tracker (Presence)     │  │
│  └──────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
```

### Component Architecture

**esphome_api_server.c**
- TCP server on port 6053
- Connection management
- Protobuf message routing

**esphome_protocol.c**
- Protobuf encoding/decoding
- Message serialization
- Protocol version: 1.9+

**esphome_entities.c**
- Entity registration
- State updates
- Entity types: Sensor, BinarySensor, Switch, DeviceTracker

**esphome_services.c**
- Service call handlers
- Actions: restart, permit_join, etc.

**esphome_discovery.c**
- mDNS advertisement
- Auto-discovery in Home Assistant

## Configuration

### Enable ESPHome API

```bash
idf.py menuconfig
```

Navigate to: `ESPHome API Configuration`

### Essential Settings

**CONFIG_ESPHOME_API_ENABLED**
- Enable ESPHome Native API server
- Default: `true` (when Bluetooth enabled)
- **Set to**: `true`

**CONFIG_ESPHOME_API_PORT**
- TCP port for API server
- Default: `6053` (ESPHome standard)
- **Do not change** unless necessary

**CONFIG_ESPHOME_DEVICE_NAME**
- Device name for Home Assistant
- Default: `esp32-c5-unified-gateway`
- Format: lowercase, hyphens, no spaces
- Example: `living-room-gateway`

**CONFIG_ESPHOME_FRIENDLY_NAME**
- Human-readable device name
- Default: `ESP32-C5 Unified Gateway`
- Example: `Living Room Gateway`

**CONFIG_ESPHOME_DEVICE_AREA**
- Home Assistant area assignment
- Default: Empty (no area)
- Example: `Living Room`

**CONFIG_ESPHOME_API_PASSWORD**
- Optional API password
- Default: Empty (no password)
- **Production**: Set a secure password
- Home Assistant needs this to connect

**CONFIG_ESPHOME_API_REBOOT_TIMEOUT_MS**
- Auto-reboot timeout on disconnect
- Default: `15000` ms (15 seconds)
- Set to `0` to disable auto-reboot

### Example Configuration

```bash
idf.py menuconfig

# ESPHome API Configuration
CONFIG_ESPHOME_API_ENABLED=y
CONFIG_ESPHOME_API_PORT=6053
CONFIG_ESPHOME_DEVICE_NAME="living-room-gateway"
CONFIG_ESPHOME_FRIENDLY_NAME="Living Room Gateway"
CONFIG_ESPHOME_DEVICE_AREA="Living Room"
CONFIG_ESPHOME_API_PASSWORD="your_secure_password"
CONFIG_ESPHOME_API_REBOOT_TIMEOUT_MS=30000
```

## Home Assistant Integration

### Automatic Discovery (Recommended)

Gateway advertises itself via **mDNS** with service type `_esphomelib._tcp`.

**Steps**:
1. Ensure gateway is on same network as Home Assistant
2. Navigate to: **Settings** → **Devices & Services**
3. ESPHome integration should show discovered device
4. Click **Configure**
5. Enter API password (if configured)
6. Click **Submit**

### Manual Configuration

If auto-discovery fails:

1. **Add Integration**:
   - Settings → Devices & Services → Add Integration
   - Search: **ESPHome**
   - Click **ESPHome**

2. **Enter Connection Details**:
   - **Host**: `192.168.1.100` (gateway IP)
   - **Port**: `6053` (default)
   - Click **Submit**

3. **Enter Password** (if configured):
   - **API Password**: `your_secure_password`
   - Click **Submit**

4. **Device Added**:
   - Gateway appears in ESPHome integration
   - Entities automatically created

### Verify Connection

```bash
# Check ESPHome API logs in Home Assistant
# Settings → System → Logs → Filter: esphome

# Look for:
# INFO ESPHome API connection established to living-room-gateway
```

### Gateway Serial Logs

```bash
idf.py monitor

# Look for:
I (xxx) ESPHOME_API: API server started on port 6053
I (xxx) ESPHOME_API: Client connected from 192.168.1.50
I (xxx) ESPHOME_API: Authentication successful
I (xxx) ESPHOME_API: Entity states sent (12 entities)
```

## Available Entities

### Sensors

**sensor.{device_name}_uptime**
- **Unit**: seconds
- **Update**: Every 60s
- **Value**: System uptime since boot

**sensor.{device_name}_free_memory**
- **Unit**: bytes
- **Update**: Every 10s
- **Value**: Free heap memory

**sensor.{device_name}_wifi_signal**
- **Unit**: dBm
- **Update**: Every 30s
- **Value**: WiFi RSSI

**sensor.{device_name}_zigbee_devices**
- **Unit**: count
- **Update**: On change
- **Value**: Number of paired Zigbee devices

**sensor.{device_name}_bluetooth_devices**
- **Unit**: count
- **Update**: On change
- **Value**: Number of tracked BLE devices

**sensor.{device_name}_cpu_usage**
- **Unit**: %
- **Update**: Every 10s
- **Value**: Estimated CPU utilization

### Binary Sensors

**binary_sensor.{device_name}_wifi_connected**
- **State**: ON/OFF
- **Update**: On change
- **Value**: WiFi connection status

**binary_sensor.{device_name}_mqtt_connected**
- **State**: ON/OFF
- **Update**: On change
- **Value**: MQTT broker connection status

**binary_sensor.{device_name}_zigbee_permit_join**
- **State**: ON/OFF
- **Update**: On change
- **Value**: Zigbee permit join enabled

### Switches

**switch.{device_name}_permit_join**
- **Action**: Enable/disable Zigbee pairing
- **State**: ON/OFF
- **Service**: `esphome.living_room_gateway_permit_join`

**switch.{device_name}_restart**
- **Action**: Restart gateway
- **State**: OFF (momentary)
- **Service**: `esphome.living_room_gateway_restart`

### Device Trackers (BLE Presence)

**device_tracker.{beacon_name}**
- **State**: home/away
- **Update**: On RSSI threshold change
- **Value**: iBeacon/Eddystone presence

**Example**:
```
device_tracker.bedroom_phone
State: home
Attributes:
  rssi: -68
  distance: 2.5
  last_seen: 2026-01-23T10:30:45Z
```

## Protocol Details

### ESPHome Native API Protocol

**Version**: 1.9+
**Transport**: TCP
**Port**: 6053
**Encoding**: Protocol Buffers (Protobuf)

### Message Types

**Hello/HandshakeRequest**
- Client identification
- Protocol version negotiation

**ConnectRequest/ConnectResponse**
- Authentication
- Password validation

**DeviceInfoRequest/DeviceInfoResponse**
- Device metadata
- Model, manufacturer, version

**ListEntitiesRequest**
- Enumerate all entities
- Types: Sensor, BinarySensor, Switch, etc.

**SubscribeStatesRequest**
- Subscribe to entity state updates
- Push-based notifications

**StateResponse**
- Entity state changes
- Sent on value update

**ExecuteServiceRequest**
- Service call from HA to device
- Actions: restart, permit_join, etc.

### Protobuf Definitions

Gateway implements protocol from:
```
https://github.com/esphome/aioesphomeapi/blob/main/aioesphomeapi/api.proto
```

### Connection Flow

```
Home Assistant                 ESP32-C5 Gateway
     │                                │
     │──── TCP Connect (6053) ────────▶│
     │                                │
     │◄─── HelloResponse ─────────────│
     │                                │
     │──── ConnectRequest ────────────▶│
     │     (password)                 │
     │                                │
     │◄─── ConnectResponse ───────────│
     │     (auth success)             │
     │                                │
     │──── DeviceInfoRequest ─────────▶│
     │                                │
     │◄─── DeviceInfoResponse ────────│
     │     (name, model, version)     │
     │                                │
     │──── ListEntitiesRequest ───────▶│
     │                                │
     │◄─── ListEntitiesResponse ──────│
     │     (12 entities)              │
     │                                │
     │──── SubscribeStatesRequest ────▶│
     │                                │
     │◄─── StateResponse ─────────────│
     │     (continuous updates)       │
```

## Security

### Authentication

ESPHome Native API supports two authentication methods:

**1. Encryption Key (RECOMMENDED)**:
- 32-byte base64-encoded encryption key
- **Provides encryption AND authentication**
- Recommended since ESPHome 2024+
- Configurable via `CONFIG_ESPHOME_API_ENCRYPTION_KEY`
- Example: `your-32-byte-base64-key-here==`

**2. Password (LEGACY)**:
- SHA-256 challenge-response
- Password hashed before transmission
- **Authentication only, NO encryption**
- Configurable via `CONFIG_ESPHOME_API_PASSWORD`
- ⚠️ Deprecated: Password-only auth is being phased out

### Security Recommendations

1. **Use Encryption Key (RECOMMENDED)**:
```bash
idf.py menuconfig
# ESPHome API → API Encryption Key: <32-byte base64 key>

# Generate key:
python3 -c "import secrets; print(secrets.token_urlsafe(32))"
```

2. **Or Set Strong Password (Legacy)**:
```bash
idf.py menuconfig
# ESPHome API → API Password: <32-char random password>
# ⚠️ WARNING: Password-only provides NO encryption!
```

3. **Firewall Rules**:
```bash
# Allow only from Home Assistant IP
iptables -A INPUT -p tcp --dport 6053 -s 192.168.1.50 -j ACCEPT
iptables -A INPUT -p tcp --dport 6053 -j DROP
```

4. **Isolated Network**:
- Use dedicated IoT VLAN
- Restrict internet access
- Allow only HA communication

5. **VPN Tunnel** (Optional):
- WireGuard between HA and gateway
- Additional layer if not using encryption key

## Troubleshooting

### Home Assistant Can't Discover Device

**Check mDNS**:
```bash
# On Linux
avahi-browse -a | grep esphome

# Look for:
# _esphomelib._tcp    living-room-gateway
```

**Solutions**:
1. Check firewall (port 5353 for mDNS)
2. Ensure same network/VLAN
3. Use manual IP configuration
4. Restart Home Assistant

### API Connection Drops Frequently

**Check Logs**:
```bash
idf.py monitor

# Look for:
E (xxx) ESPHOME_API: Connection timeout
E (xxx) ESPHOME_API: Keepalive timeout
```

**Solutions**:
1. Increase reboot timeout:
```bash
CONFIG_ESPHOME_API_REBOOT_TIMEOUT_MS=30000
```

2. Check WiFi stability
3. Monitor free memory (should be > 50 KB)

### Entities Not Updating

**Check Entity Subscription**:
```bash
# In HA logs:
# Settings → System → Logs → Filter: esphome

# Look for:
# DEBUG Subscribed to state updates
```

**Solutions**:
1. Restart Home Assistant
2. Reload ESPHome integration
3. Check gateway memory (low heap = slow updates)

### Authentication Failed

**Verify Password**:
```bash
idf.py menuconfig
# ESPHome API → API Password
```

**Home Assistant**:
- Remove integration
- Re-add with correct password

### Port 6053 Already in Use

**Check for conflicts**:
```bash
# On gateway
netstat -an | grep 6053

# If another service uses 6053, change port:
CONFIG_ESPHOME_API_PORT=16053
```

**Update Home Assistant**:
- Manual configuration with custom port

## Next Steps

- [Bluetooth Gateway Guide](BLUETOOTH_GATEWAY.md) - BLE device setup
- [Coexistence Guide](COEXISTENCE.md) - WiFi/BT/Zigbee optimization
- [Configuration Guide](CONFIGURATION.md) - Full configuration reference
- [Usage Guide](USAGE.md) - Daily operation and automations
