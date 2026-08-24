# Main component CMakeLists.txt
# ESP32-C5 Zigbee2MQTT Gateway

set(COMPONENT_SRCS
    "main.c"
    "zigbee/zb_coordinator.c"
    "zigbee/zb_router.c"
    "zigbee/zb_network.c"
    "zigbee/zb_cluster_multistate.c"
    "zigbee/zb_cluster_hvac.c"
    "zigbee/zb_cluster_measurement.c"
    "zigbee/zb_cluster_electrical.c"
    "zigbee/zb_cluster_security.c"
    "zigbee/zb_cluster_closures.c"
    "zigbee/zb_callbacks.c"
    "zigbee/zb_interview.c"
    "zigbee/zb_groups.c"
    "zigbee/zb_binding.c"
    "zigbee/zb_backup.c"
    "zigbee/zb_topology.c"
    "zigbee/zb_availability.c"
    "zigbee/zb_rejoin_helper.c"
    "zigbee/zb_leave_helper.c"
    "zigbee/zb_reporting.c"
    "zigbee/zb_install_codes.c"
    "zigbee/zb_diagnostics.c"
    "zigbee/zb_time_server.c"
    "zigbee/zb_poll_control.c"
    "zigbee/zb_zcl_helpers.c"
    "zigbee/zb_cmd_retry.c"
    "zigbee/zb_alarms.c"
    "zigbee/zb_demand_response.c"
    "zigbee/zb_hvac_dehumid.c"
    "zigbee/zb_lumi.c"
    "zigbee/zb_tuya.c"
    "zigbee/tuya/tuya_driver_registry.c"
    "zigbee/tuya/tuya_fingerbot.c"
    "zigbee/converter/zb_converter_registry.c"
    "zigbee/converter/zb_converter_std.c"
    "zigbee/converter/converters/conv_generic.c"
    "zigbee/converter/converters/conv_tuya_bridge.c"
    "zigbee/converter/converters/conv_tuya_fingerbot.c"
    "zigbee/converter/zb_converter_fn_registry.c"
    "zigbee/converter/zb_converter_loader.c"
    "zigbee/converter/zb_quirk_engine.c"
    "zigbee/converter/zb_custom_quirk.c"
    "zigbee/converter/std/zb_converter_std_hvac.c"
    "zigbee/converter/std/zb_converter_std_security.c"
    "zigbee/converter/std/zb_converter_std_lighting.c"
    "zigbee/converter/std/zb_converter_std_electrical.c"
    "zigbee/converter/std/zb_converter_std_tuya.c"
    "zigbee/converter/std/zb_converter_std_vendor.c"
    "wifi/wifi_manager.c"
    "wifi/wifi_config.c"
    "wifi/wifi_captive_portal.c"
    "mqtt/gateway_mqtt.c"
    "mqtt/mqtt_buffer.c"
    "mqtt/mqtt_topics.c"
    "core/bridge/mqtt_bridge.c"
    "core/bridge/bridge_request_handler.c"
    "core/bridge/bridge_response.c"
    "core/bridge/bridge_events.c"
    "core/discovery/ha_discovery_ng.c"
    "core/discovery/ha_bridge_discovery.c"
    "core/monitoring/system_monitor.c"
    "core/monitoring/memory_manager.c"
    "core/monitoring/task_manager.c"
    "core/monitoring/crash_reporter.c"
    "core/monitoring/perf_metrics.c"
    "core/memory/memory_manager_ng.c"
    "core/memory/module_manager.c"
    "core/memory/lifecycle.c"
    "core/memory/memory_pressure.c"
    "core/memory/string_intern.c"
    "core/memory/graceful_degradation.c"
    "core/memory/buffer_pool_helpers.c"
    "core/bridge/mqtt_event_handler.c"
    "core/events/event_bus.c"
    "core/device_state_publisher.c"
    "zigbee/zb_command_handler.c"
    "core/mqtt_logger.c"
    "core/config_manager.c"
    "core/led_status_manager.c"
    "core/littlefs_mount.c"
    "core/state_persistence.c"
    "core/lifecycle_manager.c"
    "core/device/unified_device.c"
    "core/device/device_registry.c"
    "core/adapters/zigbee_adapter.c"
    "core/adapters/mqtt_adapter.c"
    "core/adapters/ble_adapter.c"
    "core/adapters/esphome_adapter.c"
    "core/adapters/esphome_adapter_tuya.c"
    "core/adapters/esphome_adapter_exposes.c"
    "core/adapters/esphome_adapter_gateway.c"
    "core/adapters/esphome_adapter_diagnostics.c"
    "core/adapters/adapter_registry.c"
    "core/device/device_persistence.c"
    "core/foundation_init.c"
    "utils/json_utils.c"
    "utils/version.c"
    "ota/ota_handler.c"
    "ota/http_ota.c"
    "ota/converter_db_ota.c"
    "ota/mqtt_ota.c"
    "esphome/esphome_api.c"
    "esphome/esphome_api_server.c"
    "esphome/esphome_api_handlers.c"
    "esphome/esphome_protocol.c"
    "esphome/esphome_entity_core.c"
    "esphome/esphome_entity_sensors.c"
    "esphome/esphome_entity_controls.c"
    "esphome/esphome_entity_specialized.c"
    "esphome/esphome_services.c"
    "esphome/esphome_gateway_services.c"
    "esphome/esphome_ota.c"
    "esphome/esphome_noise.c"
    "led/led_controller.c"
)

