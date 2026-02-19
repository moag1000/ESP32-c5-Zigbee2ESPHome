/**
 * @file ble_esphome_bridge.c
 * @brief BLE to ESPHome Bridge Implementation
 *
 * Bridges BLE device data from the device registry to ESPHome entities,
 * enabling automatic discovery and state updates in Home Assistant.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_esphome_bridge.h"
#include "ble_common.h"
#include "devices/ble_device_registry.h"
#include "esphome_entities.h"
#include "esphome_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include <string.h>
#include <math.h>

static const char *TAG = "BLE_ESPHOME";

/* ============================================================================
 * Internal Data Structures
 * ============================================================================ */

/**
 * @brief Internal bridged device entry
 */
typedef struct {
    uint8_t mac[6];                          /**< Device MAC address */
    char name[32];                           /**< Device display name */
    ble_bridge_status_t status;              /**< Registration status */
    uint32_t entity_key_base;                /**< Base entity key */
    bool has_temperature;                    /**< Has temperature sensor */
    bool has_humidity;                       /**< Has humidity sensor */
    bool has_battery;                        /**< Has battery sensor */
    bool is_beacon;                          /**< Is a beacon */
    uint32_t last_update_ms;                 /**< Last update timestamp (ms) */
    float last_temperature;                  /**< Last temperature value */
    float last_humidity;                     /**< Last humidity value */
    int8_t last_battery;                     /**< Last battery value */
    int8_t last_rssi;                        /**< Last RSSI value */
    bool last_presence;                      /**< Last presence state */
    bool active;                             /**< Slot in use */
} bridge_device_entry_t;

/**
 * @brief Bridge module state
 */
typedef struct {
    bool initialized;
    bool running;
    ble_esphome_bridge_config_t config;
    bridge_device_entry_t devices[BLE_ESPHOME_MAX_DEVICES];
    uint32_t device_count;
    ble_esphome_bridge_stats_t stats;
    SemaphoreHandle_t mutex;
} bridge_state_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

static bridge_state_t s_bridge = {0};

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static bridge_device_entry_t *find_device_by_mac(const uint8_t mac[6]);
static bridge_device_entry_t *allocate_device_slot(void);
static void free_device_slot(bridge_device_entry_t *device);
static esp_err_t create_sensor_entities(bridge_device_entry_t *device);
static esp_err_t create_beacon_entities(bridge_device_entry_t *device);
static void device_registry_callback(const ble_registry_tracked_device_t *device, bool is_new);
static void format_entity_unique_id(const uint8_t mac[6], const char *suffix,
                                     char *buf, size_t buf_len);
static uint32_t get_current_time_ms(void);

/* ============================================================================
 * Initialization and Control
 * ============================================================================ */

esp_err_t ble_esphome_bridge_init(void)
{
    ble_esphome_bridge_config_t config = BLE_ESPHOME_BRIDGE_CONFIG_DEFAULT();
    return ble_esphome_bridge_init_with_config(&config);
}

