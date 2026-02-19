# Aktionsplan: Von "NG" zu echtem Next Generation

**Status:** Re-Audit vom 2026-02-19 mit gleicher Kritikfähigkeit wie Original-Review.
AP-1 bis AP-6 großteils erledigt, aber ehrliche Nachprüfung deckt neue systemische Defizite auf.
AP-7 definiert die verbleibenden Fixes für echte NG-Legitimität.

---

## Übersicht: 7 Arbeitspakete

| # | Arbeitspaket | Aufwand | Prio | Impact | Status |
|---|-------------|---------|------|--------|--------|
| 1 | Thread Safety Fixes | ~330 LOC | KRITISCH | Crashes & Deadlocks verhindern | ✅ DONE |
| 2 | Legacy-Elimination | ~2.000 LOC entfernen | HOCH | 30% weniger Code, eine Wahrheit | ✅ DONE |
| 3 | ESP-IDF v6 / C23 ernst nehmen | ~200 LOC | MITTEL | Claim einlösen oder entfernen | ⚠️ 3.2 nicht möglich |
| 4 | Architektur-Härtung | ~400 LOC | MITTEL | Echte Schichten statt Fassade | ✅ DONE (4.4/4.5 ongoing) |
| 5 | CI/CD + Quality Gates | ~15h Setup | MITTEL | Automatisierte Qualitätssicherung | ❌ Nicht gestartet |
| 6 | CLAUDE.md Korrektur | ~30 min | SOFORT | Ehrliche Dokumentation | ✅ DONE |
| **7** | **NG-Ehrlichkeits-Audit** | **~2.500 LOC** | **HOCH** | **Claims einlösen oder korrigieren** | **❌ NEU** |

---

## AP-1: Thread Safety Fixes (KRITISCH)

### 1.1 MQTT Latency Race Condition
**Datei:** `main/mqtt/gateway_mqtt.c`
**Problem:** `s_mqtt.latency_measuring`, `ping_sent_time_us`, `ping_msg_id`, `latency_ms`
werden ohne Mutex gelesen/geschrieben. `s_mqtt.state_mutex` existiert aber wird nicht genutzt.

**Fix:** Mutex um alle 4 Zugriffsstellen:
- Zeile 168-175: `MQTT_EVENT_PUBLISHED` Handler (liest alle 4 Felder)
- Zeile 807-843: `mqtt_latency_measure_start()` (schreibt alle 4 Felder)
- Zeile 851-854: `mqtt_latency_get_ms()` (liest `latency_ms`)
- Zeile 862-869: Timeout-Check (liest `latency_measuring` + `ping_sent_time_us`)

**Aufwand:** ~30 LOC (Mutex acquire/release um bestehende Logik)
**Risiko:** Niedrig

---

### 1.2 BLE GATT Connection Lookup ohne Mutex
**Datei:** `main/bluetooth/ble_gatt_client.c`
**Problem:** `find_connection_by_handle()` und `find_connection_by_mac()` (Zeile 1495-1513)
lesen `s_connections[]` ohne Mutex. 18 Caller, davon einige ohne gehaltenen Mutex.
Rekursiver Mutex `s_gatt_mutex` existiert aber wird inkonsistent genutzt.

**Race Scenario:** Thread A gibt Connection frei, Thread B dereferenziert stale Pointer → UAF.

**Fix-Optionen:**
- **Option A (minimal):** `find_connection_by_*()` acquiren Mutex intern (~20 LOC)
- **Option B (sauber):** Wrapper die Connection-Copy zurückgeben statt Pointer (~50 LOC)
- **Empfehlung:** Option A als Quick-Fix, Option B im Rahmen von AP-4

**Aufwand:** ~50 LOC
**Risiko:** Mittel (18 Caller müssen verifiziert werden)

---

### 1.3 Device Registry Iterator hält Mutex während Callbacks
**Datei:** `main/core/device/device_registry.c` Zeile 791-834
**Problem:** `device_registry_iterate()` ruft Callbacks mit gehaltenem Mutex auf.
12 Callback-Funktionen in 8 Dateien — einige davon blockieren auf MQTT oder NVS.

**Deadlock-Szenario:**
```
iterate() hält registry_mutex
  → republish_device_state_iterator() blockiert auf MQTT
    → Zigbee-Task will device_registry_add() → wartet auf registry_mutex → DEADLOCK
```

**Betroffene Callbacks (blockierend):**
- `republish_device_state_iterator` (mqtt_adapter.c:262) — MQTT publish
- `save_device_state_iterator` (state_persistence.c:360) — NVS I/O
- `save_all_iterator` (device_persistence.c:651) — NVS I/O
- `publish_availability_iterator` (device_state_publisher.c) — MQTT publish

**Fix:** Copy-then-iterate Pattern:
```c
// Vorher: Callback unter Mutex
device_registry_iterate(callback, ctx);

// Nachher: IDs sammeln, Mutex freigeben, dann verarbeiten
device_id_t ids[MAX]; size_t count;
device_registry_collect_ids(ids, MAX, &count);  // Neue Funktion
for (size_t i = 0; i < count; i++) {
    device_t *dev = device_registry_get(ids[i]);
    if (dev) callback(dev, ctx);
}
```

**Aufwand:** ~150 LOC (neue collect-Funktion + 8 Caller umstellen)
**Risiko:** Mittel (semantische Änderung: Device kann zwischen Collect und Callback verschwinden)

---

### 1.4 Event Ownership Dokumentation
**Datei:** `main/core/events/event_data.h`
**Problem:** Zeile 8 sagt "caller owns data", aber `EVT_HA_DISCOVERY_PUBLISH` transferiert
Ownership an Handler. Drei verschiedene Patterns ohne klare Kennzeichnung.

**Fix:** Ownership-Annotation pro Event-Struct:
```c
/** @ownership TRANSFER - handler frees topic and payload_json */
typedef struct { ... } evt_ha_discovery_publish_t;

/** @ownership BORROW - caller owns all data, handler must not free */
typedef struct { ... } evt_device_state_t;
```

**Aufwand:** ~30 LOC (Kommentare + ggf. Macro-Marker)
**Risiko:** Keins (reine Dokumentation, Code ist bereits korrekt)

---

## AP-2: Legacy-Elimination (HOCH) — ✅ DONE

### 2.1 Dead Code entfernen (~1.800 Zeilen) — ✅ DONE

