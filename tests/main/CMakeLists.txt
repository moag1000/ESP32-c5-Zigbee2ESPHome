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
)

idf_component_register(
    SRCS ${TEST_SRCS}
    INCLUDE_DIRS
        ".."
        "../mocks"
        "../../main"
        "../../main/core"
        "../../main/utils"
    REQUIRES
        nvs_flash
        esp_timer
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
)
