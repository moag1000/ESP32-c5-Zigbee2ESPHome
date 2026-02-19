/**
 * @file coex_manager.h
 * @brief WiFi/Zigbee/BLE Coexistence Manager for ESP32-C5
 *
 * ESP32-C5 has a single 2.4 GHz radio shared between WiFi, Zigbee (802.15.4),
 * and BLE. This module coordinates access to the radio to avoid interference
 * and optimize timing using time-division multiplexing.
 *
 * Key Features:
 *   - Priority-based radio access arbitration
 *   - Adaptive BLE scan duty cycle based on Zigbee/WiFi activity
 *   - Temporary BLE pause during critical Zigbee operations
 *   - Integration with ESP-IDF coexistence APIs
 *
 * @copyright Copyright (c) 2024-2025
 * @license Apache License 2.0
 */

#ifndef COEX_MANAGER_H
#define COEX_MANAGER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Maximum number of concurrent radio requesters */
#define COEX_MAX_REQUESTERS             8

/** @brief Maximum requester name length */
#define COEX_REQUESTER_NAME_MAX         16

/** @brief Default BLE scan duty cycle percentage */
#define COEX_DEFAULT_BLE_DUTY           50

/** @brief Minimum BLE scan duty cycle percentage */
#define COEX_MIN_BLE_DUTY               10

/** @brief Maximum BLE scan duty cycle percentage */
#define COEX_MAX_BLE_DUTY               100

/** @brief BLE duty when Zigbee is forming network */
#define COEX_BLE_DUTY_ZB_FORMING        20

/** @brief BLE duty when Zigbee permit_join is active */
#define COEX_BLE_DUTY_ZB_PAIRING        30

/** @brief BLE duty during high WiFi traffic */
#define COEX_BLE_DUTY_WIFI_BUSY         40

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * @brief Priority levels for radio access requests
 *
 * Higher priority requests may preempt lower priority operations.
 * The coexistence module uses these hints to adjust BLE scanning
 * and coordinate radio access.
 */
typedef enum {
    COEX_PRIO_LOW = 0,       /**< Background (BLE passive scan, periodic reports) */
    COEX_PRIO_NORMAL,        /**< Normal operation (device state updates) */
    COEX_PRIO_HIGH,          /**< Important (device join, ZCL commands) */
    COEX_PRIO_CRITICAL,      /**< Time-sensitive (network formation, OTA) */
} coex_priority_t;

/**
 * @brief Radio types managed by coexistence
 */
typedef enum {
    COEX_RADIO_WIFI = 0,     /**< WiFi 802.11 */
    COEX_RADIO_ZIGBEE,       /**< Zigbee 802.15.4 */
    COEX_RADIO_BLE,          /**< Bluetooth Low Energy */
    COEX_RADIO_COUNT
} coex_radio_t;

/**
 * @brief Coexistence status information
 */
typedef struct {
    bool wifi_active;               /**< WiFi is connected and active */
    bool zigbee_active;             /**< Zigbee network is formed */
    bool ble_scanning;              /**< BLE scanner is running */
    uint8_t ble_scan_duty;          /**< Current BLE scan duty cycle (0-100%) */
    coex_priority_t current_priority;   /**< Highest active priority */
    uint8_t active_requesters;      /**< Number of active radio requests */
    bool permit_join_active;        /**< Zigbee permit_join is active */
    bool ble_paused;                /**< BLE scanning is temporarily paused */
    uint32_t ble_pause_remaining_ms;    /**< Remaining BLE pause time in ms */
} coex_status_t;

/**
 * @brief Coexistence statistics
 */
typedef struct {
    uint32_t radio_requests_total;  /**< Total radio requests made */
    uint32_t radio_conflicts;       /**< Number of detected conflicts */
    uint32_t ble_pauses;            /**< Number of BLE pause events */
    uint32_t duty_adjustments;      /**< Number of BLE duty adjustments */
    uint32_t uptime_ms;             /**< Time since coex_manager_init */
} coex_stats_t;

/**
 * @brief Callback for coexistence events
 *
 * Called when significant coexistence events occur (priority changes,
 * BLE pause/resume, duty cycle changes).
 *
 * @param event_type Event identifier string (e.g., "ble_paused", "duty_changed")
 * @param ctx User context
 */
typedef void (*coex_event_callback_t)(const char *event_type, void *ctx);

/* ============================================================================
 * Initialization & Control
 * ============================================================================ */

/**
 * @brief Initialize the coexistence manager
 *
 * Sets up internal state, enables ESP-IDF software coexistence if configured,
 * and prepares for radio coordination.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if already initialized
 *     - ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t coex_manager_init(void);

/**
 * @brief Deinitialize the coexistence manager
 *
 * Releases all resources and stops coordination.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t coex_manager_deinit(void);

/**
 * @brief Check if coexistence manager is initialized
 *
 * @return true if initialized, false otherwise
 */
bool coex_manager_is_initialized(void);

/* ============================================================================
 * Radio Access Management
 * ============================================================================ */