Gesamte Datei `zb_device_handler.c` wurde eliminiert (siehe AP-2.5).

---

### 2.2 Aktive Legacy-Funktionen migrieren — ✅ DONE

| Caller-Datei | Legacy-Funktion | NG-Ersatz | Status |
|-------------|----------------|-----------|--------|
| zb_coordinator.c | `zb_device_load_all_from_nvs()` | `device_persistence_load_all()` (foundation_init) | ✅ Entfernt |
| zb_coordinator.c | `zb_device_test()` | Entfernt aus Self-Test | ✅ Entfernt |
| zb_cluster_internal.h | `zb_device_has_cluster()` | Inline NG (`device_zigbee_has_cluster`) | ✅ Done |
| zb_coordinator.c | `zb_device_handler_init()` | Direkte `zb_cluster_*_init()` Aufrufe | ✅ Done |
| zb_cluster_closures.c | `zb_device_handler_is_initialized()` | Lokaler `s_closures_initialized` Flag | ✅ Done |
| zb_callbacks.c (2x) | `zb_device_update_short_addr()` | NG Registry direkt | ✅ War schon |
| zb_callbacks.c | `zb_device_update_state()` | `cluster_state_update_*()` | ✅ War schon |

Alle Legacy-Funktionen sind eliminiert. Keine verbleibenden externen Aufrufe.

---

### 2.3 NVS-Namespace-Konsolidierung — ✅ DONE (via auto-erase)

**Lösung:** `device_persistence.c:114` löscht den Legacy-Namespace `zb_devices` automatisch
bei jedem Boot. NG-Namespace `devices` ist die einzige aktive Persistenz.
`bridge_request_handler.c:645` referenziert `zb_devices` nur noch für Factory-Reset.

Keine explizite Migration nötig — Legacy-Daten werden nicht mehr geladen.

---

### 2.4 device_sync.c bereinigen — ⚠️ Optional (Low Priority)

**Status:** 4 Legacy-Sync-Stubs haben **0 externe Caller** (nur Implementierung in `device_sync.c`):
- `device_sync_from_legacy()` — No-Op, 0 Caller
- `device_sync_to_legacy()` — No-Op, 0 Caller
- `device_sync_all_from_legacy()` — No-Op, 0 Caller
- `device_sync_all_to_legacy()` — No-Op, 0 Caller

Die aktiven Wrapper-Funktionen (`device_sync_get`, `_add`, `_remove`, etc.) haben 75 Caller
und leiten direkt an `device_registry` weiter. Header-Doku ist noch auf "legacy sync" formuliert.

**Optionale Aufräumung:** Stubs + Header-Doku entfernen. Niedrige Priorität — tut nichts.

---

### 2.5 zb_device_handler.c eliminieren (Endziel) — ✅ DONE

**Ergebnis:**
- `zb_device_handler.c` (~1.750 LOC) **gelöscht**
- `zb_device_handler_internal.h` (260 LOC) **gelöscht**
- `zb_device_handler.h` (1.403→24 LOC) → thin facade (types re-export only)
- Multistate-Handler → `zb_cluster_multistate.c` (335 LOC extrahiert)
- Init-Code → direkte `zb_cluster_*_init()` Aufrufe in `zb_coordinator.c`
- `ZCL_BASIC_CMD_INIT` + `zb_device_has_cluster()` → `zb_cluster_internal.h`
- Alle Cluster-APIs → eigene `zb_cluster_*.h` Header

**Gesamtergebnis AP-2:** ~5.986 Zeilen Legacy-Code → 0 LOC (vollständig eliminiert)

---

## AP-3: ESP-IDF v6 / C23 ernst nehmen (MITTEL)

### 3.1 C23 aktivieren oder Claim entfernen

**Option A: C23 tatsächlich aktivieren**
```cmake
# In CMakeLists.txt nach project():
idf_build_set_property(COMPILE_OPTIONS "-std=gnu23" APPEND)
```
Dann schrittweise C23-Features einführen:
- `_Static_assert` → `static_assert` (1-2 Dateien)
- `#include <stdbool.h>` entfernen (110 Dateien — optional, niedrige Prio)

**Aufwand:** 1h (CMake + Build-Test)
**Risiko:** Niedrig (GCC 14 in ESP-IDF v6 unterstützt gnu23)

**Option B: Claim aus CLAUDE.md entfernen**
Ehrlicher: "C11/C17 mit GCC-Extensions"

**Empfehlung:** Option A — es kostet fast nichts und legitimiert den Claim.

---

### 3.2 Picolibc Newlib-Compatibility deaktivieren — ❌ NICHT MÖGLICH

**Aktuell:** `CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY=y` (Kompatibilitätsmodus)

**Ergebnis der Analyse:** Zigbee-Stack (`libzboss_stack.zczr.a`) benötigt `__getreent` Symbol
aus dem Newlib-Compatibility-Shim. Deaktivierung → sofortiger Linker-Fehler (undefined reference).

**Kosten des Shims:** ~500 Bytes Code + ~256 Bytes TLS — vernachlässigbar.

**Entscheidung:** Kompatibilitätsmodus bleibt dauerhaft aktiv. Kein Handlungsbedarf.

---

### 3.3 PSA Crypto: Bewusste Entscheidung dokumentieren

**Status:** 3 Dateien nutzen PSA bedingt, Rest bleibt mbedTLS.
`CONFIG_MBEDTLS_USE_PSA_CRYPTO=y` ist auskommentiert.

**Empfehlung:** Nicht migrieren (ROI zu niedrig), aber ehrlich dokumentieren:
> "PSA Crypto selektiv für Noise-Protocol und OTA. Systemweit mbedTLS für WiFi/TLS/BLE."

**Aufwand:** 15 min (CLAUDE.md Update)

---

### 3.4 Deprecated APIs bereinigen

**Bereits erledigt (kein Handlungsbedarf):**
- `xTaskGetAffinity()` → `xTaskGetCoreID()` mit v6.0-Guard ✅
- `ulTaskNotifyTake()` → `xTaskNotifyWait()` ✅

**Kein Handlungsbedarf:**
- NVS-API: Legacy API funktioniert, device_persistence abstrahiert bereits
- GPIO-API: Nicht direkt genutzt (LED-Abstraction)

---

