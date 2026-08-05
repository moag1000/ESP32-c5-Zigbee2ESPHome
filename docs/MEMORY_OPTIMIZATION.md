# Memory Optimization Guide

<!-- staleness-banner -->
> **Stand 2026-08-05.** Die BLE-Zahlen sind ueberholt — BLE ist aus, was rund 30KB internes RAM
> freigibt (auf Hardware nicht nachgemessen). `core/memory_pool.c` wurde am
> 2026-08-05 geloescht; die genutzten Pools sind `buffer_pool_t` aus
> `memory_manager_ng`.
>
> Aktuell gepflegt wird `CLAUDE.md` im Projektwurzelverzeichnis.


> **Authoritative Source**: This document is the comprehensive reference for memory optimization in the ESP32-C5 Unified Gateway. [CLAUDE.md](../CLAUDE.md#memory-constraints) provides a brief summary.

Guide to understanding and optimizing memory usage in the ESP32-C5 Unified Gateway (Zigbee2MQTT + Bluetooth + ESPHome API).

## Table of Contents

- [Memory Overview](#memory-overview)
- [Memory Budget](#memory-budget)
- [Monitoring Memory](#monitoring-memory)
- [Optimization Strategies](#optimization-strategies)
- [Common Issues](#common-issues)
- [PSRAM Usage](#psram-usage)

## Memory Overview

### ESP32-C5 Memory Types

| Type | Size | Speed | Purpose |
|------|------|-------|---------|
| **SRAM** | 384 KB | Fast | Code, data, heap, stacks |
| **PSRAM** | 8 MB | Slower | Large buffers, optional |
| **Flash** | 8 MB | Slow | Firmware, data storage |
| **RTC RAM** | 16 KB | Fast | RTC, deep sleep |

### Memory Regions

```
IRAM (Instruction RAM)
├── Bootloader: ~32 KB
├── ROM code: ~128 KB
└── App code: Variable

DRAM (Data RAM) - 384 KB total

Zigbee-Only Configuration (✅ Current):
├── FreeRTOS kernel: ~32 KB
├── WiFi stack: ~50 KB
├── MQTT buffers: ~20 KB
├── Zigbee stack: ~80 KB
├── Task stacks: ~30 KB
├── Static data: ~40 KB
└── Heap: ~130 KB (target: >50KB free)

Zigbee + Bluetooth Configuration (📋 Phase 10-14):
├── FreeRTOS kernel: ~32 KB
├── WiFi stack: ~55 KB (+5KB coexistence)
├── MQTT buffers: ~20 KB
├── Zigbee stack: ~80 KB
├── Bluetooth LE stack: ~40 KB 🔵
├── ESPHome API: ~35 KB 🔵
├── Task stacks: ~40 KB (+10KB BT tasks)
├── Static data: ~42 KB
└── Heap: ~60 KB (WARNING: tight!) ⚠️
```

## Memory Budget

### Zigbee-Only Configuration (50 devices)

```
Component              Size      Percentage
─────────────────────────────────────────
FreeRTOS Kernel       32 KB      8%
WiFi Stack            50 KB      13%
Zigbee Stack          80 KB      21%
MQTT Buffers          20 KB      5%
Task Stacks           30 KB      8%
Device Tables         100 KB     26%
Static Data           22 KB      6%
Free Heap (minimum)   50 KB      13%
─────────────────────────────────────────
Total Used            334 KB     87%
Total Available       384 KB     100%
```

### Zigbee + Bluetooth Configuration (30 Zigbee + 50 BT devices) 🔵

```
Component              Size      Percentage
─────────────────────────────────────────
FreeRTOS Kernel       32 KB      8%
WiFi Stack            55 KB      14%
Zigbee Stack          80 KB      21%
Bluetooth LE Stack    40 KB      10% 🔵
ESPHome API           35 KB      9% 🔵
MQTT Buffers          20 KB      5%
Task Stacks           40 KB      10%
Device Tables         42 KB      11% (PSRAM offloaded)
Static Data           20 KB      5%
Free Heap (minimum)   40 KB      10% ⚠️
─────────────────────────────────────────
Total Used            344 KB     90%
Total Available       384 KB     100%
```

**Critical Thresholds**:
- 🟢 **Safe**: Free heap > 60 KB
- 🟡 **Warning**: Free heap 40-60 KB
- 🔴 **Critical**: Free heap < 40 KB (instability likely)

> ⚠️ **IMPORTANT**: All memory estimates are **THEORETICAL** and not verified with actual builds.
>
> **Before production use**:
> - Build actual firmware and measure real memory usage
> - Test under maximum load (all devices connected)
> - Monitor for heap fragmentation and OOM errors
> - Add memory monitoring and crash reporting
>
> **Recommendation**: Start with conservative device limits (20 Zigbee + 20 BLE) and increase gradually after stability testing.

### Per-Device Memory Cost

**Zigbee Devices**:
- Device entry: 2 KB
- Routing table entry: 256 bytes
- State cache: 512 bytes
- **Total**: ~2.7 KB per device

**Bluetooth Devices** 🔵:
- Device entry: 400 bytes (PSRAM)
- RSSI tracking: 128 bytes
- State cache: 256 bytes
- **Total**: ~0.8 KB per device (mostly in PSRAM)

**Scalability**:
- Zigbee-only (50 devices): ~135 KB
- Zigbee+BT (30+50 devices): ~42 KB (with PSRAM offloading)
- Zigbee+BT (20+30 devices): ~28 KB (reduced config)

## Monitoring Memory

### Runtime Monitoring

#### Via MQTT

```bash
# Subscribe to system stats
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/system"

# Output shows:
{
  "free_heap": 82000,
  "min_free_heap": 75000,
  "free_psram": 7900000,
  "heap_fragmentation": 8
}
```

#### Via Serial Monitor

```c
// In system_monitor.c
ESP_LOGI(TAG, "Free heap: %u bytes", esp_get_free_heap_size());
ESP_LOGI(TAG, "Min free heap: %u bytes", esp_get_minimum_free_heap_size());
ESP_LOGI(TAG, "Largest free block: %u bytes", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
```

### Heap Tracing

Enable heap tracing to find leaks:

```c
#include "esp_heap_trace.h"

#define NUM_RECORDS 100
static heap_trace_record_t trace_records[NUM_RECORDS];

void app_main(void) {
    // Initialize tracing
    ESP_ERROR_CHECK(heap_trace_init_standalone(trace_records, NUM_RECORDS));

    // Start tracing
    ESP_ERROR_CHECK(heap_trace_start(HEAP_TRACE_LEAKS));

    // Run your code...

    // Stop and dump
    ESP_ERROR_CHECK(heap_trace_stop());
    heap_trace_dump();
}
```

### Build-Time Analysis

```bash
# Overall size
idf.py size

# Component sizes
idf.py size-components

# Detailed size analysis
idf.py size-files

# Memory map
nm -S -n build/esp32-c5-zigbee2mqtt.elf | less
```

Output example:
```
Total sizes:
 DRAM .data size:   14234 bytes
 DRAM .bss  size:   26558 bytes
Used static DRAM:   40792 bytes ( 343144 available, 10.6% used)
```

## Optimization Strategies

### 1. Reduce Device Count

If running low on memory:

```bash
idf.py menuconfig
# Gateway Configuration → Maximum Zigbee Devices: 30 (reduce from 50)
# Zigbee Configuration → Maximum Zigbee Children: 15 (reduce from 20)
```

Impact: Saves ~2.7 KB per device removed

### 2. Optimize String Usage

**Problem**: String duplication wastes memory

```c
// Bad - Multiple copies
char *name1 = strdup("living_room_light");  // Allocates
char *name2 = strdup("living_room_light");  // Duplicates

// Good - String interning or constants
static const char *DEVICE_NAMES[] = {
    "living_room_light",
    "bedroom_sensor",
    // ...
};
```

### 3. Stack Size Tuning

Reduce stack sizes if not needed:

```bash
idf.py menuconfig
# Component config → FreeRTOS
#   → Main task stack size: 4096 (reduce from 8192 if safe)
#   → Task Watchdog timeout: Increase if reducing stacks
```

Check stack usage:
```c
UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(NULL);
ESP_LOGI(TAG, "Stack high water mark: %u bytes", stack_hwm * 4);
```

### 4. Buffer Size Optimization

MQTT and network buffers can be tuned:

```bash
idf.py menuconfig
# Component config → ESP MQTT Configuration
#   → Default MQTT Buffer Size: 2048 (reduce from 4096 if messages small)
```

### 5. Disable Unused Features

```bash
idf.py menuconfig

# Disable verbose logging
# Debug and Logging → Enable Verbose Logging: [ ]

# Disable task statistics if not needed
# Debug and Logging → Enable Task Statistics: [ ]

# Disable bridge logging to MQTT
# Gateway Configuration → Enable Bridge Logging to MQTT: [ ]
```

### 6. Use PSRAM for Large Buffers

Move large allocations to PSRAM:

```c
#include "esp_heap_caps.h"

// Allocate from PSRAM
void *buffer = heap_caps_malloc(LARGE_SIZE, MALLOC_CAP_SPIRAM);
if (buffer == NULL) {
    // Fall back to internal RAM
    buffer = malloc(LARGE_SIZE);
}

// Free when done
free(buffer);
```

### 7. Lazy Initialization

Don't allocate memory until needed:

```c
static device_table_t *s_device_table = NULL;

esp_err_t device_table_init(void) {
    if (s_device_table != NULL) {
        return ESP_OK;  // Already initialized
    }

    s_device_table = calloc(MAX_DEVICES, sizeof(device_entry_t));
    if (s_device_table == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
```

### 8. Memory Pooling

Pre-allocate pools for frequent allocations:

```c
#define POOL_SIZE 10
static message_t message_pool[POOL_SIZE];
static uint8_t pool_used[POOL_SIZE];

message_t* message_alloc(void) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            return &message_pool[i];
        }
    }
    return NULL;  // Pool exhausted
}

void message_free(message_t *msg) {
    int index = msg - message_pool;
    if (index >= 0 && index < POOL_SIZE) {
        pool_used[index] = 0;
    }
}
```

### 9. Reduce JSON Buffer Sizes

JSON parsing can use significant memory:

```c
// Use stack for small JSON
char json[256];
snprintf(json, sizeof(json), "{\"state\":\"%s\"}", state);

// For large JSON, allocate dynamically and free immediately
cJSON *root = cJSON_Parse(large_json);
// Use root...
cJSON_Delete(root);  // Free immediately
```

### 10. Compile-Time Optimizations

```bash
idf.py menuconfig

# Compiler options → Optimization Level
#   → Optimize for size (-Os)  # Instead of -O2

# Component config → Log output
#   → Default log verbosity: Warning  # Reduces code size
```

## Common Issues

### Issue 1: Heap Exhaustion

**Symptoms**:
```
E (12345) MEMORY: Failed to allocate 2048 bytes
E (12346) MAIN: Out of memory
abort() was called at PC 0x40084abc
```

**Solutions**:
1. Check for memory leaks
2. Reduce device count
3. Reduce buffer sizes
4. Enable PSRAM usage
5. Reduce logging verbosity

### Issue 2: Stack Overflow

**Symptoms**:
```
***ERROR*** A stack overflow in task zb_main has been detected.
abort() was called at PC 0x400d4e3c
```

**Solutions**:
```c
// Increase stack size
xTaskCreate(task_function, "task_name",
            8192,  // Increase from 4096
            NULL, 5, NULL);

// Or in menuconfig for main task
```

### Issue 3: Fragmentation

**Symptoms**:
```
E (45678) HEAP: Failed to allocate 8192 bytes (fragmented heap)
Free heap: 50000 bytes, but largest block: 4096 bytes
```

**Solutions**:
1. Allocate large buffers early
2. Use memory pools
3. Restart gateway periodically
4. Avoid frequent large allocations/deallocations

### Issue 4: WiFi Stack Memory

**Symptoms**:
```
E (1234) WIFI: WiFi Task: out of memory
```

**Solutions**:
```bash
idf.py menuconfig
# Component config → Wi-Fi
#   → WiFi Task Core ID: 0
#   → WiFi IRAM optimization: [ ]  # Reduces IRAM but uses DRAM
```

## PSRAM Usage

### Enabling PSRAM

```bash
idf.py menuconfig
# Component config → ESP32C5-Specific
#   → Support for external, SPI-connected RAM: [*]
#   → SPI RAM config
#     → Initialize SPI RAM during startup: [*]
#     → SPI RAM access method: Integrate RAM into memory map
```

### Allocating from PSRAM

```c
#include "esp_heap_caps.h"

// Allocate specifically from PSRAM
void *psram_buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);

// Allocate from either (prefers internal)
void *any_buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);

// Check where memory was allocated
if (heap_caps_get_allocated_size(buf) > 0) {
    if (heap_caps_match(buf, MALLOC_CAP_SPIRAM)) {
        ESP_LOGI(TAG, "Allocated in PSRAM");
    } else {
        ESP_LOGI(TAG, "Allocated in internal RAM");
    }
}
```

### PSRAM Considerations

**Pros**:
- 8 MB available (large capacity)
- Extends usable memory significantly

**Cons**:
- Slower than internal SRAM (~4x)
- Not suitable for ISR or time-critical code
- Increases power consumption slightly

**Best Uses for PSRAM**:
- OTA firmware download buffer
- Large JSON documents
- Temporary file buffers
- Non-time-critical caches

**Avoid PSRAM for**:
- ISR handlers
- Time-critical networking
- Zigbee stack internals
- DMA buffers

## Memory Monitoring Code Example

Comprehensive memory monitoring:

```c
void memory_stats_print(void) {
    // Heap info
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "=== Memory Statistics ===");
    ESP_LOGI(TAG, "Total free: %u bytes", info.total_free_bytes);
    ESP_LOGI(TAG, "Total allocated: %u bytes", info.total_allocated_bytes);
    ESP_LOGI(TAG, "Largest free block: %u bytes", info.largest_free_block);
    ESP_LOGI(TAG, "Min free ever: %u bytes", info.minimum_free_bytes);
    ESP_LOGI(TAG, "Fragmentation: %.1f%%",
             100.0 * (1.0 - (float)info.largest_free_block / info.total_free_bytes));

    // PSRAM info
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        ESP_LOGI(TAG, "PSRAM free: %u bytes",
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    // Task stacks
    TaskStatus_t *task_array;
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    task_array = malloc(task_count * sizeof(TaskStatus_t));
    if (task_array != NULL) {
        uxTaskGetSystemState(task_array, task_count, NULL);
        ESP_LOGI(TAG, "=== Task Stack Usage ===");
        for (int i = 0; i < task_count; i++) {
            ESP_LOGI(TAG, "%s: %u bytes free",
                     task_array[i].pcTaskName,
                     task_array[i].usStackHighWaterMark * 4);
        }
        free(task_array);
    }
}
```

## Best Practices

1. **Monitor regularly**: Check memory stats during development and in production
2. **Test with load**: Stress test with maximum expected devices
3. **Profile before optimizing**: Measure first, optimize bottlenecks
4. **Design for growth**: Leave memory margin for future features
5. **Free promptly**: Don't hold allocations longer than needed
6. **Use const**: Mark read-only data as `const` to keep in flash
7. **Avoid deep nesting**: Deep call stacks waste memory
8. **Batch allocations**: Allocate multiple items together when possible

## Recommended Monitoring

For production deployments:

```bash
# Configure periodic monitoring
idf.py menuconfig
# Debug and Logging → Log Memory Statistics: [*]
# Debug and Logging → Memory Log Interval: 60 seconds

# Enable MQTT publishing of stats
# Gateway Configuration → Bridge Publish Interval: 300 seconds
```

Monitor via MQTT:
```bash
mosquitto_sub -h localhost -t "zigbee2mqtt/bridge/system" | \
    jq '{free_heap, min_free_heap, fragmentation}'
```

## Related Documentation

- [Architecture](ARCHITECTURE.md) - Memory architecture details
- [Troubleshooting](TROUBLESHOOTING.md) - Memory issue solutions
- [Development](DEVELOPMENT.md) - Debugging memory issues
