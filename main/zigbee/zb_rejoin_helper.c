/**
 * @file zb_rejoin_helper.c
 * @brief Zigbee Rejoin Helper — atomic short_addr update during device rejoin
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_rejoin_helper.h"
#include "zb_availability.h"
#include "zb_device_handler_types.h"  /* ZB_SHORT_ADDR_PENDING */
#include "converter/zb_converter.h"
#include "tuya/tuya_driver_registry.h"
#include "core/device/device_persistence.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "ZB_REJOIN";

void zb_rejoin_update_short_addr(device_t *dev, uint16_t old_addr, uint16_t new_addr)
{
    if (dev == NULL) {
        return;
    }

    /* Always update last_seen */
    dev->last_seen = (uint32_t)time(NULL);

    if (old_addr == new_addr) {
        ESP_LOGD(TAG, "Device 0x%04X: same short_addr, last_seen updated", new_addr);
        return;
    }

    ESP_LOGI(TAG, "Rejoin short_addr update: 0x%04X -> 0x%04X (IEEE: 0x%016llx)",
             old_addr, new_addr, (unsigned long long)dev->id);

    /* 1. Update short_addr in device struct */
    dev->proto.zigbee.short_addr = new_addr;

    /* 2. Swap availability tracker entry */
    zb_availability_remove_device(old_addr);
    zb_availability_power_type_t power = ZB_AVAIL_POWER_UNKNOWN;
    if (dev->proto.zigbee.power_info.power_info_valid) {
        if (dev->proto.zigbee.power_info.current_power_source & 0x04) {
            power = ZB_AVAIL_POWER_BATTERY;
        } else if (dev->proto.zigbee.power_info.current_power_source & 0x01) {
            power = ZB_AVAIL_POWER_MAINS;
        }
    }
    zb_availability_add_device(new_addr, power);

    /* 3. Rebind converter */
    zb_converter_unbind(old_addr);
    zb_converter_unbind(ZB_SHORT_ADDR_PENDING);
    if (dev->proto.zigbee.converter != NULL) {
        const zb_converter_def_t *conv = (const zb_converter_def_t *)dev->proto.zigbee.converter;
        zb_converter_bind(new_addr, conv);
        ESP_LOGI(TAG, "Device 0x%04X: converter rebound (rejoin)", new_addr);
    }

    /* 4. Restore Tuya driver binding */
    tuya_driver_unbind(old_addr);
    if (tuya_driver_restore_for_device(dev->id, new_addr) == ESP_OK) {
        ESP_LOGI(TAG, "Device 0x%04X: Tuya driver restored (rejoin)", new_addr);
    }

    /* 5. Persist to NVS */
    esp_err_t ret = device_persistence_save(dev);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist rejoin update for 0x%04X: %s",
                 new_addr, esp_err_to_name(ret));
    }
}
