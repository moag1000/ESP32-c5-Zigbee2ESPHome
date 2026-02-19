# Declarative Table-Driven Tuya Device Driver

## Status Assessment

The **converter registry** (`main/zigbee/converter/`) is already implemented and integrated:
- `zb_converter_types.h` — Types: `zb_converter_def_t`, `zb_from_zigbee_t`, `zb_to_zigbee_t`, `zb_expose_t`
- `zb_converter_std.c` — 16 `fz_*` + 8 `tz_*` standard converters
- `zb_converter_registry.c` — Registry + dispatch (`handle_report`, `handle_command`)
- `conv_generic.c` — 6 generic fallback converters (on/off light, dimmable, CT, contact, motion, temp/hum)
- `conv_ikea.c`, `conv_xiaomi.c`, `conv_philips.c`, `conv_sonoff.c`, `conv_lidl.c` — manufacturer-specific
- `conv_tuya_bridge.c` — Bridges Tuya driver vtable into converter framework
- Already wired: `zb_callbacks.c:607` calls `zb_converter_handle_report()`, `command_handler.c:92` calls `zb_converter_handle_command()`

**This covers what was "Phase 2" in the original plan** — ZCL attribute/command mapping tables are effectively implemented via the converter registry.

**What's still missing**: Phase 1 — the **Tuya declarative DP descriptor system**. The Tuya bridge converter (`conv_tuya_bridge.c`) delegates to the existing `tuya_device_driver_t` vtable, but new Tuya devices still require writing a full custom driver (~500-1500 lines each). We need the table-driven generic driver so that simple Tuya devices can be defined in ~20 lines.

---

## What to Build: Tuya Generic Driver

A single generic `tuya_device_driver_t` implementation that reads DP descriptor tables (const, flash) to handle process_dp, handle_command, build_state_json, and publish_discovery automatically.

### Core Types

```c
/* ── Bidirectional Tuya DP value converter ── */
typedef struct tuya_dp_converter {
    void (*from_dp)(const tuya_dp_t *dp, cJSON *json,
                    const struct tuya_dp_desc *desc);
    esp_err_t (*to_dp)(const cJSON *value, tuya_dp_t *dp,
                       const struct tuya_dp_desc *desc);
} tuya_dp_converter_t;

/* ── Flags for DP behavior ── */
typedef enum {
    TUYA_DP_FLAG_NONE           = 0,
    TUYA_DP_FLAG_READ_ONLY      = (1 << 0),  /* Sensor — no command generation */
    TUYA_DP_FLAG_SKIP_DISCOVERY = (1 << 1),  /* Handled by custom_publish_discovery */
    TUYA_DP_FLAG_INVERTED       = (1 << 2),  /* Invert bool value */
    TUYA_DP_FLAG_CMD_0x04       = (1 << 3),  /* Use Tuya cmd 0x04 instead of 0x00 */
} tuya_dp_flags_t;

/* ── DP descriptor — one per datapoint, const in flash ── */
typedef struct tuya_dp_desc {
    uint8_t dp_id;                        /* Tuya DP ID (0 = sentinel) */
    tuya_dp_type_t dp_type;               /* Expected DP type */
    const char *json_field;               /* MQTT JSON field name */
    const tuya_dp_converter_t *converter; /* Bidirectional value converter */
    uint8_t flags;                        /* tuya_dp_flags_t bitmask */
    /* HA discovery metadata */
    const char *ha_component;             /* "switch", "sensor", "number", "select", "binary_sensor" */
    const char *ha_device_class;          /* optional */
    const char *ha_unit;                  /* optional */
    const char *ha_icon;                  /* optional */
    /* For number entities */
    float min_value, max_value, step;
    float scale_factor;                   /* multiply DP value (0.0 = 1.0) */
    /* For select/enum entities */
    const char *const *options;           /* NULL-terminated string array */
} tuya_dp_desc_t;

/* ── Complete device definition — const in flash ── */
typedef struct tuya_device_def {
    const char *name;                     /* e.g. "tuya_plug" */
    const char *const *manufacturer_ids;  /* NULL-terminated */
    const char *const *model_patterns;    /* optional substring matches */
    const tuya_dp_desc_t *dps;            /* sentinel-terminated (dp_id=0) */
    /* Optional custom callbacks for complex behavior */
    esp_err_t (*custom_init)(uint16_t short_addr);
    void (*custom_remove)(uint16_t short_addr);
    esp_err_t (*custom_process_dp)(uint16_t short_addr, const tuya_dp_t *dp);
    esp_err_t (*custom_handle_command)(uint16_t short_addr, uint8_t ep, const cJSON *json);
    esp_err_t (*custom_publish_discovery)(const zb_device_t *device);
} tuya_device_def_t;
```

