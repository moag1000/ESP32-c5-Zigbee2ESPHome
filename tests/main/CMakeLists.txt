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
    # state_persistence_mark_dirty() is the one exception and is stubbed: the
    # real module needs LittleFS and a task to set a boolean.
    "../unit/test_device_registry.c"
    "../../main/core/device/device_registry.c"
    "../../main/core/device/unified_device.c"
    "../../main/core/memory/memory_manager_ng.c"
    "../mocks/mock_state_persistence.c"

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
    # The Tuya datapoint parser reads a 16-bit length off the air and fills a
    # 64-byte buffer from it — worth pinning down. No stubs needed despite
    # zb_tuya.c's dependencies: -ffunction-sections plus --gc-sections drops
    # everything the parser does not reach.
    "../unit/test_zb_tuya_parse.c"
    "../../main/zigbee/zb_tuya.c"

    # zb_lumi.c includes nothing but its own header and string.h. The parser
    # walks a tag/type/value run with no per-entry length, so one wrong width
    # shifts every following read — worth pinning down.
    "../unit/test_zb_lumi.c"
    "../../main/zigbee/zb_lumi.c"

    "../unit/test_zb_backup.c"
    "../../main/zigbee/zb_backup.c"
    "../mocks/mock_zb_backup_deps.c"

    # esphome_protocol.c includes nothing but its own header and esp_log.h —
    # the cheapest module in esphome/ to cover, and the one where wrong message
    # type IDs actually caused a Home Assistant disconnect loop.
    "../unit/test_esphome_protocol.c"
    "../../main/esphome/esphome_protocol.c"

    # The ExecuteServiceRequest decoder lives in esphome_protocol.c (listed
    # above) precisely so it can be tested: reading it out of the API handler
    # would have dragged in the socket server and the service registry. It was
    # wrong for as long as the gateway had services, and nothing noticed.
    "../unit/test_esphome_execute_service.c"

    # The entity mirror needs memory_manager_ng (already above) and event_bus,
    # which comes in with the zb_diagnostics suite along with its perf_metrics
    # mock. Nothing else: the mirror deliberately does not depend on the device
    # registry any more, which is the whole point of the module.
    "../unit/test_esphome_entity_mirror.c"
    "../../main/esphome/esphome_entity_mirror.c"

    # mqtt_topics.c includes nothing but its own header, string.h, ctype.h and
    # esp_log.h — the cheapest module in the tree to cover, and the one where a
    # truncated topic silently names the wrong device.
    "../unit/test_mqtt_topics.c"
    "../../main/mqtt/mqtt_topics.c"

    # esphome_noise.c pulls in mbedtls/PSA and esp_random, all of which the
    # test app already links, plus memory_manager_ng which is listed above.
    "../unit/test_esphome_noise.c"
    "../../main/esphome/esphome_noise.c"
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
        mbedtls
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
    TEST_SUITE_ESPHOME_ENTITY_MIRROR=1
    TEST_SUITE_MQTT_TOPICS=1
    TEST_SUITE_ESPHOME_NOISE=1

    # The mirror's header compiles to no-op stubs unless this is set, and the
    # test project has no Kconfig entry for it. Define it here so the real
    # implementation is what gets tested.
    CONFIG_ESPHOME_ENTITY_MIRROR=1
    CONFIG_ESPHOME_ENTITY_MIRROR_MAX=64

    # mqtt_topics.h derives MQTT_BASE_TOPIC from this; the test project has no
    # Kconfig entry for it. Must match the gateway's own default.
    CONFIG_MQTT_BASE_TOPIC=\"zigbee2mqtt\"
    CONFIG_HOMEASSISTANT_DISCOVERY_PREFIX=\"homeassistant\"
)
