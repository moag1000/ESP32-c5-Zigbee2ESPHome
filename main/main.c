/**
 * @file main.c
 * @brief ESP32-C5 Zigbee2MQTT Gateway - Main Entry Point
 *
 * This is the main application entry point for the ESP32-C5 Zigbee2MQTT Gateway.
 * It initializes all subsystems and manages the main application flow.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_timer.h"

/* Coexistence API for WiFi + 802.15.4 (Zigbee) */
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif

/* Zigbee includes - coordinator or router based on config */
#include "zigbee/zb_coordinator.h"
#include "zigbee/zb_binding.h"
#include "zigbee/zb_groups.h"
#include "zigbee/zb_backup.h"
#include "zigbee/zb_hvac_dehumid.h"
#include "zigbee/zb_reporting.h"
#include "zigbee/zb_topology.h"
#include "zigbee/zb_router.h"

/* WiFi and MQTT includes */
#include "wifi/wifi_manager.h"
#include "wifi/wifi_config.h"
#include "esp_wifi.h"
#include "gateway_mqtt.h"
#include "mqtt/mqtt_buffer.h"
#include "mqtt/mqtt_topics.h"
#include "gateway_defaults.h"

/* Bridge includes */
#include "mqtt_bridge.h"
#include "ha_bridge_discovery.h"

/* Tuya driver registry */
#include "zigbee/tuya/tuya_driver_registry.h"
#include "zigbee/tuya/tuya_fingerbot.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"

/* Converter database */
#include "zigbee/converter/zb_converter.h"
#include "zigbee/converter/zb_converter_loader.h"
#include "zigbee/converter/zb_custom_quirk.h"

/* Generic converter fallback (capability-based) */
extern const zb_converter_def_t *conv_generic_for_capabilities(uint32_t caps);

/* LittleFS + State Persistence */
#include "core/littlefs_mount.h"
#include "zigbee/zb_interview.h"
#if CONFIG_STATE_PERSISTENCE_ENABLE
#include "core/state_persistence.h"
#endif

/* ZCL command retry */
#include "zigbee/zb_cmd_retry.h"

#if CONFIG_ZB_SCENES_ENABLE
#include "zigbee/zb_scenes.h"
#endif
#if CONFIG_ZB_TOUCHLINK_ENABLE
#include "zigbee/zb_touchlink.h"
#endif
#if CONFIG_ZB_OTA_ENABLE
#include "zigbee/zb_ota.h"
#endif


/* Lifecycle manager for dynamic service control */
#include "core/lifecycle_manager.h"

/* Phase 5-7 includes */
#include "config_manager.h"
#include "system_monitor.h"
#include "crash_reporter.h"
#include "utils/version.h"
#include "ota/ota_handler.h"
#include "ota/http_ota.h"
#include "ota/mqtt_ota.h"

/* NG Architecture Foundation */
#include "core/foundation_init.h"
#include "core/adapters/esphome_adapter.h"
#include "core/memory/module_manager.h"
#include "core/memory/graceful_degradation.h"
#include "core/memory/memory_manager_ng.h"
#include "core/memory/string_intern.h"
#include "core/monitoring/perf_metrics.h"

/* LED Status includes */
#include "led/led_controller.h"
#include "core/led_status_manager.h"

/* Captive Portal */
#if CONFIG_WIFI_CAPTIVE_PORTAL_ENABLE
#include "wifi/wifi_captive_portal.h"
#endif

/* BLE includes */
#if CONFIG_BT_SCANNER_ENABLED
#include "bluetooth/ble_manager.h"
#include "bluetooth/ble_scanner.h"
#endif

/* ESPHome includes */
#if CONFIG_ESPHOME_API_ENABLE
#include "esphome/esphome_api.h"
#include "esphome/esphome_ble_proxy.h"
#endif

/* Log tag for main module */
static const char *TAG_MAIN = "MAIN";

/* Log tags for future modules */
static const char *TAG_ZIGBEE = "ZIGBEE";
static const char *TAG_MQTT = "MQTT";
static const char *TAG_WIFI = "WIFI";
static const char *TAG_CORE = "CORE";
static const char *TAG_OTA = "OTA";

/* Note: Memory monitoring is now handled by system_monitor module */

/* Event group for WiFi and MQTT connection status */
static EventGroupHandle_t s_connection_event_group;

/* Event bits for connection status */
#define WIFI_CONNECTED_BIT      BIT0
#define MQTT_CONNECTED_BIT      BIT1
#define MQTT_BRIDGE_START_BIT   BIT2  /**< Signal main loop to start/resubscribe bridge */
#define MQTT_BRIDGE_RESUB_BIT   BIT3  /**< Signal main loop to resubscribe bridge */
#define SYSTEM_READY_BIT        (WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT)

/**
 * @brief Print system information
 *
 * Displays chip model, cores, features, flash size, and IDF version
 */
