/**
 * @file esphome_device_registry.h
 * @brief ESPHome Entity Integration with Unified Device Registry
 *
 * Provides integration between ESPHome entities and the unified device_registry.
 * ESPHome entities are registered as virtual devices (DEV_PROTOCOL_VIRTUAL) and
 * their state changes are published via the event bus.
 *
 * Features:
 *   - Automatic device_registry registration when ESPHome entities are created
 *   - Capability mapping from ESPHome entity types to device capabilities
 *   - State synchronization between ESPHome and device_registry
 *   - Event bus integration for state change notifications
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_DEVICE_REGISTRY_H
#define ESPHOME_DEVICE_REGISTRY_H

#include "esp_err.h"
#include "esphome_entities_types.h"
#include "core/device/unified_device.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/**
 * @brief Base device ID for ESPHome virtual entities
 *
 * ESPHome entity keys are 32-bit, but device_id_t is 64-bit.
 * We use a high prefix to avoid collision with IEEE/MAC addresses.
 * Format: 0xESPH_0000_XXXX_XXXX where XXXX_XXXX is the entity key.
 */
#define ESPHOME_DEVICE_ID_PREFIX    0xE5D40000ULL

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * @brief Initialize ESPHome device registry integration
 *
 * Sets up the bridge between ESPHome entity system and the unified
 * device_registry. Registers a state change callback to sync states.
 *
 * Prerequisites:
 *   - device_registry_init() must have been called
 *   - esphome_entities_init() must have been called
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized or prerequisites not met
 */
esp_err_t esphome_device_registry_init(void);

/**
 * @brief Deinitialize ESPHome device registry integration
 *
 * Removes all ESPHome virtual devices from the registry.
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_device_registry_deinit(void);

/**
 * @brief Check if integration is initialized
 *
 * @return true if initialized, false otherwise
 */
bool esphome_device_registry_is_initialized(void);

/* ============================================================================
 * Device Registration (called internally by entity registration)
 * ============================================================================ */

/**
 * @brief Register an ESPHome entity in the device registry
 *
 * Creates a virtual device entry in the unified device_registry for the
 * given ESPHome entity. Sets up appropriate capabilities based on entity type.
 *
 * @param[in] entity_type ESPHome entity type
 * @param[in] key Entity key (used to derive device_id)
 * @param[in] name Entity friendly name
 * @param[in] unique_id Entity unique ID (used as model)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_NO_MEM if registry is full
 */
esp_err_t esphome_device_registry_register(esphome_entity_type_t entity_type,
                                            esphome_entity_key_t key,
                                            const char *name,
                                            const char *unique_id);

/**
 * @brief Unregister an ESPHome entity from the device registry
 *
 * @param[in] key Entity key
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if entity not registered
 */
esp_err_t esphome_device_registry_unregister(esphome_entity_key_t key);

/* ============================================================================
 * State Synchronization
 * ============================================================================ */

/**
 * @brief Sync ESPHome entity state to device registry
 *
 * Updates the device_registry state for an ESPHome entity and publishes
 * an EVT_DEVICE_STATE_CHANGED event.
 *
 * @param[in] entity_type ESPHome entity type
 * @param[in] key Entity key
 * @param[in] state Pointer to entity state structure
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_NOT_FOUND if entity not in registry
 */
esp_err_t esphome_device_registry_sync_state(esphome_entity_type_t entity_type,
                                              esphome_entity_key_t key,
                                              const void *state);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * @brief Convert ESPHome entity key to device_id
 *
 * @param[in] key ESPHome entity key
 * @return device_id_t for the entity
 */
device_id_t esphome_key_to_device_id(esphome_entity_key_t key);

/**
 * @brief Convert device_id back to ESPHome entity key
 *
 * @param[in] id Device ID
 * @param[out] key Output entity key
 * @return
 *      - ESP_OK if this is an ESPHome device ID
 *      - ESP_ERR_INVALID_ARG if not an ESPHome device ID
 */
esp_err_t esphome_device_id_to_key(device_id_t id, esphome_entity_key_t *key);

/**
 * @brief Check if a device_id represents an ESPHome entity
 *
 * @param[in] id Device ID to check
 * @return true if this is an ESPHome virtual device
 */
bool esphome_is_virtual_device(device_id_t id);

/**
 * @brief Map ESPHome entity type to device capabilities
 *
 * @param[in] entity_type ESPHome entity type
 * @return Bitmask of device_capability_t flags
 */
uint32_t esphome_entity_type_to_capabilities(esphome_entity_type_t entity_type);

/**
 * @brief Get device from registry by ESPHome entity key
 *
 * @param[in] key ESPHome entity key
 * @return Pointer to device_t, or NULL if not found
 */
device_t *esphome_device_registry_get(esphome_entity_key_t key);

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

/**
 * @brief Log ESPHome device registry statistics
 */
void esphome_device_registry_log_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_DEVICE_REGISTRY_H */
