/**
 * @file device_persistence.h
 * @brief Device Persistence Module for ESP32-C5 Zigbee2MQTT NG
 *
 * Persists device registry entries to NVS (Non-Volatile Storage) for
 * recovery across reboots. Stores device configuration and metadata
 * but NOT runtime state (last_seen, availability, cJSON state).
 *
 * Storage format:
 *   - NVS namespace: "devices"
 *   - Device blobs: "dev_XXXXXXXXXXXXXXXX" (hex device_id)
 *   - Device count: "dev_count" (uint16_t)
 *   - Device ID list: "dev_list" (array of device_id_t)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef DEVICE_PERSISTENCE_H
#define DEVICE_PERSISTENCE_H

#include "esp_err.h"
#include "unified_device.h"
#include "device_registry.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/**
 * @brief NVS namespace for device storage
 */
#define DEVICE_PERSIST_NVS_NAMESPACE    "devices"

/**
 * @brief Maximum devices that can be persisted
 *
 * Tied to DEVICE_REGISTRY_MAX_DEVICES so registry and persistence
 * can never diverge (which would cause silent data loss on reboot).
 */
#define DEVICE_PERSIST_MAX_DEVICES      DEVICE_REGISTRY_MAX_DEVICES

/* ============================================================================
 * Initialization / Deinitialization
 * ============================================================================ */

/**
 * @brief Initialize the device persistence module
 *
 * Opens the NVS namespace for device storage. Must be called before
 * any other persistence functions.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NVS_* on NVS errors
 *      - ESP_ERR_INVALID_STATE if already initialized
 */
esp_err_t device_persistence_init(void);

/**
 * @brief Deinitialize the device persistence module
 *
 * Closes the NVS handle. Safe to call even if not initialized.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t device_persistence_deinit(void);

/**
 * @brief Check if persistence module is initialized
 *
 * @return true if initialized, false otherwise
 */
bool device_persistence_is_initialized(void);

/* ============================================================================
 * Single Device Operations
 * ============================================================================ */

/**
 * @brief Save a single device to NVS
 *
 * Persists the device's configuration and metadata. Runtime state
 * (last_seen, availability, cJSON state) is NOT persisted.
 *
 * @param[in] dev Pointer to device to save
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if dev is NULL
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_NVS_* on NVS errors
 */
esp_err_t device_persistence_save(const device_t *dev);

/**
 * @brief Delete a device from NVS
 *
 * Removes the device blob from NVS and updates the device list.
 *
 * @param[in] id Device identifier to delete
 * @return
 *      - ESP_OK on success (even if device was not persisted)
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_NVS_* on NVS errors
 */
esp_err_t device_persistence_delete(device_id_t id);

/* ============================================================================
 * Bulk Operations
 * ============================================================================ */

/**
 * @brief Load all persisted devices into the registry
 *
 * Reads all device blobs from NVS and adds them to the device registry.
 * The registry must be initialized before calling this function.
 *
 * On load, devices have:
 *   - availability set to DEV_AVAIL_UNKNOWN
 *   - last_seen set to 0
 *   - in_use set to true
 *
 * @return
 *      - ESP_OK on success (even if no devices were loaded)
 *      - ESP_ERR_INVALID_STATE if not initialized or registry not initialized
 *      - ESP_ERR_NVS_* on NVS errors
 */
esp_err_t device_persistence_load_all(void);

/**
 * @brief Save all devices from registry to NVS
 *
 * Iterates through all devices in the registry and persists them.
 * Existing persisted devices are overwritten.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized or registry not initialized
 *      - ESP_ERR_NVS_* on NVS errors
 */
esp_err_t device_persistence_save_all(void);

/**
 * @brief Clear all persisted devices from NVS
 *
 * Erases all device data from the NVS namespace. Does NOT affect
 * devices currently in the registry.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 *      - ESP_ERR_NVS_* on NVS errors
 */
esp_err_t device_persistence_clear(void);

/* ============================================================================
 * Statistics / Diagnostics
 * ============================================================================ */

/**
 * @brief Get count of persisted devices
 *
 * @return Number of devices stored in NVS, or 0 on error
 */
uint16_t device_persistence_get_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_PERSISTENCE_H */
