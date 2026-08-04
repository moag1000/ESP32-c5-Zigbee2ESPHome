# CLAUDE.md - ESP32-C5 Zigbee HA Native (ESPHome Primary)

> Stand: 2026-07-31. BLE ist projektweit deaktiviert, Converter kommen zur
> Laufzeit aus einer LittleFS-DB. Beides steht unten im Detail -- aeltere
> Dokumente unter `docs/` sind teils noch auf dem Fork-Stand von 2026-02-19.

## Vision
**Hybrid ESPHome Native API + MQTT Gateway** auf ESP32-C5 single-core RISC-V.
ESPHome Native API ist die PRIMARY Home Assistant Integration (Port 6053, Noise encryption).
MQTT ist sekundaer: Bridge-Management, Debug-Logs, Fallback.
Memory-optimiert, saubere Architektur, keine Code-Duplikation.

## BLE: deaktiviert

`CONFIG_BT_ENABLED=n` in `sdkconfig.defaults`. Der C5 haelt unter
WiFi+Zigbee-Koexistenzlast keine stabilen GATT-Verbindungen; das Abschalten
gibt rund 30KB internes RAM fuer Zigbee und WiFi frei.

- Der komplette NimBLE-Block ist aus `sdkconfig.defaults` entfernt
- `BT_SCANNER_ENABLED` hat `depends on BT_ENABLED` -- ein lokaler Override in
  `sdkconfig.local` kann BLE-App-Code nicht mehr ohne Controller aktivieren
- `BT_SRCS` in `main/CMakeLists.txt` wird nur bei aktivem BT eingebunden;
  der BLE-Quellcode bleibt vollstaendig im Baum, wird aber nicht kompiliert
- `main/bluetooth/ble_stubs.c` liefert die No-Op-Symbole. Signaturen muessen
  exakt zu `esphome_ble_proxy.h` passen (3-Param-Handler
  `uint8_t client_id, const uint8_t *payload, size_t len`)
- `esphome_api_handlers.c` meldet `bluetooth_proxy_feature_flags = 0` und laesst
  Feld 18 (BT-MAC) weg
- Damit ist auch der ESPHome BLE Proxy inaktiv

Pruefen, ob es wirklich aus ist: keine `CONFIG_BT_*`-Defines in
`build/config/sdkconfig.h`, 0 Treffer fuer `bt/host/nimble` in
`build/compile_commands.json`.

## Nicht verdrahteter Code (gemessen 2026-08-01)

Zwoelf Uebersetzungseinheiten werden kompiliert, tragen aber **kein einziges
Symbol** zum fertigen Image bei -- der Linker verwirft sie per `--gc-sections`
vollstaendig. Zusammen rund **10.560 Zeilen**.

| Modul | LOC | Warum tot |
|-------|-----|-----------|
| `zigbee/zb_ota.c` | 1982 | nie initialisiert; die Event-Konsumenten in `mqtt_event_handler.c` existieren, der Produzent laeuft nie |
| `zigbee/zb_scenes.c` | 1545 | keine externen Referenzen |
| `zigbee/zb_router.c` | 1119 | Router-Modus; Geraet laeuft als Coordinator (erwartet) |
| `zigbee/zb_touchlink.c` | 1070 | nie initialisiert |
| `zigbee/cluster_state_ng.c` | 1000 | **kein einziger Aufruf von `cluster_state_*` im Projekt** |
| `core/coex_manager.c` | 787 | nie initialisiert (`COEX_MANAGER_CONCEPT.md` nennt es selbst Draft) |
| `core/monitoring/event_trace.c` | 752 | **verdrahtet 2026-08-02** — `CONFIG_EVENT_TRACE_ENABLE`, default n |
| `core/adapters/ble_adapter.c` | 749 | BLE ist aus (erwartet) |
| `mqtt/batch_publisher.c` | 555 | **verdrahtet 2026-08-02** — `CONFIG_BATCH_PUBLISHER_ENABLE`, default n |
| `core/memory_pool.c` | 462 | nie initialisiert |
| `core/monitoring/memory_dashboard.c` | 369 | **verdrahtet 2026-08-02** — `CONFIG_MEMORY_DASHBOARD_ENABLE`, default n |
| `core/monitoring/adaptive_memory.c` | 172 | **verdrahtet 2026-08-02** — `CONFIG_ADAPTIVE_MEMORY_ENABLE`, default n |

