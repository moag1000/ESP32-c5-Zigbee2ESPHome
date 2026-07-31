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

| Suite | State |
|---|---|
| Version Management | **builds and links** |

That is genuinely all, and the reason is in the next section.

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
