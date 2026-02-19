/**
 * @file esphome_adapter.h
 * @brief ESPHome API Adapter - Bridges Event Bus and Device Registry to ESPHome API
 *
 * This adapter provides integration between the unified device registry/event bus
 * and the ESPHome Native API. It enables Home Assistant to discover and control
 * devices managed by this gateway via the ESPHome protocol.
 *
 * The adapter:
 *   - Subscribes to event bus events (device state, join, leave)
 *   - Maps unified device capabilities to ESPHome entity types
 *   - Registers/unregisters ESPHome entities dynamically
 *   - Pushes state updates to ESPHome clients
 *   - Syncs all entities when a client connects
 *
 * Entity mapping:
 *   - DEV_CAP_TEMPERATURE -> SENSOR entity (device_class=temperature)
 *   - DEV_CAP_HUMIDITY -> SENSOR entity (device_class=humidity)
 *   - DEV_CAP_ON_OFF -> SWITCH or LIGHT entity
 *   - DEV_CAP_BRIGHTNESS -> LIGHT entity with brightness support
 *   - DEV_CAP_COLOR_TEMP -> LIGHT entity with color temp support
 *   - DEV_CAP_MOTION -> BINARY_SENSOR entity (device_class=motion)
 *   - DEV_CAP_CONTACT -> BINARY_SENSOR entity (device_class=door/window)
 *   - DEV_CAP_BATTERY -> SENSOR entity (device_class=battery)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_ADAPTER_H
#define ESPHOME_ADAPTER_H

#include "esp_err.h"
#include "core/device/unified_device.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/** @brief Maximum entities per device (sensors, switches, lights, etc.) */
#define ESPHOME_ADAPTER_MAX_ENTITIES_PER_DEVICE     8

/** @brief Entity key offset to avoid conflicts with other ESPHome entities */
#define ESPHOME_ADAPTER_KEY_OFFSET                  0x10000000

/* ============================================================================
 * Initialization / Lifecycle
 * ============================================================================ */

/**
 * @brief Initialize the ESPHome adapter
 *
 * Subscribes to event bus events and prepares for entity registration.
 * Call this after event_bus_init(), device_registry_init(), and esphome_api_init().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized or dependencies not ready
 *      - ESP_ERR_NO_MEM if subscription fails
 */
esp_err_t esphome_adapter_init(void);

/**
 * @brief Deinitialize the ESPHome adapter
 *
 * Unsubscribes from all event bus events and unregisters all entities
 * created by this adapter.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t esphome_adapter_deinit(void);

/**
 * @brief Check if the adapter is initialized
 *
 * @return true if initialized, false otherwise
 */
bool esphome_adapter_is_initialized(void);

/* ============================================================================
 * Client Connection Callbacks
 * ============================================================================ */

/**
 * @brief Notify adapter that an ESPHome client connected
 *
 * Called by the ESPHome API server when a client connects and authenticates.
 * Triggers synchronization of all devices from the registry to ESPHome entities.
 *
 * This function:
 *   1. Iterates all devices in the device registry
 *   2. Registers ESPHome entities for each device based on capabilities
 *   3. Sends current state for each entity to the client
 */
void esphome_adapter_on_client_connected(void);

/**
 * @brief Notify adapter that an ESPHome client disconnected
 *
 * Called by the ESPHome API server when a client disconnects.
 * Currently used for statistics tracking and cleanup if needed.
 */
void esphome_adapter_on_client_disconnected(void);

/* ============================================================================
 * Device Synchronization
 * ============================================================================ */

/**
 * @brief Sync a single device to ESPHome entities
 *
 * Creates ESPHome entities for the device based on its capabilities.
 * If entities already exist, updates their state.
 *
 * @param id Device ID
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_INVALID_STATE if adapter not initialized
 */
esp_err_t esphome_adapter_sync_device(device_id_t id);

/**
 * @brief Sync all devices from registry to ESPHome
 *
 * Iterates all devices in the registry and creates corresponding
 * ESPHome entities. Called automatically on client connect.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if adapter not initialized
 */
esp_err_t esphome_adapter_sync_all_devices(void);

/**
 * @brief Remove ESPHome entities for a device
 *
 * Unregisters all ESPHome entities associated with the given device.
 * Called when a device leaves the network.
 *
 * @param id Device ID
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if adapter not initialized
 */
esp_err_t esphome_adapter_remove_device(device_id_t id);

/* ============================================================================
 * Entity Key Management
 * ============================================================================ */

/**
 * @brief Generate ESPHome entity key from device ID and capability
 *
 * Creates a unique 32-bit entity key by combining the device ID with
 * the capability type. This ensures unique keys across all devices.
 *
 * @param id Device ID
 * @param cap Device capability (DEV_CAP_*)
 * @return ESPHome entity key
 */
uint32_t esphome_adapter_make_entity_key(device_id_t id, device_capability_t cap);

/**
 * @brief Extract device ID from ESPHome entity key
 *
 * Reverses the key generation to find which device an entity belongs to.
 *
 * @param key ESPHome entity key
 * @param[out] id Output device ID
 * @param[out] cap Output capability type
 * @return
 *      - ESP_OK if key is valid and from this adapter
 *      - ESP_ERR_INVALID_ARG if key is not from this adapter
 */
esp_err_t esphome_adapter_parse_entity_key(uint32_t key, device_id_t *id,
                                            device_capability_t *cap);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/**
 * @brief ESPHome adapter statistics
 */
typedef struct {
    uint32_t entities_registered;       /**< Total entities created */
    uint32_t entities_removed;          /**< Total entities removed */
    uint32_t state_updates_sent;        /**< State updates pushed to clients */
    uint32_t commands_received;         /**< Commands received from clients */
    uint32_t client_connects;           /**< Total client connections */
    uint32_t client_disconnects;        /**< Total client disconnections */
    uint32_t sync_operations;           /**< Device sync operations performed */
} esphome_adapter_stats_t;

/**
 * @brief Get adapter statistics
 *
 * @param[out] stats Pointer to statistics structure to fill
 */
void esphome_adapter_get_stats(esphome_adapter_stats_t *stats);

/**
 * @brief Reset adapter statistics
 */
void esphome_adapter_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ADAPTER_H */
