# CLAUDE.md - ESP32-C5 Zigbee2MQTT NG

## Vision
**Unified Gateway** mit ALLEN Features auf ESP32-C5 single-core RISC-V.
Memory-optimiert, saubere Architektur, keine Code-Duplikation.

## Features
| Feature | Heap | Status |
|---------|------|--------|
| Zigbee Coordinator | ~80KB | ✅ |
| MQTT Bridge + LWT | ~15KB | ✅ |
| HA Discovery NG | ~5KB | ✅ |
| Captive Portal | ~10KB | ✅ |
| Event Bus | ~2KB | ✅ |
| Device Registry | ~8KB | ✅ |
| Memory Manager NG | ~2KB | ✅ |
| Device Sync Layer | — | ✅ ELIMINIERT (AP-7.3) |
| Cluster State NG | ~2KB | ✅ |
| Device Persistence | ~1KB | ✅ |
| BLE Scanner | ~40KB | ✅ |
| ESPHome API + BLE Proxy | ~25KB | ✅ |
| OTA Updates (MQTT/HTTP/ESPHome/Zigbee) | ~5KB | ✅ |
| Zigbee Groups | ~2KB | ✅ |
| Zigbee Scenes | ~2KB | ✅ |
| Zigbee Direct Binding | ~2KB | ✅ |
| Zigbee Touchlink | ~2KB | ✅ |
| Network Topology/Heal | ~3KB | ✅ |
| Zigbee Availability Tracker | ~2KB | ✅ |
| WiFi/Zigbee/BLE Coexistence | ~1KB | ✅ |
| System Monitor + Crash Reporter | ~2KB | ✅ |
| LED Status Manager | ~1KB | ✅ |

**Hardware:** 384KB SRAM, 8MB PSRAM → ~40KB internal free after full init (WiFi+Zigbee+MQTT)

## ESP-IDF

| Version | Pfad | Beschreibung |
|---------|------|--------------|
| **v6.0** | `~/esp/esp-idf-v6` | Picolibc, MbedTLS v4.x |

### Setup
```bash
source ~/esp/esp-idf-v6/export.sh
```

### Merkmale
- **Picolibc** statt Newlib (Newlib-Kompatibilitätsmodus aktiv)
- **C23 Standard** (gnu23) — `bool`/`true`/`false` built-in, `static_assert` ohne `<assert.h>`, `nullptr` verfügbar, `<stdbool.h>` entfernt (AP-7.5)
- **MbedTLS v4.x** — PSA Crypto selektiv (Noise, OTA, Install Codes), sonst Legacy mbedTLS
- Warnings: `-Wall` via ESP-IDF Default, **kein** `-Wextra`/`-Werror` (AP-5.4)
- Bootloader linker: `bootloader.ld` → `bootloader.ld.in`

## Build
```bash
source ./scripts/setup_env.sh && ./scripts/build.sh && ./scripts/flash.sh
```

## Architecture

### Layer Abstraction
```
┌─────────────────────────────────────┐
│         Application Layer           │  ← HA Discovery, Commands
├─────────────────────────────────────┤
│         Protocol Layer              │  ← MQTT, ESPHome API
├─────────────────────────────────────┤
│         Event Bus                   │  ← Event-Driven (State-Propagation, nicht Commands)
├─────────────────────────────────────┤
│         Device Abstraction          │  ← device_t, device_registry ✅
├─────────────────────────────────────┤
│         Transport Layer             │  ← Zigbee, BLE, WiFi
└─────────────────────────────────────┘
```

### Device Model Migration (✅ Complete)

NG `device_t` ist Primary. Legacy `zb_device_handler.c` wurde vollständig eliminiert (~1.750 LOC gelöscht).
Alle Cluster-Handler sind in eigene Dateien migriert:
- `zb_cluster_hvac.c` (Thermostat + Fan Control)
- `zb_cluster_measurement.c` (Illuminance + Pressure + PM2.5)
- `zb_cluster_electrical.c` (Electrical Measurement + Metering)
- `zb_cluster_security.c` (Door Lock + IAS Zone)
- `zb_cluster_closures.c` (Window Covering)
- `zb_cluster_multistate.c` (Multistate Input/Output/Value)

#### Migration Status

