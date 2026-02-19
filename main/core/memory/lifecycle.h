/**
 * @file lifecycle.h
 * @brief Lifecycle state machine for ESP32-C5 Zigbee2MQTT NG
 *
 * Manages application lifecycle states with valid state transitions,
 * callback notifications, and state tracking.
 *
 * State Transitions:
 *   BOOT -> PROVISIONING, RUNNING
 *   PROVISIONING -> RUNNING
 *   RUNNING -> PAIRING, LOW_MEMORY, OTA, SHUTDOWN
 *   PAIRING -> RUNNING
 *   LOW_MEMORY -> RUNNING
 *   OTA -> SHUTDOWN, RUNNING
 *
 * @copyright Copyright (c) 2024-2025
 */

#ifndef LIFECYCLE_H
#define LIFECYCLE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Lifecycle states for the application
 */
typedef enum {
    LIFECYCLE_BOOT,         /**< Minimal: nur WiFi/NVS */
    LIFECYCLE_PROVISIONING, /**< Captive Portal aktiv */
    LIFECYCLE_PAIRING,      /**< Zigbee Pairing, BLE pausiert */
    LIFECYCLE_RUNNING,      /**< Normal Operation */
    LIFECYCLE_LOW_MEMORY,   /**< Graceful Degradation */
    LIFECYCLE_OTA,          /**< OTA Update */
    LIFECYCLE_SHUTDOWN,     /**< Cleanup vor Restart */
} lifecycle_state_t;

/**
 * @brief Lifecycle state information
 */
typedef struct {
    lifecycle_state_t state;     /**< Current lifecycle state */
    uint32_t state_entered_time; /**< Timestamp when state was entered (ms since boot) */
    size_t heap_at_entry;        /**< Free heap when state was entered */
} lifecycle_info_t;

/**
 * @brief Callback function type for lifecycle state changes
 *
 * @param old_state Previous lifecycle state
 * @param new_state New lifecycle state
 * @param ctx User context pointer passed during registration
 */
typedef void (*lifecycle_callback_t)(lifecycle_state_t old_state, lifecycle_state_t new_state, void *ctx);

/**
 * @brief Initialize the lifecycle manager
 *
 * Sets the initial state to LIFECYCLE_BOOT.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if already initialized
 */
esp_err_t lifecycle_init(void);

/**
 * @brief Transition to a new lifecycle state
 *
 * Validates the transition against the state machine rules,
 * calls registered callbacks, and updates the state.
 *
 * @param new_state The target state to transition to
 * @return
 *     - ESP_OK on successful transition
 *     - ESP_ERR_INVALID_STATE if transition is not valid
 *     - ESP_ERR_INVALID_ARG if new_state is invalid
 */
esp_err_t lifecycle_transition(lifecycle_state_t new_state);

/**
 * @brief Get the current lifecycle state
 *
 * @return Current lifecycle state
 */
lifecycle_state_t lifecycle_get_state(void);

/**
 * @brief Get detailed lifecycle information
 *
 * @param[out] info Pointer to lifecycle_info_t struct to fill
 */
void lifecycle_get_info(lifecycle_info_t *info);

/**
 * @brief Register a callback for lifecycle state changes
 *
 * The callback will be called before the state transition completes.
 * Maximum 8 callbacks can be registered.
 *
 * @param cb Callback function
 * @param ctx User context pointer passed to callback
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_NO_MEM if maximum callbacks reached
 *     - ESP_ERR_INVALID_ARG if cb is NULL
 */
esp_err_t lifecycle_register_callback(lifecycle_callback_t cb, void *ctx);

/**
 * @brief Convert lifecycle state to string
 *
 * @param state Lifecycle state to convert
 * @return String representation of the state
 */
const char *lifecycle_state_to_str(lifecycle_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* LIFECYCLE_H */
