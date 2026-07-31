/**
 * @file lifecycle_manager.c
 * @brief Dynamic service lifecycle management implementation
 *
 * @copyright Copyright (c) 2024
 * @license Apache License 2.0
 */

#include "lifecycle_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"

/* Service headers */
#include "state_persistence.h"
#include "monitoring/system_monitor.h"
#include "mqtt_logger.h"
#include "discovery/ha_discovery_ng.h"
#include "wifi/wifi_manager.h"
#include "esp_wifi.h"

#if CONFIG_BT_SCANNER_ENABLED
#include "bluetooth/ble_manager.h"
#include "bluetooth/ble_scanner.h"
#include "bluetooth/ble_gatt_client.h"
#endif

#if CONFIG_ESPHOME_API_ENABLE
#include "esphome/esphome_api.h"
#include "esphome/esphome_ble_proxy.h"
#endif

#include "zigbee/zb_availability.h"
#include "mqtt/mqtt_buffer.h"
#include "mqtt/gateway_mqtt.h"

static const char *TAG = "LIFECYCLE";

/* Service definitions with RAM usage estimates */
static service_info_t s_services[SERVICE_COUNT] = {
    /* Essential services */
    [SERVICE_WIFI]              = { SERVICE_WIFI, "wifi", SERVICE_STATE_STOPPED, 0, false, true },
    [SERVICE_MQTT]              = { SERVICE_MQTT, "mqtt", SERVICE_STATE_STOPPED, 0, false, true },
    [SERVICE_ZIGBEE]            = { SERVICE_ZIGBEE, "zigbee", SERVICE_STATE_STOPPED, 6144, false, true },

    /* Deferrable services - internal RAM stacks */
    [SERVICE_STATE_PERSISTENCE] = { SERVICE_STATE_PERSISTENCE, "state_persist", SERVICE_STATE_STOPPED, 3072, false, false },
    [SERVICE_AVAILABILITY]      = { SERVICE_AVAILABILITY, "availability", SERVICE_STATE_STOPPED, 3072, true, false },

    /* Deferrable services - PSRAM stacks (low RAM impact) */
    [SERVICE_SYSTEM_MONITOR]    = { SERVICE_SYSTEM_MONITOR, "sys_monitor", SERVICE_STATE_STOPPED, 8192, true, false },
    [SERVICE_MQTT_LOGGER]       = { SERVICE_MQTT_LOGGER, "mqtt_logger", SERVICE_STATE_STOPPED, 6144, true, false },
    [SERVICE_HA_DISCOVERY]      = { SERVICE_HA_DISCOVERY, "ha_discovery", SERVICE_STATE_STOPPED, 0, false, false },

    /* BLE services - significant RAM impact */
    [SERVICE_BLE_SCANNER]       = { SERVICE_BLE_SCANNER, "ble_scanner", SERVICE_STATE_STOPPED, 4096, false, false },
    [SERVICE_BLE_GATT]          = { SERVICE_BLE_GATT, "ble_gatt", SERVICE_STATE_STOPPED, 4096, false, false },

    /* ESPHome services */
    [SERVICE_ESPHOME_API]       = { SERVICE_ESPHOME_API, "esphome_api", SERVICE_STATE_STOPPED, 4096, false, false },
    [SERVICE_ESPHOME_BLE_PROXY] = { SERVICE_ESPHOME_BLE_PROXY, "ble_proxy", SERVICE_STATE_STOPPED, 4096, false, false },

    /* Setup-only */
    [SERVICE_CAPTIVE_PORTAL]    = { SERVICE_CAPTIVE_PORTAL, "captive_portal", SERVICE_STATE_STOPPED, 4096, false, false },
};

