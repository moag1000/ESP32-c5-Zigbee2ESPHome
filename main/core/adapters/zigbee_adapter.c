/**
 * @file zigbee_adapter.c
 * @brief Zigbee Adapter Implementation
 *
 * Bridges the existing ESP-Zigbee-SDK callbacks to the new event bus
 * and unified device registry.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee_adapter.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "esp_log.h"

#include "adapter_interface.h"

static const char *TAG = "ZB_ADAPTER";

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

/* ============================================================================
 * Initialization / Lifecycle
 * ============================================================================ */

esp_err_t zigbee_adapter_init(void)
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

    s_adapter_state = ADAPTER_STATE_INITIALIZED;
    ESP_LOGI(TAG, "Zigbee adapter initialized");
    return ESP_OK;
}

esp_err_t zigbee_adapter_start(void)
{
    if (s_adapter_state != ADAPTER_STATE_INITIALIZED &&
        s_adapter_state != ADAPTER_STATE_STOPPED) {
        ESP_LOGE(TAG, "Adapter not initialized or already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_adapter_state = ADAPTER_STATE_RUNNING;
    ESP_LOGI(TAG, "Zigbee adapter started");
    return ESP_OK;
}

esp_err_t zigbee_adapter_stop(void)
{
    if (s_adapter_state != ADAPTER_STATE_RUNNING) {
        ESP_LOGW(TAG, "Adapter not running");
        return ESP_ERR_INVALID_STATE;
    }

    s_adapter_state = ADAPTER_STATE_STOPPED;
    ESP_LOGI(TAG, "Zigbee adapter stopped");
    return ESP_OK;
}

esp_err_t zigbee_adapter_deinit(void)
{
    if (s_adapter_state == ADAPTER_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }

    s_adapter_state = ADAPTER_STATE_UNINITIALIZED;
    ESP_LOGI(TAG, "Zigbee adapter deinitialized");
    return ESP_OK;
}

bool zigbee_adapter_is_initialized(void)
{
    return s_adapter_state != ADAPTER_STATE_UNINITIALIZED;
}

bool zigbee_adapter_is_running(void)
{
    return s_adapter_state == ADAPTER_STATE_RUNNING;
}

esp_err_t zigbee_adapter_sync_all_devices(void)
{
    // NG registry is primary source — no sync needed
    ESP_LOGI(TAG, "NG registry is now primary source");
    return ESP_OK;
}

/* ============================================================================
 * Event Notification Hooks
 * ============================================================================ */

void zigbee_adapter_on_availability_change(uint16_t short_addr, bool online)
{
    if (s_adapter_state != ADAPTER_STATE_RUNNING) {
        return;
    }

    /* Update availability in NG registry */
    device_t *avail_dev = device_registry_get_by_short_addr(short_addr);
    if (avail_dev != NULL) {
        device_registry_set_availability(avail_dev->id,
            online ? DEV_AVAIL_ONLINE : DEV_AVAIL_OFFLINE);
    }

    /* Get device ID for event */
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return;
    }
    device_id_t id = dev->id;

    /* EVT_DEVICE_STATE_CHANGED with availability update */
    evt_device_state_t evt = {
        .ieee_addr = id,
        .short_addr = short_addr,
        .endpoint = 0,
        .cluster_id = 0,
        .attr_id = 0,
        .json_state = NULL,
    };

    event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));
}

/* Adapter interface vtable */
const adapter_ops_t zigbee_adapter_ops = {
    .init = zigbee_adapter_init,
    .start = zigbee_adapter_start,
    .stop = zigbee_adapter_stop,
    .deinit = zigbee_adapter_deinit,
    .is_running = zigbee_adapter_is_running,
};
