# Hybrid Architecture Plan: ESPHome Native API + MQTT

<!-- staleness-banner -->
> **Stand 2026-08-05.** Historisch — der Plan ist umgesetzt. Die Erwaehnung von `cluster_state_ng`
> beschreibt ein Modul, das am 2026-08-05 geloescht wurde (nie aufgerufen,
> funktional durch `device_registry_set_state()` ersetzt).
>
> Aktuell gepflegt wird `CLAUDE.md` im Projektwurzelverzeichnis.


## Vision

Das ESP32-C5 nutzt **ESPHome Native API als primäre HA-Integration** für alle
Zigbee-Geräte. MQTT bleibt als sekundärer Kanal für Bridge-Management, Debug
und externe Clients (Node-RED, etc.).

**Ausgangslage:** ESPHome läuft bereits für BLE Proxy. HA kennt das Device.
Die TCP-Verbindung mit Noise Encryption steht. Wir erweitern den bestehenden
Kanal um Zigbee-Entities.

## Architektur

```
┌────────────────────────────────────────────────────────┐
│                   HOME ASSISTANT                        │
│                                                         │
│  ESPHome Integration (bereits aktiv für BLE)           │
│    ├── Gateway Sub-Device (ESP32-C5 selbst)            │
│    │   ├── sensor.gateway_uptime                       │
│    │   ├── sensor.gateway_heap_free                    │
│    │   └── sensor.gateway_zigbee_count                 │
│    │                                                    │
│    ├── Zigbee Sub-Device: "IKEA Lampe Wohnzimmer"      │
│    │   ├── light.ikea_wohnzimmer       ← ESPHome API   │
│    │   └── sensor.ikea_wohnzimmer_lqi  ← ESPHome API   │
│    │                                                    │
│    ├── Zigbee Sub-Device: "Aqara Tür Haustür"          │
│    │   ├── binary_sensor.aqara_tuer    ← ESPHome API   │
│    │   └── sensor.aqara_tuer_battery   ← ESPHome API   │
│    │                                                    │
│    └── BLE Sub-Devices (bereits vorhanden)             │
│        └── BLE Proxy Entities           ← ESPHome API   │
│                                                         │
│  MQTT Integration (optional, sekundär)                  │
│    └── zigbee2mqtt/bridge/* (Management API only)      │
└────────────────────────────────────────────────────────┘
         │ TCP:6053 (Noise)          │ MQTT (optional)
         ▼                            ▼
┌────────────────────────────────────────────────────────┐
│                    ESP32-C5                              │
│                                                         │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────┐ │
│  │ Zigbee      │  │ ESPHome API  │  │ MQTT Bridge   │ │
│  │ Coordinator │  │ Server       │  │ (sekundär)    │ │
│  │             │  │ Port 6053    │  │               │ │
│  │ Interview   │→ │ Entities     │  │ Bridge API    │ │
│  │ Converter   │  │ Sub-Devices  │  │ Logging       │ │
│  │ Clusters    │  │ Commands     │  │ Debug         │ │
│  └──────┬──────┘  └──────┬───────┘  └───────────────┘ │
│         │                │                              │
│         ▼                ▼                              │
│  ┌─────────────────────────────┐                       │
│  │        Event Bus            │                       │
│  │  EVT_DEVICE_STATE_CHANGED   │                       │
│  │  EVT_DEVICE_JOINED          │                       │
│  │  EVT_DEVICE_LEFT            │                       │
│  │  EVT_DEVICE_INTERVIEWED     │                       │
│  └─────────────┬───────────────┘                       │
│                │                                        │
│  ┌─────────────▼───────────────┐                       │
│  │   Device Registry (NG)      │                       │
│  │   device_t + capabilities   │                       │
│  │   + cluster_state (cJSON)   │                       │
│  └─────────────────────────────┘                       │
└────────────────────────────────────────────────────────┘
```

## Datenfluss

