/**
 * @file zb_availability.c
 * @brief Zigbee Device Availability Tracking Implementation
 *
 * Implements active and passive availability checking for Zigbee devices:
 * - Router devices: Active ZCL Basic cluster ping with exponential backoff
 * - Battery devices: Passive timeout-based detection
 * - MQTT availability publishing on state changes
 *
 * AP-7.9: Eliminated parallel devices[] array. All per-device availability
 * metadata is now stored in device_t.proto.zigbee.avail_meta via the
 * unified device registry. Iteration uses device_registry_iterate_zigbee()
 * and device_registry_collect_ids().
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_availability.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "zb_coordinator.h"
#include "gateway_defaults.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "utils/freertos_helpers.h"  /* For PSRAM task creation */
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "ZB_AVAIL";

/* ============================================================================
 * NVS Keys
 * ============================================================================ */

#define ZB_AVAIL_NVS_NAMESPACE      "zb_avail"
#define ZB_AVAIL_NVS_KEY_CONFIG     "config"

/* ============================================================================
 * Task Configuration
 * ============================================================================ */

#define ZB_AVAIL_TASK_STACK_SIZE    (4 * 1024)
#define ZB_AVAIL_TASK_PRIORITY      4
#define ZB_AVAIL_TASK_LOOP_MS       1000    /**< Main loop interval */
#define ZB_AVAIL_PING_TIMEOUT_MS    10000   /**< Ping response timeout */

/* ============================================================================
 * Module State
 * ============================================================================ */

/**
 * @brief Module state enumeration
 */
typedef enum {
    ZB_AVAIL_MODULE_UNINITIALIZED = 0,
    ZB_AVAIL_MODULE_INITIALIZED,
    ZB_AVAIL_MODULE_RUNNING,
    ZB_AVAIL_MODULE_STOPPED
} zb_avail_module_state_t;

/**
 * @brief Module context structure
 *
 * AP-7.9: Removed devices[] array and device_count.
 * Per-device metadata now lives in device_t.proto.zigbee.avail_meta.
 */
typedef struct {
    zb_avail_module_state_t state;
    zb_availability_config_t config;
    psram_task_handle_t psram_task;  /**< PSRAM-backed task (saves internal RAM) */
    SemaphoreHandle_t mutex;
    zb_availability_state_cb_t state_callback;
    uint32_t check_counter;
} zb_avail_context_t;

/* Static module context */
static zb_avail_context_t s_ctx = {0};

/* ============================================================================
 * Internal Helper: Availability State Mapping
 * ============================================================================ */

/**
 * @brief Convert ZB_AVAIL_* enum to DEV_AVAIL_* enum
 */
static device_availability_t zb_to_dev_avail(zb_availability_state_t state)
{
    switch (state) {
        case ZB_AVAIL_ONLINE:  return DEV_AVAIL_ONLINE;
        case ZB_AVAIL_OFFLINE: return DEV_AVAIL_OFFLINE;
        case ZB_AVAIL_UNKNOWN:
        default:               return DEV_AVAIL_UNKNOWN;
    }
}

/**
 * @brief Convert DEV_AVAIL_* enum to ZB_AVAIL_* enum
 */
static zb_availability_state_t dev_to_zb_avail(device_availability_t avail)
{
    switch (avail) {
        case DEV_AVAIL_ONLINE:  return ZB_AVAIL_ONLINE;
        case DEV_AVAIL_OFFLINE: return ZB_AVAIL_OFFLINE;
        case DEV_AVAIL_UNKNOWN:
        default:                return ZB_AVAIL_UNKNOWN;
    }
}

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void zb_availability_task(void *pvParameters);
static esp_err_t send_ping(uint16_t short_addr, uint8_t endpoint);
static void check_device_timeouts(void);
static void process_pending_checks(void);
static void set_device_state(device_t *dev, zb_availability_state_t new_state);
static uint32_t get_device_timeout(const device_avail_meta_t *meta);
static void calculate_next_check(device_avail_meta_t *meta);

/* ============================================================================
 * Initialization and Control
 * ============================================================================ */

