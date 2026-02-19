# ESP32-C5 Zigbee2MQTT NG - Roadmap

## Agent-Based Development Model

**Claude als Koordinator** orchestriert spezialisierte Agents für parallele Entwicklung:

```
                    ┌─────────────────────┐
                    │   Claude (Head)     │
                    │   - Architektur     │
                    │   - Code Review     │
                    │   - Integration     │
                    └──────────┬──────────┘
           ┌───────────┬───────┴───────┬───────────┐
           ▼           ▼               ▼           ▼
    ┌──────────┐ ┌──────────┐  ┌──────────┐ ┌──────────┐
    │ Agent A  │ │ Agent B  │  │ Agent C  │ │ Agent D  │
    │ Memory   │ │ Events   │  │ Device   │ │ Protocol │
    └──────────┘ └──────────┘  └──────────┘ └──────────┘
```

### Workflow
1. **Plan** - Claude erstellt detaillierte Specs
2. **Delegate** - Agents arbeiten parallel an isolierten Komponenten
3. **Review** - Claude prüft Code, Integration, Konsistenz
4. **Integrate** - Claude verknüpft Komponenten, löst Konflikte
5. **Validate** - Build, Tests, Memory-Check

### Qualitätskontrolle
- Jede Agent-Arbeit wird verifiziert bevor Integration
- Cross-Component Dependencies werden von Claude geprüft
- Continuous Integration der Projektziele

---

## Priorisierung nach Impact

**Impact-Kriterien:**
- 🔴 **Kritisch** - Blockiert andere Features, fundamentale Architektur
- 🟠 **Hoch** - Signifikante Memory/Performance-Verbesserung, Multi-Feature-Enabler
- 🟡 **Mittel** - Einzelnes Feature, Code-Qualität
- 🟢 **Nice-to-have** - Polish, Developer Experience

---

## Phase 1: Foundation (Woche 1-2)
*Basis für alles Weitere - muss ZUERST gemacht werden*

### 1.1 🔴 Memory Infrastructure
**Impact:** Ermöglicht ALLE Features gleichzeitig
**Agent Assignment:**
- **Agent A**: `memory_manager_ng.c` - Core alloc/free, PSRAM, Buffer Pools
- **Agent B**: `module_manager.c` - Dynamic loading/unloading
- **Agent C**: `lifecycle.c` - State machine, transitions
- **Agent D**: `memory_pressure.c` - Monitoring, callbacks, auto-response

#### 1.1.1 Core Memory Management
```c
// memory_manager_ng.h
typedef struct {
    size_t total_internal;
    size_t free_internal;
    size_t total_psram;
    size_t free_psram;
    size_t largest_block;
} memory_stats_t;

// Zentrale Allocation mit automatischer PSRAM-Nutzung
void *mem_alloc(size_t size, mem_caps_t caps);
void mem_free(void *ptr);

// Buffer Pools
buffer_pool_t *pool_create(size_t buffer_size, size_t count, mem_caps_t caps);
void *pool_acquire(buffer_pool_t *pool);
void pool_release(buffer_pool_t *pool, void *buffer);
```

#### 1.1.2 Dynamic Module Loading/Unloading
```c
// module_manager.h
typedef enum {
    MODULE_UNLOADED,
    MODULE_LOADING,
    MODULE_LOADED,
    MODULE_UNLOADING,
    MODULE_ERROR,
} module_state_t;

typedef struct {
    const char *name;
    size_t heap_required;       // Min heap needed to load
    size_t heap_usage;          // Actual usage when loaded
    uint8_t priority;           // Unload priority (lower = unload first)
    bool essential;             // Never unload (Zigbee, WiFi)
    module_state_t state;

    esp_err_t (*load)(void);
    esp_err_t (*unload)(void);
    bool (*can_unload)(void);   // Check if safe to unload
} module_def_t;

// Module Registry
esp_err_t module_register(const module_def_t *def);
esp_err_t module_load(const char *name);
esp_err_t module_unload(const char *name);
esp_err_t module_ensure_loaded(const char *name);  // Load if not loaded
module_state_t module_get_state(const char *name);

// Auto-Management
esp_err_t module_manager_init(void);
void module_manager_set_memory_threshold(size_t min_free_heap);
```