static void print_system_info(void)
{
    /* Get chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG_MAIN, "========================================");
    ESP_LOGI(TAG_MAIN, "  ESP32-C5 Zigbee2MQTT Gateway");
    ESP_LOGI(TAG_MAIN, "========================================");
    ESP_LOGI(TAG_MAIN, "Chip: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG_MAIN, "Cores: %d", chip_info.cores);
    /* Report what this firmware actually runs, not what the silicon offers.
     * The chip always reports BLE; the firmware may well have it disabled. */
    ESP_LOGI(TAG_MAIN, "Silicon: WiFi%s%s",
             (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
    ESP_LOGI(TAG_MAIN, "Radios enabled: WiFi/Zigbee%s",
#if CONFIG_BT_ENABLED
             "/BLE"
#else
             " (BLE disabled)"
#endif
    );

    /* Get flash size */
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG_MAIN, "Flash size: %lu MB", flash_size / GW_BYTES_PER_MB);
    }

    ESP_LOGI(TAG_MAIN, "ESP-IDF Version: %s", esp_get_idf_version());
    ESP_LOGI(TAG_MAIN, "Free heap: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG_MAIN, "Min free heap: %lu bytes", esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG_MAIN, "========================================");
}

/**
 * @brief MQTT connection callback
 *
 * Called when MQTT connection state changes.
 * IMPORTANT: This runs in the ESP-MQTT task context. Do NOT call
 * esp_mqtt_client_publish() or mqtt_bridge_start() directly here
 * as it can cause deadlocks. Instead, signal the main loop via events.
 *
 * @param connected true if connected, false if disconnected
 */
static void mqtt_connection_callback(bool connected)
{
    if (connected) {
        ESP_LOGI(TAG_MQTT, "MQTT connected callback");
        xEventGroupSetBits(s_connection_event_group, MQTT_CONNECTED_BIT);

        /* Signal main loop to start or resubscribe bridge
         * Do NOT call mqtt_bridge_start() directly from here - it would
         * call esp_mqtt_client_publish() from within the MQTT event handler,
         * causing a deadlock with ESP-MQTT's internal locks */
        if (!mqtt_bridge_is_enabled()) {
            xEventGroupSetBits(s_connection_event_group, MQTT_BRIDGE_START_BIT);
        } else {
            xEventGroupSetBits(s_connection_event_group, MQTT_BRIDGE_RESUB_BIT);
        }
    } else {
        ESP_LOGW(TAG_MQTT, "MQTT disconnected callback");
        xEventGroupClearBits(s_connection_event_group, MQTT_CONNECTED_BIT);
    }
}

/**
 * @brief MQTT message callback
 *
 * Called when MQTT message is received
 *
 * @param topic Topic of the message
 * @param data Message data
 * @param data_len Length of data
 */
static void mqtt_message_callback(const char *topic, const char *data, size_t data_len)
{
    ESP_LOGD(TAG_MQTT, "Received message on topic: %s", topic);
    /* Message handling is now done by mqtt_bridge */
}

/* Note: Old system_monitor_task removed - now using system_monitor module */

#if CONFIG_WIFI_CAPTIVE_PORTAL_ENABLE
/**
 * @brief Captive portal subsystem callback
 *
 * Stops BLE and ESPHome when portal starts (frees ~75KB internal RAM),
 * restarts them when portal stops. Only acts on modules that are actually
 * initialized, since the portal may run early in boot before Phase 7.
 *
 * @param portal_active true when portal is starting, false when stopping
 */
#if CONFIG_BT_SCANNER_ENABLED
static bool s_portal_stopped_ble = false;
#endif
static bool s_portal_stopped_esphome = false;

static void portal_subsystem_callback(bool portal_active)
{
    if (portal_active) {
        /* Only stop subsystems that are actually running */
#if CONFIG_ESPHOME_API_ENABLE
        if (esphome_api_is_running()) {
            esphome_api_stop();
            s_portal_stopped_esphome = true;
            ESP_LOGI(TAG_MAIN, "ESPHome API stopped for captive portal");
        }
#endif
#if CONFIG_BT_SCANNER_ENABLED
        if (ble_manager_is_initialized()) {
            ble_scanner_stop();
            ble_manager_deinit();
            s_portal_stopped_ble = true;
            ESP_LOGI(TAG_MAIN, "BLE stopped for captive portal");
        }
#endif
    } else {
        /* Only restart subsystems we actually stopped */
#if CONFIG_BT_SCANNER_ENABLED
        if (s_portal_stopped_ble) {
            ble_manager_config_t ble_config = {
                .enable_scanner = true,
                .scan_interval_ms = CONFIG_BT_SCAN_INTERVAL,
#ifdef CONFIG_BT_ZIGBEE_COEXIST
                .coexist_with_zigbee = true,
#else
                .coexist_with_zigbee = false,
#endif
            };
            if (ble_manager_init_with_config(&ble_config) == ESP_OK) {
                /* Delay BLE scanner start by 20s for Zigbee stability */
                vTaskDelay(pdMS_TO_TICKS(GW_DELAY_BOOT_MS));
                ble_scanner_start();
                ESP_LOGI(TAG_MAIN, "BLE restarted after captive portal");
            }
            s_portal_stopped_ble = false;
        }
#endif
#if CONFIG_ESPHOME_API_ENABLE
        if (s_portal_stopped_esphome) {
            esphome_api_start();
            ESP_LOGI(TAG_MAIN, "ESPHome API restarted after captive portal");
            s_portal_stopped_esphome = false;
        }
#endif
    }
}
#endif /* CONFIG_WIFI_CAPTIVE_PORTAL_ENABLE */

/**
 * @brief Initialize NVS (Non-Volatile Storage)
 *
 * Initializes NVS flash for storing WiFi credentials, Zigbee data, etc.
 *
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t initialize_nvs(void)
{
    ESP_LOGI(TAG_MAIN, "[INIT] Initializing NVS...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated and needs to be erased */
        ESP_LOGW(TAG_MAIN, "[INIT] NVS partition needs to be erased. Erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG_MAIN, "[INIT] NVS initialized successfully");
    } else {
        ESP_LOGE(TAG_MAIN, "[INIT] Failed to initialize NVS: %s", esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Initialize event loop
 *
 * Creates the default event loop for system events
 *
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t initialize_event_loop(void)
{
    ESP_LOGI(TAG_MAIN, "[INIT] Initializing event loop...");

    esp_err_t ret = esp_event_loop_create_default();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG_MAIN, "[INIT] Event loop initialized successfully");
    } else {
        ESP_LOGE(TAG_MAIN, "[INIT] Failed to initialize event loop: %s", esp_err_to_name(ret));
    }

    return ret;
}

#if CONFIG_BT_SCANNER_ENABLED && CONFIG_BT_AUTO_START_SCANNER
/**
 * @brief Timer callback to start BLE scanner after boot delay
 */
static void ble_scanner_delayed_start_callback(void *arg)
{
    esp_err_t ret = ble_scanner_start();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG_MAIN, "BLE scanner started after boot delay (active=%s)",
                 ble_scanner_is_active_enabled() ? "yes" : "no");
    } else {
        ESP_LOGW(TAG_MAIN, "Failed to start BLE scanner after delay: %s", esp_err_to_name(ret));
    }
}
#endif

/**
 * @brief Application main entry point
 *
 * Initializes all subsystems and starts the main application tasks
 */
/**
 * @brief Bring up the Zigbee radio
 *
 * Called before the MQTT phase, which is the point. Zigbee is what this
 * gateway is for and it needs no IP address, but the boot used to run it
 * strictly after WiFi and MQTT. With the access point unreachable that meant:
 * ~37s of association attempts, then CONFIG_WIFI_CONNECT_GRACE_SEC of grace,
 * then a captive portal that blocks until CONFIG_WIFI_CAPTIVE_PORTAL_TIMEOUT_SEC
 * and calls esp_restart() — so the coordinator never started at all, and the
 * device sat in a reboot loop with its paired devices unreachable. A Zigbee
 * network has no reason to depend on the uplink being up.
 *
 * It runs after the WiFi association window rather than before it: enabling
 * 802.15.4 coexistence while the station is still associating changes the RF
 * conditions of the association itself, and that interaction is not something
 * to alter blind. Association keeps the radio to itself exactly as before;
 * only the portal no longer holds Zigbee hostage.
 *
 * The three former copies of this (coordinator, router, and the no-choice
 * default) differed in two lines, so they are one branch here.
 */
static void zigbee_stack_start(void)
{
    esp_err_t ret;

#if CONFIG_ZIGBEE_DEVICE_TYPE_ROUTER
    ESP_LOGI(TAG_MAIN, "[PHASE 2b] Zigbee Router Initialization");

    ret = zb_router_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_ZIGBEE, "Failed to initialize Zigbee router: %s",
                 esp_err_to_name(ret));
        ESP_ERROR_CHECK(ret);
    }
    ESP_LOGI(TAG_ZIGBEE, "Zigbee router initialized successfully");

    ret = zb_router_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_ZIGBEE, "Failed to start Zigbee router: %s",
                 esp_err_to_name(ret));
        ESP_ERROR_CHECK(ret);
    }
    ESP_LOGI(TAG_ZIGBEE, "Zigbee router started - searching for network...");