## AP-4: Architektur-Härtung (MITTEL)

### 4.1 Layer-Violations eliminieren

**Gefunden:**

| Violation | Datei | Fix | Status |
|-----------|-------|-----|--------|
| Core → MQTT direkt | `mqtt_bridge.c:348,373,398` | `EVT_BRIDGE_PUBLISH` Event (TRANSFER ownership) | ✅ done |
| Core → MQTT direkt | `mqtt_logger.c:854` | Bereits via `EVT_HA_DISCOVERY_PUBLISH` migriert | ✅ war schon fix |
| Core → Monitoring | `lifecycle_manager.c:490` | `set_mqtt_publishing(false)` in `system_monitor_stop()` kapseln | ✅ done |

**Ergebnis:** Neuer Event-Typ `EVT_BRIDGE_PUBLISH` mit `evt_bridge_publish_t` Struct (TRANSFER ownership).
Handler in `mqtt_event_handler.c:publish_bridge_data()`. Kein direkter `mqtt_client_publish()`
Aufruf mehr in Core-Layer (außer `mqtt_event_handler.c` selbst, der zur Protocol-Layer gehört).

---

### 4.2 Event Bus härten ✅

**Vorher:** Max 8 Subscriber/Event, silent fail bei #9.

**Umgesetzt:**
- ✅ `MAX_SUBSCRIBERS` von 8 auf 16 erhöht
- ✅ Warning-Log bei >75% Auslastung (12/16)
- ✅ Actionable `ESP_LOGW` bei Overflow (zeigt was zu tun ist)
- ⏭️ Backpressure: Nicht umgesetzt (kein konkreter Bedarf aktuell)

---

### 4.3 Adapter-Interface ✅ (Bonus — nicht im Original-Plan)

**Umgesetzt:** Vtable-basiertes Adapter-Interface mit statischer Registry.

**Neue Dateien:**
- `adapter_interface.h` — `adapter_ops_t` vtable (init/start/stop/deinit/is_running)
- `adapter_registry.c` — Statische Registry, Lifecycle in Registrierungsreihenfolge

**Alle 4 Adapter exportieren `adapter_ops_t`:**
- `zigbee_adapter_ops` (full: init/start/stop/deinit/is_running)
- `mqtt_adapter_ops` (init/deinit only)
- `ble_adapter_ops` (full: init/start/stop/deinit/is_running)
- `esphome_adapter_ops` (init/deinit only)

**foundation_init.c** nutzt jetzt `adapter_registry_*()` statt manueller Makros.

---

### 4.4 Device Registry: Echte Abstraktion (DEFERRED)

**Aktuelles Problem:** Protocol-Union in `device_t` erfordert Struct-Änderung für neue Protokolle.

**Entscheidung:** Erst umbauen wenn Thread/Matter Support kommt. Aktuell reicht die Union.
Siehe Original-Analyse für Opaque-Protocol-Data-Ansatz.

**Aufwand:** ~200 LOC (wenn nötig)
**Risiko:** Hoch (tiefgreifende Struct-Änderung)

---

### 4.5 Error Handling vereinheitlichen (ONGOING)

**Aktuell:** Mix aus `if (ret != ESP_OK)`, `if (!ret)`, ignorierte Return-Werte.

**Standard definieren:**
```c
// Pattern 1: Early return
ESP_RETURN_ON_ERROR(func(), TAG, "message");

// Pattern 2: Goto cleanup
ret = func();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "func failed: %s", esp_err_to_name(ret));
    goto cleanup;
}
```

**Aufwand:** Ongoing, kein dedizierter Sprint. Bei jedem Touch einer Datei angleichen.

---

## AP-5: CI/CD + Quality Gates (MITTEL)

### 5.1 GitHub Actions Grundgerüst (Phase 1)

**Datei:** `.github/workflows/build.yml`

```
Trigger: push main/develop, PR auf main
Jobs:
  1. format-check (clang-format --dry-run --Werror)
  2. build (espressif/idf:v6.0 Container, idf.py build)
  3. static-analysis (clang-tidy, parallel zu build)
```

**Aufwand:** 6-8h
**Deliverables:** CI Badge, PR-Blocking auf Build-Fehler

---

### 5.2 Static Analysis Tooling

**Neu erstellen:** `.clang-tidy`
```yaml
Checks: >
  bugprone-*,
  readability-*,
  performance-*,
  -readability-magic-numbers,
  -bugprone-easily-swappable-parameters
```

**Aufwand:** 4-6h (Config + Tuning für Embedded-Warnungen)

---

### 5.3 Test-Integration

**Aktueller Stand:** 107 Tests existieren, nur auf Hardware ausführbar.

**Kurzfristig:** Tests in CI als Build-Only validieren (kompiliert = grün)
**Mittelfristig:** Host-basierte Unit-Tests für pure Logik (JSON, Config, Memory Manager)
**Langfristig:** QEMU-basierte Integration-Tests

**Aufwand:** 8-12h (Host-Test-Framework aufsetzen)

---

### 5.4 Compiler Warnings verschärfen

**In CMakeLists.txt:**
```cmake
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wshadow -Wdouble-promotion
    -Wformat=2 -Wformat-truncation
    -fno-common
)
```

**Aufwand:** 2-4h (Warnings fixen die auftauchen)

---

## AP-6: CLAUDE.md Korrektur (SOFORT)

### Falsche Claims korrigieren:

| Aktueller Claim | Korrektur |
|-----------------|-----------|
| "C23 Standard (gnu23)" | "C11/C17 mit GCC Extensions" (oder C23 aktivieren per AP-3.1) |
| "PSA Crypto API" | "PSA Crypto selektiv (Noise, OTA). System: mbedTLS" |
| "Picolibc (kleinerer Footprint)" | "Picolibc im Newlib-Kompatibilitätsmodus" |
| "4 internal-only zb_device_get()" | "~15 externe Legacy-Calls in 6 Dateien, Migration 60%" |
| "Device Sync Layer hält beide synchron" | "device_sync.c ist NG-Wrapper, Legacy-Stubs sind No-Ops" |
| NG Architecture "100%" überall | Ehrliche Prozentangaben |

**Aufwand:** 30 min
**Risiko:** Null

---

## AP-7: NG-Ehrlichkeits-Audit (HOCH) — NEU

