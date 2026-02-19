/**
 * @file zb_leave_helper.c
 * @brief Zigbee Device Leave Cleanup Helper
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_leave_helper.h"
#include "zb_availability.h"
#include "zb_network.h"
#include "converter/zb_converter.h"
#include "tuya/tuya_driver_registry.h"
#include "core/device/device_registry.h"
#include "core/device/device_persistence.h"
#include "core/bridge/bridge_request_handler.h"  /* device_options_remove */
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

static const char *TAG = "ZB_LEAVE";

void zb_device_leave_cleanup(uint64_t ieee64, uint16_t short_addr)
{
    ESP_LOGI(TAG, "Leave cleanup: 0x%04X (IEEE: 0x%016" PRIx64 ")", short_addr, ieee64);

    /* Look up device BEFORE publishing events (subscribers need device info) */
    device_t *dev = device_registry_get(ieee64);
    if (dev == NULL) {
        ESP_LOGW(TAG, "Device 0x%016" PRIx64 " not in registry, cleanup skipped", ieee64);
        return;
    }

    /* 1. Publish EVT_DEVICE_LEFT — BEFORE removal so subscribers can still
     *    look up the device in registry (ha_discovery_ng_remove_device needs it) */
    evt_device_left_t evt = {
        .ieee_addr = ieee64,
        .short_addr = short_addr,
        .rejoin_expected = false
    };
    event_publish(EVT_DEVICE_LEFT, &evt, sizeof(evt));

    /* 2. Let async event dispatcher process handlers (ha_discovery, mqtt_adapter)
     *    before we remove the device from registry */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 3. Clear retained MQTT topics (state + availability) via event bus.
     *    mqtt_event_handler.c constructs topics and publishes empty retained payloads. */
    evt_device_topics_clear_t clear_evt = {
        .ieee_addr = ieee64,
        .short_addr = short_addr
    };
    event_publish(EVT_DEVICE_TOPICS_CLEAR, &clear_evt, sizeof(clear_evt));

    /* 4. Unbind converter */
    zb_converter_unbind(short_addr);

    /* 5. Unbind Tuya driver */
    tuya_driver_unbind(short_addr);

    /* 6. Remove from availability tracker */
    zb_availability_remove_device(short_addr);

    /* 7. Remove from NG device registry (frees state JSON) */
    device_registry_remove(ieee64);

    /* 8. Delete from NVS persistence */
    device_persistence_delete(ieee64);

    /* 9. Delete device options from NVS */
    device_options_remove(ieee64);

    /* 10. Update network device count */
    size_t count = device_registry_count();
    zb_network_set_device_count((uint8_t)count);

    ESP_LOGI(TAG, "Device 0x%016" PRIx64 " removed, total: %zu", ieee64, count);
}
