# Test application for the ESP32-C5 gateway.
#
# This is the PROJECT file. It must not call idf_component_register() — that
# belongs in a component directory, and mixing the two is what made this suite
# unbuildable ("Called idf_component_register from a non-component directory").
# The test code is registered in main/CMakeLists.txt.
#
# Note this project deliberately does NOT pull in the gateway's own main
# component: that component defines app_main(), and so does run_all_tests.c.
# Modules under test are listed individually instead — see main/CMakeLists.txt.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_c5_zigbee2mqtt_tests)
