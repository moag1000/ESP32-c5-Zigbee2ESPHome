/**
 * @file graceful_degradation.h
 * @brief Graceful Degradation for ESP32-C5 Zigbee2MQTT NG
 *
 * Provides automatic module unloading based on memory pressure levels.
 * When memory pressure increases, non-essential modules are unloaded
 * in priority order to free up resources.
 *
 * Integration:
 *   - Subscribes to memory_pressure callbacks
 *   - Uses module_manager for priority-based unloading
 *   - Transitions lifecycle state on high memory pressure
 *
 * Behavior by pressure level:
 *   NONE/LOW   - Normal operation, may re-enable modules after recovery
 *   MEDIUM     - Unload priority 0-2 modules
 *   HIGH       - Unload priority 0-4 modules, enter LOW_MEMORY state
 *   CRITICAL   - Emergency mode, only essential modules remain
 *
 * Hysteresis:
 *   Uses a 5-second delay before re-enabling modules after recovery
 *   to prevent oscillation between states.
 *
 * @copyright Copyright (c) 2024-2026
 * @license Apache License 2.0
 */

#ifndef GRACEFUL_DEGRADATION_H
#define GRACEFUL_DEGRADATION_H

#include "esp_err.h"
#include "memory_pressure.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** @brief Hysteresis delay in milliseconds before recovery actions */
#define GRACEFUL_DEG_HYSTERESIS_MS      (5 * 1000)

/** @brief Maximum module priority to unload at MEDIUM pressure */
#define GRACEFUL_DEG_MEDIUM_MAX_PRIORITY    2

/** @brief Maximum module priority to unload at HIGH pressure */
#define GRACEFUL_DEG_HIGH_MAX_PRIORITY      4

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * @brief Initialize graceful degradation
 *
 * Registers a callback with the memory pressure monitor to receive
 * notifications when pressure levels change. Must be called after
 * memory_pressure_init() and module_manager_init().
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if already initialized
 *     - ESP_FAIL if callback registration fails
 */
esp_err_t graceful_degradation_init(void);

/**
 * @brief Deinitialize graceful degradation
 *
 * Note: Callbacks cannot be unregistered from memory_pressure,
 * so this primarily resets internal state.
 */
void graceful_degradation_deinit(void);

/**
 * @brief Get current degradation state
 *
 * @return true if system is currently in degraded mode, false otherwise
 */
bool graceful_degradation_is_degraded(void);

/**
 * @brief Get the highest pressure level reached since init
 *
 * Useful for diagnostics to see if system has experienced memory pressure.
 *
 * @return Highest memory pressure level reached
 */
memory_pressure_t graceful_degradation_get_peak_pressure(void);

/**
 * @brief Get count of modules unloaded due to degradation
 *
 * @return Total number of modules unloaded since init
 */
size_t graceful_degradation_get_unload_count(void);

/**
 * @brief Force an immediate degradation check
 *
 * Manually triggers evaluation of current memory pressure
 * and takes appropriate action. Normally called automatically
 * by the memory pressure monitor.
 */
void graceful_degradation_check_now(void);

#ifdef __cplusplus
}
#endif

#endif /* GRACEFUL_DEGRADATION_H */