esp_err_t zb_availability_init(zb_availability_config_t *config)
{
    if (s_ctx.state != ZB_AVAIL_MODULE_UNINITIALIZED) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing availability tracking...");

    /* Create mutex */
    s_ctx.mutex = xSemaphoreCreateMutex();
    if (s_ctx.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_ctx.state_callback = NULL;
    s_ctx.check_counter = 0;

    /* Set configuration */
    if (config != NULL) {
        s_ctx.config = *config;
    } else {
        /* Use defaults */
        s_ctx.config.router_check_interval = ZB_AVAIL_DEFAULT_ROUTER_INTERVAL;
        s_ctx.config.battery_timeout = ZB_AVAIL_DEFAULT_BATTERY_TIMEOUT;
        s_ctx.config.router_timeout = ZB_AVAIL_DEFAULT_ROUTER_TIMEOUT;
#ifdef CONFIG_ZB_AVAILABILITY_CHECK_ENABLED
        s_ctx.config.active_check_enabled = true;
#else
        s_ctx.config.active_check_enabled = true;  /* Default enabled */
#endif
    }

    /* Try to load saved configuration */
    esp_err_t ret = zb_availability_load_config();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Loaded configuration from NVS");
    } else {
        ESP_LOGI(TAG, "Using default configuration");
    }

    ESP_LOGI(TAG, "Config: router_interval=%lus, battery_timeout=%lus, router_timeout=%lus, active=%s",
             (unsigned long)s_ctx.config.router_check_interval,
             (unsigned long)s_ctx.config.battery_timeout,
             (unsigned long)s_ctx.config.router_timeout,
             s_ctx.config.active_check_enabled ? "yes" : "no");

    s_ctx.state = ZB_AVAIL_MODULE_INITIALIZED;
    ESP_LOGI(TAG, "Availability tracking initialized (using NG device_registry)");
    return ESP_OK;
}

esp_err_t zb_availability_deinit(void)
{
    if (s_ctx.state == ZB_AVAIL_MODULE_UNINITIALIZED) {
        return ESP_OK;
    }

    /* Stop if running */
    if (s_ctx.state == ZB_AVAIL_MODULE_RUNNING) {
        zb_availability_stop();
    }

    /* Delete mutex */
    if (s_ctx.mutex != NULL) {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
    }

    s_ctx.state = ZB_AVAIL_MODULE_UNINITIALIZED;
    ESP_LOGI(TAG, "Availability tracking deinitialized");
    return ESP_OK;
}

