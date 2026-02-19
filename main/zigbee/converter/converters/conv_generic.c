/**
 * @file conv_generic.c
 * @brief Generic Cluster-Based Fallback Converters
 *
 * Provides fallback converter definitions for devices that are not
 * matched by a manufacturer-specific converter. These use only
 * standard ZCL cluster behavior.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/converter/zb_converter.h"
#include "zigbee/converter/zb_converter_std.h"
#include "esp_zigbee_core.h"

/* ============================================================================
 * Generic On/Off Light
 * ============================================================================ */

static const zb_expose_t s_generic_on_off_exposes[] = {
    {
        .type = ZB_EXPOSE_LIGHT,
        .name = NULL,
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_generic_on_off_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .json_key = "state", .convert = fz_on_off },
};

static const zb_to_zigbee_t s_generic_on_off_tz[] = {
    { .json_key = "state", .cluster_id = 0x0006, .convert = tz_on_off },
};

static const zb_converter_def_t s_def_generic_on_off = {
    .manufacturer = NULL,
    .model = NULL,
    .vendor = "Generic",
    .description = "Generic on/off light",
    .exposes = s_generic_on_off_exposes,
    .expose_count = 1,
    .from_zigbee = s_generic_on_off_fz,
    .from_zigbee_count = 1,
    .to_zigbee = s_generic_on_off_tz,
    .to_zigbee_count = 1,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Generic Dimmable Light
 * ============================================================================ */

static const zb_expose_t s_generic_dimmable_exposes[] = {
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

static const zb_from_zigbee_t s_generic_dimmable_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .json_key = "state", .convert = fz_on_off },
    { .cluster_id = 0x0008, .attr_id = 0x0000, .json_key = "brightness", .convert = fz_brightness },
};

static const zb_to_zigbee_t s_generic_dimmable_tz[] = {
    { .json_key = "state", .cluster_id = 0x0006, .convert = tz_on_off },
    { .json_key = "brightness", .cluster_id = 0x0008, .convert = tz_brightness },
};

static const zb_converter_def_t s_def_generic_dimmable = {
    .manufacturer = NULL,
    .model = NULL,
    .vendor = "Generic",
    .description = "Generic dimmable light",
    .exposes = s_generic_dimmable_exposes,
    .expose_count = 1,
    .from_zigbee = s_generic_dimmable_fz,
    .from_zigbee_count = 2,
    .to_zigbee = s_generic_dimmable_tz,
    .to_zigbee_count = 2,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Generic Color Temperature Light
 * ============================================================================ */

static const zb_expose_t s_generic_ct_exposes[] = {
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

static const zb_from_zigbee_t s_generic_ct_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .json_key = "state", .convert = fz_on_off },
    { .cluster_id = 0x0008, .attr_id = 0x0000, .json_key = "brightness", .convert = fz_brightness },
    { .cluster_id = 0x0300, .attr_id = 0x0007, .json_key = "color_temp", .convert = fz_color_temp },
};

static const zb_to_zigbee_t s_generic_ct_tz[] = {
    { .json_key = "state", .cluster_id = 0x0006, .convert = tz_on_off },
    { .json_key = "brightness", .cluster_id = 0x0008, .convert = tz_brightness },
    { .json_key = "color_temp", .cluster_id = 0x0300, .convert = tz_color_temp },
};