#else
    /* Coordinator, and also what an unset device type falls back to. */
    ESP_LOGI(TAG_MAIN, "[PHASE 2b] Zigbee Coordinator Initialization");

    ret = zb_coordinator_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_ZIGBEE, "Failed to initialize Zigbee coordinator: %s",
                 esp_err_to_name(ret));
        ESP_ERROR_CHECK(ret);
    }
    ESP_LOGI(TAG_ZIGBEE, "Zigbee coordinator initialized successfully");

    /* Reporting and topology are initialized before the coordinator starts, so
     * their EVT_NETWORK_READY subscriptions exist by the time the stack
     * publishes it. Neither had a caller at all until now: zb_reporting stayed
     * uninitialized, which meant no device ever had attribute reporting
     * configured, and zb_topology only woke up when someone pressed Network
     * Heal, so link quality was never read. */
    /* Binding has to come first: zb_reporting_provision_device() binds each
     * cluster to the coordinator before configuring reporting, and a device
     * only delivers reports to the entries in its binding table. This module
     * had no caller either, so every bind attempt returned INVALID_STATE. */
    /* The dehumidification cluster handler was reachable and dead.
     *
     * zb_callbacks.c calls zb_dehumid_handle_report() from the ZCL report path,
     * and that function's first statement is a check on s_dehumid_initialized —
     * which nothing ever set. A dehumidifier in the network would have had its
     * reports delivered to a handler that returned INVALID_STATE and logged
     * nothing. The init itself only clears its own static state, so wiring it
     * costs nothing and cannot fail.
     *
     * Untested against a real device: there is none here. What is tested is
     * that the gateway still boots and behaves with it initialized. */
    ret = zb_dehumid_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ZIGBEE, "Dehumidification init failed: %s (continuing)",
                 esp_err_to_name(ret));
    }

    /* Groups and backup were never initialized either. Backup is the one that
     * bites: bridge_request_handler reaches zb_backup_process_mqtt_*() from the
     * MQTT bridge, and every one of those calls returned INVALID_STATE. Groups
     * is its dependency — zb_backup restores group membership. */
    ret = zb_groups_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ZIGBEE, "Groups init failed: %s (continuing)", esp_err_to_name(ret));
    }

    ret = zb_backup_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ZIGBEE, "Backup init failed: %s (continuing)", esp_err_to_name(ret));
    }

    ret = zb_binding_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ZIGBEE, "Binding init failed: %s (continuing)", esp_err_to_name(ret));
    }

    ret = zb_reporting_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ZIGBEE, "Reporting init failed: %s (continuing)", esp_err_to_name(ret));
    }

    ret = zb_topology_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ZIGBEE, "Topology init failed: %s (continuing)", esp_err_to_name(ret));
    }

    ret = zb_coordinator_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_ZIGBEE, "Failed to start Zigbee coordinator: %s",
                 esp_err_to_name(ret));
        ESP_ERROR_CHECK(ret);
    }
    ESP_LOGI(TAG_ZIGBEE, "Zigbee coordinator started successfully");

    /* Initialize ZCL command retry subsystem. Coordinator only — a router
     * does not originate the commands this retries. */
    ret = cmd_retry_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ZIGBEE, "Command retry init failed: %s (non-fatal)",
                 esp_err_to_name(ret));
    }
#endif

#if CONFIG_GW_LED_ENABLED
    led_status_manager_set_condition(LED_COND_ZIGBEE_RUNNING, true);
#endif

    /* Optional Zigbee features, each behind its own Kconfig flag and each
     * defaulting to off. They need the stack running, which is why they are
     * here rather than in foundation_init.c with the protocol-independent
     * components. A failure is non-fatal: the gateway's core job does not
     * depend on any of them. */
#if CONFIG_ZB_SCENES_ENABLE
    ret = zb_scenes_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ZIGBEE, "Scenes init failed: %s (continuing)", esp_err_to_name(ret));
    }
#endif

#if CONFIG_ZB_TOUCHLINK_ENABLE
    ret = zb_touchlink_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ZIGBEE, "Touchlink init failed: %s (continuing)", esp_err_to_name(ret));
    }
#endif

#if CONFIG_ZB_OTA_ENABLE
    ret = zb_ota_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_ZIGBEE, "Zigbee OTA init failed: %s (continuing)", esp_err_to_name(ret));
    }
#endif

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
    ESP_LOGI(TAG_MAIN, "[COEX] Enabling WiFi + 802.15.4 coexistence...");
    ret = esp_coex_wifi_i154_enable();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Failed to enable WiFi/Zigbee coexistence: %s",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG_MAIN, "[COEX] WiFi + 802.15.4 coexistence ENABLED");
    }
#endif
}


/** @brief malloc that prefers PSRAM, falling back to internal on failure. */
static void *psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}


/**
 * @brief Route cJSON's allocations to PSRAM
 *
 * cJSON is this firmware's workhorse for device state, bridge payloads and the
 * converter database, and every node it builds came out of internal RAM. The
 * converter index alone drove internal free heap from 190 KB to 32 KB in 60ms
 * during boot — the low-water mark for the entire run — and the raw file
 * buffer turned out to be the smaller half of that; the parse tree was the
 * rest.
 *
 * Safe here because cJSON is never touched from an ISR and never with the
 * flash cache disabled. Must run before any cJSON call, hence this early.
 */
static void route_cjson_to_psram(void)
{
    static cJSON_Hooks hooks = {
        .malloc_fn = psram_malloc,
        .free_fn   = free,
    };
    cJSON_InitHooks(&hooks);
}

