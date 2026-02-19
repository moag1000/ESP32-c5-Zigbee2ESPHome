/**
 * @file bridge_response.h
 * @brief Consistent Bridge Response Format for Zigbee2MQTT Compatibility
 *
 * Provides standardized response formatting for bridge requests following
 * the Zigbee2MQTT response pattern:
 * {
 *   "data": { ... },
 *   "status": "ok" | "error",
 *   "error": "message",        // Only when status="error"
 *   "transaction": "abc123"    // Optional: echoes request transaction
 * }
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BRIDGE_RESPONSE_H
#define BRIDGE_RESPONSE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bridge response status values
 */
typedef enum {
    BRIDGE_RESPONSE_OK,     /**< Request completed successfully */
    BRIDGE_RESPONSE_ERROR   /**< Request failed with error */
} bridge_response_status_t;

/**
 * @brief Bridge response structure
 *
 * Used to build consistent response messages for bridge requests.
 * The caller owns the data cJSON object and must ensure proper cleanup.
 */
typedef struct {
    bridge_response_status_t status;    /**< Response status (ok/error) */
    const char *error_message;          /**< Error message (NULL if status=OK) */
    const char *transaction_id;         /**< Transaction ID from request (NULL if not set) */
    cJSON *data;                        /**< Response data object (caller owns) */
} bridge_response_t;

/**
 * @brief Initialize bridge response structure
 *
 * Sets all fields to default values (status=OK, NULL pointers).
 * Must be called before using other bridge_response functions.
 *
 * @param[out] resp Pointer to response structure to initialize
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if resp is NULL
 */
esp_err_t bridge_response_init(bridge_response_t *resp);

/**
 * @brief Set response status to OK with optional data
 *
 * Sets status to BRIDGE_RESPONSE_OK and attaches optional data.
 * If data is NULL, an empty object will be used in the response.
 *
 * @param[in,out] resp Pointer to response structure
 * @param[in] data cJSON data object (ownership transferred to response)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if resp is NULL
 */
esp_err_t bridge_response_set_ok(bridge_response_t *resp, cJSON *data);

/**
 * @brief Set response status to ERROR with message
 *
 * Sets status to BRIDGE_RESPONSE_ERROR and sets error message.
 * The data field will contain an empty object.
 *
 * @param[in,out] resp Pointer to response structure
 * @param[in] error Error message string (copied internally)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if resp or error is NULL
 */
esp_err_t bridge_response_set_error(bridge_response_t *resp, const char *error);

/**
 * @brief Set transaction ID from request
 *
 * Sets the transaction ID to be echoed in the response.
 * Transaction ID is typically extracted from the incoming request.
 *
 * @param[in,out] resp Pointer to response structure
 * @param[in] txn_id Transaction ID string (copied internally)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if resp is NULL
 */
esp_err_t bridge_response_set_transaction(bridge_response_t *resp, const char *txn_id);

/**
 * @brief Convert response to JSON string
 *
 * Builds a JSON string from the response structure following the
 * Zigbee2MQTT format:
 * - "data": response data object (empty object if NULL)
 * - "status": "ok" or "error"
 * - "error": error message (only if status="error")
 * - "transaction": transaction ID (only if set)
 *
 * @param[in] resp Pointer to response structure
 * @return JSON string (caller must free) or NULL on error
 */
char* bridge_response_to_json(const bridge_response_t *resp);

/**
 * @brief Publish response to MQTT topic
 *
 * Builds JSON from response and publishes to specified topic.
 * Handles all JSON serialization and memory management.
 *
 * @param[in] response_topic MQTT topic to publish to
 * @param[in] resp Pointer to response structure
 * @return ESP_OK on success, ESP_FAIL on publish error
 */
esp_err_t bridge_response_publish(const char *response_topic, const bridge_response_t *resp);

/**
 * @brief Free response structure resources
 *
 * Frees any allocated resources in the response structure.
 * Does NOT free the resp pointer itself, only internal allocations.
 * The data cJSON object is freed if not NULL.
 *
 * @param[in,out] resp Pointer to response structure to free
 */
void bridge_response_free(bridge_response_t *resp);

/**
 * @brief Extract transaction ID from request JSON
 *
 * Parses request JSON and extracts the "transaction" field if present.
 * Returns NULL if the field is not present or parsing fails.
 *
 * @param[in] request Parsed cJSON request object
 * @return Transaction ID string (internal cJSON string, do not free) or NULL
 */
const char* bridge_request_get_transaction(const cJSON *request);

/**
 * @brief Build response topic from request topic
 *
 * Converts a request topic to the corresponding response topic.
 * Example: "zigbee2mqtt/bridge/request/permit_join"
 *       -> "zigbee2mqtt/bridge/response/permit_join"
 *
 * @param[in] request_topic Request topic string
 * @param[out] response_topic Buffer for response topic
 * @param[in] buf_len Size of response_topic buffer
 * @return ESP_OK on success, ESP_ERR_NO_MEM if buffer too small
 */
esp_err_t bridge_response_topic_from_request(const char *request_topic,
                                             char *response_topic,
                                             size_t buf_len);

/**
 * @brief Quick helper to publish OK response
 *
 * Convenience function to publish a simple OK response with data.
 * Handles all initialization and cleanup internally.
 *
 * @param[in] response_topic MQTT topic to publish to
 * @param[in] data cJSON data object (ownership NOT transferred, object is copied)
 * @param[in] transaction_id Transaction ID (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t bridge_response_publish_ok(const char *response_topic,
                                     cJSON *data,
                                     const char *transaction_id);

/**
 * @brief Quick helper to publish error response
 *
 * Convenience function to publish a simple error response.
 * Handles all initialization and cleanup internally.
 *
 * @param[in] response_topic MQTT topic to publish to
 * @param[in] error_message Error message string
 * @param[in] transaction_id Transaction ID (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t bridge_response_publish_error(const char *response_topic,
                                        const char *error_message,
                                        const char *transaction_id);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_RESPONSE_H */