static const zb_converter_def_t s_def_generic_ct = {
    .manufacturer = NULL,
    .model = NULL,
    .vendor = "Generic",
    .description = "Generic color temperature light",
    .exposes = s_generic_ct_exposes,
    .expose_count = 1,
    .from_zigbee = s_generic_ct_fz,
    .from_zigbee_count = 3,
    .to_zigbee = s_generic_ct_tz,
    .to_zigbee_count = 3,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Generic Contact Sensor
 * ============================================================================ */

static const zb_expose_t s_generic_contact_exposes[] = {
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

static const zb_from_zigbee_t s_generic_contact_fz[] = {
    { .cluster_id = 0x0500, .attr_id = 0x0002, .json_key = "contact", .convert = fz_ias_zone_status },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
};

static const zb_converter_def_t s_def_generic_contact = {
    .manufacturer = NULL,
    .model = NULL,
    .vendor = "Generic",
    .description = "Generic contact sensor",
    .exposes = s_generic_contact_exposes,
    .expose_count = 2,
    .from_zigbee = s_generic_contact_fz,
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
 * Generic Motion Sensor
 * ============================================================================ */

static const zb_expose_t s_generic_motion_exposes[] = {
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

static const zb_from_zigbee_t s_generic_motion_fz[] = {
    { .cluster_id = 0x0406, .attr_id = 0x0000, .json_key = "occupancy", .convert = fz_occupancy },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
};

static const zb_converter_def_t s_def_generic_motion = {
    .manufacturer = NULL,
    .model = NULL,
    .vendor = "Generic",
    .description = "Generic motion sensor",
    .exposes = s_generic_motion_exposes,
    .expose_count = 2,
    .from_zigbee = s_generic_motion_fz,
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
 * Generic Temperature/Humidity Sensor
 * ============================================================================ */

static const zb_expose_t s_generic_temp_hum_exposes[] = {
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "temperature",
        .endpoint = 0,
        .features = 0,
        .device_class = "temperature",
        .unit = "\xC2\xB0""C",  /* degree sign + C (UTF-8) */
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

static const zb_from_zigbee_t s_generic_temp_hum_fz[] = {
    { .cluster_id = 0x0402, .attr_id = 0x0000, .json_key = "temperature", .convert = fz_temperature },
    { .cluster_id = 0x0405, .attr_id = 0x0000, .json_key = "humidity", .convert = fz_humidity },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
};

static const zb_converter_def_t s_def_generic_temp_hum = {
    .manufacturer = NULL,
    .model = NULL,
    .vendor = "Generic",
    .description = "Generic temperature/humidity sensor",
    .exposes = s_generic_temp_hum_exposes,
    .expose_count = 3,
    .from_zigbee = s_generic_temp_hum_fz,
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
 * Generic Color XY Light
 * ============================================================================ */

static const zb_expose_t s_generic_color_xy_exposes[] = {
    {
        .type = ZB_EXPOSE_LIGHT,
        .name = NULL,
        .endpoint = 0,
        .features = ZB_FEATURE_BRIGHTNESS | ZB_FEATURE_COLOR_XY | ZB_FEATURE_TRANSITION,
        .device_class = NULL,
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_generic_color_xy_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .json_key = "state", .convert = fz_on_off },
    { .cluster_id = 0x0008, .attr_id = 0x0000, .json_key = "brightness", .convert = fz_brightness },
    { .cluster_id = 0x0300, .attr_id = 0x0003, .json_key = "color", .convert = fz_color_xy },
    { .cluster_id = 0x0300, .attr_id = 0x0004, .json_key = "color", .convert = fz_color_xy },
};

static const zb_to_zigbee_t s_generic_color_xy_tz[] = {
    { .json_key = "state", .cluster_id = 0x0006, .convert = tz_on_off },
    { .json_key = "brightness", .cluster_id = 0x0008, .convert = tz_brightness },
    { .json_key = "color", .cluster_id = 0x0300, .convert = tz_color_xy },
};

static const zb_converter_def_t s_def_generic_color_xy = {
    .manufacturer = NULL,
    .model = NULL,
    .vendor = "Generic",
    .description = "Generic color XY light",
    .exposes = s_generic_color_xy_exposes,
    .expose_count = 1,
    .from_zigbee = s_generic_color_xy_fz,
    .from_zigbee_count = 4,
    .to_zigbee = s_generic_color_xy_tz,
    .to_zigbee_count = 3,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Generic Door Lock
 * ============================================================================ */

static const zb_expose_t s_generic_lock_exposes[] = {
    {
        .type = ZB_EXPOSE_LOCK,
        .name = NULL,
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_generic_lock_fz[] = {
    { .cluster_id = 0x0101, .attr_id = 0x0000, .json_key = "state", .convert = fz_lock_state },
};

static const zb_to_zigbee_t s_generic_lock_tz[] = {
    { .json_key = "state", .cluster_id = 0x0101, .convert = tz_lock_command },
};

static const zb_converter_def_t s_def_generic_lock = {
    .manufacturer = NULL,
    .model = NULL,
    .vendor = "Generic",
    .description = "Generic door lock",
    .exposes = s_generic_lock_exposes,
    .expose_count = 1,
    .from_zigbee = s_generic_lock_fz,
    .from_zigbee_count = 1,
    .to_zigbee = s_generic_lock_tz,
    .to_zigbee_count = 1,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Generic Window Covering
 * ============================================================================ */

static const zb_expose_t s_generic_cover_exposes[] = {
    {
        .type = ZB_EXPOSE_COVER,
        .name = NULL,
        .endpoint = 0,
        .features = ZB_FEATURE_POSITION,
        .device_class = NULL,
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_generic_cover_fz[] = {
    { .cluster_id = 0x0102, .attr_id = 0x0008, .json_key = "position", .convert = fz_cover_position },
};

static const zb_to_zigbee_t s_generic_cover_tz[] = {
    { .json_key = "position", .cluster_id = 0x0102, .convert = tz_cover_position },
    { .json_key = "state", .cluster_id = 0x0102, .convert = tz_cover_command },
};

static const zb_converter_def_t s_def_generic_cover = {
    .manufacturer = NULL,
    .model = NULL,
    .vendor = "Generic",
    .description = "Generic window covering",
    .exposes = s_generic_cover_exposes,
    .expose_count = 1,
    .from_zigbee = s_generic_cover_fz,
    .from_zigbee_count = 1,
    .to_zigbee = s_generic_cover_tz,
    .to_zigbee_count = 2,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Capability-based Generic Converter Lookup
 * ============================================================================ */

#include "core/device/unified_device.h"

const zb_converter_def_t *conv_generic_for_capabilities(uint32_t caps)
{
    /* Check most specific first, then more general */
    if (caps & DEV_CAP_LOCK)
        return &s_def_generic_lock;
    if (caps & DEV_CAP_COVER)
        return &s_def_generic_cover;
    if (caps & DEV_CAP_COLOR_XY)
        return &s_def_generic_color_xy;
    if (caps & DEV_CAP_COLOR_TEMP)
        return &s_def_generic_ct;
    if (caps & DEV_CAP_BRIGHTNESS)
        return &s_def_generic_dimmable;
    if (caps & DEV_CAP_ON_OFF)
        return &s_def_generic_on_off;
    /* Sensors have no to_zigbee, but from_zigbee for reports */
    if (caps & DEV_CAP_MOTION)
        return &s_def_generic_motion;
    if (caps & DEV_CAP_CONTACT)
        return &s_def_generic_contact;
    if (caps & DEV_CAP_TEMPERATURE)
        return &s_def_generic_temp_hum;
    return NULL;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

void conv_generic_register(void)
{
    zb_converter_register(&s_def_generic_on_off);
    zb_converter_register(&s_def_generic_dimmable);
    zb_converter_register(&s_def_generic_ct);
    zb_converter_register(&s_def_generic_color_xy);
    zb_converter_register(&s_def_generic_lock);
    zb_converter_register(&s_def_generic_cover);
    zb_converter_register(&s_def_generic_contact);
    zb_converter_register(&s_def_generic_motion);
    zb_converter_register(&s_def_generic_temp_hum);
}
