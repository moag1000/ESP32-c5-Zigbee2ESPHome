/**
 * @file led_status_manager.h
 * @brief LED Status Manager - Centralized LED status coordination
 *
 * This module provides a centralized manager for coordinating LED status
 * indications across the system. It uses a priority-based system to ensure
 * the most important status is always displayed, and handles temporary
 * notifications that briefly interrupt the current status.
 *
 * Priority hierarchy (highest to lowest):
 * 10 - OTA active (purple breathing)
 *  9 - System error (red fast blink)
 *  8 - System warning (orange slow blink)
 *  7 - Permit join/pairing (cyan breathing)
 *  6 - WiFi connecting (yellow slow blink)
 *  5 - Normal operation (green solid)
 *  4 - Boot phase (blue breathing)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef LED_STATUS_MANAGER_H
#define LED_STATUS_MANAGER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * LED Condition Flags
 * ============================================================================ */

/**
 * @brief LED condition flags representing system states
 *
 * These flags are combined to determine the overall LED status.
 * Multiple conditions can be active simultaneously, and the
 * highest-priority condition determines the LED display.
 */
typedef enum {
    LED_COND_BOOT_COMPLETE    = (1 << 0),   /**< Boot sequence finished */
    LED_COND_WIFI_CONNECTING  = (1 << 1),   /**< WiFi connecting/reconnecting */
    LED_COND_WIFI_CONNECTED   = (1 << 2),   /**< WiFi connected */
    LED_COND_MQTT_CONNECTED   = (1 << 3),   /**< MQTT broker connected */
    LED_COND_ZIGBEE_RUNNING   = (1 << 4),   /**< Zigbee stack running */
    LED_COND_BLE_RUNNING      = (1 << 9),   /**< BLE scanner/stack running */
    LED_COND_PERMIT_JOIN      = (1 << 5),   /**< Permit join mode active */
    LED_COND_OTA_ACTIVE       = (1 << 6),   /**< OTA update in progress */
    LED_COND_SYSTEM_WARNING   = (1 << 7),   /**< System health warning */
    LED_COND_SYSTEM_ERROR     = (1 << 8),   /**< System health critical */
} led_condition_t;

/* Note: LED notification types (LED_NOTIFY_DEVICE_NEW, etc.) are defined
 * in led_controller.h as led_notify_t. This manager uses those directly
 * via the led_status_manager_notify() function. */

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Initialize the LED status manager
 *
 * Must be called after led_controller_init() and before any other
 * led_status_manager functions. Sets the initial status to BOOT.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if LED controller not initialized
 */
esp_err_t led_status_manager_init(void);

/**
 * @brief Set or clear a system condition flag
 *
 * Updates the internal condition bitmap and re-evaluates the LED status.
 * Thread-safe - can be called from any task or ISR context.
 *
 * @param[in] condition The condition flag to set or clear
 * @param[in] active true to set the condition, false to clear it
 */
void led_status_manager_set_condition(led_condition_t condition, bool active);

/**
 * @brief Check if a condition is currently active
 *
 * @param[in] condition The condition flag to check
 * @return true if the condition is active
 */
bool led_status_manager_get_condition(led_condition_t condition);

/**
 * @brief Trigger a notification blink sequence
 *
 * Triggers a brief blink sequence that temporarily interrupts the
 * current status indication. After the blink sequence completes,
 * the LED automatically returns to showing the current status.
 *
 * Thread-safe - can be called from any task context.
 *
 * @param[in] notify The notification type (from led_controller.h: LED_NOTIFY_*)
 */
void led_status_manager_notify(int notify);

/**
 * @brief Force re-evaluation of LED status
 *
 * Normally the status is automatically updated when conditions change.
 * This function can be called to manually trigger a re-evaluation,
 * for example after changing LED settings.
 */
void led_status_manager_update(void);

/**
 * @brief Get the current condition bitmap
 *
 * Useful for debugging or displaying current system state.
 *
 * @return The current condition flags as a bitmask
 */
uint32_t led_status_manager_get_conditions(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_STATUS_MANAGER_H */
