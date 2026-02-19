/**
 * @file ble_battery_service.c
 * @brief BLE Battery Service (0x180F) Implementation
 *
 * Implements automatic battery level reading from BLE devices using
 * the standard Battery Service (0x180F) and Battery Level
 * characteristic (0x2A19).
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_battery_service.h"
#include "ble_gatt_client.h"
#include "ble_common.h"
#include "ble_constants.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include <string.h>
#include <time.h>

static const char *TAG = "BLE_BATTERY";

/* ============================================================================
 * Private Types
 * ============================================================================ */

/**
 * @brief Monitored device entry
 */
typedef struct {
    uint8_t mac[6];                     /**< Device MAC address */
    uint8_t last_level;                 /**< Last known battery level */
    ble_battery_status_t last_status;   /**< Last known status */
    uint32_t last_read;                 /**< Last read timestamp */
    bool supported;                     /**< Device supports battery service */
    bool active;                        /**< Slot in use */
} ble_battery_device_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Battery service configuration */
static ble_battery_config_t s_config = BLE_BATTERY_CONFIG_DEFAULT();

/** @brief Monitored devices array */
static ble_battery_device_t s_devices[BLE_BATTERY_MAX_MONITORED_DEVICES];

/** @brief Number of monitored devices */
static uint8_t s_device_count = 0;

/** @brief Module mutex */
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Polling timer */
static esp_timer_handle_t s_poll_timer = NULL;

/** @brief Battery change callback */
static ble_battery_cb_t s_callback = NULL;

/** @brief Initialization flag */
static bool s_initialized = false;

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static void poll_timer_callback(void *arg);
static ble_battery_device_t *find_device(const uint8_t *mac);
static ble_battery_device_t *find_free_slot(void);
static esp_err_t read_battery_internal(uint16_t conn_handle, uint8_t *level);
static void notify_callback(const uint8_t *mac, uint8_t level, ble_battery_status_t status);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t ble_battery_service_init(void)
{
    ble_battery_config_t config = BLE_BATTERY_CONFIG_DEFAULT();
    return ble_battery_service_init_with_config(&config);
}

esp_err_t ble_battery_service_init_with_config(const ble_battery_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_gatt_client_is_initialized()) {
        ESP_LOGE(TAG, "GATT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize device array */
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;

    /* Store configuration */
    memcpy(&s_config, config, sizeof(ble_battery_config_t));

    /* Create polling timer */
    esp_timer_create_args_t timer_args = {
        .callback = poll_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "battery_poll"
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_poll_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ret;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Initialized (poll interval: %lu ms, auto-read: %s)",
             (unsigned long)s_config.poll_interval_ms,
             s_config.auto_read_on_connect ? "yes" : "no");

    return ESP_OK;
}

esp_err_t ble_battery_service_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop timer */
    if (s_poll_timer != NULL) {
        esp_timer_stop(s_poll_timer);
        esp_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }

    /* Clear devices */
    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    s_callback = NULL;
    xSemaphoreGive(s_mutex);

    /* Delete mutex */
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;

    s_initialized = false;

    ESP_LOGI(TAG, "Deinitialized");
    return ESP_OK;
}

bool ble_battery_service_is_initialized(void)
{
    return s_initialized;
}

esp_err_t ble_battery_service_set_config(const ble_battery_config_t *config)
{
    if (!s_initialized || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    memcpy(&s_config, config, sizeof(ble_battery_config_t));
    xSemaphoreGive(s_mutex);

    /* Restart timer with new interval if running */
    if (s_device_count > 0 && esp_timer_is_active(s_poll_timer)) {
        esp_timer_stop(s_poll_timer);
        esp_timer_start_periodic(s_poll_timer, s_config.poll_interval_ms * 1000);
    }

    return ESP_OK;
}

esp_err_t ble_battery_service_get_config(ble_battery_config_t *config)
{
    if (!s_initialized || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    memcpy(config, &s_config, sizeof(ble_battery_config_t));
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t ble_battery_read(const uint8_t *mac, uint8_t *level)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mac == NULL || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Get connection handle */
    uint16_t conn_handle = ble_gatt_get_conn_handle(mac);
    if (conn_handle == BLE_GATT_INVALID_CONN_HANDLE) {
        ESP_LOGD(TAG, "Device not connected");
        return ESP_ERR_NOT_FOUND;
    }

    return ble_battery_read_by_handle(conn_handle, level);
}

esp_err_t ble_battery_read_by_handle(uint16_t conn_handle, uint8_t *level)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_gatt_is_handle_connected(conn_handle)) {
        return ESP_ERR_NOT_FOUND;
    }

    return read_battery_internal(conn_handle, level);
}

