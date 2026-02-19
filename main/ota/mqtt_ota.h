/**
 * @file mqtt_ota.h
 * @brief MQTT OTA Trigger for ESP32-C5 Zigbee2MQTT NG
 *
 * Enables triggering OTA updates via MQTT commands, compatible with
 * Zigbee2MQTT bridge commands. Supports checking for updates, starting
 * updates with a URL, and publishing progress updates.
 *
 * MQTT Topics:
 * - zigbee2mqtt/bridge/request/device/ota_update/check - Check for updates
 * - zigbee2mqtt/bridge/request/device/ota_update/update - Start update with URL
 * - zigbee2mqtt/bridge/request/device/ota_update/abort - Abort update
 * - zigbee2mqtt/bridge/response/device/ota_update/... - Response topics
 *
 * Payload Format:
 * Check:    {"id": "...", "url": "https://..."}
 * Update:   {"id": "...", "url": "https://...", "force": false}
 * Response: {"id": "...", "status": "started|progress|completed|failed",
 *            "progress": 50, "error": "..."}
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MQTT_OTA_H
#define MQTT_OTA_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MQTT OTA Topic Definitions
 * ============================================================================ */

/** @brief OTA check request topic */
#define MQTT_OTA_TOPIC_CHECK    "zigbee2mqtt/bridge/request/device/ota_update/check"

/** @brief OTA update request topic */
#define MQTT_OTA_TOPIC_UPDATE   "zigbee2mqtt/bridge/request/device/ota_update/update"

/** @brief OTA abort request topic */
#define MQTT_OTA_TOPIC_ABORT    "zigbee2mqtt/bridge/request/device/ota_update/abort"

/** @brief OTA check response topic */
#define MQTT_OTA_RESPONSE_CHECK "zigbee2mqtt/bridge/response/device/ota_update/check"

/** @brief OTA update response topic */
#define MQTT_OTA_RESPONSE_UPDATE "zigbee2mqtt/bridge/response/device/ota_update/update"

/** @brief OTA abort response topic */
#define MQTT_OTA_RESPONSE_ABORT "zigbee2mqtt/bridge/response/device/ota_update/abort"

/** @brief OTA progress status topic (for continuous updates) */
#define MQTT_OTA_TOPIC_STATUS   "zigbee2mqtt/bridge/ota/status"

/* ============================================================================
 * MQTT OTA Status Values
 * ============================================================================ */

/** @brief OTA status enumeration for MQTT responses */
typedef enum {
    MQTT_OTA_STATUS_IDLE = 0,        /**< No OTA operation in progress */
    MQTT_OTA_STATUS_CHECKING,        /**< Checking for updates */
    MQTT_OTA_STATUS_AVAILABLE,       /**< Update available */
    MQTT_OTA_STATUS_STARTED,         /**< Update started */
    MQTT_OTA_STATUS_PROGRESS,        /**< Update in progress */
    MQTT_OTA_STATUS_COMPLETED,       /**< Update completed successfully */
    MQTT_OTA_STATUS_FAILED,          /**< Update failed */
    MQTT_OTA_STATUS_ABORTED          /**< Update aborted by user */
} mqtt_ota_status_t;

/* ============================================================================
 * MQTT OTA API Functions
 * ============================================================================ */

/**
 * @brief Initialize MQTT OTA module
 *
 * Subscribes to OTA-related MQTT topics and sets up handlers.
 * Must be called after MQTT client is connected.
 *
 * @return
 *     - ESP_OK: Initialization successful
 *     - ESP_ERR_INVALID_STATE: MQTT client not connected
 *     - ESP_FAIL: Failed to subscribe to topics
 */
esp_err_t mqtt_ota_init(void);

/**
 * @brief Deinitialize MQTT OTA module
 *
 * Unsubscribes from OTA-related MQTT topics and cleans up.
 *
 * @return
 *     - ESP_OK: Deinitialization successful
 */
