/**
 * @file http_ota.h
 * @brief HTTP OTA (Over-The-Air) Update Module
 *
 * Provides HTTP/HTTPS-based firmware update functionality with:
 * - Progress callbacks for UI/MQTT updates
 * - Automatic rollback support via ESP-IDF's app_update
 * - Version validation before install
 * - Lifecycle integration (LIFECYCLE_OTA phase)
 * - Certificate verification support for HTTPS
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef HTTP_OTA_H
#define HTTP_OTA_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP OTA configuration structure
 */
typedef struct {
    char url[256];              /**< Firmware URL (HTTP or HTTPS) */
    char cert_pem[2048];        /**< Optional server certificate for HTTPS verification */
    bool skip_cert_verify;      /**< Skip certificate verification (for testing only) */
} http_ota_config_t;

/**
 * @brief HTTP OTA progress callback function type
 *
 * @param[in] received  Number of bytes received so far
 * @param[in] total     Total firmware size in bytes (0 if unknown)
 * @param[in] ctx       User context pointer passed to http_ota_start()
 */
typedef void (*http_ota_progress_cb_t)(size_t received, size_t total, void *ctx);

/**
 * @brief HTTP OTA state enumeration
 */
typedef enum {
    HTTP_OTA_STATE_IDLE = 0,        /**< No OTA operation in progress */
    HTTP_OTA_STATE_STARTING,        /**< Preparing for OTA update */
    HTTP_OTA_STATE_DOWNLOADING,     /**< Downloading firmware */
    HTTP_OTA_STATE_VERIFYING,       /**< Verifying firmware image */
    HTTP_OTA_STATE_WRITING,         /**< Writing firmware to flash */
    HTTP_OTA_STATE_COMPLETE,        /**< OTA complete, pending reboot */
    HTTP_OTA_STATE_FAILED,          /**< OTA failed */
    HTTP_OTA_STATE_ABORTED,         /**< OTA aborted by user */
} http_ota_state_t;

/**
 * @brief HTTP OTA error codes
 */
typedef enum {
    HTTP_OTA_ERROR_NONE = 0,            /**< No error */
    HTTP_OTA_ERROR_INVALID_CONFIG,      /**< Invalid configuration */
    HTTP_OTA_ERROR_LIFECYCLE_FAILED,    /**< Failed to enter OTA lifecycle phase */
    HTTP_OTA_ERROR_ALREADY_IN_PROGRESS, /**< OTA already in progress */
    HTTP_OTA_ERROR_HTTP_CONNECT,        /**< HTTP connection failed */
    HTTP_OTA_ERROR_HTTP_RESPONSE,       /**< Invalid HTTP response */
    HTTP_OTA_ERROR_DOWNLOAD,            /**< Download failed */
    HTTP_OTA_ERROR_VERIFY,              /**< Firmware verification failed */
    HTTP_OTA_ERROR_WRITE,               /**< Flash write failed */
    HTTP_OTA_ERROR_VERSION_SAME,        /**< Same version as running */
    HTTP_OTA_ERROR_VERSION_OLDER,       /**< Older version than running */
    HTTP_OTA_ERROR_ABORTED,             /**< User aborted */
    HTTP_OTA_ERROR_OUT_OF_MEMORY,       /**< Out of memory */
} http_ota_error_t;

/**
 * @brief Initialize HTTP OTA module
 *
 * Creates required synchronization primitives and prepares the module.
 * Must be called before any other http_ota functions.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t http_ota_init(void);

/**
 * @brief Start HTTP OTA update
 *
 * Initiates firmware download from the specified URL. This function:
 * 1. Transitions to LIFECYCLE_OTA phase
 * 2. Connects to the firmware URL
 * 3. Downloads and verifies the firmware
 * 4. Writes firmware to the OTA partition
 * 5. Sets boot partition for next reboot
 *
 * The function runs in a separate task and returns immediately.
 * Use the progress callback to monitor status.
 *
 * @param[in] config        OTA configuration (URL, certificate, etc.)
 * @param[in] progress_cb   Progress callback function (may be NULL)
 * @param[in] ctx           User context passed to progress callback
 *
 * @return ESP_OK if OTA task started successfully
 * @return ESP_ERR_INVALID_ARG if config is NULL or URL is empty
 * @return ESP_ERR_INVALID_STATE if module not initialized or OTA in progress
 * @return ESP_ERR_NO_MEM if task creation fails
 */
esp_err_t http_ota_start(const http_ota_config_t *config,
                         http_ota_progress_cb_t progress_cb,
                         void *ctx);

/**
 * @brief Abort ongoing OTA update
 *
 * Signals the OTA task to abort the update. The task will clean up
 * and transition back to LIFECYCLE_RUNNING phase.
 *
 * @return ESP_OK if abort signaled
 * @return ESP_ERR_INVALID_STATE if no OTA in progress
 */
esp_err_t http_ota_abort(void);

/**
 * @brief Check if OTA is in progress
 *
 * @return true if OTA download/update is active
 * @return false if idle
 */
bool http_ota_is_in_progress(void);

/**
 * @brief Get current firmware version
 *
 * Returns the version string of the currently running firmware.
 *
 * @param[out] version  Buffer to store version string
 * @param[in]  len      Size of version buffer
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if version is NULL or len is 0
 */
esp_err_t http_ota_get_current_version(char *version, size_t len);

/**
 * @brief Get new firmware version (after download)
 *
 * Returns the version string of the downloaded firmware.
 * Only valid after a successful download.
 *
 * @param[out] version  Buffer to store version string
 * @param[in]  len      Size of version buffer
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if no new version available
 * @return ESP_ERR_INVALID_ARG if version is NULL or len is 0
 */
esp_err_t http_ota_get_new_version(char *version, size_t len);

/**
 * @brief Get current OTA state
 *
 * @return Current HTTP OTA state
 */
http_ota_state_t http_ota_get_state(void);

/**
 * @brief Get last OTA error
 *
 * @return Last HTTP OTA error code
 */
http_ota_error_t http_ota_get_error(void);

/**
 * @brief Get OTA state as string
 *
 * @param[in] state  OTA state enumeration
 * @return String representation of state
 */
const char *http_ota_state_str(http_ota_state_t state);

/**
 * @brief Get OTA error as string
 *
 * @param[in] error  OTA error enumeration
 * @return String representation of error
 */
const char *http_ota_error_str(http_ota_error_t error);

/**
 * @brief Mark current app as valid
 *
 * Confirms that the current firmware is working correctly.
 * Should be called after successful boot of new firmware to
 * prevent automatic rollback.
 *
 * @return ESP_OK on success
 */
esp_err_t http_ota_mark_app_valid(void);

/**
 * @brief Check if rollback is available
 *
 * @return true if rollback to previous firmware is possible
 */
bool http_ota_can_rollback(void);

/**
 * @brief Trigger rollback to previous firmware
 *
 * Reverts to the previous firmware version and reboots.
 * This function does not return on success.
 *
 * @return ESP_OK initiates reboot (never returns)
 * @return ESP_FAIL if rollback not available
 */
esp_err_t http_ota_rollback(void);

/**
 * @brief Get HTTP OTA status as JSON
 *
 * Generates JSON string with current OTA status including:
 * - state, error, progress, versions
 *
 * @param[out] buffer       Buffer to store JSON string
 * @param[in]  buffer_size  Size of buffer
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if buffer is NULL or size is 0
 * @return ESP_ERR_NO_MEM if buffer too small
 */
esp_err_t http_ota_get_json(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_OTA_H */
