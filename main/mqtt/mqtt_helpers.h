/**
 * @file mqtt_helpers.h
 * @brief MQTT Helper Macros and Utilities
 *
 * Provides common helper macros for MQTT operations to reduce code duplication
 * and enforce consistent error handling patterns across the codebase.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MQTT_HELPERS_H
#define MQTT_HELPERS_H

#include "gateway_mqtt.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Guard macro to check MQTT connection before operations
 *
 * This macro checks if the MQTT client is connected and returns early
 * with the specified error value if not connected. It also logs a warning
 * message using the TAG defined in the calling module.
 *
 * Usage:
 *   REQUIRE_MQTT_CONNECTED(ESP_ERR_INVALID_STATE);
 *
 * @note Requires TAG to be defined as a static const char* in the calling file
 *
 * @param error_return Value to return if MQTT is not connected
 */
#define REQUIRE_MQTT_CONNECTED(error_return) \
    do { \
        if (!mqtt_client_is_connected()) { \
            ESP_LOGW(TAG, "MQTT not connected"); \
            return (error_return); \
        } \
    } while(0)

/**
 * @brief Guard macro for void functions - checks MQTT connection before operations
 *
 * This macro checks if the MQTT client is connected and returns early (void)
 * if not connected. It also logs a warning message using the TAG defined
 * in the calling module.
 *
 * Usage:
 *   REQUIRE_MQTT_CONNECTED_VOID();
 *
 * @note Requires TAG to be defined as a static const char* in the calling file
 */
#define REQUIRE_MQTT_CONNECTED_VOID() \
    do { \
        if (!mqtt_client_is_connected()) { \
            ESP_LOGW(TAG, "MQTT not connected"); \
            return; \
        } \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif /* MQTT_HELPERS_H */