void app_main(void)
{
    esp_err_t ret;

    /* Before anything builds a cJSON node. */
    route_cjson_to_psram();

    /* Suppress noisy WiFi coexistence management frame warnings */
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    /* Temporary: Enable verbose Zigbee stack logging to debug join issues */
    esp_log_level_set("ESP_ZB_CORE", ESP_LOG_DEBUG);
    esp_log_level_set("ESP_ZB_SEC", ESP_LOG_DEBUG);
    esp_log_level_set("ESP_ZB_APS", ESP_LOG_DEBUG);
    esp_log_level_set("ESP_ZB_NWK", ESP_LOG_DEBUG);
    esp_log_level_set("ESP_ZB_ZDO", ESP_LOG_DEBUG);
    esp_log_level_set("ESP_ZB_MAC", ESP_LOG_DEBUG);
    /* IEEE 802.15.4 radio driver debug - see MAC-level frames */
    esp_log_level_set("ieee802154", ESP_LOG_VERBOSE);
    esp_log_level_set("IEEE802154", ESP_LOG_VERBOSE);
    esp_log_level_set("esp_ieee802154", ESP_LOG_VERBOSE);
    esp_log_level_set("i154", ESP_LOG_VERBOSE);
    esp_log_level_set("802154", ESP_LOG_VERBOSE);
    /* ZBOSS internal */
    esp_log_level_set("ZB", ESP_LOG_VERBOSE);
    esp_log_level_set("ZBOSS", ESP_LOG_VERBOSE);
    esp_log_level_set("zb", ESP_LOG_VERBOSE);

    /* Print system information */
    print_system_info();

    /* Print version information */
    version_print();

    /* Phase 1: Core System Initialization */
    ESP_LOGI(TAG_MAIN, "[PHASE 1] Core System Initialization");

    /* Initialize lifecycle manager first - controls service start/stop */
    ret = lifecycle_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Lifecycle manager init failed: %s (non-fatal)", esp_err_to_name(ret));
    }

    /* Initialize NVS */
    ret = initialize_nvs();
    ESP_ERROR_CHECK(ret);

    /* Phase 0: Core Memory Infrastructure (before Foundation) */
    ESP_LOGI(TAG_MAIN, "[INIT] Memory Manager NG...");
    ret = mem_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Memory manager init failed: %s (non-fatal)", esp_err_to_name(ret));
    }

    /* Initialize NG Architecture Foundation (Event Bus, Device Registry, Adapters) */
    ESP_LOGI(TAG_MAIN, "[INIT] NG Architecture Foundation");
    ret = foundation_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Foundation init returned: %s (continuing with partial init)",
                 esp_err_to_name(ret));
    } else {
        foundation_print_status();
    }

    /* Initialize Module Manager (dynamic loading/unloading) */
    ret = module_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Module manager init failed: %s (non-fatal)", esp_err_to_name(ret));
    }

    /* Initialize Graceful Degradation (memory pressure auto-response) */
    ret = graceful_degradation_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Graceful degradation init failed: %s (non-fatal)", esp_err_to_name(ret));
    }

    /* Initialize Performance Metrics */
    ret = perf_metrics_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Performance metrics init failed: %s (non-fatal)", esp_err_to_name(ret));
    }

    /* Mount LittleFS filesystem (shared by state persistence, backup, OTA) */
    ESP_LOGI(TAG_MAIN, "[INIT] LittleFS Filesystem");
    ret = littlefs_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "LittleFS mount failed: %s (non-fatal)", esp_err_to_name(ret));
    }

    /* Initialize crash reporter early - needs NVS */
    ESP_LOGI(TAG_MAIN, "[INIT] Crash Reporter");
    ret = crash_reporter_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Crash reporter init failed: %s (non-fatal)", esp_err_to_name(ret));
    }

    /* Initialize event loop */
    ret = initialize_event_loop();
    ESP_ERROR_CHECK(ret);

    /* Initialize configuration manager (Phase 6) */
    ESP_LOGI(TAG_MAIN, "[INIT] Configuration Manager");
    ret = config_manager_init();
    ESP_ERROR_CHECK(ret);

    /* Initialize Tuya driver registry and register device drivers */
    ESP_LOGI(TAG_MAIN, "[INIT] Tuya Driver Registry");
    tuya_driver_registry_init();
    ret = tuya_fingerbot_register();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Fingerbot driver registration failed: %s (non-fatal)",
                 esp_err_to_name(ret));
    }

    /* Restore persisted Tuya driver bindings for all devices loaded from NVS.
     * This must happen AFTER drivers are registered and devices are loaded. */
    {
        /* tuya_driver_restore_for_device() reads NVS, so snapshot the IDs
         * rather than walking dense indices while devices can come and go. */
        size_t dev_count = 0;
        device_id_t *dev_ids = device_registry_snapshot_ids(&dev_count);
        int restored = 0;
        for (size_t i = 0; i < dev_count; i++) {
            device_t *dev = device_registry_get(dev_ids[i]);
            if (dev != NULL && dev->protocol == DEV_PROTOCOL_ZIGBEE) {
                if (tuya_driver_restore_for_device(dev->id, dev->proto.zigbee.short_addr) == ESP_OK) {
                    restored++;
                }
            }
        }
        device_registry_release_ids(dev_ids);
        if (restored > 0) {
            ESP_LOGI(TAG_MAIN, "Restored %d Tuya driver binding(s) from NVS", restored);
        }
    }

    /* Initialize converter registry and register all converters */
    ESP_LOGI(TAG_MAIN, "[INIT] Converter Registry");
    ret = zb_converter_registry_init();
    if (ret == ESP_OK) {
        zb_converter_register_all();
        ESP_LOGI(TAG_MAIN, "Converter registry initialized");

        /* Initialize string interning (PSRAM pool for converter strings) */
        /* Sized for the whole converter index, not a sample of it.
         *
         * 512 strings / 16 KB was enough only while the index silently
         * stopped at 256 manufacturers. With the real database indexed
         * (1339 manufacturers, 6801 devices) the pool overflowed, interning
         * failed, and converters came back with NULL names —
         * "Bound converter: 0x1F0A -> (null)".
         *
         * Both the entry table and the pool live in PSRAM (see
         * string_intern.c), so this costs nothing scarce. */
        /* Sized for the converter index: one interned string per manufacturer plus
         * one per file, and the pool overflowed once already — converters came
         * back with "(null)" for a name. The database grew from 1338
         * manufacturers to 2545 when the fingerprint identities were restored,
         * so the headroom goes up with it. PSRAM, and 99 % of it is idle. */
        esp_err_t intern_ret = string_intern_init(16384, 512 * 1024);
        if (intern_ret != ESP_OK) {
            ESP_LOGW(TAG_MAIN, "String intern init failed: %s (converter loader will use raw strings)",
                     esp_err_to_name(intern_ret));
        }

        /* Initialize runtime converter loader (LittleFS DB) */
        esp_err_t loader_ret = zb_converter_loader_init();
        if (loader_ret == ESP_OK) {
            size_t db_count = 0, loaded = 0, bytes = 0;
            zb_converter_loader_get_stats(&db_count, &loaded, &bytes);
            ESP_LOGI(TAG_MAIN, "Converter loader ready: %zu devices in DB", db_count);
        } else {
            ESP_LOGW(TAG_MAIN, "Converter loader unavailable (no DB on LittleFS)");
        }

        /* Initialize custom community quirks (highest priority converters) */
        esp_err_t quirk_ret = zb_custom_quirk_init();
        if (quirk_ret == ESP_OK && zb_custom_quirk_count() > 0) {
            ESP_LOGI(TAG_MAIN, "Custom quirks loaded: %zu", zb_custom_quirk_count());
        }

        /* Re-bind converters for devices loaded from NVS.
         * device_persistence_load_all() ran before converters were registered,
         * so NG devices have NULL converter pointers.
         *
         * Fallback chain:
         * 1. Exact manufacturer+model match (zb_converter_find)
         * 2. Generic capability-based converter (conv_generic_for_capabilities)
         * 3. No converter — device gets entities from caps only (register_from_caps) */
        /* zb_converter_find() reads the converter DB from LittleFS, so this
         * loop must not run with the registry mutex held. Snapshot the IDs
         * (PSRAM, not stack) and look each one up individually. */
        size_t dev_count = 0;
        device_id_t *dev_ids = device_registry_snapshot_ids(&dev_count);
        size_t bound = 0, generic_bound = 0, no_conv = 0;
        for (size_t i = 0; i < dev_count; i++) {
            device_t *dev = device_registry_get(dev_ids[i]);
            if (dev == NULL || !dev->in_use ||
                dev->protocol != DEV_PROTOCOL_ZIGBEE ||
                dev->proto.zigbee.converter != NULL) {
                continue;
            }

            const zb_converter_def_t *conv = NULL;

            /* A sleepy device that only ever reported its model comes back
             * from persistence with an empty manufacturer, and the DB is keyed
             * on both — so it would never match. Fill it in from the model
             * prefix first, the same way the interview does. */
            if (dev->manufacturer[0] == '\0' && dev->model[0] != '\0') {
                const char *inferred = zb_interview_infer_manufacturer(dev->model);
                if (inferred != NULL) {
                    snprintf(dev->manufacturer, sizeof(dev->manufacturer), "%s", inferred);
                    ESP_LOGI(TAG_MAIN, "Inferred manufacturer '%s' from model '%s'",
                             inferred, dev->model);
                }
            }

            /* Priority 1: Exact manufacturer+model match */
            if (dev->model[0] != '\0') {
                conv = zb_converter_find(dev->manufacturer, dev->model);
            }

            /* Priority 2: Generic capability-based fallback */
            if (conv == NULL && dev->capabilities != 0) {
                conv = conv_generic_for_capabilities(dev->capabilities);
                if (conv) {
                    generic_bound++;
                    ESP_LOGI(TAG_MAIN, "Generic converter for 0x%04X: "
                             "model='%s' caps=0x%08lX -> %s",
                             dev->proto.zigbee.short_addr,
                             dev->model[0] ? dev->model : "(empty)",
                             (unsigned long)dev->capabilities,
                             conv->description ? conv->description : "generic");
                }
            }

            if (conv != NULL) {
                dev->proto.zigbee.converter = conv;
                zb_converter_bind(dev->proto.zigbee.short_addr, conv);
                dev->capabilities |= zb_converter_get_capabilities(conv);

                /* Update friendly_name if still auto-generated IEEE hex */
                char ieee_default[24];
                device_id_to_str(dev->id, ieee_default, sizeof(ieee_default));
                if (strcmp(dev->friendly_name, ieee_default) == 0) {
                    snprintf(dev->friendly_name, sizeof(dev->friendly_name),
                             "%.23s %04X", dev->model,
                             (unsigned)(dev->id & 0xFFFF));
                }

                bound++;
                if (generic_bound == 0 || conv != conv_generic_for_capabilities(dev->capabilities)) {
                    ESP_LOGI(TAG_MAIN, "Bound converter for '%s': %s (caps=0x%08lx)",
                             dev->model,
                             conv->description ? conv->description : conv->model,
                             (unsigned long)dev->capabilities);
                }
            } else {
                no_conv++;
                ESP_LOGW(TAG_MAIN, "No converter for 0x%04X: "
                         "mfr='%s' model='%s' caps=0x%08lX",
                         dev->proto.zigbee.short_addr,
                         dev->manufacturer[0] ? dev->manufacturer : "(empty)",
                         dev->model[0] ? dev->model : "(empty)",
                         (unsigned long)dev->capabilities);
            }
        }
        device_registry_release_ids(dev_ids);
        ESP_LOGI(TAG_MAIN, "Converter re-bind: %zu exact, %zu generic, %zu unmatched (of %zu devices)",
                 bound - generic_bound, generic_bound, no_conv, dev_count);
    } else {
        ESP_LOGW(TAG_MAIN, "Converter registry init failed: %s (non-fatal)",
                 esp_err_to_name(ret));
    }

    /* Initialize state persistence (requires LittleFS) */
