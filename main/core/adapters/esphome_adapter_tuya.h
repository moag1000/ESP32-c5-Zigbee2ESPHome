/**
 * @file esphome_adapter_tuya.h
 * @brief Tuya Driver Entity Sub-module for ESPHome Adapter
 *
 * Handles registration and state updates for entities created from
 * Tuya device drivers (build_state_json → ESPHome entities).
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_ADAPTER_TUYA_H
#define ESPHOME_ADAPTER_TUYA_H

#include "esp_err.h"
#include "core/device/unified_device.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register ESPHome entities from a device's Tuya driver
 *
 * Calls the driver's build_state_json() to discover fields, then
 * creates ESPHome entities based on JSON value types and optional
 * entity_meta annotations.
 *
 * @param dev Pointer to device (must have Tuya driver bound)
 * @return ESP_OK on success, ESP_OK if no driver found (no-op)
 */
esp_err_t esphome_adapter_tuya_register(const device_t *dev);

/**
 * @brief Update Tuya entity states from device state JSON
 *
 * Iterates the Tuya key cache and updates matching ESPHome entities.
 *
 * @param dev Pointer to device
 * @param state cJSON state object
 */
void esphome_adapter_tuya_update_states(const device_t *dev, cJSON *state);

/**
 * @brief Remove all Tuya key cache entries for a device
 *
 * @param id Device ID
 */
void esphome_adapter_tuya_remove_device(device_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ADAPTER_TUYA_H */