**Beispiel Module:**
| Module | Essential | Heap | Priority | Kann entladen werden wenn... |
|--------|-----------|------|----------|------------------------------|
| zigbee | ✅ | 80KB | - | Nie |
| wifi | ✅ | 30KB | - | Nie |
| mqtt | ✅ | 15KB | - | Nie |
| ble_scanner | ❌ | 40KB | 3 | Keine BLE Devices konfiguriert |
| ble_gatt | ❌ | 20KB | 2 | Keine aktive GATT Connection |
| esphome | ❌ | 25KB | 4 | Kein ESPHome Client connected |
| ble_proxy | ❌ | 15KB | 1 | Keine Proxy Session aktiv |
| captive_portal | ❌ | 10KB | 0 | WiFi connected |
| ota | ❌ | 5KB | 5 | Kein OTA aktiv |

#### 1.1.3 Lifecycle States
```c
// lifecycle.h
typedef enum {
    LIFECYCLE_BOOT,         // Minimal: nur WiFi/NVS
    LIFECYCLE_PROVISIONING, // Captive Portal aktiv, Rest deaktiviert
    LIFECYCLE_PAIRING,      // Zigbee Pairing, BLE/ESPHome pausiert
    LIFECYCLE_RUNNING,      // Normal Operation, alle Features
    LIFECYCLE_LOW_MEMORY,   // Graceful Degradation, non-essential unloaded
    LIFECYCLE_OTA,          // OTA Update, nur essentials
    LIFECYCLE_SHUTDOWN,     // Cleanup vor Restart
} lifecycle_state_t;

typedef struct {
    lifecycle_state_t state;
    uint32_t state_entered_time;
    size_t heap_at_entry;
} lifecycle_info_t;

esp_err_t lifecycle_init(void);
esp_err_t lifecycle_transition(lifecycle_state_t new_state);
lifecycle_state_t lifecycle_get_state(void);

// State-spezifische Module-Konfiguration
// BOOT:         [wifi]
// PROVISIONING: [wifi, captive_portal]
// PAIRING:      [wifi, mqtt, zigbee] - BLE pausiert für mehr Heap
// RUNNING:      [wifi, mqtt, zigbee, ble_*, esphome]
// LOW_MEMORY:   [wifi, mqtt, zigbee] - non-essential entladen
// OTA:          [wifi, ota] - alles andere pausiert
```

#### 1.1.4 Memory Pressure Handling
```c
// memory_pressure.h
typedef enum {
    MEM_PRESSURE_NONE,      // >60KB free - alles ok
    MEM_PRESSURE_LOW,       // 40-60KB free - warning
    MEM_PRESSURE_MEDIUM,    // 25-40KB free - unload non-essential
    MEM_PRESSURE_HIGH,      // 15-25KB free - aggressive unload
    MEM_PRESSURE_CRITICAL,  // <15KB free - emergency mode
} memory_pressure_t;

typedef void (*pressure_callback_t)(memory_pressure_t level, void *ctx);

esp_err_t memory_pressure_init(void);
esp_err_t memory_pressure_register_callback(pressure_callback_t cb, void *ctx);
memory_pressure_t memory_pressure_get_level(void);

// Automatische Reaktionen:
// LOW:      Log warning, GC caches
// MEDIUM:   Unload priority 0-2 modules
// HIGH:     Unload priority 0-4 modules, reduce buffers
// CRITICAL: Emergency mode, nur essential modules
```

**Tasks:**
- [x] `mem_alloc()` mit auto-PSRAM für >1KB ✅
- [x] Buffer Pool Implementation (JSON: 4x2KB, MQTT: 8x512B) ✅
- [x] Memory Watermark Tracking ✅
- [x] Low-Memory Callback System ✅
- [x] Module Manager mit Load/Unload ✅
- [x] Lifecycle State Machine ✅
- [x] Memory Pressure Detection & Response ✅
- [x] Graceful Degradation Logic ✅ (`core/memory/graceful_degradation.c`)

