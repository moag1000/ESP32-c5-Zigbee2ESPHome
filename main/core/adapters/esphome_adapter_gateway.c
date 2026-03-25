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
 */

#include "esphome_adapter_gateway.h"
#include "esphome/esphome_entities.h"
#include "esphome/esphome_entities_types.h"
#include "esphome/esphome_ota.h"
#include "zigbee/zb_coordinator.h"
#include "zigbee/zb_network.h"
#include "zigbee/zb_topology.h"
#include "core/device/device_registry.h"
#include "core/events/event_bus.h"
#include "core/led_status_manager.h"
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

#if CONFIG_MMWAVE_SENSOR_ENABLE
#define GW_KEY_MMWAVE_PRESENCE     0x40000010
#define GW_KEY_MMWAVE_DISTANCE     0x40000011
#endif

/* ============================================================================
 * State
 * ============================================================================ */

static bool s_gw_initialized = false;
static float s_permit_join_duration = 120.0f;  /* Default 120s */
static bool s_ota_mode_active = false;
static bool s_ota_initialized = false;
static esp_timer_handle_t s_ota_timeout_timer = NULL;

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

/* ============================================================================
 * Registration
 * ============================================================================ */

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

    s_gw_initialized = true;
    ESP_LOGI(TAG, "Gateway entities registered");

    return ESP_OK;
}

/* ============================================================================
 * State Updates
 * ============================================================================ */

void esphome_adapter_gateway_update_state(void)
{
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