| Komponente | Legacy | NG | Status |
|------------|--------|-----|--------|
| Device Struct | `zb_device_t` | `device_t` | ✅ NG primary |
| Device Registry | ~~`zb_device_handler.c`~~ | `device_registry.c` | ✅ Legacy eliminiert |
| Type Determination | ~~`zb_device_determine_type()`~~ | `device_determine_zigbee_type()` | ✅ NG |
| Cluster State | Eigene `zb_cluster_*.c` Dateien | `cluster_state_ng.c` (cJSON) | ✅ Modularisiert |
| HA Discovery | `ha_discovery.c` | `ha_discovery_ng.c` | ✅ NG primary |
| State Publisher | `device_state_publisher.c` | `mqtt_adapter.c` + Events | ✅ Legacy-Helper entfernt |
| NVS Persistence | `zb_devices` (auto-erased) | `devices` namespace | ✅ NG primary |

### Event Bus (✅ Implemented)
```c
#include "core/events/event_bus.h"

// Events publizieren
evt_device_state_t evt = { .ieee_addr = addr, .json_state = json };
event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));

// Events abonnieren
event_subscribe(EVT_DEVICE_STATE_CHANGED, on_state_change, NULL);
```

**Event Types:** `EVT_DEVICE_JOINED`, `EVT_DEVICE_LEFT`, `EVT_DEVICE_STATE_CHANGED`,
`EVT_DEVICE_INTERVIEWED`, `EVT_MQTT_CONNECTED`, `EVT_MQTT_DISCONNECTED`

### Unified Device Model (✅ Implemented)
```c
#include "core/device/unified_device.h"
#include "core/device/device_registry.h"

// Device lookup (NG)
device_t *dev = device_registry_get(ieee_addr);
device_t *dev = device_registry_get_by_short_addr(0x1234);

// Device capabilities
if (dev->capabilities & DEV_CAP_TEMPERATURE) { ... }
if (dev->capabilities & DEV_CAP_ON_OFF) { ... }

// Converter access
const void *conv = device_registry_get_converter(dev->id);
```

### Device Access (Direct Registry)
```c
// Direct device lookup by short address (NG primary)
device_t *dev = device_registry_get_by_short_addr(short_addr);

// Direct device lookup by IEEE/device_id
device_t *dev = device_registry_get(ieee64);
```

### Cluster State NG (✅ Implemented)
```c
#include "zigbee/cluster_state_ng.h"

// State in device_t's cJSON speichern
cluster_state_set_thermostat(dev->id, &thermo_state);
cluster_state_set_electrical(dev->id, &elec_state);

// Generische Updates
cluster_state_update_number(dev->id, "temperature", 21.5);
cluster_state_update_bool(dev->id, "contact", true);

// Event wird automatisch publiziert: EVT_DEVICE_STATE_CHANGED
```

### Device Persistence (✅ Implemented)
```c
#include "core/device/device_persistence.h"

// Lädt Geräte aus NVS, validiert Format
device_persistence_load_all();  // Auto-clear bei ungültigen Records

// Speichert einzelnes Gerät
device_persistence_save(dev);

// Speichert alle Geräte
device_persistence_save_all();
```

### Memory Manager NG (✅ Implemented)
```c
#include "core/memory/memory_manager_ng.h"

// PSRAM-aware allocation
void *ptr = mem_alloc(size);           // Auto PSRAM for >1KB
void *ptr = mem_ng_calloc(n, size);    // Zero-initialized
mem_ng_free(ptr);

// Buffer pools (in foundation_init.c)
buffer_pool_t *pool = foundation_get_json_pool();  // 4x 2KB
buffer_pool_t *pool = foundation_get_mqtt_pool();  // 8x 512B
```