bool ble_battery_device_supported(const uint8_t *mac)
{
    if (!s_initialized || mac == NULL) {
        return false;
    }

    uint16_t conn_handle = ble_gatt_get_conn_handle(mac);
    if (conn_handle == BLE_GATT_INVALID_CONN_HANDLE) {
        return false;
    }

    /* Check for battery service in discovered services */
    ble_gatt_service_t services[BLE_GATT_MAX_SERVICES];
    uint8_t count = 0;

    if (ble_gatt_get_services(conn_handle, services, BLE_GATT_MAX_SERVICES, &count) != ESP_OK) {
        return false;
    }

    for (uint8_t i = 0; i < count; i++) {
        if (services[i].uuid.type == BLE_GATT_UUID_TYPE_16 &&
            services[i].uuid.value.uuid16 == BLE_BATTERY_SERVICE_UUID) {
            return true;
        }
    }

    return false;
}

esp_err_t ble_battery_monitor_start(const uint8_t *mac)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Check if already monitored */
    ble_battery_device_t *device = find_device(mac);
    if (device != NULL) {
        xSemaphoreGive(s_mutex);
        ESP_LOGD(TAG, "Device already monitored");
        return ESP_OK;
    }

    /* Find free slot */
    device = find_free_slot();
    if (device == NULL) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Max monitored devices reached");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize device entry */
    memcpy(device->mac, mac, 6);
    device->last_level = 0;
    device->last_status = BLE_BATTERY_STATUS_UNKNOWN;
    device->last_read = 0;
    device->supported = ble_battery_device_supported(mac);
    device->active = true;
    s_device_count++;

    /* Start timer if first device */
    if (s_device_count == 1 && !esp_timer_is_active(s_poll_timer)) {
        esp_timer_start_periodic(s_poll_timer, s_config.poll_interval_ms * 1000);
    }

    xSemaphoreGive(s_mutex);

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);
    ESP_LOGI(TAG, "Started monitoring: %s (supported: %s)",
             mac_str, device->supported ? "yes" : "unknown");

    /* Perform initial read if configured */
    if (s_config.auto_read_on_connect && device->supported) {
        uint8_t level;
        if (ble_battery_read(mac, &level) == ESP_OK) {
            xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
            device->last_level = level;
            device->last_status = ble_battery_get_status(level);
            device->last_read = (uint32_t)time(NULL);
            xSemaphoreGive(s_mutex);

            notify_callback(mac, level, device->last_status);
        }
    }

    return ESP_OK;
}

esp_err_t ble_battery_monitor_stop(const uint8_t *mac)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    ble_battery_device_t *device = find_device(mac);
    if (device == NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memset(device, 0, sizeof(ble_battery_device_t));
    s_device_count--;

    /* Stop timer if no more devices */
    if (s_device_count == 0 && esp_timer_is_active(s_poll_timer)) {
        esp_timer_stop(s_poll_timer);
    }

    xSemaphoreGive(s_mutex);

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);
    ESP_LOGI(TAG, "Stopped monitoring: %s", mac_str);

    return ESP_OK;
}

