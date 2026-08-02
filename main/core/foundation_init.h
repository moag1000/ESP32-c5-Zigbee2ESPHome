/**
 * @file foundation_init.h
 * @brief Foundation Initialization for ESP32-C5 Zigbee2MQTT NG
 *
 * Wires together all Phase 1-3 components in the correct initialization order:
 *   Phase 1: lifecycle, memory_pressure, event_bus, device_registry
 *   Phase 2: zigbee_adapter, mqtt_adapter, ble_adapter
 *   Phase 3: ha_discovery_ng
 *
 * Initialization Order:
 *   1. lifecycle_init()           - State machine first
 *   2. memory_pressure_init()     - Memory monitoring
 *   3. event_bus_init()           - Event system
 *   4. device_registry_init()     - Device storage
 *   5. device_persistence_load()  - Load saved devices (if available)
 *   6. ha_discovery_ng_init()     - Subscribe to device events
 *   7. zigbee_adapter_init()      - Zigbee bridge (init only)
 *   8. mqtt_adapter_init()        - MQTT bridge
 *   9. ble_adapter_init()         - BLE bridge (init only)
 *
 * Start Order (call foundation_start_adapters() when network ready):
 *   1. memory_pressure_start()    - Start monitoring
 *   2. lifecycle_transition(LIFECYCLE_RUNNING)
 *   3. zigbee_adapter_start()     - After Zigbee network formed
 *   4. ble_adapter_start()        - After BLE init
 *   5. zigbee_adapter_sync_all_devices()
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef FOUNDATION_INIT_H
#define FOUNDATION_INIT_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Component Bitmask Definitions
 * ============================================================================ */

/**
 * @brief Bitmask flags for tracking initialized components
 */
typedef enum {
    FOUNDATION_COMP_LIFECYCLE        = (1 << 0),  /**< Lifecycle manager */
    FOUNDATION_COMP_MEMORY_PRESSURE  = (1 << 1),  /**< Memory pressure monitor */
    FOUNDATION_COMP_EVENT_BUS        = (1 << 2),  /**< Event bus */
    FOUNDATION_COMP_DEVICE_REGISTRY  = (1 << 3),  /**< Device registry */
    FOUNDATION_COMP_DEVICE_PERSIST   = (1 << 4),  /**< Device persistence (optional) */
    FOUNDATION_COMP_HA_DISCOVERY     = (1 << 5),  /**< HA Discovery NG */
    FOUNDATION_COMP_ZIGBEE_ADAPTER   = (1 << 6),  /**< Zigbee adapter */
    FOUNDATION_COMP_MQTT_ADAPTER     = (1 << 7),  /**< MQTT adapter */
    FOUNDATION_COMP_BLE_ADAPTER      = (1 << 8),  /**< BLE adapter */
    FOUNDATION_COMP_ESPHOME_ADAPTER  = (1 << 9),  /**< ESPHome adapter (optional) */
    FOUNDATION_COMP_EVENT_TRACE      = (1 << 10), /**< Event tracing (optional, debug) */
    FOUNDATION_COMP_MEMORY_DASHBOARD = (1 << 11), /**< Memory dashboard (optional) */
} foundation_component_t;

/**
 * @brief All core components that must succeed for system operation
 */
#define FOUNDATION_CORE_COMPONENTS \
    (FOUNDATION_COMP_LIFECYCLE | FOUNDATION_COMP_MEMORY_PRESSURE | \
     FOUNDATION_COMP_EVENT_BUS | FOUNDATION_COMP_DEVICE_REGISTRY)

/**
 * @brief All adapter components
 */
#define FOUNDATION_ADAPTER_COMPONENTS \
    (FOUNDATION_COMP_ZIGBEE_ADAPTER | FOUNDATION_COMP_MQTT_ADAPTER | \
     FOUNDATION_COMP_BLE_ADAPTER)

/* ============================================================================
 * Foundation Status Structure
 * ============================================================================ */

/**
 * @brief Foundation initialization status
 */