/* Services to run in each phase */
static const lifecycle_service_t s_phase_services[6][SERVICE_COUNT] = {
    /* BOOT - minimal */
    [LIFECYCLE_PHASE_BOOT] = { SERVICE_COUNT }, /* empty */

    /* SETUP - captive portal only */
    [LIFECYCLE_PHASE_SETUP] = {
        SERVICE_CAPTIVE_PORTAL,
        SERVICE_COUNT
    },

    /* NORMAL - all services.
     * The BLE services are only listed when BT is actually compiled in.
     * Listing them unconditionally made the lifecycle manager request services
     * whose start cases do not exist, so every boot logged
     * "E LIFECYCLE: Failed to start ble_scanner: ESP_ERR_NOT_SUPPORTED" —
     * error-level noise that hides real failures. */
    [LIFECYCLE_PHASE_NORMAL] = {
#if CONFIG_BT_SCANNER_ENABLED
        SERVICE_BLE_SCANNER,
        SERVICE_BLE_GATT,
        SERVICE_ESPHOME_BLE_PROXY,
#endif
        SERVICE_ESPHOME_API,
        SERVICE_HA_DISCOVERY,
        SERVICE_STATE_PERSISTENCE,
        SERVICE_SYSTEM_MONITOR,
        SERVICE_MQTT_LOGGER,
        SERVICE_AVAILABILITY,
        SERVICE_COUNT
    },

    /* PAIRING - keep HA connectivity, stop BLE/heavy services */
    [LIFECYCLE_PHASE_PAIRING] = {
        SERVICE_ESPHOME_API,
        SERVICE_STATE_PERSISTENCE,
        SERVICE_AVAILABILITY,
        SERVICE_MQTT_LOGGER,
        SERVICE_SYSTEM_MONITOR,
        SERVICE_COUNT
    },

    /* OTA - minimal services */
    [LIFECYCLE_PHASE_OTA] = {
        SERVICE_COUNT
    },

    /* LOW_MEMORY - emergency mode */
    [LIFECYCLE_PHASE_LOW_MEMORY] = {
        SERVICE_COUNT
    },
};

static lifecycle_phase_t s_current_phase = LIFECYCLE_PHASE_BOOT;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;

/* Forward declarations for service start/stop functions */
static void sync_service_states(void);
static esp_err_t start_service_internal(lifecycle_service_t service);
static esp_err_t stop_service_internal(lifecycle_service_t service);

esp_err_t lifecycle_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Lifecycle manager initialized");
    return ESP_OK;
}

lifecycle_phase_t lifecycle_get_phase(void)
{
    return s_current_phase;
}

const char *lifecycle_phase_name(lifecycle_phase_t phase)
{
    static const char *names[] = {
        "BOOT", "SETUP", "NORMAL", "PAIRING", "OTA", "LOW_MEMORY"
    };
    if (phase < sizeof(names) / sizeof(names[0])) {
        return names[phase];
    }
    return "UNKNOWN";
}

const char *lifecycle_service_name(lifecycle_service_t service)
{
    if (service < SERVICE_COUNT) {
        return s_services[service].name;
    }
    return "unknown";
}

static bool service_in_phase(lifecycle_service_t service, lifecycle_phase_t phase)
{
    const lifecycle_service_t *list = s_phase_services[phase];
    for (int i = 0; list[i] != SERVICE_COUNT && i < SERVICE_COUNT; i++) {
        if (list[i] == service) {
            return true;
        }
    }
    return false;
}

esp_err_t lifecycle_enter_phase(lifecycle_phase_t phase)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (phase == s_current_phase) {
        return ESP_OK;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    ESP_LOGI(TAG, "Phase transition: %s -> %s",
             lifecycle_phase_name(s_current_phase),
             lifecycle_phase_name(phase));

    /* First time entering NORMAL: sync with services already started by main.c */
    if (phase == LIFECYCLE_PHASE_NORMAL && s_current_phase == LIFECYCLE_PHASE_BOOT) {
        sync_service_states();
    }

    /* Stop services not needed in new phase.
     * Check actual running state, not phase membership - services may have
     * been started directly by main.c before lifecycle manager took over. */
    for (int i = 0; i < SERVICE_COUNT; i++) {
        if (s_services[i].essential) {
            continue; /* Never stop essential services */
        }

        bool should_run = service_in_phase(i, phase);
        bool is_running = (s_services[i].state == SERVICE_STATE_RUNNING);

        /* Stop if currently running but not needed in new phase */
        if (is_running && !should_run) {
            ESP_LOGI(TAG, "Stopping %s (not needed in %s)",
                     s_services[i].name, lifecycle_phase_name(phase));
            stop_service_internal(i);
        }
    }

    /* Start services needed in new phase */
    for (int i = 0; i < SERVICE_COUNT; i++) {
        bool should_run = service_in_phase(i, phase);

        if (should_run && s_services[i].state == SERVICE_STATE_STOPPED) {
            ESP_LOGI(TAG, "Starting %s (needed in %s)",
                     s_services[i].name, lifecycle_phase_name(phase));
            start_service_internal(i);
        }
    }

    /* WiFi management during PAIRING phase:
     *
     * During PAIRING, Zigbee has MIDDLE coex priority (set in zb_coordinator.c).
     * WiFi may occasionally miss beacons and disconnect during intense Zigbee
     * activity. Auto-reconnect stays ENABLED so WiFi recovers automatically. */
    if (phase == LIFECYCLE_PHASE_PAIRING) {
        ESP_LOGI(TAG, "PAIRING phase: WiFi auto-reconnect stays enabled (Zigbee has MIDDLE coex priority)");

        /* Disable power save to reduce WiFi latency during coexistence */
        esp_err_t ret = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "WiFi power save disabled for PAIRING");
        }
    } else if (s_current_phase == LIFECYCLE_PHASE_PAIRING && phase != LIFECYCLE_PHASE_PAIRING) {
        /* Leaving PAIRING phase - re-enable WiFi and reconnect */
        ESP_LOGI(TAG, "Leaving PAIRING phase - restoring WiFi...");

        /* Re-enable auto-reconnect */
        wifi_manager_set_auto_reconnect(true);

        /* Reset to AUTO band mode for reliable reconnect.
         * 5GHz-only mode may fail if signal is weak after Zigbee operations. */
#ifdef CONFIG_WIFI_PREFER_5GHZ
        ESP_LOGI(TAG, "Resetting WiFi band to AUTO for reconnect");
        esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif

        /* Trigger reconnect if not connected */
        if (!wifi_manager_is_connected()) {
            ESP_LOGI(TAG, "WiFi not connected, triggering reconnect...");

            /* Reset the retry counter to get fresh attempts */
            wifi_manager_reset_retry_count();

            esp_err_t ret = wifi_manager_reconnect();
            if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "WiFi reconnect failed: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGI(TAG, "WiFi still connected");
        }
    }

    lifecycle_phase_t prev_phase = s_current_phase;
    s_current_phase = phase;

    xSemaphoreGive(s_mutex);

    /* After returning from PAIRING to NORMAL, republish HA discovery.
     * Devices may have been interviewed during PAIRING when HA discovery
     * event handlers weren't processing events (services stopped). */
    if (prev_phase == LIFECYCLE_PHASE_PAIRING && phase == LIFECYCLE_PHASE_NORMAL) {
        ESP_LOGI(TAG, "Republishing HA discovery after PAIRING phase");
        ha_discovery_ng_publish_all();
    }

    /* Log RAM status after transition */
    ESP_LOGI(TAG, "Phase %s active, services RAM: %lu bytes",
             lifecycle_phase_name(phase), lifecycle_get_services_ram_usage());

    return ESP_OK;
}

