# Test component.
#
# Sources live one level up (tests/), so the paths escape this directory. That
# is intentional: it keeps the existing test layout intact instead of moving a
# dozen files around.
#
# Only suites that still compile against the current codebase are listed. See
# ../README_TESTS.md for what was retired and why.

set(TEST_SRCS
    "../test_framework.c"
    "../run_all_tests.c"

    # Self-contained: version.c pulls in nothing but esp_log.
    "../unit/test_version.c"
    "../../main/utils/version.c"

    # device_registry has a shallow dependency chain — memory_manager_ng and
    # unified_device, both of which only need esp_log/heap/FreeRTOS. No Zigbee
    # stack is dragged in, which is what makes this one testable at all.
    "../unit/test_device_registry.c"
    "../../main/core/device/device_registry.c"
    "../../main/core/device/unified_device.c"
    "../../main/core/memory/memory_manager_ng.c"

    # zb_diagnostics needs the event bus, and event_bus.c calls exactly one
    # perf_metrics symbol. The real perf_metrics.c would pull in task_manager,
    # gateway_mqtt and mqtt_helpers, so it is stubbed instead.
    "../unit/test_zb_diagnostics.c"
    "../../main/zigbee/zb_diagnostics.c"
    "../../main/core/events/event_bus.c"
    "../mocks/mock_perf_metrics.c"

    # zb_backup_create() builds the backup in memory (zb_backup_save() is what
    # touches files), so the collection logic is testable. Linking it needs
    # twelve symbols across zb_coordinator, zb_network, zb_groups, zb_binding,
    # bridge_response and littlefs_mount — all stubbed.
    "../unit/test_zb_backup.c"
    "../../main/zigbee/zb_backup.c"
    "../mocks/mock_zb_backup_deps.c"

    # esphome_protocol.c includes nothing but its own header and esp_log.h —
    # the cheapest module in esphome/ to cover, and the one where wrong message
    # type IDs actually caused a Home Assistant disconnect loop.
    "../unit/test_esphome_protocol.c"
    "../../main/esphome/esphome_protocol.c"
)

idf_component_register(
    SRCS ${TEST_SRCS}
    INCLUDE_DIRS
        ".."
        "../mocks"
        "../../main"
        "../../main/core"
        "../../main/core/device"
        "../../main/core/memory"
        "../../main/core/events"
        "../../main/core/monitoring"
        "../../main/core/bridge"
        "../../main/mqtt"
        "../../main/utils"
        "../../main/zigbee"
        "../../main/esphome"
    REQUIRES
        nvs_flash
        esp_timer
        espressif__cjson
        espressif__esp-zigbee-lib
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TEST_BUILD=1
    # Enable a suite only together with its sources in TEST_SRCS above,
    # otherwise the build fails at link time with an undefined reference.
    TEST_SUITE_VERSION=1
    TEST_SUITE_DEVICE_REGISTRY=1
    TEST_SUITE_ZB_DIAGNOSTICS=1
    TEST_SUITE_ZB_BACKUP=1
    TEST_SUITE_ESPHOME_PROTOCOL=1
)