Re-Audit vom 2026-02-19 mit 4 parallelen Deep-Dives hat systemische Defizite aufgedeckt
die in AP-1 bis AP-6 nicht adressiert waren. Dieses Arbeitspaket macht die NG-Claims ehrlich.

---

### 7.1 Layer-Violations: command_handler + bridge_request_handler (KRITISCH)

**Problem:** Zwei `core/`-Dateien sind de facto Zigbee-Module:

| Datei | Zigbee-Imports | Direkte `esp_zb_lock_acquire()` | LOC |
|-------|----------------|--------------------------------|-----|
| `command_handler.c` | 12 Headers | 5 Stellen | ~550 |
| `bridge_request_handler.c` | 10 Headers | 2 Stellen | ~2500 |

`command_handler.c` ruft direkt `esp_zb_zcl_on_off_cmd_req()`, `esp_zb_zcl_level_move_to_level_cmd_req()`,
`esp_zb_zcl_color_move_to_color_cmd_req()` etc. auf. `bridge_request_handler.c` ruft direkt
`esp_zb_zcl_read_attr_cmd_req()` und `esp_zb_zdo_device_leave_req()` auf.

**Fix:**
1. **`command_handler.c` → `zigbee/zb_command_handler.c`** verschieben.
   Ist logisch ein Zigbee-Modul — sendet ZCL-Commands an Geräte.
   Adapter-Interface: `zigbee_adapter_send_command(dev_id, cmd_type, params)` für
   eventuelle Abstraktion bei Multi-Protocol (Matter/Thread).

2. **`bridge_request_handler.c`** aufteilen:
   - Zigbee-spezifische Request-Handler (permit_join, leave, backup, install_codes,
     reporting, network, topology) → `zigbee/zb_bridge_commands.c` (~800 LOC)
   - Protokoll-agnostische Bridge-Logik (device_options, rename, factory_reset,
     health_check, config) bleibt in `core/bridge/` (~1700 LOC)
   - Bridge dispatcht Zigbee-Requests via Event `EVT_ZIGBEE_COMMAND` an neuen Handler

3. **Direkter `esp_zb_lock_acquire()` Aufruf in Core eliminiert** — alle 7 Stellen.

**Aufwand:** ~400 LOC umstrukturieren + ~100 LOC neues Event-Interface
**Risiko:** Mittel (viele Funktions-Signaturen bleiben gleich, nur Dateien verschieben)

---

### 7.2 MQTT-Topic-Wissen aus Transport-Layer entfernen (HOCH)

**Problem:** 7 Dateien in `zigbee/` includieren `mqtt/mqtt_topics.h` und bauen
MQTT-Topic-Strings direkt im Transport-Layer:

| Datei | Nutzung |
|-------|---------|
| `zb_availability.c` | `mqtt_topic_*()` für Availability-Topics |
| `zb_leave_helper.c` | `mqtt_topic_*()` + importiert `bridge_request_handler.h` |
| `zb_backup.c` | `mqtt_topic_*()` für Backup-Response |
| `zb_groups.c` | `mqtt_topic_*()` für Group-Response |
| `zb_reporting.c` | `mqtt_topic_*()` für Reporting-Response |
| `zb_binding.c` | `mqtt_topic_*()` für Binding-Response |
| `tuya/tuya_fingerbot.c` | `mqtt_topic_ha_discovery()` für Discovery-Topics |

Zusätzlich: `zb_router.c` und `zb_multi_pan.c` definieren eigene `MQTT_TOPIC_*` Konstanten.

**Fix:**
1. **Topic-Konstruktion in Event-Payload verlagern:**
   Zigbee-Module publishen Events mit semantischen Daten (device_id, action, payload).
   Der MQTT-Adapter/Event-Handler baut daraus Topics.
   ```c
   // Vorher (in zigbee/):
   char topic[128];
   mqtt_topic_bridge_response(topic, sizeof(topic), "groups");
   event_publish(EVT_BRIDGE_PUBLISH, &(evt_bridge_publish_t){.topic=strdup(topic), ...});

   // Nachher (in zigbee/):
   event_publish(EVT_BRIDGE_RESPONSE, &(evt_bridge_response_t){
       .category = "groups", .action = "list", .payload_json = json});
   // mqtt_event_handler.c baut den Topic
   ```

2. **Neuer Event-Typ `EVT_BRIDGE_RESPONSE`** mit `category` + `action` statt fertigem Topic.

3. **`tuya_fingerbot.c`** Discovery: HA-Discovery-Topics via `EVT_HA_DISCOVERY_PUBLISH`
   publishen ohne `mqtt_topic_ha_discovery()` direkt aufzurufen — Topic-Bau in den
   Discovery-Handler verlagern.

4. **`MQTT_TOPIC_*` Konstanten** aus `zb_router.c` und `zb_multi_pan.c` entfernen,
   durch Event-basierte Kommunikation ersetzen.

5. **`zb_leave_helper.c`**: Import von `bridge_request_handler.h` entfernen,
   `device_options_remove()` über Event-Mechanismus aufrufen.

**Aufwand:** ~300 LOC (Events + Handler-Umstellung)
**Risiko:** Mittel (semantisch äquivalent, aber viele Dateien betroffen)

---

### 7.3 device_sync.c eliminieren (HOCH)

**Problem:** `device_sync.c` ist eine tote Abstraktionsschicht:
- 48 Caller von `device_sync_get()` — triviale Weiterleitung an `device_registry_get_by_short_addr()`
- 6 No-Op Stubs mit 0 Callern (`from_legacy`, `to_legacy`, `all_from_*`, `all_to_*`)
- `device_sync_add()`, `device_sync_set_converter()` — 0 externe Caller
- Header-Doku beschreibt eine Architektur die nicht mehr existiert
- Caller includieren oft BEIDE Headers (`device_sync.h` + `device_registry.h`)

**Fix:**
1. **48× `device_sync_get(short_addr)` → `device_registry_get_by_short_addr(short_addr)`**
   Mechanisches Suchen-und-Ersetzen in 16 Dateien.

2. **Verbleibende Wrapper umstellen:**
   - `device_sync_set_availability()` (1 Caller) → `device_registry_set_availability()`
   - `device_sync_update_last_seen()` (1 Caller) → `device_registry_update_last_seen()`
   - `device_sync_add_cluster()` (4 Caller) → `device_zigbee_add_cluster()`
   - `device_sync_remove()` (1 Caller) → `device_registry_remove()`
   - `device_sync_get_ieee()` (1 Caller) → Inline-Konvertierung aus `device_id_t`
   - `device_sync_add()` (0 Caller) → löschen

