/**
 * @file esphome_adapter_internal.h
 * @brief Shared helpers between ESPHome adapter sub-modules
 *
 * Exposes internal functions from esphome_adapter.c that are needed by
 * extracted sub-modules (tuya, exposes, diagnostics, gateway).
 *
 * NOT part of the public API — only included by esphome_adapter_*.c files.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_ADAPTER_INTERNAL_H
#define ESPHOME_ADAPTER_INTERNAL_H

#include "esphome_adapter.h"
#include "esphome/esphome_entities_types.h"
#include "core/device/unified_device.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generate entity name for expose-based entity
 *
 * Capitalizes expose name: "angle_x" → "Angle X"
 * For Zigbee sub-devices: just the suffix.
 * For other devices: "<friendly_name> <suffix>"
 */
void esphome_adapter_make_expose_name(const device_t *dev, const char *expose_name,
                                       char *buf, size_t buf_size);

/**
 * @brief Generate unique ID for expose-based entity
 *
 * Format: "0x<ieee_hex>_<expose_name>"
 */
void esphome_adapter_make_expose_unique_id(const device_t *dev, const char *expose_name,
                                            char *buf, size_t buf_size);

/**
 * @brief Map expose device_class string to ESPHome sensor device class enum
 */
esphome_sensor_device_class_t esphome_adapter_expose_dc_to_sensor_class(const char *dc);

/**
 * @brief Map expose state_class string to ESPHome state class enum
 */
esphome_state_class_t esphome_adapter_expose_sc_to_state_class(const char *sc);

/**
 * @brief Map expose device_class string to ESPHome binary sensor class enum
 */
esphome_binary_device_class_t esphome_adapter_expose_dc_to_binary_class(const char *dc);

/**
 * @brief Get mutable pointer to adapter statistics
 *
 * Used by sub-modules to increment counters directly.
 */
esphome_adapter_stats_t *esphome_adapter_get_stats_ptr(void);

/**
 * @brief Auto-clear timer type
 */
typedef enum {
    ESPHOME_AUTO_CLEAR_VIBRATION,
    ESPHOME_AUTO_CLEAR_ACTION,
} esphome_auto_clear_type_t;

/**
 * @brief Schedule auto-clear for an event-type entity
 *
 * Vibration: clears binary sensor after 5s
 * Action: clears text sensor after 1s
 *
 * @param key Entity key to clear
 * @param type Type of auto-clear
 */
void esphome_adapter_schedule_auto_clear(esphome_entity_key_t key,
                                          esphome_auto_clear_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ADAPTER_INTERNAL_H */
