/**
 * @file esphome_adapter_gateway.c
 * @brief Gateway management entities for ESPHome Native API
 *
 * Registers entities on device_id=0 (the gateway itself) for Zigbee network
 * management and diagnostics:
 *   - Permit Join (Switch)
 *   - Permit Join Duration (Number)
 *   - Network Heal (Button)
 *   - Zigbee Channel (Sensor, diagnostic)
 *   - Device Count (Sensor, diagnostic)
 *   - Network PAN ID (TextSensor, diagnostic)
 *   - Coordinator State (TextSensor, diagnostic)
 *   - Last Reset Reason (TextSensor, diagnostic)
 *   - Boot Count (Sensor, diagnostic)
 *   - WiFi Disconnects (Sensor, diagnostic)
 *   - WiFi Last Disconnect Reason (TextSensor, diagnostic)
 */

#include "esphome_adapter_gateway.h"
#include "wifi/wifi_manager.h"
#include "core/monitoring/crash_reporter.h"
#include "esphome/esphome_entities.h"
#include "ota/converter_db_ota.h"
#include "esphome/esphome_entities_types.h"
#include "esphome/esphome_ota.h"
#include "zigbee/zb_coordinator.h"
#include "zigbee/zb_network.h"
#include "zigbee/zb_topology.h"
#include "core/device/device_registry.h"
#include "core/events/event_bus.h"
#include "core/led_status_manager.h"
#include "core/bridge/bridge_request_handler.h"
#include "esphome/esphome_api.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#if CONFIG_BT_SCANNER_ENABLED
#include "bluetooth/ble_scanner.h"
#endif

#if CONFIG_MMWAVE_SENSOR_ENABLE
#include "mmwave/mmwave_sensor.h"
#endif

static const char *TAG = "gw_entities";

/* ============================================================================
 * Key Definitions (0x4XXXXXXX range)
 * ============================================================================ */

#define GW_KEY_PERMIT_JOIN          0x40000001
#define GW_KEY_PERMIT_JOIN_DURATION 0x40000002
#define GW_KEY_NETWORK_HEAL        0x40000003
#define GW_KEY_ZIGBEE_CHANNEL      0x40000004
#define GW_KEY_DEVICE_COUNT        0x40000005
#define GW_KEY_PAN_ID              0x40000006
#define GW_KEY_COORDINATOR_STATE   0x40000007

#define GW_KEY_OTA_MODE            0x40000008
#define GW_KEY_RESET_REASON        0x40000009
#define GW_KEY_BOOT_COUNT          0x4000000C
#define GW_KEY_WIFI_DISCONNECTS    0x4000000D
#define GW_KEY_WIFI_LAST_REASON    0x4000000E

#if CONFIG_MMWAVE_SENSOR_ENABLE
#define GW_KEY_BLE_SCANNER         0x4000000A
#define GW_KEY_BLE_ACTIVE_SCAN     0x4000000B

#define GW_KEY_MMWAVE_PRESENCE     0x40000010
#define GW_KEY_MMWAVE_DISTANCE     0x40000011

/* Actions and diagnostics that used to exist only over MQTT.
 *
 * ha_bridge_discovery.c publishes 37 entities for this gateway, and none of
 * them were reachable over the ESPHome API — including the factory reset. On a
 * gateway configured with ESPHome as the primary integration, that means the
 * one action you need when the network is in a bad state lives on the
 * transport most likely to be unavailable. */
#define GW_KEY_FACTORY_RESET       0x40000012
#define GW_KEY_NETWORK_RESET       0x40000013
#define GW_KEY_CONFIG_RESET        0x40000014
#define GW_KEY_RESTART             0x40000015
#define GW_KEY_WAS_CRASH           0x40000016
#define GW_KEY_TIME_SYNCED         0x40000017
#define GW_KEY_WIFI_BAND           0x40000018
#define GW_KEY_WIFI_IP             0x40000019
#define GW_KEY_WIFI_HOSTNAME       0x4000001A
#define GW_KEY_WIFI_QUALITY        0x4000001B
#define GW_KEY_WIFI_UPTIME         0x4000001C
#define GW_KEY_WIFI_CONNECTS       0x4000001D
#define GW_KEY_API_CLIENTS         0x4000001E
#define GW_KEY_BLE_STATUS          0x4000001F
#define GW_KEY_BLE_DEVICES         0x40000020
#define GW_KEY_CURRENT_TIME        0x40000021
#define GW_KEY_DB_OTA_STATUS       0x40000022
#define GW_KEY_DB_UPDATE_AVAILABLE 0x40000023
#define GW_KEY_DB_UPDATE_INSTALL   0x40000024
#endif

/* ============================================================================
 * State
 * ============================================================================ */

static bool s_gw_initialized = false;
static float s_permit_join_duration = 120.0f;  /* Default 120s */
static bool s_ota_mode_active = false;
static bool s_ota_initialized = false;
static esp_timer_handle_t s_ota_timeout_timer = NULL;

/** @brief Pushes the gateway diagnostics on a fixed period */
static esp_timer_handle_t s_diag_timer = NULL;

/** @brief How often the gateway diagnostics are refreshed */
#define GW_DIAG_INTERVAL_S      30

#define OTA_MODE_TIMEOUT_S  600  /* 10 minutes auto-disable */