3. **`device_sync.c` + `device_sync.h` löschen** (~217 LOC + ~162 LOC = ~379 LOC)
4. **`device_sync_init()` / `device_sync_deinit()`** aus `foundation_init.c` entfernen
5. **CLAUDE.md** aktualisieren: Device Sync Layer entfernen

**Aufwand:** ~50 LOC Umstellung + ~379 LOC Löschung = netto -329 LOC
**Risiko:** Niedrig (rein mechanisch, alle Wrapper sind trivial)

---

### 7.4 Tote Abstraktionen und Dead Code entfernen (HOCH)

**A) zigbee_adapter.c Dead Hooks (~440 LOC)**

4 von 5 Event-Hooks in `zigbee_adapter.c` haben **0 Caller**:
- `zigbee_adapter_on_device_join()` — `zb_callbacks.c` publiziert `EVT_DEVICE_JOINED` direkt
- `zigbee_adapter_on_device_leave()` — `zb_callbacks.c` publiziert `EVT_DEVICE_LEFT` direkt
- `zigbee_adapter_on_attribute_report()` — Converter-System handelt direkt
- `zigbee_adapter_on_availability_change()` — `zb_availability.c` publiziert direkt

**Fix:** Tote Hooks löschen. Nur `zigbee_adapter_on_interview_complete()` (20 LOC) und
Capability-Mapping `map_clusters_to_capabilities()` (70 LOC) behalten.
Rest des Adapters: vtable-Lifecycle-Funktionen (~80 LOC).
Von ~530 LOC auf ~170 LOC reduzieren.

**B) `__attribute__((unused))` Funktionen (~500-800 LOC)**

15 Funktionen sind als `unused` markiert um Compiler-Warnings zu unterdrücken:

| Datei | Funktion | Aktion |
|-------|----------|--------|
| `wifi_manager.c` | `rssi_monitor_timer_callback` | Löschen |
| `zb_touchlink.c` | `touchlink_process_scan_response` | Behalten (in Entwicklung) |
| `ble_security.c` (4×) | `mac_to_nvs_key`, `handle_passkey_action`, `handle_enc_change`, `handle_pairing_complete` | Löschen |
| `cluster_state_ng.c` | `get_or_create_state` | Prüfen ob Core-NG-Funktion wirklich tot ist |
| `esphome_ble_proxy.c` (2×) | `find_connection_by_handle`, `get_conn_handle_for_address` | Löschen |
| `zb_multi_pan.c` | `switch_timer_callback` | Löschen |
| `esphome_ota.c` | (1 Funktion) | Löschen |
| `mqtt_logger.c` | (1 Funktion) | Löschen |
| `state_persistence.c` | `format_ieee_addr` | Löschen |
| `esphome_device_registry.c` | (1 Funktion) | Löschen |
| `module_manager.c` | (1 Funktion) | Löschen |

**C) Tote MQTT-Request-Handler in zigbee/ (~400 LOC)**

4 Funktionen mit MQTT-Topic-Vergleichslogik und 0 Callern:
- `zb_reporting_process_mqtt_request()` in `zb_reporting.c`
- `zb_groups_process_mqtt_request()` in `zb_groups.c`
- `zb_groups_process_mqtt_command()` in `zb_groups.c`
- `zb_binding_process_mqtt_request()` in `zb_binding.c`

Relikte der Pre-Event-Bus Architektur. **Komplett löschen.**

**D) `zb_device_t` Struct-Definition (~17 LOC)**

`zb_device_handler_types.h:155-171` definiert `zb_device_t` — niemand instantiiert diesen Typ.
**Löschen** (oder hinter `#ifdef LEGACY_COMPAT` verstecken falls externe Tools es brauchen).

**Aufwand:** ~1.200-1.600 LOC Löschung
**Risiko:** Niedrig (alles nachweislich toter Code)

---

### 7.5 C23 ehrlich machen (MITTEL)

**Problem:** `-std=gnu23` ist gesetzt, aber der Code ist C11/C17:
- 2× `static_assert` (einziges C23-Feature)
- 0× `nullptr` (4.163× `NULL`)
- 111 Dateien includieren noch `<stdbool.h>` (in C23 unnötig)
- 0× `constexpr`, `typeof_unqual`, oder andere C23-Features

**Fix (zwei Optionen — EINE wählen):**

**Option A: C23 ernst nehmen (~2h)**
1. `<stdbool.h>` aus allen 111 Dateien entfernen
2. `NULL` → `nullptr` in neuen/geänderten Dateien (NICHT alle 4.163 auf einmal)
3. Convention in CLAUDE.md: "Neuer Code nutzt `nullptr`, `static_assert`, `bool` als Keyword"
4. Bestehender `NULL`-Code wird nicht aktiv umgestellt (Churn ohne Nutzen)

**Option B: Claim reduzieren (~15 min)**
1. CLAUDE.md ändern: "gnu23 Compiler-Flag gesetzt, Code ist überwiegend C11/C17-kompatibel"
2. Kein Code-Änderung nötig

**Empfehlung:** Option A — `<stdbool.h>` entfernen ist mechanisch und zeigt Commitment.
`nullptr`-Migration nur für neuen Code um unnötigen Churn zu vermeiden.

**Aufwand:** 2h (Option A) oder 15 min (Option B)
**Risiko:** Null

---

### 7.6 sdkconfig.defaults bereinigen (MITTEL)

**A) Kommentar-Bug (SOFORT)**

Zeile 45-47:
```
# Reserve 10KB internal heap for DMA-capable and cache-disabled operations
# Lowered from 16KB - BLE controller and WiFi DMA need ~8-10KB reserved
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
```
Kommentar sagt "10KB / lowered from 16KB", Wert ist **32KB (32768)**.
**Fix:** Kommentar an Wert anpassen.

**B) Debug-Flags für Production entfernen oder gaten**