esp_err_t zb_availability_start(void)
{
    if (s_ctx.state != ZB_AVAIL_MODULE_INITIALIZED &&
        s_ctx.state != ZB_AVAIL_MODULE_STOPPED) {
        ESP_LOGE(TAG, "Cannot start: invalid state %d", s_ctx.state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting availability tracking...");

    /* Create task with PSRAM stack (saves ~4KB internal RAM) */
    esp_err_t ret = psram_task_create(
        zb_availability_task,
        "zb_avail",
        ZB_AVAIL_TASK_STACK_SIZE,
        NULL,
        ZB_AVAIL_TASK_PRIORITY,
        &s_ctx.psram_task
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create task: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ctx.state = ZB_AVAIL_MODULE_RUNNING;
    ESP_LOGI(TAG, "Availability tracking started (PSRAM stack)");
    return ESP_OK;
}

esp_err_t zb_availability_stop(void)
{
    if (s_ctx.state != ZB_AVAIL_MODULE_RUNNING) {
        ESP_LOGW(TAG, "Not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping availability tracking...");

    /* Delete task and free PSRAM resources */
    psram_task_delete(&s_ctx.psram_task);

    s_ctx.state = ZB_AVAIL_MODULE_STOPPED;
    ESP_LOGI(TAG, "Availability tracking stopped");
    return ESP_OK;
}

bool zb_availability_is_running(void)
{
    return (s_ctx.state == ZB_AVAIL_MODULE_RUNNING);
}

/* ============================================================================
 * Device Tracking
 * ============================================================================ */

esp_err_t zb_availability_add_device_with_state(uint16_t short_addr,
                                                 zb_availability_power_type_t power_type,
                                                 zb_availability_state_t initial_state)
{
    if (short_addr == 0x0000 || short_addr == 0xFFFF) {
        ESP_LOGE(TAG, "Invalid device address: 0x%04X", short_addr);
        return ESP_ERR_INVALID_ARG;
    }

    /* Look up device in NG registry */
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        ESP_LOGW(TAG, "Device 0x%04X not found in NG registry, cannot track", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    device_avail_meta_t *meta = &dev->proto.zigbee.avail_meta;

    /* Check if already tracked (backoff_multiplier != 0 means tracked) */
    if (meta->backoff_multiplier != 0) {
        ESP_LOGD(TAG, "Device 0x%04X already tracked, updating power type", short_addr);
        meta->power_type = (uint8_t)power_type;
        dev->last_seen = (uint32_t)time(NULL);
        return ESP_OK;
    }

    /* Initialize avail_meta fields */
    meta->power_type = (uint8_t)power_type;
    meta->consecutive_failures = 0;
    meta->backoff_multiplier = 1;  /* 1 = tracked, 0 = not tracked */
    meta->custom_timeout = 0;
    meta->pending_response = false;
    meta->last_check = 0;

    dev->last_seen = (uint32_t)time(NULL);

    /* Calculate initial next check time */
    calculate_next_check(meta);

    ESP_LOGI(TAG, "Added device 0x%04X for tracking (power=%s, state=%s)",
             short_addr, zb_availability_power_type_to_str(power_type),
             zb_availability_state_to_str(initial_state));

    /* Set initial state -- updates dev->availability and publishes */
    set_device_state(dev, initial_state);

    return ESP_OK;
}

esp_err_t zb_availability_add_device(uint16_t short_addr, zb_availability_power_type_t power_type)
{
    return zb_availability_add_device_with_state(short_addr, power_type, ZB_AVAIL_ONLINE);
}

esp_err_t zb_availability_remove_device(uint16_t short_addr)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        ESP_LOGW(TAG, "Device 0x%04X not found for removal", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    /* Clear avail_meta fields (backoff_multiplier=0 means "not tracked") */
    memset(&dev->proto.zigbee.avail_meta, 0, sizeof(device_avail_meta_t));

    ESP_LOGI(TAG, "Removed device 0x%04X from tracking", short_addr);
    return ESP_OK;
}

esp_err_t zb_availability_update_last_seen(uint16_t short_addr)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        /* Device not in registry -- cannot track without registry entry */
        ESP_LOGI(TAG, "Device 0x%04X not in registry, cannot update last_seen", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    device_avail_meta_t *meta = &dev->proto.zigbee.avail_meta;

    /* Auto-add if not yet tracked (backoff_multiplier == 0) */
    if (meta->backoff_multiplier == 0) {
        ESP_LOGI(TAG, "Device 0x%04X not tracked, auto-adding on activity", short_addr);
        return zb_availability_add_device(short_addr, ZB_AVAIL_POWER_UNKNOWN);
    }

    uint32_t now = (uint32_t)time(NULL);
    dev->last_seen = now;
    meta->consecutive_failures = 0;
    meta->backoff_multiplier = 1;
    meta->pending_response = false;

    /* Recalculate next check time */
    calculate_next_check(meta);

    zb_availability_state_t old_state = dev_to_zb_avail(dev->availability);

    /* Mark as online if not already */
    if (old_state != ZB_AVAIL_ONLINE) {
        set_device_state(dev, ZB_AVAIL_ONLINE);
    }

    ESP_LOGD(TAG, "Updated last_seen for device 0x%04X", short_addr);
    return ESP_OK;
}

esp_err_t zb_availability_check_device(uint16_t short_addr)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        ESP_LOGW(TAG, "Device 0x%04X not found for check", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    device_avail_meta_t *meta = &dev->proto.zigbee.avail_meta;

    if (meta->pending_response) {
        ESP_LOGW(TAG, "Check already pending for device 0x%04X", short_addr);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t endpoint = dev->proto.zigbee.endpoint;
    if (endpoint == 0) {
        endpoint = 1;  /* EP0 = ZDO, fallback to EP1 */
    }

    meta->pending_response = true;
    meta->last_check = (uint32_t)time(NULL);

    ESP_LOGI(TAG, "Sending availability ping to 0x%04X (EP=%d)", short_addr, endpoint);

    /* Send ZCL Read Attribute request to Basic cluster */
    esp_err_t ret = send_ping(short_addr, endpoint);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send ping to 0x%04X: %s", short_addr, esp_err_to_name(ret));
        meta->pending_response = false;
    }

    return ret;
}

zb_availability_state_t zb_availability_get_state(uint16_t short_addr)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return ZB_AVAIL_UNKNOWN;
    }

    return dev_to_zb_avail(dev->availability);
}

/* ============================================================================
 * Configuration
 * ============================================================================ */

esp_err_t zb_availability_set_device_timeout(uint16_t short_addr, uint32_t timeout_sec)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    device_avail_meta_t *meta = &dev->proto.zigbee.avail_meta;
    meta->custom_timeout = timeout_sec;
    calculate_next_check(meta);

    ESP_LOGI(TAG, "Set custom timeout for 0x%04X: %lu seconds",
             short_addr, (unsigned long)timeout_sec);
    return ESP_OK;
}

esp_err_t zb_availability_set_device_power_type(uint16_t short_addr,
                                                  zb_availability_power_type_t power_type)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    device_avail_meta_t *meta = &dev->proto.zigbee.avail_meta;
    meta->power_type = (uint8_t)power_type;
    calculate_next_check(meta);

    ESP_LOGI(TAG, "Set power type for 0x%04X: %s",
             short_addr, zb_availability_power_type_to_str(power_type));
    return ESP_OK;
}

esp_err_t zb_availability_set_config(const zb_availability_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ctx.mutex, GW_TIMEOUT_LONG_TICKS);

    if (config->router_check_interval > 0) {
        s_ctx.config.router_check_interval = config->router_check_interval;
    }
    if (config->battery_timeout > 0) {
        s_ctx.config.battery_timeout = config->battery_timeout;
    }
    if (config->router_timeout > 0) {
        s_ctx.config.router_timeout = config->router_timeout;
    }
    s_ctx.config.active_check_enabled = config->active_check_enabled;

    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Configuration updated");
    return ESP_OK;
}