set(COMPONENT_ADD_INCLUDEDIRS
    "."
    "zigbee"
    "zigbee/converter"
    "zigbee/converter/converters"
    "mqtt"
    "wifi"
    "core"
    "core/bridge"
    "core/discovery"
    "core/monitoring"
    "core/memory"
    "core/device"
    "core/events"
    "core/adapters"
    "utils"
    "ota"
    "bluetooth"
    "bluetooth/devices"
    "esphome"
    "led"
    "mmwave"
)

# Bluetooth sources - only include when BT is enabled
set(BT_SRCS
    "bluetooth/ble_manager.c"
    "bluetooth/ble_scanner.c"
    "bluetooth/ble_security.c"
    "bluetooth/ble_gatt_client.c"
    "bluetooth/ble_battery_service.c"
    "bluetooth/ble_gatt_discovery.c"
    "bluetooth/devices/ble_device_registry.c"
    "bluetooth/devices/ble_xiaomi.c"
    "bluetooth/devices/ble_govee.c"
    "bluetooth/devices/ble_beacon.c"
    "bluetooth/devices/ble_ruuvi.c"
    "bluetooth/devices/ble_qingping.c"
    "bluetooth/devices/ble_switchbot.c"
    "bluetooth/devices/ble_inkbird.c"
    "bluetooth/ble_esphome_bridge.c"
    "esphome/esphome_ble_proxy.c"
)

# Required components - base set
# Note: Component names adapt based on ESP-IDF version
# v5.x: Uses built-in mqtt, json components
# v6.x: Uses component manager espressif__mqtt, espressif__cjson
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(MQTT_COMPONENT "espressif__mqtt")
    set(JSON_COMPONENT "espressif__cjson")
    set(RINGBUF_COMPONENT "esp_ringbuf")
else()
    set(MQTT_COMPONENT "mqtt")
    set(JSON_COMPONENT "json")
    set(RINGBUF_COMPONENT "")
endif()

set(REQUIRED_COMPONENTS
    esp-zigbee-lib
    ${MQTT_COMPONENT}
    esp_wifi
    nvs_flash
    joltwallet__littlefs
    esp_event
    esp_netif
    esp_http_client
    esp_http_server
    esp_https_ota
    app_update
    bootloader_support
    mdns
    led_strip
    ${JSON_COMPONENT}
    ${RINGBUF_COMPONENT}
)

# Add Bluetooth sources and component if BT is enabled
# Always add stubs - they compile to nothing when BT is enabled (ifdef guards)
# This allows the component to register before Kconfig is processed
list(APPEND COMPONENT_SRCS "bluetooth/ble_stubs.c")
if(CONFIG_BT_ENABLED)
    list(APPEND COMPONENT_SRCS ${BT_SRCS})