esp_err_t ble_esphome_bridge_init_with_config(const ble_esphome_bridge_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Configuration is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_bridge.initialized) {
        ESP_LOGW(TAG, "Bridge already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing BLE-ESPHome bridge...");

    /* Create mutex */
    s_bridge.mutex = xSemaphoreCreateMutex();
    if (s_bridge.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Store configuration */
    s_bridge.config = *config;

    /* Initialize device array */
    memset(s_bridge.devices, 0, sizeof(s_bridge.devices));
    s_bridge.device_count = 0;

    /* Reset statistics */
    memset(&s_bridge.stats, 0, sizeof(s_bridge.stats));

    s_bridge.initialized = true;
    s_bridge.running = false;

    ESP_LOGI(TAG, "BLE-ESPHome bridge initialized (auto_register=%d, max_devices=%d)",
             config->auto_register, BLE_ESPHOME_MAX_DEVICES);

    return ESP_OK;
}

esp_err_t ble_esphome_bridge_deinit(void)
{
    if (!s_bridge.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing BLE-ESPHome bridge...");

    /* Stop if running */
    if (s_bridge.running) {
        ble_esphome_bridge_stop();
    }

    /* Delete mutex */
    if (s_bridge.mutex != NULL) {
        vSemaphoreDelete(s_bridge.mutex);
        s_bridge.mutex = NULL;
    }

    /* Clear state */
    memset(&s_bridge, 0, sizeof(s_bridge));

    ESP_LOGI(TAG, "BLE-ESPHome bridge deinitialized");

    return ESP_OK;
}

bool ble_esphome_bridge_is_initialized(void)
{
    return s_bridge.initialized;
}

esp_err_t ble_esphome_bridge_start(void)
{
    if (!s_bridge.initialized) {
        ESP_LOGE(TAG, "Bridge not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_bridge.running) {
        ESP_LOGW(TAG, "Bridge already running");
        return ESP_OK;
    }

    /* Verify device registry is initialized */
    if (!ble_device_registry_is_initialized()) {
        ESP_LOGE(TAG, "BLE device registry not initialized - cannot start bridge");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting BLE-ESPHome bridge...");

    /* Register callback with device registry */
    esp_err_t ret = ble_device_registry_register_callback(device_registry_callback);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register device registry callback: %s", esp_err_to_name(ret));
        /* Continue anyway, manual registration still works */
    }

    s_bridge.running = true;

    ESP_LOGI(TAG, "BLE-ESPHome bridge started");

    return ESP_OK;
}

esp_err_t ble_esphome_bridge_stop(void)
{
    if (!s_bridge.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping BLE-ESPHome bridge...");

    /* Unregister callback */
    ble_device_registry_register_callback(NULL);

    s_bridge.running = false;

    ESP_LOGI(TAG, "BLE-ESPHome bridge stopped");

    return ESP_OK;
}

/* ============================================================================
 * Device Registration
 * ============================================================================ */

esp_err_t ble_esphome_bridge_register_device(const uint8_t mac[6], const char *name)
{
    if (mac == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_bridge.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);

    xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);

    /* Check if already registered */
    bridge_device_entry_t *device = find_device_by_mac(mac);
    if (device != NULL) {
        ESP_LOGD(TAG, "Device %s already registered", mac_str);
        xSemaphoreGive(s_bridge.mutex);
        return ESP_OK;
    }

    /* Allocate new slot */
    device = allocate_device_slot();
    if (device == NULL) {
        xSemaphoreGive(s_bridge.mutex);
        ESP_LOGE(TAG, "No free device slots for %s", mac_str);
        return ESP_ERR_NO_MEM;
    }

    /* Initialize device entry */
    memcpy(device->mac, mac, 6);
    strncpy(device->name, name, sizeof(device->name) - 1);
    device->name[sizeof(device->name) - 1] = '\0';
    device->status = BLE_BRIDGE_STATUS_REGISTERED;
    device->entity_key_base = BLE_ESPHOME_ENTITY_KEY_BASE +
                               (s_bridge.device_count * BLE_ENTITY_SLOTS_PER_DEVICE);
    device->last_update_ms = get_current_time_ms();
    device->active = true;
    device->last_battery = -1;
    device->last_rssi = -127;
    device->last_temperature = NAN;
    device->last_humidity = NAN;

    s_bridge.device_count++;
    s_bridge.stats.devices_registered++;

    xSemaphoreGive(s_bridge.mutex);

    ESP_LOGI(TAG, "Registered device %s (%s) with entity key base 0x%04lX",
             mac_str, name, (unsigned long)device->entity_key_base);

    return ESP_OK;
}

esp_err_t ble_esphome_bridge_unregister_device(const uint8_t mac[6])
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_bridge.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);

    bridge_device_entry_t *device = find_device_by_mac(mac);
    if (device == NULL) {
        xSemaphoreGive(s_bridge.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);

    /* Mark entities as unavailable */
    if (device->has_temperature) {
        esphome_entity_set_sensor_missing(device->entity_key_base + BLE_ENTITY_OFFSET_TEMPERATURE);
    }
    if (device->has_humidity) {
        esphome_entity_set_sensor_missing(device->entity_key_base + BLE_ENTITY_OFFSET_HUMIDITY);
    }
    if (device->has_battery) {
        esphome_entity_set_sensor_missing(device->entity_key_base + BLE_ENTITY_OFFSET_BATTERY);
    }
    if (device->is_beacon) {
        esphome_entity_set_binary_sensor_missing(device->entity_key_base + BLE_ENTITY_OFFSET_PRESENCE);
    }

    free_device_slot(device);
    s_bridge.device_count--;

    xSemaphoreGive(s_bridge.mutex);

    ESP_LOGI(TAG, "Unregistered device %s", mac_str);

    return ESP_OK;
}

bool ble_esphome_bridge_is_device_registered(const uint8_t mac[6])
{
    if (mac == NULL || !s_bridge.initialized) {
        return false;
    }

    xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);
    bridge_device_entry_t *device = find_device_by_mac(mac);
    xSemaphoreGive(s_bridge.mutex);

    return (device != NULL);
}

esp_err_t ble_esphome_bridge_get_device(const uint8_t mac[6], ble_bridged_device_t *device)
{
    if (mac == NULL || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_bridge.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);

    bridge_device_entry_t *entry = find_device_by_mac(mac);
    if (entry == NULL) {
        xSemaphoreGive(s_bridge.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Copy to output */
    memcpy(device->mac, entry->mac, 6);
    strncpy(device->name, entry->name, sizeof(device->name) - 1);
    device->name[sizeof(device->name) - 1] = '\0';
    device->status = entry->status;
    device->entity_key_base = entry->entity_key_base;
    device->has_temperature = entry->has_temperature;
    device->has_humidity = entry->has_humidity;
    device->has_battery = entry->has_battery;
    device->is_beacon = entry->is_beacon;
    device->last_update = entry->last_update_ms / 1000;

    xSemaphoreGive(s_bridge.mutex);

    return ESP_OK;
}

size_t ble_esphome_bridge_get_all_devices(ble_bridged_device_t *devices, size_t max_count)
{
    if (devices == NULL || max_count == 0 || !s_bridge.initialized) {
        return 0;
    }

    xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);

    size_t count = 0;
    for (size_t i = 0; i < BLE_ESPHOME_MAX_DEVICES && count < max_count; i++) {
        if (s_bridge.devices[i].active) {
            bridge_device_entry_t *entry = &s_bridge.devices[i];
            memcpy(devices[count].mac, entry->mac, 6);
            strncpy(devices[count].name, entry->name, sizeof(devices[count].name) - 1);
            devices[count].name[sizeof(devices[count].name) - 1] = '\0';
            devices[count].status = entry->status;
            devices[count].entity_key_base = entry->entity_key_base;
            devices[count].has_temperature = entry->has_temperature;
            devices[count].has_humidity = entry->has_humidity;
            devices[count].has_battery = entry->has_battery;
            devices[count].is_beacon = entry->is_beacon;
            devices[count].last_update = entry->last_update_ms / 1000;
            count++;
        }
    }

    xSemaphoreGive(s_bridge.mutex);

    return count;
}

/* ============================================================================
 * State Updates
 * ============================================================================ */

esp_err_t ble_esphome_bridge_update_sensor(const uint8_t mac[6],
                                            float temperature,
                                            float humidity,
                                            int8_t battery,
                                            int8_t rssi)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_bridge.initialized || !s_bridge.running) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);

    bridge_device_entry_t *device = find_device_by_mac(mac);
    if (device == NULL) {
        xSemaphoreGive(s_bridge.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Rate limiting check */
    uint32_t now = get_current_time_ms();
    if ((now - device->last_update_ms) < s_bridge.config.update_interval_ms) {
        xSemaphoreGive(s_bridge.mutex);
        return ESP_OK;  /* Skip update, too soon */
    }

    /* Create entities if this is first sensor data */
    if (!device->has_temperature && !device->has_humidity && !isnan(temperature)) {
        device->has_temperature = true;
        device->has_humidity = !isnan(humidity);
        xSemaphoreGive(s_bridge.mutex);
        create_sensor_entities(device);
        xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);
    }

    /* Create battery entity if first battery data */
    if (!device->has_battery && battery >= 0 && s_bridge.config.register_battery) {
        device->has_battery = true;
        /* Register battery sensor */
        char unique_id[64];
        format_entity_unique_id(mac, "battery", unique_id, sizeof(unique_id));

        esphome_sensor_config_t battery_config = {
            .key = device->entity_key_base + BLE_ENTITY_OFFSET_BATTERY,
            .accuracy_decimals = 0,
            .force_update = false,
            .device_class = ESPHOME_SENSOR_CLASS_BATTERY,
            .state_class = ESPHOME_STATE_CLASS_MEASUREMENT,
            .disabled_by_default = false
        };
        snprintf(battery_config.name, sizeof(battery_config.name), "%s Battery", device->name);
        strncpy(battery_config.unique_id, unique_id, sizeof(battery_config.unique_id) - 1);
        strncpy(battery_config.unit_of_measurement, "%", sizeof(battery_config.unit_of_measurement) - 1);
        strncpy(battery_config.icon, "mdi:battery", sizeof(battery_config.icon) - 1);

        esphome_entity_register_sensor(&battery_config);
        s_bridge.stats.entities_created++;
    }

    uint32_t entity_key;

    /* Update temperature */
    if (!isnan(temperature) && device->has_temperature) {
        entity_key = device->entity_key_base + BLE_ENTITY_OFFSET_TEMPERATURE;
        esphome_entity_update_sensor(entity_key, temperature);
        device->last_temperature = temperature;
        s_bridge.stats.state_updates++;
    }

    /* Update humidity */
    if (!isnan(humidity) && device->has_humidity) {
        entity_key = device->entity_key_base + BLE_ENTITY_OFFSET_HUMIDITY;
        esphome_entity_update_sensor(entity_key, humidity);
        device->last_humidity = humidity;
        s_bridge.stats.state_updates++;
    }

    /* Update battery */
    if (battery >= 0 && device->has_battery) {
        entity_key = device->entity_key_base + BLE_ENTITY_OFFSET_BATTERY;
        esphome_entity_update_sensor(entity_key, (float)battery);
        device->last_battery = battery;
        s_bridge.stats.state_updates++;
    }

    /* Update RSSI if configured */
    if (rssi > -127 && s_bridge.config.register_rssi) {
        /* RSSI sensor would be registered here if enabled */
        device->last_rssi = rssi;
    }

    device->last_update_ms = now;
    device->status = BLE_BRIDGE_STATUS_ACTIVE;

    xSemaphoreGive(s_bridge.mutex);

    return ESP_OK;
}