esp_err_t mqtt_ota_deinit(void);

/**
 * @brief Publish OTA status to MQTT
 *
 * Publishes current OTA status to the response topic. Called internally
 * by the OTA handler progress callback, but can also be called manually
 * to publish custom status updates.
 *
 * @param[in] status Status string ("started", "progress", "completed", "failed")
 * @param[in] progress Progress percentage (0-100), -1 to omit
 * @param[in] error Error message (NULL if no error)
 *
 * @return
 *     - ESP_OK: Status published successfully
 *     - ESP_ERR_INVALID_ARG: Invalid status string
 *     - ESP_FAIL: Failed to publish
 */
esp_err_t mqtt_ota_publish_status(const char *status, int progress, const char *error);

/**
 * @brief Publish OTA status with transaction ID
 *
 * Same as mqtt_ota_publish_status but includes a transaction ID for
 * correlation with the original request.
 *
 * @param[in] status Status string
 * @param[in] progress Progress percentage (0-100), -1 to omit
 * @param[in] error Error message (NULL if no error)
 * @param[in] transaction_id Transaction ID from request (NULL to omit)
 *
 * @return
 *     - ESP_OK: Status published successfully
 *     - ESP_FAIL: Failed to publish
 */
esp_err_t mqtt_ota_publish_status_with_id(const char *status, int progress,
                                          const char *error, const char *transaction_id);

/**
 * @brief Handle OTA check request from MQTT
 *
 * Parses the check request payload and initiates an OTA check.
 * If a URL is provided in the payload, it will be used for the check.
 *
 * @param[in] payload JSON payload from MQTT message
 * @param[in] len Length of the payload
 *
 * @return
 *     - ESP_OK: Check initiated successfully
 *     - ESP_ERR_INVALID_ARG: Invalid payload
 *     - ESP_ERR_INVALID_STATE: OTA already in progress
 */
esp_err_t mqtt_ota_handle_check(const char *payload, size_t len);

/**
 * @brief Handle OTA update request from MQTT
 *
 * Parses the update request payload and starts an OTA update.
 * The URL must be provided in the payload.
 *
 * @param[in] payload JSON payload from MQTT message
 * @param[in] len Length of the payload
 *
 * @return
 *     - ESP_OK: Update started successfully
 *     - ESP_ERR_INVALID_ARG: Invalid payload or missing URL
 *     - ESP_ERR_INVALID_STATE: OTA already in progress
 */
esp_err_t mqtt_ota_handle_update(const char *payload, size_t len);

/**
 * @brief Handle OTA abort request from MQTT
 *
 * Aborts an ongoing OTA update if one is in progress.
 *
 * @param[in] payload JSON payload from MQTT message
 * @param[in] len Length of the payload
 *
 * @return
 *     - ESP_OK: Abort successful
 *     - ESP_ERR_INVALID_STATE: No OTA in progress
 */
esp_err_t mqtt_ota_handle_abort(const char *payload, size_t len);

/**
 * @brief Check if MQTT OTA module is initialized
 *
 * @return true if initialized, false otherwise
 */
bool mqtt_ota_is_initialized(void);

/**
 * @brief Get current MQTT OTA status
 *
 * @return Current MQTT OTA status enum value
 */
mqtt_ota_status_t mqtt_ota_get_status(void);

/**
 * @brief Get status string from enum
 *
 * @param[in] status MQTT OTA status enum
 * @return String representation of status
 */
const char* mqtt_ota_status_to_string(mqtt_ota_status_t status);

/**
 * @brief Re-subscribe to OTA topics after MQTT reconnect
 *
 * Called by the MQTT bridge when connection is re-established.
 *
 * @return
 *     - ESP_OK: Re-subscription successful
 *     - ESP_FAIL: Failed to re-subscribe
 */
esp_err_t mqtt_ota_resubscribe(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_OTA_H */