### State Updates (Zigbee → HA)
```
Zigbee Device sendet Report
  → zb_callbacks.c: attribute_report_handler()
  → cluster_state_update_*() in cluster_state_ng.c
  → event_publish(EVT_DEVICE_STATE_CHANGED)
  → esphome_adapter.c: handle_device_state_changed()
     → update_entity_states() extrahiert JSON-Felder
     → esphome_entity_update_sensor(key, value)
     → esphome_api_broadcast_state() → TCP → HA
```
**Status: ✅ Bereits implementiert und funktional**

### Commands (HA → Zigbee) — HAUPTARBEIT
```
HA User drückt Button
  → ESPHome API: LightCommandRequest {key, state, brightness}
  → esphome_api_handlers.c: handle_light_command()
  → esphome_adapter.c: light_command_callback()
  → [NEU] Lookup device_t via entity key
  → [NEU] zb_converter_handle_command(short_addr, endpoint, json)
     → zb_converter_get(short_addr) → converter_def
     → tz_on_off() / tz_brightness() / tz_color_temp()
     → esp_zb_zcl_*_cmd_req() → Zigbee ZCL → Device
```
**Status: ❌ Callbacks existieren, aber TODO: "Implement command forwarding"**

### Device Discovery (Zigbee Join → HA Entity)
```
Zigbee Device joined
  → zb_interview.c: interview_complete()
  → event_publish(EVT_DEVICE_INTERVIEWED)
  → esphome_adapter.c: handle_device_joined()
     → register_device_entities()
        → [NEU] Register DeviceInfo sub-device
        → Register entities mit device_id
     → esphome_api_broadcast_state() → HA sieht neues Gerät
```
**Status: ⚠️ Entity-Registration existiert, Sub-Device fehlt**

## Ist-Analyse: Was existiert bereits

### ✅ Vollständig implementiert (0 Aufwand)
| Komponente | Datei | Details |
|-----------|-------|---------|
| TCP Server + Noise | esphome_api_server.c | Port 6053, 2 Clients, Keepalive |
| Protocol Encoding | esphome_protocol.c | 30+ Message Types, Protobuf-Wire |
| Entity System | esphome_entity_*.c | 15 Entity-Typen, je max 8-32 |
| State Broadcasting | esphome_api.c | broadcast_state() an alle Clients |
| Event Bus Integration | esphome_adapter.c | 3 Events subscribed |
| Capability→Entity Map | esphome_adapter.c | 14 Capabilities → Entities |
| State JSON→Entity | esphome_adapter.c | update_entity_states() |
| BLE Proxy | esphome_ble_proxy.c | Advertisements, GATT, Connect |
| mDNS Discovery | esphome_api.c | _esphomelib._tcp |
| OTA via ESPHome | esphome_ota.c | Firmware Update |
| Services | esphome_services.c | Custom HA Services |

### ⚠️ Teilweise implementiert (kleiner Aufwand)
| Komponente | Datei | Was fehlt |
|-----------|-------|-----------|
| Switch Command Callback | esphome_adapter.c:385 | TODO: Forward to Zigbee |
| Light Command Callback | esphome_adapter.c:434 | TODO: Forward to Zigbee |
| Device Leave Handling | esphome_adapter.c:994 | Nur TEMP/HUMIDITY markiert |

### ❌ Nicht implementiert (Kernarbeit)
| Komponente | Aufwand | Details |
|-----------|---------|---------|
| Sub-Device Protocol (device_id) | Mittel | DeviceInfo + field in 15 Entity-Typen |
| Command Routing (alle Typen) | Mittel | Cover, Lock, Climate, Fan, Number, Select, Button |
| Availability Propagation | Klein | Zigbee offline → Entity unavailable |
| HA Discovery Bypass | Klein | Kconfig-Switch zum Deaktivieren |
| Entity Unregistration | Klein | Device Leave → Entities entfernen |

## Implementierungsplan

### Phase 0: Protokoll-Erweiterung (Sub-Device Support)
**Ziel:** Zigbee-Geräte als eigenständige Sub-Devices in HA

#### 0.1 DeviceInfo in DeviceInfoResponse
**Datei:** `esphome_api_handlers.c` + `esphome_protocol.c`