esp_err_t ble_esphome_bridge_update_beacon(const uint8_t mac[6],
                                            bool present,
                                            int8_t rssi)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_bridge.initialized || !s_bridge.running) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);

    bridge_device_entry_t *device = find_device_by_mac(mac);
    if (device == NULL) {
        xSemaphoreGive(s_bridge.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Create beacon entities if this is first beacon update */
    if (!device->is_beacon && s_bridge.config.register_beacons) {
        device->is_beacon = true;
        xSemaphoreGive(s_bridge.mutex);
        create_beacon_entities(device);
        xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);
    }

    uint32_t now = get_current_time_ms();

    /* Update presence */
    if (device->is_beacon) {
        bool presence_changed = (device->last_presence != present);
        device->last_presence = present;
        device->last_rssi = rssi;
        device->last_update_ms = now;

        uint32_t entity_key = device->entity_key_base + BLE_ENTITY_OFFSET_PRESENCE;
        esphome_entity_update_binary_sensor(entity_key, present);
        s_bridge.stats.state_updates++;

        if (presence_changed) {
            s_bridge.stats.presence_changes++;
            char mac_str[18];
            ble_mac_to_str(mac, mac_str);
            ESP_LOGI(TAG, "Beacon %s presence: %s (RSSI: %d)",
                     mac_str, present ? "PRESENT" : "ABSENT", rssi);
        }
    }

    device->status = present ? BLE_BRIDGE_STATUS_ACTIVE : BLE_BRIDGE_STATUS_STALE;

    xSemaphoreGive(s_bridge.mutex);

    return ESP_OK;
}