esp_err_t zb_availability_get_config(zb_availability_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ctx.mutex, GW_TIMEOUT_LONG_TICKS);
    *config = s_ctx.config;
    xSemaphoreGive(s_ctx.mutex);

    return ESP_OK;
}

esp_err_t zb_availability_save_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(ZB_AVAIL_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    xSemaphoreTake(s_ctx.mutex, GW_TIMEOUT_LONG_TICKS);

    ret = nvs_set_blob(nvs_handle, ZB_AVAIL_NVS_KEY_CONFIG,
                       &s_ctx.config, sizeof(zb_availability_config_t));

    xSemaphoreGive(s_ctx.mutex);

    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Configuration saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to save configuration: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t zb_availability_load_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(ZB_AVAIL_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    zb_availability_config_t loaded_config;
    size_t size = sizeof(zb_availability_config_t);

    ret = nvs_get_blob(nvs_handle, ZB_AVAIL_NVS_KEY_CONFIG, &loaded_config, &size);

    nvs_close(nvs_handle);

    if (ret == ESP_OK && size == sizeof(zb_availability_config_t)) {
        xSemaphoreTake(s_ctx.mutex, GW_TIMEOUT_LONG_TICKS);
        s_ctx.config = loaded_config;
        xSemaphoreGive(s_ctx.mutex);
        ESP_LOGI(TAG, "Configuration loaded from NVS");
    }

    return ret;
}

/* ============================================================================
 * MQTT Publishing
 * ============================================================================ */

esp_err_t zb_availability_publish_state(uint16_t short_addr)
{
    /* Get device from NG registry */
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL) {
        ESP_LOGW(TAG, "Device 0x%04X not found in device registry", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    /* Copy everything needed before publishing: event_publish() can block on
     * the queue and yield, and a remove from the MQTT or ESPHome task would
     * invalidate this pointer. */
    const uint64_t ieee_addr = device->id;
    const uint8_t endpoint = device->proto.zigbee.endpoint;
    char friendly_name[sizeof(device->friendly_name)];
    snprintf(friendly_name, sizeof(friendly_name), "%s", device->friendly_name);

    /* Publish via event bus - handler reads state from device_registry */
    evt_device_state_t evt = {
        .ieee_addr = ieee_addr,
        .short_addr = short_addr,
        .endpoint = endpoint,
        .cluster_id = 0,  /* Availability is not cluster-specific */
        .attr_id = 0,
        .json_state = NULL,
    };

    esp_err_t ret = event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to publish availability event for 0x%04X: %s",
                 short_addr, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Published availability event for %s", friendly_name);
    }

    return ret;
}

esp_err_t zb_availability_publish_all(void)
{
    ESP_LOGI(TAG, "Publishing availability for all devices...");

    /* Collect device IDs first, then publish outside registry mutex */
    device_id_t ids[DEVICE_REGISTRY_MAX_DEVICES];
    size_t count = 0;
    esp_err_t ret = device_registry_collect_ids(ids, DEVICE_REGISTRY_MAX_DEVICES, &count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to collect device IDs: %s", esp_err_to_name(ret));
        return ret;
    }

    for (size_t i = 0; i < count; i++) {
        device_t *dev = device_registry_get(ids[i]);
        if (dev == NULL || dev->protocol != DEV_PROTOCOL_ZIGBEE) {
            continue;
        }
        /* Only publish for tracked devices (backoff_multiplier != 0) */
        if (dev->proto.zigbee.avail_meta.backoff_multiplier == 0) {
            continue;
        }
        zb_availability_publish_state(dev->proto.zigbee.short_addr);
    }

    ESP_LOGI(TAG, "Finished publishing all availability states");
    return ESP_OK;
}

/* ============================================================================
 * Callbacks
 * ============================================================================ */

esp_err_t zb_availability_register_callback(zb_availability_state_cb_t callback)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ctx.mutex, GW_TIMEOUT_LONG_TICKS);
    s_ctx.state_callback = callback;
    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "State change callback registered");
    return ESP_OK;
}

