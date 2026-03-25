/**
 * @file esphome_adapter_exposes.h
 * @brief Expose-Based Entity Sub-module for ESPHome Adapter
 *
 * Handles registration, command callbacks, and state updates for entities
 * created from converter expose definitions (selects, numbers, text,
 * buttons, sensors, binary sensors, switches).
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_ADAPTER_EXPOSES_H
#define ESPHOME_ADAPTER_EXPOSES_H

#include "esp_err.h"
#include "core/device/unified_device.h"
#include "zigbee/converter/zb_converter_types.h"
#include "esphome/esphome_common.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Key offset for expose-based entities (prefix 0x2, distinct from cap-based 0x1) */
#define ESPHOME_ADAPTER_EXPOSE_KEY_OFFSET  0x20000000

/**
 * @brief Register entities from converter exposes (PRIMARY expose-first path)
 *
 * Iterates ALL exposes and registers ESPHome entities with full metadata.
 * Returns a bitmask of DEV_CAP_* flags covered, so cap-based fallback skips those.
 *
 * @param dev Device pointer
 * @param conv Converter definition with exposes
 * @return Bitmask of capabilities covered by expose entities
 */
uint32_t esphome_adapter_exposes_register(const device_t *dev,
                                           const zb_converter_def_t *conv);

/**
 * @brief Register expose-based entities (LEGACY path)
 *
 * Only registers entities NOT already covered by DEV_CAP_* capability-based
 * registration. Called for devices where expose-first was NOT used.
 *
 * @param dev Device pointer
 */
void esphome_adapter_exposes_register_legacy(const device_t *dev);

/**
 * @brief Update expose-based entity states from device state JSON
 *
 * Handles ALL expose types for expose-first devices, or only non-cap-covered
 * ones for legacy devices.
 *
 * @param dev Device pointer
 * @param state cJSON state object
 */
void esphome_adapter_exposes_update_states(const device_t *dev, cJSON *state);

/**
 * @brief Generate ESPHome entity key from device ID and expose index
 */
esphome_entity_key_t esphome_adapter_make_expose_key(device_id_t id, uint8_t expose_idx);

/**
 * @brief Check if a converter expose is already covered by DEV_CAP_* registration
 */
bool esphome_adapter_is_expose_covered_by_cap(const zb_expose_t *expose, uint32_t caps);

/**
 * @brief Map expose to DEV_CAP_* bitmask that it covers
 */
uint32_t esphome_adapter_expose_to_cap_bitmask(const zb_expose_t *expose);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ADAPTER_EXPOSES_H */
