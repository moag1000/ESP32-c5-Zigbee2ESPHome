# CMakeLists.txt for ESP32-C5 Zigbee2MQTT Gateway Tests
# Build configuration for test suite

cmake_minimum_required(VERSION 3.16)

# Set the project name
set(PROJECT_NAME "esp32_c5_zigbee2mqtt_tests")

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set extra component directories to include main component
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../main")

# Define project
project(${PROJECT_NAME})

# Target: tests
idf_component_register(
    SRCS
        "test_framework.c"
        "run_all_tests.c"

        # Unit tests
        "unit/test_memory_manager.c"
        "unit/test_config_manager.c"
        "unit/test_json_utils.c"
        "unit/test_version.c"
        "unit/test_mqtt_topics.c"

        # Integration tests
        "integration/test_wifi_mqtt_integration.c"
        "integration/test_zigbee_mqtt_bridge.c"
        "integration/test_ota.c"

        # Mocks
        "mocks/mock_mqtt.c"
        "mocks/mock_zigbee.c"

    INCLUDE_DIRS
        "."
        "../main"
        "../main/core"
        "../main/utils"
        "../main/wifi"
        "../main/mqtt"
        "../main/zigbee"
        "mocks"

    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        mqtt
        json
        esp_http_client
        app_update
        bootloader_support
)

# Set compile options for tests
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

# Add test-specific definitions
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TEST_BUILD=1
    CONFIG_LOG_DEFAULT_LEVEL=3
)
