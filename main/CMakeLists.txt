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
    "zigbee/zb_scenes.c"
    "zigbee/zb_binding.c"
    "zigbee/zb_backup.c"
    "zigbee/zb_topology.c"
    "zigbee/zb_touchlink.c"
    "zigbee/zb_ota.c"
    "zigbee/zb_multi_pan.c"
    "zigbee/zb_green_power.c"
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
    "zigbee/zb_tuya.c"
    "zigbee/cluster_state_ng.c"
    "zigbee/tuya/tuya_driver_registry.c"
    "zigbee/tuya/tuya_fingerbot.c"
    "zigbee/converter/zb_converter_registry.c"
    "zigbee/converter/zb_converter_std.c"
    "zigbee/converter/converters/conv_generic.c"
    "zigbee/converter/converters/conv_ikea.c"
    "zigbee/converter/converters/conv_xiaomi.c"
    "zigbee/converter/converters/conv_philips.c"
    "zigbee/converter/converters/conv_sonoff.c"
    "zigbee/converter/converters/conv_lidl.c"
    "zigbee/converter/converters/conv_tuya_bridge.c"
    "zigbee/converter/zb_converter_fn_registry.c"
    "zigbee/converter/zb_converter_loader.c"
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
    "core/monitoring/memory_dashboard.c"
    "core/monitoring/event_trace.c"
    "core/monitoring/perf_metrics.c"
    "core/monitoring/adaptive_memory.c"
    "core/memory_pool.c"
    "core/memory/memory_manager_ng.c"
    "core/memory/module_manager.c"
    "core/memory/lifecycle.c"
    "core/memory/memory_pressure.c"
    "core/memory/string_intern.c"
    "core/memory/graceful_degradation.c"
    "core/memory/buffer_pool_helpers.c"
    "core/bridge/mqtt_event_handler.c"
    "core/coex_manager.c"
    "mqtt/batch_publisher.c"
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
    "core/adapters/adapter_registry.c"
    "core/device/device_persistence.c"
    "core/foundation_init.c"
    "utils/json_utils.c"
    "utils/version.c"
    "ota/ota_handler.c"
    "ota/http_ota.c"
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
    "esphome/esphome_ota.c"
    "esphome/esphome_noise.c"
    "esphome/esphome_device_registry.c"
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
