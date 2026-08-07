/**
 * @file esphome_services.c
 * @brief ESPHome API Service Calls Implementation
 *
 * Implements service registration, execution, and enumeration for the
 * ESPHome Native API. Services allow Home Assistant to trigger custom
 * actions on the device.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_services.h"
#include "core/memory/memory_manager_ng.h"
#include "esphome_protocol.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

/* Log tag */
static const char *TAG = "ESPHOME_SVC";

/* ============================================================================
 * Internal Storage Structures
 * ============================================================================ */

/**
 * @brief Service storage entry
 */
typedef struct {
    esphome_service_t service;
    bool registered;
} service_entry_t;

/* ============================================================================
 * Module State
 * ============================================================================ */

static struct {
    /* Allocated at init, in PSRAM. Just under 6 KB of internal RAM otherwise,
     * for a table that is only ever read and written from task context. Only
     * the array moved — the surrounding struct stays static so every existing
     * s_services.x access is unchanged and nothing can observe it as null. */
    service_entry_t *services;
    size_t service_count;
    SemaphoreHandle_t mutex;
    bool initialized;
} s_services = {
    .service_count = 0,
    .initialized = false,
};

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * @brief FNV-1a hash function for generating unique keys from service names
 *
 * @param[in] str String to hash
 * @return 32-bit hash value
 */
static uint32_t fnv1a_hash(const char *str)
{
    uint32_t hash = 2166136261u;  /* FNV offset basis */
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;  /* FNV prime */
    }
    return hash;
}

/**
 * @brief Find service by key (internal, no lock)
 *
 * @param[in] key Service key
 * @return Pointer to service entry or NULL if not found
 */
static service_entry_t *find_service_by_key_internal(uint32_t key)
{
    for (size_t i = 0; i < s_services.service_count; i++) {
        if (s_services.services[i].registered && s_services.services[i].service.key == key) {
            return &s_services.services[i];
        }
    }
    return NULL;
}

/**
 * @brief Find service by name (internal, no lock)
 *
 * @param[in] name Service name
 * @return Pointer to service entry or NULL if not found
 */
static service_entry_t *find_service_by_name_internal(const char *name)
{
    for (size_t i = 0; i < s_services.service_count; i++) {
        if (s_services.services[i].registered &&
            strcmp(s_services.services[i].service.name, name) == 0) {
            return &s_services.services[i];
        }
    }
    return NULL;
}

/**
 * @brief Find free slot in services array (internal, no lock)
 *
 * @return Index of free slot, or -1 if full
 */
static int find_free_slot(void)
{
    /* First, try to find an unregistered slot */
    for (size_t i = 0; i < s_services.service_count; i++) {
        if (!s_services.services[i].registered) {
            return (int)i;
        }
    }

    /* If no unregistered slots, use next available if under limit */
    if (s_services.service_count < ESPHOME_MAX_SERVICES) {
        return (int)s_services.service_count;
    }

    return -1;
}

/* ============================================================================
 * Service Manager Functions
 * ============================================================================ */

/**
 * @brief Initialize the service manager
 */