esp_err_t zb_availability_unregister_callback(void)
{
    xSemaphoreTake(s_ctx.mutex, GW_TIMEOUT_LONG_TICKS);
    s_ctx.state_callback = NULL;
    xSemaphoreGive(s_ctx.mutex);

    return ESP_OK;
}

/* ============================================================================
 * Response Handlers
 * ============================================================================ */

esp_err_t zb_availability_handle_read_response(uint16_t short_addr, uint8_t status)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    device_avail_meta_t *meta = &dev->proto.zigbee.avail_meta;
    meta->pending_response = false;

    if (status == 0) {  /* ESP_ZB_ZCL_STATUS_SUCCESS */
        ESP_LOGI(TAG, "Ping response from 0x%04X: SUCCESS", short_addr);

        dev->last_seen = (uint32_t)time(NULL);
        meta->consecutive_failures = 0;
        meta->backoff_multiplier = 1;
        calculate_next_check(meta);

        zb_availability_state_t old_state = dev_to_zb_avail(dev->availability);

        if (old_state != ZB_AVAIL_ONLINE) {
            set_device_state(dev, ZB_AVAIL_ONLINE);
        }
    } else {
        ESP_LOGW(TAG, "Ping response from 0x%04X: FAILED (status=0x%02X)", short_addr, status);

        meta->consecutive_failures++;

        /* Check if we should mark offline */
        zb_availability_handle_timeout(short_addr);
    }

    return ESP_OK;
}

esp_err_t zb_availability_handle_timeout(uint16_t short_addr)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    device_avail_meta_t *meta = &dev->proto.zigbee.avail_meta;

    meta->pending_response = false;
    meta->consecutive_failures++;

    ESP_LOGW(TAG, "Ping timeout for 0x%04X (failures=%d)",
             short_addr, meta->consecutive_failures);

    /* Apply exponential backoff */
    if (meta->backoff_multiplier < ZB_AVAIL_MAX_BACKOFF) {
        meta->backoff_multiplier *= ZB_AVAIL_BACKOFF_BASE;
        if (meta->backoff_multiplier == 0) {
            meta->backoff_multiplier = 1;  /* Safety: keep tracked */
        }
    }
    calculate_next_check(meta);

    /* Check if we should mark offline */
    if (meta->consecutive_failures >= ZB_AVAIL_MAX_FAILURES) {
        zb_availability_state_t old_state = dev_to_zb_avail(dev->availability);

        if (old_state != ZB_AVAIL_OFFLINE) {
            set_device_state(dev, ZB_AVAIL_OFFLINE);
        }
    }

    return ESP_OK;
}

esp_err_t zb_availability_mark_offline(uint16_t short_addr)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    set_device_state(dev, ZB_AVAIL_OFFLINE);
    ESP_LOGI(TAG, "Device 0x%04X forced offline (interview failed)", short_addr);
    return ESP_OK;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

const char* zb_availability_state_to_str(zb_availability_state_t state)
{
    switch (state) {
        case ZB_AVAIL_ONLINE:  return "online";
        case ZB_AVAIL_OFFLINE: return "offline";
        case ZB_AVAIL_UNKNOWN:
        default:               return "unknown";
    }
}

const char* zb_availability_power_type_to_str(zb_availability_power_type_t power_type)
{
    switch (power_type) {
        case ZB_AVAIL_POWER_MAINS:   return "mains";
        case ZB_AVAIL_POWER_BATTERY: return "battery";
        case ZB_AVAIL_POWER_UNKNOWN:
        default:                     return "unknown";
    }
}

/**
 * @brief Iterator callback context for get_stats
 */
typedef struct {
    uint16_t online;
    uint16_t offline;
    uint16_t unknown;
} avail_stats_ctx_t;

/**
 * @brief Iterator callback for collecting availability statistics
 * @note Called with registry mutex held -- must be fast, no blocking I/O
 */
static bool stats_iterator(device_t *dev, void *ctx)
{
    avail_stats_ctx_t *stats = (avail_stats_ctx_t *)ctx;

    /* Only count tracked devices (backoff_multiplier != 0) */
    if (dev->proto.zigbee.avail_meta.backoff_multiplier == 0) {
        return true;  /* Continue */
    }

    switch (dev->availability) {
        case DEV_AVAIL_ONLINE:  stats->online++;  break;
        case DEV_AVAIL_OFFLINE: stats->offline++; break;
        default:                stats->unknown++; break;
    }

    return true;  /* Continue */
}