size_t ble_esphome_bridge_process_presence_timeout(void)
{
    if (!s_bridge.initialized || !s_bridge.running) {
        return 0;
    }

    uint32_t now = get_current_time_ms();
    uint32_t timeout_ms = s_bridge.config.presence_timeout_s * 1000;
    size_t marked_absent = 0;

    xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);

    for (size_t i = 0; i < BLE_ESPHOME_MAX_DEVICES; i++) {
        bridge_device_entry_t *device = &s_bridge.devices[i];
        if (!device->active || !device->is_beacon) {
            continue;
        }

        if (device->last_presence && (now - device->last_update_ms) > timeout_ms) {
            /* Mark as absent */
            device->last_presence = false;
            device->status = BLE_BRIDGE_STATUS_STALE;

            uint32_t entity_key = device->entity_key_base + BLE_ENTITY_OFFSET_PRESENCE;
            esphome_entity_update_binary_sensor(entity_key, false);

            char mac_str[18];
            ble_mac_to_str(device->mac, mac_str);
            ESP_LOGI(TAG, "Beacon %s marked absent (timeout)", mac_str);

            s_bridge.stats.presence_changes++;
            marked_absent++;
        }
    }

    xSemaphoreGive(s_bridge.mutex);

    return marked_absent;
}

/* ============================================================================
 * Statistics and Debug
 * ============================================================================ */