Aktuell sendet `DeviceInfoResponse` nur Gateway-Infos. Erweitern um:
```c
// In DeviceInfoResponse (nach bestehenden Feldern):
// Field 20: repeated DeviceInfo devices
//   DeviceInfo { device_id=1, name=2, area_id=3 }

// Neue Struktur:
typedef struct {
    uint32_t device_id;         // Hash aus IEEE-Addr
    char name[64];              // friendly_name
} esphome_sub_device_t;
```

**Ablauf:**
1. Bei `handle_device_info_request()`: Device Registry iterieren
2. Für jedes Zigbee-Device: DeviceInfo-Eintrag in Response encodieren
3. device_id = untere 32 Bit der IEEE-Adresse (kollisionsfrei bei <100 Devices)

#### 0.2 device_id in Entity-Structs
**Dateien:** `esphome_entities_types.h` (alle 15 Structs)

Jede Entity-Config bekommt ein `uint32_t device_id` Feld:
```c
typedef struct {
    esphome_entity_key_t key;
    char name[ESPHOME_MAX_NAME_LEN];
    // ... bestehende Felder ...
    uint32_t device_id;         // NEU: 0 = Gateway, >0 = Sub-Device
} esphome_sensor_config_t;
```

#### 0.3 device_id in Protocol Encoding
**Dateien:** `esphome_entity_sensors.c`, `esphome_entity_controls.c`,
`esphome_entity_specialized.c`

Jede `esphome_encode_*_list_entry()` Funktion bekommt den device_id-Feld-Encode:
```c
// Am Ende jedes Encoders (nach bestehenden Feldern):
if (config->device_id != 0) {
    ENCODE_UINT32(output, &pos, FIELD_DEVICE_ID, config->device_id);
}
```

**Field-Nummern** (aus ESPHome api.proto, variieren je Entity-Typ):
- Sensor: field 14
- BinarySensor: field 9 (oder nächstes freies)
- Light: field 16
- Switch: field 10
- Cover: field 13
- etc.

→ Diese müssen exakt mit ESPHome's api.proto übereinstimmen!

#### 0.4 device_id Zuweisung im Adapter
**Datei:** `esphome_adapter.c`

Bei `register_device_entities()`:
```c
uint32_t device_id = (uint32_t)(device->id & 0xFFFFFFFF);

// Bei Entity-Registration:
esphome_sensor_config_t sensor_cfg = {
    .key = make_entity_key(device->id, DEV_CAP_TEMPERATURE),
    .name = "Temperatur",
    .device_id = device_id,     // NEU
    // ...
};
```

**Geschätzter Aufwand:** 2-3 Tage (15 Structs + 15 Encoder + Handler + Adapter)

---

### Phase 1: Command Routing (ESPHome → Zigbee)
**Ziel:** HA kann Zigbee-Geräte über ESPHome steuern

#### 1.1 Device Lookup aus Entity Key
**Datei:** `esphome_adapter.c`

Neuer Helper der aus einem Entity-Key das Zigbee-Device findet:
```c
// Bestehend: esphome_adapter_parse_entity_key() → gibt nur capability zurück
// Neu: vollständiger Lookup

static device_t* find_device_for_entity_key(esphome_entity_key_t key) {
    device_capability_t cap;
    // Parse key to get capability type
    esphome_adapter_parse_entity_key(key, NULL, &cap);

    // Iterate registry to find device with matching key
    // (entity key enthält hash von device_id + capability)
    device_registry_iterate(match_by_entity_key_cb, &key);
    return matched_device;
}
```

#### 1.2 Switch Command → Zigbee
**Datei:** `esphome_adapter.c`, `switch_command_callback()`

```c
static esp_err_t switch_command_callback(esphome_entity_key_t key, bool state) {
    device_t *dev = find_device_for_entity_key(key);
    if (!dev) return ESP_ERR_NOT_FOUND;

    // Erstelle JSON wie MQTT command_handler es erwartet:
    cJSON *cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "state", state ? "ON" : "OFF");

    esp_err_t ret = zb_converter_handle_command(
        dev->proto.zigbee.short_addr,
        dev->proto.zigbee.endpoint,
        cmd
    );
    cJSON_Delete(cmd);
    return ret;
}
```

