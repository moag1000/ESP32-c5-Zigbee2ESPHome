/**
 * @file esphome_adapter_cap_metadata.h
 * @brief Capability Metadata Table for ESPHome Entity Registration
 *
 * Provides a single source of truth for DEV_CAP_* → entity metadata mapping.
 * Replaces three separate switch/case blocks (name, key, sensor config)
 * with one const table + lookup function.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_ADAPTER_CAP_METADATA_H
#define ESPHOME_ADAPTER_CAP_METADATA_H

#include "core/device/unified_device.h"
#include "esphome/esphome_common.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Metadata for a single device capability
 *
 * Used during entity registration to populate ESPHome entity configs
 * (name suffix, unique_id key, device_class, unit, icon, etc.).
 */
typedef struct {
    device_capability_t cap;
    const char *display_name;           /**< "Temperature", "Humidity", etc. */
    const char *key;                    /**< "temperature", "humidity", etc. */
    esphome_sensor_device_class_t sensor_class;
    esphome_binary_device_class_t binary_class;
    const char *unit;                   /**< "C", "%", "hPa", etc. */
    const char *icon;                   /**< "mdi:thermometer", etc. */
    uint8_t accuracy_decimals;
    esphome_state_class_t state_class;  /**< 0 = use default MEASUREMENT */
    uint8_t entity_category;            /**< 0=none, 2=diagnostic */
} cap_metadata_t;

/**
 * @brief Static const capability metadata table
 *
 * 28 entries covering all DEV_CAP_* values. Called only during registration
 * (not hot path), so linear scan is fine.
 */