esp_err_t ble_esphome_bridge_get_stats(ble_esphome_bridge_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_bridge.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);
    *stats = s_bridge.stats;
    xSemaphoreGive(s_bridge.mutex);

    return ESP_OK;
}

esp_err_t ble_esphome_bridge_reset_stats(void)
{
    if (!s_bridge.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bridge.mutex, GW_TIMEOUT_LONG_TICKS);
    s_bridge.stats.state_updates = 0;
    s_bridge.stats.presence_changes = 0;
    /* Keep devices_registered and entities_created */
    xSemaphoreGive(s_bridge.mutex);

    return ESP_OK;
}

esp_err_t ble_esphome_bridge_self_test(void)
{
    ESP_LOGI(TAG, "Running BLE-ESPHome bridge self-test...");

    if (!s_bridge.initialized) {
        ESP_LOGE(TAG, "Bridge not initialized");
        return ESP_FAIL;
    }

    if (s_bridge.mutex == NULL) {
        ESP_LOGE(TAG, "Mutex not created");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Self-test PASSED (devices: %lu, entities: %lu)",
             (unsigned long)s_bridge.stats.devices_registered,
             (unsigned long)s_bridge.stats.entities_created);

    return ESP_OK;
}

void ble_esphome_bridge_log_status(void)
{
    if (!s_bridge.initialized) {
        ESP_LOGI(TAG, "Bridge not initialized");
        return;
    }

    ESP_LOGI(TAG, "BLE-ESPHome Bridge Status:");
    ESP_LOGI(TAG, "  Running: %s", s_bridge.running ? "yes" : "no");
    ESP_LOGI(TAG, "  Devices registered: %lu", (unsigned long)s_bridge.stats.devices_registered);
    ESP_LOGI(TAG, "  Entities created: %lu", (unsigned long)s_bridge.stats.entities_created);
    ESP_LOGI(TAG, "  State updates: %lu", (unsigned long)s_bridge.stats.state_updates);
    ESP_LOGI(TAG, "  Presence changes: %lu", (unsigned long)s_bridge.stats.presence_changes);
}

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

static bridge_device_entry_t *find_device_by_mac(const uint8_t mac[6])
{
    for (size_t i = 0; i < BLE_ESPHOME_MAX_DEVICES; i++) {
        if (s_bridge.devices[i].active &&
            memcmp(s_bridge.devices[i].mac, mac, 6) == 0) {
            return &s_bridge.devices[i];
        }
    }
    return NULL;
}

static bridge_device_entry_t *allocate_device_slot(void)
{
    for (size_t i = 0; i < BLE_ESPHOME_MAX_DEVICES; i++) {
        if (!s_bridge.devices[i].active) {
            memset(&s_bridge.devices[i], 0, sizeof(bridge_device_entry_t));
            return &s_bridge.devices[i];
        }
    }
    return NULL;
}

static void free_device_slot(bridge_device_entry_t *device)
{
    if (device != NULL) {
        memset(device, 0, sizeof(bridge_device_entry_t));
    }
}