#### 1.3 Light Command → Zigbee
**Datei:** `esphome_adapter.c`, `light_command_callback()`

```c
static esp_err_t light_command_callback(esphome_entity_key_t key,
                                         const esphome_light_command_t *cmd) {
    device_t *dev = find_device_for_entity_key(key);
    if (!dev) return ESP_ERR_NOT_FOUND;

    cJSON *json = cJSON_CreateObject();
    if (cmd->has_state)
        cJSON_AddStringToObject(json, "state", cmd->state ? "ON" : "OFF");
    if (cmd->has_brightness)
        cJSON_AddNumberToObject(json, "brightness", cmd->brightness * 254.0f);
    if (cmd->has_color_temp)
        cJSON_AddNumberToObject(json, "color_temp", cmd->color_temp);
    if (cmd->has_rgb) {
        cJSON_AddNumberToObject(json, "color", /* convert RGB→XY */);
    }

    esp_err_t ret = zb_converter_handle_command(
        dev->proto.zigbee.short_addr,
        dev->proto.zigbee.endpoint,
        json
    );
    cJSON_Delete(json);
    return ret;
}
```

#### 1.4 Weitere Command Callbacks (Cover, Lock, Climate, Fan, Number, Select, Button)
**Datei:** `esphome_adapter.c`

Für jeden Entity-Typ einen Callback registrieren:

| ESPHome Entity | JSON-Key | Zigbee Converter |
|---------------|----------|-----------------|
| Cover | `position`, `tilt` | `tz_cover_position` |
| Lock | `state: LOCK/UNLOCK` | `tz_lock_state` |
| Climate | `temperature`, `mode` | `tz_thermostat_*` |
| Fan | `state`, `speed` | `tz_fan_mode` |
| Number | `value` | `tz_tuya_number` |
| Select | `option` | `tz_tuya_enum` |
| Button | (press event) | `tz_identify` |

Pattern: Callback → find_device → build JSON → zb_converter_handle_command()

#### 1.5 Entity Registration für neue Typen
**Datei:** `esphome_adapter.c`, `register_device_entities()`

Erweitern um Cover, Lock, Climate, Fan für die entsprechenden device_t Capabilities:

| Capability | ESPHome Entity | Registration |
|-----------|---------------|--------------|
| DEV_CAP_COVER | Cover | position + tilt modes |
| DEV_CAP_LOCK | Lock | lock/unlock states |
| DEV_CAP_CLIMATE | Climate | heat/cool modes, target temp |
| DEV_CAP_FAN | Fan | speed modes |
| DEV_CAP_ILLUMINANCE | Sensor | lux, device_class=ILLUMINANCE |
| DEV_CAP_CO | BinarySensor | device_class=CO |
| DEV_CAP_COLOR_TEMP | (in Light) | color temp range |

**Geschätzter Aufwand:** 3-4 Tage (Device Lookup + 8 Callbacks + Registration)

---

### Phase 2: Availability & Lifecycle
**Ziel:** Offline-Geräte korrekt in HA anzeigen

#### 2.1 Availability Event Handling
**Datei:** `esphome_adapter.c`

Neuen Event-Handler für `EVT_DEVICE_AVAILABILITY_CHANGED`:
```c
static void handle_availability_changed(event_type_t type, void *data, ...) {
    evt_availability_t *evt = (evt_availability_t *)data;
    device_t *dev = device_registry_get(evt->ieee_addr);
    if (!dev) return;

    if (evt->available) {
        // Mark all entities for this device as available
        set_device_entities_available(dev->id, true);
    } else {
        // Mark all entities as unavailable (NaN for sensors, etc.)
        set_device_entities_available(dev->id, false);
    }
}
```

#### 2.2 Device Leave → Entity Cleanup
**Datei:** `esphome_adapter.c`, `esphome_adapter_remove_device()`