#if CONFIG_STATE_PERSISTENCE_ENABLE
    ESP_LOGI(TAG_MAIN, "[INIT] State Persistence");
    ret = state_persistence_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "State persistence init failed: %s (non-fatal)",
                 esp_err_to_name(ret));
    }
#endif

    /* Get configuration for use in initialization */
    const gateway_config_t *config = config_manager_get_config();
    if (config == NULL) {
        ESP_LOGE(TAG_MAIN, "Failed to get configuration");
        abort();
    }

    /* Initialize LED controller and status manager */
#if CONFIG_GW_LED_ENABLED
    ESP_LOGI(TAG_MAIN, "[INIT] LED Controller");
    ret = led_controller_init();
    if (ret == ESP_OK) {
        ret = led_status_manager_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG_MAIN, "LED status manager init failed: %s", esp_err_to_name(ret));
        }
        /* Boot status is set automatically by led_status_manager_init() */
    } else {
        ESP_LOGW(TAG_MAIN, "LED controller init failed: %s", esp_err_to_name(ret));
    }
#endif

    /* Initialize system monitor (Phase 5) */
    ESP_LOGI(TAG_MAIN, "[INIT] System Monitor");
    ret = system_monitor_init(false); /* Enable MQTT publishing later */
    ESP_ERROR_CHECK(ret);

    /* Start system monitoring task */
    ret = system_monitor_start(SYSMON_INTERVAL_DEFAULT_SEC);
    ESP_ERROR_CHECK(ret);

    /* Create connection event group */
    s_connection_event_group = xEventGroupCreate();
    if (!s_connection_event_group) {
        ESP_LOGE(TAG_MAIN, "Failed to create connection event group");
        abort();
    }

    /* Phase 2: WiFi Initialization */
    ESP_LOGI(TAG_MAIN, "[PHASE 2] WiFi Initialization");

    ret = wifi_manager_init();
    ESP_ERROR_CHECK(ret);

    /* Load WiFi configuration */
    wifi_manager_config_t wifi_config;
    ret = wifi_config_load_from_nvs(&wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_WIFI, "Failed to load WiFi config from NVS, using Kconfig");
        ret = wifi_config_load_from_kconfig(&wifi_config);
        ESP_ERROR_CHECK(ret);
    }

    /* Set LED to WiFi connecting status */
#if CONFIG_GW_LED_ENABLED
    led_status_manager_set_condition(LED_COND_WIFI_CONNECTING, true);
