/**
 * @file conv_tuya_fingerbot.c
 * @brief Tuya Fingerbot Converter Definition
 *
 * Full converter DB entry for Adaprox Fingerbot / Fingerbot Plus with
 * z2m-parity exposes. Delegates DP handling to the existing Tuya driver
 * vtable (tuya_fingerbot.c).
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/converter/zb_converter.h"
#include "zigbee/converter/zb_converter_std.h"
#include "zigbee/tuya/tuya_fingerbot.h"
#include "zigbee/zb_tuya.h"
#include "esp_log.h"
#include <string.h>

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* ============================================================================
 * Tuya DP Map — maps DP IDs to semantic property names for fz_tuya_dp
 * Without this, fz_tuya_dp outputs generic "dp_1", "dp_101" keys that
 * don't match the expose property names, breaking state propagation.
 * ============================================================================ */

static const char *const s_mode_enum_names[] = { "push", "switch", "program" };
static const uint8_t s_mode_enum_values[] = { 0, 1, 2 };

static const tuya_dp_entry_t s_fingerbot_dp_map[] = {
    { .dp_id = 1,   .dp_type = TUYA_DP_TYPE_BOOL,  .key = "state" },
    { .dp_id = 101, .dp_type = TUYA_DP_TYPE_ENUM,  .key = "mode",
      .enum_names = (const char **)s_mode_enum_names, .enum_values = s_mode_enum_values,
      .enum_count = 3 },
    { .dp_id = 102, .dp_type = TUYA_DP_TYPE_VALUE, .key = "down_movement" },
    /* DP 103 carries seconds directly — no scaling. The 0.1 factor that used
     * to sit here made Home Assistant show a tenth of the real hold time and
     * send ten times what was asked for: a requested 1s held the switch for
     * 10s. Confirmed against the device's own quirk
     * (data/converters_zhaquirks/tz3210_dse8ogfy.json, DP 103 plain int) and
     * zigbee2mqtt, which writes the value raw. */
    { .dp_id = 103, .dp_type = TUYA_DP_TYPE_VALUE, .key = "sustain_time" },
    { .dp_id = 104, .dp_type = TUYA_DP_TYPE_BOOL,  .key = "reverse" },
    { .dp_id = 105, .dp_type = TUYA_DP_TYPE_VALUE, .key = "battery" },
    { .dp_id = 106, .dp_type = TUYA_DP_TYPE_VALUE, .key = "up_movement" },
    { .dp_id = 107, .dp_type = TUYA_DP_TYPE_BOOL,  .key = "touch_control" },
    { .dp_id = 108, .dp_type = TUYA_DP_TYPE_BOOL,  .key = "click_control" },
    { .dp_id = 109, .dp_type = TUYA_DP_TYPE_RAW,   .key = "program" },
    { .dp_id = 111, .dp_type = TUYA_DP_TYPE_VALUE, .key = "click_count" },
    { .dp_id = 117, .dp_type = TUYA_DP_TYPE_BOOL,  .key = "repeat_forever" },
    { .dp_id = 121, .dp_type = TUYA_DP_TYPE_BOOL,  .key = "program_enable" },
};

/* ============================================================================
 * Exposes — 14 entities matching z2m definitions
 * ============================================================================ */

static const char *const s_mode_options[] = { "push", "switch", "program" };

