/**
 * @file ota_handler.h
 * @brief OTA (Over-The-Air) Update Handler
 *
 * Manages firmware updates over HTTP/HTTPS including:
 * - Checking for available updates
 * - Downloading firmware
 * - Verifying firmware integrity
 * - Installing updates
 * - Rollback on failure
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief OTA state enumeration
 */
typedef enum {
    OTA_STATE_IDLE = 0,          /**< No OTA operation in progress */
    OTA_STATE_CHECKING,          /**< Checking for updates */
    OTA_STATE_AVAILABLE,         /**< Update available */
    OTA_STATE_DOWNLOADING,       /**< Downloading firmware */
    OTA_STATE_VERIFYING,         /**< Verifying firmware */
    OTA_STATE_UPDATING,          /**< Writing firmware to flash */
    OTA_STATE_SUCCESS,           /**< Update successful */
    OTA_STATE_FAILED             /**< Update failed */
} ota_state_t;

/**
 * @brief OTA error codes
 */
typedef enum {
    OTA_ERROR_NONE = 0,          /**< No error */
    OTA_ERROR_NO_CONNECTION,     /**< No network connection */
    OTA_ERROR_HTTP_FAILED,       /**< HTTP request failed */
    OTA_ERROR_DOWNLOAD_FAILED,   /**< Download failed */
    OTA_ERROR_VERIFY_FAILED,     /**< Verification failed */
    OTA_ERROR_WRITE_FAILED,      /**< Flash write failed */
    OTA_ERROR_NO_UPDATE,         /**< No update available */
    OTA_ERROR_INVALID_IMAGE,     /**< Invalid firmware image */
    OTA_ERROR_OUT_OF_MEMORY      /**< Out of memory */
} ota_error_t;

/**
 * @brief OTA information structure
 */
typedef struct {
    char current_version[32];    /**< Current firmware version */
    char available_version[32];  /**< Available firmware version */
    ota_state_t state;           /**< Current OTA state */
    ota_error_t error;           /**< Last error code */
    uint32_t download_progress;  /**< Download progress (0-100) */
    uint32_t total_size;         /**< Total firmware size in bytes */
    uint32_t downloaded_size;    /**< Downloaded size in bytes */
    bool update_available;       /**< True if update available */
} ota_info_t;

/**
 * @brief OTA progress callback function type
 *
 * @param[in] progress Progress percentage (0-100)
 * @param[in] state Current OTA state
 */
typedef void (*ota_progress_callback_t)(uint32_t progress, ota_state_t state);

/**
 * @brief Initialize OTA handler
 *
 * Sets up OTA subsystem and configures firmware URL.
 *
 * @param[in] firmware_url URL for firmware updates (HTTP/HTTPS)
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if URL is NULL or invalid
 */
esp_err_t ota_handler_init(const char *firmware_url);

/**
 * @brief Check for available firmware updates
 *
 * Queries the update server to check if a newer version is available.
 * Non-blocking - returns immediately and updates OTA info.
 *
 * @return ESP_OK if check initiated successfully
 *         ESP_ERR_INVALID_STATE if OTA not initialized
 */
esp_err_t ota_handler_check_for_update(void);

/**
 * @brief Start firmware update
 *
 * Begins downloading and installing firmware update.
 * This is a blocking operation that runs in a separate task.
 *
 * @return ESP_OK if update started successfully
 *         ESP_ERR_INVALID_STATE if no update available or OTA busy
 */
esp_err_t ota_handler_start_update(void);

/**
 * @brief Get current OTA information
 *
 * @return OTA information structure
 */
ota_info_t ota_handler_get_info(void);

/**
 * @brief Get OTA state as string
 *
 * @param[in] state OTA state enumeration
 * @return String representation of state
 */
const char* ota_handler_get_state_string(ota_state_t state);

/**
 * @brief Get OTA error as string
 *
 * @param[in] error OTA error enumeration
 * @return String representation of error
 */
const char* ota_handler_get_error_string(ota_error_t error);

/**
 * @brief Cancel ongoing OTA update
 *
 * Stops any in-progress OTA operation.
 *
 * @return ESP_OK on success
 */
esp_err_t ota_handler_cancel(void);

/**
 * @brief Set firmware URL
 *
 * Updates the URL used for firmware updates.
 *
 * @param[in] firmware_url New firmware URL
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if URL invalid
 */
esp_err_t ota_handler_set_url(const char *firmware_url);

/**
 * @brief Register progress callback
 *
 * Registers a callback function to receive OTA progress updates.
 *
 * @param[in] callback Callback function pointer
 * @return ESP_OK on success
 */
esp_err_t ota_handler_register_callback(ota_progress_callback_t callback);

/**
 * @brief Get OTA partition information
 *
 * Returns information about OTA partitions.
 *
 * @param[out] buffer Buffer to store partition info string
 * @param[in] buffer_size Size of buffer
 * @return ESP_OK on success
 */
esp_err_t ota_handler_get_partition_info(char *buffer, size_t buffer_size);

/**
 * @brief Mark current app as valid
 *
 * Confirms that the current firmware is working correctly.
 * Should be called after successful boot of new firmware.
 *
 * @return ESP_OK on success
 */
esp_err_t ota_handler_mark_valid(void);

/**
 * @brief Check if rollback is possible
 *
 * @return true if rollback to previous version is possible
 */
bool ota_handler_can_rollback(void);

/**
 * @brief Trigger rollback to previous firmware
 *
 * Reverts to the previous firmware version.
 *
 * @return ESP_OK on success (system will reboot)
 */
esp_err_t ota_handler_rollback(void);

/**
 * @brief OTA task function
 *
 * Main OTA task that handles background operations.
 * Should be created as a FreeRTOS task.
 *
 * @param[in] pvParameters Task parameters (unused)
 */
void ota_handler_task(void *pvParameters);

/**
 * @brief Enable/disable automatic OTA checks
 *
 * @param[in] enable true to enable, false to disable
 * @param[in] interval_hours Check interval in hours
 * @return ESP_OK on success
 */
esp_err_t ota_handler_set_auto_check(bool enable, uint32_t interval_hours);

/**
 * @brief Get OTA info as JSON
 *
 * @param[out] buffer Buffer to store JSON string
 * @param[in] buffer_size Size of buffer
 * @return ESP_OK on success
 */
esp_err_t ota_handler_get_json(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* OTA_HANDLER_H */