### 1.2 🔴 Event System
**Impact:** Entkoppelt alle Module, ermöglicht saubere Architektur
**Agent Assignment:**
- **Agent A**: `event_bus.h` + `event_bus.c` - FreeRTOS Queue, Subscribe/Publish, ISR-safe
- **Agent B**: `event_data.h` - Payload structures for each event type

```c
// event_bus.h
typedef enum {
    EVT_DEVICE_JOINED,
    EVT_DEVICE_LEFT,
    EVT_DEVICE_STATE_CHANGED,
    EVT_DEVICE_INTERVIEWED,
    EVT_NETWORK_READY,
    EVT_MQTT_CONNECTED,
    EVT_MQTT_DISCONNECTED,
    EVT_BLE_DEVICE_FOUND,
    EVT_CONFIG_CHANGED,
} event_type_t;

typedef void (*event_handler_t)(event_type_t type, void *data, void *user_ctx);

esp_err_t event_bus_init(void);
esp_err_t event_subscribe(event_type_t type, event_handler_t handler, void *ctx);
esp_err_t event_unsubscribe(event_type_t type, event_handler_t handler);
esp_err_t event_publish(event_type_t type, void *data, size_t data_size);
esp_err_t event_publish_isr(event_type_t type, void *data, size_t data_size);
```

**Tasks:**
- [x] Event Bus mit FreeRTOS Queue ✅
- [x] ISR-safe Event Publishing ✅
- [x] Event Payload Structures (14 event types) ✅
- [x] Event Filtering (Handler können Events filtern) ✅
- [x] Event Priorities (Critical/High/Normal/Low) ✅
- [x] Command Events (EVT_COMMAND_RECEIVED, EVT_COMMAND_RESPONSE) ✅

### 1.3 🔴 Unified Device Model
**Impact:** Eliminiert Code-Duplikation, vereinheitlicht Device-Handling
**Agent Assignment:**
- **Agent A**: `unified_device.h` + `unified_device.c` - Types, capability helpers
- **Agent B**: `device_registry.h` + `device_registry.c` - PSRAM-backed registry, iterator

```c
// device_model.h
typedef uint64_t device_id_t;  // IEEE addr für Zigbee, MAC für BLE

typedef enum {
    DEV_PROTOCOL_ZIGBEE,
    DEV_PROTOCOL_BLE,
    DEV_PROTOCOL_VIRTUAL,  // Für berechnete Entities
} device_protocol_t;

typedef enum {
    DEV_CAP_ON_OFF      = (1 << 0),
    DEV_CAP_BRIGHTNESS  = (1 << 1),
    DEV_CAP_COLOR_TEMP  = (1 << 2),
    DEV_CAP_COLOR_XY    = (1 << 3),
    DEV_CAP_TEMPERATURE = (1 << 4),
    DEV_CAP_HUMIDITY    = (1 << 5),
    DEV_CAP_BATTERY     = (1 << 6),
    DEV_CAP_MOTION      = (1 << 7),
    DEV_CAP_CONTACT     = (1 << 8),
    DEV_CAP_VIBRATION   = (1 << 9),
    // ... weitere
} device_capability_t;

typedef struct {
    device_id_t id;
    char friendly_name[32];
    char model[32];
    char manufacturer[32];
    device_protocol_t protocol;
    uint32_t capabilities;
    uint32_t last_seen;
    bool online;

    // Protocol-specific data (in PSRAM)
    union {
        struct { uint16_t short_addr; uint8_t endpoint; } zigbee;
        struct { uint8_t addr_type; int8_t rssi; } ble;
    } proto;

    // Current state (generic key-value)
    cJSON *state;  // In PSRAM
} device_t;

// Device Registry API
esp_err_t device_registry_init(size_t max_devices);
device_t *device_add(device_id_t id, device_protocol_t proto);
device_t *device_get(device_id_t id);
device_t *device_get_by_name(const char *name);
esp_err_t device_remove(device_id_t id);
esp_err_t device_update_state(device_id_t id, const char *key, cJSON *value);
esp_err_t device_iterate(device_iterator_fn fn, void *ctx);
size_t device_count(void);
```