#endif

    /* Zigbee first — and with it esp_coex_wifi_i154_enable().
     *
     * This is not about Zigbee. It is that the WiFi station on this ESP32-C5
     * barely associates until WiFi/802.15.4 coexistence has been enabled.
     * Found by correlating four captures:
     *
     *     zigbee_stack_start()   association    delta
     *          335,160             370,742      35.6s
     *          335,165             370,763      35.6s
     *          335,182             356,974      21.8s
     *          455,394             484,662      29.3s
     *     never started          never          -
     *
     * The station associated 21-36s after that call in every run, and never in
     * runs that ended before it. Until then the driver scans correctly —
     * tuning each channel, passive on the DFS ones, active elsewhere, exactly
     * per regulation — and hears nothing at all, not even neighbouring
     * networks, while the access point sits at -41 dBm.
     *
     * Measured effect of moving it here, boot to associated:
     *
     *     before:  371s and 11 failed attempts
     *     after:    27s and 0 failed attempts
     *
     * A previous version of this comment argued the opposite: that enabling
     * coexistence during association would disturb it, so the radio should be
     * left to the station. That was a guess, and it was backwards. */
    zigbee_stack_start();

    /* Connect to WiFi */
    bool wifi_connected = false;
    ret = wifi_manager_connect(wifi_config.ssid, wifi_config.password);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_WIFI, "WiFi connect failed: %s (SSID may be empty or invalid)",
                 esp_err_to_name(ret));
        /* Don't abort - fall through to captive portal if enabled */
    } else {
        /* Wait for WiFi connection with retry logic
         * After factory reset or fresh boot, DHCP may take longer.
         * Retry connection if timeout occurs instead of continuing without network. */
        ESP_LOGI(TAG_WIFI, "Waiting for WiFi connection...");
        EventGroupHandle_t wifi_event_group = wifi_manager_get_event_group();

        const int max_wifi_retries = 3;
        int wifi_retry_count = 0;

        while (!wifi_connected && wifi_retry_count < max_wifi_retries) {
            EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                                   BIT0,  /* WIFI_CONNECTED_BIT in wifi_manager */
                                                   pdFALSE, pdTRUE,
                                                   pdMS_TO_TICKS(GW_DEFAULT_WIFI_TIMEOUT_MS));

            if (bits & BIT0) {
                char ip_str[WIFI_IP_STRING_LEN];
                wifi_manager_get_ip(ip_str, sizeof(ip_str));
                ESP_LOGI(TAG_WIFI, "WiFi connected successfully! IP: %s", ip_str);
                xEventGroupSetBits(s_connection_event_group, WIFI_CONNECTED_BIT);
                wifi_connected = true;

#if CONFIG_GW_LED_ENABLED
                /* Update LED status for WiFi connected */
                led_status_manager_set_condition(LED_COND_WIFI_CONNECTING, false);
                led_status_manager_set_condition(LED_COND_WIFI_CONNECTED, true);
                led_status_manager_set_condition(LED_COND_BOOT_COMPLETE, true);
#endif
            } else {
                wifi_retry_count++;
                if (wifi_retry_count < max_wifi_retries) {
                    /* Just keep waiting — do NOT drive the connection from here.
                     *
                     * This used to disconnect, sleep 1s and call
                     * wifi_manager_connect() again. But wifi_manager runs its
                     * own reconnect timer with exponential backoff, so two
                     * places were driving the same connection at once. The
                     * disconnect is asynchronous and one second is not always
                     * enough, so esp_wifi_set_config() landed on a station that
                     * was already associating:
                     *
                     *   E wifi:sta is connecting, cannot set config
                     *   E WIFI_MGR: Failed to set WiFi config: ESP_ERR_WIFI_STATE
                     *
                     * The retry then did nothing except abort whatever progress
                     * the reconnect timer had made — plausibly a good part of
                     * why association took minutes. Ownership now sits in one
                     * place: wifi_manager reconnects, boot only waits. */
                    ESP_LOGW(TAG_WIFI, "Still waiting for WiFi (window %d/%d) — "
                                       "wifi_manager keeps retrying in the background",
                             wifi_retry_count, max_wifi_retries);
                } else {
                    ESP_LOGW(TAG_WIFI, "No connection after %d fast attempts", max_wifi_retries);
                }
            }
        }

        /* Grace period before falling back to the captive portal.
         *
         * The three attempts above cover roughly 37 seconds. That is not
         * enough for every access point: this gateway has been observed
         * failing the initial attempts against a WPA2/WPA3 mesh and then
         * associating fine on a later background retry, staying up for hours.
         *
         * Starting the portal is expensive and disruptive — it disables
         * auto-reconnect, takes ESPHome down and puts the radio into AP mode
         * for CONFIG_WIFI_CAPTIVE_PORTAL_TIMEOUT_SEC. Doing that to a gateway
         * whose credentials are perfectly good costs minutes of downtime on
         * every boot. The portal exists for unknown or wrong credentials, not
         * for a slow AP.
         *
         * So: keep auto-reconnect running and give it a real window. If it
         * connects, the portal is never started. */
        if (!wifi_connected) {
            const int grace_sec = CONFIG_WIFI_CONNECT_GRACE_SEC;
            if (grace_sec > 0) {
                ESP_LOGW(TAG_WIFI, "Waiting up to %ds for background reconnect "
                                   "before starting the captive portal", grace_sec);
                EventBits_t bits = xEventGroupWaitBits(wifi_event_group, BIT0,
                                                       pdFALSE, pdTRUE,
                                                       pdMS_TO_TICKS(grace_sec * 1000));
                if (bits & BIT0) {
                    char ip_str[WIFI_IP_STRING_LEN];
                    wifi_manager_get_ip(ip_str, sizeof(ip_str));
                    ESP_LOGI(TAG_WIFI, "WiFi connected on background retry! IP: %s", ip_str);
                    xEventGroupSetBits(s_connection_event_group, WIFI_CONNECTED_BIT);
                    wifi_connected = true;
#if CONFIG_GW_LED_ENABLED
                    led_status_manager_set_condition(LED_COND_WIFI_CONNECTING, false);
                    led_status_manager_set_condition(LED_COND_WIFI_CONNECTED, true);
                    led_status_manager_set_condition(LED_COND_BOOT_COMPLETE, true);
#endif
                } else {
                    ESP_LOGE(TAG_WIFI, "Still no WiFi after %ds - network appears unavailable",
                             grace_sec);
                }
            }
        }
    }

    /* Phase 3: MQTT Client Initialization */
    ESP_LOGI(TAG_MAIN, "[PHASE 3] MQTT Client Initialization");

    if (!wifi_connected) {
#if CONFIG_WIFI_CAPTIVE_PORTAL_ENABLE
        /* The portal is for obtaining credentials, not for waiting out a slow
         * access point. If this SSID has authenticated us before, the
         * credentials are not the problem and the portal is the wrong answer:
         * it disables auto-reconnect, stops ESPHome and puts the radio into AP
         * mode, all while the station would have connected on its own.
         *
         * Measured here: association took 7s on one boot, 356-372s on several,
         * and had not happened within 450s on another — with correct
         * credentials throughout. No fixed grace period separates that from a
         * wrong password; "has this ever worked" does. */
        esp_err_t portal_ret;
        if (wifi_manager_credentials_known_good(wifi_config.ssid)) {
            ESP_LOGW(TAG_WIFI, "WiFi not up yet, but '%s' has connected before — "
                                "not starting the portal. wifi_manager keeps "
                                "retrying in the background.", wifi_config.ssid);
            portal_ret = ESP_ERR_NOT_SUPPORTED;   /* Neither started nor needed */
        } else {
            ESP_LOGW(TAG_WIFI, "WiFi failed - starting captive portal");
            portal_ret = captive_portal_start(portal_subsystem_callback);
        }

        if (portal_ret == ESP_OK) {
            /* Portal succeeded - check if we're connected now */
            wifi_connected = wifi_manager_is_connected();
            if (wifi_connected) {
                char ip_str[WIFI_IP_STRING_LEN];
                wifi_manager_get_ip(ip_str, sizeof(ip_str));
                ESP_LOGI(TAG_WIFI, "WiFi configured via captive portal! IP: %s", ip_str);
                xEventGroupSetBits(s_connection_event_group, WIFI_CONNECTED_BIT);
#if CONFIG_GW_LED_ENABLED
                led_status_manager_set_condition(LED_COND_WIFI_CONNECTING, false);
                led_status_manager_set_condition(LED_COND_WIFI_CONNECTED, true);
                led_status_manager_set_condition(LED_COND_BOOT_COMPLETE, true);
#endif
            }
        } else if (portal_ret == ESP_ERR_TIMEOUT) {
            /* Carry on rather than restart.
             *
             * Restarting here was harmless while Zigbee started later in boot:
             * there was nothing running to lose. The coordinator now comes up
             * before this point, so a restart tears down a working Zigbee
             * network — with its paired devices online — because the WiFi
             * uplink is unavailable. With a 300s grace period and a 300s portal
             * timeout that repeated roughly every ten minutes, and Zigbee was
             * up for about half of each cycle.
             *
             * Nothing needs the restart: captive_portal_stop() has already put
             * the radio back into station mode and re-enabled wifi_manager's
             * auto-reconnect, so the gateway keeps trying to associate in the
             * background while doing its actual job. MQTT and ESPHome pick up
             * on their own once an address appears. */
            ESP_LOGW(TAG_WIFI, "Captive portal timed out — continuing without an "
                               "uplink. Zigbee keeps running; wifi_manager retries "
                               "in the background.");
        }
#else
        ESP_LOGW(TAG_MQTT, "WiFi not connected - waiting for background reconnect before MQTT...");
        /* Wait up to 60s for WiFi background reconnect */
        EventBits_t wifi_bits = xEventGroupWaitBits(wifi_manager_get_event_group(), BIT0,
                                                     pdFALSE, pdTRUE,
                                                     pdMS_TO_TICKS(60000));
        if (wifi_bits & BIT0) {
            char ip_str[WIFI_IP_STRING_LEN];
            wifi_manager_get_ip(ip_str, sizeof(ip_str));
            ESP_LOGI(TAG_WIFI, "WiFi connected (background)! IP: %s", ip_str);
            xEventGroupSetBits(s_connection_event_group, WIFI_CONNECTED_BIT);
            wifi_connected = true;
        } else {
            ESP_LOGE(TAG_WIFI, "WiFi still not connected - starting MQTT anyway (will retry)");
        }
#endif /* CONFIG_WIFI_CAPTIVE_PORTAL_ENABLE */
    }

    /* Wait a bit for WiFi to stabilize */
    vTaskDelay(pdMS_TO_TICKS(GW_WIFI_STABILIZE_DELAY_MS));

    /* Configure MQTT client */
    mqtt_config_t mqtt_config = {
        .port = CONFIG_MQTT_BROKER_PORT,
        .use_ssl = false,
        .keepalive = CONFIG_MQTT_KEEPALIVE,
        .qos = CONFIG_MQTT_QOS,
        .clean_session = true,
    };

    strlcpy(mqtt_config.broker_url, CONFIG_MQTT_BROKER_URL, sizeof(mqtt_config.broker_url));
    strlcpy(mqtt_config.username, CONFIG_MQTT_USERNAME, sizeof(mqtt_config.username));
    strlcpy(mqtt_config.password, CONFIG_MQTT_PASSWORD, sizeof(mqtt_config.password));

    /* Generate client ID with MAC address for uniqueness */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mqtt_config.client_id, sizeof(mqtt_config.client_id),
            "%s_%02x%02x%02x", CONFIG_MQTT_CLIENT_ID,
            mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG_MQTT, "MQTT Client ID: %s", mqtt_config.client_id);

    ret = mqtt_client_init(&mqtt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_MQTT, "Failed to initialize MQTT client: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG_MQTT, "Continuing without MQTT - will retry later");
        /* Continue without MQTT - Zigbee can still work */
        goto skip_mqtt;
    }

    /* Initialize MQTT message buffer (PSRAM) for offline queuing during pairing */
    ret = mqtt_buffer_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MQTT, "MQTT buffer init failed: %s (non-fatal)", esp_err_to_name(ret));
        /* Non-fatal - messages won't be buffered during WiFi-off periods */
    } else {
        ESP_LOGI(TAG_MQTT, "MQTT buffer initialized (PSRAM)");
    }

    /* Initialize MQTT bridge early - before connection callback */
    ret = mqtt_bridge_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CORE, "Failed to initialize MQTT bridge: %s", esp_err_to_name(ret));
        /* Non-fatal - continue anyway */
    } else {
        ESP_LOGI(TAG_CORE, "MQTT bridge initialized successfully");
    }

    /* Register callbacks - bridge is now ready to handle connection */
    mqtt_client_register_callback(mqtt_message_callback);
    mqtt_client_register_connection_callback(mqtt_connection_callback);

    /* Start MQTT client */
    ret = mqtt_client_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_MQTT, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG_MQTT, "Continuing without MQTT - will retry later");
        goto skip_mqtt;
    }

    /* Wait for MQTT connection */
    ESP_LOGI(TAG_MQTT, "Waiting for MQTT connection...");
    EventBits_t mqtt_bits = xEventGroupWaitBits(s_connection_event_group,
                              MQTT_CONNECTED_BIT,
                              pdFALSE, pdTRUE,
                              pdMS_TO_TICKS(GW_DEFAULT_MQTT_TIMEOUT_MS));

    if (mqtt_bits & MQTT_CONNECTED_BIT) {
        ESP_LOGI(TAG_MQTT, "MQTT connected successfully!");
#if CONFIG_GW_LED_ENABLED
        led_status_manager_set_condition(LED_COND_MQTT_CONNECTED, true);
#endif
    } else {
        ESP_LOGW(TAG_MQTT, "Failed to connect to MQTT within timeout");
        /* Continue anyway - will retry in background */
    }

