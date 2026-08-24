# Architecture Documentation

<!-- staleness-banner -->

> ⚠️ **Partially outdated (last reviewed 2026-08-05).** This document still
> carries the fork state from 2026-02-19. Four structural changes are missing:
> 1. **Bluetooth is disabled** (`CONFIG_BT_ENABLED=n`). `BT_SRCS` in
>    `main/CMakeLists.txt` is only added when BT is enabled, so the entire
>    `main/bluetooth/` tree and `esphome_ble_proxy.c` are not compiled. Every
>    BLE task, queue and data path described below is inactive.
> 2. **Converters moved from compile time to runtime.** Device definitions live
>    in a JSON database in LittleFS (`/littlefs/converters`, `index.json` v2),
>    loaded by `zb_converter_loader.c`. Only three converters remain in C.
>    Note that `zb_converter_find()` therefore performs file I/O — it must not
>    be called while holding `s_mutex` in `zb_interview.c`.
>
> 3. **Zigbee no longer waits for the uplink.** `zigbee_stack_start()` in
>    `main.c` brings the coordinator up before the MQTT phase, and the captive
>    portal timeout no longer restarts the device. The boot order described
>    below (WiFi -> MQTT -> Zigbee) is the one that left the gateway dead with
>    no access point.
> 4. **Modules removed.** `cluster_state_ng.c`, `core/memory_pool.c` and
>    `core/coex_manager.c` were deleted on 2026-08-05 — each was superseded by
>    something already in use. The 4 references to `zb_device_handler` below
>    describe code eliminated during the NG migration. Scenes, Touchlink and
>    Zigbee OTA are now behind Kconfig flags, default off.
>
> Also newer than this document: `main/mmwave/` (S3KM1110 presence sensor),
> `esphome_services.c`, `esphome_ota.c`, `zb_quirk_engine.c` and
> `esphome_entity_mirror.c` (entity state no longer lives in the device
> registry).
>
> `CLAUDE.md` in the repository root is the current source of truth.

This document provides a comprehensive overview of the ESP32-C5 gateway
architecture, including system design, module structure, data flow, and key
design decisions.

## Table of Contents

