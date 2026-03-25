/**
 * @file tuya_driver_registry.h
 * @brief Tuya Device Driver Registry API
 *
 * Manages registration and lookup of Tuya device drivers.
 * Provides cached binding of short_addr to driver for O(1) lookup.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef TUYA_DRIVER_REGISTRY_H
#define TUYA_DRIVER_REGISTRY_H

#include "tuya_device_driver.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of registered drivers */
#define TUYA_MAX_DRIVERS          16

/** @brief Maximum number of device-to-driver bindings */
#define TUYA_MAX_DEVICE_BINDINGS  ZB_TUYA_MAX_DEVICES

/**
 * @brief Initialize the driver registry
 *
 * @return ESP_OK on success
 */
esp_err_t tuya_driver_registry_init(void);

/**
 * @brief Register a Tuya device driver
 *
 * @param driver Pointer to static driver vtable (must remain valid)
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if registry is full
 * @return ESP_ERR_INVALID_ARG if driver is NULL
 */
esp_err_t tuya_driver_register(const tuya_device_driver_t *driver);

/**
 * @brief Find a driver matching manufacturer/model
 *
 * Iterates registered drivers and calls match() on each.
 *
 * @param manufacturer Manufacturer string
 * @param model Model string
 * @return Driver pointer, or NULL if no match
 */
const tuya_device_driver_t *tuya_driver_find(const char *manufacturer, const char *model);

/**
 * @brief Get cached driver for a device by short address
 *
 * Fast lookup via linear scan of bindings (max TUYA_MAX_DEVICE_BINDINGS).
 *
 * @param short_addr Device short address
 * @return Driver pointer, or NULL if not bound
 */
const tuya_device_driver_t *tuya_driver_get(uint16_t short_addr);

/**
 * @brief Bind a device to a driver
 *
 * Creates a cached mapping from short_addr to driver.
 *
 * @param short_addr Device short address
 * @param driver Driver to bind
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if binding table is full
 */
esp_err_t tuya_driver_bind(uint16_t short_addr, const tuya_device_driver_t *driver);

/**
 * @brief Remove device binding
 *
 * @param short_addr Device short address
 */
void tuya_driver_unbind(uint16_t short_addr);

/**
 * @brief Unbind all Tuya driver bindings
 *
 * Clears the entire binding table. Used during network reset/factory reset
 * and new network formation to prevent stale bindings.
 */
void tuya_driver_unbind_all(void);

/**
 * @brief Persist driver binding to NVS (called automatically by tuya_driver_bind)
 *
 * @param short_addr Device short address (used to look up IEEE in device registry)
 * @param driver Driver that was bound
 */
void tuya_driver_persist_binding(uint16_t short_addr, const tuya_device_driver_t *driver);

/**
 * @brief Restore persisted driver binding for a device
 *
 * Called on device join/announce to re-bind a Tuya driver that was
 * persisted in NVS from a previous session. Uses IEEE address to
 * find the saved driver name, then binds using the current short_addr.
 *
 * @param ieee_addr Device IEEE address
 * @param short_addr Current device short address
 * @return ESP_OK if binding was restored
 * @return ESP_ERR_NOT_FOUND if no persisted binding exists
 */
esp_err_t tuya_driver_restore_for_device(uint64_t ieee_addr, uint16_t short_addr);

#ifdef __cplusplus
}
#endif

#endif /* TUYA_DRIVER_REGISTRY_H */
