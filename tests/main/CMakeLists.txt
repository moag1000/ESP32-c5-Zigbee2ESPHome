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
        "../../main/utils"
    REQUIRES
        nvs_flash
        esp_timer
        espressif__cjson
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
)
