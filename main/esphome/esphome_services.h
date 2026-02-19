/**
 * @file esphome_services.h
 * @brief ESPHome API Service Calls
 *
 * Provides custom service registration and execution for Home Assistant.
 * Services allow Home Assistant to call custom functions on the device,
 * enabling advanced automation scenarios and device control.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_SERVICES_H
#define ESPHOME_SERVICES_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esphome_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Service Argument Definitions
 * ============================================================================ */

/**
 * @brief Service argument definition
 *
 * Defines a single argument that a service accepts.
 * Arguments are passed by Home Assistant when invoking the service.
 */
typedef struct {
    char name[32];                      /**< Argument name (used in HA UI) */
    esphome_service_arg_type_t type;    /**< Argument data type */
} esphome_service_arg_def_t;

/**
 * @brief Service argument value
 *
 * Holds the actual value of a service argument passed during execution.
 * The union member used depends on the type field.
 */
typedef struct {
    esphome_service_arg_type_t type;    /**< Argument data type */
    union {
        bool bool_value;                /**< Boolean argument value */
        int32_t int_value;              /**< Integer argument value */
        float float_value;              /**< Float argument value */
        char string_value[128];         /**< String argument value */
    };
} esphome_service_arg_value_t;

/* ============================================================================
 * Service Callback Type
 * ============================================================================ */

/**
 * @brief Service execution callback type
 *
 * Called when a service is invoked from Home Assistant.
 *
 * @param[in] args      Array of argument values
 * @param[in] arg_count Number of arguments in the array
 * @param[in] user_data User data provided during service registration
 * @return ESP_OK if service executed successfully
 * @return ESP_ERR_INVALID_ARG if arguments are invalid
 * @return Other error codes as appropriate
 */
typedef esp_err_t (*esphome_service_cb_t)(const esphome_service_arg_value_t *args,
                                          size_t arg_count, void *user_data);

/* ============================================================================
 * Service Definition
 * ============================================================================ */

/**
 * @brief Service definition structure
 *
 * Defines a custom service that can be called from Home Assistant.
 * Services are exposed through the ESPHome Native API and appear
 * in the Home Assistant services list.
 */
typedef struct {
    char name[64];                                          /**< Service name (used in HA) */
    esphome_service_cb_t callback;                          /**< Execution callback */
    void *user_data;                                        /**< User data for callback */
    esphome_service_arg_def_t args[ESPHOME_MAX_SERVICE_ARGS]; /**< Argument definitions */
    size_t arg_count;                                       /**< Number of arguments */
    uint32_t key;                                           /**< Unique service key */
} esphome_service_t;

/* ============================================================================
 * Service Manager Functions
 * ============================================================================ */

/**
 * @brief Initialize the service manager
 *
 * Must be called before registering any services.
 * Allocates internal data structures for service management.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if memory allocation failed
 */
esp_err_t esphome_services_init(void);

/**
 * @brief Deinitialize the service manager
 *
 * Frees all resources and unregisters all services.
 * Should be called during shutdown or module cleanup.
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_services_deinit(void);

/* ============================================================================
 * Service Registration
 * ============================================================================ */

/**
 * @brief Register a new service
 *
 * Registers a custom service that can be called from Home Assistant.
 * The service will appear in the Home Assistant services list after
 * the next entity list request.
 *
 * @param[in] service Service definition to register
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if service is NULL or has invalid fields
 * @return ESP_ERR_NO_MEM if maximum services reached (ESPHOME_MAX_SERVICES)
 * @return ESP_ERR_INVALID_STATE if service manager not initialized
 */
esp_err_t esphome_service_register(const esphome_service_t *service);

/**
 * @brief Unregister a service by name
 *
 * Removes a previously registered service. The service will no longer
 * appear in Home Assistant after the next entity list request.
 *
 * @param[in] name Service name to unregister
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if name is NULL
 * @return ESPHOME_ERR_ENTITY_NOT_FOUND if service not found
 */
esp_err_t esphome_service_unregister(const char *name);

/* ============================================================================
 * Service Execution
 * ============================================================================ */

/**
 * @brief Execute a service by key
 *
 * Called when a service execution request is received from Home Assistant.
 * Validates arguments and invokes the registered callback.
 *
 * @param[in] key       Service unique key
 * @param[in] args      Array of argument values
 * @param[in] arg_count Number of arguments provided
 * @return ESP_OK on success
 * @return ESPHOME_ERR_ENTITY_NOT_FOUND if service not found
 * @return ESP_ERR_INVALID_ARG if argument count or types mismatch
 * @return Error code from service callback on execution failure
 */
esp_err_t esphome_service_execute(uint32_t key, const esphome_service_arg_value_t *args,
                                   size_t arg_count);

/* ============================================================================
 * Service Query Functions
 * ============================================================================ */

/**
 * @brief Get the number of registered services
 *
 * @return Number of currently registered services
 */
size_t esphome_service_get_count(void);

/**
 * @brief Get a service by index
 *
 * Retrieves service definition by array index. Useful for iterating
 * through all registered services.
 *
 * @param[in] index Service index (0 to count-1)
 * @return Pointer to service definition, or NULL if index out of range
 */
const esphome_service_t *esphome_service_get_by_index(size_t index);

/**
 * @brief Get a service by key
 *
 * Retrieves service definition by unique key.
 *
 * @param[in] key Service unique key
 * @return Pointer to service definition, or NULL if not found
 */
const esphome_service_t *esphome_service_get_by_key(uint32_t key);

/* ============================================================================
 * Service Enumeration
 * ============================================================================ */

/**
 * @brief Callback for service enumeration
 *
 * @param[in] service   Pointer to service definition
 * @param[in] user_data User data passed to enumerate function
 * @return true to continue enumeration, false to stop
 */
typedef bool (*esphome_service_enum_cb_t)(const esphome_service_t *service, void *user_data);

/**
 * @brief Enumerate all registered services
 *
 * Calls the callback for each registered service. Enumeration stops
 * when all services have been visited or callback returns false.
 *
 * @param[in] callback  Enumeration callback function
 * @param[in] user_data User data passed to callback
 */
void esphome_services_enumerate(esphome_service_enum_cb_t callback, void *user_data);

/* ============================================================================
 * Message Encoding Helpers
 * ============================================================================ */

/**
 * @brief Encode service definition for ListServices response
 *
 * Encodes a service definition in the ESPHome Native API protocol format
 * for transmission to Home Assistant.
 *
 * @param[in]  service     Service definition to encode
 * @param[out] output      Output buffer for encoded message
 * @param[in]  output_size Size of output buffer
 * @param[out] output_len  Actual encoded message length
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if service or output is NULL
 * @return ESPHOME_ERR_BUFFER_OVERFLOW if output buffer too small
 */
esp_err_t esphome_encode_service_list_entry(const esphome_service_t *service,
                                             uint8_t *output, size_t output_size,
                                             size_t *output_len);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_SERVICES_H */
