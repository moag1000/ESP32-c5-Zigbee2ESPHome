# CLAUDE.md - ESP32-C5 Zigbee HA Native (ESPHome Primary)

## Vision
**Hybrid ESPHome Native API + MQTT Gateway** auf ESP32-C5 single-core RISC-V.
ESPHome Native API ist die PRIMARY Home Assistant Integration (Port 6053, Noise encryption).
MQTT ist sekundaer: Bridge-Management, Debug-Logs, Fallback.
Memory-optimiert, saubere Architektur, keine Code-Duplikation.

## Hybrid Architecture

### Integration Modes
| Mode | Config | HA Integration | MQTT |
|------|--------|----------------|------|
| **ESPHome Primary** (default) | `CONFIG_ESPHOME_PRIMARY_INTEGRATION=y` | ESPHome Native API | Bridge status + debug only |
| MQTT Primary (legacy) | `CONFIG_ESPHOME_PRIMARY_INTEGRATION=n` | MQTT Discovery | Full MQTT state/command |

When `CONFIG_ESPHOME_PRIMARY_INTEGRATION=y`:
- MQTT Discovery (`ha_discovery_ng.c`) is **disabled**
- MQTT state publishing (`mqtt_adapter.c`) is **disabled**
- All device entities are exposed via ESPHome Native API
- Zigbee devices appear as **sub-devices** under the gateway (ESPHome 2025.7.0+)

### Data Flow (ESPHome Primary)
```
Command Flow:
  HA --> ESPHome API (port 6053) --> esphome_adapter.c
    --> zb_converter_handle_command() --> ZCL command --> Zigbee device

State Flow:
  Zigbee Report --> zb_callbacks.c --> Event Bus (EVT_DEVICE_STATE_CHANGED)
    --> esphome_adapter.c --> ESPHome entity state update --> HA
```

### ESPHome Sub-Device Support
Each Zigbee device registers as a sub-device under the ESP32-C5 gateway.
The `device_id` field in entity protobuf messages links entities to their parent sub-device.
Sub-device info is provided in `DeviceInfoResponse` via `esphome_api_handlers.c`.

#### Protobuf device_id Field Numbers
| Entity Type | Field # | Entity Type | Field # |
|-------------|---------|-------------|---------|
| Sensor | 14 | Light | 16 |
| BinarySensor | 10 | Cover | 13 |
| Switch | 10 | Fan | 13 |
| TextSensor | 9 | Climate | 26 |
| Number | 14 | Lock | 12 |
| Button | 9 | MediaPlayer | 10 |
| Select | 9 | AlarmPanel | 11 |
| Text | 12 | | |

### ESPHome Entity Mapping (esphome_adapter.c)
| Device Capability | ESPHome Entity | Notes |
|-------------------|----------------|-------|
| `DEV_CAP_TEMPERATURE` | SENSOR | device_class: temperature |
| `DEV_CAP_HUMIDITY` | SENSOR | device_class: humidity |
| `DEV_CAP_PRESSURE` | SENSOR | device_class: pressure |
| `DEV_CAP_BATTERY` | SENSOR | device_class: battery |
| `DEV_CAP_POWER` | SENSOR | device_class: power |
| `DEV_CAP_ENERGY` | SENSOR | device_class: energy |
| `DEV_CAP_VOLTAGE` | SENSOR | device_class: voltage |
| `DEV_CAP_CURRENT` | SENSOR | device_class: current |
| `DEV_CAP_MOTION` | BINARY_SENSOR | device_class: motion |
| `DEV_CAP_CONTACT` | BINARY_SENSOR | device_class: door |
| `DEV_CAP_VIBRATION` | BINARY_SENSOR | device_class: vibration |
| `DEV_CAP_WATER_LEAK` | BINARY_SENSOR | device_class: moisture |
| `DEV_CAP_SMOKE` | BINARY_SENSOR | device_class: smoke |
| `DEV_CAP_BRIGHTNESS` | LIGHT | + color_temp, RGB support |
| `DEV_CAP_ON_OFF` (no brightness) | SWITCH | simple on/off |
| `DEV_CAP_COVER` | COVER | position, tilt |
| `DEV_CAP_FAN` | FAN | speed levels, oscillation |
| `DEV_CAP_CLIMATE` | CLIMATE | mode, target temp |
| `DEV_CAP_LOCK` | LOCK | lock/unlock/open |

