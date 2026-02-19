/**
 * @file ble_adapter.c
 * @brief BLE Adapter Implementation
 *
 * Bridges the existing BLE scanner and device parsers to the new event bus
 * and unified device registry.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_adapter.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <inttypes.h>

#include "adapter_interface.h"

#ifdef CONFIG_BT_ENABLED

#include "bluetooth/ble_scanner.h"
#include "bluetooth/ble_common.h"
#include "bluetooth/devices/ble_device_registry.h"

static const char *TAG = "BLE_ADAPTER";

/* ============================================================================
 * Adapter State
 * ============================================================================ */

typedef enum {
    ADAPTER_STATE_UNINITIALIZED = 0,
    ADAPTER_STATE_INITIALIZED,
    ADAPTER_STATE_RUNNING,
    ADAPTER_STATE_STOPPED,
} adapter_state_t;

static adapter_state_t s_adapter_state = ADAPTER_STATE_UNINITIALIZED;

/* Statistics */
static ble_adapter_stats_t s_stats = {0};

/* Last stale check timestamp */
static uint32_t s_last_stale_check_ms = 0;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void ble_adv_callback(const ble_adv_data_t *adv);
static void process_device_update(const uint8_t mac[6], const char *name,
                                   int8_t rssi, const ble_adapter_parsed_data_t *data);
static esp_err_t add_device_to_registry(device_id_t id, const uint8_t mac[6],
                                         const char *name, int8_t rssi);
static esp_err_t update_device_state(device_id_t id, int8_t rssi,
                                      const ble_adapter_parsed_data_t *data);
static void publish_device_joined(device_id_t id, const uint8_t mac[6],
                                   const char *name, const device_t *dev);
static void publish_device_found(device_id_t id, const uint8_t mac[6],
                                  const char *name, int8_t rssi);
static void publish_device_left(device_id_t id, const uint8_t mac[6]);
static void publish_device_lost(device_id_t id, const uint8_t mac[6]);
static void publish_state_changed(device_id_t id, const ble_adapter_parsed_data_t *data);
static uint32_t get_timestamp_ms(void);
static void check_stale_devices(void);

/* ============================================================================
 * Initialization / Lifecycle
 * ============================================================================ */