| Setting | Problem | Fix |
|---------|---------|-----|
| `FREERTOS_USE_TRACE_FACILITY=y` | Extra RAM pro Task | In `sdkconfig.debug` verschieben |
| `FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y` | ~1-2KB Code | In `sdkconfig.debug` verschieben |
| `FREERTOS_GENERATE_RUN_TIME_STATS=y` | CPU-Overhead | In `sdkconfig.debug` verschieben |
| `FREERTOS_VTASKLIST_INCLUDE_COREID=y` | Nutzlos auf Single-Core | Entfernen |
| `LOG_MAXIMUM_LEVEL_VERBOSE=y` | Verbose-Strings in Flash | → `LOG_MAXIMUM_LEVEL_INFO` für Production |

**Alternative:** `sdkconfig.defaults.debug` Overlay für Development, `sdkconfig.defaults`
bleibt Production-optimiert. ESP-IDF unterstützt `-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.debug"`.

**C) Fehlende Production-Settings**

| Setting | Warum |
|---------|-------|
| `CONFIG_COMPILER_STACK_CHECK_MODE_NORM=y` | Stack-Canary gegen Overflow |
| `CONFIG_BOOTLOADER_LOG_LEVEL_ERROR=y` | Schnellerer Boot |
| `CONFIG_ESP_BROWNOUT_DET=y` | Power-Stability |

**D) Redundante Settings entfernen**

5 Settings die bereits Default sind:
`LWIP_DNS_MAX_SERVERS=3`, `MQTT_PROTOCOL_311=y`, `ESP_TASK_WDT=y`,
`ESP_INT_WDT=y`, `NVS_ENCRYPTION=n`

**E) `CONFIG_ESP_SYSTEM_CHECK_INT_LEVEL_5` prüfen**

Zeile 360 — möglicherweise Xtensa-only, auf RISC-V ESP32-C5 ungültig.
Verifizieren ob Kconfig dieses Setting für C5 akzeptiert.

**Aufwand:** ~1h
**Risiko:** Niedrig (Build-Test nach jeder Änderung)

---

### 7.7 Code-Safety Fixes (MITTEL)

**A) `strncpy` ohne Null-Terminierung (5 Stellen)**

| Datei | Zeile | Pattern |
|-------|-------|---------|
| `esphome_noise.c` | 857-858 | `strncpy(dst, src, N)` ohne `-1` |
| `esphome_noise.c` | 866-867 | Gleich |
| `zb_ota.c` | 1633 | `strncpy(info->header_string, ..., MAX_LEN)` |
| `zb_scenes.c` | 244 | `strncpy(scene_name, ..., MAX_LEN)` |
| `zb_groups.c` | 237 | `strncpy(group_name, ..., MAX_LEN)` |

**Fix:** `strncpy(dst, src, sizeof(dst) - 1); dst[sizeof(dst) - 1] = '\0';`
Oder C23-Style: `strlcpy()` wenn verfügbar in Picolibc.

**B) `atoi`/`atol` ohne Error-Checking (2 Stellen)**

| Datei | Zeile | Code |
|-------|-------|------|
| `http_ota.c` | 503 | `s_total_size = (size_t)atol(evt->header_value)` |
| `ota_handler.c` | 103 | `s_ota_info.total_size = atoi(evt->header_value)` |

**Fix:** `strtol()` mit Fehlerprüfung. Malformed `Content-Length` darf nicht zu size=0 führen.

**C) Unchecked `xSemaphoreTake` Return-Values (~376 Stellen)**

Dominantes Pattern: `xSemaphoreTake(mutex, GW_TIMEOUT_LONG_TICKS)` ohne Check.
Bei 5s-Timeout und Contention → Code läuft ohne Mutex → Data Race.

**Fix-Strategie:**
1. Neues Macro `MUTEX_TAKE_OR_RETURN(mutex, timeout, ret)` das bei Timeout returnt
2. Schrittweise einführen bei jedem File-Touch (nicht alle 376 auf einmal)
3. Für kritische Pfade (device_registry, event_bus, NVS) sofort umstellen

```c
#define MUTEX_TAKE_OR_RETURN(mutex, timeout, retval) \
    do { \
        if (xSemaphoreTake(mutex, timeout) != pdTRUE) { \
            ESP_LOGW(TAG, "Mutex timeout in %s", __func__); \
            return retval; \
        } \
    } while (0)
```

**D) Unchecked `esp_timer_create` in `led_controller.c` (2 Stellen)**

Zeilen 315 und 603 — wenn Timer-Creation feilt, wird NULL-Handle verwendet.
**Fix:** Return-Value prüfen.

**Aufwand:** ~200 LOC (Safety-Fixes) + ongoing für Mutex-Macro
**Risiko:** Niedrig

---

### 7.8 Stale Kommentare und Dokumentation bereinigen (NIEDRIG)

**A) Stale "Legacy/Migration/Phase 2" Kommentare (20+ Stellen)**

| Datei | Zeile | Staler Kommentar |
|-------|-------|-----------------|
| `device_sync.h` | 5-11 | "Provides synchronization between legacy and NG" |
| `device_sync.h` | 44-85 | Doku für No-Op Stubs als ob sie funktionieren |
| `zigbee_adapter.c` | 171, 191, 251 | "auto-sync from legacy" |
| `zigbee_adapter.h` | 6 | Referenziert `zb_device_handler` |
| `zb_interview.c` | 1424 | "device_sync handles legacy registry updates" |
| `zb_coordinator.c` | 200, 250 | "legacy zb_device_handler eliminated" |
| `zb_cluster_multistate.h/c` | 5 | "Extracted from zb_device_handler.c" |
| `event_bus.h/c`, `event_data.h` | | "Phase 2 Callback Migration" |
| `device_persistence.c` | 112, 804, 888 | Migration-Kommentare |
| `state_persistence.c` | 775 | "Reserved for future use in legacy device migration" |
| `zb_callbacks.c` | 1406 | "replaces legacy zb_device_update_state" |
| `zb_converter_registry.c` | 366 | Referenziert `zb_device_t` |
| `unified_device.h` | 161 | Referenziert `zb_device_t` |

**Fix:** Alle durch aktuelle, zutreffende Kommentare ersetzen oder entfernen.

**B) 22 TODO-Kommentare für unimplementierte Features**

