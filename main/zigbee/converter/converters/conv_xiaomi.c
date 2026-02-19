/**
 * @file conv_xiaomi.c
 * @brief Xiaomi/Aqara Converter Definitions
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/converter/zb_converter.h"
#include "zigbee/converter/zb_converter_std.h"
#include "esp_zigbee_core.h"

/* ============================================================================
 * MCCGQ11LM - Aqara Door/Window Sensor
 * ============================================================================ */

static const zb_expose_t s_mccgq11lm_exposes[] = {
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
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "voltage",
        .endpoint = 0,
        .features = 0,
        .device_class = "voltage",
        .unit = "V",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "device_temperature",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = "\xC2\xB0""C",
        .state_class = "measurement",
    },
};

static const zb_from_zigbee_t s_mccgq11lm_fz[] = {
    { .cluster_id = 0x0500, .attr_id = 0x0002, .json_key = "contact", .convert = fz_ias_zone_status },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
    { .cluster_id = 0x0000, .attr_id = 0xFF01, .json_key = "battery", .convert = fz_xiaomi_ff01 },
};

static const zb_converter_def_t s_def_mccgq11lm = {
    .manufacturer = "LUMI",
    .model = "lumi.sensor_magnet.aq2",
    .vendor = "Aqara",
    .description = "Aqara door & window contact sensor",
    .exposes = s_mccgq11lm_exposes,
    .expose_count = 4,
    .from_zigbee = s_mccgq11lm_fz,
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
 * WSDCGQ11LM - Aqara Temperature/Humidity Sensor
 * ============================================================================ */

static const zb_expose_t s_wsdcgq11lm_exposes[] = {
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
        .name = "pressure",
        .endpoint = 0,
        .features = 0,
        .device_class = "pressure",
        .unit = "hPa",
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
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "voltage",
        .endpoint = 0,
        .features = 0,
        .device_class = "voltage",
        .unit = "V",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "device_temperature",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = "\xC2\xB0""C",
        .state_class = "measurement",
    },
};

static const zb_from_zigbee_t s_wsdcgq11lm_fz[] = {
    { .cluster_id = 0x0402, .attr_id = 0x0000, .json_key = "temperature", .convert = fz_temperature },
    { .cluster_id = 0x0405, .attr_id = 0x0000, .json_key = "humidity", .convert = fz_humidity },
    { .cluster_id = 0x0403, .attr_id = 0x0000, .json_key = "pressure", .convert = fz_pressure },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
    { .cluster_id = 0x0000, .attr_id = 0xFF01, .json_key = "battery", .convert = fz_xiaomi_ff01 },
};

static const zb_converter_def_t s_def_wsdcgq11lm = {
    .manufacturer = "LUMI",
    .model = "lumi.weather",
    .vendor = "Aqara",
    .description = "Aqara temperature, humidity and pressure sensor",
    .exposes = s_wsdcgq11lm_exposes,
    .expose_count = 6,
    .from_zigbee = s_wsdcgq11lm_fz,
    .from_zigbee_count = 5,
    .to_zigbee = NULL,
    .to_zigbee_count = 0,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * RTCGQ11LM - Aqara Motion Sensor
 * ============================================================================ */

static const zb_expose_t s_rtcgq11lm_exposes[] = {
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
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "voltage",
        .endpoint = 0,
        .features = 0,
        .device_class = "voltage",
        .unit = "V",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "device_temperature",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = "\xC2\xB0""C",
        .state_class = "measurement",
    },
};

static const zb_from_zigbee_t s_rtcgq11lm_fz[] = {
    { .cluster_id = 0x0406, .attr_id = 0x0000, .json_key = "occupancy", .convert = fz_occupancy },
    { .cluster_id = 0x0400, .attr_id = 0x0000, .json_key = "illuminance", .convert = fz_illuminance },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery", .convert = fz_battery },
    { .cluster_id = 0x0000, .attr_id = 0xFF01, .json_key = "battery", .convert = fz_xiaomi_ff01 },
};