esp_err_t zb_availability_get_stats(uint16_t *online_count, uint16_t *offline_count,
                                     uint16_t *unknown_count)
{
    avail_stats_ctx_t stats = {0};

    device_registry_iterate_zigbee(stats_iterator, &stats);

    if (online_count)  *online_count = stats.online;
    if (offline_count) *offline_count = stats.offline;
    if (unknown_count) *unknown_count = stats.unknown;

    return ESP_OK;
}

esp_err_t zb_availability_test(void)
{
    ESP_LOGI(TAG, "Running availability module self-test...");

    /* Test initialization */
    if (s_ctx.state == ZB_AVAIL_MODULE_UNINITIALIZED) {
        ESP_LOGE(TAG, "Test failed: module not initialized");
        return ESP_FAIL;
    }

    /* Test mutex */
    if (s_ctx.mutex == NULL) {
        ESP_LOGE(TAG, "Test failed: mutex not created");
        return ESP_FAIL;
    }

    /* Test add/remove device -- requires device 0x1234 to exist in registry.
     * In production this is always called after devices are loaded,
     * so we test with a real device or skip if none exist. */
    device_t *test_dev = device_registry_get_by_short_addr(0x1234);
    if (test_dev != NULL) {
        esp_err_t ret = zb_availability_add_device(0x1234, ZB_AVAIL_POWER_MAINS);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Test failed: add device");
            return ESP_FAIL;
        }

        zb_availability_state_t state = zb_availability_get_state(0x1234);
        if (state != ZB_AVAIL_ONLINE) {
            ESP_LOGE(TAG, "Test failed: expected ONLINE state");
            return ESP_FAIL;
        }

        ret = zb_availability_update_last_seen(0x1234);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Test failed: update last_seen");
            return ESP_FAIL;
        }

        ret = zb_availability_remove_device(0x1234);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Test failed: remove device");
            return ESP_FAIL;
        }

        state = zb_availability_get_state(0x1234);
        if (state != ZB_AVAIL_UNKNOWN) {
            ESP_LOGE(TAG, "Test failed: expected UNKNOWN after removal");
            return ESP_FAIL;
        }
    } else {
        ESP_LOGI(TAG, "Self-test: skipping add/remove (no test device in registry)");
    }

    ESP_LOGI(TAG, "Availability module self-test PASSED");
    return ESP_OK;
}

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

/**
 * @brief Send ZCL Read Attribute request as ping
 */
