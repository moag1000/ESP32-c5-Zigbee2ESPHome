/**
 * @file zigbee_adapter.h
 * @brief Zigbee Adapter - Bridges ESP-Zigbee-SDK to Event Bus and Device Registry
 *
 * This adapter provides a thin bridge layer between the existing Zigbee implementation
 * (zb_callbacks, zb_device_handler, zb_coordinator) and the new Phase 1 foundation
 * (event_bus, device_registry, unified_device).
 *
 * The adapter:
 *   - Hooks into existing Zigbee callbacks without heavily modifying them
 *   - Translates Zigbee events to event bus publications
 *   - Synchronizes Zigbee device state to the unified device registry
 *   - Maps Zigbee clusters/attributes to device capabilities
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZIGBEE_ADAPTER_H
#define ZIGBEE_ADAPTER_H

#include "esp_err.h"
#include "core/device/unified_device.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Initialization / Lifecycle
 * ============================================================================ */

/**
 * @brief Initialize the Zigbee adapter
 *
 * Sets up internal state and registers callbacks with the existing Zigbee modules.
 * Call this after event_bus_init() and device_registry_init() but before
 * zb_coordinator_start().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized or dependencies not ready
 *      - ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t zigbee_adapter_init(void);

/**
 * @brief Start the Zigbee adapter
 *
 * Begins actively processing events from the Zigbee stack and publishing
 * to the event bus. Call after zb_coordinator_start() when the network is ready.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized or already started
 */
esp_err_t zigbee_adapter_start(void);

/**
 * @brief Stop the Zigbee adapter
 *
 * Stops processing events but maintains internal state. The adapter can be
 * restarted with zigbee_adapter_start().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not running
 */
esp_err_t zigbee_adapter_stop(void);

/**
 * @brief Deinitialize the Zigbee adapter
 *
 * Unregisters callbacks and frees all resources. The adapter must be
 * reinitialized to use again.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t zigbee_adapter_deinit(void);

/**
 * @brief Check if the adapter is initialized
 *
 * @return true if initialized, false otherwise
 */
bool zigbee_adapter_is_initialized(void);

/**
 * @brief Check if the adapter is running
 *
 * @return true if started and processing events, false otherwise
 */
bool zigbee_adapter_is_running(void);

/* ============================================================================
 * Device Synchronization
 * ============================================================================ */

/**
 * @brief Sync all Zigbee devices to the unified registry
 *
 * Iterates through all devices in the Zigbee device handler and syncs
 * them to the unified device registry. Useful on startup or after
 * recovering from an error condition.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if adapter not initialized
 */
esp_err_t zigbee_adapter_sync_all_devices(void);

/* ============================================================================
 * Event Notification Hooks (Internal - called by Zigbee stack)
 * ============================================================================ */

/**
 * @brief Notify adapter of availability change
 *
 * Called when device availability status changes (online/offline).
 * Updates device registry and publishes appropriate events.
 *
 * @param short_addr Device network short address
 * @param online True if device is now online
 */
void zigbee_adapter_on_availability_change(uint16_t short_addr, bool online);

#ifdef __cplusplus
}
#endif

#endif /* ZIGBEE_ADAPTER_H */
