/**
 * @file conv_tuya_bridge.c
 * @brief Tuya Driver Bridge Converter
 *
 * Provides a wrapper that bridges the existing Tuya device driver system
 * (tuya_device_driver_t vtable) into the converter framework. This allows
 * Tuya devices to be handled through the converter pipeline while
 * reusing existing driver implementations.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/converter/zb_converter.h"
#include "zigbee/converter/zb_converter_std.h"
#include "zigbee/tuya/tuya_device_driver.h"
#include "zigbee/tuya/tuya_driver_registry.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "CONV_TUYA";

/* fz_tuya_dp is now implemented in std/zb_converter_std_tuya.c */

/**
 * @brief toZigbee converter that delegates to Tuya driver command handler
 */
esp_err_t tz_tuya_command(uint16_t short_addr, uint8_t endpoint, const cJSON *value)
{
    /* Look up the Tuya driver for this device */
    const tuya_device_driver_t *driver = tuya_driver_get(short_addr);
    if (driver == NULL || driver->handle_command == NULL) {
        ESP_LOGW(TAG, "No Tuya driver for device 0x%04X", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    /* The registry passes the full JSON command object for Tuya devices
     * (via ZB_QUIRK_TUYA_DEVICE flag). The Tuya driver's handle_command
     * expects this full object to access all command keys. */
    return driver->handle_command(short_addr, endpoint, value);
}

/* ============================================================================
 * Tuya Bridge Converter Definition
 *
 * This is a special converter that acts as a passthrough to the existing
 * Tuya driver system. It is bound to devices that match a Tuya driver.
 * ============================================================================ */

static const zb_expose_t s_tuya_bridge_exposes[] = {
    {
        .type = ZB_EXPOSE_SWITCH,
        .name = NULL,
        .property = "state",
        .endpoint = 0,
        .access = EA_STATE_SET,
        .features = 0,
        .device_class = NULL,
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_tuya_bridge_fz[] = {
    { .cluster_id = 0xEF00, .attr_id = 0xFFFF, .json_key = "tuya_dp", .convert = fz_tuya_dp },
};

static const zb_to_zigbee_t s_tuya_bridge_tz[] = {
    { .json_key = "state", .cluster_id = 0xEF00, .convert = tz_tuya_command },
};

static const zb_converter_def_t s_def_tuya_bridge = {
    .manufacturer = NULL,  /* Matched dynamically via Tuya driver registry */
    .model = NULL,
    .vendor = "Tuya",
    .description = "Tuya device (via driver bridge)",
    .exposes = s_tuya_bridge_exposes,
    .expose_count = 1,
    .from_zigbee = s_tuya_bridge_fz,
    .from_zigbee_count = 1,
    .to_zigbee = s_tuya_bridge_tz,
    .to_zigbee_count = 1,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_TUYA_DEVICE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Registration
 * ============================================================================ */

void conv_tuya_bridge_register(void)
{
    zb_converter_register(&s_def_tuya_bridge);
    ESP_LOGI(TAG, "Tuya bridge converter registered");
}
