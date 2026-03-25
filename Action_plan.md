# Action Plan: ZHA Feature Parity

Vergleichsbasis: [Home Assistant ZHA Integration](https://www.home-assistant.io/integrations/zha/)
Erstellt: 2026-02-20 | Reviewed: 2026-02-20

---

## Architektur-Prinzipien

Diese Prinzipien gelten fuer ALLE Aenderungen in diesem Plan:

| Prinzip | Regel |
|---------|-------|
| **Kein God Module** | Max ~500 LOC pro .c Datei. Aufteilen nach Domain. |
| **Kein God Struct** | Tagged Unions statt Fat Structs. Typ-spezifische Daten in Union. |
| **Ein Format** | Eine JSON-Struktur fuer alle Converter-Quellen (z2m, zhaquirks, custom). Konvertierung auf dem Host, nicht auf dem ESP32. |
| **Ein Lookup** | Ein Index, eine Lookup-Funktion, Priority-Feld im Index. Kein Triple-I/O. |
| **Keine Signatur-Aenderungen** | Bestehende fz/tz Signaturen bleiben. Context via Static statt Parameter. |
| **Flags = Verhalten** | Quirk-Flags nur fuer Verhaltensaenderungen. Daten-Transforms gehoeren in die Transform-Pipeline. |
| **Testbar** | Jede neue Komponente muss Host-compilierbar und Unit-testbar sein. |
| **PSRAM-first** | Alles >256 Bytes in PSRAM. LRU-Cache fuer Runtime-Daten. |

---

## Zusammenfassung

| Kategorie | Gaps | Prioritaet |
|-----------|------|------------|
| P1-P3: Foundation (Expose, Generic Attr, fz/tz) | Strukturelles Fundament | **KRITISCH** |
| P4-P6: Mass Import (z2m, zhaquirks, Tuya DP) | 27 vs ~4000+ Geraete | **KRITISCH** |
| P7-P9: Runtime (Quirk Engine, Upload, Custom) | Keine Runtime-Quirks, kein Upload | HOCH |
| Fehlende Entity-Typen | Siren, Device Tracker, Update | HOCH |
| ESPHome Services | Keine HA-Services (permit_join, remove, etc.) | HOCH |
| OTA Provider System | Kein built-in Firmware-Katalog | MITTEL |
| Lock User Code Management | Keine User-Code-Verwaltung | MITTEL |
| Device Reconfigure | Kein Re-Interview/Reconfigure | MITTEL |
| Enhanced Light Transitions | Kein Off-State Color Handling | NIEDRIG |
| Device Signature View | Keine Cluster/Endpoint Inspektion via HA | NIEDRIG |

---

## Ressourcen-Budget

### LittleFS Partition

```
spiffs Partition: 0x921000, Size 0x6DF000 = 6.75 MB
```

| Verbraucher | Aktuell | Nach Plan | Max |
|-------------|---------|-----------|-----|
| State Persistence | ~50KB | ~50KB | 200KB |
| Network Backup | ~10KB | ~10KB | 50KB |
| OTA Images | 0 | ~1MB | 2MB |
| **Converter DB** | **0 → ~2MB** | **~800KB** → ~2MB | **2MB** |
| Custom Quirks | 0 | ~50KB | 200KB |
| **Frei** | **~6.7MB** | **~4.8MB** | — |

### PSRAM Budget (Converter Runtime)

| Verbraucher | Groesse | Notiz |
|-------------|---------|-------|
| LRU Cache (32 Entries) | ~8KB | 256B avg pro gecachter Definition |
| Quirk Data (aktive Devices) | ~2KB | ~200B pro Device mit Quirks, max 10 aktiv |
| Tuya DP Maps (aktive Tuya) | ~1KB | ~100B pro Tuya Device, max 10 aktiv |
| fn_registry (Lookup Tables) | ~1.5KB | 58 fz + 37 tz × 16B |
| **Total Runtime** | **~12.5KB PSRAM** | Gut innerhalb Budget |

### Code Size (Flash .text + .rodata)

| Verbraucher | Aktuell | Nach Plan |
|-------------|---------|-----------|
| Converter System | ~20KB | ~30KB (+10KB) |
| Quirk Engine | 0 | ~3KB |
| Erweiterte fz/tz | — | ~5KB (23 neue fz + 9 neue tz) |
| Custom Quirk Handler | 0 | ~2KB |
| **Total** | **~20KB** | **~40KB** |

---

## Datei-Struktur (Modularitaet)

### Converter System (nach Rewrite)

```
main/zigbee/converter/
├── zb_converter.h                  # Public API (unveraendert)
├── zb_converter_types.h            # Typen: expose_t (tagged union), converter_def_t, quirk_data_t
├── zb_converter_registry.c         # Lookup, Binding, Dispatch (Priority Chain)
├── zb_converter_loader.c           # LittleFS JSON Loader (unified index, LRU cache)
├── zb_converter_loader.h           # Loader API
├── zb_converter_fn_registry.c      # Function Name → Pointer (binary search)
├── zb_converter_fn_registry.h      # Registry API
│
├── std/                            # ← NEU: fz/tz nach Domain aufgeteilt
│   ├── zb_converter_std.c          # Generisch: fz_on_off, fz_brightness, fz_battery, etc. (~20 fn)
│   ├── zb_converter_std.h          # Alle fz/tz Deklarationen (ein Header)
│   ├── zb_converter_std_hvac.c     # Thermostat + Fan: fz_thermostat_*, tz_thermostat_*, fz_fan_mode
│   ├── zb_converter_std_security.c # IAS + Lock: fz_ias_*, fz_lock_*, tz_lock_*, tz_warning
│   ├── zb_converter_std_lighting.c # Color: fz_color_*, tz_color_*, fz_color_enhance_hs
│   ├── zb_converter_std_electrical.c # Power: fz_electrical_*, fz_metering, fz_current, fz_voltage
│   ├── zb_converter_std_tuya.c     # Tuya DP: fz_tuya_dp, tz_tuya_dp (context-aware)
│   └── zb_converter_std_vendor.c   # Vendor: fz_xiaomi_*, fz_aqara_*, fz_ikea_*, fz_philips_*
│
├── converters/                     # Hardcoded Converter Definitionen (unveraendert)
│   ├── conv_generic.c
│   ├── conv_ikea.c
│   ├── conv_xiaomi.c
│   ├── conv_philips.c
│   ├── conv_sonoff.c
│   ├── conv_lidl.c
│   └── conv_tuya_bridge.c
│
├── zb_quirk_engine.c              # ← NEU: Transform Pipeline, Init Sequenzen
├── zb_quirk_engine.h              # ← NEU: Quirk Engine API
├── zb_custom_quirk.c              # ← NEU: Custom Quirk Upload, Validation, Hot-Reload
└── zb_custom_quirk.h              # ← NEU: Custom Quirk API
```

### ESPHome Adapter (aufgeteilt)

```
main/core/adapters/
├── esphome_adapter.c              # Core: Event-Sub, State Dispatch, Device Join/Leave
├── esphome_adapter.h              # Public API (unveraendert)
├── esphome_adapter_internal.h     # ← NEU: Shared State + Helpers fuer Sub-Module
├── esphome_adapter_entities.c     # ← NEU: Capability → Entity Registration
├── esphome_adapter_gateway.c      # ← NEU: Gateway Management Entities (Permit Join, Heal, etc.)
└── esphome_adapter_diagnostics.c  # ← NEU: Per-Device Diagnostics (LQI, RSSI, Signature, Identify)
```

### Host Tools

```
tools/
├── z2m_transpiler.py              # z2m TypeScript → Compact JSON
├── zhaquirks_transpiler.py        # zhaquirks Python → Compact JSON
├── merge_converter_dbs.py         # Merge + Deduplizierung → Unified DB
├── validate_converter_db.py       # Schema-Validierung, fn-Name Check, Coverage-Report
├── upload_converters.py           # MQTT/HTTP Upload auf ESP32
├── ota_provider_update.py         # OTA Image Index aktualisieren
└── converter_schema.json          # JSON Schema fuer Converter-Definitionen
```

### Test-Infrastruktur

```
tests/
├── test_quirk_engine.c            # Unit: Transform Pipeline (Host-compilierbar)
├── test_expose_mapping.c          # Unit: Expose → ESPHome Entity
├── test_converter_dispatch.c      # Unit: fz/tz Dispatch + Priority Chain
├── test_tuya_dp.c                 # Unit: Tuya DP Map Parsing + Conversion
├── Makefile                       # Host-Build (gcc, kein ESP-IDF noetig)
└── tools/
    ├── test_transpiler_z2m.py     # Transpiler Output Validation
    ├── test_transpiler_zhaquirks.py
    └── test_fn_coverage.py        # Prueft ob alle fn-Namen in Registry
```

---

## P1: Erweiterte Expose-Datenstruktur ✅ DONE

### Problem

`zb_expose_t` ist ein flaches Struct. Felder fuer jeden Entity-Typ verschwenden Speicher.
z2m-Exposes haben `property`, `access`, `values[]`, `value_min/max/step` die wir nicht abbilden.

### Loesung: Tagged Union

```c
typedef struct {
    zb_expose_type_t type;      // HA component type
    const char *name;           // Display name suffix (NULL = primary)
    const char *property;       // JSON key name (z.B. "temperature", "local_temperature")
    uint8_t endpoint;           // Endpoint (0 = primary)
    uint8_t access;             // Access flags: 1=STATE, 2=SET, 4=GET
    uint32_t features;          // Feature bitmask (Lights, Covers, etc.)
    const char *device_class;   // HA device_class (NULL = default)
    const char *unit;           // Unit of measurement (NULL = none)
    const char *state_class;    // HA state_class (NULL = none)
    union {
        struct {                        // NUMBER, SENSOR
            float min, max, step;
        } numeric;
        struct {                        // SELECT
            const char **values;
            uint8_t count;
        } select;
        struct {                        // BINARY_SENSOR, SWITCH
            const char *val_on;         // Custom ON label (NULL = default)
            const char *val_off;        // Custom OFF label (NULL = default)
        } binary;
    } ext;                      // Typ-spezifische Extension (0-initialisiert wenn unused)
} zb_expose_t;
```

**Groesse:** 40 Bytes (vs. ~60 Bytes ohne Union) → spart 33% pro Expose.

### Access Flags (von z2m)

| Flag | Wert | Bedeutung | ESPHome Mapping |
|------|------|-----------|-----------------|
| `EA_STATE` | 1 | Read-only | Sensor, Binary Sensor, Text Sensor |
| `EA_SET` | 2 | Write-only | Button |
| `EA_STATE_SET` | 3 | Read + Write | Switch, Number, Select, Light, etc. |
| `EA_GET` | 4 | Pollable | Zusaetzlich: Poll-Button (diagnostic) |
| `EA_ALL` | 7 | Alles | Voller Control |

### JSON Expose Format (einheitlich kompakt)

```json
{"t":2, "n":"Temperature", "p":"temperature", "ac":5, "ep":1,
 "dc":"temperature", "u":"°C", "sc":"measurement",
 "num":{"min":-40.0, "max":80.0, "step":0.1}}

{"t":8, "n":"Power-On Behavior", "p":"power_on_behavior", "ac":7,
 "sel":{"v":["off","on","toggle","previous"]}}

{"t":3, "n":"Contact", "p":"contact", "ac":1,
 "dc":"door", "bin":{"von":"OPEN","voff":"CLOSED"}}
```

Type-spezifische Keys: `"num"` (numeric), `"sel"` (select), `"bin"` (binary).
Felder die nicht gesetzt sind werden weggelassen (JSON sparse).

### Impact auf ESPHome Adapter

| Expose-Feld | ESPHome Entity-Config | Effekt in HA |
|-------------|----------------------|--------------|
| `access` = STATE only | Kein command_callback | Read-only Entity |
| `access` = SET only | `assumed_state = true` | Optimistic UI |
| `property` | State-Key Mapping (JSON → Entity) | Korrekte Zuordnung |
| `ext.numeric.min/max/step` | `number_config.min_value/max_value/step` | Slider-Range |
| `ext.select.values[]` | `select_config.options[]` | Dropdown-Optionen |
| `ext.binary.val_on/off` | Intern: State-String Mapping | Custom Labels |

### Dateien

- `main/zigbee/converter/zb_converter_types.h` — Tagged Union
- `main/zigbee/converter/zb_converter_loader.c` — Neue JSON-Felder parsen (`p`, `ac`, `num`, `sel`, `bin`)
- `main/core/adapters/esphome_adapter_entities.c` — Expose → ESPHome Entity durchreichen
- `main/zigbee/converter/converters/conv_*.c` — Hardcoded Converter anpassen
- `tools/converter_schema.json` — JSON Schema

**Aufwand:** 2 Tage

---

## P2: Generischer Attribut-Converter (Catch-All) ✅ DONE

Universeller Converter der anhand von ZCL Data Type automatisch konvertiert.
Deckt sofort ~500+ Devices ab die nur Standard-ZCL-Attribute nutzen.

```c
// fz_generic_attr: Cluster+Attr+DataType → JSON
esp_err_t fz_generic_attr(const void *raw, size_t len, cJSON *json, const char *key);

// tz_generic_write_attr: JSON → ZCL Write Attribute
esp_err_t tz_generic_write_attr(uint16_t short_addr, uint8_t ep, const cJSON *value);
```

ZCL Data Type Mapping:
- 0x10 (boolean) → bool
- 0x20 (uint8) → int
- 0x21 (uint16) → int
- 0x28 (int8) → int
- 0x29 (int16) → float/100 (Temperature/Humidity Convention)
- 0x39 (float) → float
- 0x42 (string) → string

**Dateien:**
- `main/zigbee/converter/std/zb_converter_std.c`
- `main/zigbee/converter/zb_converter_fn_registry.c`

**Aufwand:** 1 Tag

---

## P3: Erweiterte fz/tz Standard-Library ✅ DONE

### Modularitaets-Regel

Alle neuen fz/tz Funktionen werden nach Domain in separate Dateien aufgeteilt.
**Ein Header** (`zb_converter_std.h`), **mehrere Implementierungen** (`std/*.c`).

### Neue Funktionen nach Datei

#### `std/zb_converter_std_hvac.c` (~8 Funktionen)

| Funktion | Cluster | z2m Equivalent |
|----------|---------|----------------|
| `fz_thermostat_setpoint` | 0x0201 | `fz.thermostat` |
| `fz_thermostat_weekly_schedule` | 0x0201 | `fz.thermostat_weekly_schedule` |
| `fz_thermostat_running_state` | 0x0201 | (existiert schon, hierher verschieben) |
| `fz_thermostat_system_mode` | 0x0201 | (existiert schon, hierher verschieben) |
| `tz_thermostat_setpoint` | 0x0201 | (existiert schon als tz_thermostat_setpoint) |
| `tz_thermostat_system_mode` | 0x0201 | `tz.thermostat_system_mode` |
| `tz_thermostat_weekly_schedule` | 0x0201 | `tz.thermostat_weekly_schedule` |
| `tz_fan_mode` | 0x0202 | (existiert schon, hierher verschieben) |

#### `std/zb_converter_std_security.c` (~8 Funktionen)

| Funktion | Cluster | z2m Equivalent |
|----------|---------|----------------|
| `fz_ias_zone_enrollment` | 0x0500 | `fz.ias_zone_enrolled` |
| `fz_ias_wd` | 0x0502 | `fz.ias_wd` |
| `fz_door_lock_user_status` | 0x0101 | `fz.lock_user_status` |
| `fz_door_lock_pin_code` | 0x0101 | `fz.lock_pin_code` |
| `fz_door_lock_event` | 0x0101 | `fz.lock_operation_event` |
| `tz_warning` | 0x0502 | `tz.warning` |
| `tz_door_lock_pin` | 0x0101 | `tz.pincode_lock` |
| `tz_ias_zone_enroll_response` | 0x0500 | `tz.ias_zone_enroll` |

#### `std/zb_converter_std_lighting.c` (~2 Funktionen)

| Funktion | Cluster | z2m Equivalent |
|----------|---------|----------------|
| `fz_color_enhance_hs` | 0x0300 | `fz.color_colortemp` (enhanced) |
| `tz_effect` | 0x0003 | `tz.effect` |

#### `std/zb_converter_std_electrical.c` (~3 Funktionen)

| Funktion | Cluster | z2m Equivalent |
|----------|---------|----------------|
| `fz_electrical_frequency` | 0x0B04 | custom |
| `fz_diagnostics_rssi` | 0x0B05 | custom |
| `fz_diagnostics_lqi` | 0x0B05 | custom |

#### `std/zb_converter_std_tuya.c` (~2 Funktionen)

| Funktion | Cluster | z2m Equivalent |
|----------|---------|----------------|
| `fz_tuya_dp` | 0xEF00 | universeller Tuya DP Parser |
| `tz_tuya_dp` | 0xEF00 | universeller Tuya DP Writer |

**Tuya Context-Problem geloest via Static Context:**

```c
// In zb_converter_registry.c — VOR fz/tz Dispatch:
static _Thread_local const zb_converter_def_t *s_dispatch_context = NULL;

void zb_converter_set_dispatch_context(const zb_converter_def_t *conv) {
    s_dispatch_context = conv;
}
const zb_converter_def_t *zb_converter_get_dispatch_context(void) {
    return s_dispatch_context;
}

// In handle_report():
zb_converter_set_dispatch_context(conv);
ret = conv->from_zigbee[i].convert(raw, len, json, key);
zb_converter_set_dispatch_context(NULL);

// In fz_tuya_dp():
const zb_converter_def_t *conv = zb_converter_get_dispatch_context();
// Lese tuya_dp_map aus conv → kein globaler State, keine Signatur-Aenderung
```

#### `std/zb_converter_std_vendor.c` (~7 Funktionen)

| Funktion | Cluster | Hersteller |
|----------|---------|------------|
| `fz_aqara_opple` | 0xFCC0 | Aqara |
| `tz_aqara_opple` | 0xFCC0 | Aqara |
| `fz_ikea_air_purifier` | 0xFC7D | IKEA |
| `fz_ikea_voc_index` | 0xFC7D | IKEA |
| `fz_philips_hue_motion` | 0xFC00 | Philips |
| `tz_identify` | 0x0003 | Generic |
| `tz_identify_effect` | 0x0003 | Generic |

#### Uebrige in `std/zb_converter_std.c` (~6 Funktionen)

| Funktion | Cluster | z2m Equivalent |
|----------|---------|----------------|
| `fz_pm25` | 0x042A | `fz.pm25` |
| `fz_soil_moisture` | 0x0408 | `fz.soil_moisture` |
| `fz_device_temperature` | 0x0002 | `fz.device_temperature` |
| `fz_multistate_input` | 0x0012 | `fz.multistate_input` |
| `fz_analog_input` | 0x000C | `fz.analog_input` |
| `fz_generic_attr` | beliebig | Catch-all (P2) |

### Ergebnis

| Metrik | Vorher | Nachher |
|--------|--------|---------|
| fz Funktionen | 35 | 58+ |
| tz Funktionen | 14 | 37+ |
| Dateien | 1 (zb_converter_std.c) | 7 (std/*.c) |
| Max LOC/Datei | ~800 | ~300 |

**Aufwand:** 3-4 Tage (inkl. bestehende Funktionen verschieben)

---

## P4: z2m Transpiler Tool ✅ DONE

### Eingabe / Ausgabe

```
Eingabe:  zigbee-herdsman-converters/src/devices/*.ts (NPM)
Ausgabe:  converters/index.json + converters/<manufacturer>.json
Format:   Einheitliches kompaktes JSON (identisch fuer z2m, zhaquirks, custom)
```

### Unified Index Format

**Ein Index, ein Lookup, Priority im Index:**

```json
{
  "version": 2,
  "sources": {
    "z2m": {"ts": "2026-02-20", "commit": "abc123"},
    "zhaquirks": {"ts": "2026-02-20", "commit": "def456"}
  },
  "manufacturers": {
    "LUMI": {"file": "lumi.json", "count": 250},
    "IKEA of Sweden": {"file": "ikea.json", "count": 200},
    "_TZE200_*": {"file": "tuya.json", "count": 800}
  },
  "total_devices": 4200
}
```

**Manufacturer JSON (ein Device pro Eintrag):**

```json
[
  {
    "m": "LED1545G12",
    "mf": "IKEA of Sweden",
    "v": "IKEA",
    "d": "TRADFRI LED bulb E26/E27 980 lumen",
    "src": "z2m",
    "pri": 0,
    "e": [
      {"t":0, "p":"state", "ac":3, "f":3}
    ],
    "fz": [
      {"c":6, "a":0, "k":"state", "fn":"fz_on_off"},
      {"c":8, "a":0, "k":"brightness", "fn":"fz_brightness"},
      {"c":768, "a":7, "k":"color_temp", "fn":"fz_color_temp"}
    ],
    "tz": [
      {"k":"state", "c":6, "fn":"tz_on_off"},
      {"k":"brightness", "c":8, "fn":"tz_brightness"},
      {"k":"color_temp", "c":768, "fn":"tz_color_temp"}
    ],
    "q": 0
  }
]
```

**Priority (`"pri"`):** 0=custom (hoechste), 1=zhaquirks, 2=z2m, 3=hardcoded.
Lookup: Lade Manufacturer-File, iterate, nehme Eintrag mit niedrigstem `pri`.

### Transpiler-Schritte

1. z2m als NPM Dependency clonen
2. Device-Definitionen parsen (TypeScript AST oder `--export-json` wenn verfuegbar)
3. Pro Device: exposes → `zb_expose_t[]`, fromZigbee → fn-Name Mapping, toZigbee → analog
4. **Kompatibilitaets-Filter**: Nur emittieren wenn ALLE fz/tz in `zb_fn_registry` existieren ODER auf `fz_generic_attr` abbildbar
5. Statistik: importiert vs. uebersprungen (mit Grund)

### Kompatibilitaets-Filter Detail

```python
def is_compatible(device):
    for fz in device.from_zigbee:
        fn_name = map_z2m_fz_to_esp32(fz)
        if fn_name not in ESP32_FZ_REGISTRY and fn_name != "fz_generic_attr":
            return False, f"Missing fz: {fz.name} -> {fn_name}"
    for tz in device.to_zigbee:
        fn_name = map_z2m_tz_to_esp32(tz)
        if fn_name not in ESP32_TZ_REGISTRY and fn_name != "tz_generic_write_attr":
            return False, f"Missing tz: {tz.name} -> {fn_name}"
    return True, "OK"
```

**Aufwand:** 3-5 Tage
**Ergebnis:** ~2000-3000 Devices

---

## P5: zhaquirks Transpiler ✅ DONE

### Ergebnis

| Quelle | Devices | Hersteller |
|--------|---------|------------|
| z2m Transpiler | 4,144 | 431 |
| zhaquirks Transpiler | 1,074 | 562 |
| **Merged Total** | **4,761** | **916** |

### Bekannte Edge Cases (nicht transpiliert)

#### z2m Transpiler (~600 fehlende Devices)
| Kategorie | ~Count | Grund | Loesung |
|-----------|--------|-------|---------|
| Complex JS logic in extends | ~200 | Runtime-berechnete Exposes (z.B. `if (appVersion >= 100)`) | Braeuchte JS-Interpreter |
| Deeply nested extend chains | ~150 | `modernExtend` chains mit 4+ Level Vererbung | Bessere Chain-Aufloesung |
| Dynamic model matching | ~100 | Regex-Modelle wie `TS011F_*` mit Runtime-Fingerprinting | Wildcard-Matching in Loader |
| Template literal models | ~50 | `` `${prefix}_something` `` als Model-String | Template-Auswertung |
| Platform-specific code | ~100 | Node.js-only Features (fs, path, etc.) | Nicht anwendbar |

#### zhaquirks Transpiler (~400 fehlende Definitions)
| Kategorie | ~Count | Grund | Loesung |
|-----------|--------|-------|---------|
| Mfr-specific clusters only | ~120 | Devices mit 0xFF01/0xFCC0 etc. ohne Standard-Cluster-Mapping | Vendor-spezifische fz/tz |
| Remote controls (output clusters) | ~40 | Funktionale Cluster nur in OUTPUT_CLUSTERS | Output-Cluster-zu-fz Mapping |
| MODELS_INFO in base class | ~30 | Vererbung aus abstrakter Basisklasse statt in jedem File | AST Inheritance traversal |
| Multi-quirk files ohne MODEL | ~20 | Kein MODELS_INFO, nur `class.signature` matching | Signatur-basiertes Matching |
| XBee/Serial protocol devices | ~10 | Keine ZCL-basierten Devices | Nicht anwendbar |
| Complex __init__.py exports | ~20 | Re-exports und bedingte Klassen-Registrierung | Erweiterte Import-Aufloesung |
| V2 entity methods without cluster | ~50 | `.number()`, `.sensor()` auf Mfr-Cluster ohne Mapping | Mfr-Cluster-DB |
| Clone chains mit Transforms | ~10 | `.clone().replaces(...)` mit Cluster-Ersetzungen | Erweiterte Clone-Engine |

### Was zhaquirks liefert

[zha-device-handlers](https://github.com/zigpy/zha-device-handlers): ~1500+ Python Quirks.
Quirks ueberschreiben Cluster-Verhalten fuer nicht-konforme Geraete.

### Transpiler: `tools/zhaquirks_transpiler.py`

```
Eingabe:  zhaquirks/zhaquirks/*/  (Python Quirk-Dateien)
Ausgabe:  Merged in dieselben converters/*.json Dateien (mit pri=1)
```

**Schritte:**
1. zhaquirks als Python-Paket importieren (oder AST parsen)
2. Alle Quirk-Klassen finden (erben von `CustomDevice`)
3. Pro Quirk:
   - `signature.MODELS_INFO` → manufacturer + model
   - `replacement.ENDPOINTS` → Cluster-Overrides extrahieren
   - Custom Cluster-Klassen → Attribute-Mapping, Value-Transforms
   - `device_automation_triggers` → Action-Events
4. In dasselbe JSON-Format konvertieren wie P4, mit `"pri": 1`
5. Quirks-spezifische Felder in `"quirks"` Section:

```json
{
  "m": "lumi.sensor_magnet.aq2",
  "mf": "LUMI",
  "src": "zhaquirks",
  "pri": 1,
  "e": [{"t":3, "p":"contact", "ac":1, "dc":"door"}],
  "fz": [
    {"c":1, "a":33, "k":"battery", "fn":"fz_battery"},
    {"c":0, "a":65281, "k":"_xiaomi_raw", "fn":"fz_xiaomi_ff01", "mf":4447}
  ],
  "tz": [],
  "q": 64,
  "quirks": {
    "bind": [1, 1280],
    "rpt": [{"c":1, "a":33, "min":3600, "max":62000, "chg":1}],
    "init": [{"op":"read_attr", "c":0, "a":[4,5,7]}],
    "transforms": {"battery": {"scale":0.5, "min":0, "max":100}}
  }
}
```

### Merge-Tool: `tools/merge_converter_dbs.py`

```
1. z2m-Output laden (pri=2)
2. zhaquirks-Output laden (pri=1)
3. Pro manufacturer+model:
   - Nur in z2m → as-is
   - Nur in zhaquirks → as-is
   - In BEIDEN → z2m-Definition behalten, "quirks" Section von zhaquirks anfuegen
4. Custom Quirks (pri=0) bleiben separat auf dem Device (LittleFS /converters/custom/)
```

### Quirk-Kategorien Abdeckung

| Kategorie | ~Count | Methode |
|-----------|--------|---------|
| Aqara/Xiaomi Proprietary | ~200 | `fz_xiaomi_ff01` + `fz_aqara_opple` |
| Tuya DP-based | ~400 | `fz_tuya_dp` + DP-Map (P6) |
| IKEA Custom Clusters | ~30 | Dedizierte fz/tz |
| Standard ZCL Overrides | ~300 | `fz_generic_attr` + transforms |
| Complex Multi-Endpoint | ~100 | Endpoint-aware fz/tz |
| Custom Commands | ~200 | Spaeter (Command-Converter Framework) |
| **Transpilierbar gesamt** | **~1100-1200** | |

**Aufwand:** 4-5 Tage

---

## P6: Tuya DP Mapping Engine ✅ DONE

Tuya-Geraete nutzen Cluster 0xEF00 mit Datapoints statt Standard-ZCL.

### DP Map im Converter JSON

```json
{
  "m": "_TZE200_auin8mzr",
  "mf": "_TZ3000_...",
  "src": "z2m",
  "pri": 2,
  "tuya_dp": {
    "1":  {"k":"state", "t":"bool"},
    "2":  {"k":"system_mode", "t":"enum", "v":{"0":"auto","1":"heat","2":"off"}},
    "16": {"k":"current_temperature", "t":"int", "scale":0.1},
    "24": {"k":"target_temperature", "t":"int", "scale":0.5, "min":5, "max":35}
  },
  "e": [
    {"t":6, "p":"system_mode", "ac":3},
    {"t":2, "p":"current_temperature", "ac":1, "dc":"temperature", "u":"°C"}
  ],
  "fz": [{"c":61184, "a":65535, "k":"_tuya", "fn":"fz_tuya_dp"}],
  "tz": [{"k":"_tuya", "c":61184, "fn":"tz_tuya_dp"}],
  "q": 16
}
```

### Implementierung

`fz_tuya_dp` und `tz_tuya_dp` nutzen den Static Dispatch Context (P3) um die DP-Map zu lesen.
Die DP-Map wird beim Converter-Load als `cJSON*` im LRU-Cache gehalten (kein separates Caching noetig).

**Dateien:**
- `main/zigbee/converter/std/zb_converter_std_tuya.c`
- `main/zigbee/converter/zb_converter_loader.c` — `tuya_dp` Feld parsen
- `tools/z2m_transpiler.py` + `tools/zhaquirks_transpiler.py` — Tuya DP Extraktion

**Aufwand:** 2-3 Tage

---

## P7: Quirk Runtime Engine ✅ DONE

### Bereinigte Quirk Flags

**Prinzip:** Flags nur fuer Verhaltensaenderungen. Daten-Transforms in die Transform-Pipeline.

```c
// BEHALTEN (Verhalten, nicht als Transform abbildbar):
#define ZB_QUIRK_NONE                0
#define ZB_QUIRK_NO_DEFAULT_RESPONSE (1 << 0)  // ZCL Protokoll
#define ZB_QUIRK_INVERT_COVER        (1 << 1)  // Cover 0=open vs 0=closed
#define ZB_QUIRK_SWAP_COLOR_XY       (1 << 2)  // X/Y vertauscht
#define ZB_QUIRK_NEEDS_CONFIGURE     (1 << 3)  // Extra Config nach Join
#define ZB_QUIRK_TUYA_DEVICE         (1 << 4)  // Tuya DP Routing
#define ZB_QUIRK_XIAOMI_INIT         (1 << 5)  // Xiaomi Quick-Init
#define ZB_QUIRK_TUYA_MCU_INIT       (1 << 6)  // Tuya MCU Handshake
#define ZB_QUIRK_CUSTOM_BIND         (1 << 7)  // quirks.bind[] statt Auto-Bind
#define ZB_QUIRK_CUSTOM_REPORTING    (1 << 8)  // quirks.rpt[] statt Default
#define ZB_QUIRK_MULTI_ENDPOINT      (1 << 9)  // Endpoint-aware Routing

// ENTFERNT (jetzt via Transform-Pipeline):
// ZB_QUIRK_BATTERY_HALF       → transforms: {"battery": {"scale": 0.5}}
// ZB_QUIRK_TEMPERATURE_X10    → transforms: {"temperature": {"scale": 10.0}}
// ZB_QUIRK_INVERT_BOOL        → transforms: {"contact": {"invert": true}}
// ZB_QUIRK_NO_OCCUPANCY_TIMEOUT → expose: {"auto_clear": false}
```

### Quirk Data Structure

```c
typedef struct {
    float scale;            // Multiplikator (default 1.0)
    float offset;           // Addend (default 0.0)
    float min, max;         // Clamp range
    bool has_min, has_max;
    bool invert;            // Boolean invertieren
} quirk_transform_t;

typedef struct {
    uint8_t op;             // QUIRK_OP_READ_ATTR, _WRITE_ATTR, _SEND_CMD
    uint16_t cluster_id;
    uint16_t attr_or_cmd_id;
    uint8_t data[8];
    uint8_t data_len;
} quirk_init_step_t;

typedef struct {
    // Transforms: key → transform (max 8 per device, PSRAM)
    struct { char key[16]; quirk_transform_t t; } transforms[8];
    uint8_t transform_count;

    // Custom bind clusters
    uint16_t bind_clusters[8];
    uint8_t bind_cluster_count;

    // Custom reporting
    zb_reporting_override_t rpt[8];
    uint8_t rpt_count;

    // Init sequence
    quirk_init_step_t init_steps[8];
    uint8_t init_step_count;
} quirk_data_t;
// Groesse: ~280 Bytes (PSRAM, nur fuer Devices mit Quirks)
```

### Transform Pipeline

In `zb_converter_registry.c` nach jedem fz-Dispatch:

```c
// Nach fz_* Konvertierung:
if (conv->quirk_data && conv->quirk_data->transform_count > 0) {
    quirk_apply_transforms(conv->quirk_data, json);
}
```

```c
// In zb_quirk_engine.c:
void quirk_apply_transforms(const quirk_data_t *qd, cJSON *json) {
    for (int i = 0; i < qd->transform_count; i++) {
        cJSON *val = cJSON_GetObjectItem(json, qd->transforms[i].key);
        if (!val) continue;
        const quirk_transform_t *t = &qd->transforms[i].t;
        if (cJSON_IsNumber(val)) {
            double v = (val->valuedouble * t->scale) + t->offset;
            if (t->has_min && v < t->min) v = t->min;
            if (t->has_max && v > t->max) v = t->max;
            cJSON_SetNumberValue(val, v);
        }
        if (t->invert && cJSON_IsBool(val)) {
            cJSON_SetBoolValue(val, !cJSON_IsTrue(val));
        }
    }
}
```

### Init-Sequenzen (bei Device Join)

In `zb_interview.c` nach Interview + Bind + Reporting:

```c
if (conv->quirk_data && conv->quirk_data->init_step_count > 0) {
    quirk_execute_init_sequence(dev->short_addr, dev->endpoint,
                                conv->quirk_data);
}
```

**Dateien:**
- `main/zigbee/converter/zb_quirk_engine.c` — Transform Pipeline + Init Engine
- `main/zigbee/converter/zb_quirk_engine.h` — API
- `main/zigbee/converter/zb_converter_types.h` — Bereinigte Flags + quirk_data_t
- `main/zigbee/converter/zb_converter_registry.c` — Post-fz Transform Hook
- `main/zigbee/converter/zb_converter_loader.c` — quirks JSON parsen
- `main/zigbee/zb_interview.c` — Custom Bind/Reporting/Init

**Aufwand:** 3-4 Tage

---

## P8: Converter DB Upload ✅ DONE (Build-Time)

### Upload-Mechanismen

1. **Build-Time Embedding** (primaer):
   - `tools/merge_converter_dbs.py` → Output in `data/converters/`
   - CMake: `spiffs_create_partition_image(spiffs data FLASH_IN_PROJECT)`
   - Converter DB wird bei jedem Firmware-Flash aktualisiert

2. **MQTT Upload** (Runtime-Update):
   - `bridge/request/converter/upload` — Chunked Base64 Transfer
   - `zb_converter_loader_reload_index()` nach Upload

3. **HTTP Upload** (optionaler Captive Portal Endpoint):
   - `/api/converters` — Multipart Upload
   - Fuer grosse Updates (>100KB)

4. **CLI Tool**: `tools/upload_converters.py` — MQTT oder HTTP

**Dateien:**
- `main/mqtt/mqtt_bridge.c` — Upload Request Handler
- `tools/upload_converters.py` — CLI Tool

**Aufwand:** 2 Tage

---

## P9: Custom Community Quirks

### Anforderungen

1. User kann eigene Device-Definitionen erstellen (JSON, kein Recompile)
2. Community kann Quirks teilen (Copy-Paste JSON)
3. Override-Kette: Custom (`pri=0`) > zhaquirks (`pri=1`) > z2m (`pri=2`) > Hardcoded > Generic
4. Hot-Reload mit ESPHome Reconnect (siehe unten)
5. Validierung beim Upload

### Custom Quirks nutzen DASSELBE kompakte JSON-Format

Kein zweites "user-freundliches" Format. Stattdessen:

```
User schreibt JSON (compact) → tools/validate_converter_db.py validiert
                              → Upload via MQTT/HTTP
                              → Gespeichert als /converters/custom/{name}.json
```

Fuer User-Freundlichkeit: `tools/validate_converter_db.py` gibt verstaendliche Fehlermeldungen:

```
$ python tools/validate_converter_db.py my_device.json
ERROR: from_zigbee[0].fn = "fz_foobar" — unknown function.
  Available functions: fz_on_off, fz_brightness, fz_temperature, ...
  Did you mean: fz_battery?
```

### Unified Lookup (kein Triple-I/O)

**Ein Lookup, ein Index, Priority-Feld:**

```c
const zb_converter_def_t *zb_converter_find(const char *manufacturer, const char *model) {
    // 1. Lade Manufacturer-File aus LittleFS (gecached im LRU)
    //    File enthaelt ALLE Quellen (z2m + zhaquirks, sorted by pri)
    const zb_converter_def_t *def = zb_converter_loader_find(manufacturer, model);
    // → Loader returned automatisch den Eintrag mit niedrigstem pri

    // 2. Check Custom Quirks (separate Dateien, immer pri=0)
    const zb_converter_def_t *custom = zb_custom_quirk_find(manufacturer, model);
    if (custom) return custom;  // Custom hat immer Vorrang

    if (def) return def;

    // 3. Hardcoded Fallback
    def = zb_converter_find_hardcoded(manufacturer, model);
    if (def) return def;

    // 4. Generic Fallback
    return NULL;  // caller uses conv_generic_for_capabilities()
}
```

### Hot-Reload mit ESPHome Reconnect

**Kritisches Detail:** ESPHome-Protokoll sendet Entity-Liste beim Connect.
Danach ist die Liste fix. Wenn ein Quirk die Exposes aendert, muss HA reconnecten.

```c
void custom_quirk_hot_reload(const char *manufacturer, const char *model) {
    // 1. Finde aktive Devices mit diesem manufacturer+model
    // 2. Re-Bind Converter (neues Custom Quirk hat pri=0)
    // 3. Reconfigure Reporting (falls quirks.rpt definiert)
    // 4. Falls Exposes geaendert:
    //    a) Entities de-registrieren
    //    b) Neue Entities registrieren
    //    c) ESPHome API Client disconnect erzwingen
    //       → Client reconnected automatisch
    //       → Bekommt neue ListEntitiesResponse
    esphome_api_disconnect_all_clients();  // ← WICHTIG
}
```

### Upload-Mechanismen

```
MQTT:   bridge/request/converter/custom/add    → {"name": "...", "definition": {...}}
        bridge/request/converter/custom/remove  → {"name": "..."}
        bridge/request/converter/custom/list    → Response: [...]
```

**Dateien:**
- `main/zigbee/converter/zb_custom_quirk.c` — Upload, Validation, Hot-Reload
- `main/zigbee/converter/zb_custom_quirk.h` — API
- `main/zigbee/converter/zb_converter_registry.c` — Unified Lookup
- `main/esphome/esphome_api_server.c` — `esphome_api_disconnect_all_clients()`
- `main/mqtt/mqtt_bridge.c` — MQTT Request Handlers
- `tools/validate_converter_db.py` — Schema + fn-Name Validation

**Aufwand:** 3-4 Tage

---

## Fehlende Entity-Typen

### Siren Entity

ESPHome hat keinen Siren-Typ → Switch mit `icon: "mdi:alarm-bell"`.

1. `DEV_CAP_SIREN` in `unified_device.h`
2. `fz_ias_wd` + `tz_warning` in `std/zb_converter_std_security.c`
3. ESPHome Adapter: `DEV_CAP_SIREN` → Switch + optional Number (Duration) + Select (Mode)

**Aufwand:** 1 Tag

### Update Entity

ESPHome 2025.x Update-Entity-Support pruefen (`ListEntitiesUpdateResponse` msg_type 116).
Falls vorhanden: nativ implementieren. Sonst: Composite (TextSensor + Button + Sensor).

**Aufwand:** 2-3 Tage

### Device Tracker

Abbildung als Binary Sensor `device_class: presence`. Optional via `CONFIG_ZIGBEE_DEVICE_TRACKER=n`.

**Aufwand:** 0.5 Tage

---

## ESPHome Gateway & Device Management ✅ Gateway Entities DONE

### Gateway Management Entities (device_id=0)

| Entity | Typ | Funktion |
|--------|-----|----------|
| Permit Join | Switch | Pairing-Fenster |
| Permit Join Duration | Number | Dauer (5-254s) |
| Remove Device | Select + Button | Device auswaehlen + Remove |
| Network Heal | Button | Full network heal |
| Zigbee Channel | Sensor | Aktueller Channel (diagnostic) |
| Device Count | Sensor | Gepairte Geraete (diagnostic) |
| Network PAN ID | Text Sensor | PAN ID hex (diagnostic) |

**Datei:** `main/core/adapters/esphome_adapter_gateway.c`

### Per-Device Diagnostic Entities (entity_category: DIAGNOSTIC) ✅ DONE

| Entity | Typ | Funktion |
|--------|-----|----------|
| Remove | Button | Device entfernen |
| Reconfigure | Button | Re-interview |
| Identify | Button | Identify-Effekt |
| LQI | Sensor | Link Quality |
| RSSI | Sensor | Signal Strength |
| Last Seen | Text Sensor | Timestamp |
| Zigbee Info | Text Sensor | Device Signature JSON (disabled_by_default) |

**Datei:** `main/core/adapters/esphome_adapter_diagnostics.c`

**Aufwand:** 3 Tage (Gateway + Per-Device)

---

## Device Reconfigure

```c
// In zb_interview.c:
esp_err_t zb_interview_reconfigure(esp_zb_ieee_addr_t ieee_addr);
```

Ablauf: ZDO Endpoint Query → Simple Descriptor → Converter Re-Match → Reporting → Bindings → Event.
Trigger: Button Entity (ESPHome) + MQTT (`bridge/request/device/reconfigure`).

**Aufwand:** 1 Tag

---

## Lock User Code Management

ZCL Door Lock PIN Commands (0x0101):
- `tz_door_lock_set_pin` (cmd 0x05)
- `tz_door_lock_clear_pin` (cmd 0x07)
- `fz_door_lock_pin_response` (cmd 0x06 response)
- `fz_door_lock_event` (attr 0x0041/0x0042)

ESPHome: Number (User Slot) + Text (PIN) + Buttons (Set/Clear).
MQTT: `{"pin_code": {"user": 1, "pin_code": "1234"}}`.

**Aufwand:** 2 Tage

---

## OTA Provider System

Host-Tool (`tools/ota_provider_update.py`) fragt Hersteller-Endpunkte ab:
- Koenkk/zigbee-OTA (Aggregat), IKEA, Ledvance, Sonoff, Third Reality

Output: `/ota/index.json` + OTA Images auf LittleFS.
ESP32 matcht `QueryNextImageRequest` gegen Index.

**Aufwand:** 3-4 Tage

---

## Enhanced Light Transitions

1. **Off-State Pre-Set**: Color/Temp setzen VOR ON (verhindert "flash of wrong color")
2. **Default Transition**: `CONFIG_ZIGBEE_DEFAULT_TRANSITION_TIME=400` (ms)
3. **Optimistic Group State**: Nach Group-Command Members sofort updaten

**Aufwand:** 1 Tag

---

## Test-Strategie

### Host-compilierbare Unit-Tests (`tests/`)

```makefile
# tests/Makefile
CC = gcc
CFLAGS = -I../main -DTEST_BUILD -DESP_OK=0
TESTS = test_quirk_engine test_expose_mapping test_converter_dispatch test_tuya_dp

test_quirk_engine: test_quirk_engine.c ../main/zigbee/converter/zb_quirk_engine.c
	$(CC) $(CFLAGS) -o $@ $^ -lcjson && ./$@
```

**Was getestet wird:**
- `test_quirk_engine.c` — Transform Pipeline (scale, offset, clamp, invert)
- `test_expose_mapping.c` — Expose → ESPHome Entity (access flags, tagged union)
- `test_converter_dispatch.c` — Lookup Priority Chain, fz/tz Dispatch
- `test_tuya_dp.c` — DP Map Parsing, Conversion

### Python-Tests (`tests/tools/`)

- `test_transpiler_z2m.py` — Validiert z2m Transpiler Output gegen Schema
- `test_transpiler_zhaquirks.py` — Validiert zhaquirks Transpiler Output
- `test_fn_coverage.py` — Prueft ob alle fn-Namen im JSON auch in der C fn_registry existieren
- `test_merge.py` — Priority-Merge Korrektheit

### Schema-Validierung

`tools/converter_schema.json` (JSON Schema Draft 7):

```json
{
  "type": "array",
  "items": {
    "type": "object",
    "required": ["m", "mf", "e", "fz"],
    "properties": {
      "m": {"type": "string"},
      "mf": {"type": "string"},
      "pri": {"type": "integer", "minimum": 0, "maximum": 3},
      "e": {"type": "array", "items": {"$ref": "#/definitions/expose"}},
      "fz": {"type": "array", "items": {"$ref": "#/definitions/from_zigbee"}},
      "tz": {"type": "array", "items": {"$ref": "#/definitions/to_zigbee"}},
      "q": {"type": "integer"},
      "quirks": {"$ref": "#/definitions/quirk_data"},
      "tuya_dp": {"type": "object"}
    }
  }
}
```

### CI-Integration (Zukunft)

```yaml
# .github/workflows/ci.yml
- run: make -C tests               # Host Unit Tests
- run: cd tests/tools && pytest     # Python Tests
- run: python tools/validate_converter_db.py data/converters/  # Schema Check
- run: idf.py build                 # ESP-IDF Build
```

---

## Priorisierte Reihenfolge

| # | Phase | Abhaengigkeit | Aufwand | Ergebnis |
|---|-------|---------------|---------|----------|
| 0 | **P1**: Expose-Erweiterung | — | 2d | Tagged Union, property, access, min/max, values[] |
| 1 | **P2**: Generic Attr Converter | — | 1d | ~500 mehr Devices sofort |
| 2 | **P3**: fz/tz Library Split | — | 3-4d | 58+ fz, 37+ tz, 7 Dateien statt 1 |
| 3 | **P4**: z2m Transpiler | P1, P3 | 3-5d | ~3000 Devices |
| 4 | **P5**: zhaquirks Transpiler | P4 | 4-5d | +1200 Devices, Quirk-Overrides |
| 5 | **P6**: Tuya DP Engine | P3, P4 | 2-3d | +400 Tuya Devices |
| 6 | **P7**: Quirk Runtime Engine | P5 | 3-4d | Transforms, Init, Custom Bind/Report |
| 7 | **P8**: Converter DB Upload | P4 | 2d | MQTT/HTTP Upload + Build-Time Embed |
| 8 | **P9**: Custom Community Quirks | P7, P8 | 3-4d | User Defs + Hot-Reload + Validation |
| 9 | ESPHome Gateway Entities | — | 2d | Permit Join, Heal, Device Count, etc. |
| 10 | Siren Entity | P3 (9A) | 1d | IAS WD als Switch |
| 11 | Device Reconfigure | — | 1d | Re-Interview Button |
| 12 | Per-Device Diagnostics | #11 | 1d | LQI, RSSI, Identify, Remove, Signature |
| 13 | Lock User Codes | P3 | 2d | PIN Set/Clear/Enable/Disable |
| 14 | Update Entity | — | 2-3d | OTA als HA Entity |
| 15 | OTA Provider System | #14 | 3-4d | Auto-Image-Discovery |
| 16 | Enhanced Light Transitions | — | 1d | Off-State Pre-Set, Default Transition |
| 17 | Device Tracker | — | 0.5d | Presence Binary Sensor |
| 18 | **Tests** | P1-P7 | 2d | Unit Tests + Schema Validation + CI |

**Gesamt: ~39-52 Arbeitstage**

### Meilensteine

| Meilenstein | Phases | Ergebnis |
|-------------|--------|----------|
| **M1: Foundation** (Woche 1-2) | P1, P2, P3 | Tagged Union Exposes, 58+ fz/37+ tz in 7 Dateien, Generic Attr |
| **M2: Mass Import** (Woche 3-5) | P4, P5, P6, Tests | ~4000+ Devices in Unified JSON DB |
| **M3: Runtime** (Woche 6-7) | P7, P8, P9 | Quirk Engine + Upload + Custom Quirks + Hot-Reload |
| **M4: UX Parity** (Woche 8-9) | #9-#13 | Gateway Mgmt, Siren, Reconfigure, Diagnostics, Lock Codes |
| **M5: Advanced** (Woche 10-11) | #14-#17 | Update Entity, OTA Provider, Transitions, Tracker |

---

## Architektur-Diagramm

```
  zigbee-herdsman-converters          zha-device-handlers
  (NPM/TypeScript)                    (Python/zhaquirks)
          │                                    │
          ▼                                    ▼
  tools/z2m_transpiler.py          tools/zhaquirks_transpiler.py
          │                                    │
          └──────────┐    ┌────────────────────┘
                     ▼    ▼
              tools/merge_converter_dbs.py
              (Deduplizierung, pri=2 z2m, pri=1 zhaquirks)
                        │
                        ▼
              tools/validate_converter_db.py
              (Schema + fn-Name Check)
                        │
                        ▼
              data/converters/ (Host)
              ├── index.json             (Unified Index mit Priority)
              ├── ikea.json              (z2m + zhaquirks merged)
              ├── lumi.json              (~250 devices, mixed sources)
              ├── tuya.json              (~800 devices, mit tuya_dp maps)
              └── ...                    (~4000 total)
                        │
       ┌────────────────┼────────────────┐
       ▼                ▼                ▼
  Build-Time        MQTT Upload     HTTP Upload
  (spiffs image)    (chunked)       (captive portal)
       │                │                │
       └────────────────┴────────────────┘
                        │
                        ▼
                 LittleFS /converters/    (6.75 MB Partition)
                 ├── index.json
                 ├── *.json              (Merged DB)
                 └── custom/*.json       (User Quirks, pri=0)
                        │
                        ▼
              zb_converter_loader.c       (Unified Lookup, LRU Cache 32)
                   │         │
                   │    zb_fn_registry.c  (58+ fz, 37+ tz, binary search)
                   │         │
                   ▼         ▼
              zb_converter_registry.c     (Priority: custom > runtime > hardcoded > generic)
                        │
           ┌────────────┼──────────────────┐
           ▼            ▼                  ▼
    Attribute Report  Command          Interview
    → fz_* → JSON    → tz_* → ZCL     → Bind Converter
           │                           → Quirk Init Sequence
           ▼                           → Custom Reporting
    zb_quirk_engine.c
    (transforms: scale, offset, invert, clamp)
           │
           ▼
    EVT_DEVICE_STATE_CHANGED
           │
    ┌──────┴──────┐
    ▼             ▼
 esphome_       mqtt_
 adapter.c      adapter.c
    │
    ├── esphome_adapter_entities.c     (Capability → Entity)
    ├── esphome_adapter_gateway.c      (Gateway Mgmt)
    └── esphome_adapter_diagnostics.c  (Per-Device Diag)
    │
    ▼
 Home Assistant (ESPHome Native API, Port 6053)
```

---

## Metriken & Erfolgskriterien

| Metrik | Aktuell | Ziel | Messung |
|--------|---------|------|---------|
| Geraete (hardcoded) | 27 | 27 | Code count |
| Geraete (runtime DB) | 0 → **4,761** | **4000+** ✅ | `zb_converter_loader_get_stats()` |
| fz Funktionen | 35 | **58+** | `zb_fn_registry_fz_count()` |
| tz Funktionen | 14 | **37+** | `zb_fn_registry_tz_count()` |
| Max LOC pro Datei | ~800 | **<500** | `wc -l` |
| Quirk Flags (Verhalten) | 5 | **10** | Bitmask count |
| Transform-Engine | nein | **ja** | scale/offset/invert/clamp |
| ESPHome Entity-Typen | 15 | 15 (+ Siren via Switch) | Entity count |
| ZHA Services | 2 | **8+** | Service count |
| Unit Tests (Host) | 0 | **4+** | `make -C tests` |
| Python Tests | 0 | **4+** | `pytest tests/tools/` |
| LittleFS genutzt | ~60KB | **~900KB** | `df /converters/` |
| PSRAM Runtime | 0 | **~12.5KB** | Cache stats |
| Code Size Delta | — | **+20KB Flash** | `size` |