esp_err_t esphome_services_init(void)
{
    if (s_services.initialized) {
        ESP_LOGW(TAG, "Service manager already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing service manager...");

    /* Create mutex */
    s_services.mutex = xSemaphoreCreateMutex();
    if (!s_services.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate storage */
    s_services.services = mem_ng_calloc(ESPHOME_MAX_SERVICES, sizeof(service_entry_t),
                                        MEM_CAP_PSRAM);
    if (!s_services.services) {
        ESP_LOGE(TAG, "Failed to allocate %d service slot(s)", ESPHOME_MAX_SERVICES);
        vSemaphoreDelete(s_services.mutex);
        s_services.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_services.service_count = 0;
    s_services.initialized = true;

    ESP_LOGI(TAG, "Service manager initialized (max: %d services)", ESPHOME_MAX_SERVICES);

    return ESP_OK;
}

/**
 * @brief Deinitialize the service manager
 */
esp_err_t esphome_services_deinit(void)
{
    if (!s_services.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing service manager...");

    if (s_services.mutex) {
        vSemaphoreDelete(s_services.mutex);
        s_services.mutex = NULL;
    }

    if (s_services.services) {
        mem_ng_free(s_services.services);
        s_services.services = NULL;
    }
    s_services.service_count = 0;
    s_services.initialized = false;

    return ESP_OK;
}

/* ============================================================================
 * Service Registration
 * ============================================================================ */

/**
 * @brief Register a new service
 */
esp_err_t esphome_service_register(const esphome_service_t *service)
{
    if (!service) {
        return ESP_ERR_INVALID_ARG;
    }

    if (service->name[0] == '\0') {
        ESP_LOGE(TAG, "Service name cannot be empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (service->arg_count > ESPHOME_MAX_SERVICE_ARGS) {
        ESP_LOGE(TAG, "Too many arguments (%zu, max %d)", service->arg_count,
                 ESPHOME_MAX_SERVICE_ARGS);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_services.initialized) {
        ESP_LOGE(TAG, "Service manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_services.mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Generate key if not provided */
    uint32_t key = service->key;
    if (key == 0) {
        key = fnv1a_hash(service->name);
    }

    /* Check for duplicate key */
    if (find_service_by_key_internal(key)) {
        ESP_LOGW(TAG, "Service with key %lu already exists", (unsigned long)key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check for duplicate name */
    if (find_service_by_name_internal(service->name)) {
        ESP_LOGW(TAG, "Service with name '%s' already exists", service->name);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Find free slot */
    int slot = find_free_slot();
    if (slot < 0) {
        ESP_LOGE(TAG, "Maximum services reached (%d)", ESPHOME_MAX_SERVICES);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add service */
    memcpy(&s_services.services[slot].service, service, sizeof(esphome_service_t));
    s_services.services[slot].service.key = key;  /* Ensure key is set */
    s_services.services[slot].registered = true;

    if ((size_t)slot >= s_services.service_count) {
        s_services.service_count = (size_t)slot + 1;
    }

    ESP_LOGI(TAG, "Registered service: key=%lu, name='%s', args=%zu",
             (unsigned long)key, service->name, service->arg_count);

done:
    xSemaphoreGive(s_services.mutex);
    return ret;
}

/**
 * @brief Unregister a service by name
 */
esp_err_t esphome_service_unregister(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_services.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_services.mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    service_entry_t *entry = find_service_by_name_internal(name);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    ESP_LOGI(TAG, "Unregistering service: key=%lu, name='%s'",
             (unsigned long)entry->service.key, entry->service.name);

    entry->registered = false;
    memset(&entry->service, 0, sizeof(esphome_service_t));

done:
    xSemaphoreGive(s_services.mutex);
    return ret;
}

/* ============================================================================
 * Service Execution
 * ============================================================================ */

/**
 * @brief Execute a service by key
 */
esp_err_t esphome_service_execute(uint32_t key, const esphome_service_arg_value_t *args,
                                   size_t arg_count)
{
    if (!s_services.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_services.mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    service_entry_t *entry = find_service_by_key_internal(key);
    esphome_service_cb_t callback = NULL;
    void *user_data = NULL;
    size_t expected_args = 0;
    char service_name[64] = {0};

    if (entry) {
        callback = entry->service.callback;
        user_data = entry->service.user_data;
        expected_args = entry->service.arg_count;
        strncpy(service_name, entry->service.name, sizeof(service_name) - 1);
    }

    xSemaphoreGive(s_services.mutex);

    if (!entry) {
        ESP_LOGW(TAG, "Service not found: key=%lu", (unsigned long)key);
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    /* Validate argument count */
    if (arg_count != expected_args) {
        ESP_LOGW(TAG, "Argument count mismatch for '%s': got %zu, expected %zu",
                 service_name, arg_count, expected_args);
        return ESP_ERR_INVALID_ARG;
    }

    if (!callback) {
        ESP_LOGW(TAG, "Service '%s' has no callback", service_name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Executing service '%s': key=%lu, args=%zu",
             service_name, (unsigned long)key, arg_count);

    return callback(args, arg_count, user_data);
}

/* ============================================================================
 * Service Query Functions
 * ============================================================================ */

/**
 * @brief Get the number of registered services
 */
size_t esphome_service_get_count(void)
{
    size_t count = 0;

    if (!s_services.initialized) {
        return 0;
    }

    if (xSemaphoreTake(s_services.mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
        return 0;
    }

    for (size_t i = 0; i < s_services.service_count; i++) {
        if (s_services.services[i].registered) {
            count++;
        }
    }

    xSemaphoreGive(s_services.mutex);
    return count;
}

/**
 * @brief Get a service by index
 */
const esphome_service_t *esphome_service_get_by_index(size_t index)
{
    const esphome_service_t *result = NULL;

    if (!s_services.initialized) {
        return NULL;
    }

    if (xSemaphoreTake(s_services.mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
        return NULL;
    }

    /* Find the nth registered service */
    size_t current = 0;
    for (size_t i = 0; i < s_services.service_count; i++) {
        if (s_services.services[i].registered) {
            if (current == index) {
                result = &s_services.services[i].service;
                break;
            }
            current++;
        }
    }

    xSemaphoreGive(s_services.mutex);
    return result;
}

/**
 * @brief Get a service by key
 */
const esphome_service_t *esphome_service_get_by_key(uint32_t key)
{
    const esphome_service_t *result = NULL;

    if (!s_services.initialized) {
        return NULL;
    }

    if (xSemaphoreTake(s_services.mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
        return NULL;
    }

    service_entry_t *entry = find_service_by_key_internal(key);
    if (entry) {
        result = &entry->service;
    }

    xSemaphoreGive(s_services.mutex);
    return result;
}

/* ============================================================================
 * Service Enumeration
 * ============================================================================ */

/**
 * @brief Enumerate all registered services
 */
void esphome_services_enumerate(esphome_service_enum_cb_t callback, void *user_data)
{
    if (!callback || !s_services.initialized) {
        return;
    }

    if (xSemaphoreTake(s_services.mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
        return;
    }

    for (size_t i = 0; i < s_services.service_count; i++) {
        if (s_services.services[i].registered) {
            if (!callback(&s_services.services[i].service, user_data)) {
                break;
            }
        }
    }

    xSemaphoreGive(s_services.mutex);
}

/* ============================================================================
 * Message Encoding Helpers
 * ============================================================================ */

/**
 * @brief Encode a single service argument as a submessage
 *
 * ESPHome service argument format:
 * - Field 1: name (string)
 * - Field 2: type (uint32)
 *
 * @param[in,out] buf Buffer to write to
 * @param[in] field_number Field number for the argument submessage
 * @param[in] arg Argument definition
 * @return ESP_OK on success
 */
static esp_err_t encode_service_arg(esphome_buffer_t *buf, uint32_t field_number,
                                    const esphome_service_arg_def_t *arg)
{
    /* Encode submessage: field 1 = name, field 2 = type */
    uint8_t submsg[64];
    esphome_buffer_t sub_buf;
    esphome_buffer_init(&sub_buf, submsg, sizeof(submsg));

    /* Field 1: name (string) */
    esphome_encode_string(&sub_buf, 1, arg->name);

    /* Field 2: type (uint32) */
    esphome_encode_uint32(&sub_buf, 2, (uint32_t)arg->type);

    if (esphome_buffer_overflow(&sub_buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    /* Encode the submessage as length-delimited bytes */
    return esphome_encode_bytes(buf, field_number, submsg, sub_buf.position);
}

/**
 * @brief Encode service definition for ListServices response
 *
 * ESPHome ListServicesResponse message format:
 * - Field 1: name (string)
 * - Field 2: key (fixed32)
 * - Field 3: args (repeated submessage)
 */
esp_err_t esphome_encode_service_list_entry(const esphome_service_t *service,
                                             uint8_t *output, size_t output_size,
                                             size_t *output_len)
{
    if (!service || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_SERVICE_PAYLOAD_SIZE];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: name (string) */
    esphome_encode_string(&buf, 1, service->name);

    /* Field 2: key (fixed32) */
    esphome_encode_fixed32(&buf, 2, service->key);

    /* Field 3: args (repeated submessage) */
    for (size_t i = 0; i < service->arg_count; i++) {
        esp_err_t ret = encode_service_arg(&buf, 3, &service->args[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_SERVICES_RESPONSE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Testing
 * ============================================================================ */

/**
 * @brief Test service callback for testing
 */
static esp_err_t test_service_callback(const esphome_service_arg_value_t *args,
                                        size_t arg_count, void *user_data)
{
    ESP_LOGI(TAG, "Test service executed, arg_count=%zu, user_data=%p",
             arg_count, user_data);

    for (size_t i = 0; i < arg_count; i++) {
        switch (args[i].type) {
            case ESPHOME_SERVICE_ARG_BOOL:
                ESP_LOGI(TAG, "  Arg %zu: bool = %s", i, args[i].bool_value ? "true" : "false");
                break;
            case ESPHOME_SERVICE_ARG_INT:
                ESP_LOGI(TAG, "  Arg %zu: int = %ld", i, (long)args[i].int_value);
                break;
            case ESPHOME_SERVICE_ARG_FLOAT:
                ESP_LOGI(TAG, "  Arg %zu: float = %.2f", i, (double)args[i].float_value);
                break;
            case ESPHOME_SERVICE_ARG_STRING:
                ESP_LOGI(TAG, "  Arg %zu: string = '%s'", i, args[i].string_value);
                break;
            default:
                ESP_LOGI(TAG, "  Arg %zu: unknown type %d", i, args[i].type);
                break;
        }
    }

    return ESP_OK;
}

/**
 * @brief Enumeration callback for testing
 */
static bool test_enum_callback(const esphome_service_t *service, void *user_data)
{
    size_t *count = (size_t *)user_data;
    (*count)++;
    ESP_LOGI(TAG, "  Enumerated service: '%s' (key=%lu)",
             service->name, (unsigned long)service->key);
    return true;
}

/**
 * @brief Test service management
 */
esp_err_t esphome_services_test(void)
{
    ESP_LOGI(TAG, "=== ESPHome Services Test ===");

    esp_err_t ret;

    /* Initialize service manager */
    ret = esphome_services_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init failed: %d", ret);
        return ESP_FAIL;
    }

    /* Test service registration with 2 arguments */
    ESP_LOGI(TAG, "Testing service registration...");
    esphome_service_t svc = {
        .name = "test_service",
        .callback = test_service_callback,
        .user_data = (void *)0x12345678,
        .arg_count = 2,
        .key = 0,  /* Auto-generate */
    };
    strncpy(svc.args[0].name, "level", sizeof(svc.args[0].name) - 1);
    svc.args[0].type = ESPHOME_SERVICE_ARG_INT;
    strncpy(svc.args[1].name, "message", sizeof(svc.args[1].name) - 1);
    svc.args[1].type = ESPHOME_SERVICE_ARG_STRING;

    ret = esphome_service_register(&svc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Service registration failed: %d", ret);
        esphome_services_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Service registered successfully");

    /* Verify service count */
    size_t count = esphome_service_get_count();
    if (count != 1) {
        ESP_LOGE(TAG, "Expected 1 service, got %zu", count);
        esphome_services_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Service count: %zu", count);

    /* Test finding service by index */
    ESP_LOGI(TAG, "Testing get by index...");
    const esphome_service_t *found = esphome_service_get_by_index(0);
    if (!found) {
        ESP_LOGE(TAG, "Get by index failed");
        esphome_services_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Found service: key=%lu, name='%s', args=%zu",
             (unsigned long)found->key, found->name, found->arg_count);

    /* Verify generated key */
    if (found->key == 0) {
        ESP_LOGE(TAG, "Key should have been auto-generated");
        esphome_services_deinit();
        return ESP_FAIL;
    }

    /* Test finding service by key */
    ESP_LOGI(TAG, "Testing get by key...");
    const esphome_service_t *found_by_key = esphome_service_get_by_key(found->key);
    if (!found_by_key) {
        ESP_LOGE(TAG, "Get by key failed");
        esphome_services_deinit();
        return ESP_FAIL;
    }
    if (strcmp(found_by_key->name, "test_service") != 0) {
        ESP_LOGE(TAG, "Service name mismatch");
        esphome_services_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Get by key successful");

    /* Test service execution */
    ESP_LOGI(TAG, "Testing service execution...");
    esphome_service_arg_value_t args[2];
    args[0].type = ESPHOME_SERVICE_ARG_INT;
    args[0].int_value = 42;
    args[1].type = ESPHOME_SERVICE_ARG_STRING;
    strncpy(args[1].string_value, "Hello, World!", sizeof(args[1].string_value) - 1);

    ret = esphome_service_execute(found->key, args, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Service execution failed: %d", ret);
        esphome_services_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Service execution successful");

    /* Test message encoding */
    ESP_LOGI(TAG, "Testing message encoding...");
    uint8_t output[ESPHOME_SERVICE_OUTPUT_SIZE];
    size_t output_len;

    ret = esphome_encode_service_list_entry(found, output, sizeof(output), &output_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Service list entry encode failed: %d", ret);
        esphome_services_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Service list entry encoded: %zu bytes", output_len);

    /* Test enumeration */
    ESP_LOGI(TAG, "Testing enumeration...");
    size_t enum_count = 0;
    esphome_services_enumerate(test_enum_callback, &enum_count);
    if (enum_count != 1) {
        ESP_LOGE(TAG, "Enumeration count mismatch: expected 1, got %zu", enum_count);
        esphome_services_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Enumeration successful");

    /* Test unregistration */
    ESP_LOGI(TAG, "Testing service unregistration...");
    ret = esphome_service_unregister("test_service");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Service unregistration failed: %d", ret);
        esphome_services_deinit();
        return ESP_FAIL;
    }

    count = esphome_service_get_count();
    if (count != 0) {
        ESP_LOGE(TAG, "Expected 0 services after unregister, got %zu", count);
        esphome_services_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Service unregistration successful");

    /* Cleanup */
    esphome_services_deinit();

    ESP_LOGI(TAG, "=== All Service Tests PASSED ===");
    return ESP_OK;
}
