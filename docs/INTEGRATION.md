# Foundation System Integration Guide

This document explains how to integrate the new foundation system (`foundation_init.h`) into the existing `main.c`.

## Overview

The foundation system provides:
- **Phase 1:** Lifecycle manager, memory pressure monitor, event bus, device registry
- **Phase 2:** Zigbee adapter, MQTT adapter, BLE adapter
- **Phase 3:** HA Discovery NG (event-driven)

---

## 1. Where to Call `foundation_init()`

**Location:** After NVS init, before WiFi/Zigbee initialization.

In `main.c`, this should be placed at the end of **Phase 1** (around line 460), after all the existing core initializations but before the connection event group creation.

### Recommended Placement

```c
// main/main.c - Inside app_main()

/* ... existing Phase 1 code ... */

/* Initialize system monitor (Phase 5) */
ESP_LOGI(TAG_MAIN, "[INIT] System Monitor");
ret = system_monitor_init(false);
ESP_ERROR_CHECK(ret);

/* Start system monitoring task */
ret = system_monitor_start(SYSMON_INTERVAL_DEFAULT_SEC);
ESP_ERROR_CHECK(ret);

/* =========================================================
 * NEW: Foundation System Init (Phase 1-3 NG Components)
 * ========================================================= */
#include "core/foundation_init.h"

ESP_LOGI(TAG_MAIN, "[INIT] Foundation System (NG)");
ret = foundation_init();
if (ret != ESP_OK) {
    ESP_LOGW(TAG_MAIN, "Foundation init failed: %s (non-fatal)", esp_err_to_name(ret));
    /* Log which components failed */
    foundation_print_status();
} else {
    ESP_LOGI(TAG_MAIN, "Foundation system initialized successfully");
    foundation_print_status();
}
/* ========================================================= */

/* Create connection event group */
s_connection_event_group = xEventGroupCreate();
if (!s_connection_event_group) {
    ESP_LOGE(TAG_MAIN, "Failed to create connection event group");
    abort();
}

/* Phase 2: WiFi Initialization */
// ... continues as normal ...
```

### Why This Location?

1. NVS is initialized (required for device persistence load)
2. LittleFS is mounted (required for device persistence)
3. Event loop is created (required for event bus)
4. Config manager is ready (adapters may need config)
5. Before WiFi/MQTT/Zigbee (adapters will connect to these later)

---

## 2. Where to Call `foundation_start_adapters()`

**Location:** After Zigbee network is formed AND MQTT is connected.

The best place is in the main loop, after `mqtt_bridge_start()` succeeds.

### Recommended Placement

```c
// main/main.c - Inside main loop (around line 988)

if (bits & MQTT_BRIDGE_START_BIT) {
    ESP_LOGI(TAG_MQTT, "Starting MQTT bridge from main loop...");
    ret = mqtt_bridge_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_MQTT, "Failed to start MQTT bridge: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG_MQTT, "MQTT bridge started successfully");

        /* =========================================================
         * NEW: Start Foundation Adapters
         * Now that MQTT is connected and bridge is running,
         * start the NG adapters to begin event-driven processing.
         * ========================================================= */
        if (foundation_is_initialized() && !foundation_is_running()) {
            ret = foundation_start_adapters();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG_MAIN, "Foundation adapters start failed: %s",
                         esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG_MAIN, "Foundation adapters started - NG system active");
            }
        }
        /* ========================================================= */

        /* Enable system monitor MQTT publishing */
        system_monitor_set_mqtt_publishing(true);
        // ... rest of existing code ...
    }
}
```

### Alternative: Call in `mqtt_connection_callback()` via Event Bit

If you want more explicit control, add a new event bit:

```c
#define FOUNDATION_START_BIT    BIT4

// In mqtt_connection_callback():
if (connected && foundation_is_initialized() && !foundation_is_running()) {
    xEventGroupSetBits(s_connection_event_group, FOUNDATION_START_BIT);
}

// In main loop:
if (bits & FOUNDATION_START_BIT) {
    foundation_start_adapters();
}
```

---

## 3. Event Flow After Integration

Once integrated, the event-driven flow works as follows:

### Device Join Flow

```
[Zigbee Stack]
    │
    ▼ Device joins network
[zb_coordinator.c] ─── existing callback ───▶ [mqtt_bridge] (legacy path)
    │
    ▼ zigbee_adapter intercepts
[zigbee_adapter]
    │
    ▼ Publishes event
[event_bus] ──── EVT_DEVICE_JOINED ────┬──▶ [ha_discovery_ng]
                                       │       │
                                       │       ▼ Creates discovery configs
                                       │    [mqtt_adapter] ─▶ MQTT publish
                                       │
                                       └──▶ [device_registry]
                                               │
                                               ▼ Stores device
                                            [device_persistence]
```

### State Change Flow

```
[Zigbee ZCL Report]
    │
    ▼
[zigbee_adapter] ─── converts to unified format ───▶ [device_registry]
    │                                                     │
    ▼ Updates state                                       ▼ Stores state
[event_bus] ── EVT_DEVICE_STATE_CHANGED ──┬──▶ [mqtt_adapter]
                                          │       │
                                          │       ▼
                                          │    MQTT: zigbee2mqtt/<friendly_name>
                                          │
                                          └──▶ [ha_discovery_ng]
                                                  │
                                                  ▼ (if needed)
                                               Update availability
```

### Command Flow (MQTT to Zigbee)