Zwei davon sind konfigurationsbedingt korrekt tot (`ble_adapter`, `zb_router`).
Vier sind seit 2026-08-02 verdrahtet (`event_trace`, `memory_dashboard`,
`batch_publisher`, `adaptive_memory` — alle opt-in per Kconfig, default aus).
Bleiben rund **6.900 Zeilen** fertig gebauter Features, die nie angeschlossen
wurden: `zb_ota` (3 TODOs, halbfertig), `zb_scenes`, `zb_touchlink`,
`coex_manager` (laut eigener Doku Draft), `memory_pool` (generische API, kein
Singleton) und `cluster_state_ng` (funktional durch `device_registry` ersetzt —
eher ein Loesch- als ein Anschlusskandidat).

Verdrahten heisst hier: Kconfig-Flag (default n), `_init()` in
`foundation_init.c` unter `INIT_OPTIONAL_COMPONENT`, und die Quelle in
`main/CMakeLists.txt` an dasselbe Flag haengen, damit sie bei ausgeschaltetem
Feature gar nicht erst kompiliert wird.

**Wichtig fuer die Fehlersuche:** Bugs in diesen Dateien koennen kein
Laufzeitverhalten erklaeren. `cluster_state_ng.c` ist der auffaelligste Fall --
`CLAUDE.md` beschrieb es lange als zentralen State-Mechanismus, obwohl es nie
aufgerufen wird. Der State-Weg, der tatsaechlich laeuft, geht ueber
`device_registry_set_state()` / `_merge_state()` und den Event-Bus.

### Den Befund reproduzieren

```bash
grep -oE "libmain\.a\([a-z_0-9]+\.c\.obj\)" build/esp32_c5_zigbee2mqtt.map \
  | sed 's/.*(\(.*\))/\1/' | sort -u > /tmp/kept.txt
# gegen die aktuell kompilierten Quellen aus build/compile_commands.json vergleichen
```

Die Map ist die verlaessliche Quelle. Textsuche nach `<modul>_init` taeuscht in
beide Richtungen: Referenzen aus ebenfalls totem Code zaehlen mit, und
Substring-Treffer wie `evt_zb_ota_progress_t` sehen aus wie Aufrufe.

## Device-Registry: haelt nur noch Geraete (entkoppelt 2026-08-04)

Die Registry zaehlt jetzt Geraete:

    Registry = Zigbee-Geraete + BLE-Geraete

ESPHome-Entities liegen in `main/esphome/esphome_entity_mirror.c` mit eigener
Kapazitaet (`CONFIG_ESPHOME_ENTITY_MIRROR_MAX`, Default 128).

**Warum das noetig war.** Vorher legte `esphome_device_registry.c` fuer *jede*
Entity ein virtuelles Geraet an. Live gemessen (2026-08-04), ueber einen
HA-Verbindungsaufbau hinweg:

    t= 3s   registry= 2/64  ( 3%)   esphome_clients=0   zigbee=2
    t=56s   registry=56/64  (87%)   esphome_clients=1   zigbee=2

54 virtuelle Eintraege fuer zwei gepairte Geraete. `CONFIG_MAX_ZIGBEE_DEVICES`
steht auf 50 — mit dieser Auslegung unerreichbar.

**Der Befund, der die Sache entschied:** unter `ESPHOME_PRIMARY_INTEGRATION`
(= Default) hatte der Spiegel *gar keinen Konsumenten*. `handle_device_state_changed()`
im `mqtt_adapter` steigt bei genau diesem Define in Zeile 1 aus, und
`republish_all_device_states()` ueberspringt `DEV_PROTOCOL_VIRTUAL`. Die 54
Slots und 54 Events pro Zustandsaenderung erzeugten also **kein einziges
MQTT-Topic**. Einziger Empfaenger war `esphome_adapter`s eigener Handler, der
virtuelle Geraete nicht filtert — kein Kreislauf (`diagnostics_update()` bricht
bei non-ZIGBEE ab, `update_entity_states()` findet die Keys nicht), aber
vollstaendig verschwendete Arbeit.

Daher: `CONFIG_ESPHOME_ENTITY_MIRROR` ist `default n if ESPHOME_PRIMARY_INTEGRATION`.
Wer ihn dort trotzdem einschaltet, bekommt die Topics jetzt wirklich — vorher
kostete Einschalten Slots und lieferte nichts.