- [System Overview](#system-overview)
- [High-Level Architecture](#high-level-architecture)
- [Module Dependencies](#module-dependencies)
- [Data Flow Architecture](#data-flow-architecture)
- [FreeRTOS Task Architecture](#freertos-task-architecture)
- [Memory Layout and Constraints](#memory-layout-and-constraints)
- [Key Synchronization Points](#key-synchronization-points)
- [Core Components](#core-components)
- [Protocol Stacks](#protocol-stacks)
- [FreeRTOS Task Structure](#freertos-task-structure)
- [Inter-Task Communication](#inter-task-communication)
- [Key Patterns](#key-patterns)
- [Memory Management](#memory-management)
- [Configuration System](#configuration-system)
- [Module Structure](#module-structure)
- [Data Flow](#data-flow)
- [Design Decisions](#design-decisions)
- [Performance Characteristics](#performance-characteristics)

## System Overview

The ESP32-C5 Unified Gateway is a production-ready firmware that bridges **Zigbee**, **Bluetooth LE**, and **WiFi** networks to MQTT-based and ESPHome-compatible home automation systems.

**Current Status**:
- Phase 1-9 Complete: Zigbee2MQTT Core (~12,000 LOC)
- Phase 10-14 Complete: Bluetooth Gateway + ESPHome API (~6,000 LOC)

**Hardware Target**: ESP32-C5 (single-core RISC-V @ 240MHz, WiFi 6, Zigbee, BLE 5.0)

> **Experimental Notice**: Espressif recommends dual-SoC solutions for production Zigbee gateways.

## Hardware Capabilities (ESP32-C5)

The ESP32-C5 is the industry's first RISC-V MCU with 2.4 and 5 GHz dual-band Wi-Fi 6 support.

### Processor
| Component | Specification |
|-----------|---------------|
| HP CPU | 32-bit RISC-V (RV32IMAC) @ 240 MHz |
| LP CPU | 32-bit RISC-V for ultra-low-power sleep modes |
| ROM | 320 KB |
| SRAM | 384 KB |
| PSRAM | Up to 8 MB external (optional) |
| Flash | Up to 16 MB external |

### WiFi 6 (802.11ax)
| Feature | Description |
|---------|-------------|
| Bands | Dual-band 2.4 GHz + 5 GHz |
| Standards | 802.11ax/ac/a/b/g/n backward compatible |
| TWT | Target Wake Time for extended device sleep |
| MU-MIMO | Multi-User MIMO (downlink) |
| OFDMA | Orthogonal Frequency-Division Multiple Access (UL/DL) |
| BSS Coloring | Stable connectivity in congested environments |
| AMPDU | Block Ack window up to 64 (vs 32 on non-HE) |

### Bluetooth & 802.15.4
| Feature | Specification |
|---------|---------------|
| Bluetooth | 5.0 LE with Long Range, Coded PHY |
| Throughput | 2 Mbps high throughput PHY |
| Mesh | BLE SIG Mesh, ESP-Mesh-Lite |
| 802.15.4 | Zigbee 3.0, Thread, Matter |
| Coexistence | Hardware arbitration for WiFi/BLE/Zigbee |

### Security
- Secure Boot with RSA-3072
- Flash encryption (AES-128/256-XTS)
- PSRAM encryption
- Hardware crypto accelerators (AES, SHA, RSA, ECC)
- Digital Signature Peripheral
- Key Manager with secure storage
- Trusted Execution Environment (TEE)
- Physical Memory Protection (PMP)

### GPIO & Peripherals
- 29 programmable GPIOs
- SPI, I2C, I2S, UART, SDIO
- USB Serial/JTAG
- ADC, Temperature sensor
- PWM, LED PWM Controller
- Package: QFN48 (6x6 mm)

### Reference
- [Espressif ESP32-C5 Product Page](https://www.espressif.com/en/products/socs/esp32-c5)
- [ESP32-C5 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf)

## High-Level Architecture

```
+==============================================================================+
|                          APPLICATION LAYER                                    |
|  +----------------+  +----------------+  +------------------+                 |
|  |  MQTT Bridge   |  |    Zigbee      |  | Home Assistant   |                 |
|  | (Zigbee+BLE)   |  |  Coordinator   |  | Discovery (MQTT) |                 |
|  +-------+--------+  +-------+--------+  +--------+---------+                 |
|          |                   |                    |                           |
|  +-------v--------+  +-------v--------+  +--------v---------+                |
|  |  BLE Scanner   |  |   BLE Proxy    |  | ESPHome Native   |                 |
|  |   (Passive)    |  |    (Active)    |  | API Server:6053  |                 |
|  +----------------+  +----------------+  +------------------+                 |
+==============================================================================+
|                            CORE LAYER                                         |
|  +-----------+  +------------+  +----------+  +--------------+               |
|  |  Device   |  |  Command   |  |  Config  |  |   System     |               |
|  |   State   |  |  Handler   |  |  Manager |  |   Monitor    |               |
|  | Publisher |  |            |  |          |  | (Mem/CPU/WDT)|               |
|  +-----------+  +------------+  +----------+  +--------------+               |
+==============================================================================+
|                          PROTOCOL LAYER                                       |
|  +------------+  +--------------+  +-----------+  +------------+             |
|  |   WiFi     |  |    MQTT      |  |  Zigbee   |  | Bluetooth  |             |
|  |  Manager   |  |   Client     |  |   Stack   |  |  LE Stack  |             |
|  | (2.4/5GHz) |  |              |  | (2.4GHz)  |  |  (2.4GHz)  |             |
|  +------------+  +--------------+  +-----------+  +------------+             |
+==============================================================================+
|                    COEXISTENCE LAYER (ESP-IDF)                               |
|           Hardware Arbitration for WiFi/Zigbee/BLE on 2.4GHz                 |
+==============================================================================+
|                          UTILITY LAYER                                        |
|  +------+  +--------+  +---------+  +---------+  +---------+  +----------+  |
|  | JSON |  | Memory |  |  Task   |  | Version |  |   OTA   |  |  Bridge  |  |
|  | Utils|  |  Mgr   |  | Manager |  |         |  | Handler |  |  Events  |  |
|  +------+  +--------+  +---------+  +---------+  +---------+  +----------+  |
+==============================================================================+
|                      ESP-IDF / FreeRTOS                                      |
|  +--------+  +-------+  +--------+  +-------+  +-------------+  +--------+ |
|  |  NVS   |  | Event |  |  WiFi  |  | TCP/IP|  |   Zigbee    |  | NimBLE | |
|  | Flash  |  | Loop  |  | Stack  |  | Stack |  |    Stack    |  | Stack  | |
|  +--------+  +-------+  +--------+  +-------+  +-------------+  +--------+ |
+==============================================================================+
                            ESP32-C5 HARDWARE
           RISC-V CPU @ 240MHz | 384KB SRAM | 8MB PSRAM | 8MB Flash
                 WiFi 6 (802.11ax) | Zigbee 3.0 | BLE 5.0
```

## Module Dependencies

The following diagram shows the dependency relationships between major modules:

```
                              main.c
                                |
                +-------+--------+--------+
                |       |        |       |
                v       v        v       v
            +-------+-------+-------+-------+
            | mqtt_ | zb_   | wifi_ | ble_  |
            |bridge |coord. |mgr    |mgr    |
            +-------+-------+-------+-------+
                |       |        |       |
          +-----+---+---+-----+   |   +---+-----+
          |         |         |   |   |         |
          v         v         v   v   v         v
        core/    zigbee/    mqtt/ esphome/  utils/

Dependencies:
├── core/ depends on: mqtt/, zigbee/, utils/
│   ├── mqtt_bridge → coordinates all protocols
│   ├── device_state_publisher → publishes to MQTT
│   ├── command_handler → sends commands via Zigbee
│   ├── ha_discovery → formats HA messages
│   └── system_monitor → tracks health
│
├── zigbee/ depends on: core/, mqtt/, utils/
│   ├── zb_coordinator → forms network
│   ├── zb_device_handler → manages device registry
│   ├── zb_callbacks → signals device events
│   └── zb_reporting → configures attribute reports
│
├── bluetooth/ depends on: core/, mqtt/, esphome/
│   ├── ble_scanner → scans for advertisements
│   ├── ble_gatt_client → connects to devices
│   ├── ble_device_registry → tracks BLE devices
│   └── ble_esphome_bridge → publishes to ESPHome
│
├── esphome/ depends on: core/, bluetooth/
│   ├── esphome_api → Native API server (port 6053)
│   ├── esphome_entities → creates sensor entities
│   ├── esphome_ble_proxy → BLE proxy protocol
│   └── esphome_protocol → protobuf messaging
│
└── utils/ depends on: none
    ├── json_utils → JSON parsing/creation
    └── version → version information
```

## Data Flow Architecture

### 1. Zigbee → MQTT → Home Assistant Flow

```
Zigbee Device             Coordinator             Home Assistant
      |                       |                          |
      | ZCL Report Attr       |                          |
      |-----(Zigbee)--------->|                          |
      |                       |                          |
      |                   zb_callbacks:                  |
      |                   report_attr()                  |
      |                       |                          |
      |                   mqtt_bridge:                   |
      |                   on_attribute_change()          |
      |                       |                          |
      |                   device_state_publisher:        |
      |                   publish_by_addr()              |
      |                       |                          |
      |                   mqtt_client_publish()          |
      |                       |                          |
      |                    MQTT Broker                   |
      |                       |                          |
      |                       |  Publish Topic           |
      |                       |  zigbee2mqtt/<device>    |
      |                       |--------(MQTT)----------->|
      |                       |                          |
      |                       |                  Home Assistant MQTT
      |                       |                  Discovery receives update
      |                       |                          |
```

### 2. BLE Scanner → ESPHome Bridge → Home Assistant Flow

```
BLE Device           BLE Scanner          ESPHome API        Home Assistant
     |                    |                    |                    |
     | Advertisement      |                    |                    |
     |---(Advertising)--->|                    |                    |
     |                    |                    |                    |
     |                ble_scanner:             |                    |
     |                parse_adv()              |                    |
     |                    |                    |                    |
     |                ble_device_registry:     |                    |
     |                add_device()             |                    |
     |                    |                    |                    |
     |                ble_esphome_bridge:      |                    |
     |                publish_entity()         |                    |
     |                    |                    |                    |
     |                esphome_api:             |                    |
     |                broadcast_state()        |                    |
     |                    |                    |                    |
     |                    |  ESPHome Proto     |                    |
     |                    |  Entity State      |                    |
     |                    |----(TCP:6053)----->|                    |
     |                    |                    |                    |
     |                    |                    | Home Assistant Conn|
     |                    |                    |----(TCP:6053)----->|
     |                    |                    |                    |
     |                    |                    |  ESPHome Client    |
     |                    |                    |  receives state    |
     |                    |                    |                    |
```

### 3. Command Flow: MQTT → Bridge → Zigbee/BLE

```
Home Assistant          MQTT Broker         Command Handler        Zigbee Device
      |                      |                   |                      |
      | Command Publish      |                   |                      |
      | zigbee2mqtt/light/set|                   |                      |
      |----(MQTT)----------->|                   |                      |
      |                      |                   |                      |
      |                   mqtt_client:           |                      |
      |                   message_callback()     |                      |
      |                      |                   |                      |
      |                   mqtt_bridge:           |                      |
      |                   handle_command()       |                      |
      |                      |                   |                      |
      |                   command_handler:       |                      |
      |                   process()              |                      |
      |                      |                   |                      |
      |                      |  Extract name     |                      |
      |                      |  Parse JSON       |                      |
      |                      |  Lookup device    |                      |
      |                      |----(Lock)-------->|                      |
      |                      |                   |                      |
      |                      |  esp_zb_zcl_      |                      |
      |                      |  cmd_req()        |                      |
      |                      |                   |----(ZCL)----------->|
      |                      |                   |                      |
      |                      |                   |<---(Response)--------|
      |                      |                   |                      |
      |                      |  Update Stats     |                      |
      |                      |<---(Unlock)-------|                      |
      |                      |                   |                      |
```

## FreeRTOS Task Architecture

### Task Priorities and Responsibilities

| Task | Priority | Stack | Purpose | Module |
|------|----------|-------|---------|--------|
| `zb_main` | 20 | 8KB | Zigbee stack main event loop (highest priority) | zigbee/ |
| `nimble_host` | 18 | 4KB | BLE/NimBLE host processing | bluetooth/ |
| `ble_scanner` | 8 | 4KB | BLE passive scanning and device tracking | bluetooth/ |
| `esphome_srv` | 6 | 8KB | ESPHome API server and client handlers | esphome/ |
| `mqtt_task` | 5 | 4KB | MQTT client event loop and subscriptions | mqtt/ |
| `wifi_task` | 5 | 4KB | WiFi connection management and reconnection | wifi/ |
| `system_monitor` | 3 | 2KB | Health monitoring, memory tracking (low priority) | core/ |
| `ota_task` | 2 | 6KB | OTA firmware updates (background, on-demand) | ota/ |

**Priority Levels** (0-25, higher = more priority):
- **20+**: Critical/Real-time (Zigbee stack)
- **18**: High (BLE stack)
- **6-8**: Protocol processing (ESPHome, BLE scanning)
- **5**: Network I/O (MQTT, WiFi)
- **3**: Monitoring (system health, low CPU usage)
- **2**: Background (OTA updates)

**Task Interaction Flow**:
```
┌─────────────────┐
│  zb_main (20)   |  Zigbee events, callbacks
└────────┬────────┘
         |
         +──> zb_callbacks → mqtt_bridge_on_*
              |
              +──> device_state_publish() (inline)
                   |
                   +──> mqtt_task (5)
                        |
                        +──> MQTT Broker
```

## Memory Layout and Constraints

> **Optimization Guide**: For detailed optimization strategies, monitoring, and common issues, see [MEMORY_OPTIMIZATION.md](MEMORY_OPTIMIZATION.md).

### SRAM and PSRAM Configuration

```
Total SRAM: 384KB
├── FreeRTOS Kernel: ~32 KB
│   ├── Scheduler structures
│   ├── Idle task stack
│   └── Timer task stack
├── Task Stacks: ~40 KB
│   ├── zb_main: 8KB (highest priority)
│   ├── esphome tasks: 8KB
│   ├── mqtt_task: 4KB
│   ├── wifi_task: 4KB
│   ├── ble_scanner: 4KB
│   ├── system_monitor: 2KB
│   └── Other tasks: 10KB
├── Static Data: ~50 KB
│   ├── Device registries and tables
│   ├── Configuration structures
│   ├── Protocol stack buffers
│   └── String constants
└── Heap (Dynamic): ~260 KB available
    ├── Network buffers
    ├── cJSON parsing (temporary)
    ├── MQTT message buffers
    └── BLE scan result cache

Total PSRAM: 8MB (External)
├── Large buffers (OTA firmware)
├── Extended device caches
├── BLE scan history
└── Large JSON documents (> 2KB)
```

### Heap Usage Targets and Thresholds

| Configuration | Free Heap Target | Warning Threshold | Critical Threshold |
|--------------|------------------|-------------------|-------------------|
| Zigbee Only | 50 KB+ | 40 KB | 30 KB |
| Zigbee + BLE | 40 KB+ | 30 KB | 20 KB |
| Full Stack (Z+B+ESPHome) | 35 KB+ | 25 KB | 15 KB |

**Device Limits**:
- Maximum Zigbee devices: 50 (standalone) or 30 (with BLE enabled)
- Maximum BLE devices tracked: 50
- MQTT publish queue depth: 32 messages
- ESPHome API client connections: 4 (max simultaneous)

## Key Synchronization Points

> **Thread Safety Guide**: See [CODE_STYLE.md](CODE_STYLE.md#thread-safety-guidelines) for comprehensive thread safety rules. For troubleshooting, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md#esp-zigbee-sdk-v16x-issues).

All inter-task communication uses these synchronization primitives:

### 1. Zigbee Lock (Critical for Thread Safety)

```c
// ALL Zigbee ZCL API calls from non-Zigbee tasks MUST use this lock:
esp_zb_lock_acquire(portMAX_DELAY);
esp_err_t ret = esp_zb_zcl_on_off_cmd_req(&cmd);
esp_zb_lock_release();

// Affected APIs:
// - esp_zb_zcl_*_cmd_req()      (all ZCL commands)
// - esp_zb_zcl_read_attr_cmd_req()
// - esp_zb_zcl_write_attr_cmd_req()
// - esp_zb_zcl_default_resp_cmd_req()
```

**Used By**: command_handler, mqtt_bridge, system_monitor

### 2. Device Handler Mutex

```c
// Protects device registry access:
xSemaphoreTake(s_device_mutex, portMAX_DELAY);
zb_device_t *device = zb_device_handler_find_by_short_addr(addr);
xSemaphoreGive(s_device_mutex);

// Protects: Device add/remove, state updates, attribute caching
```

**Used By**: zb_callbacks, command_handler, device_state_publisher

### 3. MQTT Publish Queue

```c
// Thread-safe message queue for publishing:
mqtt_message_t msg = {...};
xQueueSend(s_publish_queue, &msg, pdMS_TO_TICKS(100));

// Serializes access to MQTT connection:
// - Prevents simultaneous publish operations
// - Rate-limits to prevent broker overload
```

**Used By**: device_state_publisher, ha_discovery, mqtt_logger

### 4. WiFi State Mutex

```c
// Protects WiFi connection state:
xSemaphoreTake(s_wifi_state_mutex, portMAX_DELAY);
if (s_wifi_state == WIFI_CONNECTED) {
    // Perform WiFi operation
}
xSemaphoreGive(s_wifi_state_mutex);
```

**Used By**: mqtt_task, wifi_manager, system_monitor

### 5. ESPHome API Mutexes

```c
// Protects client connection list:
xSemaphoreTake(s_api_client_mutex, portMAX_DELAY);
esphome_client_t *client = find_client_by_fd(fd);
xSemaphoreGive(s_api_client_mutex);

// Protects entity state updates:
xSemaphoreTake(s_entity_state_mutex, portMAX_DELAY);
entity_state_update(&entity, new_value);
xSemaphoreGive(s_entity_state_mutex);
```

**Used By**: esphome_api, ble_esphome_bridge

### Synchronization Point Interaction Diagram

```
┌──────────────────────────────────────────────────────────────┐
│                MQTT Incoming Message (mqtt_task)             │
├──────────────────────────────────────────────────────────────┤
│ 1. Parse topic/payload                                        │
│ 2. Acquire Device Mutex                                       │
│    └─> Lookup device in registry                              │
│ 3. Release Device Mutex                                       │
│ 4. Validate command parameters                                │
│ 5. Acquire Zigbee Lock                                        │
│    └─> Call esp_zb_zcl_*_cmd_req()                            │
│ 6. Release Zigbee Lock                                        │
│ 7. Post message to publish queue (with timeout)               │
│ 8. Return to MQTT event loop                                  │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│            Zigbee Callback (zb_main task context)             │
├──────────────────────────────────────────────────────────────┤
│ 1. Handle Zigbee event (no locks needed, in zb_main)          │
│ 2. Acquire Device Mutex                                       │
│    └─> Update device registry                                 │
│ 3. Release Device Mutex                                       │
│ 4. Call mqtt_bridge_on_attribute_change()                     │
│ 5. Return to zb_main                                          │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│      Device State Publishing (inline in callback context)     │
├──────────────────────────────────────────────────────────────┤
│ 1. Format JSON payload                                        │
│ 2. Build MQTT topic                                           │
│ 3. Publish via mqtt_client (queued to mqtt_task)              │
│ 4. Return to caller                                           │
└──────────────────────────────────────────────────────────────┘
```

### Module Dependencies Graph

```
                                 main.c
                                    |
               +--------------------+--------------------+
               |                                         |
               v                                         v
        +-------------+                          +---------------+
        | mqtt_bridge |<------------------------>| zb_coordinator|
        +------+------+                          +-------+-------+
               |                                         |
       +-------+-------+-------+-------+         +-------+-------+
       |       |       |       |       |         |       |       |
       v       v       v       v       v         v       v       v
  +--------+ +-----+ +-----+ +------+ +----+  +------+ +------+ +------+
  |device_ | |cmd_ | |ha_  | |bridge| |mqtt|  |zb_   | |zb_   | |zb_   |
  |state_  | |hand-| |disc-| |_req_ | |_log|  |call- | |device| |net-  |
  |publish | |ler  | |overy| |handlr| |ger |  |backs | |_handl| |work  |
  +---+----+ +--+--+ +--+--+ +--+---+ +----+  +---+--+ +---+--+ +--+---+
      |         |       |       |                 |        |       |
      +---------+-------+-------+-----------------+--------+-------+
                                |
                    +-----------+-----------+
                    |                       |
                    v                       v
             +-------------+         +-------------+
             |gateway_mqtt |         |config_manager|
             +------+------+         +------+------+
                    |                       |
                    v                       v
             +-------------+         +-------------+
             | wifi_manager|         |system_monitor|
             +-------------+         +-------------+
```

## Core Components

### 1. MQTT Bridge (`main/core/mqtt_bridge.c`)

The central coordinator between all protocols and the MQTT broker.

**Responsibilities**:
- Initialize and coordinate all sub-modules
- Route MQTT messages to appropriate handlers
- Manage bridge state (online/offline)
- Publish device lists and bridge info
- Handle device join/leave events

**Key Functions**:
```c
esp_err_t mqtt_bridge_init(void);           // Initialize all sub-modules
esp_err_t mqtt_bridge_start(void);          // Start bridge, subscribe topics
esp_err_t mqtt_bridge_stop(void);           // Stop bridge, unsubscribe
esp_err_t mqtt_bridge_handle_command(...);  // Route incoming messages
esp_err_t mqtt_bridge_on_device_join(...);  // Handle Zigbee device joins
esp_err_t mqtt_bridge_on_attribute_change(...); // Handle attribute reports
```

**Sub-modules Initialized**:
- `device_state_publisher` - Publishes device states to MQTT
- `command_handler` - Processes MQTT commands
- `ha_discovery` - Home Assistant auto-discovery
- `bridge_request_handler` - Bridge control commands
- `bridge_events` - Event publishing
- `mqtt_logger` - MQTT-based logging

### 2. Zigbee Coordinator (`main/zigbee/zb_coordinator.c`)

Manages the Zigbee network as the coordinator (Trust Center).

**Responsibilities**:
- Initialize and configure ESP-Zigbee-SDK
- Form and manage the Zigbee network
- Handle permit join with timer management
- Track network health and statistics
- Manage the coordinator task

**Key State Machine**:
```
UNINITIALIZED --> INITIALIZED --> STARTING --> RUNNING
                                      |            |
                                      v            v
                                   ERROR <----- STOPPED
```

**Thread-Safe API Pattern**:
```c
esp_err_t zb_coordinator_permit_join(uint8_t duration)
{
    xSemaphoreTake(s_coordinator_mutex, portMAX_DELAY);

    // Stop any existing timers
    stop_permit_join_timers();

    esp_err_t ret = esp_zb_bdb_open_network(duration_ms);

    if (ret == ESP_OK) {
        // Update internal state
        s_permit_join_state.enabled = true;
        // Start timers...
    }

    xSemaphoreGive(s_coordinator_mutex);
    return ret;
}
```

### 3. Device State Publisher (`main/core/device_state_publisher.c`)

Publishes Zigbee device states to MQTT.

**Responsibilities**:
- Convert device states to JSON
- Publish to device-specific topics
- Manage availability messages
- Handle attribute updates

**Topic Structure**:
```
zigbee2mqtt/<friendly_name>              # Device state
zigbee2mqtt/<friendly_name>/availability # Online/offline status
```

### 4. Command Handler (`main/core/command_handler.c`)

Processes incoming MQTT commands and sends to Zigbee devices.

**Command Flow**:
1. Extract friendly name from topic
2. Find device in registry
3. Parse JSON payload
4. Acquire Zigbee lock
5. Send ZCL command
6. Release lock
7. Update TX statistics

**Thread-Safe ZCL Command**:
```c
esp_err_t command_send_on_off(uint16_t short_addr, uint8_t endpoint, bool on)
{
    esp_zb_zcl_on_off_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID
                            : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID,
    };

    // Thread-safety: Acquire Zigbee lock before API call
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_on_off_cmd_req(&cmd_req);
    esp_zb_lock_release();

    if (ret == ESP_OK) {
        zb_coordinator_update_tx_count();
    }
    return ret;
}
```

### 5. System Monitor (`main/core/system_monitor.c`)

Monitors system health and resources.

**Tracked Metrics**:
- Uptime (seconds)
- Free heap / minimum free heap
- Free PSRAM
- Heap fragmentation
- Task count and CPU usage
- WiFi/MQTT connection status
- WiFi RSSI

**Health Levels**:
```c
typedef enum {
    SYSTEM_HEALTH_GOOD = 0,      // All systems healthy
    SYSTEM_HEALTH_WARNING,       // Minor issues (high memory usage)
    SYSTEM_HEALTH_CRITICAL       // Critical issues (very low memory)
} system_health_t;
```

## Protocol Stacks

### Zigbee Stack (ESP-Zigbee-SDK)

**SDK Version**: v1.6.0+ (compile-time validated)

```
+--------------------------------------------------+
|                Application Layer                  |
|  zb_coordinator | zb_callbacks | zb_device_handler|
+--------------------------------------------------+
|              ESP-Zigbee-SDK v1.6.x                |
|  +--------------------------------------------+  |
|  | ZCL (Zigbee Cluster Library)               |  |
|  |   On/Off | Level | Color | Temperature     |  |
|  |   Occupancy | Door Lock | Power Metering   |  |
|  +--------------------------------------------+  |
|  | APS (Application Support Sub-layer)        |  |
|  |   Binding | Groups | Security             |  |
|  +--------------------------------------------+  |
|  | NWK (Network Layer)                        |  |
|  |   Routing | Network Formation | Addresses  |  |
|  +--------------------------------------------+  |
|  | MAC (IEEE 802.15.4)                        |  |
|  +--------------------------------------------+  |
+--------------------------------------------------+
|              IEEE 802.15.4 Radio                 |
|                  (2.4 GHz)                       |
+--------------------------------------------------+
```

**Coordinator Configuration**:
```c
esp_zb_cfg_t zb_nwk_cfg = {
    .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
    .install_code_policy = false,
    .nwk_cfg.zczr_cfg = {
        .max_children = ZB_COORDINATOR_MAX_CHILDREN,  // 32
    },
};
```

### Bluetooth Stack (NimBLE)

```
+--------------------------------------------------+
|              Application Layer                    |
|  ble_manager | ble_scanner | ble_gatt_client     |
+--------------------------------------------------+
|                  NimBLE Host                      |
|  +--------------------------------------------+  |
|  | GAP (Generic Access Profile)               |  |
|  |   Advertising | Scanning | Connection      |  |
|  +--------------------------------------------+  |
|  | GATT (Generic Attribute Profile)           |  |
|  |   Services | Characteristics | Descriptors |  |
|  +--------------------------------------------+  |
|  | ATT (Attribute Protocol)                   |  |
|  +--------------------------------------------+  |
|  | L2CAP (Logical Link Control)               |  |
|  +--------------------------------------------+  |
+--------------------------------------------------+
|               NimBLE Controller                   |
|                BLE 5.0 Radio                     |
|                  (2.4 GHz)                       |
+--------------------------------------------------+
```

**BLE Manager Configuration**:
```c
typedef struct {
    bool enable_scanner;           // Enable passive scanning
    bool coexist_with_zigbee;      // Enable coexistence mode
    uint16_t scan_interval_ms;     // Scan interval (default: 10000)
    uint16_t scan_window_ms;       // Scan window (default: 1000)
    uint8_t max_tracked_devices;   // Max BLE devices (default: 50)
} ble_manager_config_t;
```

### MQTT Communication

```
+--------------------------------------------------+
|              gateway_mqtt Module                  |
|  - Connection management with auto-reconnect     |
|  - QoS 0/1/2 support                             |
|  - Retained message handling                     |
|  - Callback registration for messages            |
+--------------------------------------------------+
|              ESP-MQTT Client                      |
|  - TLS/SSL support (MQTTS)                       |
|  - Keep-alive management                         |
|  - Last Will Testament (LWT)                     |
+--------------------------------------------------+
|                  TCP/IP Stack                     |
|                  WiFi Driver                      |
+--------------------------------------------------+
```

**MQTT Topic Structure**:
```
zigbee2mqtt/                       # Base topic
zigbee2mqtt/bridge/state           # Bridge online/offline
zigbee2mqtt/bridge/info            # Bridge information
zigbee2mqtt/bridge/devices         # Device list
zigbee2mqtt/bridge/logging         # Log messages
zigbee2mqtt/bridge/request/...     # Control requests
zigbee2mqtt/bridge/response/...    # Control responses
zigbee2mqtt/<device>/              # Device state
zigbee2mqtt/<device>/availability  # Device availability
zigbee2mqtt/<device>/set           # Device commands (subscribe)
zigbee2mqtt/<device>/get           # State requests (subscribe)
homeassistant/<domain>/<id>/config # HA Discovery configs
```

### ESPHome Native API

```
+--------------------------------------------------+
|             esphome_api Module                    |
|  - TCP Server on port 6053                       |
|  - Client connection management (max 4)          |
|  - Entity state broadcasting                     |
|  - Service call handling                         |
+--------------------------------------------------+
|            ESPHome Protocol                       |
|  - Protobuf-based messaging                      |
|  - Entity list requests                          |
|  - State subscriptions                           |
|  - Log subscriptions                             |
|  - Keepalive (ping/pong)                         |
+--------------------------------------------------+
|                   TCP Socket                      |
+--------------------------------------------------+
```

## FreeRTOS Task Structure

### Task Lifecycle

```
                    +-----------+
                    |  CREATED  |
                    +-----+-----+
                          |
                          v
                    +-----------+
            +------>|   READY   |<------+
            |       +-----+-----+       |
            |             |             |
            |             v             |
            |       +-----------+       |
            |       |  RUNNING  |-------+
            |       +-----+-----+
            |             |
            |             v
            |       +-----------+
            +-------|  BLOCKED  |
                    | (waiting) |
                    +-----------+
```

## Inter-Task Communication

### Synchronization Primitives

**Mutexes**:
```c
// Zigbee coordinator mutex - protects coordinator state
static SemaphoreHandle_t s_coordinator_mutex;

// WiFi state mutex - protects WiFi connection state
static SemaphoreHandle_t s_wifi.state_mutex;

// BLE manager mutex - protects BLE state
static SemaphoreHandle_t s_manager_mutex;

// ESPHome API mutex - protects client list
static SemaphoreHandle_t s_api.mutex;
```

**Event Groups**:
```c
// Connection status event group
static EventGroupHandle_t s_connection_event_group;
#define WIFI_CONNECTED_BIT    BIT0
#define MQTT_CONNECTED_BIT    BIT1
#define SYSTEM_READY_BIT      (WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT)

// WiFi manager event group
static EventGroupHandle_t s_wifi.event_group;
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1
#define WIFI_DISCONNECTED_BIT BIT2

// ESPHome server event group
static EventGroupHandle_t s_api.event_group;
#define EVENT_STOP_SERVER     BIT0
#define EVENT_CLIENT_DISCONNECT BIT1
```

**Timers**:
```c
// Permit join timers
static esp_timer_handle_t s_permit_join_timer;       // Expiry (one-shot)
static esp_timer_handle_t s_permit_join_update_timer; // Updates (periodic)

// WiFi timers
static esp_timer_handle_t s_wifi.uptime_timer;       // Uptime tracking
static esp_timer_handle_t s_wifi.reconnect_timer;    // Reconnect backoff

// ESPHome uptime timer
static esp_timer_handle_t s_api.uptime_timer;
```

### Callback Mechanism

**Pattern**: Register callbacks for asynchronous event notification.

```c
// MQTT connection callback
typedef void (*mqtt_connection_callback_t)(bool connected);
esp_err_t mqtt_client_register_connection_callback(mqtt_connection_callback_t cb);

// MQTT message callback
typedef void (*mqtt_message_callback_t)(const char *topic, const char *data, size_t len);
esp_err_t mqtt_client_register_callback(mqtt_message_callback_t cb);

// ESPHome connection callback
typedef void (*esphome_api_connection_cb_t)(uint8_t client_id, bool connected, bool authenticated);
```

### Message Flow Diagram

```
+----------+     +----------+     +----------+     +----------+
|  WiFi    |     |   MQTT   |     |  Bridge  |     |  Zigbee  |
|  Task    |     |   Task   |     |  Logic   |     |   Task   |
+----+-----+     +----+-----+     +----+-----+     +----+-----+
     |                |                |                |
     | WiFi Connected |                |                |
     |--------------->|                |                |
     |                | MQTT Connected |                |
     |                |--------------->|                |
     |                |                | Start Bridge   |
     |                |                |--------------->|
     |                |                |                |
     |                | MQTT Message   |                |
     |                |<---------------|                |
     |                |--------------->|                |
     |                |                | Parse Command  |
     |                |                |--------------->|
     |                |                |                | ZCL Command
     |                |                |                |------------>
     |                |                |                |
     |                |                |<---------------| ZCL Response
     |                |                | Publish State  |
     |                |<---------------|                |
     |                |                |                |
```

## Key Patterns

### 1. Callback Flow for Zigbee Actions

The Zigbee stack uses an action handler callback pattern for ZCL command processing:

```
+-------------------+
|  esp_zb_start()   |
+--------+----------+
         |
         v
+-------------------+
| esp_zb_stack_     |
| main_loop()       |
+--------+----------+
         |
         v (Zigbee events)
+-------------------+
| esp_zb_app_       |
| signal_handler()  |
+--------+----------+
         |
         v (Device join, leave, etc.)
+-------------------+
| zb_callback_      |
| signal_handler()  |
+--------+----------+
         |
    +----+----+----+----+
    |    |    |    |    |
    v    v    v    v    v
  Device Network Permit BDB  Other
  Annce  Formed  Join  Done signals
```

**ZCL Action Handler Flow**:
```
+---------------------------+
| esp_zb_core_action_       |
| handler_register()        |
+-----------+---------------+
            |
            v (ZCL messages)
+---------------------------+
| zb_coordinator_action_    |
| handler()                 |
+-----------+---------------+
            |
    +-------+-------+-------+
    |       |       |       |
    v       v       v       v
  Report  Read   Write  Default
  Attr    Resp   Resp   Response
    |
    v
+---------------------------+
| zb_callback_report_attr() |
+-----------+---------------+
            |
            v
+---------------------------+
| mqtt_bridge_on_attribute_ |
| change()                  |
+-----------+---------------+
            |
            v
+---------------------------+
| device_state_publish()    |
+---------------------------+
```

### 2. Thread-Safety Patterns

**Mutex Usage Pattern**:
```c
// Pattern 1: Simple mutex lock/unlock
esp_err_t function_thread_safe(args) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Critical section
    esp_err_t ret = do_operation();

    xSemaphoreGive(s_mutex);
    return ret;
}

// Pattern 2: Mutex with timeout
esp_err_t function_with_timeout(args) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = do_operation();

    xSemaphoreGive(s_mutex);
    return ret;
}

// Pattern 3: State read without lock (atomic)
bool is_enabled(void) {
    return s_enabled;  // Simple bool read is atomic on ESP32
}
```

**ESP-Zigbee-SDK Lock Pattern**:
```c
// The Zigbee stack has its own internal lock that MUST be used
// for all ZCL API calls from non-Zigbee tasks

esp_err_t send_zigbee_command(params) {
    // Acquire Zigbee stack lock
    esp_zb_lock_acquire(portMAX_DELAY);

    // Now safe to call Zigbee APIs
    esp_err_t ret = esp_zb_zcl_xxx_cmd_req(&cmd);

    // Release lock
    esp_zb_lock_release();

    return ret;
}
```

### 3. Lock Sequence for MQTT to Zigbee Commands

```
MQTT Broker                                     Zigbee Device
     |                                               |
     | 1. Publish command                            |
     |--------------------------------------------->  |
     |                                               |
     |  mqtt_client (callback context)               |
     |  +----------------------------------+         |
     |  | 2. mqtt_message_callback()       |         |
     |  +----------------------------------+         |
     |                    |                          |
     |                    v                          |
     |  mqtt_bridge                                  |
     |  +----------------------------------+         |
     |  | 3. mqtt_bridge_handle_command()  |         |
     |  +----------------------------------+         |
     |                    |                          |
     |                    v                          |
     |  command_handler                              |
     |  +----------------------------------+         |
     |  | 4. command_handler_process()     |         |
     |  |    - Extract device name         |         |
     |  |    - Parse JSON                  |         |
     |  |    - Validate command            |         |
     |  +----------------------------------+         |
     |                    |                          |
     |                    v                          |
     |  +----------------------------------+         |
     |  | 5. esp_zb_lock_acquire()         |  <-- LOCK
     |  +----------------------------------+         |
     |                    |                          |
     |                    v                          |
     |  +----------------------------------+         |
     |  | 6. esp_zb_zcl_xxx_cmd_req()      |         |
     |  +----------------------------------+         |
     |                    |                          |
     |                    v                          |
     |  +----------------------------------+         |
     |  | 7. esp_zb_lock_release()         |  <-- UNLOCK
     |  +----------------------------------+         |
     |                    |                          |
     |                    v                          |
     |                 Zigbee TX                     |
     |  ------------------------------------------->  |
     |                                               |
     |                                  8. ZCL Command
     |                                               |
```

### 4. Event-Driven Architecture

**ESP-IDF Event Loop Usage**:
```c
// WiFi events flow through the default event loop
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
        case WIFI_EVENT_STA_CONNECTED:
            // Handle connection
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            // Handle disconnection, trigger reconnect
            break;
    }
}

// Registration during init
esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                          &wifi_event_handler, NULL);
esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                          &ip_event_handler, NULL);
```

**Zigbee Signal Handler**:
```c
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    esp_zb_app_signal_type_t sig_type = *signal_struct->p_app_signal;

    switch (sig_type) {
        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            zb_callback_network_formed();
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
            zb_callback_device_announce(params);
            break;

        case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
            zb_callback_permit_join_changed(duration);
            break;

        case ESP_ZB_NLME_STATUS_INDICATION:
            // Handle network layer status
            break;
    }
}
```

## Memory Management

### Memory Layout

```
+==============================================================+
|                    DRAM (384 KB SRAM)                        |
+==============================================================+
|  FreeRTOS Kernel              |  ~32 KB                      |
|  - Scheduler                  |                              |
|  - Idle task stack            |                              |
|  - Timer task stack           |                              |
+-------------------------------+------------------------------+
|  Task Stacks                  |  ~40 KB                      |
|  - zb_coord: 6KB              |                              |
|  - mqtt_task: 4KB             |                              |
|  - wifi_task: 4KB             |                              |
|  - nimble_host: 4KB (planned) |                              |
|  - sys_monitor: 4KB           |                              |
|  - esphome tasks: 8KB         |                              |
|  - Others: 10KB               |                              |
+-------------------------------+------------------------------+
|  Static Data                  |  ~50 KB                      |
|  - Device tables              |                              |
|  - Configuration structures   |                              |
|  - Protocol stack data        |                              |
|  - String constants           |                              |
+-------------------------------+------------------------------+
|  Heap (Dynamic)               |  ~260 KB available           |
|  - Network buffers            |                              |
|  - JSON parsing (cJSON)       |                              |
|  - Temporary strings          |                              |
|  - MQTT message buffers       |                              |
+==============================================================+

+==============================================================+
|                   PSRAM (8 MB External)                      |
+==============================================================+
|  Large Buffers                                               |
|  - OTA firmware buffer (configurable)                        |
|  - Large JSON documents (> 2KB)                              |
|  - Extended device tables (if needed)                        |
|  - BLE scan results cache                                    |
+==============================================================+

+==============================================================+
|                   Flash (16 MB)                              |
+==============================================================+
|  Partition           | Size    | Purpose                     |
+----------------------+---------+-----------------------------+
|  nvs                 | 24 KB   | NVS storage                 |
|  phy_init            | 4 KB    | PHY calibration             |
|  factory             | 6 MB    | Main application            |
|  ota_0               | 6 MB    | OTA update slot             |
|  zb_storage          | 20 KB   | Zigbee network data         |
|  zb_fct              | 4 KB    | Zigbee factory data         |
|  spiffs              | ~3.9 MB | File storage (optional)     |
+==============================================================+
```

### Heap Usage Targets

| Configuration | Free Heap Target | Warning | Critical |
|--------------|------------------|---------|----------|
| Zigbee Only | 50 KB+ | 40 KB | 30 KB |
| Zigbee + BLE | 40 KB+ | 30 KB | 20 KB |
| Full (Z+B+ESPHome) | 35 KB+ | 25 KB | 15 KB |

### Memory Monitoring

```c
// System monitor tracks memory continuously
typedef struct {
    size_t free_heap;           // Current free heap
    size_t min_free_heap;       // Minimum free heap since boot
    size_t free_psram;          // Current free PSRAM
    uint8_t heap_fragmentation; // Fragmentation percentage
} memory_stats_t;

// Health calculation
system_health_t calculate_system_health(void) {
    memory_status_t mem_status = memory_manager_get_status();

    if (mem_status == MEMORY_STATUS_CRITICAL) {
        return SYSTEM_HEALTH_CRITICAL;  // < 20KB free
    }
    if (mem_status == MEMORY_STATUS_WARNING) {
        return SYSTEM_HEALTH_WARNING;   // < 40KB free
    }
    return SYSTEM_HEALTH_GOOD;
}
```

### PSRAM Allocation

PSRAM is available for large allocations:

```c
// Allocate from PSRAM explicitly
void *large_buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);

// Check PSRAM availability
size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

// Allocate preferring PSRAM, fallback to internal
void *buffer = heap_caps_malloc_prefer(size, 2,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
```

### Buffer Management

Standardized buffer sizes defined in `gateway_defaults.h`:

```c
#define GW_BUFFER_SIZE_TINY     32    // Error codes, short strings
#define GW_BUFFER_SIZE_SMALL    64    // Names, identifiers
#define GW_BUFFER_SIZE_MEDIUM   128   // Topics, friendly names
#define GW_BUFFER_SIZE_LARGE    256   // Command topics
#define GW_BUFFER_SIZE_XLARGE   512   // Standard JSON
#define GW_BUFFER_SIZE_HUGE     1024  // Large JSON documents
#define GW_BUFFER_SIZE_MAX      2048  // Maximum standard buffer
```

## Configuration System

### Three-Tier Configuration

```
+------------------+     +------------------+     +------------------+
|     Kconfig      |     |       NVS        |     |      MQTT        |
|  (Compile-time)  | --> |   (Persistent)   | --> |    (Runtime)     |
+------------------+     +------------------+     +------------------+
| - Default values |     | - Saved config   |     | - Remote updates |
| - Build options  |     | - Runtime mods   |     | - Live changes   |
| - Feature flags  |     | - Survives boot  |     | - bridge/config  |
+------------------+     +------------------+     +------------------+
```

### Configuration Manager

```c
// Configuration structure
typedef struct {
    // Network
    char wifi_ssid[CONFIG_SSID_MAX_LEN];
    char wifi_password[CONFIG_PASSWORD_MAX_LEN];

    // MQTT
    char mqtt_broker_url[CONFIG_URL_MAX_LEN];
    uint16_t mqtt_port;
    char mqtt_username[CONFIG_ID_MAX_LEN];
    char mqtt_password[CONFIG_PASSWORD_MAX_LEN];
    char mqtt_client_id[CONFIG_ID_MAX_LEN];
    uint16_t mqtt_keepalive;
    uint8_t mqtt_qos;

    // Zigbee
    uint16_t zigbee_pan_id;
    uint8_t zigbee_channel;
    uint8_t zigbee_max_children;
    bool zigbee_permit_join_on_boot;

    // Gateway
    uint32_t device_publish_interval_ms;
    bool ha_discovery_enabled;
    bool bridge_logging_enabled;

    // System
    esp_log_level_t log_level;
    bool ota_enabled;
    char ota_url[CONFIG_URL_MAX_LEN];

    // Version
    uint32_t config_version;
} gateway_config_t;
```

### NVS Storage

```c
// Initialize and load configuration
esp_err_t config_manager_init(void)
{
    // Try NVS first
    esp_err_t ret = config_manager_load(&s_current_config);

    if (ret == ESP_ERR_NOT_FOUND) {
        // No config in NVS, load from Kconfig defaults
        config_manager_load_defaults(&s_current_config);
        // Save to NVS for future boots
        config_manager_save(&s_current_config);
    }

    return ESP_OK;
}

// NVS namespace: "gw_config"
// Key: "config" (blob of gateway_config_t)
```

### Kconfig Menu Structure

```
ESP32-C5 Zigbee Gateway Configuration
├── WiFi Configuration
│   ├── WiFi SSID
│   └── WiFi Password
├── MQTT Configuration
│   ├── Broker URL
│   ├── Broker Port
│   ├── Username/Password
│   ├── Client ID
│   └── QoS Level
├── Zigbee Configuration
│   ├── Device Type (Coordinator/Router)
│   ├── PAN ID
│   ├── Channel (11-26)
│   └── Max Children
├── Gateway Features
│   ├── Home Assistant Discovery
│   ├── Bridge Logging to MQTT
│   └── Publish Interval
└── OTA Configuration
    ├── Enable OTA
    ├── Firmware URL
    └── Check Interval
```

## Module Structure

### Directory Layout

```
main/
├── main.c                     # Application entry point
│
├── core/                      # Core application logic
│   ├── bridge/                # MQTT bridge and request handling
│   │   ├── mqtt_bridge.c/h        # Central bridge coordinator
│   │   ├── bridge_request_handler.c/h
│   │   ├── bridge_events.c/h
│   │   └── bridge_response.c/h
│   ├── discovery/             # Home Assistant discovery
│   │   └── ha_discovery.c/h
│   ├── monitoring/            # System monitoring
│   │   ├── system_monitor.c/h
│   │   ├── memory_manager.c/h
│   │   └── task_manager.c/h
│   ├── device_state_publisher.c/h
│   ├── command_handler.c/h
│   ├── mqtt_logger.c/h
│   ├── config_manager.c/h
│   ├── gateway_defaults.h     # Cross-module constants
│   ├── gateway_buffer_sizes.h
│   ├── gateway_timeouts.h
│   └── ha_constants.h         # HA device classes, units
│
├── zigbee/                    # Zigbee coordinator
│   ├── zb_coordinator.c/h     # Coordinator core
│   ├── zb_network.c/h         # Network management
│   ├── zb_device_handler.c/h  # Device registry
│   ├── zb_callbacks.c/h       # Event callbacks
│   ├── zb_availability.c/h    # Device availability tracking
│   ├── zb_interview.c/h       # Device interview
│   ├── zb_reporting.c/h       # Attribute reporting config
│   ├── zb_binding.c/h         # Binding table
│   ├── zb_groups.c/h          # Group management
│   ├── zb_scenes.c/h          # Scene support
│   ├── zb_install_codes.c/h   # Install code support
│   ├── zb_diagnostics.c/h
│   ├── zb_ota.c/h             # Zigbee OTA updates
│   ├── zb_zcl_helpers.c/h     # Thread-safe ZCL command helpers
│   ├── zb_cluster_closures.c/h    # Closures cluster (door locks, window coverings)
│   ├── zb_cluster_electrical.c/h  # Electrical measurement cluster
│   ├── zb_cluster_hvac.c/h        # HVAC cluster (thermostat, fan control)
│   ├── zb_cluster_measurement.c/h # Measurement clusters (temperature, humidity)
│   ├── zb_cluster_security.c/h    # Security cluster (IAS zone, ACE)
│   ├── zb_alarms.c/h          # Alarm cluster support
│   ├── zb_demand_response.c/h # Demand response and load control
│   ├── zb_hvac_dehumid.c/h    # HVAC dehumidification
│   ├── zb_constants.h
│   └── zb_unit_scales.h       # Unit conversion scale factors
│
├── mqtt/                      # MQTT client
│   ├── gateway_mqtt.c/h       # MQTT API implementation
│   └── mqtt_topics.c/h        # Topic management
│
├── wifi/                      # WiFi manager
│   ├── wifi_manager.c/h
│   └── wifi_config.c/h
│
├── bluetooth/                 # Bluetooth LE
│   ├── ble_manager.c/h        # NimBLE stack initialization
│   ├── ble_scanner.c/h        # BLE advertisement scanner
│   ├── ble_gatt_client.c/h    # Thread-safe GATT client
│   ├── ble_security.c/h       # Pairing, bonding, encryption
│   ├── ble_esphome_bridge.c/h # Bridge to ESPHome entities
│   ├── ble_common.h           # BLE constants
│   ├── ble_battery_service.c/h    # Battery service support
│   ├── ble_gatt_discovery.c/h     # GATT service discovery
│   └── devices/               # Device-specific parsers
│       ├── ble_device_registry.c/h  # Device tracking with LRU eviction
│       ├── ble_xiaomi.c/h     # Xiaomi sensor parser
│       ├── ble_govee.c/h      # Govee device parser
│       ├── ble_beacon.c/h     # iBeacon/Eddystone parser
│       ├── ble_ruuvi.c/h      # RuuviTag parser
│       ├── ble_switchbot.c/h  # SwitchBot device parser
│       ├── ble_qingping.c/h   # Qingping sensor parser
│       └── ble_inkbird.c/h    # Inkbird sensor parser
│
├── esphome/                   # ESPHome API
│   ├── esphome_api.c/h        # ESPHome Native API v1.9 server
│   ├── esphome_protocol.c/h   # Protobuf message encoding/decoding
│   ├── esphome_entities.c/h   # Sensor, Binary Sensor, Switch entities
│   ├── esphome_services.c/h   # Service call handling
│   ├── esphome_ble_proxy.c/h  # BLE Proxy Protocol
│   ├── esphome_ota.c/h        # OTA update support
│   ├── esphome_common.h       # API message types and protocol constants
│   └── esphome_entity_macros.h    # Entity definition macros
│
├── ota/                       # OTA updates
│   └── ota_handler.c/h
│
├── led/                       # LED status indicators
│   └── led_controller.c/h
│
└── utils/                     # Utilities
    ├── json_utils.c/h
    └── version.c/h
```

## Data Flow

### Device Join Flow

```
Zigbee Device                                               Home Assistant
     |                                                           |
     | 1. Association Request                                    |
     |-------------------------------------------------------->  |
     |                                                           |
     | 2. Device Announce                                        |
     |-------------------------------------------------------->  |
     |                                                           |
     v                                                           |
+------------------+                                             |
| zb_coordinator   |                                             |
| esp_zb_app_      |                                             |
| signal_handler() |                                             |
+--------+---------+                                             |
         |                                                       |
         v                                                       |
+------------------+                                             |
| zb_callback_     |                                             |
| device_announce()|                                             |
+--------+---------+                                             |
         |                                                       |
         +---> zb_device_add()                                   |
         |                                                       |
         +---> zb_availability_add_device()                      |
         |                                                       |
         +---> zb_interview_start()                              |
         |                                                       |
         v                                                       |
+------------------+                                             |
| mqtt_bridge_     |                                             |
| on_device_join() |                                             |
+--------+---------+                                             |
         |                                                       |
         +---> device_state_publish_availability(online)         |
         |                                                       |
         +---> device_state_publish()                            |
         |                                                       |
         +---> mqtt_bridge_publish_device_list()                 |
         |                                                       |
         +---> ha_discovery_publish_device()                     |
         |                                                       |
         v                                                       v
    MQTT Broker <-------------------------------------------> HA Discovery
```

### Command Flow (MQTT to Zigbee)

```
MQTT Broker                                          Zigbee Device
     |                                                    |
     | 1. zigbee2mqtt/<device>/set                        |
     |    {"state":"ON","brightness":200}                 |
     |                                                    |
     v                                                    |
+------------------+                                      |
| mqtt_client      |                                      |
| message_callback |                                      |
+--------+---------+                                      |
         |                                                |
         v                                                |
+------------------+                                      |
| mqtt_bridge      |                                      |
| handle_command() |                                      |
+--------+---------+                                      |
         |                                                |
         v                                                |
+------------------+                                      |
| command_handler  |                                      |
| process()        |                                      |
+--------+---------+                                      |
         |                                                |
         +---> Extract friendly name from topic           |
         +---> Find device in registry                    |
         +---> Parse JSON payload                         |
         |                                                |
         v                                                |
+------------------+                                      |
| esp_zb_lock_     |                                      |
| acquire()        |  <-- LOCK ACQUIRED                   |
+--------+---------+                                      |
         |                                                |
         v                                                |
+------------------+                                      |
| esp_zb_zcl_      |                                      |
| on_off_cmd_req() |                                      |
+--------+---------+                                      |
         |                                                |
         v                                                |
+------------------+                                      |
| esp_zb_lock_     |                                      |
| release()        |  <-- LOCK RELEASED                   |
+--------+---------+                                      |
         |                                                |
         |  2. ZCL On/Off Command (Zigbee TX)             |
         |----------------------------------------------->|
         |                                                |
         |  3. ZCL Default Response                       |
         |<-----------------------------------------------|
         |                                                |
```

### Attribute Report Flow (Zigbee to MQTT)

```
Zigbee Device                                          MQTT Broker
     |                                                      |
     | 1. ZCL Report Attributes                             |
     |    Cluster: OnOff (0x0006)                           |
     |    Attr: OnOff (0x0000) = 0x01                       |
     |                                                      |
     v                                                      |
+------------------+                                        |
| zb_coordinator   |                                        |
| action_handler() |                                        |
| ESP_ZB_CORE_     |                                        |
| REPORT_ATTR_CB_ID|                                        |
+--------+---------+                                        |
         |                                                  |
         v                                                  |
+------------------+                                        |
| zb_callback_     |                                        |
| report_attr()    |                                        |
+--------+---------+                                        |
         |                                                  |
         +---> zb_device_update_last_seen()                 |
         +---> zb_availability_update_last_seen()           |
         +---> zb_device_add_cluster()                      |
         +---> Handle cluster-specific logic                |
         |                                                  |
         v                                                  |
+------------------+                                        |
| mqtt_bridge_     |                                        |
| on_attribute_    |                                        |
| change()         |                                        |
+--------+---------+                                        |
         |                                                  |
         v                                                  |
+------------------+                                        |
| device_state_    |                                        |
| publish_by_addr()|                                        |
+--------+---------+                                        |
         |                                                  |
         +---> Build JSON: {"state":"ON"}                   |
         +---> Build topic: zigbee2mqtt/<device>            |
         |                                                  |
         v                                                  |
+------------------+                                        |
| mqtt_client_     |                                        |
| publish()        |                                        |
+--------+---------+                                        |
         |                                                  |
         | 2. MQTT Publish                                  |
         |------------------------------------------------->|
         |                                                  |
```

## Design Decisions

### 1. Single-Core Optimization

**Decision**: Optimize for ESP32-C5's single RISC-V core

**Rationale**:
- ESP32-C5 has one core (unlike ESP32 dual-core)
- Cooperative multitasking via FreeRTOS
- No SMP complexity
- Lower memory overhead

**Implications**:
- Careful task priority tuning essential
- Non-blocking operations critical
- Zigbee stack needs responsive scheduling
- Watchdog monitoring prevents lockups

### 2. Centralized Bridge Pattern

**Decision**: Single `mqtt_bridge` coordinates all protocol communication

**Benefits**:
- Single point of control
- Easier debugging
- Consistent message formatting
- Rate limiting and flow control

### 3. ESP-Zigbee-SDK Lock Usage

**Decision**: Always use `esp_zb_lock_acquire/release` for cross-task Zigbee API calls

**Rationale**:
- Zigbee stack is not thread-safe by default
- Commands from MQTT task must be synchronized
- Prevents race conditions with Zigbee task

### 4. Event-Driven Architecture

**Decision**: Use ESP-IDF event loop and callbacks extensively

**Benefits**:
- Non-blocking operation
- Efficient resource usage
- Natural fit for embedded networking
- Follows ESP-IDF best practices

### 5. Three-Tier Configuration

**Decision**: Kconfig (compile) -> NVS (persistent) -> MQTT (runtime)

**Benefits**:
- Sensible defaults at compile time
- Changes persist across reboots
- Remote configuration possible
- No serial access needed post-deployment

## Performance Characteristics

### Throughput

| Metric | Typical | Maximum |
|--------|---------|---------|
| MQTT messages/sec | 50-100 | 200+ |
| Zigbee commands/sec | 10-20 | 50 |
| State updates/sec | 20-50 | 100 |

### Latency

| Operation | Typical | Worst Case |
|-----------|---------|------------|
| MQTT to Zigbee command | 50-200ms | 500ms |
| Zigbee to MQTT state | 100-500ms | 1s |
| Device join complete | 5-30s | 60s |

### Scalability

| Resource | Zigbee Only | With Bluetooth |
|----------|-------------|----------------|
| Max Zigbee devices | 50 | 30 |
| Max BLE devices | N/A | 50 |
| Command queue depth | 10 | 10 |
| Free heap target | 50KB+ | 40KB+ |

## References

- **ESP-IDF**: https://docs.espressif.com/projects/esp-idf/
- **ESP-Zigbee-SDK**: https://github.com/espressif/esp-zigbee-sdk
- **Zigbee Specification**: https://zigbeealliance.org/
- **MQTT Protocol**: https://mqtt.org/mqtt-specification/
- **Home Assistant Discovery**: https://www.home-assistant.io/docs/mqtt/discovery/
- **NimBLE Guide**: https://mynewt.apache.org/latest/network/ble_hs/ble_hs.html

## Related Documents

- [API Reference](API_REFERENCE.md) - Detailed API documentation
- [Development Guide](DEVELOPMENT.md) - Development workflows
- [Memory Optimization](MEMORY_OPTIMIZATION.md) - Memory management details
- [MQTT Protocol](MQTT_PROTOCOL.md) - MQTT message formats
- [Coexistence](COEXISTENCE.md) - WiFi/Zigbee/BLE coexistence
- [ESPHome API](ESPHOME_API.md) - ESPHome Native API details