static const zb_converter_def_t s_def_rtcgq11lm = {
    .manufacturer = "LUMI",
    .model = "lumi.sensor_motion.aq2",
    .vendor = "Aqara",
    .description = "Aqara motion sensor",
    .exposes = s_rtcgq11lm_exposes,
    .expose_count = 5,
    .from_zigbee = s_rtcgq11lm_fz,
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
 * DJT11LM - Aqara Vibration Sensor
 *
 * All vibration data arrives on cluster 0x0101 (Door Lock) via proprietary
 * attribute reports:
 *   0x0055 = status type (1=vibration, 2=tilt, 3=drop)
 *   0x0503 = rotation angle in degrees (uint16)
 *   0x0505 = vibration strength (uint32, byte-swapped)
 *   0x0508 = orientation XYZ accelerometer (uint48 -> angles)
 * Battery on cluster 0x0001 attr 0x0021 as usual.
 * ============================================================================ */

static const zb_expose_t s_djt11lm_exposes[] = {
    {
        .type = ZB_EXPOSE_BINARY_SENSOR,
        .name = NULL,
        .endpoint = 0,
        .features = 0,
        .device_class = "vibration",
        .unit = NULL,
        .state_class = NULL,
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "action",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = NULL,
        .state_class = NULL,
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "angle",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = "\xC2\xB0",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "strength",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = NULL,
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "angle_x",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = "\xC2\xB0",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "angle_y",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = "\xC2\xB0",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "angle_z",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = "\xC2\xB0",
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
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "voltage",
        .endpoint = 0,
        .features = 0,
        .device_class = "voltage",
        .unit = "V",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "device_temperature",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = "\xC2\xB0""C",
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "x_axis",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = NULL,
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "y_axis",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = NULL,
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "z_axis",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = NULL,
        .state_class = "measurement",
    },
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "power_outage_count",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = NULL,
        .state_class = "total_increasing",
    },
    {
        .type = ZB_EXPOSE_SELECT,
        .name = "sensitivity",
        .endpoint = 0,
        .features = 0,
        .device_class = NULL,
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_djt11lm_fz[] = {
    { .cluster_id = 0x0101, .attr_id = 0x0055, .json_key = "action",   .convert = fz_vibration_action },
    { .cluster_id = 0x0101, .attr_id = 0x0503, .json_key = "angle",    .convert = fz_uint16 },
    { .cluster_id = 0x0101, .attr_id = 0x0505, .json_key = "strength", .convert = fz_vibration_strength },
    { .cluster_id = 0x0101, .attr_id = 0x0508, .json_key = "angle_x",  .convert = fz_vibration_angle },
    { .cluster_id = 0x0001, .attr_id = 0x0021, .json_key = "battery",  .convert = fz_battery },
    { .cluster_id = 0x0000, .attr_id = 0xFF01, .json_key = "battery",  .convert = fz_xiaomi_ff01 },
};

static const zb_to_zigbee_t s_djt11lm_tz[] = {
    { .json_key = "sensitivity", .endpoint = 1, .cluster_id = 0x0000, .convert = tz_xiaomi_sensitivity },
};

static const zb_converter_def_t s_def_djt11lm = {
    .manufacturer = "LUMI",
    .model = "lumi.vibration.aq1",
    .vendor = "Aqara",
    .description = "Aqara vibration sensor",
    .exposes = s_djt11lm_exposes,
    .expose_count = 15,
    .from_zigbee = s_djt11lm_fz,
    .from_zigbee_count = 6,
    .to_zigbee = s_djt11lm_tz,
    .to_zigbee_count = 1,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * QBKG03LM - Aqara Dual Switch (multi-endpoint)
 * ============================================================================ */

static const zb_expose_t s_qbkg03lm_exposes[] = {
    {
        .type = ZB_EXPOSE_SWITCH,
        .name = "left",
        .endpoint = 1,
        .features = 0,
        .device_class = "switch",
        .unit = NULL,
        .state_class = NULL,
    },
    {
        .type = ZB_EXPOSE_SWITCH,
        .name = "right",
        .endpoint = 2,
        .features = 0,
        .device_class = "switch",
        .unit = NULL,
        .state_class = NULL,
    },
};

static const zb_from_zigbee_t s_qbkg03lm_fz[] = {
    { .cluster_id = 0x0006, .attr_id = 0x0000, .endpoint = 1, .json_key = "state_left",  .convert = fz_on_off },
    { .cluster_id = 0x0006, .attr_id = 0x0000, .endpoint = 2, .json_key = "state_right", .convert = fz_on_off },
};

static const zb_to_zigbee_t s_qbkg03lm_tz[] = {
    { .json_key = "state_left",  .endpoint = 1, .cluster_id = 0x0006, .convert = tz_on_off },
    { .json_key = "state_right", .endpoint = 2, .cluster_id = 0x0006, .convert = tz_on_off },
};

static const zb_converter_def_t s_def_qbkg03lm = {
    .manufacturer = "LUMI",
    .model = "lumi.ctrl_neutral2",
    .vendor = "Aqara",
    .description = "Aqara double key wired wall switch",
    .exposes = s_qbkg03lm_exposes,
    .expose_count = 2,
    .from_zigbee = s_qbkg03lm_fz,
    .from_zigbee_count = 2,
    .to_zigbee = s_qbkg03lm_tz,
    .to_zigbee_count = 2,
    .reporting = NULL,
    .reporting_count = 0,
    .quirk_flags = ZB_QUIRK_NONE,
    .init = NULL,
    .tuya_driver = NULL,
};

/* ============================================================================
 * Registration
 * ============================================================================ */

void conv_xiaomi_register(void)
{
    zb_converter_register(&s_def_mccgq11lm);
    zb_converter_register(&s_def_wsdcgq11lm);
    zb_converter_register(&s_def_rtcgq11lm);
    zb_converter_register(&s_def_djt11lm);
    zb_converter_register(&s_def_qbkg03lm);
}