skip_mqtt:
    /* Phase 4: NG Architecture Adapters
     *
     * The Zigbee radio itself came up before Phase 3 — see zigbee_stack_start().
     * What is left here is the adapter layer, which belongs after MQTT because
     * that is what it publishes through. */
    ESP_LOGI(TAG_MAIN, "[PHASE 4] Starting NG Architecture Adapters");
    ret = foundation_start_adapters();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Foundation adapters start failed: %s (continuing)",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG_MAIN, "NG Architecture adapters started successfully");
    }

    /* Phase 5: Core Logic and Gateway Bridge */
    ESP_LOGI(TAG_MAIN, "[PHASE 5] Gateway Bridge Startup");

    /* Bridge was already initialized in Phase 3 before callbacks were registered.
     * Start the bridge now if MQTT is connected. If MQTT connects later,
     * the connection callback will start the bridge. */
    if (mqtt_client_is_connected() && !mqtt_bridge_is_enabled()) {
        ret = mqtt_bridge_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_CORE, "Failed to start MQTT bridge: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG_CORE, "MQTT bridge started successfully");

            /* Publish bridge statistics */
            bridge_stats_t stats = mqtt_bridge_get_stats();
            ESP_LOGI(TAG_CORE, "Bridge stats: enabled=%d", stats.enabled);
        }
    } else if (!mqtt_client_is_connected()) {
        ESP_LOGW(TAG_CORE, "MQTT not connected - bridge will start when connection established");
    }

    /* Phase 6: OTA Update Support (Phase 7 Implementation) */
    ESP_LOGI(TAG_MAIN, "[PHASE 6] OTA Update Support");

    if (config->ota_enabled && strlen(config->ota_url) > 0) {
        ret = ota_handler_init(config->ota_url);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG_OTA, "OTA handler initialized (URL: %s)", config->ota_url);

            /* Mark current app as valid (if pending) */
            ota_handler_mark_valid();

            /* Enable automatic OTA checks if configured */
            if (CONFIG_OTA_CHECK_INTERVAL > 0) {
                ota_handler_set_auto_check(true, CONFIG_OTA_CHECK_INTERVAL);
                ESP_LOGI(TAG_OTA, "Automatic OTA checks enabled (interval: %d hours)",
                        CONFIG_OTA_CHECK_INTERVAL);
            }
        } else {
            ESP_LOGW(TAG_OTA, "Failed to initialize OTA handler: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGI(TAG_OTA, "OTA updates disabled in configuration");
    }

    /* Initialize HTTP OTA module (HTTPS downloads with lifecycle integration) */
    ret = http_ota_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_OTA, "HTTP OTA init failed: %s (non-fatal)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG_OTA, "HTTP OTA module initialized");
    }

    /* Initialize MQTT OTA trigger (Zigbee2MQTT compatible OTA commands) */
    ret = mqtt_ota_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_OTA, "MQTT OTA init failed: %s (non-fatal)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG_OTA, "MQTT OTA trigger initialized");
    }

    /* Note: System monitor MQTT publishing and crash report are enabled
     * in the main loop after bridge start to avoid callback deadlocks */

    /* Phase 7: BLE + ESPHome Initialization */
#if CONFIG_BT_SCANNER_ENABLED
    bool ble_initialized = false;
    ESP_LOGI(TAG_MAIN, "[PHASE 7] Bluetooth BLE Scanner");
    ESP_LOGI(TAG_MAIN, "Heap before BLE init: free=%lu, internal=%lu, min_free=%lu",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             esp_get_minimum_free_heap_size());
    {
        ble_manager_config_t ble_config = {
            .enable_scanner = true,
            .scan_interval_ms = CONFIG_BT_SCAN_INTERVAL,
#ifdef CONFIG_BT_ZIGBEE_COEXIST
            .coexist_with_zigbee = true,
#else
            .coexist_with_zigbee = false,
#endif
        };
        ret = ble_manager_init_with_config(&ble_config);
        if (ret == ESP_OK) {
            ble_initialized = true;
            ESP_LOGI(TAG_MAIN, "BLE manager initialized");
#if CONFIG_GW_LED_ENABLED
            led_status_manager_set_condition(LED_COND_BLE_RUNNING, true);
#endif

#if CONFIG_BT_AUTO_START_SCANNER
            /* Delay BLE scanner start by 20s to give Zigbee devices time to rejoin
             * after coordinator boot. Single-radio ESP32-C5 needs clear air for rejoins. */
            {
                esp_timer_handle_t ble_delay_timer;
                const esp_timer_create_args_t ble_delay_args = {
                    .callback = ble_scanner_delayed_start_callback,
                    .name = "ble_delay_start"
                };
                if (esp_timer_create(&ble_delay_args, &ble_delay_timer) == ESP_OK) {
                    esp_timer_start_once(ble_delay_timer, 20 * 1000000ULL); /* 20 seconds */
                    ESP_LOGI(TAG_MAIN, "BLE scanner will start in 20s (Zigbee rejoin window)");
                } else {
                    /* Fallback: start immediately if timer creation fails */
                    ret = ble_scanner_start();
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG_MAIN, "BLE scanner started immediately (timer failed)");
                    }
                }
            }
#endif
        } else {
            ESP_LOGW(TAG_MAIN, "Failed to init BLE manager: %s", esp_err_to_name(ret));
        }
    }
#endif /* CONFIG_BT_SCANNER_ENABLED */