### Pre-Built Converters

| Converter | from_dp | to_dp | Notes |
|-----------|---------|-------|-------|
| `tuya_conv_on_off` | bool → `"ON"`/`"OFF"` | `"ON"`/`"OFF"` → bool | Respects `INVERTED` flag |
| `tuya_conv_bool` | bool → `true`/`false` | `true`/`false` → bool | Respects `INVERTED` flag |
| `tuya_conv_value` | int32 → number | number → int32 | Raw, no scaling |
| `tuya_conv_value_scaled` | int32 × scale → number | number / scale → int32 | Uses `desc->scale_factor` |
| `tuya_conv_enum` | enum index → string | string → enum index | Uses `desc->options[]` |
| `tuya_conv_bitmap` | uint32 → number | number → uint32 | Direct |
| `tuya_conv_battery` | int32 → number | — (read-only) | Auto-updates `zb_device_t.power_info` |

### Convenience Macros

```c
#define TUYA_DP_SWITCH(id, field) \
    { .dp_id = id, .dp_type = TUYA_DP_TYPE_BOOL, .json_field = field, \
      .converter = &tuya_conv_on_off, .ha_component = "switch" }

#define TUYA_DP_SENSOR(id, field, cls, unit) \
    { .dp_id = id, .dp_type = TUYA_DP_TYPE_VALUE, .json_field = field, \
      .converter = &tuya_conv_value, .flags = TUYA_DP_FLAG_READ_ONLY, \
      .ha_component = "sensor", .ha_device_class = cls, .ha_unit = unit }

#define TUYA_DP_NUMBER(id, field, lo, hi, stp) \
    { .dp_id = id, .dp_type = TUYA_DP_TYPE_VALUE, .json_field = field, \
      .converter = &tuya_conv_value, .ha_component = "number", \
      .min_value = lo, .max_value = hi, .step = stp }

#define TUYA_DP_SELECT(id, field, opts) \
    { .dp_id = id, .dp_type = TUYA_DP_TYPE_ENUM, .json_field = field, \
      .converter = &tuya_conv_enum, .ha_component = "select", .options = opts }

#define TUYA_DP_BATTERY(id) \
    { .dp_id = id, .dp_type = TUYA_DP_TYPE_VALUE, .json_field = "battery", \
      .converter = &tuya_conv_battery, .flags = TUYA_DP_FLAG_READ_ONLY, \
      .ha_component = "sensor", .ha_device_class = "battery", .ha_unit = "%" }

#define TUYA_DP_END  { 0 }
```

### Example: Tuya Smart Plug (~20 lines)

```c
static const char *const s_plug_mfg[] = {"_TZ3000_abcd1234", "_TZ3000_efgh5678", NULL};

static const tuya_dp_desc_t s_plug_dps[] = {
    TUYA_DP_SWITCH(1, "state"),
    TUYA_DP_NUMBER(9, "countdown", 0, 86400, 1),
    TUYA_DP_END
};

const tuya_device_def_t tuya_plug_def = {
    .name = "tuya_plug",
    .manufacturer_ids = s_plug_mfg,
    .dps = s_plug_dps,
};
```

### Generic Driver Internal Architecture

**Single `tuya_device_driver_t` vtable** registered with the existing Tuya driver registry. The converter registry's Tuya bridge (`conv_tuya_bridge.c`) then routes MQTT commands to it automatically.

```
MQTT command → command_handler.c
  → zb_converter_handle_command() [converter registry]
    → conv_tuya_bridge: tz_tuya_command()
      → tuya_driver_get(short_addr)
        → tuya_generic_driver.handle_command()  ← NEW
          → iterate dps[], find json_field, to_dp(), zb_tuya_send_dp()

Zigbee DP report → zb_tuya.c
  → tuya_driver_get(short_addr)
    → tuya_generic_driver.process_dp()  ← NEW
      → iterate dps[], find dp_id, from_dp(), cache value
  → device_state_publish_tuya()
    → tuya_generic_driver.build_state_json()  ← NEW
      → iterate dps[], from_dp() for each cached value
```

**Per-device state** (internal SRAM, ~144 bytes/device):

```c
#define TUYA_GENERIC_MAX_DEVICES    10
#define TUYA_GENERIC_MAX_DPS        16

typedef struct {
    int32_t int_value;
    bool valid;
} tuya_dp_cache_t;              /* 8 bytes */

typedef struct {
    uint16_t short_addr;        /* 0 = unused */
    const tuya_device_def_t *def;
    tuya_dp_cache_t dp_cache[TUYA_GENERIC_MAX_DPS];
    int64_t last_update;
} tuya_generic_state_t;         /* ~144 bytes */
```