**Tasks:**
- [x] Unified Device Model (device_t, capabilities, protocols) ✅
- [x] Device Registry in PSRAM (64 devices, O(1) lookup) ✅
- [x] Generic State Storage (cJSON-basiert) ✅
- [x] Device Iterator für Batch-Operations ✅
- [x] Device Persistence (NVS) ✅
- [x] Foundation Init (wires all components) ✅

---

## Phase 2: Core Protocols (Woche 3-4)
*Protokoll-Adapter auf neuer Architektur*

### 2.1 🔴 Zigbee Adapter
**Impact:** Kern-Funktionalität
**Agent Assignment:**
- **Agent A**: `zigbee_adapter.h` + `zigbee_adapter.c` - Bridge callbacks to events + device registry

```c
// zigbee_adapter.h
// Brücke zwischen ESP-Zigbee-SDK und Unified Device Model

esp_err_t zigbee_adapter_init(void);
esp_err_t zigbee_adapter_start(void);

// Callbacks → Events
// zb_callback_device_join → EVT_DEVICE_JOINED
// zb_callback_report_attr → EVT_DEVICE_STATE_CHANGED
```

**Tasks:**
- [x] Zigbee Callbacks → Event Bus ✅
- [x] Device Join → device_add() ✅
- [x] Attribute Reports → device_update_state() ✅
- [x] Cluster to Capability Mapping ✅
- [ ] Converter System Integration (Phase 4)

### 2.2 🟠 MQTT Adapter
**Impact:** Home Assistant Integration
**Agent Assignment:**
- **Agent B**: `mqtt_adapter.h` + `mqtt_adapter.c` - Subscribe events, publish states

```c
// mqtt_adapter.h
esp_err_t mqtt_adapter_init(void);

// Event Subscriptions:
// EVT_DEVICE_STATE_CHANGED → publish state
// EVT_DEVICE_JOINED → publish discovery (optional)
// EVT_MQTT_CONNECTED → republish all states
```

**Tasks:**
- [x] State Publishing via Events ✅
- [x] Zigbee2MQTT Topic Compatibility ✅
- [x] QoS und Retain Handling ✅
- [x] Command Handling → Events ✅ (EVT_COMMAND_RECEIVED/RESPONSE)

### 2.3 🟠 BLE Adapter
**Impact:** BLE Device Support
**Agent Assignment:**
- **Agent C**: `ble_adapter.h` + `ble_adapter.c` - Bridge BLE scanner to events + device registry

```c
// ble_adapter.h
esp_err_t ble_adapter_init(void);
esp_err_t ble_adapter_start_scan(void);
esp_err_t ble_adapter_stop_scan(void);

// Scanner Results → EVT_BLE_DEVICE_FOUND
// Parsed Data → device_update_state()
```

**Tasks:**
- [x] BLE Scanner → Event Bus ✅
- [x] BLE Device → Unified Device Model ✅
- [x] RSSI/Battery Tracking ✅
- [x] Stale device detection (5min timeout) ✅
- [ ] Device Parsers Integration (existing parsers call ble_adapter)

---

## Phase 3: HA Integration (Woche 5-6)
*Home Assistant Discovery & Control*

### 3.1 🟠 Generic HA Discovery
**Impact:** Eliminiert Discovery Code-Duplikation

