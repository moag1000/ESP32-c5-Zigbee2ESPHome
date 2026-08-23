/**
 * @file zb_lumi.h
 * @brief Parser for Aqara/Xiaomi's proprietary 0xFF01 attribute
 *
 * Aqara devices do not implement genPowerCfg. They put battery voltage — and
 * a handful of other readings — into a manufacturer-specific attribute on the
 * Basic cluster instead, as a tag/type/value sequence inside a string.
 *
 * That is why the vibration sensor in this network shows no battery while the
 * Fingerbot next to it shows 66 %: nothing was reading 0xFF01.
 *
 * The payload is a run of entries, each:
 *
 *     [tag:1][zcl type:1][value: width of that type]
 *
 * Known tags, of which only the first two are acted on here:
 *
 *     0x01  battery voltage in mV      (uint16)
 *     0x03  device temperature in °C   (int8)
 *     0x05  power outage count, off by one (uint8/16/32)
 *     0x0A  parent short address       (uint16)
 *     0x64+ per-device payloads whose meaning depends on the model
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_LUMI_H
#define ZB_LUMI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Manufacturer-specific attribute on the Basic cluster */
#define ZB_LUMI_ATTR_SPECIAL        0xFF01

/** @brief Voltage a full CR2032 reports, in mV */
#define ZB_LUMI_BATTERY_FULL_MV     3000

/** @brief Voltage at which these devices stop working, in mV */
#define ZB_LUMI_BATTERY_EMPTY_MV    2500

/**
 * @brief What a 0xFF01 payload yielded
 *
 * Each `has_*` says whether the corresponding tag was present. A device sends
 * whichever subset it feels like, so absence is normal and not an error.
 */
typedef struct {
    bool     has_voltage;       /**< Tag 0x01 was present */
    uint16_t voltage_mv;        /**< Battery voltage in mV */
    bool     has_temperature;   /**< Tag 0x03 was present */
    int8_t   temperature_c;     /**< Device temperature in °C */
    bool     has_power_outages; /**< Tag 0x05 was present */
    uint32_t power_outages;     /**< Times the device lost power */
    bool     has_parent;        /**< Tag 0x0A was present */
    uint16_t parent_addr;       /**< Short address of the parent router */
    uint8_t  tags_seen;         /**< How many entries were decoded */
} zb_lumi_attrs_t;

/**
 * @brief Parse a 0xFF01 payload
 *
 * Stops at the first entry it cannot make sense of and keeps everything read
 * up to that point — a payload that ends mid-entry, or carries a type this
 * parser does not know, still yields the voltage if the voltage came first.
 *
 * @param[in]  data Payload, without the ZCL string length byte
 * @param[in]  len  Payload length in bytes
 * @param[out] out  Result, zeroed by this function before use
 * @return ESP_OK if at least one entry was decoded
 * @return ESP_ERR_INVALID_ARG on NULL arguments
 * @return ESP_ERR_NOT_FOUND if nothing could be decoded
 */
esp_err_t zb_lumi_parse_special(const uint8_t *data, size_t len, zb_lumi_attrs_t *out);

/**
 * @brief Convert a CR2032 voltage to a percentage
 *
 * Linear between ZB_LUMI_BATTERY_EMPTY_MV and ZB_LUMI_BATTERY_FULL_MV and
 * clamped at both ends, which is what zigbee2mqtt does for these devices. A
 * coin cell's real discharge curve is flatter than this in the middle, so the
 * number is an indication rather than a measurement — but it is the same
 * indication every other gateway shows, and a battery sensor that disagrees
 * with the rest of the world is worse than one that is roughly right.
 *
 * @param voltage_mv Battery voltage in mV
 * @return Percentage from 0 to 100
 */
uint8_t zb_lumi_voltage_to_percent(uint16_t voltage_mv);

/**
 * @brief Parse a 0xFF01 attribute value, length byte and all
 *
 * The attribute arrives as a ZCL string: byte 0 is the length, and the
 * tag/type/value run starts after it. Every caller was stripping that byte
 * itself, and one of them — the converter's fz_xiaomi_ff01() — did not: it read
 * the length as the first tag, which put every following read at the wrong
 * offset. It then returned success having decoded nothing, so the report
 * counted as handled and an Aqara sensor's battery stayed 'unknown' with
 * nothing logged anywhere.
 *
 * The length byte is the device's claim about its own payload, so it is clamped
 * to what actually arrived rather than trusted.
 *
 * @param[in]  raw Attribute value as received, including the length byte
 * @param[in]  len Size of that buffer
 * @param[out] out Result, zeroed by this function before use
 * @return ESP_OK if at least one entry was decoded
 * @return ESP_ERR_INVALID_ARG on NULL arguments or a buffer below two bytes
 * @return ESP_ERR_NOT_FOUND if nothing could be decoded
 */
esp_err_t zb_lumi_parse_attribute(const void *raw, size_t len, zb_lumi_attrs_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZB_LUMI_H */