static esp_err_t create_sensor_entities(bridge_device_entry_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char unique_id[64];
    esp_err_t ret;

    /* Create temperature sensor */
    if (device->has_temperature && s_bridge.config.register_sensors) {
        format_entity_unique_id(device->mac, "temperature", unique_id, sizeof(unique_id));

        esphome_sensor_config_t temp_config = {
            .key = device->entity_key_base + BLE_ENTITY_OFFSET_TEMPERATURE,
            .accuracy_decimals = 1,
            .force_update = false,
            .device_class = ESPHOME_SENSOR_CLASS_TEMPERATURE,
            .state_class = ESPHOME_STATE_CLASS_MEASUREMENT,
            .disabled_by_default = false
        };
        snprintf(temp_config.name, sizeof(temp_config.name), "%s Temperature", device->name);
        strncpy(temp_config.unique_id, unique_id, sizeof(temp_config.unique_id) - 1);
        strncpy(temp_config.unit_of_measurement, "°C", sizeof(temp_config.unit_of_measurement) - 1);
        strncpy(temp_config.icon, "mdi:thermometer", sizeof(temp_config.icon) - 1);

        ret = esphome_entity_register_sensor(&temp_config);
        if (ret == ESP_OK) {
            s_bridge.stats.entities_created++;
            ESP_LOGD(TAG, "Created temperature sensor for %s", device->name);
        }
    }

    /* Create humidity sensor */
    if (device->has_humidity && s_bridge.config.register_sensors) {
        format_entity_unique_id(device->mac, "humidity", unique_id, sizeof(unique_id));

        esphome_sensor_config_t hum_config = {
            .key = device->entity_key_base + BLE_ENTITY_OFFSET_HUMIDITY,
            .accuracy_decimals = 0,
            .force_update = false,
            .device_class = ESPHOME_SENSOR_CLASS_HUMIDITY,
            .state_class = ESPHOME_STATE_CLASS_MEASUREMENT,
            .disabled_by_default = false
        };
        snprintf(hum_config.name, sizeof(hum_config.name), "%s Humidity", device->name);
        strncpy(hum_config.unique_id, unique_id, sizeof(hum_config.unique_id) - 1);
        strncpy(hum_config.unit_of_measurement, "%", sizeof(hum_config.unit_of_measurement) - 1);
        strncpy(hum_config.icon, "mdi:water-percent", sizeof(hum_config.icon) - 1);

        ret = esphome_entity_register_sensor(&hum_config);
        if (ret == ESP_OK) {
            s_bridge.stats.entities_created++;
            ESP_LOGD(TAG, "Created humidity sensor for %s", device->name);
        }
    }

    return ESP_OK;
}

static esp_err_t create_beacon_entities(bridge_device_entry_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char unique_id[64];

    /* Create presence binary sensor */
    format_entity_unique_id(device->mac, "presence", unique_id, sizeof(unique_id));

    esphome_binary_sensor_config_t presence_config = {
        .key = device->entity_key_base + BLE_ENTITY_OFFSET_PRESENCE,
        .device_class = ESPHOME_BINARY_CLASS_PRESENCE,
        .is_status_binary_sensor = false,
        .disabled_by_default = false
    };
    snprintf(presence_config.name, sizeof(presence_config.name), "%s Presence", device->name);
    strncpy(presence_config.unique_id, unique_id, sizeof(presence_config.unique_id) - 1);
    strncpy(presence_config.icon, "mdi:bluetooth", sizeof(presence_config.icon) - 1);

    esp_err_t ret = esphome_entity_register_binary_sensor(&presence_config);
    if (ret == ESP_OK) {
        s_bridge.stats.entities_created++;
        ESP_LOGD(TAG, "Created presence sensor for %s", device->name);
    }

    return ret;
}

static void device_registry_callback(const ble_registry_tracked_device_t *device, bool is_new)
{
    if (device == NULL || !s_bridge.running) {
        return;
    }

    /* Auto-register new devices if configured */
    if (is_new && s_bridge.config.auto_register) {
        const char *name = device->name[0] != '\0' ? device->name : "BLE Device";
        ble_esphome_bridge_register_device(device->mac, name);
    }

    /* Update based on device type */
    switch (device->type) {
        case BLE_REG_DEVICE_TYPE_XIAOMI_LYWSD03MMC:
        case BLE_REG_DEVICE_TYPE_GOVEE_H5075:
        case BLE_REG_DEVICE_TYPE_GENERIC_SENSOR:
            ble_esphome_bridge_update_sensor(
                device->mac,
                device->data.sensor.valid ? device->data.sensor.temperature : NAN,
                device->data.sensor.valid ? device->data.sensor.humidity : NAN,
                device->data.sensor.valid ? device->data.sensor.battery : -1,
                device->data.sensor.rssi
            );
            break;

        case BLE_REG_DEVICE_TYPE_IBEACON:
        case BLE_REG_DEVICE_TYPE_EDDYSTONE_UID:
            ble_esphome_bridge_update_beacon(
                device->mac,
                device->data.beacon.present,
                device->data.beacon.rssi
            );
            break;

        default:
            /* Unknown device type, just update RSSI if tracking */
            break;
    }
}

static void format_entity_unique_id(const uint8_t mac[6], const char *suffix,
                                     char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "ble_%02x%02x%02x%02x%02x%02x_%s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], suffix);
}

static uint32_t get_current_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