```c
// ha_discovery_ng.h
typedef struct {
    const char *component;      // "light", "sensor", "binary_sensor"
    const char *device_class;   // "temperature", "motion", etc.
    const char *unit;
    const char *icon;
    const char *value_template;
    uint32_t required_caps;     // DEV_CAP_* mask
} ha_entity_template_t;

// Templates statt Code
static const ha_entity_template_t s_templates[] = {
    { "sensor", "temperature", "°C", NULL, "{{ value_json.temperature }}", DEV_CAP_TEMPERATURE },
    { "sensor", "humidity", "%", NULL, "{{ value_json.humidity }}", DEV_CAP_HUMIDITY },
    { "binary_sensor", "motion", NULL, "mdi:motion-sensor", "{{ value_json.occupancy }}", DEV_CAP_MOTION },
    { "light", NULL, NULL, NULL, NULL, DEV_CAP_ON_OFF | DEV_CAP_BRIGHTNESS },
    // ...
};

// Auto-Discovery basierend auf Capabilities
esp_err_t ha_discovery_publish_device(const device_t *device);
```

**Tasks:**
- [x] Template-basierte Discovery (22 templates) ✅
- [x] Auto-Discovery aus Capabilities ✅
- [x] Batch Discovery (alle Devices auf einmal) ✅
- [x] Event-driven (EVT_DEVICE_JOINED, EVT_DEVICE_INTERVIEWED) ✅
- [x] Discovery Caching (FNV-1a hash, verhindert redundante Publishes) ✅

### 3.2 🟡 ESPHome API Adapter
**Impact:** Native HA Integration
**Agent Assignment:**
- **Agent D**: `esphome_adapter.h` + `esphome_adapter.c` - Bridge to event bus

```c
// esphome_adapter.h
esp_err_t esphome_adapter_init(void);

// EVT_DEVICE_STATE_CHANGED → ESPHome State Update
// ESPHome Command → EVT_COMMAND_RECEIVED
```

**Tasks:**
- [x] Entity Sync mit Device Registry ✅
- [x] State Updates via Events ✅
- [x] Capability → Entity Type Mapping ✅
- [ ] BLE Proxy Protocol (separate module)

---

## Phase 4: Advanced Features (Woche 7-8)
*Erweiterte Funktionalität*

### 4.1 🟡 Converter System 2.0
**Impact:** Einfachere Device-Unterstützung

```c
// converter_ng.h
typedef struct {
    const char *manufacturer;
    const char *model;
    uint32_t capabilities;      // Auto-set capabilities

    // From-Zigbee: Cluster/Attr → State Key
    const fz_entry_t *from_zigbee;
    size_t fz_count;

    // To-Zigbee: State Key → Cluster/Attr
    const tz_entry_t *to_zigbee;
    size_t tz_count;

    // Optional init/deinit
    esp_err_t (*init)(device_t *dev);
} converter_def_t;

// Converter auto-registriert Capabilities
```

**Tasks:**
- [x] Converter → Capabilities Mapping ✅ (`zb_converter_get_capabilities()` auto-maps exposes to DEV_CAP_*)
- [ ] Converter Hot-Reload (ohne Restart)
- [ ] Converter Validation

### 4.2 🟡 OTA Updates
**Impact:** Remote Updates

**Tasks:**
- [x] HTTP OTA ✅ (`ota/http_ota.c` - HTTPS download, lifecycle integration, progress callbacks)
- [x] MQTT OTA Trigger ✅ (`ota/mqtt_ota.c` - Zigbee2MQTT compatible topics)
- [x] Rollback Support ✅ (ESP-IDF app_update integration)
- [x] Version Reporting ✅ (Current/new version in status JSON)

### 4.3 🟡 Captive Portal 2.0
**Impact:** Einfache Einrichtung

**Tasks:**
- [ ] WiFi Provisioning
- [ ] MQTT Config
- [ ] Zigbee Channel Selection
- [ ] Device List View

---

## Phase 5: Optimization (Woche 9-10)
*Performance & Memory*

### 5.1 🟠 Memory Optimization
**Impact:** Mehr Devices, stabilerer Betrieb