#if CONFIG_MMWAVE_SENSOR_ENABLE
static void mmwave_state_changed(const mmwave_state_t *state)
{
    if (!s_gw_initialized || !state) return;
    esphome_entity_update_binary_sensor(GW_KEY_MMWAVE_PRESENCE, state->presence);
    if (state->presence) {
        esphome_entity_update_sensor(GW_KEY_MMWAVE_DISTANCE, (float)state->distance_cm);
    }
}
#endif

/* ============================================================================
 * Command Callbacks
 * ============================================================================ */

static esp_err_t gw_permit_join_command(esphome_entity_key_t key, bool state)
{
    (void)key;
    uint8_t duration = state ? (uint8_t)s_permit_join_duration : 0;
    ESP_LOGI(TAG, "Permit join: %s (duration=%u)", state ? "ON" : "OFF", duration);

    esp_err_t ret = zb_coordinator_permit_join(duration);
    if (ret == ESP_OK) {
        esphome_entity_update_switch(GW_KEY_PERMIT_JOIN, state);
    }
    return ret;
}

static esp_err_t gw_permit_join_duration_command(esphome_entity_key_t key, float value)
{
    (void)key;
    if (value < 5.0f) value = 5.0f;
    if (value > 254.0f) value = 254.0f;
    s_permit_join_duration = value;
    ESP_LOGI(TAG, "Permit join duration set to %u s", (unsigned)value);
    esphome_entity_update_number(GW_KEY_PERMIT_JOIN_DURATION, value);
    return ESP_OK;
}

static esp_err_t gw_network_heal_command(esphome_entity_key_t key)
{
    (void)key;
    ESP_LOGI(TAG, "Network heal (topology scan) requested");
    return zb_topology_scan(NULL);
}

/* ============================================================================
 * OTA Mode
 * ============================================================================ */

static void ota_mode_disable(void)
{
    if (!s_ota_mode_active) return;

    ESP_LOGI(TAG, "OTA Mode: disabling");

    /* Stop timeout timer */
    if (s_ota_timeout_timer) {
        esp_timer_stop(s_ota_timeout_timer);
    }

    /* Stop OTA server */
    esphome_ota_stop();

    /* Clear LED */
    led_status_manager_set_condition(LED_COND_OTA_ACTIVE, false);

    /* Restart BLE scanner */
#if CONFIG_BT_SCANNER_ENABLED
    if (!ble_scanner_is_running()) {
        ble_scanner_start();
        ESP_LOGI(TAG, "OTA Mode: BLE scanner restarted");
    }
#endif

    s_ota_mode_active = false;
    esphome_entity_update_switch(GW_KEY_OTA_MODE, false);
    ESP_LOGI(TAG, "OTA Mode: disabled");
}

static void ota_timeout_callback(void *arg)
{
    (void)arg;
    /* Don't disable if an OTA transfer is actively in progress */
    if (esphome_ota_in_progress()) {
        ESP_LOGI(TAG, "OTA Mode: timeout fired but transfer in progress, extending...");
        esp_timer_start_once(s_ota_timeout_timer, (uint64_t)OTA_MODE_TIMEOUT_S * 1000000);
        return;
    }
    ESP_LOGW(TAG, "OTA Mode: timeout after %d seconds, disabling", OTA_MODE_TIMEOUT_S);
    ota_mode_disable();
}

static esp_err_t gw_ota_mode_command(esphome_entity_key_t key, bool state)
{
    (void)key;

    if (state) {
        ESP_LOGI(TAG, "OTA Mode: enabling (timeout=%ds)", OTA_MODE_TIMEOUT_S);

        /* Stop BLE scanner to free RAM */
#if CONFIG_BT_SCANNER_ENABLED
        if (ble_scanner_is_running()) {
            ble_scanner_stop();
            ESP_LOGI(TAG, "OTA Mode: BLE scanner stopped to free RAM");
        }
#endif

        /* Initialize OTA server on first use */
        if (!s_ota_initialized) {
            esphome_ota_config_t ota_cfg = ESPHOME_OTA_CONFIG_DEFAULT();
            esp_err_t ret = esphome_ota_init(&ota_cfg);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "OTA init failed: %s", esp_err_to_name(ret));
#if CONFIG_BT_SCANNER_ENABLED
                ble_scanner_start();
#endif
                return ret;
            }
            s_ota_initialized = true;
        }

        /* Start OTA server */
        esp_err_t ret = esphome_ota_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA start failed: %s", esp_err_to_name(ret));
#if CONFIG_BT_SCANNER_ENABLED
            ble_scanner_start();
#endif
            return ret;
        }

        /* Set LED to red pulsing */
        led_status_manager_set_condition(LED_COND_OTA_ACTIVE, true);

        /* Start timeout timer */
        if (!s_ota_timeout_timer) {
            esp_timer_create_args_t timer_args = {
                .callback = ota_timeout_callback,
                .name = "ota_timeout",
            };
            esp_timer_create(&timer_args, &s_ota_timeout_timer);
        }
        esp_timer_stop(s_ota_timeout_timer);
        esp_timer_start_once(s_ota_timeout_timer, (uint64_t)OTA_MODE_TIMEOUT_S * 1000000);

        s_ota_mode_active = true;
        esphome_entity_update_switch(GW_KEY_OTA_MODE, true);
        ESP_LOGI(TAG, "OTA Mode: active on port 3232");
    } else {
        ota_mode_disable();
    }

    return ESP_OK;
}

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