## Features
| Feature | Heap | Status |
|---------|------|--------|
| Zigbee Coordinator | ~80KB | done |
| ESPHome Native API (Primary) | ~25KB | done |
| MQTT Bridge (Secondary) | ~15KB | done |
| Captive Portal | ~10KB | done |
| Event Bus | ~2KB | done |
| Device Registry | ~8KB | done |
| Memory Manager NG | ~2KB | done |
| Cluster State NG | ~2KB | done |
| Device Persistence | ~1KB | done |
| BLE Scanner | ~40KB | done |
| ESPHome BLE Proxy | ~5KB | done |
| OTA Updates (MQTT/HTTP/ESPHome/Zigbee) | ~5KB | done |
| Zigbee Groups | ~2KB | done |
| Zigbee Scenes | ~2KB | done |
| Zigbee Direct Binding | ~2KB | done |
| Zigbee Touchlink | ~2KB | done |
| Network Topology/Heal | ~3KB | done |
| Zigbee Availability Tracker | ~2KB | done |
| WiFi/Zigbee/BLE Coexistence | ~1KB | done |
| System Monitor + Crash Reporter | ~2KB | done |
| LED Status Manager | ~1KB | done |

**Hardware:** 384KB SRAM, 8MB PSRAM -> ~40KB internal free after full init (WiFi+Zigbee+ESPHome)

## ESP-IDF

| Version | Pfad | Beschreibung |
|---------|------|--------------|
| **v6.0** | `~/esp/esp-idf-v6` | Picolibc, MbedTLS v4.x |

### Setup
```bash
source ~/esp/esp-idf-v6/export.sh
```

### Merkmale
- **Picolibc** statt Newlib (Newlib-Kompatibilitaetsmodus aktiv)
- **C23 Standard** (gnu23) -- `bool`/`true`/`false` built-in, `static_assert` ohne `<assert.h>`, `nullptr` verfuegbar
- **MbedTLS v4.x** -- PSA Crypto selektiv (Noise, OTA, Install Codes), sonst Legacy mbedTLS
- Warnings: `-Wall` via ESP-IDF Default, **kein** `-Wextra`/`-Werror`
- Bootloader linker: `bootloader.ld` -> `bootloader.ld.in`

## Build
```bash
source ./scripts/setup_env.sh && ./scripts/build.sh && ./scripts/flash.sh
```

## Architecture

### Layer Abstraction
```
+-------------------------------------+
|         Application Layer           |  <- ESPHome Adapter, Commands
+-------------------------------------+
|         Integration Layer           |  <- ESPHome Native API (primary), MQTT (secondary)
+-------------------------------------+
|         Event Bus                   |  <- Event-Driven (State-Propagation, nicht Commands)
+-------------------------------------+
|         Device Abstraction          |  <- device_t, device_registry
+-------------------------------------+
|         Transport Layer             |  <- Zigbee, BLE, WiFi
+-------------------------------------+
```

### Device Model (NG Complete)

NG `device_t` ist Primary. Legacy `zb_device_handler.c` wurde vollstaendig eliminiert (~1.750 LOC geloescht).
Alle Cluster-Handler sind in eigene Dateien migriert:
- `zb_cluster_hvac.c` (Thermostat + Fan Control)
- `zb_cluster_measurement.c` (Illuminance + Pressure + PM2.5)
- `zb_cluster_electrical.c` (Electrical Measurement + Metering)
- `zb_cluster_security.c` (Door Lock + IAS Zone)
- `zb_cluster_closures.c` (Window Covering)
- `zb_cluster_multistate.c` (Multistate Input/Output/Value)

### Event Bus
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

### Unified Device Model
```c
#include "core/device/unified_device.h"
#include "core/device/device_registry.h"

// Device lookup
device_t *dev = device_registry_get(ieee_addr);
device_t *dev = device_registry_get_by_short_addr(0x1234);

// Device capabilities
if (dev->capabilities & DEV_CAP_TEMPERATURE) { ... }
if (dev->capabilities & DEV_CAP_ON_OFF) { ... }

// Converter access
const void *conv = device_registry_get_converter(dev->id);
```

### Cluster State NG
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

### Device Persistence
```c
#include "core/device/device_persistence.h"

// Laedt Geraete aus NVS, validiert Format
device_persistence_load_all();  // Auto-clear bei ungueltigen Records

// Speichert einzelnes Geraet
device_persistence_save(dev);

// Speichert alle Geraete
device_persistence_save_all();
```