Kritische (Feature-Lücken die dokumentiert sein sollten):
- `zb_green_power.c` (6 TODOs): GP Key Derivation, MIC Verification, ZCL Dispatch unimplementiert
- `ble_gatt_discovery.c`, `ble_battery_service.c`: "TODO: Implement MQTT publishing"
- `zb_groups.c` (2 TODOs): "TODO: Actually publish via MQTT client"
- `esphome_adapter.c` (2 TODOs): "TODO: Implement command forwarding"
- `zb_topology.c`: "TODO: Implement routing table queries"
- `zb_multi_pan.c`: "TODO: Implement actual Zigbee stack synchronization"
- `zb_ota.c` (2 TODOs): "TODO: Implement IEEE parsing", "TODO: Implement update check"

**Fix:** Jedes TODO bewerten:
- Feature geplant → TODO behalten + Tracking in CLAUDE.md Roadmap
- Feature nicht geplant → Code und TODO entfernen (Dead Feature)
- Feature-Stub ohne echte Funktion → Ehrlich in Feature-Tabelle markieren

**Aufwand:** ~2h
**Risiko:** Null

---

### 7.9 Availability Tracker: Parallele Device-Liste eliminieren (MITTEL)

**Problem:** `zb_availability.c` unterhält ein **eigenes Device-Array**:
```c
static zb_availability_device_t devices[ZB_AVAIL_MAX_DEVICES];  // Zeile 74
```
Mit eigenem `find_device()`, eigenem Mutex, eigener State-Machine.
Das widerspricht dem "Unified Device Model" Claim — es ist ein paralleles
Device-Tracking-System neben `device_registry`.

**Fix:**
1. Availability-State als Feld in `device_t` speichern (z.B. `last_seen`, `check_interval`,
   `availability_state_machine`) oder als Cluster-State via `cluster_state_ng`
2. `zb_availability.c` arbeitet auf `device_registry` statt eigenem Array
3. `zb_availability_device_t` Struct in `device_t` integrieren oder als cJSON-State

**Aufwand:** ~200 LOC Umstrukturierung
**Risiko:** Mittel (Availability-Logik ist timing-kritisch, sorgfältig testen)

---

### 7.10 CLAUDE.md Nachkorrektur (SOFORT)

Die Re-Analyse zeigt dass CLAUDE.md mehrere Claims enthält die zu optimistisch sind:

| Aktueller Claim | Korrektur |
|-----------------|-----------|
| "Event-Driven Architecture ✅" | "Event-Driven für State-Propagation. Bridge-Commands und Zigbee-Control sind Direct-Call" |
| "0 direct MQTT in zigbee/" | "0 MQTT publish calls, aber 7 Files nutzen mqtt_topics.h für Topic-Konstruktion" |
| "C23 Standard (gnu23) — static_assert, nullptr verfügbar" | "gnu23 Flag aktiv, 2× static_assert, nullptr verfügbar aber nicht genutzt" |
| "Layer-Violations eliminiert" | "Core→MQTT eliminiert, Core→Zigbee noch 19 Imports in command_handler + bridge_request_handler" |
| Feature-Tabelle zeigt ✅ für alles | Zigbee Green Power, Multi-Pan, einige BLE-Features haben TODOs für Kernfunktionen |
| "Device Sync Layer ✅" | Entfernen nach AP-7.3, oder ehrlich: "Thin wrapper, geplant zur Elimination" |

**Aufwand:** 30 min
**Risiko:** Null

---

### 7.11 `zb_device_handler_types.h` aufräumen (NIEDRIG)

**Problem:** 1.078 Zeilen Header-Datei die nach der Handler-Elimination zu groß ist.
Enthält:
- `zb_device_t` Struct (17 LOC, tot — 0 Instantiierungen)
- `ZB_STATE_MAX_*` Konstanten (31 LOC, noch aktiv in Cluster-Dateien)
- Legacy Power-Descriptor Typen die jetzt in `unified_device.h` leben
- Cluster-spezifische State-Structs die zu den jeweiligen `zb_cluster_*.h` Headers gehören
- ZCL Cluster-ID/Attribut-ID Konstanten (universell genutzt)

**Fix:**
1. `zb_device_t` Struct löschen (0 Nutzer)
2. Cluster-State-Structs in die jeweiligen `zb_cluster_*.h` verschieben:
   - `zb_thermostat_state_t` → `zb_cluster_hvac.h`
   - `zb_fan_control_state_t` → `zb_cluster_hvac.h`
   - `zb_electrical_state_t` → `zb_cluster_electrical.h`
   - `zb_metering_state_t` → `zb_cluster_electrical.h`
   - `zb_door_lock_state_t` → `zb_cluster_security.h`
   - `zb_ias_zone_state_t` → `zb_cluster_security.h`
   - `zb_window_covering_state_t` → `zb_cluster_closures.h`
   - `zb_illuminance_state_t` → `zb_cluster_measurement.h`
   - `zb_pressure_state_t` → `zb_cluster_measurement.h`
   - `zb_pm25_state_t` → `zb_cluster_measurement.h`
   - `zb_multistate_state_t` → `zb_cluster_multistate.h`
3. `ZB_STATE_MAX_*` Konstanten mit den Structs verschieben
4. ZCL-Konstanten (Cluster-IDs, Attribut-IDs) bleiben in `zb_device_handler_types.h`
   → Datei umbenennen zu `zb_zcl_constants.h`
5. Callback-Typedefs verschieben zu den jeweiligen Cluster-Headers

**Ziel:** `zb_device_handler_types.h` von 1.078 auf ~400 LOC (reine ZCL-Konstanten)
+ saubere Zuordnung der Cluster-Types zu ihren Modulen

**Aufwand:** ~4h (43 Dateien die den Header includieren müssen geprüft werden)
**Risiko:** Mittel (viele Include-Chain-Änderungen)

---

### Priorisierte Reihenfolge AP-7