**Neuer Datenweg.** Entities haben kein `device_t`:

    Entity-Zustand -> esphome_entity_mirror_sync_state()
      -> EVT_ESPHOME_ENTITY_STATE (traegt nur den Key)
      -> mqtt_adapter -> esphome_entity_mirror_get() -> zigbee2mqtt/<name>

`_get()` liefert eine unter der Mirror-Sperre gezogene **Kopie**. Den
cJSON-Zeiger ins Event zu legen waere ein Use-after-free, sobald der naechste
`sync_state()` ihn ersetzt — derselbe Fehler, der schon bei `json_state` steckte.

Die Tabelle ist offen adressiert mit linearem Probing und schliesst Luecken per
**Backward-Shift** statt Tombstones. Das ist der eine Teil, der stillschweigend
kaputtgehen kann: ein falsch geschlossener Probe-Lauf macht kollidierte
Eintraege unerreichbar, ohne zu crashen. `test_deletion_keeps_collided_entries_reachable`
in `tests/unit/test_esphome_entity_mirror.c` deckt das ab (Suite: 20 Tests).

**Nachweis, dass die Kopplung weg ist:** kein `device_registry_add()` im Baum
nimmt noch `DEV_PROTOCOL_VIRTUAL`, und im Default-Build wird
`esphome_entity_mirror.c` gar nicht erst kompiliert (0 Treffer in
`build/compile_commands.json`). Der Fuellstand ist damit strukturell begrenzt,
nicht nur beobachtet.

`bridge/state` fuehrt `registry_used` und `registry_max` mit, damit der
Fuellstand ohne Serial-Log sichtbar ist. `device_registry_add()` warnt ab 80 %
statt erst zu scheitern, wenn ein echtes Geraet abgewiesen wird.

Der Zaehler in `bridge/state` und `bridge/info` meldet ausschliesslich
`stats.zigbee_count` — vorher stand dort die Gesamtzahl, weshalb `bridge/state`
56 gepairte Geraete behauptete waehrend `bridge/devices` zwei auflistete.

## Konfigurations-Kette

`CMakeLists.txt` setzt `SDKCONFIG_DEFAULTS` auf **zwei** Dateien:
`sdkconfig.defaults`, danach `sdkconfig.local`. Die lokale Datei ueberschreibt
die Defaults und ist gitignored (enthaelt WLAN-/MQTT-Zugangsdaten im Klartext --
nicht committen).

`sdkconfig.defaults` zu aendern reicht nicht: das erzeugte `sdkconfig` entsteht
nur bei fehlendem `sdkconfig` oder `idf.py set-target` neu. Vorgehen: `sdkconfig`
sichern und loeschen, `build/` weg, neu bauen -- danach immer
`build/config/sdkconfig.h` gegenpruefen. `idf.py reconfigure` regeneriert sie
nicht zuverlaessig.

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
| Cluster State NG | -- | **nicht verdrahtet** -- kein Aufrufer, wird wegoptimiert |
| Device Persistence | ~1KB | done |
| OTA Updates (MQTT/HTTP/ESPHome) | ~5KB | done |
| Zigbee-OTA (`zb_ota.c`) | -- | **nicht verdrahtet** -- nie initialisiert |
| Zigbee Groups | ~2KB | done |
| Zigbee Scenes | -- | **nicht verdrahtet** -- keine externen Referenzen |
| Zigbee Direct Binding | ~2KB | done |
| Zigbee Touchlink | -- | **nicht verdrahtet** -- nie initialisiert |
| Network Topology/Heal | ~3KB | done |
| Zigbee Availability Tracker | ~2KB | done |
| WiFi/Zigbee Coexistence | ~1KB | done |
| System Monitor + Crash Reporter | ~2KB | done |
| LED Status Manager | ~1KB | done |
| mmWave Presence Sensor (S3KM1110) | ~1KB | done |
| ESPHome Service Calls | ~1KB | done -- `permit_join`, `remove_device`, `reconfigure_device` |
| BLE Scanner | -- | **deaktiviert** (Code im Baum, nicht kompiliert) |
| ESPHome BLE Proxy | -- | **deaktiviert** |

**Hardware:** 384KB SRAM, 8MB PSRAM. Der Wert "~40KB internal free after full
init" stammt aus der Zeit mit aktivem BLE; ohne BLE sollten rund 30KB mehr frei
sein, das ist aber seit der Abschaltung **nicht auf Hardware nachgemessen**.

## ESP-IDF

