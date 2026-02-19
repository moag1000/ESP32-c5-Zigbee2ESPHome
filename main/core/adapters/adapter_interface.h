/**
 * @file adapter_interface.h
 * @brief Unified adapter interface for protocol adapters
 *
 * Defines a common lifecycle contract (init/start/stop/deinit) for all
 * protocol adapters (Zigbee, MQTT, BLE, ESPHome). Provides a static
 * registry for generic iteration (status reporting, graceful shutdown).
 *
 * Each adapter registers a static adapter_ops_t vtable. NULL function
 * pointers are allowed — the registry skips them during iteration.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ADAPTER_INTERFACE_H
#define ADAPTER_INTERFACE_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of adapters that can be registered */
#define ADAPTER_MAX_COUNT   6
static_assert(ADAPTER_MAX_COUNT >= 4, "Need at least 4 slots for Zigbee/MQTT/BLE/ESPHome");

/**
 * @brief Adapter lifecycle operations (vtable)
 *
 * All function pointers are optional (may be NULL).
 * Return ESP_OK on success, any other value on failure.
 */
typedef struct {
    esp_err_t (*init)(void);        /**< Initialize adapter (allocate resources) */
    esp_err_t (*start)(void);       /**< Start adapter (begin processing) */
    esp_err_t (*stop)(void);        /**< Stop adapter (pause processing) */
    esp_err_t (*deinit)(void);      /**< Deinitialize adapter (free resources) */
    bool (*is_running)(void);       /**< Check if adapter is currently running */
} adapter_ops_t;

/**
 * @brief Adapter descriptor (registered at startup)
 */
typedef struct {
    const char *name;               /**< Human-readable adapter name */
    const adapter_ops_t *ops;       /**< Lifecycle operations vtable */
    bool initialized;               /**< Set by registry after successful init */
    bool started;                   /**< Set by registry after successful start */
} adapter_desc_t;

/**
 * @brief Register an adapter with the registry
 *
 * Must be called before foundation_init() or during Phase 3.
 * Registration order determines start order (and reverse stop order).
 *
 * @param name Adapter name (string literal, not copied)
 * @param ops  Lifecycle operations vtable (must be static/global)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if registry is full (ADAPTER_MAX_COUNT)
 *      - ESP_ERR_INVALID_ARG if name or ops is NULL
 */
esp_err_t adapter_registry_register(const char *name, const adapter_ops_t *ops);

/**
 * @brief Initialize all registered adapters (in registration order)
 *
 * Calls ops->init() for each adapter. Continues on failure.
 *
 * @return ESP_OK if all succeeded, first error otherwise
 */
esp_err_t adapter_registry_init_all(void);

/**
 * @brief Start all registered adapters (in registration order)
 *
 * Calls ops->start() for each initialized adapter that has a start function.
 *
 * @return ESP_OK if all succeeded, first error otherwise
 */
esp_err_t adapter_registry_start_all(void);

/**
 * @brief Stop all registered adapters (in reverse registration order)
 *
 * Calls ops->stop() for each started adapter that has a stop function.
 *
 * @return ESP_OK if all succeeded, first error otherwise
 */
esp_err_t adapter_registry_stop_all(void);

/**
 * @brief Deinitialize all registered adapters (in reverse registration order)
 *
 * Calls ops->deinit() for each initialized adapter that has a deinit function.
 *
 * @return ESP_OK if all succeeded, first error otherwise
 */
esp_err_t adapter_registry_deinit_all(void);

/**
 * @brief Get the number of registered adapters
 *
 * @return Number of registered adapters
 */
size_t adapter_registry_count(void);

/**
 * @brief Get adapter descriptor by index
 *
 * @param index Adapter index (0-based, in registration order)
 * @return Pointer to adapter descriptor, or NULL if index out of range
 */
const adapter_desc_t *adapter_registry_get(size_t index);

/**
 * @brief Log status of all registered adapters
 *
 * Logs name, initialized, started, and is_running status for each adapter.
 */
void adapter_registry_log_status(void);

/* ============================================================================
 * Built-in adapter vtables (defined in respective adapter .c files)
 * ============================================================================ */

extern const adapter_ops_t zigbee_adapter_ops;
extern const adapter_ops_t mqtt_adapter_ops;
extern const adapter_ops_t ble_adapter_ops;
extern const adapter_ops_t esphome_adapter_ops;

#ifdef __cplusplus
}
#endif

#endif /* ADAPTER_INTERFACE_H */
