/**
 * @file conv_philips.c
 * @brief Philips Hue Converter Definitions
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/converter/zb_converter.h"
#include "zigbee/converter/zb_converter_std.h"
#include "esp_zigbee_core.h"

/* ============================================================================
 * 9290012573A - Hue White A19
 * ============================================================================ */

static const zb_expose_t s_hue_white_exposes[] = {
    {
        .type = ZB_EXPOSE_LIGHT,
        .name = NULL,
        .endpoint = 0,
        .features = ZB_FEATURE_BRIGHTNESS | ZB_FEATURE_TRANSITION,
        .device_class = NULL,
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_hue_white_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .json_key = "state", .convert = fz_on_off },
    { .cluster_id = 0x0008, .attr_id = 0x0000, .json_key = "brightness", .convert = fz_brightness },
};

static const zb_to_zigbee_t s_hue_white_tz[] = {
    { .json_key = "state", .cluster_id = 0x0006, .convert = tz_on_off },
    { .json_key = "brightness", .cluster_id = 0x0008, .convert = tz_brightness },
};

static const zb_converter_def_t s_def_hue_white = {
    .manufacturer = "Signify Netherlands B.V.",
    .model = "9290012573A",
    .vendor = "Philips",
    .description = "Hue White A19",
    .exposes = s_hue_white_exposes,
    .expose_count = 1,
    .from_zigbee = s_hue_white_fz,
    .from_zigbee_count = 2,
    .to_zigbee = s_hue_white_tz,
    .to_zigbee_count = 2,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * 9290022166A - Hue Color A19 E26
 * ============================================================================ */

static const zb_expose_t s_hue_color_exposes[] = {
    {
        .type = ZB_EXPOSE_LIGHT,
        .name = NULL,
        .endpoint = 0,
        .features = ZB_FEATURE_BRIGHTNESS | ZB_FEATURE_COLOR_TEMP |
                    ZB_FEATURE_COLOR_XY | ZB_FEATURE_TRANSITION,
        .device_class = NULL,
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_hue_color_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .json_key = "state", .convert = fz_on_off },
    { .cluster_id = 0x0008, .attr_id = 0x0000, .json_key = "brightness", .convert = fz_brightness },
    { .cluster_id = 0x0300, .attr_id = 0x0007, .json_key = "color_temp", .convert = fz_color_temp },
    { .cluster_id = 0x0300, .attr_id = 0x0003, .json_key = "color_x", .convert = fz_color_xy },
    { .cluster_id = 0x0300, .attr_id = 0x0004, .json_key = "color_y", .convert = fz_color_xy },
};

static const zb_to_zigbee_t s_hue_color_tz[] = {
    { .json_key = "state", .cluster_id = 0x0006, .convert = tz_on_off },
    { .json_key = "brightness", .cluster_id = 0x0008, .convert = tz_brightness },
    { .json_key = "color_temp", .cluster_id = 0x0300, .convert = tz_color_temp },
    { .json_key = "color", .cluster_id = 0x0300, .convert = tz_color_xy },
};

static const zb_converter_def_t s_def_hue_color = {
    .manufacturer = "Signify Netherlands B.V.",
    .model = "9290022166A",
    .vendor = "Philips",
    .description = "Hue Color A19 E26",
    .exposes = s_hue_color_exposes,
    .expose_count = 1,
    .from_zigbee = s_hue_color_fz,
    .from_zigbee_count = 5,
    .to_zigbee = s_hue_color_tz,
    .to_zigbee_count = 4,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * SML001 - Hue Motion Sensor
 * ============================================================================ */

static const zb_expose_t s_hue_motion_exposes[] = {
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
        .name = "temperature",
        .endpoint = 0,
        .features = 0,
        .device_class = "temperature",
        .unit = "\xC2\xB0""C",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "illuminance",
        .endpoint = 0,
        .features = 0,
        .device_class = "illuminance",
        .unit = "lx",
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

static const zb_from_zigbee_t s_hue_motion_fz[] = {
    { .cluster_id = 0x0406, .attr_id = 0x0000, .json_key = "occupancy", .convert = fz_occupancy },
    { .cluster_id = 0x0402, .attr_id = 0x0000, .json_key = "temperature", .convert = fz_temperature },
    { .cluster_id = 0x0400, .attr_id = 0x0000, .json_key = "illuminance", .convert = fz_illuminance },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
};

static const zb_converter_def_t s_def_hue_motion = {
    .manufacturer = "Signify Netherlands B.V.",
    .model = "SML001",
    .vendor = "Philips",
    .description = "Hue motion sensor (SML001)",
    .exposes = s_hue_motion_exposes,
    .expose_count = 4,
    .from_zigbee = s_hue_motion_fz,
    .from_zigbee_count = 4,
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

void conv_philips_register(void)
{
    zb_converter_register(&s_def_hue_white);
    zb_converter_register(&s_def_hue_color);
    zb_converter_register(&s_def_hue_motion);
}