typedef struct {
    uint32_t initialized_mask;   /**< Bitmask of successfully initialized components */
    uint32_t started_mask;       /**< Bitmask of started adapters */
    uint32_t failed_mask;        /**< Bitmask of components that failed to initialize */
    esp_err_t first_error;       /**< First error encountered during init */
    const char *first_error_comp; /**< Name of component that failed first */
} foundation_status_t;

/* ============================================================================
 * Initialization / Deinitialization
 * ============================================================================ */

/**
 * @brief Initialize all foundation components in correct order
 *
 * Initializes Phase 1-3 components sequentially:
 *   1. Lifecycle manager (state machine)
 *   2. Memory pressure monitor
 *   3. Event bus
 *   4. Device registry
 *   5. Device persistence load (if available)
 *   6. HA Discovery NG
 *   7. Zigbee adapter
 *   8. MQTT adapter
 *   9. BLE adapter
 *
 * If a component fails, initialization continues with remaining components
 * unless it's a core component. The error is logged and tracked.
 *
 * @return
 *      - ESP_OK if all components initialized successfully
 *      - ESP_ERR_INVALID_STATE if already initialized
 *      - First error code if a core component failed
 *
 * @note Use foundation_get_status() to check which components initialized
 */
esp_err_t foundation_init(void);

/**
 * @brief Deinitialize all foundation components
 *
 * Deinitializes components in reverse order. Only deinitializes
 * components that were successfully initialized.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t foundation_deinit(void);

/* ============================================================================
 * Adapter Start / Stop
 * ============================================================================ */

/**
 * @brief Start all adapters (call after network/zigbee is ready)
 *
 * Starts adapters and transitions system to running state:
 *   1. memory_pressure_start()
 *   2. lifecycle_transition(LIFECYCLE_RUNNING)
 *   3. zigbee_adapter_start()
 *   4. ble_adapter_start()
 *   5. zigbee_adapter_sync_all_devices()
 *
 * @return
 *      - ESP_OK if all adapters started successfully
 *      - ESP_ERR_INVALID_STATE if foundation not initialized
 *      - First error code if an adapter failed to start
 */
esp_err_t foundation_start_adapters(void);

/**
 * @brief Stop all adapters
 *
 * Stops all running adapters and transitions to shutdown state.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if adapters not started
 */
esp_err_t foundation_stop_adapters(void);

/* ============================================================================
 * Status & Diagnostics
 * ============================================================================ */

/**
 * @brief Check if foundation is initialized
 *
 * @return true if foundation_init() completed, false otherwise
 */
bool foundation_is_initialized(void);

/**
 * @brief Check if adapters are started
 *
 * @return true if foundation_start_adapters() completed, false otherwise
 */
bool foundation_is_running(void);

/**
 * @brief Get current foundation status
 *
 * @param[out] status Pointer to status structure to fill
 */
void foundation_get_status(foundation_status_t *status);

/**
 * @brief Check if a specific component is initialized
 *
 * @param component Component flag to check
 * @return true if component is initialized, false otherwise
 */
bool foundation_is_component_initialized(foundation_component_t component);

/**
 * @brief Get component name string
 *
 * @param component Component flag
 * @return String name of component, or "UNKNOWN" if invalid
 */
const char *foundation_component_to_str(foundation_component_t component);

/**
 * @brief Print foundation status to log
 *
 * Logs initialization state of all components at INFO level.
 */
void foundation_print_status(void);

/* ============================================================================
 * Buffer Pool Access
 * ============================================================================ */

/**
 * @brief Get the JSON buffer pool handle
 *
 * The JSON pool provides 4 x 2KB buffers for JSON serialization work.
 * Use mem_pool_acquire/release from memory_manager_ng.h.
 *
 * @return Pool handle, or NULL if not initialized
 */
struct buffer_pool *foundation_get_json_pool(void);

/**
 * @brief Get the MQTT buffer pool handle
 *
 * The MQTT pool provides 8 x 512B buffers for MQTT message formatting.
 * Use mem_pool_acquire/release from memory_manager_ng.h.
 *
 * @return Pool handle, or NULL if not initialized
 */
struct buffer_pool *foundation_get_mqtt_pool(void);

#ifdef __cplusplus
}
#endif

#endif /* FOUNDATION_INIT_H */