**Tasks:**
- [ ] Picolibc Evaluation (v6.0)
- [ ] FreeRTOS-in-Flash
- [x] String Interning für häufige Strings ✅ (`core/memory/string_intern.c`)
- [x] Memory Pool für fixed-size blocks ✅ (`core/memory_pool.c`)
- [x] Adaptive Memory Response ✅ (`core/monitoring/adaptive_memory.c`)
- [ ] State Compression (optional)

### 5.2 🟡 Performance Optimization
**Impact:** Schnellere Response

**Tasks:**
- [ ] Zero-Copy wo möglich
- [x] Batch MQTT Publishing ✅ (`mqtt/batch_publisher.c`)
- [ ] Lazy JSON Serialization
- [ ] Connection Pooling

### 5.3 🟢 Coexistence Tuning
**Impact:** Stabilere Radio-Nutzung

**Tasks:**
- [x] WiFi/Zigbee/BLE Coex Manager ✅ (`core/coex_manager.c`)
- [x] Priority-based Radio Access ✅
- [ ] Adaptive Scan Intervals (runtime tuning)

---

## Phase 6: Quality & Polish (Woche 11-12)
*Stabilität & Developer Experience*

### 6.1 🟡 Testing Framework
**Tasks:**
- [ ] Unit Tests für Device Model
- [ ] Integration Tests
- [ ] Memory Leak Detection
- [ ] Stress Tests

### 6.2 🟢 Monitoring & Debugging
**Tasks:**
- [x] Memory Dashboard (MQTT) ✅
- [x] Event Tracing ✅
- [x] Performance Metrics ✅ (`core/monitoring/perf_metrics.c` - Event Bus, MQTT, Zigbee, System metrics)
- [x] Crash Reporting (already exists) ✅

### 6.3 🟢 Documentation
**Tasks:**
- [ ] API Documentation
- [ ] Architecture Guide
- [ ] Converter Development Guide
- [ ] Troubleshooting Guide

---

## Feature Matrix

| Feature | Phase | Impact | Abhängigkeit |
|---------|-------|--------|--------------|
| Memory Infrastructure | 1.1 | 🔴 | - |
| Event System | 1.2 | 🔴 | - |
| Unified Device Model | 1.3 | 🔴 | 1.1 |
| Zigbee Adapter | 2.1 | 🔴 | 1.2, 1.3 |
| MQTT Adapter | 2.2 | 🟠 | 1.2, 1.3 |
| BLE Adapter | 2.3 | 🟠 | 1.2, 1.3 |
| HA Discovery NG | 3.1 | 🟠 | 1.3, 2.2 |
| ESPHome Adapter | 3.2 | 🟡 | 1.2, 1.3 |
| Converter 2.0 | 4.1 | 🟡 | 2.1 |
| OTA | 4.2 | 🟡 | 2.2 |
| Captive Portal | 4.3 | 🟡 | - |
| Memory Optimization | 5.1 | 🟠 | 1.1 |
| Performance Opt | 5.2 | 🟡 | 1.1, 1.2 |
| Coexistence | 5.3 | 🟢 | - |
| Testing | 6.1 | 🟡 | All |
| Monitoring | 6.2 | 🟢 | 1.1, 1.2 |
| Documentation | 6.3 | 🟢 | All |

---

## Milestone Ziele

### M1: Foundation Complete (Ende Woche 2)
- [x] Memory System funktioniert ✅
- [x] Event Bus funktioniert ✅
- [x] Device Registry funktioniert ✅
- [ ] Alle Tests grün (manuell später)

### M2: Zigbee Working (Ende Woche 4)
- [ ] Zigbee Devices joinen
- [ ] States werden gepublisht
- [ ] Commands funktionieren
- [ ] HA sieht Devices

### M3: Multi-Protocol (Ende Woche 6)
- [ ] BLE Devices werden erkannt
- [ ] ESPHome API funktioniert
- [ ] Alle Protocols parallel aktiv
- [ ] <60KB Heap Usage

### M4: Feature Complete (Ende Woche 8)
- [ ] Alle Features implementiert
- [ ] OTA funktioniert
- [ ] Captive Portal funktioniert
- [ ] Stable für Daily Use

