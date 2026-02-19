/**
 * @file conv_lidl.c
 * @brief Silvercrest/Lidl Converter Definitions
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/converter/zb_converter.h"
#include "zigbee/converter/zb_converter_std.h"
#include "esp_zigbee_core.h"

/* ============================================================================
 * HG06106C - Silvercrest Color Bulb
 * ============================================================================ */

static const zb_expose_t s_hg06106c_exposes[] = {
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

static const zb_from_zigbee_t s_hg06106c_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .json_key = "state", .convert = fz_on_off },
    { .cluster_id = 0x0008, .attr_id = 0x0000, .json_key = "brightness", .convert = fz_brightness },
    { .cluster_id = 0x0300, .attr_id = 0x0007, .json_key = "color_temp", .convert = fz_color_temp },
    { .cluster_id = 0x0300, .attr_id = 0x0003, .json_key = "color_x", .convert = fz_color_xy },
    { .cluster_id = 0x0300, .attr_id = 0x0004, .json_key = "color_y", .convert = fz_color_xy },
};

static const zb_to_zigbee_t s_hg06106c_tz[] = {
    { .json_key = "state", .cluster_id = 0x0006, .convert = tz_on_off },
    { .json_key = "brightness", .cluster_id = 0x0008, .convert = tz_brightness },
    { .json_key = "color_temp", .cluster_id = 0x0300, .convert = tz_color_temp },
    { .json_key = "color", .cluster_id = 0x0300, .convert = tz_color_xy },
};

/* Note: Tuya-format manufacturer string but uses standard ZCL clusters.
 * No ZB_QUIRK_TUYA_DEVICE needed since it doesn't use cluster 0xEF00. */
static const zb_converter_def_t s_def_hg06106c = {
    .manufacturer = "_TZ3000_riwp3k79",
    .model = "HG06106C",
    .vendor = "Lidl",
    .description = "Silvercrest smart color bulb",
    .exposes = s_hg06106c_exposes,
    .expose_count = 1,
    .from_zigbee = s_hg06106c_fz,
    .from_zigbee_count = 5,
    .to_zigbee = s_hg06106c_tz,
    .to_zigbee_count = 4,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Registration
 * ============================================================================ */

void conv_lidl_register(void)
{
    zb_converter_register(&s_def_hg06106c);
}