### Memory Manager NG
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
| Zigbee Clusters | `zb_cluster_hvac.c`, `zb_cluster_electrical.c`, `zb_cluster_security.c`, `zb_cluster_closures.c`, `zb_cluster_measurement.c`, `zb_cluster_multistate.c` |
| Cluster State | `main/zigbee/cluster_state_ng.c` |
| **ESPHome API** | `main/esphome/esphome_api_server.c`, `esphome_api_handlers.c`, `esphome_api.c` |
| **ESPHome Entities** | `esphome_entity_sensors.c`, `esphome_entity_controls.c`, `esphome_entity_specialized.c`, `esphome_entities_types.h` |
| **ESPHome Adapter** | `main/core/adapters/esphome_adapter.c` -- Bridges event bus + device registry to ESPHome entities |
| ESPHome BLE | `main/esphome/esphome_ble_proxy.c` |
| ESPHome Crypto | `esphome_noise.c`, `esphome_crypto_constants.h` |
| MQTT (Secondary) | `main/mqtt/gateway_mqtt.c`, `main/core/bridge/mqtt_bridge.c` |
| Events | `main/core/events/event_bus.c` |
| Device (NG) | `main/core/device/device_registry.c`, `unified_device.c`, `device_persistence.c` |
| Command Handler | `main/zigbee/zb_command_handler.c` |
| Memory | `main/core/memory/memory_manager_ng.c`, `adaptive_memory.c` |
| Discovery | `main/core/discovery/ha_discovery_ng.c` (disabled when ESPHome primary) |
| Converter | `main/zigbee/converter/*.c` (27 definitions) |
| Adapters | `main/core/adapters/esphome_adapter.c`, `mqtt_adapter.c`, `zigbee_adapter.c`, `ble_adapter.c` |
| BLE | `main/bluetooth/ble_scanner.c`, `ble_manager.c` |
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

// ESPHome Entity Registration (esphome_adapter.c)
// Capabilities -> entity types, device_id links to sub-device
esphome_adapter_register_device(dev);  // auto-maps capabilities to entities
```

## NG Architecture Status

| Komponente | Aufrufe | Status |
|------------|---------|--------|
| Memory Manager NG | 315 | done, 100% |
| Event Bus | 209 (108 publish, 48 subscribe, 53 unsubscribe) | done |
| Device Registry NG | 342 | done, Primary |
| ESPHome Adapter | -- | done, Primary HA integration |
| ESPHome Sub-Devices | -- | done, device_id in all 15 entity types |
| Buffer Pools | 66 | done |
| Direct MQTT in zigbee/ | 0 publish | done, migrated |
| Legacy zb_device_handler.c | ELIMINIERT | done, ~1.750 LOC geloescht |
| Legacy zb_device_get() | 0 | done, vollstaendig migriert |
| Legacy NVS (zb_devices) | auto-erased on boot | done, NG `devices` namespace primary |
| C23 Standard | gnu23 | done, bool/true/false built-in |
| MQTT Discovery | conditional | done, disabled when CONFIG_ESPHOME_PRIMARY_INTEGRATION=y |

## Roadmap

### Core Architecture
- [x] Event Bus System
- [x] Unified Device Model (device_t)
- [x] Device Registry with PSRAM
- [x] Memory Manager NG + Adaptive Memory
- [x] HA Discovery NG (capability-based, conditional on MQTT primary)
- [x] Cluster State NG (cJSON-based)
- [x] Device Persistence NG (auto-clear invalid, power_info)
- [x] Buffer Pool Helpers (pool_json_print/free)
- [x] Zigbee->Event migration (0 direct MQTT in zigbee/)
- [x] MQTT LWT (offline state on unclean disconnect)
- [x] ESP-IDF v6.0 Platform (Picolibc im Compat-Modus, PSA selektiv)
- [x] C23 Standard (gnu23)
- [x] Legacy zb_device_handler.c eliminiert

### ESPHome Native API (Primary Integration)
- [x] ESPHome API Server (port 6053, Noise encryption)
- [x] ESPHome Adapter (event bus -> ESPHome entities)
- [x] Sub-device support (device_id in all 15 entity types)
- [x] Sensor entities (temperature, humidity, pressure, battery, power, energy, voltage, current)
- [x] Binary sensor entities (motion, contact, vibration, water_leak, smoke)
- [x] Light entities (brightness, color_temp, RGB)
- [x] Switch entities (on/off)
- [x] Cover entities (position, tilt)
- [x] Fan entities (speed, oscillation)
- [x] Climate entities (mode, target temp)
- [x] Lock entities (lock/unlock/open)
- [x] CONFIG_ESPHOME_PRIMARY_INTEGRATION Kconfig option
- [x] MQTT discovery/state disabled when ESPHome primary
- [x] ESPHome device info sync (manufacturer, model from interview)
- [ ] ESPHome OTA for Zigbee sub-devices

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
- [x] ESPHome BLE Proxy
- [x] OTA Updates (MQTT/HTTP/ESPHome/Zigbee)
- [x] WiFi Manager (5GHz auto, captive portal)
- [x] WiFi/Zigbee/BLE Coexistence

### Monitoring
- [x] System Monitor (heap, CPU, tasks)
- [x] Crash Reporter (boot reason, NVS persistence)
- [x] LED Status Manager (RGB patterns)
- [x] Performance Metrics + Latency Measurement

### Remaining
- [ ] CI/CD Pipeline (Build + Lint + Format)
- [ ] Web Dashboard (HTTP status page)
- [ ] Expand converter library