### M5: Optimized (Ende Woche 10)
- [ ] <50KB Heap Usage
- [ ] 100+ Devices supportet
- [ ] Schnelle Response Times
- [ ] Keine Memory Leaks

### M6: Production Ready (Ende Woche 12)
- [ ] Vollständig getestet
- [ ] Dokumentiert
- [ ] Community Feedback eingearbeitet
- [ ] v1.0 Release

---

## Nächste Schritte

1. ~~**DONE:** Phase 1.1 Memory Infrastructure~~ ✅
2. ~~**DONE:** Phase 1.2 Event System~~ ✅
3. ~~**DONE:** Phase 1.3 Unified Device Model~~ ✅
4. ~~**DONE:** Phase 2.1 Zigbee Adapter~~ ✅
5. ~~**DONE:** Phase 2.2 MQTT Adapter~~ ✅
6. ~~**DONE:** Phase 2.3 BLE Adapter~~ ✅
7. ~~**DONE:** Phase 3.1 HA Discovery NG~~ ✅
8. ~~**DONE:** Phase 3.2 ESPHome Adapter~~ ✅
9. ~~**DONE:** Device Persistence + Foundation Init~~ ✅
10. ~~**DONE:** Memory Dashboard + Event Trace~~ ✅
11. ~~**DONE:** Integration Guide (docs/INTEGRATION.md)~~ ✅
12. ~~**DONE:** Phase 5.1 String Interning~~ ✅
13. ~~**DONE:** Phase 5.2 Batch Publisher~~ ✅
14. ~~**DONE:** Phase 5.3 Coex Manager~~ ✅
15. ~~**DONE:** Code Quality Audit (CMakeLists, orphan files, build verification)~~ ✅
16. **JETZT:** Verbleibende Phase 5 + Phase 6 Testing

### Code Quality Status (2026-02-05)
- **Build:** ✅ Erfolgreich (2.58MB, 39% Partition frei, 92KB SRAM frei)
- **CMakeLists.txt:** ✅ Alle Source-Files korrekt gelistet
- **Orphan Files:** ✅ Behoben (memory_pool.c, adaptive_memory.c, graceful_degradation.c, perf_metrics.c)
- **Type Conflicts:** ✅ Behoben (trace_device_id_t, ieee_to_u64, mem_ng_free/calloc)
- **Compiler Warnings:** ✅ Keine Fehler
- **Architecture Integration:** ✅ **VOLLSTÄNDIG** - main.c ruft foundation_init() und alle Module auf

### NG-Architecture Integration Status
| Component | In main.c | Initialized | Running |
|-----------|-----------|-------------|---------|
| foundation_init() | ✅ | ✅ | ✅ |
| foundation_start_adapters() | ✅ | ✅ | ✅ |
| module_manager_init() | ✅ | ✅ | ✅ |
| graceful_degradation_init() | ✅ | ✅ | ✅ |
| perf_metrics_init() | ✅ | ✅ | ✅ |
| http_ota_init() | ✅ | ✅ | ✅ |
| mqtt_ota_init() | ✅ | ✅ | ✅ |

### Agent-Completed Features (Latest Session)
- ✅ Event Priorities (4 levels: Critical/High/Normal/Low)
- ✅ Event Filtering (per-handler filter callbacks)
- ✅ Graceful Degradation (memory_pressure → module_manager integration)
- ✅ HA Discovery Caching (FNV-1a hash prevents redundant publishes)
- ✅ Command Events (EVT_COMMAND_RECEIVED, EVT_COMMAND_RESPONSE)
- ✅ Converter → Capabilities Mapping (auto-maps exposes to device capabilities)
- ✅ Performance Metrics Module (Event Bus, MQTT, Zigbee, System metrics)
- ✅ HTTP OTA Module (HTTPS download, lifecycle integration, rollback support)
- ✅ MQTT OTA Trigger (Zigbee2MQTT compatible OTA commands)
- ✅ **main.c Integration** (foundation, module_manager, all new modules wired)