```
Sprint 1 (Sofort):
  7.10 CLAUDE.md Nachkorrektur       — 30 min   Ehrlichkeit herstellen
  7.6A sdkconfig Kommentar-Bug       — 5 min    Offensichtlicher Fehler
  7.7A strncpy Safety                 — 30 min   Buffer-Overflow-Risiko
  7.7B atoi→strtol                    — 15 min   Parsing-Safety

Sprint 2 (Hoch):
  7.3  device_sync eliminieren        — 2h       ~379 LOC weg, sauberes API
  7.4A zigbee_adapter aufräumen       — 2h       ~360 LOC Dead Code weg
  7.4C Tote MQTT-Handler löschen      — 1h       ~400 LOC Dead Code weg
  7.4D zb_device_t Struct löschen     — 15 min   Toten Typ entfernen

Sprint 3 (Mittel):
  7.5  C23 ehrlich machen             — 2h       stdbool.h entfernen
  7.6  sdkconfig komplett bereinigen  — 1h       Debug-Flags, Redundanzen
  7.4B unused-Funktionen löschen      — 2h       ~600 LOC Dead Code weg
  7.8  Stale Kommentare               — 2h       20+ veraltete Kommentare

Sprint 4 (Groß):
  7.1  command_handler verschieben    — 4h       Layer-Violation beheben
  7.2  MQTT-Topics aus zigbee/        — 4h       Transport→Protocol entkoppeln
  7.9  Availability Tracker           — 3h       Parallele Device-Liste weg

Sprint 5 (Aufräumen):
  7.11 types.h aufteilen              — 4h       Cluster-Types zu Cluster-Headers
  7.7C Mutex-Macro einführen          — Ongoing  Bei jedem File-Touch
  7.8B TODO-Audit                     — 2h       22 TODOs bewerten
```

---

### Metriken AP-7

| Metrik | Vorher | Nachher (Ziel) |
|--------|--------|----------------|
| Layer-Violations (Core→Zigbee direkt) | 19 Imports in 2 Dateien | 0 |
| MQTT-Topic-Includes in zigbee/ | 7 Dateien + 2 mit eigenen Konstanten | 0 |
| device_sync.c (tote Abstraktion) | 379 LOC, 48 Passthrough-Caller | 0 LOC |
| Dead Code (unused Funktionen) | ~1.500-2.000 LOC | 0 LOC |
| C23 Features tatsächlich genutzt | 2 (static_assert) | stdbool.h weg + nullptr-Convention |
| sdkconfig Debug-in-Production | 5 Settings | 0 (oder in .debug Overlay) |
| strncpy ohne Null-Term | 5 Stellen | 0 |
| Unchecked atoi/atol | 2 Stellen | 0 |
| Stale Legacy-Kommentare | 20+ | 0 |
| Parallele Device-Arrays | 1 (zb_availability) | 0 |
| `zb_device_handler_types.h` | 1.078 LOC Monolith | ~400 LOC (reine ZCL-Konstanten) |

---

## Erfolgskriterien

Nach Abschluss aller APs kann das Projekt **legitimerweise** "NG" heißen wenn:

### AP-1 bis AP-6 (Original-Review)
- [x] **Null** Race Conditions in bekannten Hotspots ✅ AP-1
- [x] **Null** Legacy `zb_device_handler.c` ✅ AP-2
- [x] **Null** Legacy NVS Namespace ✅ AP-2.3
- [x] **Event Bus** mit Overflow-Warning und >8 Subscriber Support ✅ AP-4.2
- [x] **Device Registry Iterator** blockiert nicht mehr auf externe I/O ✅ AP-1.3
- [x] **Picolibc** Compat-Modus permanent (Zigbee-Abhängigkeit dokumentiert) ✅ AP-3.2

### AP-7 (Re-Audit 2026-02-19)
- [ ] **Null** Layer-Violations Core→Zigbee (command_handler, bridge_request_handler) → AP-7.1
- [ ] **Null** MQTT-Topic-Wissen in Transport-Layer → AP-7.2
- [ ] **Null** tote Abstraktionsschichten (device_sync) → AP-7.3
- [ ] **Null** Dead Code (`__attribute__((unused))`, tote MQTT-Handler) → AP-7.4
- [ ] **C23** ehrlich: entweder Features nutzen oder Claim reduzieren → AP-7.5
- [ ] **sdkconfig** produktionsreif (keine Debug-Flags, keine Kommentar-Bugs) → AP-7.6
- [ ] **Code-Safety** (strncpy, atoi, Mutex-Checks) → AP-7.7
- [ ] **Null** stale Legacy-Kommentare → AP-7.8
- [ ] **Unified Device Model** wirklich unified (keine parallelen Device-Arrays) → AP-7.9
- [ ] **CLAUDE.md** reflektiert ehrlichen Stand nach Re-Audit → AP-7.10
- [ ] **Cluster-Types** bei ihren Modulen, nicht im Monolith-Header → AP-7.11

### Verbleibend
- [ ] **CI/CD Pipeline** die bei jedem Push baut und lintet → AP-5
- [x] **CLAUDE.md** erste Korrektur ✅ AP-6 (Nachkorrektur in AP-7.10)

---

## Metriken

| Metrik | Original (AP-1-6) | Aktuell | Ziel (AP-7) | Status |
|--------|-------------------|---------|-------------|--------|
| Legacy-Code (handler.*) | 5.986 LOC | 0 LOC | 0 LOC | ✅ |
| Race Conditions | 4 | 0 | 0 | ✅ |
| Legacy NVS | 2 parallel | 1 (auto-erase) | 1 | ✅ |
| Layer-Violations Core→Zigbee | nicht gemessen | 19 Imports | 0 | ❌ AP-7.1 |
| MQTT-Wissen in zigbee/ | 13 Imports | 9 Imports | 0 | ❌ AP-7.2 |
| Tote Abstraktionsschichten | nicht gemessen | 2 (sync, adapter) | 0 | ❌ AP-7.3/7.4 |
| Dead Code (unused) | ~1.800 LOC | ~1.500-2.000 LOC | 0 | ❌ AP-7.4 |
| C23 Features genutzt | 0 | 2 (static_assert) | stdbool weg + Convention | ❌ AP-7.5 |
| sdkconfig Debug-in-Prod | nicht geprüft | 5 Settings | 0 | ❌ AP-7.6 |
| Code-Safety Issues | nicht geprüft | 7+ Stellen | 0 | ❌ AP-7.7 |
| Stale Kommentare | nicht geprüft | 20+ | 0 | ❌ AP-7.8 |
| Parallele Device-Arrays | nicht gemessen | 1 | 0 | ❌ AP-7.9 |
| types.h Monolith | 1.078 LOC | 1.078 LOC | ~400 LOC | ❌ AP-7.11 |
| CI/CD Pipeline | nicht existent | nicht existent | Build+Lint | ❌ AP-5 |
| Event Bus | 8 (silent fail) | 16 (Warning) | — | ✅ |
| Iterator Deadlock | 4 Callbacks | 0 | — | ✅ |
