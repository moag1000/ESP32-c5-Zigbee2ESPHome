# Troubleshooting Guide

This guide helps diagnose and resolve common issues with the ESP32-C5 Unified Gateway (Zigbee2MQTT + Bluetooth + ESPHome API).

## Table of Contents

- [Diagnostic Tools](#diagnostic-tools)
- [WiFi Connection Issues](#wifi-connection-issues)
- [MQTT Broker Issues](#mqtt-broker-issues)
- [Zigbee Pairing Failures](#zigbee-pairing-failures)
- [Bluetooth Issues](#bluetooth-issues) 🔵
- [WiFi/BT/Zigbee Coexistence Problems](#wifibt-zigbee-coexistence-problems) 🔵
- [ESPHome API Issues](#esphome-api-issues) 🔵
- [Device Communication Issues](#device-communication-issues)
- [Memory Issues](#memory-issues)
- [Boot Loops and Crashes](#boot-loops-and-crashes)
- [Performance Issues](#performance-issues)
- [Log Analysis](#log-analysis)
- [ESP-Zigbee-SDK v1.6.x Issues](#esp-zigbee-sdk-v16x-issues)

## Diagnostic Tools

### Serial Monitor

Essential for debugging:

```bash
# ESP-IDF monitor (recommended)
idf.py monitor

# Or helper script
./scripts/monitor.sh

# Exit: Ctrl+]
```

### MQTT Monitoring

Monitor all MQTT traffic:

```bash
# Subscribe to all topics
mosquitto_sub -h localhost -t "#" -v

# Subscribe to bridge topics only
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/#" -v

# Subscribe to specific device
mosquitto_sub -h localhost -t "zigbee2mqtt/living_room_light" -v
```

### System Health Check

Request health status:

```bash
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/health_check" -m '{}'

# Watch response on:
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/response/health_check"
```

### Log Levels

Adjust verbosity:

```bash
# Increase logging (via MQTT)
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
  -m '{"log_level": 4}'  # 4 = Debug

# Log levels: 0=None, 1=Error, 2=Warn, 3=Info, 4=Debug, 5=Verbose
```

## WiFi Connection Issues

### Symptom: Cannot Connect to WiFi

#### Check Configuration

```bash
# Via serial monitor
idf.py monitor

# Look for:
[WIFI] Connecting to WiFi...
[WIFI] Failed to connect: ESP_ERR_WIFI_SSID
```

**Solutions:**

1. **Verify SSID/Password**:
   ```bash
   idf.py menuconfig
   # WiFi Configuration → WiFi SSID
   # WiFi Configuration → WiFi Password
   ```

2. **Check WiFi band**:
   - ESP32-C5 supports 2.4 GHz and 5 GHz
   - Verify your router broadcasts on 2.4 GHz
   - Some networks use separate SSIDs for bands

3. **Signal strength**:
   - Move gateway closer to router
   - Remove physical obstacles
   - Check antenna connection

4. **Router compatibility**:
   - Ensure WPA2/WPA3 enabled (not WEP)
   - Check MAC filtering isn't blocking device
   - Try disabling 802.11w (PMF) if enabled

### Symptom: WiFi Connects Then Disconnects

**Check Logs:**
```
[WIFI] WiFi connected! IP: 192.168.1.100
[WIFI] WiFi disconnected! Reason: AUTH_EXPIRE
```

**Solutions:**

1. **Power supply issues**:
   - Check USB cable quality
   - Use powered USB hub if needed
   - Verify 5V/1A minimum power

2. **Router issues**:
   - Increase DHCP lease time
   - Reserve IP address for gateway
   - Disable client isolation if enabled
   - Update router firmware

3. **Interference**:
   - Change WiFi channel
   - Move away from microwave/Bluetooth devices
   - Check for channel congestion

4. **Increase retry count**:
   ```bash
   idf.py menuconfig
   # WiFi Configuration → Maximum WiFi Retry: 10
   ```

### Symptom: Slow WiFi Connection

**Solutions:**

1. **Check signal strength**:
   ```bash
   # Via serial monitor
   # Look for: [WIFI] RSSI: -XX dBm
   # Good: > -50 dBm
   # Fair: -50 to -70 dBm
   # Poor: < -70 dBm
   ```

2. **Optimize placement**:
   - Reduce distance to router
   - Elevate gateway off ground
   - Use external antenna

3. **Router settings**:
   - Enable WiFi 6 if supported
   - Set channel width to 20 MHz for stability
   - Disable band steering

## MQTT Broker Issues

### Symptom: Cannot Connect to MQTT Broker

**Check Logs:**
```
[MQTT] Connecting to MQTT broker...
[MQTT] Connection failed: MQTT_CONNECTION_REFUSED
```

**Solutions:**

1. **Verify broker is running**:
   ```bash
   # Test with mosquitto_pub
   mosquitto_pub -h localhost -t test -m "hello"

   # Check broker logs
   sudo journalctl -u mosquitto -f
   ```

2. **Check broker URL**:
   ```bash
   idf.py menuconfig
   # MQTT Configuration → MQTT Broker URL
   # Format: mqtt://192.168.1.100
   ```

3. **Verify port**:
   ```bash
   # Default: 1883 (MQTT), 8883 (MQTTS)
   # MQTT Configuration → MQTT Broker Port: 1883
   ```

4. **Check credentials**:
   ```bash
   # If broker requires authentication
   # MQTT Configuration → MQTT Username
   # MQTT Configuration → MQTT Password
   ```

5. **Firewall rules**:
   ```bash
   # Allow MQTT port
   sudo ufw allow 1883/tcp
   ```

### Symptom: MQTT Connects Then Disconnects

**Check Logs:**
```
[MQTT] MQTT connected successfully!
[MQTT] MQTT disconnected callback
```

**Solutions:**

1. **Client ID conflict**:
   - Ensure unique client ID per device
   - Firmware auto-appends MAC address
   - Check for duplicate gateways

2. **Keep-alive timeout**:
   ```bash
   idf.py menuconfig
   # MQTT Configuration → MQTT Keep Alive: 120 seconds
   ```

3. **QoS issues**:
   - Reduce QoS level if messages are large
   - Check broker max message size

4. **Broker resource limits**:
   - Check broker connection limits
   - Monitor broker CPU/memory usage
   - Increase broker max_connections

### Symptom: MQTT Messages Not Received

**Solutions:**

1. **Verify subscription**:
   ```bash
   # Check what topics are subscribed
   # Via serial monitor:
   [MQTT] Subscribed to: zigbee2mqtt/#
   ```

2. **Check QoS level**:
   ```bash
   # Set QoS to 1 for guaranteed delivery
   idf.py menuconfig
   # MQTT Configuration → MQTT QoS Level: 1
   ```

3. **Check topic format**:
   ```bash
   # Correct:
   zigbee2mqtt/bridge/state

   # Wrong:
   zigbee2mqtt//bridge/state  # Extra slash
   Zigbee2mqtt/bridge/state    # Wrong case
   ```

4. **ACL permissions** (if broker has ACLs):
   ```bash
   # Check mosquitto ACL file
   # Ensure client has read/write permissions
   ```

## Zigbee Pairing Failures

### Symptom: Device Won't Pair

**Check Pairing Mode:**
```bash
# Verify pairing is enabled
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/info" -C 1

# Look for: "permit_join": true
```

**Solutions:**

1. **Enable pairing**:
   ```bash
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/permit_join" \
     -m '{"value": true}'
   ```

2. **Device-specific pairing**:
   - **Light bulbs**: Power cycle 5-6 times
   - **Sensors**: Hold button 5-10 seconds until LED flashes
   - **Switches**: Consult manufacturer instructions
   - Try factory reset on device

3. **Proximity**:
   - Bring device close to gateway (< 1 meter)
   - Pair close, then move to final location

4. **Channel interference**:
   ```bash
   # Change Zigbee channel
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
     -m '{"zigbee_channel": 20}'
   # Requires restart
   ```

5. **Network capacity**:
   - Check current device count
   - Increase max children if needed:
   ```bash
   idf.py menuconfig
   # Zigbee Configuration → Maximum Zigbee Children: 30
   ```

### Symptom: Device Pairs But Doesn't Respond

**Solutions:**

1. **Signal strength**:
   - Check linkquality in device messages (should be > 50)
   - Add router devices between coordinator and device
   - Reposition gateway or device

2. **Device timeout**:
   - Some devices go to deep sleep
   - Try waking device (press button)
   - Check device battery level

3. **Incompatible firmware**:
   - Update device firmware if possible
   - Check device compatibility list
   - Try re-pairing

4. **Network issues**:
   - Too many devices on network
   - Network congestion
   - Try removing and re-pairing device

### Symptom: Pairing Takes Too Long

**Solutions:**

1. **Reduce distance**:
   - Pair within 1 meter of gateway

2. **Remove interference**:
   - Turn off microwave, Bluetooth devices
   - Move away from WiFi router

3. **Factory reset device**:
   - Reset device to clear old network data
   - Retry pairing

## Bluetooth Issues 🔵

> **Note**: These issues apply when `CONFIG_ENABLE_BLUETOOTH_GATEWAY=y`

### Symptom: BLE Scanner Not Discovering Devices

**Check Scanner Status:**
```bash
# Via serial monitor
idf.py monitor

# Look for:
I (12345) BLE_SCANNER: BLE scanner started
I (12346) BLE_SCANNER: Scan interval: 1000ms, window: 100ms
```

**Common Causes:**

1. **BLE Scanner Disabled**:
   ```bash
   idf.py menuconfig
   # Bluetooth Gateway Configuration → BLE Scanner Enabled: [*]
   ```

2. **RSSI Threshold Too High**:
   ```bash
   # Lower threshold to detect distant devices
   idf.py menuconfig
   # Bluetooth Gateway → RSSI Threshold: -90 (from -80)
   ```

3. **Bluetooth Stack Not Initialized**:
   - Check logs for `E (xxx) BT_INIT: Bluetooth init failed`
   - Memory issue - see [Memory Issues](#memory-issues)

**Solutions:**

1. **Increase scan window**:
   ```bash
   idf.py menuconfig
   # Bluetooth Gateway → BLE Scan Window: 200ms (from 100ms)
   ```

2. **Verify device is BLE-capable**:
   - Classic Bluetooth devices won't appear
   - Only BLE 4.0+ devices supported

### Symptom: Cannot Connect to BLE Sensor (Xiaomi, Govee)

**Check Connection Status:**
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bridge/event"

# Look for:
{
  "type": "ble_connect_failed",
  "data": {
    "mac": "A4:C1:38:XX:XX:XX",
    "reason": "timeout"
  }
}
```

**Common Causes:**

1. **Device Out of Range**: RSSI < -90 dBm
2. **Device Already Connected**: Another app/gateway connected
3. **Authentication Required**: Device needs pairing button press
4. **Memory Exhausted**: Too many GATT connections

**Solutions:**

1. **Move device closer**: RSSI should be > -70 dBm
2. **Disconnect from other apps**: Unbind from Mi Home, etc.
3. **Press pairing button**: Follow device manual
4. **Reduce active connections**:
   ```bash
   idf.py menuconfig
   # Bluetooth Gateway → Max Tracked Devices: 30 (reduce from 50)
   ```

### Symptom: BLE Device Data Not Updating

**Check Update Interval:**
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bluetooth/bedroom_temp_sensor"

# Should update every 60-120 seconds for Xiaomi sensors
```

**Common Causes:**

1. **Device Battery Low**: < 10%
2. **Connection Lost**: Device moved out of range
3. **Proxy Task Crashed**: Check logs

**Solutions:**

1. **Replace battery**
2. **Check RSSI**: Should be > -80 dBm
3. **Restart gateway**:
   ```bash
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/restart" -m '{}'
   ```

## WiFi/BT/Zigbee Coexistence Problems 🔵

### Symptom: WiFi Disconnects When Bluetooth Active

**Common Causes:**

1. **WiFi on 2.4 GHz**: Interference with BT/Zigbee
2. **Coexistence Mode Wrong**: WiFi not prioritized
3. **Router Doesn't Support 5 GHz**: Forced to 2.4 GHz

**Solutions:**

1. **Enable 5 GHz WiFi** (CRITICAL):
   ```bash
   idf.py menuconfig
   # WiFi Coexistence → Prefer 5GHz WiFi: [*]
   # WiFi Configuration → SSID: YourNetwork_5G
   ```

2. **Adjust coexistence mode**:
   ```bash
   idf.py menuconfig
   # WiFi Coexistence → Coexistence Mode: 0 (WiFi Priority)
   ```

3. **Reduce BT scan duty cycle**:
   ```bash
   idf.py menuconfig
   # WiFi Coexistence → BT Scan Duty Cycle: 30% (from 50%)
   ```

### Symptom: Zigbee Devices Become Unreliable with Bluetooth Enabled

**Check Channel Overlap:**
```bash
# Via serial monitor - look for:
I (xxx) ZB_NETWORK: Zigbee channel: 11
I (xxx) WIFI_MGR: WiFi connected, channel: 6 (2.4GHz)
```

**Problem**: Zigbee channel 11 overlaps with WiFi channel 6

**Solutions:**

1. **Change Zigbee to non-overlapping channel**:
   ```bash
   idf.py menuconfig
   # Zigbee Configuration → Channel: 20 (or 15, 25)
   ```

2. **Use WiFi 5 GHz**:
   ```bash
   idf.py menuconfig
   # WiFi Coexistence → Prefer 5GHz WiFi: [*]
   ```

3. **Enable channel mask**:
   ```bash
   idf.py menuconfig
   # WiFi Coexistence → Zigbee Channel Mask: 0x02108000 (channels 15,20,25)
   ```

### Symptom: High Packet Loss on All Radios

**Check CPU Load:**
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/info"

# Look for:
{
  "system": {
    "cpu_usage": 85,  # Too high!
    "free_heap": 45000  # Low!
  }
}
```

**Solutions:**

1. **Reduce device limits**:
   ```bash
   idf.py menuconfig
   # Gateway Configuration → Maximum Zigbee Devices: 30 (from 50)
   # Bluetooth Gateway → Max Tracked Devices: 30 (from 50)
   ```

2. **Increase BLE scan interval**:
   ```bash
   idf.py menuconfig
   # Bluetooth Gateway → BLE Scan Interval: 2000ms (from 1000ms)
   ```

3. **Disable verbose logging**:
   ```bash
   idf.py menuconfig
   # Debug and Logging → Enable Verbose Logging: [ ]
   ```

## ESPHome API Issues 🔵

### Symptom: Home Assistant Can't Discover ESPHome Device

**Check API Server Status:**
```bash
# Via serial monitor
idf.py monitor

# Look for:
I (xxx) ESPHOME_API: API server started on port 6053
I (xxx) ESPHOME_API: Waiting for connections...
```

**Common Causes:**

1. **API Disabled**:
   ```bash
   idf.py menuconfig
   # ESPHome API Configuration → Enable ESPHome API: [*]
   ```

2. **Firewall Blocking Port 6053**:
   ```bash
   # Test connectivity
   telnet <gateway_ip> 6053
   ```

3. **mDNS Not Working**: Discovery relies on mDNS

**Solutions:**

1. **Manual IP entry**: Use gateway IP directly in HA
2. **Check mDNS**:
   ```bash
   # On Linux
   avahi-browse -a | grep esphome
   ```

### Symptom: ESPHome API Connection Drops

**Check Logs:**
```bash
# In Home Assistant logs:
# Settings → System → Logs → Filter: esphome

# Look for:
# ERROR ESPHome API connection timed out
```

**Common Causes:**

1. **API Timeout Too Short**
2. **Memory Pressure**: Heap < 40KB
3. **WiFi Instability**

**Solutions:**

1. **Increase timeout**:
   ```bash
   idf.py menuconfig
   # ESPHome API → Reboot Timeout: 30000ms (from 15000ms)
   ```

2. **Monitor memory**:
   ```bash
   mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/info"
   # free_heap should be > 50KB
   ```

## Device Communication Issues

### Symptom: Device Shows Offline

**Check Device State:**
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/{device}/availability"

# Output: offline or online
```

**Solutions:**

1. **Battery level**:
   - Check battery percentage
   - Replace batteries if low

2. **Signal strength**:
   - Check linkquality (should be > 50)
   - Move device closer or add routers

3. **Device sleep mode**:
   - Battery devices sleep to save power
   - Press button to wake
   - Wait for check-in interval

4. **Re-pair device**:
   - Remove device
   - Factory reset
   - Re-pair to network

### Symptom: Commands Don't Work

**Check Logs:**
```
[MQTT] Received command for device: living_room_light
[ZB] Failed to send command: ESP_ERR_TIMEOUT
```

**Solutions:**

1. **Verify device is online**:
   ```bash
   mosquitto_sub -h localhost -t "zigbee2mqtt/{device}"
   ```

2. **Check command format**:
   ```bash
   # Correct:
   mosquitto_pub -h localhost -t "zigbee2mqtt/light/set" \
     -m '{"state": "ON"}'

   # Wrong:
   mosquitto_pub -h localhost -t "zigbee2mqtt/light/set" \
     -m 'ON'  # Not JSON
   ```

3. **Device capability**:
   - Verify device supports command
   - Check device model capabilities

4. **Network routing**:
   - Ensure route exists to device
   - Add router devices if needed

### Symptom: Delayed Responses

**Solutions:**

1. **Check network hops**:
   - More hops = more delay
   - Optimize device placement

2. **Reduce polling interval**:
   ```bash
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
     -m '{"device_publish_interval_ms": 500}'
   ```

3. **WiFi/MQTT latency**:
   - Check network latency
   - Ensure MQTT broker is responsive

## Memory Issues

### Symptom: Out of Memory Errors

**Check Memory:**
```
[MEMORY] Heap free: 25000 bytes
[MEMORY] WARNING: Low memory!
```

**Solutions:**

1. **View memory stats**:
   ```bash
   mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/system"
   # Check: free_heap, min_free_heap
   ```

2. **Reduce device count**:
   ```bash
   idf.py menuconfig
   # Gateway Configuration → Maximum Zigbee Devices: 30
   # Zigbee Configuration → Maximum Zigbee Children: 15
   ```

3. **Disable features**:
   ```bash
   # Disable HA discovery if not needed
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
     -m '{"ha_discovery_enabled": false}'

   # Disable bridge logging
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
     -m '{"bridge_logging_enabled": false}'
   ```

4. **Reduce logging**:
   ```bash
   idf.py menuconfig
   # Debug and Logging → Enable Verbose Logging: [ ]
   # Debug and Logging → Enable Task Statistics: [ ]
   ```

5. **Optimize build**:
   ```bash
   idf.py menuconfig
   # Component config → Log output → Default log verbosity: Warning
   ```

### Symptom: Memory Fragmentation

**Check Fragmentation:**
```
[MEMORY] Heap fragmentation: 35%
```

**Solutions:**

1. **Restart gateway periodically**:
   ```bash
   # Schedule weekly restart via automation
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/restart" -m '{}'
   ```

2. **Monitor heap**:
   ```bash
   idf.py menuconfig
   # Debug and Logging → Log Memory Statistics: [*]
   # Debug and Logging → Memory Log Interval: 30 seconds
   ```

## Boot Loops and Crashes

### Symptom: Device Keeps Rebooting

**Check Boot Logs:**
```bash
idf.py monitor

# Look for:
Guru Meditation Error
abort() was called
assert failed
Stack overflow
```

**Solutions:**

1. **Power supply**:
   - Use quality USB cable
   - Ensure 5V/1A minimum
   - Try powered USB hub

2. **Corrupted flash**:
   ```bash
   # Erase flash and reflash
   idf.py erase-flash
   idf.py flash
   ```

3. **Configuration issue**:
   ```bash
   # Reset to defaults
   idf.py erase-flash flash monitor
   # Reconfigure via menuconfig
   ```

4. **Stack overflow**:
   ```bash
   # Increase stack sizes
   idf.py menuconfig
   # Component config → FreeRTOS → Main task stack size: 8192
   ```

### Symptom: Watchdog Timeout

**Check Logs:**
```
Task watchdog timeout
- IDLE0
```

**Solutions:**

1. **CPU overload**:
   - Reduce device count
   - Increase intervals
   - Disable verbose logging

2. **Blocking operations**:
   - Check for infinite loops in code
   - Verify no blocking delays in critical sections

3. **Increase watchdog timeout**:
   ```bash
   idf.py menuconfig
   # Component config → ESP System Settings → Task Watchdog timeout: 10s
   ```

### Symptom: Brown-out Detector (BOD) Triggered

**Check Logs:**
```
Brownout detector was triggered
```

**Solutions:**

1. **Insufficient power**:
   - Use power supply rated for 1A minimum
   - Check voltage drops during TX bursts

2. **Capacitance**:
   - Add bulk capacitor (100-470µF) near ESP32-C5
   - Add bypass capacitor (0.1µF)

3. **Disable BOD** (not recommended):
   ```bash
   idf.py menuconfig
   # Component config → Hardware Settings → Brown-out Detector: [ ]
   ```

## Performance Issues

### Symptom: Slow MQTT Publishing

**Solutions:**

1. **Reduce publish interval**:
   ```bash
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
     -m '{"device_publish_interval_ms": 500}'
   ```

2. **Increase QoS**:
   ```bash
   idf.py menuconfig
   # MQTT Configuration → MQTT QoS Level: 1
   ```

3. **Check broker performance**:
   - Monitor broker CPU/memory
   - Check broker logs for errors

### Symptom: High CPU Usage

**Check Stats:**
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/system"
# Look for: "cpu_usage": 85  # Should be < 50%
```

**Solutions:**

1. **Reduce logging**:
   ```bash
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
     -m '{"log_level": 2}'  # Warnings only
   ```

2. **Increase intervals**:
   ```bash
   mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/config_set" \
     -m '{
       "device_publish_interval_ms": 2000,
       "bridge_publish_interval": 600
     }'
   ```

3. **Disable features**:
   - Disable HA discovery
   - Disable bridge logging
   - Disable task statistics

## Log Analysis

### Key Log Messages

#### Normal Operation

```
[MAIN] System Initialization Complete
[WIFI] WiFi connected! IP: 192.168.1.100
[MQTT] MQTT connected successfully!
[ZIGBEE] Zigbee coordinator started successfully
[CORE] MQTT bridge started successfully
```

#### Warnings

```
[MEMORY] WARNING: Low memory! (Free: 45KB)
[WIFI] WiFi signal weak (RSSI: -75 dBm)
[MQTT] MQTT publish queue full
[ZB] Device timeout: 0x00158d0001a2b3c4
```

#### Errors

```
[ERROR] Failed to initialize NVS: ESP_ERR_NVS_NOT_FOUND
[ERROR] MQTT connection failed: MQTT_CONNECTION_REFUSED
[ERROR] Zigbee command failed: ESP_ERR_TIMEOUT
[ERROR] Out of memory! Cannot allocate 2048 bytes
```

### Log Filtering

Save logs to file:
```bash
idf.py monitor > gateway.log
```

Filter for errors:
```bash
cat gateway.log | grep ERROR
cat gateway.log | grep -E "ERROR|WARNING"
```

Analyze specific module:
```bash
cat gateway.log | grep "\[MQTT\]"
cat gateway.log | grep "\[ZIGBEE\]"
```

### Debug Tips

1. **Enable timestamp in logs**:
   ```bash
   idf.py menuconfig
   # Component config → Log output → Show timestamp in log: [*]
   ```

2. **Increase log buffer**:
   ```bash
   idf.py menuconfig
   # Component config → Log output → Log buffer size: 1024
   ```

3. **Module-specific logging**:
   ```c
   // In sdkconfig:
   CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
   CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y
   ```

## ESP-Zigbee-SDK v1.6.x Issues

> **Quick Reference**: See [CLAUDE.md](../CLAUDE.md#esp-zigbee-sdk-v16x-patterns) for a summary of essential SDK patterns.

### Thread-Safety Problems

**Symptom:** Crash or hang when handling MQTT commands

**Cause:** Zigbee API is called without acquiring the lock

**Solution:**
```c
esp_zb_lock_acquire(portMAX_DELAY);
// Zigbee API call
esp_zb_lock_release();
```

**Affected Functions:**
- All `esp_zb_zcl_*_cmd_req()`
- `esp_zb_zcl_read/write_attr_cmd_req()`

### Action Handler Not Registered

**Symptom:** ZCL commands are not being processed

**Cause:** `esp_zb_core_action_handler_register()` is missing

**Solution:** Register in `zb_callbacks_init()`:
```c
esp_zb_core_action_handler_register(zb_coordinator_action_handler);
```

### Network Configuration Problems

**Symptom:** Only a few devices can join the network

**Cause:** Default network size is too small

**Solution:** Call BEFORE `esp_zb_init()`:
```c
esp_zb_overall_network_size_set(100);
esp_zb_io_buffer_size_set(128);
```

## Getting Help

If issues persist:

1. **Collect information**:
   - Full boot log (from idf.py monitor)
   - Configuration (via config_get)
   - System stats (via health_check)
   - MQTT message dumps

2. **GitHub Issues**:
   - Open issue with logs and configuration
   - Describe steps to reproduce
   - Include hardware version

3. **Community Forums**:
   - ESP32 forums
   - Home Assistant community
   - Reddit: r/homeassistant, r/esp32

4. **Serial dumps**:
   ```bash
   idf.py monitor > issue_log.txt
   # Reproduce issue
   # Ctrl+] to exit
   # Share issue_log.txt
   ```

## Prevention

### Regular Maintenance

1. **Monitor health**:
   - Check system stats daily
   - Monitor memory usage
   - Check WiFi signal strength

2. **Update firmware**:
   - Check for updates monthly
   - Test updates in non-production first
   - Keep backups of working versions

3. **Backup configuration**:
   ```bash
   mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/response/config_get" \
     -C 1 > config_backup.json
   ```

4. **Document changes**:
   - Keep log of configuration changes
   - Note device additions/removals
   - Track firmware versions

### Best Practices

- Use quality power supply
- Maintain good WiFi signal
- Keep device count reasonable
- Monitor memory usage
- Regular restarts (weekly)
- Keep firmware updated
- Document network layout

## Quick Reference

| Symptom | First Check | Quick Fix |
|---------|-------------|-----------|
| No WiFi | Credentials | Verify SSID/password |
| No MQTT | Broker URL | Check broker is running |
| Can't pair | Permit join | Enable pairing mode |
| Device offline | Battery | Replace batteries |
| Low memory | Device count | Reduce devices or features |
| Crashes | Power supply | Use 1A+ power supply |
| Slow performance | Logs | Reduce log level |

## Advanced Diagnostics

For developers, see:
- [Development Guide](DEVELOPMENT.md) - Debugging techniques
- [Architecture](ARCHITECTURE.md) - System internals
- [Memory Optimization](MEMORY_OPTIMIZATION.md) - Memory debugging