Komplett implementieren für ALLE Capability-Typen (aktuell nur TEMP/HUMIDITY):
```c
esp_err_t esphome_adapter_remove_device(device_id_t id) {
    device_t *dev = device_registry_get(id);
    if (!dev) return ESP_ERR_NOT_FOUND;

    // Für jede Capability die Entity als missing markieren
    for (uint32_t cap = 1; cap < DEV_CAP_MAX; cap <<= 1) {
        if (dev->capabilities & cap) {
            uint32_t key = esphome_adapter_make_entity_key(id, cap);
            mark_entity_unavailable(key, cap);
        }
    }

    // Sub-Device aus DeviceInfo-Liste entfernen
    remove_sub_device(id);
    return ESP_OK;
}
```

#### 2.3 Reconnect-Handling
**Datei:** `esphome_adapter.c`

Bei `esphome_adapter_on_client_connected()`:
- Alle Sub-Devices neu registrieren
- Alle Entity-States synchronisieren
- Availability-Status für alle Geräte senden

**Geschätzter Aufwand:** 1-2 Tage

---

### Phase 3: MQTT Discovery Bypass
**Ziel:** MQTT nur noch für Bridge-API, kein HA Discovery mehr

#### 3.1 Kconfig-Option
**Datei:** `main/Kconfig.projbuild`

```
config ESPHOME_PRIMARY_INTEGRATION
    bool "Use ESPHome Native API as primary HA integration"
    default y
    help
        When enabled, Zigbee devices are exposed via ESPHome Native API
        instead of MQTT Discovery. MQTT remains available for bridge
        management, debug logging, and external clients.

config MQTT_HA_DISCOVERY_ENABLED
    bool "Enable MQTT HA Discovery"
    default n
    depends on !ESPHOME_PRIMARY_INTEGRATION
    help
        Publish MQTT Discovery messages for Home Assistant.
        Disable when using ESPHome as primary integration.
```

#### 3.2 Discovery Guard
**Datei:** `ha_discovery_ng.c`

```c
esp_err_t ha_discovery_publish_device(device_t *dev) {
    #if CONFIG_ESPHOME_PRIMARY_INTEGRATION
    // Skip MQTT discovery when ESPHome is primary
    return ESP_OK;
    #endif
    // ... bestehende Discovery-Logik ...
}
```

#### 3.3 MQTT Bridge vereinfachen
**Datei:** `mqtt_bridge.c`

Bei `mqtt_bridge_start()`:
- Skip `ha_discovery_ng_init()` wenn ESPHome primary
- Skip `device_state_publisher_init()` (State geht über ESPHome)
- Behalte: `bridge_request_handler`, `mqtt_event_handler` (für Bridge-API)

#### 3.4 MQTT Adapter einschränken
**Datei:** `mqtt_adapter.c`

Wenn ESPHome primary:
- NICHT mehr auf `EVT_DEVICE_STATE_CHANGED` reagieren (ESPHome macht das)
- NUR noch Bridge-relevante Events weiterleiten

**Geschätzter Aufwand:** 1 Tag

---

### Phase 4: Optimierung & Polish
**Ziel:** Produktionsreife

#### 4.1 Entity-Name-Generierung
Zigbee friendly_name → ESPHome entity name:
```
"IKEA Lampe Wohnzimmer" → entity: "ikea_lampe_wohnzimmer"
                        → name: "IKEA Lampe Wohnzimmer"
                        → unique_id: "0x00124b001234abcd_light"
```

#### 4.2 Converter Expose → ESPHome Entity
Converter `exposes[]` (definiert welche Features ein Gerät hat) direkt auf
ESPHome Entities mappen — nicht nur über device_t Capabilities:
```c
// Converter expose:
{ .type = EXPOSE_NUMERIC, .name = "temperature", .unit = "°C" }
// → ESPHome Sensor: name="temperature", unit="°C", device_class=TEMPERATURE
```