| Version | Pfad | Beschreibung |
|---------|------|--------------|
| **v6.0.2** | `~/esp/esp-idf-v6` | Tag-Checkout (detached HEAD), Picolibc, MbedTLS v4.x |

### Setup
```bash
source ~/esp/esp-idf-v6/export.sh
```

Der venv liegt unter `~/.espressif/python_env/idf6.0_py3.14_env` und traegt die
**Host-Python-Version im Namen** -- nach einem Python-Upgrade ist der alte venv
unbrauchbar. Symptom: `export.sh` laeuft mit rc=0 durch, danach
`command not found: idf.py`, im Log
`ERROR: ESP-IDF Python virtual environment ... not found`.
Reparatur: `~/esp/esp-idf-v6/install.sh esp32c5` (zieht ~3.4GB).

### Managed Components (Stand 2026-07-31)
`esp-zigbee-lib` 1.6.8, `esp-zboss-lib` 1.6.4, `led_strip` 3.0.3,
`mdns` 1.11.3, `mqtt` 1.1.0, `littlefs` 1.22.3, `cjson` 1.7.19~2.
Aktualisieren mit `idf.py update-dependencies`.

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

### Converter: Laufzeit-DB statt einkompilierter Definitionen

Die frueheren 27 einkompilierten Converter-Definitionen gibt es nicht mehr. In C
liegen nur noch drei: `conv_generic.c`, `conv_tuya_bridge.c`,
`conv_tuya_fingerbot.c`. Alles andere kommt zur Laufzeit aus einer JSON-DB in
LittleFS.

- Mountpoint `/littlefs`, DB unter `/littlefs/converters`, Einstieg `index.json`
  (Format v2 mit `manufacturers`- und `files`-Objekten)
- Loader: `main/zigbee/converter/zb_converter_loader.c`
- Partition: `spiffs` ab 0x921000, 0x6DF000 gross (siehe `partitions.csv`)
- Quelldaten im Repo unter `data/`: 447 Hersteller-JSONs, `converters_merged/`
  (162), `converters_zhaquirks/` (554), gepacktes `converters.bin` (6.9MB)
- Host-Tools in `tools/`: `z2m_converter_extract.py`, `zhaquirks_transpiler.py`,
  `merge_converter_dbs.py`, `validate_converter_db.py`, `upload_converters.py`.
  `tools/zhc/` und `tools/zhaquirks/` sind die eingecheckten Upstream-Quellen.
- DB-Update zur Laufzeit per MQTT:
  `zigbee2mqtt/bridge/request/converter_db/update` ->
  `handle_converter_db_update()` in `bridge_request_handler.c`

**Zwei Fallstricke:**

1. Der Rebind beim Boot passiert in `main.c` (~Zeile 509), nicht in
   `device_persistence.c`. `device_persistence_load_all()` laeuft, bevor die
   Converter registriert sind -- NG-Devices haben danach `converter == NULL`.
   Fallback-Kette: exakter Hersteller+Modell-Treffer ->
   `conv_generic_for_capabilities()` -> gar kein Converter (Entities dann nur
   aus Capabilities).
2. `zb_converter_find()` macht LittleFS-File-I/O und darf in `zb_interview.c`
   **nicht** unter gehaltenem `s_mutex` laufen -- haelt die Sperre zu lange und
   zerlegt die FreeRTOS-Priority-Inheritance auf dem single-core C5 (siehe
   Kommentar bei `zb_interview.c:1746`).

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
`EVT_DEVICE_INTERVIEWED`, `EVT_MQTT_CONNECTED`, `EVT_MQTT_DISCONNECTED`,
`EVT_ESPHOME_ENTITY_STATE` (Entities, die kein `device_t` haben)

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

### Cluster State NG -- NICHT VERDRAHTET