static esp_err_t send_ping(uint16_t short_addr, uint8_t endpoint)
{
    if (!zb_coordinator_is_running()) {
        ESP_LOGW(TAG, "Coordinator not running, cannot send ping");
        return ESP_ERR_INVALID_STATE;
    }

    /* Read ZCL Version attribute from Basic cluster as ping */
    esp_zb_zcl_read_attr_cmd_t read_req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        .attr_number = 1,
        .attr_field = (uint16_t[]){ESP_ZB_ZCL_ATTR_BASIC_ZCL_VERSION_ID},
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_read_attr_cmd_req(&read_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Ping sent to 0x%04X, TSN=0x%02X", short_addr, tsn);
    return ESP_OK;
}

/**
 * @brief Iterator callback context for check_device_timeouts
 */
typedef struct {
    uint32_t now;
    uint16_t timeout_addrs[DEVICE_REGISTRY_MAX_DEVICES];
    uint16_t offline_addrs[DEVICE_REGISTRY_MAX_DEVICES];
    size_t timeout_count;
    size_t offline_count;
} timeout_check_ctx_t;

/**
 * @brief Iterator callback for checking device timeouts
 * @note Called with registry mutex held -- collect addresses only, act after
 */
static bool timeout_check_iterator(device_t *dev, void *ctx)
{
    timeout_check_ctx_t *tc = (timeout_check_ctx_t *)ctx;
    device_avail_meta_t *meta = &dev->proto.zigbee.avail_meta;

    /* Skip untracked devices */
    if (meta->backoff_multiplier == 0) {
        return true;
    }

    /* Check pending response timeout */
    if (meta->pending_response) {
        uint32_t ping_elapsed = tc->now - meta->last_check;
        if (ping_elapsed > (ZB_AVAIL_PING_TIMEOUT_MS / 1000)) {
            if (tc->timeout_count < DEVICE_REGISTRY_MAX_DEVICES) {
                tc->timeout_addrs[tc->timeout_count++] = dev->proto.zigbee.short_addr;
            }
        }
    }

    /* Check if device has timed out (battery/unknown devices only).
     * Skip when last_seen == 0: device was loaded from NVS but hasn't been
     * seen since boot — we can't determine elapsed time without a valid
     * timestamp, so don't mark offline prematurely. */
    if (dev->last_seen != 0) {
        uint32_t timeout = get_device_timeout(meta);
        uint32_t elapsed = (tc->now > dev->last_seen) ? (tc->now - dev->last_seen) : 0;

        if (dev->availability == DEV_AVAIL_ONLINE && elapsed > timeout) {
            if (meta->power_type == (uint8_t)ZB_AVAIL_POWER_BATTERY ||
                meta->power_type == (uint8_t)ZB_AVAIL_POWER_UNKNOWN) {
                if (tc->offline_count < DEVICE_REGISTRY_MAX_DEVICES) {
                    tc->offline_addrs[tc->offline_count++] = dev->proto.zigbee.short_addr;
                }
            }
        }
    }

    return true;  /* Continue */
}

/**
 * @brief Check all devices for timeout and mark offline if needed
 */
static void check_device_timeouts(void)
{
    timeout_check_ctx_t tc = {
        .now = (uint32_t)time(NULL),
        .timeout_count = 0,
        .offline_count = 0,
    };

    /* Collect addresses that need action (registry mutex held during iteration) */
    device_registry_iterate_zigbee(timeout_check_iterator, &tc);

    /* Process timeout addresses (outside registry mutex) */
    for (size_t i = 0; i < tc.timeout_count; i++) {
        ESP_LOGW(TAG, "Ping timeout detected for 0x%04X", tc.timeout_addrs[i]);
        zb_availability_handle_timeout(tc.timeout_addrs[i]);
    }

    /* Process offline addresses (outside registry mutex) */
    for (size_t i = 0; i < tc.offline_count; i++) {
        device_t *dev = device_registry_get_by_short_addr(tc.offline_addrs[i]);
        if (dev != NULL) {
            uint32_t elapsed = (tc.now > dev->last_seen) ? (tc.now - dev->last_seen) : 0;
            uint32_t timeout = get_device_timeout(&dev->proto.zigbee.avail_meta);
            ESP_LOGW(TAG, "Device 0x%04X timed out (elapsed=%lu, timeout=%lu)",
                     tc.offline_addrs[i], (unsigned long)elapsed, (unsigned long)timeout);
            set_device_state(dev, ZB_AVAIL_OFFLINE);
        }
    }
}

/**
 * @brief Iterator callback context for process_pending_checks
 */
typedef struct {
    uint32_t now;
    uint16_t check_addrs[DEVICE_REGISTRY_MAX_DEVICES];
    size_t check_count;
} pending_check_ctx_t;

/**
 * @brief Iterator callback for collecting devices that need active checks
 * @note Called with registry mutex held -- collect addresses only
 */
static bool pending_check_iterator(device_t *dev, void *ctx)
{
    pending_check_ctx_t *pc = (pending_check_ctx_t *)ctx;
    device_avail_meta_t *meta = &dev->proto.zigbee.avail_meta;

    /* Skip untracked devices */
    if (meta->backoff_multiplier == 0) {
        return true;
    }

    /* Only active check for mains-powered (router) devices */
    if (meta->power_type != (uint8_t)ZB_AVAIL_POWER_MAINS) {
        return true;
    }

    /* Skip if already waiting for response */
    if (meta->pending_response) {
        return true;
    }

    /* Check if it's time for the next check */
    if (pc->now >= meta->next_check) {
        if (pc->check_count < DEVICE_REGISTRY_MAX_DEVICES) {
            pc->check_addrs[pc->check_count++] = dev->proto.zigbee.short_addr;
        }
    }

    return true;  /* Continue */
}

/**
 * @brief Process pending availability checks for router devices
 */
static void process_pending_checks(void)
{
    xSemaphoreTake(s_ctx.mutex, GW_TIMEOUT_LONG_TICKS);
    bool active_enabled = s_ctx.config.active_check_enabled;
    xSemaphoreGive(s_ctx.mutex);

    if (!active_enabled) {
        return;
    }

    pending_check_ctx_t pc = {
        .now = (uint32_t)time(NULL),
        .check_count = 0,
    };

    /* Collect addresses that need checking (registry mutex held during iteration) */
    device_registry_iterate_zigbee(pending_check_iterator, &pc);

    /* Send pings outside registry mutex */
    for (size_t i = 0; i < pc.check_count; i++) {
        ESP_LOGD(TAG, "Scheduled availability check for 0x%04X", pc.check_addrs[i]);
        zb_availability_check_device(pc.check_addrs[i]);
    }
}

/**
 * @brief Set device state and trigger callbacks/publishing
 */
static void set_device_state(device_t *dev, zb_availability_state_t new_state)
{
    if (dev == NULL) {
        return;
    }

    zb_availability_state_t old_state = dev_to_zb_avail(dev->availability);
    uint16_t short_addr = dev->proto.zigbee.short_addr;

    if (old_state == new_state) {
        return;
    }

    /* Update the availability in the device struct directly */
    dev->availability = zb_to_dev_avail(new_state);

    /* Get callback reference under mutex */
    xSemaphoreTake(s_ctx.mutex, GW_TIMEOUT_LONG_TICKS);
    zb_availability_state_cb_t callback = s_ctx.state_callback;
    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Device 0x%04X state changed: %s -> %s",
             short_addr,
             zb_availability_state_to_str(old_state),
             zb_availability_state_to_str(new_state));

    /* Update device online status in NG registry (thread-safe) */
    device_registry_set_online(dev->id, (new_state == ZB_AVAIL_ONLINE));

    /* Publish availability change via event bus.
     * mqtt_event_handler.c constructs the MQTT topic and publishes.
     * This is separate from the state event below -- EVT_DEVICE_STATE_CHANGED
     * only updates the state topic, not the availability topic. */
    evt_device_availability_t avail_evt = {
        .device_id = dev->id,
        .online = (new_state == ZB_AVAIL_ONLINE)
    };
    esp_err_t ret = event_publish(EVT_DEVICE_AVAILABILITY_CHANGED,
                                  &avail_evt, sizeof(avail_evt));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Published availability %s for %s",
                 (new_state == ZB_AVAIL_ONLINE) ? "online" : "offline",
                 dev->friendly_name);
    }

    /* Publish state data via event bus */
    zb_availability_publish_state(short_addr);

    /* Call state change callback */
    if (callback != NULL) {
        callback(short_addr, old_state, new_state);
    }
}