esp_err_t ble_adapter_init(void)
{
    if (s_adapter_state != ADAPTER_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "Adapter already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Verify dependencies are ready */
    if (!device_registry_is_initialized()) {
        ESP_LOGE(TAG, "Device registry not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Reset statistics */
    memset(&s_stats, 0, sizeof(s_stats));
    s_last_stale_check_ms = get_timestamp_ms();

    s_adapter_state = ADAPTER_STATE_INITIALIZED;
    ESP_LOGI(TAG, "BLE adapter initialized");
    return ESP_OK;
}

esp_err_t ble_adapter_start(void)
{
    if (s_adapter_state != ADAPTER_STATE_INITIALIZED &&
        s_adapter_state != ADAPTER_STATE_STOPPED) {
        ESP_LOGE(TAG, "Adapter not initialized or already running");
        return ESP_ERR_INVALID_STATE;
    }

    /* Register callback with BLE scanner - works even before scanner init
     * (callbacks stored in static array, used once scanner starts scanning) */
    esp_err_t ret = ble_scanner_register_callback(ble_adv_callback);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register scanner callback: %s", esp_err_to_name(ret));
    }

    s_adapter_state = ADAPTER_STATE_RUNNING;
    ESP_LOGI(TAG, "BLE adapter started (scanner: %s)", ble_scanner_get_state_str());
    return ESP_OK;
}

esp_err_t ble_adapter_stop(void)
{
    if (s_adapter_state != ADAPTER_STATE_RUNNING) {
        ESP_LOGW(TAG, "Adapter not running");
        return ESP_ERR_INVALID_STATE;
    }

    /* Unregister callback from BLE scanner */
    esp_err_t ret = ble_scanner_unregister_callback(ble_adv_callback);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to unregister scanner callback: %s", esp_err_to_name(ret));
    }

    /* Note: We don't stop the BLE scanner here as other modules may be using it */

    s_adapter_state = ADAPTER_STATE_STOPPED;
    ESP_LOGI(TAG, "BLE adapter stopped");
    return ESP_OK;
}

esp_err_t ble_adapter_deinit(void)
{
    if (s_adapter_state == ADAPTER_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop if running */
    if (s_adapter_state == ADAPTER_STATE_RUNNING) {
        ble_adapter_stop();
    }

    s_adapter_state = ADAPTER_STATE_UNINITIALIZED;
    ESP_LOGI(TAG, "BLE adapter deinitialized");
    return ESP_OK;
}

bool ble_adapter_is_initialized(void)
{
    return s_adapter_state != ADAPTER_STATE_UNINITIALIZED;
}

bool ble_adapter_is_running(void)
{
    return s_adapter_state == ADAPTER_STATE_RUNNING;
}

/* ============================================================================
 * BLE Scanner Callback
 * ============================================================================ */

/**
 * @brief Callback invoked by BLE scanner for each advertisement
 */
static void ble_adv_callback(const ble_adv_data_t *adv)
{
    if (adv == NULL || s_adapter_state != ADAPTER_STATE_RUNNING) {
        return;
    }

    s_stats.advertisements++;

    /* Pass to raw advertisement handler for presence tracking */
    ble_adapter_on_raw_advertisement(adv->mac, (uint8_t)adv->addr_type,
                                      adv->rssi, adv->adv_data, adv->adv_data_len);

    /* Periodic stale device check */
    uint32_t now = get_timestamp_ms();
    if ((now - s_last_stale_check_ms) > BLE_ADAPTER_STALE_CHECK_INTERVAL_MS) {
        s_last_stale_check_ms = now;
        check_stale_devices();
    }
}

/* ============================================================================
 * Device Data Callbacks
 * ============================================================================ */

void ble_adapter_on_device_data(const uint8_t mac[6],
                                 const char *name,
                                 int8_t rssi,
                                 const ble_adapter_parsed_data_t *data)
{
    if (mac == NULL || s_adapter_state != ADAPTER_STATE_RUNNING) {
        return;
    }

    process_device_update(mac, name, rssi, data);
}

void ble_adapter_on_raw_advertisement(const uint8_t mac[6],
                                       uint8_t addr_type,
                                       int8_t rssi,
                                       const uint8_t *adv_data,
                                       size_t adv_len)
{
    if (mac == NULL || s_adapter_state != ADAPTER_STATE_RUNNING) {
        return;
    }

    (void)adv_data;  /* May be used for future parsing */
    (void)adv_len;

    device_id_t id = ble_adapter_mac_to_device_id(mac);
    device_t *dev = device_registry_get(id);

    if (dev == NULL) {
        /* New device - add to registry */
        esp_err_t ret = add_device_to_registry(id, mac, NULL, rssi);
        if (ret == ESP_OK) {
            publish_device_found(id, mac, NULL, rssi);
        }
    } else {
        /* Existing device - update last seen */
        device_registry_update_last_seen(id);

        /* Update BLE-specific data */
        memcpy(dev->proto.ble.mac_addr, mac, 6);
        dev->proto.ble.rssi = rssi;
        dev->proto.ble.addr_type = addr_type;

        /* Update availability if was offline */
        if (dev->availability != DEV_AVAIL_ONLINE) {
            device_registry_set_availability(id, DEV_AVAIL_ONLINE);
            /* Publish device joined again when it comes back online */
            publish_device_found(id, mac, NULL, rssi);
        }
    }
}

/* ============================================================================
 * Internal Processing Functions
 * ============================================================================ */

/**
 * @brief Process a device update with parsed data
 */
static void process_device_update(const uint8_t mac[6], const char *name,
                                   int8_t rssi, const ble_adapter_parsed_data_t *data)
{
    device_id_t id = ble_adapter_mac_to_device_id(mac);
    device_t *dev = device_registry_get(id);
    bool is_new = false;

    if (dev == NULL) {
        /* New device - add to registry */
        esp_err_t ret = add_device_to_registry(id, mac, name, rssi);
        if (ret != ESP_OK) {
            s_stats.parse_errors++;
            return;
        }
        is_new = true;
        dev = device_registry_get(id);
        if (dev == NULL) {
            return;
        }
    }

    /* Update device name if provided and not already set */
    if (name != NULL && dev->friendly_name[0] == '\0') {
        strncpy(dev->friendly_name, name, sizeof(dev->friendly_name) - 1);
        dev->friendly_name[sizeof(dev->friendly_name) - 1] = '\0';
    }

    /* Update capabilities based on parsed data */
    if (data != NULL) {
        uint32_t caps = ble_adapter_get_capabilities(data);
        dev->capabilities |= caps;

        /* Set device type string as model if provided */
        if (data->device_type != NULL && dev->model[0] == '\0') {
            strncpy(dev->model, data->device_type, sizeof(dev->model) - 1);
            dev->model[sizeof(dev->model) - 1] = '\0';
        }
    }

    /* Update BLE-specific data */
    memcpy(dev->proto.ble.mac_addr, mac, 6);
    dev->proto.ble.rssi = rssi;

    /* Update availability */
    if (dev->availability != DEV_AVAIL_ONLINE) {
        device_registry_set_availability(id, DEV_AVAIL_ONLINE);
    }
    device_registry_update_last_seen(id);

    /* Publish events */
    if (is_new) {
        publish_device_found(id, mac, name, rssi);
    }

    /* Update state and publish state change if we have data */
    if (data != NULL) {
        esp_err_t ret = update_device_state(id, rssi, data);
        if (ret == ESP_OK) {
            publish_state_changed(id, data);
        }
    }
}

/**
 * @brief Add a new BLE device to the unified registry
 */
static esp_err_t add_device_to_registry(device_id_t id, const uint8_t mac[6],
                                         const char *name, int8_t rssi)
{
    device_t *dev = device_registry_add(id, DEV_PROTOCOL_BLE);
    if (dev == NULL) {
        ESP_LOGW(TAG, "Failed to add BLE device to registry");
        return ESP_ERR_NO_MEM;
    }

    /* Set friendly name if provided */
    if (name != NULL) {
        strncpy(dev->friendly_name, name, sizeof(dev->friendly_name) - 1);
        dev->friendly_name[sizeof(dev->friendly_name) - 1] = '\0';
    }

    /* Initialize BLE-specific data */
    memcpy(dev->proto.ble.mac_addr, mac, 6);
    dev->proto.ble.rssi = rssi;
    dev->proto.ble.addr_type = BLE_ADDR_TYPE_PUBLIC;
    dev->proto.ble.service_uuid = 0;

    /* Set availability */
    dev->availability = DEV_AVAIL_ONLINE;

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);
    ESP_LOGI(TAG, "Added BLE device %s to unified registry", mac_str);

    s_stats.devices_found++;
    return ESP_OK;
}

/**
 * @brief Update device state in the registry
 */
static esp_err_t update_device_state(device_id_t id, int8_t rssi,
                                      const ble_adapter_parsed_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Get or create state JSON */
    cJSON *state = device_registry_get_state(id);
    if (state == NULL) {
        state = cJSON_CreateObject();
        if (state == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t ret = device_registry_set_state(id, state);
        if (ret != ESP_OK) {
            cJSON_Delete(state);
            return ret;
        }
        /* After set_state, ownership is transferred - get fresh pointer */
        state = device_registry_get_state(id);
        if (state == NULL) {
            return ESP_FAIL;
        }
    }

    /* Update state values */
    if (data->has_temperature) {
        cJSON_DeleteItemFromObject(state, "temperature");
        cJSON_AddNumberToObject(state, "temperature", data->temperature);
    }

    if (data->has_humidity) {
        cJSON_DeleteItemFromObject(state, "humidity");
        cJSON_AddNumberToObject(state, "humidity", data->humidity);
    }

    if (data->has_pressure) {
        cJSON_DeleteItemFromObject(state, "pressure");
        cJSON_AddNumberToObject(state, "pressure", data->pressure);
    }

    if (data->has_battery) {
        cJSON_DeleteItemFromObject(state, "battery");
        cJSON_AddNumberToObject(state, "battery", data->battery);
    }

    if (data->has_motion) {
        cJSON_DeleteItemFromObject(state, "motion");
        cJSON_AddBoolToObject(state, "motion", data->motion);
    }

    /* Always update RSSI and linkquality */
    cJSON_DeleteItemFromObject(state, "rssi");
    cJSON_AddNumberToObject(state, "rssi", rssi);

    /* Add linkquality (normalized from RSSI, 0-255 scale) */
    int lqi = (rssi + 100) * 255 / 100;  /* Rough mapping: -100dBm = 0, 0dBm = 255 */
    if (lqi < 0) lqi = 0;
    if (lqi > 255) lqi = 255;
    cJSON_DeleteItemFromObject(state, "linkquality");
    cJSON_AddNumberToObject(state, "linkquality", lqi);

    s_stats.state_updates++;
    return ESP_OK;
}

/* ============================================================================
 * Event Publishing
 * ============================================================================ */

/**
 * @brief Publish EVT_DEVICE_JOINED event for unified device handling
 *
 * This allows other components (HA Discovery, MQTT adapter) to handle BLE
 * devices the same way they handle Zigbee devices.
 */
static void publish_device_joined(device_id_t id, const uint8_t mac[6],
                                   const char *name, const device_t *dev)
{
    evt_device_joined_t evt = {
        .ieee_addr = id,
        .short_addr = 0,  /* Not applicable for BLE */
        .endpoint = 0,    /* Not applicable for BLE */
        .manufacturer = (dev && dev->manufacturer[0] != '\0') ? dev->manufacturer : NULL,
        .model = (dev && dev->model[0] != '\0') ? dev->model : name,
    };

    esp_err_t ret = event_publish(EVT_DEVICE_JOINED, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish EVT_DEVICE_JOINED for BLE: %s", esp_err_to_name(ret));
    } else {
        char mac_str[18];
        ble_mac_to_str(mac, mac_str);
        ESP_LOGI(TAG, "Published EVT_DEVICE_JOINED for BLE device: %s", mac_str);
    }
}

/**
 * @brief Publish EVT_BLE_DEVICE_FOUND event
 */
static void publish_device_found(device_id_t id, const uint8_t mac[6],
                                  const char *name, int8_t rssi)
{
    /* First publish the unified EVT_DEVICE_JOINED event */
    device_t *dev = device_registry_get(id);
    publish_device_joined(id, mac, name, dev);

    /* Then publish BLE-specific EVT_BLE_DEVICE_FOUND event */
    evt_ble_device_found_t evt = {0};
    memcpy(evt.mac, mac, 6);
    evt.addr_type = BLE_ADDR_TYPE_PUBLIC;
    evt.rssi = rssi;
    evt.name = name;
    evt.service_uuid = 0;
    evt.adv_data = NULL;
    evt.adv_data_len = 0;

    esp_err_t ret = event_publish(EVT_BLE_DEVICE_FOUND, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish BLE_DEVICE_FOUND: %s", esp_err_to_name(ret));
    } else {
        char mac_str[18];
        ble_mac_to_str(mac, mac_str);
        ESP_LOGD(TAG, "Published BLE_DEVICE_FOUND: %s rssi=%d", mac_str, rssi);
    }
}

/**
 * @brief Publish EVT_DEVICE_LEFT for unified device handling (BLE device offline)
 */
static void publish_device_left(device_id_t id, const uint8_t mac[6])
{
    evt_device_left_t evt = {
        .ieee_addr = id,
        .short_addr = 0,     /* Not applicable for BLE */
        .rejoin_expected = true,  /* BLE devices may come back */
    };

    esp_err_t ret = event_publish(EVT_DEVICE_LEFT, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish EVT_DEVICE_LEFT for BLE: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief Publish EVT_BLE_DEVICE_LOST event
 */
static void publish_device_lost(device_id_t id, const uint8_t mac[6])
{
    /* First publish the unified EVT_DEVICE_LEFT event */
    publish_device_left(id, mac);

    /* Then publish BLE-specific EVT_BLE_DEVICE_LOST event */
    evt_ble_device_lost_t evt = {0};
    memcpy(evt.mac, mac, 6);
    evt.last_seen_ms = get_timestamp_ms();

    esp_err_t ret = event_publish(EVT_BLE_DEVICE_LOST, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish BLE_DEVICE_LOST: %s", esp_err_to_name(ret));
    } else {
        char mac_str[18];
        ble_mac_to_str(mac, mac_str);
        ESP_LOGI(TAG, "Published BLE_DEVICE_LOST: %s", mac_str);
    }
}

/**
 * @brief Publish EVT_DEVICE_STATE_CHANGED event
 */
static void publish_state_changed(device_id_t id, const ble_adapter_parsed_data_t *data)
{
    if (data == NULL) {
        return;
    }

    /* Build JSON state string */
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return;
    }

    if (data->has_temperature) {
        cJSON_AddNumberToObject(json, "temperature", data->temperature);
    }
    if (data->has_humidity) {
        cJSON_AddNumberToObject(json, "humidity", data->humidity);
    }
    if (data->has_pressure) {
        cJSON_AddNumberToObject(json, "pressure", data->pressure);
    }
    if (data->has_battery) {
        cJSON_AddNumberToObject(json, "battery", data->battery);
    }
    if (data->has_motion) {
        cJSON_AddBoolToObject(json, "motion", data->motion);
    }

    /* Merge into device registry (handler reads from device_registry_get_state()) */
    device_registry_merge_state(id, json);
    cJSON_Delete(json);

    /* Convert device_id_t to ieee_addr format for evt_device_state_t */
    evt_device_state_t evt = {0};
    evt.ieee_addr = id;
    evt.short_addr = 0;  /* Not applicable for BLE */
    evt.endpoint = 0;
    evt.cluster_id = 0;
    evt.attr_id = 0;
    evt.json_state = NULL;

    esp_err_t ret = event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish DEVICE_STATE_CHANGED: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Published DEVICE_STATE_CHANGED for device 0x%" PRIx64, id);
    }
}

/* ============================================================================
 * Stale Device Management
 * ============================================================================ */

/**
 * @brief Iterator callback to check for stale BLE devices
 */
typedef struct {
    uint32_t now;
    uint32_t timeout_ms;
    device_id_t stale_ids[16];  /* Buffer for stale device IDs */
    size_t stale_count;
} stale_check_ctx_t;

static bool stale_check_iterator(device_t *dev, void *ctx)
{
    stale_check_ctx_t *check = (stale_check_ctx_t *)ctx;

    /* Only check BLE devices */
    if (dev->protocol != DEV_PROTOCOL_BLE) {
        return true;  /* Continue iteration */
    }

    /* Check if device is stale */
    uint32_t age_ms = check->now - dev->last_seen;
    if (age_ms > check->timeout_ms && check->stale_count < 16) {
        check->stale_ids[check->stale_count++] = dev->id;
    }

    return true;  /* Continue iteration */
}

static void check_stale_devices(void)
{
    stale_check_ctx_t ctx = {
        .now = get_timestamp_ms(),
        .timeout_ms = BLE_DEVICE_STALE_TIMEOUT_MS,
        .stale_count = 0,
    };

    /* Find stale devices (cannot remove during iteration) */
    device_registry_iterate(stale_check_iterator, &ctx);

    /* Remove stale devices and publish events */
    for (size_t i = 0; i < ctx.stale_count; i++) {
        device_id_t id = ctx.stale_ids[i];
        device_t *dev = device_registry_get(id);

        if (dev != NULL) {
            /* Extract MAC before removal */
            uint8_t mac[6];
            mac[0] = (id >> 40) & 0xFF;
            mac[1] = (id >> 32) & 0xFF;
            mac[2] = (id >> 24) & 0xFF;
            mac[3] = (id >> 16) & 0xFF;
            mac[4] = (id >> 8) & 0xFF;
            mac[5] = id & 0xFF;

            /* Mark as offline instead of removing */
            device_registry_set_availability(id, DEV_AVAIL_OFFLINE);

            /* Publish lost event */
            publish_device_lost(id, mac);
            s_stats.devices_lost++;
        }
    }

    if (ctx.stale_count > 0) {
        ESP_LOGI(TAG, "Marked %d BLE devices as offline (stale)", (int)ctx.stale_count);
    }
}

size_t ble_adapter_prune_stale_devices(void)
{
    size_t initial_lost = s_stats.devices_lost;
    check_stale_devices();
    return s_stats.devices_lost - initial_lost;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

device_id_t ble_adapter_mac_to_device_id(const uint8_t mac[6])
{
    return device_id_from_mac(mac);
}

uint32_t ble_adapter_get_capabilities(const ble_adapter_parsed_data_t *data)
{
    uint32_t caps = 0;

    if (data == NULL) {
        return caps;
    }

    if (data->has_temperature) {
        caps |= DEV_CAP_TEMPERATURE;
    }
    if (data->has_humidity) {
        caps |= DEV_CAP_HUMIDITY;
    }
    if (data->has_pressure) {
        caps |= DEV_CAP_PRESSURE;
    }
    if (data->has_battery) {
        caps |= DEV_CAP_BATTERY;
    }
    if (data->has_motion) {
        caps |= DEV_CAP_MOTION;
    }

    return caps;
}

static uint32_t get_timestamp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void ble_adapter_get_stats(ble_adapter_stats_t *stats)
{
    if (stats != NULL) {
        memcpy(stats, &s_stats, sizeof(ble_adapter_stats_t));
    }
}

void ble_adapter_reset_stats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
}

#else /* CONFIG_BT_ENABLED */

/* ============================================================================
 * Stub Implementations (BLE Disabled)
 * ============================================================================ */

static const char *TAG = "BLE_ADAPTER";

esp_err_t ble_adapter_init(void)
{
    ESP_LOGW(TAG, "BLE adapter disabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_adapter_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_adapter_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_adapter_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_adapter_is_initialized(void)
{
    return false;
}

bool ble_adapter_is_running(void)
{
    return false;
}

void ble_adapter_on_device_data(const uint8_t mac[6],
                                 const char *name,
                                 int8_t rssi,
                                 const ble_adapter_parsed_data_t *data)
{
    (void)mac;
    (void)name;
    (void)rssi;
    (void)data;
}

void ble_adapter_on_raw_advertisement(const uint8_t mac[6],
                                       uint8_t addr_type,
                                       int8_t rssi,
                                       const uint8_t *adv_data,
                                       size_t adv_len)
{
    (void)mac;
    (void)addr_type;
    (void)rssi;
    (void)adv_data;
    (void)adv_len;
}

size_t ble_adapter_prune_stale_devices(void)
{
    return 0;
}

device_id_t ble_adapter_mac_to_device_id(const uint8_t mac[6])
{
    (void)mac;
    return 0;
}

uint32_t ble_adapter_get_capabilities(const ble_adapter_parsed_data_t *data)
{
    (void)data;
    return 0;
}

void ble_adapter_get_stats(ble_adapter_stats_t *stats)
{
    if (stats != NULL) {
        memset(stats, 0, sizeof(ble_adapter_stats_t));
    }
}

void ble_adapter_reset_stats(void)
{
}

#endif /* CONFIG_BT_ENABLED */

/* Adapter interface vtable */
const adapter_ops_t ble_adapter_ops = {
    .init = ble_adapter_init,
    .start = ble_adapter_start,
    .stop = ble_adapter_stop,
    .deinit = ble_adapter_deinit,
    .is_running = ble_adapter_is_running,
};