static const cap_metadata_t s_cap_metadata[] = {
    /* Sensors */
    { DEV_CAP_TEMPERATURE,  "Temperature",   "temperature",
      ESPHOME_SENSOR_CLASS_TEMPERATURE, 0, "C",    "mdi:thermometer",     1, 0, 0 },
    { DEV_CAP_HUMIDITY,     "Humidity",       "humidity",
      ESPHOME_SENSOR_CLASS_HUMIDITY, 0,    "%",    "mdi:water-percent",   1, 0, 0 },
    { DEV_CAP_PRESSURE,     "Pressure",       "pressure",
      ESPHOME_SENSOR_CLASS_PRESSURE, 0,    "hPa",  "mdi:gauge",           0, 0, 0 },
    { DEV_CAP_BATTERY,      "Battery",        "battery",
      ESPHOME_SENSOR_CLASS_BATTERY, 0,     "%",    "mdi:battery",         0, 0, 2 },
    { DEV_CAP_POWER,        "Power",          "power",
      ESPHOME_SENSOR_CLASS_POWER, 0,       "W",    "mdi:flash",           1, 0, 0 },
    { DEV_CAP_ENERGY,       "Energy",         "energy",
      ESPHOME_SENSOR_CLASS_ENERGY, 0,      "kWh",  "mdi:lightning-bolt",  2,
      ESPHOME_STATE_CLASS_TOTAL_INCREASING, 0 },
    { DEV_CAP_VOLTAGE,      "Voltage",        "voltage",
      ESPHOME_SENSOR_CLASS_VOLTAGE, 0,     "V",    "mdi:sine-wave",       1, 0, 0 },
    { DEV_CAP_CURRENT,      "Current",        "current",
      ESPHOME_SENSOR_CLASS_CURRENT, 0,     "A",    "mdi:current-ac",      2, 0, 0 },
    { DEV_CAP_ILLUMINANCE,  "Illuminance",    "illuminance",
      ESPHOME_SENSOR_CLASS_ILLUMINANCE, 0, "lx",   "mdi:brightness-6",    0, 0, 0 },
    { DEV_CAP_CO2,          "CO2",            "co2",
      ESPHOME_SENSOR_CLASS_CARBON_DIOXIDE, 0, "ppm", "mdi:molecule-co2",  0, 0, 0 },
    { DEV_CAP_VOC,          "VOC",            "voc",
      ESPHOME_SENSOR_CLASS_VOLATILE_ORGANIC_COMPOUNDS, 0, "", "mdi:air-filter", 0, 0, 0 },
    { DEV_CAP_PM25,         "PM2.5",          "pm25",
      ESPHOME_SENSOR_CLASS_PM25, 0,        "ug/m3", "mdi:blur",           0, 0, 0 },
    { DEV_CAP_SOIL_MOISTURE, "Soil Moisture", "soil_moisture",
      ESPHOME_SENSOR_CLASS_MOISTURE, 0,    "%",    "mdi:water",           0, 0, 0 },

    /* Binary sensors */
    { DEV_CAP_MOTION,       "Motion",         "motion",
      0, ESPHOME_BINARY_CLASS_MOTION,      NULL,   "mdi:motion-sensor",   0, 0, 0 },
    { DEV_CAP_CONTACT,      "Contact",        "contact",
      0, ESPHOME_BINARY_CLASS_DOOR,        NULL,   "mdi:door",            0, 0, 0 },
    { DEV_CAP_VIBRATION,    "Vibration",      "vibration",
      0, ESPHOME_BINARY_CLASS_VIBRATION,   NULL,   "mdi:vibrate",         0, 0, 0 },
    { DEV_CAP_WATER_LEAK,   "Water Leak",     "water_leak",
      0, ESPHOME_BINARY_CLASS_MOISTURE,    NULL,   "mdi:water-alert",     0, 0, 0 },
    { DEV_CAP_SMOKE,        "Smoke",          "smoke",
      0, ESPHOME_BINARY_CLASS_SMOKE,       NULL,   "mdi:smoke-detector",  0, 0, 0 },
    { DEV_CAP_CO,           "CO",             "co",
      0, ESPHOME_BINARY_CLASS_GAS,         NULL,   "mdi:molecule-co",     0, 0, 0 },
    { DEV_CAP_TAMPER,       "Tamper",         "tamper",
      0, ESPHOME_BINARY_CLASS_TAMPER,      NULL,   "mdi:shield-alert",    0, 0, 2 },
    { DEV_CAP_BATTERY_LOW,  "Battery Low",    "battery_low",
      0, ESPHOME_BINARY_CLASS_BATTERY_LOW, NULL,   "mdi:battery-alert",   0, 0, 2 },

    /* Control entities (no sensor/binary metadata, just name/key) */
    { DEV_CAP_ON_OFF,       "Switch",         "switch",
      0, 0, NULL, "mdi:power",          0, 0, 0 },
    { DEV_CAP_BRIGHTNESS,   "Light",          "light",
      0, 0, NULL, "mdi:lightbulb",      0, 0, 0 },
    { DEV_CAP_COLOR_TEMP,   "Color Temp",     "color_temp",
      0, 0, NULL, NULL,                 0, 0, 0 },
    { DEV_CAP_COLOR_XY,     "Color",          "color_xy",
      0, 0, NULL, NULL,                 0, 0, 0 },
    { DEV_CAP_COVER,        "Cover",          "cover",
      0, 0, NULL, "mdi:window-shutter", 0, 0, 0 },
    { DEV_CAP_FAN,          "Fan",            "fan",
      0, 0, NULL, "mdi:fan",            0, 0, 0 },
    { DEV_CAP_CLIMATE,      "Climate",        "climate",
      0, 0, NULL, "mdi:thermostat",     0, 0, 0 },
    { DEV_CAP_LOCK,         "Lock",           "lock",
      0, 0, NULL, "mdi:lock",           0, 0, 0 },
};

#define CAP_METADATA_COUNT  (sizeof(s_cap_metadata) / sizeof(s_cap_metadata[0]))

/**
 * @brief Find metadata for a capability
 *
 * Linear search through 28 entries — called only during registration.
 *
 * @param cap Capability flag (single bit)
 * @return Pointer to metadata, or NULL if not found
 */
static inline const cap_metadata_t *cap_metadata_find(device_capability_t cap)
{
    for (size_t i = 0; i < CAP_METADATA_COUNT; i++) {
        if (s_cap_metadata[i].cap == cap) {
            return &s_cap_metadata[i];
        }
    }
    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ADAPTER_CAP_METADATA_H */