#if CONFIG_ESPHOME_API_ENABLE
    ESP_LOGI(TAG_MAIN, "[PHASE 7] ESPHome Native API");
    {
        esphome_api_config_t esphome_config = ESPHOME_API_CONFIG_DEFAULT();
        esphome_config.port = CONFIG_ESPHOME_API_PORT;
        strlcpy(esphome_config.device_name, CONFIG_ESPHOME_DEVICE_NAME,
                sizeof(esphome_config.device_name));
        strlcpy(esphome_config.friendly_name, CONFIG_ESPHOME_FRIENDLY_NAME,
                sizeof(esphome_config.friendly_name));
        strlcpy(esphome_config.password, CONFIG_ESPHOME_API_PASSWORD,
                sizeof(esphome_config.password));
        esphome_config.max_clients = CONFIG_ESPHOME_API_MAX_CLIENTS;
        esphome_config.use_mdns = CONFIG_ESPHOME_MDNS_ENABLE;

        ret = esphome_api_init(&esphome_config);
        if (ret == ESP_OK) {
            esphome_api_set_device_info(NULL, version_get_number(), "ESP32-C5");

            ret = esphome_api_start();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG_MAIN, "ESPHome API started (port=%d, mdns=%s)",
                         CONFIG_ESPHOME_API_PORT,
                         CONFIG_ESPHOME_MDNS_ENABLE ? "yes" : "no");

                /* This image works well enough to be replaced, so tell the
                 * bootloader to keep it.
                 *
                 * With rollback enabled an OTA image boots as
                 * ESP_OTA_IMG_PENDING_VERIFY and is reverted at the next restart
                 * unless something confirms it. The only confirmation in this
                 * firmware sat behind `config->ota_enabled && ota_url` — behind
                 * HTTP OTA being configured — so an image pushed over the ESPHome
                 * OTA port would have been taken back silently, and the update
                 * would have looked like it had worked.
                 *
                 * Confirming at this point rather than at startup makes the
                 * criterion "Wi-Fi is up and the API is listening", which is
                 * precisely what installing a further update requires. An image
                 * that cannot manage that is one the bootloader should undo. */
                {
                    const esp_partition_t *running = esp_ota_get_running_partition();
                    esp_ota_img_states_t ota_state;
                    if (running != NULL &&
                        esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
                        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                        esp_err_t mv = esp_ota_mark_app_valid_cancel_rollback();
                        ESP_LOGW(TAG_OTA, "New image confirmed, rollback cancelled: %s",
                                 esp_err_to_name(mv));
                    }
                }

#if CONFIG_BT_SCANNER_ENABLED
                /* Initialize BLE Proxy only if BLE stack is available */
                if (ble_initialized) {
                    esphome_ble_proxy_config_t proxy_config = ESPHOME_BLE_PROXY_CONFIG_DEFAULT();
                    ret = esphome_ble_proxy_init(&proxy_config);
                    if (ret == ESP_OK) {
                        ret = esphome_ble_proxy_start();
                        if (ret == ESP_OK) {
                            ESP_LOGI(TAG_MAIN, "ESPHome BLE Proxy started");
                        } else {
                            ESP_LOGW(TAG_MAIN, "Failed to start BLE proxy: %s",
                                     esp_err_to_name(ret));
                        }
                    } else {
                        ESP_LOGW(TAG_MAIN, "Failed to init BLE proxy: %s",
                                 esp_err_to_name(ret));
                    }
                } else {
                    ESP_LOGW(TAG_MAIN, "BLE Proxy skipped - BLE stack not available");
                }
#endif /* CONFIG_BT_SCANNER_ENABLED */
            } else {
                ESP_LOGE(TAG_MAIN, "Failed to start ESPHome API: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGE(TAG_MAIN, "Failed to init ESPHome API: %s", esp_err_to_name(ret));
        }
    }
#endif /* CONFIG_ESPHOME_API_ENABLE */

    /* Initialize mmWave presence sensor if enabled */
#if CONFIG_MMWAVE_SENSOR_ENABLE
    {
        extern esp_err_t mmwave_sensor_init(void);
        ret = mmwave_sensor_init();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG_MAIN, "mmWave presence sensor initialized");
        } else {
            ESP_LOGW(TAG_MAIN, "mmWave sensor init failed: %s", esp_err_to_name(ret));
        }
    }
#endif

    /* Enter NORMAL operational phase - all services running */
    lifecycle_enter_phase(LIFECYCLE_PHASE_NORMAL);
    ESP_LOGI(TAG_MAIN, "Entered NORMAL operational phase");

    /* Application Ready */
    ESP_LOGI(TAG_MAIN, "========================================");
    ESP_LOGI(TAG_MAIN, "  System Initialization Complete");
    ESP_LOGI(TAG_MAIN, "  ESP32-C5 Zigbee2MQTT Gateway Ready");
    ESP_LOGI(TAG_MAIN, "  Version: %s", version_get_number());
#if CONFIG_ZIGBEE_DEVICE_TYPE_COORDINATOR
    ESP_LOGI(TAG_MAIN, "  Mode: Zigbee Coordinator");
#elif CONFIG_ZIGBEE_DEVICE_TYPE_ROUTER
    ESP_LOGI(TAG_MAIN, "  Mode: Zigbee Router (Mesh Extender)");
#endif
#if CONFIG_BT_SCANNER_ENABLED
    ESP_LOGI(TAG_MAIN, "  BLE: %s (active=%s)",
             ble_scanner_is_running() ? "scanning" : "stopped",
             ble_scanner_is_active_enabled() ? "yes" : "no");
#endif
#if CONFIG_ESPHOME_API_ENABLE
    ESP_LOGI(TAG_MAIN, "  ESPHome: %s (port %d)",
             esphome_api_is_running() ? "running" : "stopped",
             CONFIG_ESPHOME_API_PORT);
#endif
    ESP_LOGI(TAG_MAIN, "========================================");

    /* Main loop - handles deferred MQTT bridge operations
     * Bridge start/resubscribe must happen here (not in MQTT callback)
     * to avoid deadlocks with ESP-MQTT's internal locks */
    while (1) {
        /* Check for pending bridge operations (non-blocking) */
        EventBits_t bits = xEventGroupWaitBits(s_connection_event_group,
                                               MQTT_BRIDGE_START_BIT | MQTT_BRIDGE_RESUB_BIT,
                                               pdTRUE,  /* Clear bits on exit */
                                               pdFALSE, /* Don't wait for all bits */
                                               pdMS_TO_TICKS(GW_MAIN_LOOP_DELAY_MS));

        if (bits & MQTT_BRIDGE_START_BIT) {
            ESP_LOGI(TAG_MQTT, "Starting MQTT bridge from main loop...");
            ret = mqtt_bridge_start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG_MQTT, "Failed to start MQTT bridge: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG_MQTT, "MQTT bridge started successfully");

                /* Enable system monitor MQTT publishing */
                system_monitor_set_mqtt_publishing(true);

                /* Load and publish cached device states */
#if CONFIG_STATE_PERSISTENCE_ENABLE
                if (state_persistence_is_initialized()) {
                    esp_err_t sp_ret = state_persistence_load_and_publish();
                    if (sp_ret == ESP_OK) {
                        ESP_LOGI(TAG_MAIN, "Cached device states published");

                        /* Publishing is not enough on an ESPHome-primary build.
                         * The restore is hooked to the MQTT bridge coming up —
                         * a leftover from when MQTT was the primary integration
                         * — and that moment has nothing to do with whether the
                         * ESPHome entities exist yet. Measured on hardware: the
                         * restore fired at 31.80 s and the entity manager
                         * initialized at 31.73 s, seventy milliseconds apart,
                         * so the restored values landed in a void and every
                         * device read "unknown" in Home Assistant until it
                         * happened to report again.
                         *
                         * Syncing afterwards removes the ordering question
                         * entirely: sync_all_devices() registers whatever is
                         * missing and then applies each device's current
                         * registry state, which the restore has just filled in.
                         * It is idempotent, so doing it here costs nothing when
                         * the entities were already up to date. */
                        esp_err_t sync_ret = esphome_adapter_sync_all_devices();
                        if (sync_ret != ESP_OK) {
                            ESP_LOGW(TAG_MAIN, "Entity resync after restore failed: %s",
                                     esp_err_to_name(sync_ret));
                        }
                    } else if (sp_ret != ESP_ERR_NOT_FOUND) {
                        ESP_LOGW(TAG_MAIN, "State restore failed: %s", esp_err_to_name(sp_ret));
                    }
                }
#endif

                /* Publish crash report */
                esp_err_t crash_ret = crash_reporter_publish();
                if (crash_ret != ESP_OK) {
                    ESP_LOGW(TAG_MAIN, "Failed to publish crash report: %s", esp_err_to_name(crash_ret));
                }
            }
        }

        if (bits & MQTT_BRIDGE_RESUB_BIT) {
            ESP_LOGI(TAG_MQTT, "Resubscribing MQTT bridge from main loop...");
            ret = mqtt_bridge_resubscribe();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG_MQTT, "Failed to resubscribe: %s", esp_err_to_name(ret));
            }
        }
    }
}