- RAW/STRING DPs are NOT cached. Devices needing them use `custom_process_dp`.
- Battery DP (`tuya_conv_battery`) auto-updates `zb_device_t.power_info`.

**match→init_device binding**: `match()` caches last matched def in `s_pending_def` (file-scope static). `init_device()` consumes it. Safe — both run sequentially on the same task.

---

## Files

### New Files (4)

| File | Purpose | ~LOC |
|------|---------|------|
| `main/zigbee/tuya/tuya_dp_descriptors.h` | Types: `tuya_dp_converter_t`, `tuya_dp_desc_t`, `tuya_device_def_t`, flags, macros, extern converter declarations | ~130 |
| `main/zigbee/tuya/tuya_dp_converters.c` | Pre-built converter implementations (7 converters) | ~200 |
| `main/zigbee/tuya/tuya_generic_driver.h` | Public API: `tuya_generic_init()`, `tuya_generic_register_def()` | ~40 |
| `main/zigbee/tuya/tuya_generic_driver.c` | Generic driver: vtable impl, state table, DP cache, HA discovery generator | ~400 |

### New Files — Example Device (1)

| File | Purpose | ~LOC |
|------|---------|------|
| `main/zigbee/tuya/tuya_plug.c` | Example Tuya plug device definition (proves the pattern, template for new devices) | ~25 |

### Modified Files (2)

| File | Change |
|------|--------|
| `main/CMakeLists.txt` | Add 3 new .c files: `tuya_dp_converters.c`, `tuya_generic_driver.c`, `tuya_plug.c` |
| `main/main.c` | After `tuya_driver_registry_init()`: call `tuya_generic_init()` + `tuya_generic_register_def(&tuya_plug_def)` |

### Unchanged Files

| File | Why unchanged |
|------|---------------|
| `main/zigbee/tuya/tuya_device_driver.h` | Vtable interface unchanged — generic driver implements it |
| `main/zigbee/tuya/tuya_driver_registry.c` | Registry unchanged — generic driver registers as one more driver |
| `main/zigbee/tuya/tuya_fingerbot.c` | Existing custom driver, untouched |
| `main/zigbee/converter/*` | Converter registry already done — Tuya bridge routes to generic driver automatically |
| `main/core/device_state_publisher.c` | Converter registry already handles report→JSON dispatch |
| `main/core/command_handler.c` | Converter registry already handles JSON→command dispatch |

---

## Implementation Order

1. `tuya_dp_descriptors.h` — types + macros (no compilation dependency issues)
2. `tuya_dp_converters.c` — converter implementations
3. `tuya_generic_driver.h` + `tuya_generic_driver.c` — generic driver
4. `tuya_plug.c` — example device definition
5. `CMakeLists.txt` — add new sources
6. `main.c` — register generic driver + plug def
7. Build + verify

---

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| Only Tuya generic driver (no ZCL table changes) | Converter registry already covers ZCL attribute/command mapping |
| Function pointers for converters | More extensible than enum dispatch — custom converters don't need core modifications |
| Fingerbot NOT migrated | Working custom driver, no risk. Can migrate later |
| Internal SRAM for state | ~1.4KB total (10 x 144 bytes). Not worth PSRAM overhead |
| DP cache stores `int32_t` only | RAW/STRING DPs handled by `custom_process_dp` |
| `CMD_0x04` flag per DP | Fingerbot-class devices need Tuya cmd 0x04. Flag controls `zb_tuya_send_dp_with_cmd()` cmd ID |
| Battery converter auto-updates power_info | Matches existing Fingerbot behavior for HA battery display |
| No entity grouping in v1 | Complex entities (light with brightness+color_temp as one) use `custom_publish_discovery` |

---

## Verification

1. `idf.py build` — zero errors
2. **DP report test**: Bind a Tuya plug (or simulate via log). DP1=true → log shows generic driver cached value, MQTT publishes `{"state":"ON"}`
3. **Command test**: Send `{"state":"OFF"}` via MQTT → generic driver converts to Tuya DP, sends via `zb_tuya_send_dp()`
4. **HA discovery test**: Verify plug publishes `homeassistant/switch/.../config` with correct payload
5. **Fingerbot regression**: Fingerbot still uses its custom driver (registered before generic driver → matched first)
6. **Converter pipeline**: Verify that the Tuya bridge converter (`conv_tuya_bridge.c`) correctly routes to the generic driver for plug devices