/**
 * @brief Get effective timeout for a device
 */
static uint32_t get_device_timeout(const device_avail_meta_t *meta)
{
    if (meta->custom_timeout > 0) {
        return meta->custom_timeout;
    }

    switch ((zb_availability_power_type_t)meta->power_type) {
        case ZB_AVAIL_POWER_MAINS:
            return s_ctx.config.router_timeout;
        case ZB_AVAIL_POWER_BATTERY:
        case ZB_AVAIL_POWER_UNKNOWN:
        default:
            return s_ctx.config.battery_timeout;
    }
}

/**
 * @brief Calculate next check time for a device
 */
static void calculate_next_check(device_avail_meta_t *meta)
{
    uint32_t now = (uint32_t)time(NULL);
    uint32_t interval;
    uint8_t multiplier = (meta->backoff_multiplier > 0) ? meta->backoff_multiplier : 1;

    if ((zb_availability_power_type_t)meta->power_type == ZB_AVAIL_POWER_MAINS) {
        /* Router: use check interval with backoff */
        interval = s_ctx.config.router_check_interval * multiplier;
    } else {
        /* Battery device: no active checks, just use timeout for reference */
        interval = s_ctx.config.battery_timeout;
    }

    meta->next_check = now + interval;
}

/**
 * @brief Main availability tracking task
 */
static void zb_availability_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Availability task started");

    /* Wait for coordinator to be running */
    while (!zb_coordinator_is_running()) {
        vTaskDelay(pdMS_TO_TICKS(GW_WIFI_STABILIZE_DELAY_MS));
    }

    ESP_LOGI(TAG, "Coordinator running, starting availability checks");

    /* Initial delay to let devices settle */
    vTaskDelay(pdMS_TO_TICKS(GW_TIMEOUT_VERY_LONG_MS));

    while (1) {
        s_ctx.check_counter++;

        /* Check for device timeouts */
        check_device_timeouts();

        /* Process scheduled active checks */
        process_pending_checks();

        /* Log statistics periodically (every 60 iterations = 1 minute) */
        if ((s_ctx.check_counter % 60) == 0) {
            uint16_t online, offline, unknown;
            zb_availability_get_stats(&online, &offline, &unknown);
            ESP_LOGI(TAG, "Status: online=%d, offline=%d, unknown=%d",
                     online, offline, unknown);

            /* Log stack watermark */
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGD(TAG, "Stack watermark: %d bytes", watermark * sizeof(StackType_t));
        }

        vTaskDelay(pdMS_TO_TICKS(ZB_AVAIL_TASK_LOOP_MS));
    }

    ESP_LOGW(TAG, "Availability task exiting");
    vTaskDelete(NULL);
}
