/**
 * @file zb_lumi.c
 * @brief Parser for Aqara/Xiaomi's proprietary 0xFF01 attribute
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zigbee/zb_lumi.h"
#include <string.h>

/* ZCL data types that turn up inside a 0xFF01 payload. The width table below
 * is what lets the parser skip a tag it does not care about without losing its
 * place — a payload is a flat run of entries with no length prefix, so an
 * unknown width means the rest is unreadable. */
#define ZCL_TYPE_BOOL       0x10
#define ZCL_TYPE_U8         0x20
#define ZCL_TYPE_U16        0x21
#define ZCL_TYPE_U24        0x22
#define ZCL_TYPE_U32        0x23
#define ZCL_TYPE_U40        0x24
#define ZCL_TYPE_U48        0x25
#define ZCL_TYPE_S8         0x28
#define ZCL_TYPE_S16        0x29
#define ZCL_TYPE_S32        0x2B
#define ZCL_TYPE_ENUM8      0x30
#define ZCL_TYPE_ENUM16     0x31
#define ZCL_TYPE_SINGLE     0x39

#define ZB_LUMI_TAG_VOLTAGE     0x01
#define ZB_LUMI_TAG_TEMPERATURE 0x03
#define ZB_LUMI_TAG_POWER_OUTAGES 0x05
#define ZB_LUMI_TAG_PARENT      0x0A

/**
 * @brief Width in bytes of a ZCL type, or 0 if this parser does not know it
 */
static size_t zcl_type_width(uint8_t type)
{
    switch (type) {
        case ZCL_TYPE_BOOL:
        case ZCL_TYPE_U8:
        case ZCL_TYPE_S8:
        case ZCL_TYPE_ENUM8:
            return 1;
        case ZCL_TYPE_U16:
        case ZCL_TYPE_S16:
        case ZCL_TYPE_ENUM16:
            return 2;
        case ZCL_TYPE_U24:
            return 3;
        case ZCL_TYPE_U32:
        case ZCL_TYPE_S32:
        case ZCL_TYPE_SINGLE:
            return 4;
        case ZCL_TYPE_U40:
            return 5;
        case ZCL_TYPE_U48:
            return 6;
        default:
            return 0;
    }
}

esp_err_t zb_lumi_parse_special(const uint8_t *data, size_t len, zb_lumi_attrs_t *out)
{
    if (data == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    size_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t tag  = data[pos];
        uint8_t type = data[pos + 1];

        size_t width = zcl_type_width(type);
        if (width == 0) {
            /* An unknown type has an unknown width, so the position of the
             * next entry is unknown too. Everything decoded so far stands;
             * the rest is not guessed at. */
            break;
        }
        if (pos + 2 + width > len) {
            break;  /* payload ends mid-entry */
        }

        const uint8_t *value = &data[pos + 2];

        switch (tag) {
            case ZB_LUMI_TAG_VOLTAGE:
                if (width == 2) {
                    out->voltage_mv = (uint16_t)(value[0] | ((uint16_t)value[1] << 8));
                    out->has_voltage = true;
                }
                break;

            case ZB_LUMI_TAG_TEMPERATURE:
                if (width == 1) {
                    out->temperature_c = (int8_t)value[0];
                    out->has_temperature = true;
                }
                break;

            case ZB_LUMI_TAG_POWER_OUTAGES:
                /* The width varies by device, so read whatever the declared
                 * type says rather than assuming one. The counter is off by
                 * one on the wire — zigbee-herdsman-converters subtracts 1
                 * unconditionally (lumi.ts, case "5"); clamping at 0 keeps a
                 * device that reports 0 from wrapping to 4294967295. */
                if (width >= 1 && width <= 4) {
                    uint32_t raw = 0;
                    for (size_t b = 0; b < width; b++) {
                        raw |= (uint32_t)value[b] << (8 * b);
                    }
                    out->power_outages = (raw > 0) ? (raw - 1) : 0;
                    out->has_power_outages = true;
                }
                break;

            case ZB_LUMI_TAG_PARENT:
                if (width == 2) {
                    out->parent_addr = (uint16_t)(value[0] | ((uint16_t)value[1] << 8));
                    out->has_parent = true;
                }
                break;

            default:
                break;  /* known width, uninteresting tag — skip it */
        }

        out->tags_seen++;
        pos += 2 + width;
    }

    return (out->tags_seen > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

uint8_t zb_lumi_voltage_to_percent(uint16_t voltage_mv)
{
    if (voltage_mv >= ZB_LUMI_BATTERY_FULL_MV) {
        return 100;
    }
    if (voltage_mv <= ZB_LUMI_BATTERY_EMPTY_MV) {
        return 0;
    }

    uint32_t span  = ZB_LUMI_BATTERY_FULL_MV - ZB_LUMI_BATTERY_EMPTY_MV;
    uint32_t above = (uint32_t)voltage_mv - ZB_LUMI_BATTERY_EMPTY_MV;

    return (uint8_t)((above * 100U) / span);
}