Dies ermöglicht Tuya-spezifische Entities (Fingerbot-Modus, etc.) ohne
Capability-Erweiterung.

#### 4.3 ESPHome Services für Bridge-Funktionen
Bridge-Funktionen als ESPHome HA-Services registrieren:
```
esphome.esp32_c5_permit_join:
  time: 254

esphome.esp32_c5_network_map:
  format: "graphviz"
```

#### 4.4 State Batching
Bei vielen gleichzeitigen Updates (z.B. nach Reconnect): State Updates
batchen und als Burst senden statt einzeln.

#### 4.5 Memory-Optimierung
- Entity-Structs in PSRAM (name/unique_id sind große Strings)
- Lazy Entity Encoding (nur bei ListEntities, nicht vorher)
- Shared unique_id Prefix (IEEE-Addr als Pointer statt Kopie)

**Geschätzter Aufwand:** 2-3 Tage

---

## Zusammenfassung

| Phase | Beschreibung | Aufwand | Abhängigkeiten |
|-------|-------------|---------|----------------|
| **0** | Sub-Device Protocol | 2-3 Tage | Keine (kann sofort starten) |
| **1** | Command Routing | 3-4 Tage | Phase 0 (device lookup) |
| **2** | Availability & Lifecycle | 1-2 Tage | Phase 0 (entity tracking) |
| **3** | MQTT Discovery Bypass | 1 Tag | Phase 1+2 (alles muss funktionieren) |
| **4** | Optimierung & Polish | 2-3 Tage | Phase 3 |
| | **Gesamt** | **~10-13 Tage** | |

## Risiken

| Risiko | Auswirkung | Mitigation |
|--------|-----------|------------|
| **ESPHome Protocol Mismatch** | HA erkennt Sub-Devices nicht | api.proto Field-Nummern exakt abgleichen |
| **HA ESPHome Version** | Alte HA-Version ohne Sub-Device Support | Min. HA 2025.7+ erforderlich |
| **Memory Overhead** | 15 Entity-Structs × device_id = +60 Bytes | Minimal (~1KB für 15 Devices) |
| **Converter JSON Format** | ESPHome-Command ≠ Z2M-JSON-Format | Adapter-Layer übersetzt |
| **Entity Limits** | Max 32 Sensors, 16 Switches, etc. | Reicht für ~10-15 Zigbee Devices |
| **BLE + Zigbee Entity Conflict** | Entity Key Kollision | Adapter-Prefix 0x1 vs 0x2 |

## Testplan

### Phase 0 Verifizierung
1. ESP32 flashen → HA ESPHome Integration → Sub-Devices sichtbar?
2. Zigbee-Gerät joinen → Neues Sub-Device erscheint in HA?
3. Sensor-Werte → Updates in HA Echtzeit?

### Phase 1 Verifizierung
4. HA Dashboard: Light-Switch drücken → Zigbee-Lampe reagiert?
5. HA Dashboard: Dimmer bewegen → Helligkeit ändert sich?
6. Lock/Cover/Climate commands → Zigbee-Gerät reagiert?

### Phase 2 Verifizierung
7. Zigbee-Gerät ausschalten → Entity in HA zeigt "unavailable"?
8. Wieder einschalten → Entity zurück "available"?
9. Device Leave → Entities verschwinden aus HA?

### Phase 3 Verifizierung
10. MQTT Discovery deaktiviert → Keine Duplikate in HA?
11. Bridge API via MQTT → Permit Join funktioniert noch?
12. Node-RED kann noch Bridge-Events empfangen?

## Quellen

- [ESPHome 2025.7.0 Changelog - Sub-Device Support](https://esphome.io/changelog/2025.7.0/)
- [ESPHome Native API Component](https://esphome.io/components/api/)
- [ESPHome API Protocol Details](https://developers.esphome.io/architecture/api/protocol_details/)
- [ESPHome api.proto (Protobuf Definitionen)](https://github.com/esphome/esphome/blob/dev/esphome/components/api/api.proto)
- [aioesphomeapi (Python Client für HA)](https://github.com/esphome/aioesphomeapi)