static void on_permit_join_event(event_type_t type, void *data, size_t data_size, void *user_ctx)
{
    (void)type;
    (void)user_ctx;

    if (!data || data_size < sizeof(bool)) return;

    /* EVT_ZB_PERMIT_JOIN payload starts with bool enabled */
    const bool *enabled = (const bool *)data;
    esphome_entity_update_switch(GW_KEY_PERMIT_JOIN, *enabled);
}

static void on_device_count_change(event_type_t type, void *data, size_t data_size, void *user_ctx)
{
    (void)type;
    (void)data;
    (void)data_size;
    (void)user_ctx;

    /* Update device count whenever a device joins or leaves */
    size_t count = device_registry_count();
    esphome_entity_update_sensor(GW_KEY_DEVICE_COUNT, (float)count);
}

static void diag_timer_cb(void *arg)
{
    (void)arg;
    esphome_adapter_gateway_update_state();
}

/* ============================================================================
 * Destructive actions
 * ============================================================================ */

/**
 * @brief Run a reset on its own task
 *
 * Every one of these ends in esp_restart(), and factory reset spends seconds
 * erasing NVS and two flash partitions first. Running that inside the button
 * callback would block the ESPHome client task that still holds the
 * connection, so Home Assistant would never see the press acknowledged and
 * would report a failure for an action that in fact succeeded.
 *
 * The short delay serves the same purpose: it lets the response leave before
 * the device stops answering.
 */
static void reset_task(void *arg)
{
    esp_err_t (*aktion)(void) = (esp_err_t (*)(void))arg;
    vTaskDelay(pdMS_TO_TICKS(700));
    aktion();
    vTaskDelete(NULL);   /* not reached: aktion() restarts the device */
}

