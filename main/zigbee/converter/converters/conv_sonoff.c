/**
 * @file conv_sonoff.c
 * @brief Sonoff SNZB Converter Definitions
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/converter/zb_converter.h"
#include "zigbee/converter/zb_converter_std.h"
#include "esp_zigbee_core.h"

/* ============================================================================
 * SNZB-01 - Sonoff Button
 * ============================================================================ */

/* TODO: Add fz_action converter for button press events */
static const zb_expose_t s_snzb01_exposes[] = {
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "battery",
        .endpoint = 0,
        .features = 0,
        .device_class = "battery",
        .unit = "%",
        .state_class = "measurement",
    },
};

static const zb_from_zigbee_t s_snzb01_fz[] = {
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
};

static const zb_converter_def_t s_def_snzb01 = {
    .manufacturer = "SONOFF",
    .model = "SNZB-01",
    .vendor = "Sonoff",
    .description = "SNZB-01 button",
    .exposes = s_snzb01_exposes,
    .expose_count = 1,
    .from_zigbee = s_snzb01_fz,
    .from_zigbee_count = 1,
    .to_zigbee = NULL,
    .to_zigbee_count = 0,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * SNZB-02 - Sonoff Temperature/Humidity Sensor
 * ============================================================================ */

static const zb_expose_t s_snzb02_exposes[] = {
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "temperature",
        .endpoint = 0,
        .features = 0,
        .device_class = "temperature",
        .unit = "\xC2\xB0""C",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "humidity",
        .endpoint = 0,
        .features = 0,
        .device_class = "humidity",
        .unit = "%",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "battery",
        .endpoint = 0,
        .features = 0,
        .device_class = "battery",
        .unit = "%",
        .state_class = "measurement",
    },
};

static const zb_from_zigbee_t s_snzb02_fz[] = {
    { .cluster_id = 0x0402, .attr_id = 0x0000, .json_key = "temperature", .convert = fz_temperature },
    { .cluster_id = 0x0405, .attr_id = 0x0000, .json_key = "humidity", .convert = fz_humidity },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
};

static const zb_converter_def_t s_def_snzb02 = {
    .manufacturer = "SONOFF",
    .model = "SNZB-02",
    .vendor = "Sonoff",
    .description = "SNZB-02 temperature and humidity sensor",
    .exposes = s_snzb02_exposes,
    .expose_count = 3,
    .from_zigbee = s_snzb02_fz,
    .from_zigbee_count = 3,
    .to_zigbee = NULL,
    .to_zigbee_count = 0,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * SNZB-03 - Sonoff Motion Sensor
 * ============================================================================ */

static const zb_expose_t s_snzb03_exposes[] = {
    {
        .type = ZB_EXPOSE_BINARY_SENSOR,
        .name = NULL,
        .endpoint = 0,
        .features = 0,
        .device_class = "motion",
        .unit = NULL,
        .state_class = NULL,
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "battery",
        .endpoint = 0,
        .features = 0,
        .device_class = "battery",
        .unit = "%",
        .state_class = "measurement",
    },
};

static const zb_from_zigbee_t s_snzb03_fz[] = {
    { .cluster_id = 0x0500, .attr_id = 0x0002, .json_key = "occupancy", .convert = fz_ias_zone_status },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
};

static const zb_converter_def_t s_def_snzb03 = {
    .manufacturer = "SONOFF",
    .model = "SNZB-03",
    .vendor = "Sonoff",
    .description = "SNZB-03 motion sensor",
    .exposes = s_snzb03_exposes,
    .expose_count = 2,
    .from_zigbee = s_snzb03_fz,
    .from_zigbee_count = 2,
    .to_zigbee = NULL,
    .to_zigbee_count = 0,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * SNZB-04 - Sonoff Contact Sensor
 * ============================================================================ */

static const zb_expose_t s_snzb04_exposes[] = {
    {
        .type = ZB_EXPOSE_BINARY_SENSOR,
        .name = NULL,
        .endpoint = 0,
        .features = 0,
        .device_class = "door",
        .unit = NULL,
        .state_class = NULL,
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "battery",
        .endpoint = 0,
        .features = 0,
        .device_class = "battery",
        .unit = "%",
        .state_class = "measurement",
    },
};

static const zb_from_zigbee_t s_snzb04_fz[] = {
    { .cluster_id = 0x0500, .attr_id = 0x0002, .json_key = "contact", .convert = fz_ias_zone_status },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
};

static const zb_converter_def_t s_def_snzb04 = {
    .manufacturer = "SONOFF",
    .model = "SNZB-04",
    .vendor = "Sonoff",
    .description = "SNZB-04 contact sensor",
    .exposes = s_snzb04_exposes,
    .expose_count = 2,
    .from_zigbee = s_snzb04_fz,
    .from_zigbee_count = 2,
    .to_zigbee = NULL,
    .to_zigbee_count = 0,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Registration
 * ============================================================================ */

void conv_sonoff_register(void)
{
    zb_converter_register(&s_def_snzb01);
    zb_converter_register(&s_def_snzb02);
    zb_converter_register(&s_def_snzb03);
    zb_converter_register(&s_def_snzb04);
}
