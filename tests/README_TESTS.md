# Test suite status

Last reviewed: 2026-08-01, against ESP-IDF v6.0.2.

## What was wrong

The suite had not been buildable for a long time — likely never in its current
form. Two separate problems:

**1. Invalid CMake structure.** `tests/CMakeLists.txt` called both `project()`
and `idf_component_register()`. ESP-IDF rejects that outright:

```
CMake Error: Called idf_component_register from a non-component directory.
```

Fixed by splitting it the normal way: `tests/CMakeLists.txt` is the project
file, `tests/main/CMakeLists.txt` registers the test component.

**2. It pulled in the gateway's `main` component wholesale**, via
`EXTRA_COMPONENT_DIRS`. That cannot work: `main/main.c` defines `app_main()`
and so does `run_all_tests.c`. Modules under test are now listed individually
in `main/CMakeLists.txt` instead.

## What is green

61 tests, all passing on hardware (ESP32-C5, ESP-IDF v6.0.2).

Run them with one command — it builds, flashes, runs, reports and puts the
gateway firmware back, even if the tests fail or you interrupt it:

```bash
scripts/test.sh                  # autodetects the port
scripts/test.sh /dev/cu.usbmodemXXXX
scripts/test.sh --no-restore     # leave the test app on the board
```

Exit status is the test result, so it works in a pipeline.

| Suite | Tests | Covers |
|---|---|---|
| Version Management | 17 | version strings and comparison |
| Device Registry | 19 | the concurrency-safety contract — snapshot_ids, state_dup ownership, slot recycling |
| Zigbee Diagnostics | 9 | `zb_diagnostics_get_network_map()` — ID snapshot iteration and the protocol filter |
| Zigbee Backup | 7 | `collect_devices()` via `zb_backup_create()` — same iteration and filter |
| ESPHome Protocol | 13 | protobuf encode/decode round-trips, and the message type IDs |
| ESPHome Entity Mirror | 20 | the open-addressed entity table — backward-shift deletion and `_get()` copy ownership |

85 tests total.

The registry and diagnostics suites exist specifically to pin down behaviour
that was changed without runtime cover:

- `state_dup_survives_device_removal` is the regression test for the
  use-after-free. `device_registry_remove()` calls `cJSON_Delete()` on the
  state, so a borrowed pointer would be reading freed heap.
- `freed_slot_not_reused_immediately` fails if slot allocation goes back to
  scanning from index 0, which handed a removed device's slot straight to the
  next one that joined.
- `deletion_keeps_collided` in the entity mirror suite is the guard on
  backward-shift deletion. Getting that wrong does not crash — it makes
  colliding entries silently unreachable, so it would surface in the field as
  "some MQTT topics stopped updating after a device was removed" and nowhere
  else.
- `skips_non_zigbee_devices` fails if the `dev->protocol` check is dropped
  again. `device_t::proto` is a union, so reading the Zigbee arm for a BLE
  device reinterprets BLE fields. Present in both the diagnostics and the
  backup suite, because both functions had the same defect.

Writing the backup suite turned up a leak: `zb_backup_create()` allocated a
`zb_backup_t` internally when passed NULL, then neither returned nor freed it
on the success path. Several KB per call. The NULL branch was dead — no caller
used it — so it is now rejected with `ESP_ERR_INVALID_ARG`.

### zb_topology is not covered

`zb_topology_scan_start()` got the same iteration and protocol-filter rewrite,
but its only observable effect is internal scan state, and the function ends by
calling `esp_zb_zdo_mgmt_lqi_req()` — it needs a running Zigbee stack. Testing
it would mean adding an accessor to production code purely for the test. The
logic is character-for-character the same as the two covered copies, so the
gap is narrow, but it is a gap.

The protocol suite pins the message type IDs by hand-written number rather
than by reading the enum — a test that reads the constant it checks proves
nothing. Wrong IDs previously put Home Assistant into a disconnect loop
(errno 104), and the values come from aioesphomeapi's api.proto.

### Safety properties of this test app

Two things were deliberate, and both matter if you touch the harness:

- **It shares the gateway's partition layout** (`../partitions.csv`, 16MB).
  The stock single-app 2MB table would rewrite the partition table with a
  different geometry and move NVS, `zb_storage` and the LittleFS converter
  database out from under the gateway. As configured, the test binary just
  occupies `ota_0`. Verified by flashing the tests and restoring the gateway:
  both pairings, the Tuya binding and the 5383-device converter DB survived.
- **It never erases NVS.** `run_all_tests.c` does *not* do the usual
  "on ESP_ERR_NVS_NO_FREE_PAGES, erase and retry" dance, because on this layout
  that would wipe the real pairings and WiFi credentials. It warns and
  continues instead.

`scripts/test.sh` restores the gateway automatically. Doing it by hand:
`idf.py -p <port> flash` from the project root.

## What is retired

Two suites test code that no longer exists in the project:

| Suite | Missing dependency |
|---|---|
| `unit/test_memory_manager.c` | `core/memory_manager.h` — replaced by `core/memory/memory_manager_ng.h`, different API |
| `integration/test_zigbee_mqtt_bridge.c` | `zigbee/zb_device_handler.h` — deleted (~1750 LOC removed during the NG migration) |

These need rewriting against the current API, not repairing. They are left in
the tree as reference for what used to be covered.

## What is blocked on dependency cascades

The remaining suites compile against headers that still exist, but linking them
means pulling their implementation and everything it drags along. For example
`utils/json_utils.c` includes `zigbee/zb_network.h`,
`core/device/device_registry.h` and three cluster modules — so testing one
helper function ends up linking most of the Zigbee stack.

| Suite | Needs |
|---|---|
| JSON Utilities | `utils/json_utils.c` → zb_network, device_registry, cluster modules |
| Config Manager | `core/config_manager.c` → NVS, event bus |
| MQTT Topics | `mqtt/mqtt_topics.c` |
| WiFi + MQTT Integration | wifi_manager, mqtt client, mocks |
| OTA | ota_handler, app_update |

Getting these green is a real piece of work: either accept linking large parts
of the gateway into the test binary, or introduce seams so the units can be
tested in isolation. Worth doing, but it is a project rather than a fix.

## Adding a suite

Two places must stay in step, or you get an undefined reference at link time
rather than a compile error:

1. `main/CMakeLists.txt` — add the test file **and** the sources it exercises
   to `TEST_SRCS`
2. same file — add the matching `TEST_SUITE_*` define

`run_all_tests.c` guards every suite behind its define.

## Building and running

```bash
source ~/esp/esp-idf-v6/export.sh
idf.py -C tests build
idf.py -C tests -p /dev/cu.usbmodem211201 flash monitor
```

Flashing the test app **replaces the gateway firmware** on the device. Reflash
the gateway afterwards with `idf.py -p <port> flash` from the project root.
NVS, `zb_storage` and `spiffs` are untouched by either, so pairings, network
keys and the converter DB survive.
