/**
 * @file conv_ikea.c
 * @brief IKEA TRADFRI Converter Definitions
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/converter/zb_converter.h"
#include "zigbee/converter/zb_converter_std.h"
#include "esp_zigbee_core.h"

/* ============================================================================
 * LED1545G12 - E27 980lm Color Temperature
 * ============================================================================ */

static const zb_expose_t s_led1545g12_exposes[] = {
    {
        .type = ZB_EXPOSE_LIGHT,
        .name = NULL,
        .endpoint = 0,
        .features = ZB_FEATURE_BRIGHTNESS | ZB_FEATURE_COLOR_TEMP | ZB_FEATURE_TRANSITION,
        .device_class = NULL,
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_led1545g12_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .json_key = "state", .convert = fz_on_off },
    { .cluster_id = 0x0008, .attr_id = 0x0000, .json_key = "brightness", .convert = fz_brightness },
    { .cluster_id = 0x0300, .attr_id = 0x0007, .json_key = "color_temp", .convert = fz_color_temp },
};

static const zb_to_zigbee_t s_led1545g12_tz[] = {
    { .json_key = "state", .cluster_id = 0x0006, .convert = tz_on_off },
    { .json_key = "brightness", .cluster_id = 0x0008, .convert = tz_brightness },
    { .json_key = "color_temp", .cluster_id = 0x0300, .convert = tz_color_temp },
};

static const zb_converter_def_t s_def_led1545g12 = {
    .manufacturer = "IKEA of Sweden",
    .model = "LED1545G12",
    .vendor = "IKEA",
    .description = "TRADFRI E27 980lm color temp",
    .exposes = s_led1545g12_exposes,
    .expose_count = 1,
    .from_zigbee = s_led1545g12_fz,
    .from_zigbee_count = 3,
    .to_zigbee = s_led1545g12_tz,
    .to_zigbee_count = 3,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * LED1623G12 - E27 806lm Dimmable
 * ============================================================================ */

static const zb_expose_t s_led1623g12_exposes[] = {
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

static const zb_from_zigbee_t s_led1623g12_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .json_key = "state", .convert = fz_on_off },
    { .cluster_id = 0x0008, .attr_id = 0x0000, .json_key = "brightness", .convert = fz_brightness },
};

static const zb_to_zigbee_t s_led1623g12_tz[] = {
    { .json_key = "state", .cluster_id = 0x0006, .convert = tz_on_off },
    { .json_key = "brightness", .cluster_id = 0x0008, .convert = tz_brightness },
};

static const zb_converter_def_t s_def_led1623g12 = {
    .manufacturer = "IKEA of Sweden",
    .model = "LED1623G12",
    .vendor = "IKEA",
    .description = "TRADFRI E27 806lm dimmable",
    .exposes = s_led1623g12_exposes,
    .expose_count = 1,
    .from_zigbee = s_led1623g12_fz,
    .from_zigbee_count = 2,
    .to_zigbee = s_led1623g12_tz,
    .to_zigbee_count = 2,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * E1524/E1810 - TRADFRI Remote 5-button
 * ============================================================================ */

/* TODO: Add fz_action converter for button press events (clusters 0x0006, 0x0008, 0x0005) */
static const zb_expose_t s_e1524_exposes[] = {
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

static const zb_from_zigbee_t s_e1524_fz[] = {
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
};

static const zb_converter_def_t s_def_e1524 = {
    .manufacturer = "IKEA of Sweden",
    .model = "E1524/E1810",
    .vendor = "IKEA",
    .description = "TRADFRI remote 5-button",
    .exposes = s_e1524_exposes,
    .expose_count = 1,
    .from_zigbee = s_e1524_fz,
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
 * E1603/E1702 - TRADFRI Control Outlet
 * ============================================================================ */

static const zb_expose_t s_e1603_exposes[] = {
    {
        .type = ZB_EXPOSE_SWITCH,
        .name = NULL,
        .endpoint = 0,
        .features = 0,
        .device_class = "outlet",
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_e1603_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .json_key = "state", .convert = fz_on_off },
};

static const zb_to_zigbee_t s_e1603_tz[] = {
    { .json_key = "state", .cluster_id = 0x0006, .convert = tz_on_off },
};

static const zb_converter_def_t s_def_e1603 = {
    .manufacturer = "IKEA of Sweden",
    .model = "E1603/E1702",
    .vendor = "IKEA",
    .description = "TRADFRI control outlet",
    .exposes = s_e1603_exposes,
    .expose_count = 1,
    .from_zigbee = s_e1603_fz,
    .from_zigbee_count = 1,
    .to_zigbee = s_e1603_tz,
    .to_zigbee_count = 1,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Registration
 * ============================================================================ */

void conv_ikea_register(void)
{
    zb_converter_register(&s_def_led1545g12);
    zb_converter_register(&s_def_led1623g12);
    zb_converter_register(&s_def_e1524);
    zb_converter_register(&s_def_e1603);
}