uint32_t lifecycle_estimate_ram_freed(lifecycle_phase_t phase)
{
    uint32_t freed = 0;

    for (int i = 0; i < SERVICE_COUNT; i++) {
        if (s_services[i].essential || s_services[i].uses_psram_stack) {
            continue;
        }

        bool currently_running = service_in_phase(i, s_current_phase);
        bool will_run = service_in_phase(i, phase);

        if (currently_running && !will_run) {
            freed += s_services[i].stack_size;
        }
    }

    return freed;
}

bool lifecycle_service_is_running(lifecycle_service_t service)
{
    if (service >= SERVICE_COUNT) {
        return false;
    }
    return s_services[service].state == SERVICE_STATE_RUNNING;
}

esp_err_t lifecycle_get_service_info(lifecycle_service_t service, service_info_t *info)
{
    if (service >= SERVICE_COUNT || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *info = s_services[service];
    return ESP_OK;
}

uint32_t lifecycle_get_services_ram_usage(void)
{
    uint32_t total = 0;
    for (int i = 0; i < SERVICE_COUNT; i++) {
        if (s_services[i].state == SERVICE_STATE_RUNNING && !s_services[i].uses_psram_stack) {
            total += s_services[i].stack_size;
        }
    }
    return total;
}

esp_err_t lifecycle_start_service(lifecycle_service_t service)
{
    if (!s_initialized || service >= SERVICE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    esp_err_t ret = start_service_internal(service);
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t lifecycle_stop_service(lifecycle_service_t service)
{
    if (!s_initialized || service >= SERVICE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_services[service].essential) {
        ESP_LOGW(TAG, "Cannot stop essential service: %s", s_services[service].name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    esp_err_t ret = stop_service_internal(service);
    xSemaphoreGive(s_mutex);
    return ret;
}

/* ============================================================================
 * Internal service start/stop implementations
 * ============================================================================ */

/**
 * @brief Sync service state with actual running status
 *
 * Called when entering NORMAL phase to detect services already started by main.c
 */
static void sync_service_states(void)
{
#if CONFIG_BT_SCANNER_ENABLED
    if (ble_manager_is_initialized()) {
        s_services[SERVICE_BLE_SCANNER].state = SERVICE_STATE_RUNNING;
        s_services[SERVICE_BLE_GATT].state = SERVICE_STATE_RUNNING;
    }
#endif

#if CONFIG_ESPHOME_API_ENABLE
    if (esphome_api_is_running()) {
        s_services[SERVICE_ESPHOME_API].state = SERVICE_STATE_RUNNING;
        s_services[SERVICE_ESPHOME_BLE_PROXY].state = SERVICE_STATE_RUNNING;
    }
#endif

    /* Sync state with actual module running state.
     * Check each module's is_running/is_initialized function to avoid
     * marking as RUNNING when the module wasn't actually started. */
    if (state_persistence_is_initialized()) {
        s_services[SERVICE_STATE_PERSISTENCE].state = SERVICE_STATE_RUNNING;
    }
    /* system_monitor doesn't export is_running, assume started if we get here */
    s_services[SERVICE_SYSTEM_MONITOR].state = SERVICE_STATE_RUNNING;
    /* mqtt_logger doesn't export is_running, assume started if we get here */
    s_services[SERVICE_MQTT_LOGGER].state = SERVICE_STATE_RUNNING;
    /* Check actual availability module state */
    if (zb_availability_is_running()) {
        s_services[SERVICE_AVAILABILITY].state = SERVICE_STATE_RUNNING;
    }
    s_services[SERVICE_HA_DISCOVERY].state = SERVICE_STATE_RUNNING;

    ESP_LOGI(TAG, "Synced service states with actual running status");
}

static esp_err_t start_service_internal(lifecycle_service_t service)
{
    esp_err_t ret = ESP_OK;
    s_services[service].state = SERVICE_STATE_STARTING;

    switch (service) {
        case SERVICE_STATE_PERSISTENCE:
            ret = state_persistence_init();
            break;

        case SERVICE_SYSTEM_MONITOR:
            ret = system_monitor_start(30); /* 30 second interval */
            break;

        case SERVICE_MQTT_LOGGER:
            ret = mqtt_logger_start();
            break;

        case SERVICE_AVAILABILITY:
            ret = zb_availability_start();
            break;

#if CONFIG_BT_SCANNER_ENABLED
        case SERVICE_BLE_SCANNER:
            if (!ble_manager_is_initialized()) {
                ret = ble_manager_init();
            }
            if (ret == ESP_OK) {
                ret = ble_scanner_start();
            }
            break;

        case SERVICE_BLE_GATT:
            ret = ble_gatt_client_init();
            break;
#endif

#if CONFIG_ESPHOME_API_ENABLE
        case SERVICE_ESPHOME_API:
            ret = esphome_api_start();
            break;

        case SERVICE_ESPHOME_BLE_PROXY:
            ret = esphome_ble_proxy_start();
            break;
#endif

        case SERVICE_HA_DISCOVERY:
            /* HA Discovery doesn't have a separate start - it's event-driven */
            ret = ESP_OK;
            break;

        default:
            ESP_LOGW(TAG, "No start handler for service %d", service);
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
    }

    if (ret == ESP_OK) {
        s_services[service].state = SERVICE_STATE_RUNNING;
        ESP_LOGI(TAG, "Started %s", s_services[service].name);
    } else {
        s_services[service].state = SERVICE_STATE_ERROR;
        ESP_LOGE(TAG, "Failed to start %s: %s", s_services[service].name, esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t stop_service_internal(lifecycle_service_t service)
{
    esp_err_t ret = ESP_OK;
    s_services[service].state = SERVICE_STATE_STOPPING;

    switch (service) {
        case SERVICE_STATE_PERSISTENCE:
            state_persistence_deinit();
            break;

        case SERVICE_SYSTEM_MONITOR:
            ret = system_monitor_stop();
            break;

        case SERVICE_MQTT_LOGGER:
            ret = mqtt_logger_stop();
            break;

        case SERVICE_AVAILABILITY:
            ret = zb_availability_stop();
            break;

#if CONFIG_BT_SCANNER_ENABLED
        case SERVICE_BLE_SCANNER:
            ret = ble_scanner_stop();
            /* Note: Don't deinit ble_manager here - other BLE services may need it */
            break;

        case SERVICE_BLE_GATT:
            ret = ble_gatt_client_deinit();
            break;
#endif

#if CONFIG_ESPHOME_API_ENABLE
        case SERVICE_ESPHOME_API:
            ret = esphome_api_stop();
            break;

        case SERVICE_ESPHOME_BLE_PROXY:
            ret = esphome_ble_proxy_stop();
            break;
#endif

        case SERVICE_HA_DISCOVERY:
            /* HA Discovery doesn't have a separate stop */
            ret = ESP_OK;
            break;

        default:
            ESP_LOGW(TAG, "No stop handler for service %d", service);
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
    }

    if (ret == ESP_OK) {
        s_services[service].state = SERVICE_STATE_STOPPED;
        ESP_LOGI(TAG, "Stopped %s", s_services[service].name);
    } else {
        s_services[service].state = SERVICE_STATE_ERROR;
        ESP_LOGE(TAG, "Failed to stop %s: %s", s_services[service].name, esp_err_to_name(ret));
    }

    return ret;
}
