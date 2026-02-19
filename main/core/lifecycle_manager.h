/**
 * @file lifecycle_manager.h
 * @brief Dynamic service lifecycle management for RAM optimization
 *
 * Manages services that are only needed in specific operational phases.
 * Services are started/stopped dynamically to free internal RAM when needed.
 *
 * Phases:
 * - BOOT: Minimal services for startup
 * - SETUP: WiFi captive portal (no MQTT/Zigbee/BLE)
 * - NORMAL: Full operation (all services)
 * - PAIRING: Zigbee permit_join (stop non-essential services for max RAM)
 * - OTA: Firmware update (minimal services)
 * - LOW_MEMORY: Emergency mode when heap is critical
 *
 * @copyright Copyright (c) 2024
 * @license Apache License 2.0
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Operational phases that affect service availability
 */
typedef enum {
    LIFECYCLE_PHASE_BOOT = 0,      /**< Initial boot, minimal services */
    LIFECYCLE_PHASE_SETUP,          /**< WiFi setup via captive portal */
    LIFECYCLE_PHASE_NORMAL,         /**< Normal operation, all services */
    LIFECYCLE_PHASE_PAIRING,        /**< Zigbee permit_join active */
    LIFECYCLE_PHASE_OTA,            /**< OTA update in progress */
    LIFECYCLE_PHASE_LOW_MEMORY,     /**< Emergency low memory mode */
} lifecycle_phase_t;

/**
 * @brief Service identifiers for lifecycle management
 */
typedef enum {
    /* Essential services - never stopped */
    SERVICE_WIFI = 0,               /**< WiFi connection (essential) */
    SERVICE_MQTT,                   /**< MQTT client (essential for commands) */
    SERVICE_ZIGBEE,                 /**< Zigbee coordinator (essential) */

    /* Deferrable services - can be stopped during pairing/low memory */
    SERVICE_BLE_SCANNER,            /**< BLE advertisement scanner */
    SERVICE_BLE_GATT,               /**< BLE GATT client */
    SERVICE_ESPHOME_API,            /**< ESPHome Native API server */
    SERVICE_ESPHOME_BLE_PROXY,      /**< ESPHome BLE Proxy */
    SERVICE_HA_DISCOVERY,           /**< Home Assistant MQTT discovery */
    SERVICE_STATE_PERSISTENCE,      /**< Device state persistence to flash */
    SERVICE_SYSTEM_MONITOR,         /**< System health monitoring */
    SERVICE_MQTT_LOGGER,            /**< MQTT log forwarding */
    SERVICE_AVAILABILITY,           /**< Device availability tracking */

    /* Setup-only services */
    SERVICE_CAPTIVE_PORTAL,         /**< WiFi captive portal */

    SERVICE_COUNT
} lifecycle_service_t;

/**
 * @brief Service state
 */
typedef enum {
    SERVICE_STATE_STOPPED = 0,
    SERVICE_STATE_STARTING,
    SERVICE_STATE_RUNNING,
    SERVICE_STATE_STOPPING,
    SERVICE_STATE_ERROR,
} service_state_t;

/**
 * @brief Service information
 */
typedef struct {
    lifecycle_service_t id;
    const char *name;
    service_state_t state;
    uint32_t stack_size;            /**< Approximate internal RAM used */
    bool uses_psram_stack;          /**< true if stack is in PSRAM */
    bool essential;                 /**< true if service cannot be stopped */
} service_info_t;

/**
 * @brief Initialize lifecycle manager
 * @return ESP_OK on success
 */
esp_err_t lifecycle_manager_init(void);

/**
 * @brief Get current operational phase
 * @return Current phase
 */
lifecycle_phase_t lifecycle_get_phase(void);

/**
 * @brief Transition to a new operational phase
 *
 * This will start/stop services as needed for the new phase.
 * Non-blocking - services are started/stopped asynchronously.
 *
 * @param phase New phase to enter
 * @return ESP_OK on success
 */
esp_err_t lifecycle_enter_phase(lifecycle_phase_t phase);

/**
 * @brief Get estimated internal RAM freed by entering a phase
 * @param phase Target phase
 * @return Estimated bytes of internal RAM that would be freed
 */
uint32_t lifecycle_estimate_ram_freed(lifecycle_phase_t phase);

/**
 * @brief Check if a service is currently running
 * @param service Service to check
 * @return true if running
 */
bool lifecycle_service_is_running(lifecycle_service_t service);

/**
 * @brief Get service information
 * @param service Service to query
 * @param info Output: service info
 * @return ESP_OK if service exists
 */
esp_err_t lifecycle_get_service_info(lifecycle_service_t service, service_info_t *info);

/**
 * @brief Manually start a service (outside of phase management)
 * @param service Service to start
 * @return ESP_OK on success
 */
esp_err_t lifecycle_start_service(lifecycle_service_t service);

/**
 * @brief Manually stop a service (outside of phase management)
 * @param service Service to stop
 * @return ESP_OK on success
 */
esp_err_t lifecycle_stop_service(lifecycle_service_t service);

/**
 * @brief Get total internal RAM used by running services
 * @return Bytes of internal RAM used by service stacks
 */
uint32_t lifecycle_get_services_ram_usage(void);

/**
 * @brief Get phase name as string
 * @param phase Phase to name
 * @return Phase name string
 */
const char *lifecycle_phase_name(lifecycle_phase_t phase);

/**
 * @brief Get service name as string
 * @param service Service to name
 * @return Service name string
 */
const char *lifecycle_service_name(lifecycle_service_t service);

#ifdef __cplusplus
}
#endif
