/**
 * @file ble_manager.h
 * @brief BLE Stack Management API for ESP32-C5 Zigbee2MQTT Gateway
 *
 * This module provides BLE stack initialization, configuration, and lifecycle
 * management using the NimBLE stack for efficient memory usage.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "esp_err.h"
#include "ble_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BLE manager state enumeration
 */
typedef enum {
    BLE_MANAGER_STATE_UNINITIALIZED = 0,    /**< BLE stack not initialized */
    BLE_MANAGER_STATE_INITIALIZED,          /**< BLE stack initialized and ready */
    BLE_MANAGER_STATE_RUNNING,              /**< BLE stack running (scanning/connected) */
    BLE_MANAGER_STATE_ERROR,                /**< BLE stack in error state */
    BLE_MANAGER_STATE_DEINITIALIZING        /**< BLE stack shutting down */
} ble_manager_state_t;

/**
 * @brief BLE manager configuration structure
 */
typedef struct {
    bool enable_scanner;                    /**< Enable passive scanner on init */
    uint32_t scan_interval_ms;              /**< Initial scan interval in ms */
    bool coexist_with_zigbee;               /**< Enable Zigbee coexistence mode */
} ble_manager_config_t;

/**
 * @brief Default BLE manager configuration
 */
#define BLE_MANAGER_CONFIG_DEFAULT() { \
    .enable_scanner = true, \
    .scan_interval_ms = BLE_DEFAULT_SCAN_INTERVAL_MS, \
    .coexist_with_zigbee = true \
}

/**
 * @brief Initialize the BLE manager and NimBLE stack
 *
 * Initializes the NimBLE BLE stack, configures the host, and prepares
 * for scanning operations. This function must be called before any
 * other BLE operations.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if memory allocation fails
 * @return ESP_ERR_INVALID_STATE if already initialized
 * @return ESP_FAIL on NimBLE stack initialization failure
 */
esp_err_t ble_manager_init(void);

/**
 * @brief Initialize the BLE manager with custom configuration
 *
 * @param config Pointer to configuration structure
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if config is NULL
 * @return ESP_ERR_NO_MEM if memory allocation fails
 * @return ESP_ERR_INVALID_STATE if already initialized
 * @return ESP_FAIL on NimBLE stack initialization failure
 */
esp_err_t ble_manager_init_with_config(const ble_manager_config_t *config);

/**
 * @brief Deinitialize the BLE manager and release resources
 *
 * Stops all BLE operations, deinitializes the NimBLE stack, and
 * releases all allocated resources. After calling this, ble_manager_init()
 * must be called again before any BLE operations.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 * @return ESP_FAIL on cleanup failure
 */
esp_err_t ble_manager_deinit(void);

/**
 * @brief Check if BLE manager is initialized
 *
 * @return true if BLE manager is initialized (ready or running)
 * @return false if not initialized or in error state
 */
bool ble_manager_is_initialized(void);

/**
 * @brief Get current BLE manager state
 *
 * @return Current manager state
 */
ble_manager_state_t ble_manager_get_state(void);

/**
 * @brief Get BLE manager state as string
 *
 * @return String representation of current state
 */
const char *ble_manager_get_state_str(void);

/**
 * @brief Enable Zigbee coexistence mode
 *
 * Configures BLE scanning parameters to minimize interference with
 * the Zigbee radio. This reduces scan duty cycle during Zigbee
 * network activity.
 *
 * @param enable true to enable coexistence mode, false to disable
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ble_manager_set_coexist_mode(bool enable);

/**
 * @brief Check if Zigbee coexistence mode is enabled
 *
 * @return true if coexistence mode is active
 * @return false if coexistence mode is disabled
 */
bool ble_manager_is_coexist_enabled(void);

/**
 * @brief Get free heap memory available for BLE operations
 *
 * @return Available heap memory in bytes
 */
uint32_t ble_manager_get_free_heap(void);

/**
 * @brief Self-test function for BLE manager
 *
 * Verifies that the BLE manager has been properly initialized.
 *
 * @return ESP_OK if all checks pass
 * @return ESP_FAIL if any check fails
 */
esp_err_t ble_manager_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_MANAGER_H */