static const zb_expose_t s_fingerbot_exposes[] = {
    /* Push action button — triggers DP 1 ON command (primary UX for push/program mode) */
    {
        .type = ZB_EXPOSE_BUTTON,
        .name = "Push",
        .property = "state",
        .access = EA_SET,
        .icon = "mdi:gesture-tap-button",
        .description = "Trigger push action",
    },

    /* State switch — DP 1 (stateful toggle for switch mode, shows current state) */
    {
        .type = ZB_EXPOSE_SWITCH,
        .name = NULL,
        .property = "state",
        .access = EA_STATE_SET,
        .description = "On/Off state toggle",
    },

    /* Mode select — DP 101 */
    {
        .type = ZB_EXPOSE_SELECT,
        .name = "Mode",
        .category = "config",
        .property = "mode",
        .access = EA_STATE_SET,
        .icon = "mdi:gesture-tap-button",
        .description = "Operating mode",
        .ext.select = { .values = (const char **)s_mode_options, .count = 3 },
    },

    /* Down Movement — DP 102 */
    {
        .type = ZB_EXPOSE_NUMBER,
        .name = "Down Movement",
        .category = "config",
        .property = "down_movement",
        .access = EA_STATE_SET,
        .unit = "%",
        .icon = "mdi:arrow-down-bold",
        .description = "How far the arm pushes down",
        .ext.numeric = { .min = 51, .max = 100, .step = 1 },
    },

    /* Up Movement — DP 106 */
    {
        .type = ZB_EXPOSE_NUMBER,
        .name = "Up Movement",
        .category = "config",
        .property = "up_movement",
        .access = EA_STATE_SET,
        .unit = "%",
        .icon = "mdi:arrow-up-bold",
        .description = "How far the arm returns up",
        .ext.numeric = { .min = 0, .max = 50, .step = 1 },
    },

    /* Sustain Time — DP 103 */
    {
        .type = ZB_EXPOSE_NUMBER,
        .name = "Sustain Time",
        .category = "config",
        .property = "sustain_time",
        .access = EA_STATE_SET,
        .unit = "s",
        .icon = "mdi:timer-outline",
        .description = "Hold time at bottom position",
        /* zigbee2mqtt caps this at 10s; 25 fed the device values it does not
         * accept. */
        .ext.numeric = { .min = 0, .max = 10, .step = 1 },
    },

    /* Reverse — DP 104 */
    {
        .type = ZB_EXPOSE_SWITCH,
        .name = "Reverse",
        .category = "config",
        .property = "reverse",
        .access = EA_STATE_SET,
        .icon = "mdi:swap-vertical",
        .description = "Invert arm direction",
    },

    /* Touch Control — DP 107 */
    {
        .type = ZB_EXPOSE_SWITCH,
        .name = "Touch Control",
        .category = "config",
        .property = "touch_control",
        .access = EA_STATE_SET,
        .icon = "mdi:gesture-tap",
    },

    /* Click Control — DP 108 (Plus only) */
    {
        .type = ZB_EXPOSE_SWITCH,
        .name = "Click Control",
        .category = "config",
        .property = "click_control",
        .access = EA_STATE_SET,
        .icon = "mdi:mouse",
        .description = "Physical button (Plus only)",
    },

    /* Program Enable — DP 121 */
    {
        .type = ZB_EXPOSE_SWITCH,
        .name = "Program Enable",
        .category = "config",
        .property = "program_enable",
        .access = EA_STATE_SET,
        .icon = "mdi:play-circle",
    },

    /* Repeat Forever — DP 117 */
    {
        .type = ZB_EXPOSE_SWITCH,
        .name = "Repeat Forever",
        .category = "config",
        .property = "repeat_forever",
        .access = EA_STATE_SET,
        .icon = "mdi:repeat",
    },

    /* Battery — DP 105 */
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "Battery",
        .category = "diagnostic",
        .property = "battery",
        .access = EA_STATE,
        .device_class = "battery",
        .unit = "%",
        .state_class = "measurement",
    },

    /* Click Count — DP 111 */
    {
        .type = ZB_EXPOSE_SENSOR,
        .name = "Click Count",
        .category = "diagnostic",
        .property = "click_count",
        .access = EA_STATE,
        .state_class = "total_increasing",
        .icon = "mdi:counter",
    },

    /* Program text — DP 109 */
    {
        .type = ZB_EXPOSE_TEXT,
        .name = "Program",
        .property = "program",
        .access = EA_STATE_SET,
        .icon = "mdi:script-text-outline",
        .description = "Action sequence (pos/delay;pos/delay;...)",
    },
};

/* ============================================================================
 * From/To Zigbee Converters
 * ============================================================================ */

static const zb_from_zigbee_t s_fingerbot_fz[] = {
    { .cluster_id = 0xEF00, .attr_id = 0xFFFF, .json_key = "tuya_dp",
      .convert = fz_tuya_dp },
};

static const zb_to_zigbee_t s_fingerbot_tz[] = {
    { .json_key = "state",          .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "mode",           .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "down_movement",  .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "up_movement",    .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "sustain_time",   .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "reverse",        .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "touch_control",  .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "click_control",  .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "program_enable", .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "repeat_forever", .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "program",        .cluster_id = 0xEF00, .convert = tz_tuya_command },
    { .json_key = "action",         .cluster_id = 0xEF00, .convert = tz_tuya_command },
};

/* ============================================================================
 * Converter Definitions (two variants)
 * ============================================================================ */

static const zb_converter_def_t s_def_fingerbot = {
    .manufacturer = "_TZ3210_dse8ogfy",
    .model = "TS0001",
    .vendor = "Adaprox",
    .description = "Fingerbot",
    .exposes = s_fingerbot_exposes,
    .expose_count = COUNTOF(s_fingerbot_exposes),
    .from_zigbee = s_fingerbot_fz,
    .from_zigbee_count = COUNTOF(s_fingerbot_fz),
    .to_zigbee = s_fingerbot_tz,
    .to_zigbee_count = COUNTOF(s_fingerbot_tz),
    .quirk_flags = ZB_QUIRK_TUYA_DEVICE,
    .tuya_driver = &tuya_fingerbot_driver,
    .tuya_dp_map = s_fingerbot_dp_map,
    .tuya_dp_count = COUNTOF(s_fingerbot_dp_map),
};

static const zb_converter_def_t s_def_fingerbot_plus = {
    .manufacturer = "_TZ3210_j4pdtz9v",
    .model = "TS0001",
    .vendor = "Adaprox",
    .description = "Fingerbot Plus",
    .exposes = s_fingerbot_exposes,
    .expose_count = COUNTOF(s_fingerbot_exposes),
    .from_zigbee = s_fingerbot_fz,
    .from_zigbee_count = COUNTOF(s_fingerbot_fz),
    .to_zigbee = s_fingerbot_tz,
    .to_zigbee_count = COUNTOF(s_fingerbot_tz),
    .quirk_flags = ZB_QUIRK_TUYA_DEVICE,
    .tuya_driver = &tuya_fingerbot_driver,
    .tuya_dp_map = s_fingerbot_dp_map,
    .tuya_dp_count = COUNTOF(s_fingerbot_dp_map),
};

/* ============================================================================
 * Registration
 * ============================================================================ */

void conv_tuya_fingerbot_register(void)
{
    zb_converter_register(&s_def_fingerbot);
    zb_converter_register(&s_def_fingerbot_plus);
    ESP_LOGI("CONV_FINGERBOT", "Fingerbot converters registered (2 variants)");
}