## Key Files
| Module | Files |
|--------|-------|
| Entry | `main/main.c` |
| Zigbee | `main/zigbee/zb_coordinator.c`, `zb_callbacks.c`, `zb_interview.c`, `zb_command_handler.c` |
| Zigbee Features | `zb_groups.c`, `zb_scenes.c`, `zb_binding.c`, `zb_touchlink.c`, `zb_topology.c` |
| Zigbee Network | `zb_network.c`, `zb_availability.c`, `zb_reporting.c`, `zb_backup.c` |
| Zigbee OTA | `main/zigbee/zb_ota.c` |
| Tuya | `main/zigbee/tuya/zb_tuya.c`, `tuya_fingerbot.c`, `tuya_driver_registry.c` |
| MQTT | `main/mqtt/gateway_mqtt.c`, `main/core/bridge/mqtt_bridge.c` |
| Events | `main/core/events/event_bus.c` |
| Device (NG) | `main/core/device/device_registry.c`, `unified_device.c`, `device_persistence.c` |
| Command Handler | `main/zigbee/zb_command_handler.c` (moved from core/ — AP-7.1) |
| Zigbee Clusters | `zb_cluster_hvac.c`, `zb_cluster_electrical.c`, `zb_cluster_security.c`, `zb_cluster_closures.c`, `zb_cluster_measurement.c`, `zb_cluster_multistate.c` |
| Cluster State | `main/zigbee/cluster_state_ng.c` |
| Memory | `main/core/memory/memory_manager_ng.c`, `adaptive_memory.c` |
| Discovery | `main/core/discovery/ha_discovery_ng.c`, `ha_bridge_discovery.c` |
| Converter | `main/zigbee/converter/*.c` (27 definitions) |
| Adapters | `main/core/adapters/mqtt_adapter.c`, `zigbee_adapter.c`, `ble_adapter.c` |
| BLE | `main/bluetooth/ble_scanner.c`, `ble_manager.c` |
| ESPHome | `main/esphome/esphome_api_server.c`, `esphome_ble_proxy.c` |
| OTA | `main/ota/ota_handler.c`, `mqtt_ota.c`, `http_ota.c` |
| WiFi | `main/wifi/wifi_manager.c`, `wifi_captive_portal.c` |
| Monitoring | `main/core/monitoring/system_monitor.c`, `crash_reporter.c`, `perf_metrics.c` |
| Lifecycle | `main/core/lifecycle_manager.c` |
| Module Mgmt | `main/core/memory/module_manager.c` |

## Coding Rules
| Element | Convention |
|---------|------------|
| Functions | `snake_case` |
| Constants | `UPPER_CASE` |
| Static vars | `s_` prefix |
| NG functions | `_ng` suffix |
| Errors | always check `esp_err_t` |

## Critical Patterns
```c
// Zigbee Thread-Safety
esp_zb_lock_acquire(portMAX_DELAY);
esp_err_t ret = esp_zb_zcl_*();
esp_zb_lock_release();

// Resource Cleanup
esp_err_t func(void) {
    resource_t *r1 = NULL, *r2 = NULL;
    r1 = alloc();
    if (!r1) goto cleanup;
    r2 = alloc();
    if (!r2) goto cleanup;
    // ... use resources ...
    ret = ESP_OK;
cleanup:
    free(r2);
    free(r1);
    return ret;
}

// Event Publishing
evt_device_state_t evt = { ... };
event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));

// Device Access (NG primary)
device_t *dev = device_registry_get_by_short_addr(short_addr);
device_t *dev = device_registry_get(ieee_addr);
```

## NG Architecture Status

| Komponente | Aufrufe | Status |
|------------|---------|--------|
| Memory Manager NG | 315 | ✅ 100% |
| Event Bus | 209 (108 publish, 48 subscribe, 53 unsubscribe) | ✅ |
| Device Registry NG | 342 | ✅ Primary |
| Device Sync Layer | **ELIMINIERT** | ✅ ~379 LOC gelöscht, alle Caller direkt auf device_registry |
| Buffer Pools | 66 | ✅ |
| Direct MQTT in zigbee/ | 0 publish, 2 verbleibend (command_handler, fingerbot) | ✅ 6/8 Topic-Includes entfernt (AP-7.2) |
| Legacy zb_device_handler.c | **ELIMINIERT** | ✅ ~1.750 LOC gelöscht, Multistate extrahiert |
| Legacy zb_device_get() | 0 | ✅ Vollständig migriert |
| Legacy NVS (zb_devices) | auto-erased on boot | ✅ NG `devices` namespace primary |
| C23 Standard | gnu23, `<stdbool.h>` aus 111 Dateien entfernt | ✅ bool/true/false built-in (AP-7.5) |
| Event Ownership | 51 Structs annotiert | ✅ BORROW/TRANSFER Doku |

## Roadmap