endif()

# mmWave presence sensor (UART-connected S3KM1110)
if(CONFIG_MMWAVE_SENSOR_ENABLE)
    list(APPEND COMPONENT_SRCS "mmwave/mmwave_sensor.c")
endif()

# Green Power and Multi-PAN sit in the tree but were never wired up: neither
# zb_gp_init() nor zb_multi_pan_init() has a caller anywhere. They were compiled
# and linked regardless, costing flash for nothing. Multi-PAN even had a Kconfig
# flag already (CONFIG_ZIGBEE_MULTI_PAN_ENABLED, default n) that the build
# simply ignored.
#
# Green Power additionally has no MIC verification (see zb_gp_verify_security);
# frames at FULL_COUNTER_MIC or above are rejected outright, so enabling this
# will not give you working secured Green Power devices yet.
# Zigbee features that were complete but never connected. Each is opt-in and
# gated here too, so turning one off keeps it out of the image entirely.
if(CONFIG_ZB_SCENES_ENABLE)
    list(APPEND COMPONENT_SRCS "zigbee/zb_scenes.c")
endif()

if(CONFIG_ZB_TOUCHLINK_ENABLE)
    list(APPEND COMPONENT_SRCS "zigbee/zb_touchlink.c")
endif()

if(CONFIG_ZB_OTA_ENABLE)
    list(APPEND COMPONENT_SRCS "zigbee/zb_ota.c")
endif()

if(CONFIG_ZB_GREEN_POWER_ENABLE)
    list(APPEND COMPONENT_SRCS "zigbee/zb_green_power.c")
endif()

# Observability modules: complete but never wired up until now, so both are
# opt-in and default to off.
if(CONFIG_EVENT_TRACE_ENABLE)
    list(APPEND COMPONENT_SRCS "core/monitoring/event_trace.c")
endif()

if(CONFIG_ADAPTIVE_MEMORY_ENABLE)
    list(APPEND COMPONENT_SRCS "core/monitoring/adaptive_memory.c")
endif()

if(CONFIG_BATCH_PUBLISHER_ENABLE)
    list(APPEND COMPONENT_SRCS "mqtt/batch_publisher.c")
endif()

if(CONFIG_MEMORY_DASHBOARD_ENABLE)
    list(APPEND COMPONENT_SRCS "core/monitoring/memory_dashboard.c")
endif()

# Entity state mirror. Its only consumer is the per-entity MQTT topics, which
# ESPHome primary mode compiles out, so it defaults to off there.
if(CONFIG_ESPHOME_ENTITY_MIRROR)
    list(APPEND COMPONENT_SRCS "esphome/esphome_entity_mirror.c")
endif()

if(CONFIG_ZIGBEE_MULTI_PAN_ENABLED)
    list(APPEND COMPONENT_SRCS "zigbee/zb_multi_pan.c")
endif()

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS ${COMPONENT_ADD_INCLUDEDIRS}
    REQUIRES ${REQUIRED_COMPONENTS}
    PRIV_REQUIRES
        spi_flash
)

# Add bt component as dependency if BT is enabled
# This is done after idf_component_register because CONFIG_* variables
# may not be available during the initial cmake pass
if(CONFIG_BT_ENABLED)
    idf_component_get_property(bt_lib bt COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PUBLIC ${bt_lib})
endif()

# Version and commit come from git, not from a literal somebody has to remember
# to bump. Without this the firmware reported whatever was last hard-coded in
# version.h, which had drifted three releases out of date.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GW_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GW_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GW_GIT_DESCRIBE)
    string(REGEX REPLACE "^v" "" GW_VERSION "${GW_GIT_DESCRIBE}")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        FIRMWARE_VERSION="${GW_VERSION}")
    message(STATUS "Firmware version from git: ${GW_VERSION}")
endif()
if(GW_GIT_HASH)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        GIT_COMMIT="${GW_GIT_HASH}")
endif()
