# WiFi/Bluetooth/Zigbee Coexistence Guide

> **Authoritative Source**: This document is the comprehensive reference for WiFi/Bluetooth/Zigbee coexistence configuration and optimization.

Complete guide to operating WiFi, Bluetooth LE, and Zigbee simultaneously on the ESP32-C5.

> **Status**: 📋 Phase 10-14 (Planned for Bluetooth integration)

> ⚠️ **IMPORTANT - Espressif Recommendation**: For production Wi-Fi based Thread Border Router or Zigbee Gateway products, **Espressif recommends using a dual-SoC solution** (e.g., ESP32-S3 + ESP32-H2) with separate antennas for better reliability and lower packet loss.
>
> **Single ESP32-C5 tri-radio operation**:
> - ✅ Suitable for: Learning, experimentation, hobbyist projects
> - ⚠️ Expected performance: 5-10% packet loss even with WiFi 5GHz
> - ❌ Not recommended for: Production deployments, mission-critical applications
>
> **Source**: [ESP-IDF Coexistence Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/coexist.html)

## Table of Contents

- [Overview](#overview)
- [Frequency Allocation](#frequency-allocation)
- [Hardware Coexistence](#hardware-coexistence)
- [Configuration](#configuration)
- [Optimization Strategies](#optimization-strategies)
- [Performance Metrics](#performance-metrics)
- [Troubleshooting](#troubleshooting)
- [Best Practices](#best-practices)

## Overview

The ESP32-C5 integrates **three wireless radios** that must share spectrum and hardware resources:

| Radio | Frequency | Standard | Bandwidth |
|-------|-----------|----------|-----------|
| **WiFi** | 2.4 GHz / 5 GHz | 802.11ax (WiFi 6) | 20/40/80 MHz |
| **Zigbee** | 2.4 GHz | 802.15.4 | 2 MHz |
| **Bluetooth** | 2.4 GHz | BLE 5.0 | 2 MHz (79 channels) |

### Key Challenge

**All three radios share the 2.4 GHz ISM band** (2400-2483.5 MHz):
- **79 Bluetooth channels** (2402-2480 MHz, 1 MHz spacing)
- **16 Zigbee channels** (11-26, 2405-2480 MHz, 5 MHz spacing)
- **14 WiFi channels** (1-14, 2412-2484 MHz, 20 MHz width)

### Critical Solution

**Use WiFi on 5 GHz band** whenever possible:
- ESP32-C5 supports **dual-band WiFi** (2.4 + 5 GHz)
- **5 GHz frees entire 2.4 GHz spectrum** for Zigbee and Bluetooth
- **Hardware arbitration** handles remaining 2.4 GHz conflicts

## Frequency Allocation

### 2.4 GHz Spectrum Map

```
2400 MHz                2.4 GHz ISM Band                 2483.5 MHz
│                                                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   Bluetooth LE (79 channels, 1 MHz spacing)                    │
│   ├───┬───┬───┬───┬───┬───┬───┬───┬─ ... ─┬───┬───┬───┬───┤   │
│   0   1   2   3   4   5   6   7   8       77  78  79  80      │
│   2402                                                   2480   │
│                                                                 │
│   WiFi (14 channels, 20 MHz width)                             │
│   ├─────────────────┤                                          │
│   │   Channel 1     │                                          │
│   2412          2432                                           │
│                     ├─────────────────┤                        │
│                     │   Channel 6     │                        │
│                     2437          2457                         │
│                                       ├─────────────────┤      │
│                                       │   Channel 11    │      │
│                                       2462          2482       │
│                                                                 │
│   Zigbee 802.15.4 (16 channels, 2 MHz width, 5 MHz spacing)   │
│         ├─┤   ├─┤   ├─┤   ├─┤   ├─┤   ├─┤   ├─┤   ├─┤        │
│         11  12  13  14  15  16  17  18  19  20  ... 26        │
│        2405     2415     2425     2435     2445     2475       │
└─────────────────────────────────────────────────────────────────┘
```

### Overlap Analysis

**WiFi Channel 1 (2412 MHz, 20 MHz width)**:
- Spans: 2402-2422 MHz
- **Overlaps**: Zigbee 11, 12, 13
- **Overlaps**: Bluetooth 0-20

**WiFi Channel 6 (2437 MHz, 20 MHz width)**:
- Spans: 2427-2447 MHz
- **Overlaps**: Zigbee 14, 15, 16, 17
- **Overlaps**: Bluetooth 25-45

**WiFi Channel 11 (2462 MHz, 20 MHz width)**:
- Spans: 2452-2472 MHz
- **Overlaps**: Zigbee 20, 21, 22, 23
- **Overlaps**: Bluetooth 50-70

### Recommended Channel Selection

#### Strategy 1: WiFi 5 GHz (BEST)

```
WiFi:     5 GHz (5150-5850 MHz) ✅ NO OVERLAP
Zigbee:   Channel 15, 20, or 25
Bluetooth: Adaptive Frequency Hopping (AFH)
```

**Result**: Zero 2.4 GHz WiFi interference

#### Strategy 2: WiFi 2.4 GHz (Fallback)

If 5 GHz unavailable:

**WiFi Channel 1**:
```
WiFi:     Channel 1 (2412 MHz)
Zigbee:   Channel 20 or 25 ✅ Minimal overlap
Bluetooth: AFH avoids WiFi channels automatically
```

**WiFi Channel 6**:
```
WiFi:     Channel 6 (2437 MHz)
Zigbee:   Channel 11 or 25 ✅ Minimal overlap
Bluetooth: AFH
```

**WiFi Channel 11**:
```
WiFi:     Channel 11 (2462 MHz)
Zigbee:   Channel 15 or 25 ✅ Minimal overlap
Bluetooth: AFH
```

## Hardware Coexistence

### ESP32-C5 Coexistence Mechanism

The ESP32-C5 includes **hardware arbitration** for radio access:

```
┌─────────────────────────────────────────────┐
│         Coexistence Arbitration             │
│                                             │
│   ┌───────┐   ┌──────────┐   ┌──────────┐ │
│   │ WiFi  │   │Bluetooth │   │  Zigbee  │ │
│   │Request│   │ Request  │   │ Request  │ │
│   └───┬───┘   └────┬─────┘   └────┬─────┘ │
│       │            │              │        │
│       ▼            ▼              ▼        │
│   ┌────────────────────────────────────┐  │
│   │    Priority-Based Scheduler        │  │
│   │  (Configurable arbitration mode)   │  │
│   └────────────────────────────────────┘  │
│                    │                       │
│                    ▼                       │
│   ┌────────────────────────────────────┐  │
│   │      2.4 GHz Radio Access          │  │
│   │   (Time-division multiplexing)     │  │
│   └────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
```

### Arbitration Modes

**Mode 0: WiFi Priority**
- WiFi gets priority over BT/Zigbee
- Use when WiFi throughput critical
- Zigbee/BT may experience delays

**Mode 1: Balanced (RECOMMENDED)**
- Equal priority for all radios
- Best for tri-radio operation
- Fair time allocation

**Mode 2: Bluetooth Priority**
- BT gets priority (audio use cases)
- Not recommended for gateway

**Mode 3: Zigbee Priority**
- Zigbee gets priority
- Use if Zigbee reliability critical

### Time-Slicing Example

In Balanced mode (1), radio access is time-sliced:

```
Time ──────────────────────────────────────▶
│ WiFi │ Zigbee │ BT │ WiFi │ Zigbee │ BT │...
  10ms    5ms    5ms   10ms    5ms    5ms
```

- WiFi gets longer slots (higher bandwidth)
- Zigbee/BT get shorter slots (lower bandwidth)
- Scheduler ensures all radios progress

## Configuration

### Essential Coexistence Settings

```bash
idf.py menuconfig
```

#### CONFIG_WIFI_PREFER_5GHZ

**CRITICAL**: Enable WiFi 5 GHz preference

```
WiFi Coexistence Configuration → Prefer 5GHz WiFi: [*]
```

- **Effect**: Gateway tries 5 GHz first, falls back to 2.4 GHz
- **Impact**: Eliminates WiFi 2.4 GHz interference
- **Requirement**: Router must support 5 GHz

#### CONFIG_COEXISTENCE_MODE

Set arbitration mode:

```
WiFi Coexistence Configuration → Coexistence Mode: 1
```

- **0**: WiFi Priority
- **1**: Balanced ✅ **RECOMMENDED**
- **2**: BT Priority
- **3**: Zigbee Priority

#### CONFIG_COEXISTENCE_PREFERENCE

WiFi band preference:

```
WiFi Coexistence Configuration → Coexistence Preference: 2
```

- **0**: WiFi 2.4 GHz only
- **1**: WiFi dual-band (no preference)
- **2**: WiFi 5 GHz preferred ✅ **RECOMMENDED**
- **3**: WiFi 5 GHz only (strict)

#### CONFIG_ZIGBEE_CHANNEL

Select non-overlapping Zigbee channel:

```
Zigbee Configuration → Channel: 20
```

Recommended channels: **15, 20, 25**

#### CONFIG_ZIGBEE_CHANNEL_MASK

Restrict Zigbee channels to avoid WiFi:

```
WiFi Coexistence Configuration → Zigbee Channel Mask: 0x02108000
```

- `0x02108000` = Channels 15, 20, 25 only
- `0x00008000` = Channel 15 only
- `0x00100000` = Channel 20 only
- `0x02000000` = Channel 25 only

#### CONFIG_COEXISTENCE_BT_SCAN_DUTY_CYCLE

BT scan duty cycle during coexistence:

```
WiFi Coexistence Configuration → BT Scan Duty Cycle: 40
```

- Range: 10-90%
- Lower = more time for WiFi/Zigbee
- Higher = more BT scanning
- **Recommended**: 30-50%

### Example Configuration (Optimal)

```bash
# WiFi on 5 GHz
CONFIG_WIFI_PREFER_5GHZ=y
CONFIG_COEXISTENCE_PREFERENCE=2

# Balanced arbitration
CONFIG_COEXISTENCE_MODE=1

# Zigbee on non-overlapping channel
CONFIG_ZIGBEE_CHANNEL=20
CONFIG_ZIGBEE_CHANNEL_MASK=0x02108000

# BT scan duty cycle
CONFIG_COEXISTENCE_BT_SCAN_DUTY_CYCLE=40
```

## Optimization Strategies

### Strategy 1: WiFi 5 GHz (Highest Priority)

**Steps**:
1. Configure router with separate 5 GHz SSID
2. Set `CONFIG_WIFI_PREFER_5GHZ=y`
3. Set `CONFIG_COEXISTENCE_PREFERENCE=2`
4. Verify connection on 5 GHz via logs

**Benefits**:
- **Zero 2.4 GHz WiFi interference**
- Zigbee and BT have full 2.4 GHz spectrum
- 10-15% performance improvement

**Verification**:
```bash
idf.py monitor

# Look for:
I (xxx) WIFI_MGR: Connected to SSID, channel: 36 (5GHz)
```

### Strategy 2: Zigbee Channel Optimization

**Steps**:
1. Scan WiFi environment:
```bash
# Linux
sudo iwlist wlan0 scan | grep Channel

# Or use WiFi analyzer app
```

2. Choose Zigbee channel based on WiFi:
   - WiFi ch 1 → Zigbee 15, 20, or 25
   - WiFi ch 6 → Zigbee 20 or 25
   - WiFi ch 11 → Zigbee 15 or 20

3. Configure:
```bash
CONFIG_ZIGBEE_CHANNEL=20
```

### Strategy 3: Reduce BT Scan Overhead

**Steps**:
1. Increase BT scan interval:
```bash
CONFIG_BT_SCAN_INTERVAL_MS=2000  # From 1000ms
```

2. Reduce scan window:
```bash
CONFIG_BT_SCAN_WINDOW_MS=100  # Keep at 100ms
```

3. Lower duty cycle:
```bash
CONFIG_COEXISTENCE_BT_SCAN_DUTY_CYCLE=30  # From 50%
```

**Impact**:
- Reduces BT 2.4 GHz usage by 40%
- More time for WiFi/Zigbee
- Slower BLE device discovery

### Strategy 4: Limit Device Counts

**Steps**:
1. Reduce Zigbee devices:
```bash
CONFIG_MAX_ZIGBEE_DEVICES=30  # From 50
```

2. Reduce BT devices:
```bash
CONFIG_BT_MAX_TRACKED_DEVICES=30  # From 50
```

**Benefits**:
- Lower memory usage
- Reduced CPU load
- Less radio contention

## Performance Metrics

> ⚠️ **DISCLAIMER**: All performance metrics below are **THEORETICAL ESTIMATES** based on similar projects and ESP-IDF documentation.
>
> **Actual performance depends on**:
> - Device count and update rates
> - Network conditions and interference
> - Firmware implementation details
> - Build configuration and optimizations
>
> **Before relying on these metrics**: Run load tests, profiling, and benchmarks with your specific configuration.

### Baseline (Zigbee-Only, WiFi 5 GHz)

| Metric | Value | Status |
|--------|-------|--------|
| WiFi Throughput | 40-60 Mbps | Estimated |
| Zigbee Packet Loss | < 1% | Estimated |
| CPU Usage | 38% | Estimated |
| Free Heap | 120 KB | Estimated |

### Tri-Radio (WiFi 5 GHz + Zigbee + BT)

| Metric | Value | Change | Status |
|--------|-------|--------|--------|
| WiFi Throughput | 35-55 Mbps | -10% | Estimated |
| Zigbee Packet Loss | < 2% | +1% | Estimated |
| BT Scan Rate | 1 Hz | Baseline | Estimated |
| CPU Usage | 68% | +30% | Estimated |
| Free Heap | 60 KB | -60 KB | Estimated |

### Tri-Radio (WiFi 2.4 GHz + Zigbee + BT)

| Metric | Value | Change | Status |
|--------|-------|--------|--------|
| WiFi Throughput | 15-25 Mbps | -60% ⚠️ | Estimated |
| Zigbee Packet Loss | 5-10% | +9% ⚠️ | Estimated |
| BT Scan Rate | 0.5 Hz | -50% ⚠️ | Estimated |
| CPU Usage | 73% | +35% | Estimated |
| Free Heap | 60 KB | -60 KB | Estimated |

**Conclusion**: WiFi 5 GHz is **CRITICAL** for acceptable performance.

## Troubleshooting

### High Packet Loss on All Radios

**Symptoms**:
- WiFi disconnects frequently
- Zigbee commands timeout
- BLE devices not discovered

**Check**:
```bash
idf.py monitor

# Look for:
W (xxx) COEX: High contention detected
E (xxx) WIFI: WiFi disconnect reason: 205 (coex issue)
```

**Solutions**:
1. **Enable WiFi 5 GHz** (CRITICAL)
2. Change Zigbee channel: 20 or 25
3. Reduce BT scan duty cycle: 30%
4. Lower device limits

### WiFi on 2.4 GHz Despite 5 GHz Config

**Check**:
```bash
idf.py monitor

# Look for:
I (xxx) WIFI_MGR: Connected, channel: 6 (2.4GHz)  # Wrong!
```

**Causes**:
- Router doesn't support 5 GHz
- 5 GHz SSID different from 2.4 GHz
- 5 GHz signal too weak

**Solutions**:
1. Verify router 5 GHz enabled
2. Check SSID configuration
3. Move gateway closer to router
4. Use separate 5 GHz SSID

### Zigbee Devices Drop When BT Active

**Check overlap**:
```bash
idf.py monitor

# Look for:
I (xxx) ZB: Zigbee channel: 11
I (xxx) WIFI: WiFi channel: 6
```

**Problem**: Zigbee ch 11 overlaps WiFi ch 6

**Solution**:
```bash
CONFIG_ZIGBEE_CHANNEL=20  # No overlap with WiFi ch 1/6/11
```

### BLE Scan Rate Drops Below 50%

**Check CPU**:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/info"

# Look for:
{
  "system": {
    "cpu_usage": 85  # Too high!
  }
}
```

**Solutions**:
1. Reduce device counts
2. Increase BT scan interval
3. Disable verbose logging

## Best Practices

### 1. Always Use WiFi 5 GHz

**Mandatory** for tri-radio operation:
```bash
CONFIG_WIFI_PREFER_5GHZ=y
CONFIG_COEXISTENCE_PREFERENCE=2
```

### 2. Plan Zigbee Channels

**Before deployment**:
1. Scan WiFi environment
2. Select Zigbee channel 15, 20, or 25
3. Avoid channels overlapping with WiFi

### 3. Start with Conservative Device Limits

**Initial configuration**:
```bash
CONFIG_MAX_ZIGBEE_DEVICES=20
CONFIG_BT_MAX_TRACKED_DEVICES=20
```

**Expand gradually** after verifying stability.

### 4. Monitor Performance Metrics

**Key metrics**:
- Free heap (should be > 50 KB)
- CPU usage (should be < 75%)
- Zigbee link quality (should be > 100)
- WiFi RSSI (should be > -70 dBm)

### 5. Use Balanced Coexistence Mode

**Recommended**:
```bash
CONFIG_COEXISTENCE_MODE=1  # Balanced
```

Only change if specific use case requires WiFi/BT/Zigbee priority.

### 6. Optimize BT Scan Parameters

**Production settings**:
```bash
CONFIG_BT_SCAN_INTERVAL_MS=1500   # Balance of responsiveness
CONFIG_BT_SCAN_WINDOW_MS=100      # 10% duty cycle
CONFIG_COEXISTENCE_BT_SCAN_DUTY_CYCLE=40
```

### 7. Test Under Load

**Before production**:
1. Pair maximum expected devices
2. Generate WiFi traffic (streaming)
3. Trigger Zigbee commands
4. Scan for BLE devices
5. Monitor metrics for 24+ hours

## Next Steps

- [Bluetooth Gateway Guide](BLUETOOTH_GATEWAY.md) - BLE device setup
- [ESPHome API Guide](ESPHOME_API.md) - ESPHome integration
- [Configuration Guide](CONFIGURATION.md) - Full configuration reference
- [Troubleshooting Guide](TROUBLESHOOTING.md) - Problem solving