```
MQTT: zigbee2mqtt/<friendly_name>/set
    │
    ▼
[mqtt_adapter] ─── subscribed ───▶ parse command
    │
    ▼ Publishes event
[event_bus] ── EVT_DEVICE_COMMAND ──▶ [zigbee_adapter]
    │
    ▼ Sends ZCL command
[Zigbee Stack]
```

---

## 4. Backward Compatibility Notes

The foundation system is **additive** and does not replace existing functionality:

| Existing Code | Status | Notes |
|---------------|--------|-------|
| `mqtt_bridge.c` | Works as-is | Legacy path still functional |
| `zb_coordinator.c` | Works as-is | `zigbee_adapter` hooks in parallel |
| `ha_bridge_discovery.c` | Works as-is | `ha_discovery_ng` is separate |
| Device callbacks | Works as-is | Both old and new handlers can coexist |

### Migration Strategy

1. **Phase A (Current):** Foundation runs alongside existing code
   - Both `mqtt_bridge` and `mqtt_adapter` publish
   - Temporary duplication is OK for testing

2. **Phase B:** Validate new system works correctly
   - Compare MQTT messages from both paths
   - Verify HA discovers entities correctly

3. **Phase C:** Gradually disable legacy paths
   - Add config flags to disable old handlers
   - Route all traffic through foundation system

4. **Phase D:** Remove legacy code
   - Delete `ha_bridge_discovery.c` (replaced by `ha_discovery_ng`)
   - Simplify `mqtt_bridge.c` to only handle bridge/* topics

### Coexistence Configuration

```c
// Optional: Disable NG system via Kconfig
#if CONFIG_FOUNDATION_NG_ENABLED
    ret = foundation_init();
    // ...
#endif
```

---

## 5. Testing the Integration

### Verify Initialization

After `foundation_init()`, call status check:

```c
foundation_print_status();
```

Expected log output:

```
I (1234) FOUNDATION: === Foundation Status ===
I (1234) FOUNDATION:   lifecycle:        INITIALIZED
I (1234) FOUNDATION:   memory_pressure:  INITIALIZED
I (1234) FOUNDATION:   event_bus:        INITIALIZED
I (1234) FOUNDATION:   device_registry:  INITIALIZED (0 devices)
I (1234) FOUNDATION:   ha_discovery_ng:  INITIALIZED
I (1234) FOUNDATION:   zigbee_adapter:   INITIALIZED
I (1234) FOUNDATION:   mqtt_adapter:     INITIALIZED
I (1234) FOUNDATION:   ble_adapter:      INITIALIZED
I (1234) FOUNDATION: Core components: OK
I (1234) FOUNDATION: =========================
```

### Monitor Adapter Logs

Subscribe to the logging topic:

```bash
mosquitto_sub -h <broker> -t "zigbee2mqtt/bridge/logging"
```

You should see adapter activity:

```json
{"level":"info","message":"zigbee_adapter: device 0x1234 joined"}
{"level":"info","message":"ha_discovery_ng: publishing config for 0x1234"}
{"level":"debug","message":"mqtt_adapter: state update for 0x1234"}
```

### Test Device Join

1. Put a Zigbee device in pairing mode
2. Enable permit join: `mosquitto_pub -t zigbee2mqtt/bridge/request/permit_join -m '{"value":true}'`
3. Watch for:
   - `EVT_DEVICE_JOINED` in logs
   - HA discovery config on `homeassistant/*/0x<ieee>/config`
   - Device state on `zigbee2mqtt/<friendly_name>`

### Test State Changes

1. Trigger a device state change (e.g., turn on a light)
2. Verify both paths publish (during coexistence phase):
   - Legacy: `mqtt_bridge` publishes to `zigbee2mqtt/<name>`
   - NG: `mqtt_adapter` publishes via event bus

### Memory Verification

Check heap usage before and after:

```c
ESP_LOGI(TAG_MAIN, "Heap before foundation: %lu", esp_get_free_heap_size());
foundation_init();
ESP_LOGI(TAG_MAIN, "Heap after foundation: %lu", esp_get_free_heap_size());
```

Expected: Foundation system adds ~5-10KB to heap usage (mostly event bus queues).

---

## Quick Reference: Required Changes to main.c

1. **Add include** (top of file):
   ```c
   #include "core/foundation_init.h"
   ```

2. **Add init call** (end of Phase 1, ~line 460):
   ```c
   ret = foundation_init();
   if (ret != ESP_OK) {
       ESP_LOGW(TAG_MAIN, "Foundation init failed: %s", esp_err_to_name(ret));
   }
   foundation_print_status();
   ```

3. **Add start call** (in main loop after bridge start, ~line 990):
   ```c
   if (foundation_is_initialized() && !foundation_is_running()) {
       foundation_start_adapters();
   }
   ```

---

## Troubleshooting

| Issue | Cause | Solution |
|-------|-------|----------|
| `ESP_ERR_NO_MEM` on init | Not enough heap | Increase PSRAM usage or reduce buffer sizes |
| Adapters not starting | Called before MQTT ready | Ensure `foundation_start_adapters()` is after MQTT connect |
| Duplicate MQTT messages | Both legacy and NG active | Expected during migration; disable legacy later |
| Events not firing | Event bus not initialized | Check `foundation_print_status()` for event_bus state |
| Device persistence fails | LittleFS not mounted | Ensure `littlefs_mount()` succeeds before `foundation_init()` |
