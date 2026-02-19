# ESP32-C5 Zigbee2MQTT Gateway Project
# Root CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

# Minimum ESP-IDF version requirement
set(MIN_IDF_VERSION "6.0")

# ESP-Zigbee-SDK is managed by IDF Component Manager (see idf_component.yml)
# If using local SDK installation, set ESP_ZIGBEE_SDK_PATH environment variable
if(DEFINED ENV{ESP_ZIGBEE_SDK_PATH})
    if(EXISTS $ENV{ESP_ZIGBEE_SDK_PATH}/components)
        list(APPEND EXTRA_COMPONENT_DIRS $ENV{ESP_ZIGBEE_SDK_PATH}/components)
        message(STATUS "Using local ESP-Zigbee-SDK: $ENV{ESP_ZIGBEE_SDK_PATH}")
    else()
        message(STATUS "ESP_ZIGBEE_SDK_PATH set but components dir not found, using managed components")
    endif()
else()
    message(STATUS "Using ESP-Zigbee components from IDF Component Manager")
endif()

# Project information
set(PROJECT_NAME "esp32_c5_zigbee2mqtt")
set(PROJECT_VER "0.1.0")

# Include sdkconfig.local if it exists (for machine-specific settings)
# This is applied AFTER sdkconfig.defaults, so local values override defaults
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_SOURCE_DIR}/sdkconfig.defaults"
        "${CMAKE_SOURCE_DIR}/sdkconfig.local"
    )
    message(STATUS "Using local config: sdkconfig.local")
else()
    set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(${PROJECT_NAME})

# Enable C23 standard for C files only (gnu23 for GCC extensions)
# Generator expression prevents applying to C++ files in ESP-IDF components
idf_build_set_property(COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:C>:-std=gnu23>" APPEND)