esp_err_t ble_battery_monitor_stop_all(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;

    if (esp_timer_is_active(s_poll_timer)) {
        esp_timer_stop(s_poll_timer);
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Stopped all monitoring");
    return ESP_OK;
}

bool ble_battery_is_monitored(const uint8_t *mac)
{
    if (!s_initialized || mac == NULL) {
        return false;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    ble_battery_device_t *device = find_device(mac);
    xSemaphoreGive(s_mutex);

    return device != NULL;
}

uint8_t ble_battery_get_monitored_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    return s_device_count;
}

esp_err_t ble_battery_get_info(const uint8_t *mac, ble_battery_info_t *info)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mac == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    ble_battery_device_t *device = find_device(mac);
    if (device == NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(info->mac, device->mac, 6);
    info->level = device->last_level;
    info->status = device->last_status;
    info->last_read = device->last_read;
    info->supported = device->supported;
    info->monitoring = device->active;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

uint8_t ble_battery_get_all_info(ble_battery_info_t *info_array, uint8_t max_count)
{
    if (!s_initialized || info_array == NULL || max_count == 0) {
        return 0;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    uint8_t count = 0;
    for (uint8_t i = 0; i < BLE_BATTERY_MAX_MONITORED_DEVICES && count < max_count; i++) {
        if (s_devices[i].active) {
            memcpy(info_array[count].mac, s_devices[i].mac, 6);
            info_array[count].level = s_devices[i].last_level;
            info_array[count].status = s_devices[i].last_status;
            info_array[count].last_read = s_devices[i].last_read;
            info_array[count].supported = s_devices[i].supported;
            info_array[count].monitoring = true;
            count++;
        }
    }

    xSemaphoreGive(s_mutex);
    return count;
}

ble_battery_status_t ble_battery_get_status(uint8_t level)
{
    if (level <= s_config.critical_threshold) {
        return BLE_BATTERY_STATUS_CRITICAL;
    }
    if (level <= s_config.low_threshold) {
        return BLE_BATTERY_STATUS_LOW;
    }
    return BLE_BATTERY_STATUS_NORMAL;
}

const char *ble_battery_status_to_string(ble_battery_status_t status)
{
    switch (status) {
        case BLE_BATTERY_STATUS_NORMAL:     return "normal";
        case BLE_BATTERY_STATUS_LOW:        return "low";
        case BLE_BATTERY_STATUS_CRITICAL:   return "critical";
        default:                            return "unknown";
    }
}

esp_err_t ble_battery_register_callback(ble_battery_cb_t callback)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    s_callback = callback;
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t ble_battery_publish_mqtt(const uint8_t *mac, uint8_t level)
{
    if (!s_initialized || mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* TODO: Implement MQTT publishing */
    /* Topic: zigbee2mqtt/ble/<mac>/battery */
    /* Payload: {"battery": level, "battery_low": bool, "status": "normal|low|critical"} */

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);
    ESP_LOGD(TAG, "MQTT publish: %s battery=%d%%", mac_str, level);

    return ESP_OK;
}

esp_err_t ble_battery_publish_ha_discovery(const uint8_t *mac, const char *device_name)
{
    if (!s_initialized || mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* TODO: Implement Home Assistant discovery */
    /* Topic: homeassistant/sensor/<mac>_battery/config */

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);
    ESP_LOGD(TAG, "HA discovery: %s (%s)", mac_str, device_name ? device_name : "unknown");

    return ESP_OK;
}

/* ============================================================================
 * Private Function Implementation
 * ============================================================================ */

/**
 * @brief Polling timer callback
 */
static void poll_timer_callback(void *arg)
{
    (void)arg;

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    for (uint8_t i = 0; i < BLE_BATTERY_MAX_MONITORED_DEVICES; i++) {
        if (!s_devices[i].active || !s_devices[i].supported) {
            continue;
        }

        /* Check if device is still connected */
        if (!ble_gatt_is_connected(s_devices[i].mac)) {
            continue;
        }

        /* Read battery level */
        uint8_t level;
        if (ble_battery_read(s_devices[i].mac, &level) == ESP_OK) {
            ble_battery_status_t new_status = ble_battery_get_status(level);

            /* Check for changes */
            bool level_changed = (level != s_devices[i].last_level);
            bool status_changed = (new_status != s_devices[i].last_status);

            s_devices[i].last_level = level;
            s_devices[i].last_status = new_status;
            s_devices[i].last_read = (uint32_t)time(NULL);

            /* Notify on change */
            if (level_changed || status_changed) {
                xSemaphoreGive(s_mutex);
                notify_callback(s_devices[i].mac, level, new_status);
                xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
            }
        }
    }

    xSemaphoreGive(s_mutex);
}

/**
 * @brief Find device by MAC address
 */
static ble_battery_device_t *find_device(const uint8_t *mac)
{
    for (uint8_t i = 0; i < BLE_BATTERY_MAX_MONITORED_DEVICES; i++) {
        if (s_devices[i].active && ble_mac_equal(s_devices[i].mac, mac)) {
            return &s_devices[i];
        }
    }
    return NULL;
}

/**
 * @brief Find free slot in device array
 */
static ble_battery_device_t *find_free_slot(void)
{
    for (uint8_t i = 0; i < BLE_BATTERY_MAX_MONITORED_DEVICES; i++) {
        if (!s_devices[i].active) {
            return &s_devices[i];
        }
    }
    return NULL;
}

/**
 * @brief Read battery level via GATT
 */
static esp_err_t read_battery_internal(uint16_t conn_handle, uint8_t *level)
{
    /* Create UUID for battery level characteristic */
    ble_gatt_uuid_t battery_uuid = ble_gatt_uuid16(BLE_BATTERY_LEVEL_CHAR_UUID);

    /* Read by UUID (searches entire handle range) */
    uint8_t buf[1];
    uint16_t len = sizeof(buf);

    esp_err_t ret = ble_gatt_read_by_uuid(conn_handle, 0x0001, 0xFFFF,
                                           &battery_uuid, buf, &len,
                                           BLE_BATTERY_READ_TIMEOUT_MS);

    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Battery read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (len < 1) {
        ESP_LOGW(TAG, "Invalid battery data length: %d", len);
        return ESP_ERR_INVALID_SIZE;
    }

    *level = buf[0];

    /* Validate range */
    if (*level > BLE_BATTERY_PERCENTAGE_MAX) {
        ESP_LOGW(TAG, "Invalid battery level: %d", *level);
        *level = BLE_BATTERY_PERCENTAGE_MAX;
    }

    ESP_LOGD(TAG, "Battery level: %d%%", *level);
    return ESP_OK;
}

/**
 * @brief Notify callback and publish if configured
 */
static void notify_callback(const uint8_t *mac, uint8_t level, ble_battery_status_t status)
{
    /* Invoke user callback */
    if (s_callback != NULL) {
        s_callback(mac, level, status);
    }

    /* Publish to MQTT if configured */
    if (s_config.publish_to_mqtt) {
        ble_battery_publish_mqtt(mac, level);
    }

    /* Log status changes */
    if (status == BLE_BATTERY_STATUS_LOW || status == BLE_BATTERY_STATUS_CRITICAL) {
        char mac_str[18];
        ble_mac_to_str(mac, mac_str);
        ESP_LOGW(TAG, "Battery %s: %s (%d%%)",
                 ble_battery_status_to_string(status), mac_str, level);
    }
}