> Stand 2026-08-01: `cluster_state_ng.c` wird von **keiner** Stelle im Projekt
> aufgerufen und komplett wegoptimiert. Die folgende Beschreibung dokumentiert,
> was das Modul kann, nicht was laeuft. Der tatsaechliche State-Weg geht ueber
> `device_registry_set_state()` / `_merge_state()` und den Event-Bus.
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
| ESPHome Services | `main/esphome/esphome_services.c` (Framework), `esphome_gateway_services.c` (die 3 Gateway-Services) |
| ESPHome OTA | `main/esphome/esphome_ota.c` (Port 3232, gestartet aus `esphome_adapter_gateway.c`) |
| ESPHome Entity Mirror | `main/esphome/esphome_entity_mirror.c` (eigene Ablage, **nicht** die Device-Registry) |
| ESPHome BLE | `main/esphome/esphome_ble_proxy.c` (**inaktiv**, BT aus) |
| ESPHome Crypto | `esphome_noise.c`, `esphome_crypto_constants.h` |
| mmWave | `main/mmwave/mmwave_sensor.c` (S3KM1110, UART, 16 Gates) |
| MQTT (Secondary) | `main/mqtt/gateway_mqtt.c`, `main/core/bridge/mqtt_bridge.c` |
| Events | `main/core/events/event_bus.c` |
| Device (NG) | `main/core/device/device_registry.c`, `unified_device.c`, `device_persistence.c` |
| Command Handler | `main/zigbee/zb_command_handler.c` |
| Memory | `main/core/memory/memory_manager_ng.c`, `adaptive_memory.c` |
| Discovery | `main/core/discovery/ha_discovery_ng.c` (disabled when ESPHome primary) |
| Converter (C) | `main/zigbee/converter/converters/` -- nur noch `conv_generic.c`, `conv_tuya_bridge.c`, `conv_tuya_fingerbot.c` |
| Converter (Laufzeit-DB) | `zb_converter_loader.c`, `zb_converter_registry.c`, `zb_converter_fn_registry.c` |
| Quirks | `zb_quirk_engine.c` (Transform-Pipeline + Init-Sequenzen), `zb_custom_quirk.c` |
| Adapters | `main/core/adapters/esphome_adapter.c`, `mqtt_adapter.c`, `zigbee_adapter.c`, `ble_adapter.c` |
| BLE | `main/bluetooth/` (**nicht kompiliert**, `BT_SRCS` haengt an `CONFIG_BT_ENABLED`); aktiv ist nur `ble_stubs.c` |
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
- [~] Cluster State NG (cJSON-based) -- fertig, aber nirgends aufgerufen
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
- [x] Groups, Direct Binding
- [~] Scenes -- `zb_scenes.c` fertig, aber keine externen Referenzen
- [~] Touchlink (Hue device takeover) -- fertig, aber nie initialisiert
- [x] Network Topology + Heal
- [~] Zigbee OTA -- siehe oben, nie initialisiert
- [x] Tuya driver framework + Fingerbot

### Connectivity
- [x] OTA Updates (MQTT/HTTP/ESPHome)
- [~] Zigbee-OTA -- `zb_ota.c` fertig, aber nie initialisiert
- [x] WiFi Manager (5GHz auto, captive portal)
- [x] WiFi/Zigbee Coexistence
- [~] BLE Scanner (BLE 5.0 extended, Xiaomi, passive) -- Code fertig, aber
      **abgeschaltet**: keine stabilen GATT-Verbindungen auf dem C5
- [~] ESPHome BLE Proxy -- dito, inaktiv

### Converter & Quirks
- [x] Laufzeit-Converter-DB in LittleFS (index.json v2)
- [x] z2m- und zhaquirks-Transpiler (Host-Tools unter `tools/`)
- [x] Quirk-Engine (Transform-Pipeline, Init-Sequenzen)
- [x] Custom-Quirk-Upload + DB-Update per MQTT

### Sensorik
- [x] mmWave-Praesenzsensor S3KM1110 (UART, 16 Gates)

### Monitoring
- [x] System Monitor (heap, CPU, tasks)
- [x] Crash Reporter (boot reason, NVS persistence)
- [x] LED Status Manager (RGB patterns)
- [x] Performance Metrics + Latency Measurement

### Remaining
- [ ] CI/CD Pipeline (Build + Lint + Format)
- [ ] Web Dashboard (HTTP status page)
- [x] ESPHome-Services: `permit_join`, `remove_device`, `reconfigure_device`
- [ ] ESPHome OTA fuer Zigbee-Sub-Devices (`esphome_ota.c` deckt bisher nur das
      Gateway selbst ab, Port 3232)
- [ ] Bootloader-Groesse: 0x5700 von 0x6000 belegt, nur ~2.3KB frei
- [ ] Freien internen RAM ohne BLE auf Hardware nachmessen
- [ ] `docs/` auf den aktuellen Stand ziehen -- grosse Teile sind noch der
      Fork-Stand vom 2026-02-19 und beschreiben BLE als aktiv