static esp_err_t start_reset(esp_err_t (*aktion)(void), const char *was)
{
    ESP_LOGW(TAG, "%s requested from Home Assistant", was);
    if (xTaskCreate(reset_task, "gw_reset", 4096, (void *)aktion, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start reset task - running inline");
        return aktion();
    }
    return ESP_OK;
}

static esp_err_t gw_factory_reset_command(esphome_entity_key_t key)
{
    (void)key;
    return start_reset(bridge_request_factory_reset, "Factory reset");
}

static esp_err_t gw_network_reset_command(esphome_entity_key_t key)
{
    (void)key;
    return start_reset(bridge_request_network_reset, "Network reset");
}

static esp_err_t gw_config_reset_command(esphome_entity_key_t key)
{
    (void)key;
    return start_reset(bridge_request_config_reset, "Config reset");
}

static esp_err_t gw_restart_command(esphome_entity_key_t key)
{
    (void)key;
    return start_reset(bridge_request_restart, "Restart");
}

/* ============================================================================
 * Registration
 * ============================================================================ */


#if CONFIG_BT_SCANNER_ENABLED
/** @brief Start/stop BLE scanning from Home Assistant. */
static esp_err_t gw_ble_scanner_command(esphome_entity_key_t key, bool state)
{
    (void)key;
    esp_err_t ret = state ? ble_scanner_start() : ble_scanner_stop();
    if (ret == ESP_OK) {
        esphome_entity_update_switch(GW_KEY_BLE_SCANNER, state);
        ESP_LOGI(TAG, "BLE scanner %s via Home Assistant", state ? "started" : "stopped");
    } else {
        ESP_LOGW(TAG, "BLE scanner %s failed: %s",
                 state ? "start" : "stop", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief Switch active scanning from Home Assistant
 *
 * Active scanning transmits scan requests, so on this part it competes with
 * Zigbee for the 2.4 GHz radio. Making it a switch rather than a build-time
 * option means the trade can be made while watching what it costs, instead of
 * being decided once at compile time.
 */
/**
 * @brief Fetch the converter database from CONFIG_CONVERTER_DB_UPDATE_URL
 */
static esp_err_t gw_db_update_command(esphome_entity_key_t key)
{
    (void)key;
    const char *url = CONFIG_CONVERTER_DB_UPDATE_URL;
    if (url[0] == '\0') {
        ESP_LOGW(TAG, "No converter database source configured");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Converter database update requested: %s", url);
    return converter_db_ota_start(url);
}

static esp_err_t gw_ble_active_scan_command(esphome_entity_key_t key, bool state)
{
    (void)key;
    ble_scanner_set_active_mode(state);
    esphome_entity_update_switch(GW_KEY_BLE_ACTIVE_SCAN, state);
    ESP_LOGI(TAG, "BLE %s scanning selected via Home Assistant",
             state ? "active" : "passive");
    return ESP_OK;
}
#endif

esp_err_t esphome_adapter_gateway_register(void)
{
    if (s_gw_initialized) {
        return ESP_OK;  /* Already registered */
    }

    ESP_LOGI(TAG, "Registering gateway management entities");

    /* --- Permit Join Switch --- */
    {
        esphome_switch_config_t cfg = {0};
        cfg.key = GW_KEY_PERMIT_JOIN;
        cfg.device_id = 0;
        strncpy(cfg.name, "Permit Join", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_permit_join");
        strncpy(cfg.icon, "mdi:plus-network", sizeof(cfg.icon) - 1);
        cfg.assumed_state = false;
        cfg.disabled_by_default = false;
        cfg.command_callback = gw_permit_join_command;
        esphome_entity_register_switch(&cfg);
        esphome_entity_update_switch(GW_KEY_PERMIT_JOIN, zb_coordinator_is_permit_join_enabled());
    }


#if CONFIG_BT_SCANNER_ENABLED
    /* --- BLE Scanner Switch --- */
    {
        esphome_switch_config_t cfg = {0};
        cfg.key = GW_KEY_BLE_SCANNER;
        cfg.device_id = 0;
        strncpy(cfg.name, "BLE Scanner", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_ble_scanner");
        strncpy(cfg.icon, "mdi:bluetooth", sizeof(cfg.icon) - 1);
        cfg.entity_category = 1;  /* CONFIG */
        cfg.command_callback = gw_ble_scanner_command;
        esphome_entity_register_switch(&cfg);
        esphome_entity_update_switch(GW_KEY_BLE_SCANNER,
                                     ble_scanner_get_state() == BLE_SCANNER_STATE_RUNNING);
    }

    /* --- BLE Active Scan Switch --- */
    {
        esphome_switch_config_t cfg = {0};
        cfg.key = GW_KEY_BLE_ACTIVE_SCAN;
        cfg.device_id = 0;
        strncpy(cfg.name, "BLE Active Scan", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_ble_active_scan");
        strncpy(cfg.icon, "mdi:bluetooth-transfer", sizeof(cfg.icon) - 1);
        cfg.entity_category = 1;  /* CONFIG */
        cfg.command_callback = gw_ble_active_scan_command;
        esphome_entity_register_switch(&cfg);
        esphome_entity_update_switch(GW_KEY_BLE_ACTIVE_SCAN,
                                     ble_scanner_is_active_enabled());
    }
#endif

    /* --- Permit Join Duration Number --- */
    {
        esphome_number_config_t cfg = {0};
        cfg.key = GW_KEY_PERMIT_JOIN_DURATION;
        cfg.device_id = 0;
        strncpy(cfg.name, "Permit Join Duration", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_permit_join_duration");
        strncpy(cfg.icon, "mdi:timer-outline", sizeof(cfg.icon) - 1);
        strncpy(cfg.unit_of_measurement, "s", sizeof(cfg.unit_of_measurement) - 1);
        cfg.min_value = 5.0f;
        cfg.max_value = 254.0f;
        cfg.step = 1.0f;
        cfg.mode = ESPHOME_NUMBER_MODE_SLIDER;
        cfg.disabled_by_default = false;
        cfg.command_callback = gw_permit_join_duration_command;
        esphome_entity_register_number(&cfg);
        esphome_entity_update_number(GW_KEY_PERMIT_JOIN_DURATION, s_permit_join_duration);
    }

    /* --- Network Heal Button --- */
    {
        esphome_button_config_t cfg = {0};
        cfg.key = GW_KEY_NETWORK_HEAL;
        cfg.device_id = 0;
        strncpy(cfg.name, "Network Heal", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_network_heal");
        strncpy(cfg.icon, "mdi:heart-pulse", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BUTTON_CLASS_NONE;
        cfg.disabled_by_default = false;
        cfg.press_callback = gw_network_heal_command;
        esphome_entity_register_button(&cfg);
    }

    /* --- Zigbee Channel Sensor (diagnostic) --- */
    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_ZIGBEE_CHANNEL;
        cfg.device_id = 0;
        strncpy(cfg.name, "Zigbee Channel", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_zigbee_channel");
        strncpy(cfg.icon, "mdi:antenna", sizeof(cfg.icon) - 1);
        cfg.accuracy_decimals = 0;
        cfg.device_class = ESPHOME_SENSOR_CLASS_NONE;
        cfg.state_class = ESPHOME_STATE_CLASS_NONE;
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }

    /* --- Device Count Sensor (diagnostic) --- */
    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_DEVICE_COUNT;
        cfg.device_id = 0;
        strncpy(cfg.name, "Zigbee Device Count", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_device_count");
        strncpy(cfg.icon, "mdi:counter", sizeof(cfg.icon) - 1);
        cfg.accuracy_decimals = 0;
        cfg.device_class = ESPHOME_SENSOR_CLASS_NONE;
        cfg.state_class = ESPHOME_STATE_CLASS_MEASUREMENT;
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }

    /* --- PAN ID TextSensor (diagnostic) --- */
    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_PAN_ID;
        cfg.device_id = 0;
        strncpy(cfg.name, "Network PAN ID", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_pan_id");
        strncpy(cfg.icon, "mdi:identifier", sizeof(cfg.icon) - 1);
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    /* --- Coordinator State TextSensor (diagnostic) --- */
    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_COORDINATOR_STATE;
        cfg.device_id = 0;
        strncpy(cfg.name, "Coordinator State", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_coordinator_state");
        strncpy(cfg.icon, "mdi:access-point-network", sizeof(cfg.icon) - 1);
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    /* --- Converter DB Update TextSensor (diagnostic) ---
     *
     * update_converter_db() runs on its own task and reports what happened
     * through converter_db_ota_status(). Nothing read that string, so a failed
     * database update looked exactly like a successful one from Home Assistant:
     * the service call returned, and the only account of what went wrong went
     * to a serial console nobody was attached to. */
    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_DB_OTA_STATUS;
        cfg.device_id = 0;
        strncpy(cfg.name, "Converter DB Update", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_db_ota_status");
        strncpy(cfg.icon, "mdi:database-sync", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    /* --- Converter DB: is there a newer one, and fetch it --- */
    {
        esphome_binary_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_DB_UPDATE_AVAILABLE;
        cfg.device_id = 0;
        strncpy(cfg.name, "Converter DB Update Available", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_db_update_available");
        strncpy(cfg.icon, "mdi:database-arrow-up", sizeof(cfg.icon) - 1);
        /* No device class: Home Assistant has no binary class for "an update
         * exists", and PROBLEM would say something this is not. */
        cfg.device_class = ESPHOME_BINARY_CLASS_NONE;
        cfg.disabled_by_default = false;
        esphome_entity_register_binary_sensor(&cfg);
    }
    {
        esphome_button_config_t cfg = {0};
        cfg.key = GW_KEY_DB_UPDATE_INSTALL;
        cfg.device_id = 0;
        strncpy(cfg.name, "Update Converter Database", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_db_update_install");
        strncpy(cfg.icon, "mdi:database-sync", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BUTTON_CLASS_UPDATE;
        cfg.disabled_by_default = false;
        cfg.press_callback = gw_db_update_command;
        esphome_entity_register_button(&cfg);
    }

    /* --- Last Reset Reason TextSensor (diagnostic) ---
     *
     * The crash reporter has always known this and only ever published it over
     * MQTT, which an ESPHome-primary build switches off. So a gateway that
     * restarted overnight gave its owner no way to tell a crash from a power
     * cut from a watchdog reboot — the exact question that took a broker and a
     * serial cable to answer here. The string carries the crash marker, because
     * "was this a crash" is the first thing anyone wants to know. */
    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_RESET_REASON;
        cfg.device_id = 0;
        strncpy(cfg.name, "Last Reset Reason", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_reset_reason");
        strncpy(cfg.icon, "mdi:restart-alert", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    /* --- Boot Count Sensor (diagnostic) --- */
    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_BOOT_COUNT;
        cfg.device_id = 0;
        strncpy(cfg.name, "Boot Count", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_boot_count");
        strncpy(cfg.icon, "mdi:counter", sizeof(cfg.icon) - 1);
        cfg.accuracy_decimals = 0;
        cfg.state_class = ESPHOME_STATE_CLASS_TOTAL_INCREASING;
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }

    /* --- WiFi Disconnects + Last Reason (diagnostic) ---
     *
     * This chip loses its access point every nine to fourteen hours with the AP
     * sitting at -50 dBm, and until now the only way to see that was a serial
     * cable. The watchdog recovers it, so a user may never notice — but they
     * should be able to find out, and "how often" should be a number they can
     * read rather than something someone infers from logs. */
    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_WIFI_DISCONNECTS;
        cfg.device_id = 0;
        strncpy(cfg.name, "WiFi Disconnects", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_wifi_disconnects");
        strncpy(cfg.icon, "mdi:wifi-remove", sizeof(cfg.icon) - 1);
        cfg.accuracy_decimals = 0;
        cfg.state_class = ESPHOME_STATE_CLASS_TOTAL_INCREASING;
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }
    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_WIFI_LAST_REASON;
        cfg.device_id = 0;
        strncpy(cfg.name, "WiFi Last Disconnect Reason", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_wifi_last_reason");
        strncpy(cfg.icon, "mdi:wifi-alert", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    /* --- OTA Mode Switch --- */
    {
        esphome_switch_config_t cfg = {0};
        cfg.key = GW_KEY_OTA_MODE;
        cfg.device_id = 0;
        strncpy(cfg.name, "OTA Mode", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_ota_mode");
        strncpy(cfg.icon, "mdi:upload-network", sizeof(cfg.icon) - 1);
        cfg.assumed_state = false;
        cfg.disabled_by_default = false;
        cfg.command_callback = gw_ota_mode_command;
        esphome_entity_register_switch(&cfg);
        esphome_entity_update_switch(GW_KEY_OTA_MODE, false);
    }

    /* --- mmWave Presence Sensor entities --- */
#if CONFIG_MMWAVE_SENSOR_ENABLE
    {
        esphome_binary_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_MMWAVE_PRESENCE;
        cfg.device_id = 0;
        strncpy(cfg.name, "Presence", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_mmwave_presence");
        strncpy(cfg.icon, "mdi:motion-sensor", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BINARY_CLASS_OCCUPANCY;
        cfg.disabled_by_default = false;
        esphome_entity_register_binary_sensor(&cfg);
    }
    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_MMWAVE_DISTANCE;
        cfg.device_id = 0;
        strncpy(cfg.name, "Presence Distance", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_mmwave_distance");
        strncpy(cfg.icon, "mdi:signal-distance-variant", sizeof(cfg.icon) - 1);
        strncpy(cfg.unit_of_measurement, "cm", sizeof(cfg.unit_of_measurement) - 1);
        cfg.accuracy_decimals = 0;
        cfg.device_class = ESPHOME_SENSOR_CLASS_DISTANCE;
        cfg.state_class = ESPHOME_STATE_CLASS_MEASUREMENT;
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }
#endif

#if CONFIG_MMWAVE_SENSOR_ENABLE
    /* Register mmWave state change callback for real-time ESPHome updates */
    mmwave_sensor_set_callback(mmwave_state_changed);
#endif

    /* Subscribe to events for dynamic updates */
    event_subscribe(EVT_ZB_PERMIT_JOIN, on_permit_join_event, NULL);
    event_subscribe(EVT_DEVICE_JOINED, on_device_count_change, NULL);
    event_subscribe(EVT_DEVICE_LEFT, on_device_count_change, NULL);

    /* Push initial values */
    esphome_adapter_gateway_update_state();

    /* Keep pushing them.
     *
     * This used to be the only call, which meant every gateway diagnostic was
     * a snapshot of the first seconds after boot: Wi-Fi band, IP address and
     * signal quality were captured before the station had even associated, and
     * then never corrected. Boot count and reset reason are genuinely fixed
     * for the life of a boot, but the rest is not, and a diagnostic that never
     * changes is worse than none — it looks current. */
    if (s_diag_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = diag_timer_cb,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "gw_diag",
        };
        if (esp_timer_create(&args, &s_diag_timer) == ESP_OK) {
            esp_timer_start_periodic(s_diag_timer,
                                     (uint64_t)GW_DIAG_INTERVAL_S * 1000000);
            ESP_LOGI(TAG, "Gateway diagnostics refresh every %d s", GW_DIAG_INTERVAL_S);
        } else {
            ESP_LOGW(TAG, "No diagnostics timer — values stay at their boot state");
        }
    }

    s_gw_initialized = true;

    /* --- Actions that previously existed only over MQTT --- */

    /* Factory Reset is disabled by default on purpose.
     *
     * It erases NVS, both Zigbee storage partitions and the persisted device
     * state — the whole Zigbee network, every pairing. There is no undo and no
     * confirmation dialog on a Home Assistant button, so it should not sit on a
     * dashboard next to Permit Join waiting to be hit by accident. Enabling it
     * is one toggle in Home Assistant, and that toggle is the confirmation. */

    {
        esphome_button_config_t cfg = {0};
        cfg.key = GW_KEY_FACTORY_RESET;
        cfg.device_id = 0;
        strncpy(cfg.name, "Factory Reset", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_factory_reset");
        strncpy(cfg.icon, "mdi:nuke", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BUTTON_CLASS_NONE;
        cfg.disabled_by_default = true;
        cfg.press_callback = gw_factory_reset_command;
        esphome_entity_register_button(&cfg);
    }

    /* Network Reset keeps Wi-Fi and gateway settings, drops the Zigbee network.
     * Also destructive, also disabled by default. */

    {
        esphome_button_config_t cfg = {0};
        cfg.key = GW_KEY_NETWORK_RESET;
        cfg.device_id = 0;
        strncpy(cfg.name, "Network Reset", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_network_reset");
        strncpy(cfg.icon, "mdi:lan-disconnect", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BUTTON_CLASS_NONE;
        cfg.disabled_by_default = true;
        cfg.press_callback = gw_network_reset_command;
        esphome_entity_register_button(&cfg);
    }

    /* Config Reset is the mirror image: paired devices stay, the Wi-Fi
     * credentials go. Recovering from it needs the captive portal. */

    {
        esphome_button_config_t cfg = {0};
        cfg.key = GW_KEY_CONFIG_RESET;
        cfg.device_id = 0;
        strncpy(cfg.name, "Config Reset", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_config_reset");
        strncpy(cfg.icon, "mdi:cog-refresh", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BUTTON_CLASS_NONE;
        cfg.disabled_by_default = true;
        cfg.press_callback = gw_config_reset_command;
        esphome_entity_register_button(&cfg);
    }

    /* Restart is harmless and stays enabled. */

    {
        esphome_button_config_t cfg = {0};
        cfg.key = GW_KEY_RESTART;
        cfg.device_id = 0;
        strncpy(cfg.name, "Restart", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_restart");
        strncpy(cfg.icon, "mdi:restart", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BUTTON_CLASS_NONE;
        cfg.disabled_by_default = false;
        cfg.press_callback = gw_restart_command;
        esphome_entity_register_button(&cfg);
    }

    /* --- Diagnostics that previously existed only over MQTT --- */

    {
        esphome_binary_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_WAS_CRASH;
        cfg.device_id = 0;
        strncpy(cfg.name, "Last Reset Was Crash", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_was_crash");
        strncpy(cfg.icon, "mdi:alert-octagon", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_binary_sensor(&cfg);
    }

    {
        esphome_binary_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_TIME_SYNCED;
        cfg.device_id = 0;
        strncpy(cfg.name, "Time Synchronized", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_time_synced");
        strncpy(cfg.icon, "mdi:clock-check", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_binary_sensor(&cfg);
    }

    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_WIFI_BAND;
        cfg.device_id = 0;
        strncpy(cfg.name, "WiFi Band", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_wifi_band");
        strncpy(cfg.icon, "mdi:wifi", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_WIFI_IP;
        cfg.device_id = 0;
        strncpy(cfg.name, "WiFi IP", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_wifi_ip");
        strncpy(cfg.icon, "mdi:ip-network", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_WIFI_HOSTNAME;
        cfg.device_id = 0;
        strncpy(cfg.name, "WiFi Hostname", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_wifi_hostname");
        strncpy(cfg.icon, "mdi:dns", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_CURRENT_TIME;
        cfg.device_id = 0;
        strncpy(cfg.name, "Current Time", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_current_time");
        strncpy(cfg.icon, "mdi:clock", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_BLE_STATUS;
        cfg.device_id = 0;
        strncpy(cfg.name, "BLE Status", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_ble_status");
        strncpy(cfg.icon, "mdi:bluetooth", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_WIFI_QUALITY;
        cfg.device_id = 0;
        strncpy(cfg.name, "WiFi Signal Quality", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_wifi_quality");
        strncpy(cfg.icon, "mdi:wifi-strength-3", sizeof(cfg.icon) - 1);
        strncpy(cfg.unit_of_measurement, "%", sizeof(cfg.unit_of_measurement) - 1);
        cfg.accuracy_decimals = 0;
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }

    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_WIFI_UPTIME;
        cfg.device_id = 0;
        strncpy(cfg.name, "WiFi Uptime", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_wifi_uptime");
        strncpy(cfg.icon, "mdi:timer-outline", sizeof(cfg.icon) - 1);
        strncpy(cfg.unit_of_measurement, "s", sizeof(cfg.unit_of_measurement) - 1);
        cfg.accuracy_decimals = 0;
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }

    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_WIFI_CONNECTS;
        cfg.device_id = 0;
        strncpy(cfg.name, "WiFi Connects", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_wifi_connects");
        strncpy(cfg.icon, "mdi:wifi-plus", sizeof(cfg.icon) - 1);
        cfg.accuracy_decimals = 0;
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }

    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_API_CLIENTS;
        cfg.device_id = 0;
        strncpy(cfg.name, "API Clients", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_api_clients");
        strncpy(cfg.icon, "mdi:account-network", sizeof(cfg.icon) - 1);
        cfg.accuracy_decimals = 0;
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }

    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = GW_KEY_BLE_DEVICES;
        cfg.device_id = 0;
        strncpy(cfg.name, "BLE Devices Seen", sizeof(cfg.name) - 1);
        snprintf(cfg.unique_id, sizeof(cfg.unique_id), "zbgw_ble_devices");
        strncpy(cfg.icon, "mdi:bluetooth-audio", sizeof(cfg.icon) - 1);
        cfg.accuracy_decimals = 0;
        cfg.entity_category = 2;
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }

    ESP_LOGI(TAG, "Gateway entities registered");

    return ESP_OK;
}

/* ============================================================================
 * State Updates
 * ============================================================================ */

void esphome_adapter_gateway_update_state(void)
{
    /* Reset reason and boot count. Both are fixed for the life of this boot,
     * but they are cheap and pushing them here means a reconnecting client sees
     * them without waiting for a restart. */
    if (crash_reporter_is_initialized()) {
        char reason[48];
        snprintf(reason, sizeof(reason), "%s%s",
                 crash_reporter_get_reason_str(),
                 crash_reporter_was_crash() ? " (crash)" : "");
        esphome_entity_update_text_sensor(GW_KEY_RESET_REASON, reason);
        esphome_entity_update_sensor(GW_KEY_BOOT_COUNT,
                                     (float)crash_reporter_get_boot_count());
    }

    /* Was the last reset a crash? Cheap, and it is the first thing anyone
     * wants to know when a gateway reappears unexpectedly. */
    if (crash_reporter_is_initialized()) {
        esphome_entity_update_binary_sensor(GW_KEY_WAS_CRASH, crash_reporter_was_crash());
    }

    /* WiFi statistics */
    {
        wifi_stats_t ws = {0};
        if (wifi_manager_get_stats(&ws) == ESP_OK) {
            esphome_entity_update_sensor(GW_KEY_WIFI_DISCONNECTS, (float)ws.disconnect_count);
            esphome_entity_update_text_sensor(GW_KEY_WIFI_LAST_REASON,
                wifi_manager_disconnect_reason_str(ws.last_disconnect_reason));
            esphome_entity_update_sensor(GW_KEY_WIFI_QUALITY, (float)ws.signal_quality);
            esphome_entity_update_sensor(GW_KEY_WIFI_UPTIME, (float)ws.uptime_seconds);
            esphome_entity_update_sensor(GW_KEY_WIFI_CONNECTS, (float)ws.connect_count);
        }
    }

    /* Band, derived from the channel the station actually landed on.
     *
     * Not from the configured preference: this project has already shipped a
     * setting that said 5 GHz while the driver sat on 2.4, and a diagnostic
     * that repeats the configuration instead of measuring it would have hidden
     * exactly that. */
    if (wifi_manager_is_connected()) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            esphome_entity_update_text_sensor(GW_KEY_WIFI_BAND,
                ap.primary >= WIFI_MGR_5GHZ_CHANNEL_MIN ? "5GHz" : "2.4GHz");
        } else {
            esphome_entity_update_text_sensor(GW_KEY_WIFI_BAND, "unknown");
        }

        char ip[16] = {0};
        if (wifi_manager_get_ip(ip, sizeof(ip)) == ESP_OK) {
            esphome_entity_update_text_sensor(GW_KEY_WIFI_IP, ip);
        }
    } else {
        esphome_entity_update_text_sensor(GW_KEY_WIFI_BAND, "not connected");
        esphome_entity_update_text_sensor(GW_KEY_WIFI_IP, "-");
    }

    {
        const char *host = wifi_manager_get_hostname();
        esphome_entity_update_text_sensor(GW_KEY_WIFI_HOSTNAME, host ? host : "unknown");
    }

    /* Clock */
    {
        bool synced = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
        esphome_entity_update_binary_sensor(GW_KEY_TIME_SYNCED, synced);

        if (synced) {
            time_t now;
            struct tm tm_utc;
            char stamp[32];
            time(&now);
            gmtime_r(&now, &tm_utc);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%SZ", &tm_utc);
            esphome_entity_update_text_sensor(GW_KEY_CURRENT_TIME, stamp);
        } else {
            esphome_entity_update_text_sensor(GW_KEY_CURRENT_TIME, "not synced");
        }
    }

    /* How many clients hold an API connection. Four slots exist; knowing how
     * many are taken is what turns "Home Assistant cannot connect" from a
     * guess into a reading. */
    esphome_entity_update_sensor(GW_KEY_API_CLIENTS, (float)esphome_api_get_client_count());

    /* BLE */
#if CONFIG_BT_SCANNER_ENABLED
    esphome_entity_update_text_sensor(GW_KEY_BLE_STATUS,
        ble_scanner_is_running() ? "scanning" : "stopped");
    esphome_entity_update_sensor(GW_KEY_BLE_DEVICES, (float)ble_scanner_get_device_count());
#else
    esphome_entity_update_text_sensor(GW_KEY_BLE_STATUS, "disabled");
    esphome_entity_update_sensor(GW_KEY_BLE_DEVICES, 0.0f);
#endif

    /* Channel + PAN ID from network info */
    zb_network_info_t info = {0};
    if (zb_network_get_info(&info) == ESP_OK) {
        esphome_entity_update_sensor(GW_KEY_ZIGBEE_CHANNEL, (float)info.channel);

        char pan_str[8];
        snprintf(pan_str, sizeof(pan_str), "0x%04X", info.pan_id);
        esphome_entity_update_text_sensor(GW_KEY_PAN_ID, pan_str);
    }

    /* Device count */
    size_t count = device_registry_count();
    esphome_entity_update_sensor(GW_KEY_DEVICE_COUNT, (float)count);

    /* Coordinator state */
    zb_coordinator_health_t health = {0};
    if (zb_coordinator_health_check(&health) == ESP_OK) {
        const char *state = health.healthy ? (health.network_up ? "running" : "no_network") : "error";
        esphome_entity_update_text_sensor(GW_KEY_COORDINATOR_STATE, state);
    } else {
        esphome_entity_update_text_sensor(GW_KEY_COORDINATOR_STATE, "unknown");
    }

    /* Converter database: status, and whether the source has something newer.
     *
     * The check costs one index.json — 134 KB — so it runs once a few minutes
     * after boot and then daily, not on every update cycle. Without it the
     * "available" sensor would only ever be right just after somebody pressed
     * something. */
    esphome_entity_update_text_sensor(GW_KEY_DB_OTA_STATUS, converter_db_ota_status());
    esphome_entity_update_binary_sensor(GW_KEY_DB_UPDATE_AVAILABLE,
                                        converter_db_ota_update_available());
    {
        /* Not five minutes after boot. That is exactly when somebody who just
         * restarted the gateway is pairing a device, and the check has no
         * business competing with an interview. Half an hour in, then daily. */
        static int64_t s_next_db_check_us = 1800LL * 1000000LL;
        int64_t now = esp_timer_get_time();
        if (CONFIG_CONVERTER_DB_UPDATE_URL[0] != '\0' && now >= s_next_db_check_us) {
            s_next_db_check_us = now + 24LL * 3600LL * 1000000LL;
            converter_db_ota_check(CONFIG_CONVERTER_DB_UPDATE_URL);
        }
    }

    /* Permit join state */
    esphome_entity_update_switch(GW_KEY_PERMIT_JOIN, zb_coordinator_is_permit_join_enabled());

    /* mmWave presence sensor */
#if CONFIG_MMWAVE_SENSOR_ENABLE
    if (mmwave_sensor_is_connected()) {
        mmwave_state_t mw = {0};
        mmwave_sensor_get_state(&mw);
        esphome_entity_update_binary_sensor(GW_KEY_MMWAVE_PRESENCE, mw.presence);
        esphome_entity_update_sensor(GW_KEY_MMWAVE_DISTANCE, (float)mw.distance_cm);
    }
#endif
}

void esphome_adapter_gateway_deinit(void)
{
    if (!s_gw_initialized) return;

    event_unsubscribe(EVT_ZB_PERMIT_JOIN, on_permit_join_event);
    event_unsubscribe(EVT_DEVICE_JOINED, on_device_count_change);
    event_unsubscribe(EVT_DEVICE_LEFT, on_device_count_change);

    s_gw_initialized = false;
    ESP_LOGI(TAG, "Gateway entities deinitialized");
}
