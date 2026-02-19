/**
 * @file crash_reporter.h
 * @brief Crash Reporter - Boot Reason and Crash Detection
 *
 * Reports the device reset reason to MQTT on boot, allowing users to
 * detect crashes, watchdog resets, brownouts, and other abnormal reboots.
 * Tracks boot count in NVS for persistent monitoring.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef CRASH_REPORTER_H
#define CRASH_REPORTER_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Crash Reporter Constants
 * ============================================================================ */

/** @brief NVS namespace for crash reporter */
#define CRASH_REPORTER_NVS_NAMESPACE        "crash_rpt"

/** @brief NVS key for boot count */
#define CRASH_REPORTER_NVS_KEY_BOOT_COUNT   "boot_cnt"

/** @brief Maximum reset reason string length */
#define CRASH_REPORTER_REASON_STR_LEN       32

/** @brief MQTT topic for crash reports */
#define CRASH_REPORTER_MQTT_TOPIC           "zigbee2mqtt/bridge/crash"

/* ============================================================================
 * Crash Information Structure
 * ============================================================================ */

/**
 * @brief Crash information structure
 *
 * Contains information about the last reset and boot count.
 */
typedef struct {
    esp_reset_reason_t reason;      /**< ESP-IDF reset reason enum */
    const char *reason_str;         /**< Human-readable reset reason string */
    bool was_crash;                 /**< True if reset was due to a crash/error */
    uint32_t boot_count;            /**< Total boot count since NVS was cleared */
} crash_info_t;

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Initialize the crash reporter
 *
 * Reads the reset reason from the system and increments the boot count
 * stored in NVS. Should be called early in the boot process, after NVS
 * is initialized.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NVS_* on NVS errors
 */
esp_err_t crash_reporter_init(void);

/**
 * @brief Publish crash report to MQTT
 *
 * Publishes the last reset reason and boot count to MQTT. Should be called
 * after MQTT connection is established.
 *
 * Published JSON format:
 * @code
 * {
 *   "reset_reason": "panic",
 *   "was_crash": true,
 *   "boot_count": 42,
 *   "timestamp": 1234567890
 * }
 * @endcode
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if MQTT not connected or module not initialized
 * @return ESP_ERR_NO_MEM if JSON creation fails
 */
esp_err_t crash_reporter_publish(void);

/**
 * @brief Get current crash information
 *
 * Retrieves the crash information collected during initialization.
 *
 * @param[out] info Pointer to crash_info_t structure to populate
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if info is NULL
 * @return ESP_ERR_INVALID_STATE if module not initialized
 */
esp_err_t crash_reporter_get_info(crash_info_t *info);

/**
 * @brief Get the reset reason as a string
 *
 * Converts the ESP-IDF reset reason enum to a human-readable string.
 *
 * @param[in] reason ESP-IDF reset reason enum value
 * @return Constant string representing the reset reason
 */
const char* crash_reporter_reason_to_string(esp_reset_reason_t reason);

/**
 * @brief Check if the last reset was a crash
 *
 * Determines if the last reset was due to an error condition
 * (panic, watchdog timeout, brownout, etc.) as opposed to a
 * normal power-on or software reset.
 *
 * @param[in] reason ESP-IDF reset reason enum value
 * @return true if the reset was a crash/error, false otherwise
 */
bool crash_reporter_is_crash(esp_reset_reason_t reason);

/**
 * @brief Get the current boot count
 *
 * Returns the total number of boots since NVS was cleared.
 *
 * @return Boot count, or 0 if module not initialized
 */
uint32_t crash_reporter_get_boot_count(void);

/**
 * @brief Get the last reset reason string
 *
 * Returns the human-readable string for the last reset reason.
 *
 * @return Constant string, or "unknown" if module not initialized
 */
const char* crash_reporter_get_reason_str(void);

/**
 * @brief Check if last reset was a crash
 *
 * @return true if last reset was a crash, false otherwise
 */
bool crash_reporter_was_crash(void);

/**
 * @brief Check if crash reporter is initialized
 *
 * @return true if initialized, false otherwise
 */
bool crash_reporter_is_initialized(void);

/**
 * @brief Reset boot count to zero
 *
 * Clears the boot count in NVS. Useful for factory reset.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NVS_* on NVS errors
 */
esp_err_t crash_reporter_reset_boot_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CRASH_REPORTER_H */