/**
 * @brief Request radio time with specified priority
 *
 * Registers a request for radio access. The coexistence manager uses
 * this information to adjust BLE scanning duty cycle and coordinate
 * radio time-division.
 *
 * @param prio Priority level for this request
 * @param requester Name identifying the requester (e.g., "zigbee_cmd", "mqtt_pub")
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized
 *     - ESP_ERR_INVALID_ARG if requester is NULL
 *     - ESP_ERR_NO_MEM if maximum requesters reached
 */
esp_err_t coex_request_radio(coex_priority_t prio, const char *requester);

/**
 * @brief Release radio time previously requested
 *
 * Removes the request from active tracking. BLE duty cycle may be
 * increased if no high-priority requests remain.
 *
 * @param requester Name used in coex_request_radio
 */
void coex_release_radio(const char *requester);

/**
 * @brief Update priority for an existing request
 *
 * @param requester Name of existing requester
 * @param new_prio New priority level
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_NOT_FOUND if requester not found
 */
esp_err_t coex_update_priority(const char *requester, coex_priority_t new_prio);

/* ============================================================================
 * BLE Scan Control
 * ============================================================================ */

/**
 * @brief Set BLE scan duty cycle
 *
 * Adjusts the BLE scan duty cycle. Lower values give more radio time
 * to WiFi and Zigbee operations.
 *
 * Duty cycle affects scan timing:
 *   duty_percent = (scan_window / scan_interval) * 100
 *
 * @param duty_percent Duty cycle percentage (10-100)
 */
void coex_set_ble_scan_duty(uint8_t duty_percent);

/**
 * @brief Get current BLE scan duty cycle
 *
 * @return Current duty cycle percentage (0-100)
 */
uint8_t coex_get_ble_scan_duty(void);

/**
 * @brief Temporarily pause BLE scanning
 *
 * Pauses BLE scanning for the specified duration to allow uninterrupted
 * Zigbee operations (e.g., network formation, device joining).
 *
 * @param duration_ms Pause duration in milliseconds (0 to resume immediately)
 */
void coex_pause_ble_scan(uint32_t duration_ms);

/**
 * @brief Resume BLE scanning immediately
 *
 * Cancels any active pause and resumes BLE scanning.
 */
void coex_resume_ble_scan(void);

/**
 * @brief Check if BLE scanning is currently paused
 *
 * @return true if paused, false if running normally
 */
bool coex_is_ble_paused(void);

/* ============================================================================
 * Radio Activity Notifications
 * ============================================================================ */

/**
 * @brief Notify coexistence manager of WiFi activity change
 *
 * Call when WiFi connection state changes or during high traffic periods.
 *
 * @param active true if WiFi is active/busy, false otherwise
 */
void coex_notify_wifi_activity(bool active);

/**
 * @brief Notify coexistence manager of Zigbee activity change
 *
 * Call when Zigbee network state changes or during operations.
 *
 * @param active true if Zigbee is active, false otherwise
 */
void coex_notify_zigbee_activity(bool active);

/**
 * @brief Notify coexistence manager that Zigbee permit_join changed
 *
 * When permit_join is active, BLE duty cycle is reduced to improve
 * Zigbee device pairing reliability.
 *
 * @param active true if permit_join enabled, false if disabled
 */
void coex_notify_permit_join(bool active);

/**
 * @brief Notify coexistence manager of BLE scanner state change
 *
 * @param scanning true if BLE scanning started, false if stopped
 */
void coex_notify_ble_scanning(bool scanning);

/* ============================================================================
 * Status & Statistics
 * ============================================================================ */

/**
 * @brief Get current coexistence status
 *
 * @param status Output: current status information
 */
void coex_get_status(coex_status_t *status);

/**
 * @brief Get coexistence statistics
 *
 * @param stats Output: statistics information
 */
void coex_get_stats(coex_stats_t *stats);

/**
 * @brief Reset coexistence statistics
 */
void coex_reset_stats(void);

/**
 * @brief Convert priority level to string
 *
 * @param prio Priority level
 * @return String representation (e.g., "LOW", "NORMAL", "HIGH", "CRITICAL")
 */
const char *coex_priority_to_str(coex_priority_t prio);

/**
 * @brief Convert radio type to string
 *
 * @param radio Radio type
 * @return String representation (e.g., "WIFI", "ZIGBEE", "BLE")
 */
const char *coex_radio_to_str(coex_radio_t radio);

/* ============================================================================
 * Event Callbacks
 * ============================================================================ */

/**
 * @brief Register callback for coexistence events
 *
 * @param callback Callback function
 * @param ctx User context passed to callback
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if callback is NULL
 *     - ESP_ERR_NO_MEM if maximum callbacks reached
 */
esp_err_t coex_register_callback(coex_event_callback_t callback, void *ctx);

/**
 * @brief Unregister coexistence event callback
 *
 * @param callback Previously registered callback
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_NOT_FOUND if callback not found
 */
esp_err_t coex_unregister_callback(coex_event_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* COEX_MANAGER_H */
