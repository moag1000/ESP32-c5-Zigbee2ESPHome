/**
 * @file zb_converter_std_electrical.c
 * @brief Standard Electrical & Diagnostics Converter Functions
 *
 * Provides electrical measurement and diagnostics fromZigbee converters:
 * - fz_electrical_frequency: AC frequency from Electrical Measurement cluster
 * - fz_diagnostics_rssi: Last message RSSI from Diagnostics cluster
 * - fz_diagnostics_lqi: Last message LQI from Diagnostics cluster
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_converter_std.h"
#include "zb_converter.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "core/gateway_timeouts.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "CONV_ELEC";

/* ============================================================================
 * fromZigbee (fz_*) Converters
 * ============================================================================ */

/**
 * @brief AC Frequency from Electrical Measurement cluster (0x0B04)
 *
 * Reads AC frequency as uint16, divides by 10 to produce Hz.
 * ZCL Electrical Measurement attribute ACFrequency (0x0300) is in units of
 * 0.1 Hz.
 *
 * Output: frequency in Hz (double) to the specified JSON key.
 */
esp_err_t fz_electrical_frequency(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw_val;
    memcpy(&raw_val, raw, sizeof(raw_val));
    double frequency_hz = (double)raw_val / 10.0;
    cJSON_AddNumberToObject(json, key, frequency_hz);

    ESP_LOGD(TAG, "AC frequency: raw=%" PRIu16 " -> %.1f Hz", raw_val, frequency_hz);
    return ESP_OK;
}

/**
 * @brief Last Message RSSI from Diagnostics cluster (0x0B05)
 *
 * Reads attribute 0x011D (LastMessageRSSI) as int8.
 * Output: RSSI value in dBm to the specified JSON key.
 */
esp_err_t fz_diagnostics_rssi(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    int8_t rssi = *(const int8_t *)raw;
    cJSON_AddNumberToObject(json, key, rssi);

    ESP_LOGD(TAG, "Diagnostics RSSI: %d dBm", rssi);
    return ESP_OK;
}

/**
 * @brief Last Message LQI from Diagnostics cluster (0x0B05)
 *
 * Reads attribute 0x011C (LastMessageLQI) as uint8 (0-255).
 * Output: LQI value to the specified JSON key.
 */
esp_err_t fz_diagnostics_lqi(const void *raw, size_t len, cJSON *json, const char *key)
{
    if (raw == NULL || json == NULL || key == NULL || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t lqi = *(const uint8_t *)raw;
    cJSON_AddNumberToObject(json, key, lqi);

    ESP_LOGD(TAG, "Diagnostics LQI: %u", lqi);
    return ESP_OK;
}