### Core Architecture
- [x] Event Bus System
- [x] Unified Device Model (device_t)
- [x] Device Registry with PSRAM
- [x] Memory Manager NG + Adaptive Memory
- [x] HA Discovery NG (capability-based)
- [x] Device Sync Layer (zb_device_t ↔ device_t)
- [x] Cluster State NG (cJSON-based)
- [x] Device Persistence NG (auto-clear invalid, power_info)
- [x] Buffer Pool Helpers (pool_json_print/free)
- [x] MQTT Event Handler (mqtt_event_handler.c)
- [x] Zigbee→Event migration (0 direct MQTT in zigbee/)
- [x] MQTT LWT (offline state on unclean disconnect)
- [x] Discovery cache invalidation on reconnect
- [x] ESP-IDF v6.0 Platform (Picolibc im Compat-Modus, PSA selektiv)
- [x] C23 Standard aktivieren (gnu23 C-only Generator Expression + static_assert)
- [x] Thread Safety: MQTT Latency Race, BLE GATT Race (23/23), Registry Iterator Deadlock (6/6)
- [x] Event Ownership Dokumentation (51 Structs BORROW/TRANSFER)
- [x] Dead Code Entfernung (~795 LOC aus zb_device_handler.c)
- [x] Legacy External Calls migriert (0 externe Aufrufe an zb_device_handler.c)
- [x] Layer-Violations: EVT_BRIDGE_PUBLISH, Adapter-Interface vtable + Registry
- [x] Legacy zb_device_handler.c vollständig eliminiert (~1.750 LOC gelöscht + 335 LOC Multistate extrahiert)
- [x] Legacy NVS Namespace konsolidiert (auto-erase on boot, NG primary)
- [x] Picolibc Newlib-Kompatibilitätsmodus: MUSS aktiv bleiben (Zigbee `__getreent`-Abhängigkeit)

### Zigbee Features
- [x] Coordinator with auto-network formation
- [x] Device Interview + Converter binding
- [x] Availability Tracker (power-aware timeouts)
- [x] Groups, Scenes, Direct Binding
- [x] Touchlink (Hue device takeover)
- [x] Network Topology + Heal
- [x] Zigbee OTA
- [x] Tuya driver framework + Fingerbot

### Connectivity
- [x] BLE Scanner (BLE 5.0 extended, Xiaomi, passive)
- [x] ESPHome API Server (Noise encryption, BLE Proxy)
- [x] OTA Updates (MQTT/HTTP/ESPHome/Zigbee)
- [x] WiFi Manager (5GHz auto, captive portal)
- [x] WiFi/Zigbee/BLE Coexistence

### Monitoring
- [x] System Monitor (heap, CPU, tasks)
- [x] Crash Reporter (boot reason, NVS persistence)
- [x] LED Status Manager (RGB patterns)
- [x] Performance Metrics + Latency Measurement

### Remaining (siehe docs/ACTION_PLAN.md AP-7)
- [x] Thread Safety: MQTT latency race ✅, BLE GATT race ✅, Registry iterator deadlock ✅
- [x] Layer-Violations eliminiert (EVT_BRIDGE_PUBLISH, Adapter-Interface)
- [x] Legacy: zb_device_handler.c eliminiert (~1.750 LOC gelöscht)
- [x] Legacy NVS Namespace konsolidiert (auto-erase, NG primary)
- [x] Picolibc: Compat-Modus permanent (Zigbee-Dep), dokumentiert
- [x] Code Safety: strncpy null-termination (7 Stellen), atoi→strtol (2 Stellen)
- [x] AP-7.3: device_sync.c eliminiert (~379 LOC, 48 Callers → device_registry direkt)
- [x] AP-7.4: Dead Code entfernt (~380 LOC aus zigbee_adapter, cluster_state_ng, zb_touchlink, zb_multi_pan, zb_device_handler_types.h)
- [x] AP-7.5: C23 ehrlich machen (`<stdbool.h>` aus 111 Dateien entfernt, 0 verbleibend)
- [x] AP-7.6: sdkconfig.defaults bereinigt (9 stale Comments, ESP32-only Config entfernt)
- [x] AP-7.8: Stale comments + TODO audit (13 Dateien, 20 TODOs als echt verifiziert)
- [x] AP-7.1: command_handler.c → zigbee/zb_command_handler.c (Layer-Violation behoben)
- [x] AP-7.2: MQTT-Topic-Wissen aus zigbee/ entfernt (6 Dateien, 2 neue Events: AVAILABILITY_CHANGED, TOPICS_CLEAR)
- [x] AP-7.9: Availability-Tracker parallel Device-Array eliminiert (~2KB static RAM frei, avail_meta in device_t)
- [x] AP-7.11: zb_device_handler_types.h gesplittet (1051→223 LOC, Cluster-Types in eigene Headers)
- [ ] CI/CD Pipeline (Build + Lint + Format)
- [ ] Web Dashboard (HTTP status page)
- [ ] Expand converter library
