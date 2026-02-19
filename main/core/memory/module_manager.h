/**
 * @file module_manager.h
 * @brief Dynamic Module Loading/Unloading for Memory-Constrained Devices
 *
 * Provides dynamic module lifecycle management with:
 * - Module registration and state tracking
 * - Heap-aware loading (checks available memory before load)
 * - Priority-based unloading for memory pressure scenarios
 * - Essential module protection (never unload core modules)
 * - Thread-safe operations
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef MODULE_MANAGER_H
#define MODULE_MANAGER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** @brief Maximum number of registered modules */
#define MODULE_MAX_COUNT            16

/** @brief Maximum length of module name (including null terminator) */
#define MODULE_NAME_MAX_LEN         32

/** @brief Default memory threshold for auto-unload (bytes) */
#define MODULE_DEFAULT_MEM_THRESHOLD    (20 * 1024)

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * @brief Module lifecycle states
 */
typedef enum {
    MODULE_UNLOADED = 0,    /**< Module is not loaded */
    MODULE_LOADING,         /**< Module is currently loading */
    MODULE_LOADED,          /**< Module is loaded and operational */
    MODULE_UNLOADING,       /**< Module is currently unloading */
    MODULE_ERROR,           /**< Module encountered an error */
} module_state_t;

/**
 * @brief Module definition structure
 *
 * Defines a module's properties and callbacks for lifecycle management.
 */
typedef struct {
    const char *name;           /**< Module name (must be unique) */
    size_t heap_required;       /**< Minimum heap needed to load (bytes) */
    size_t heap_usage;          /**< Actual heap usage when loaded (bytes) */
    uint8_t priority;           /**< Unload priority (lower = unload first) */
    bool essential;             /**< If true, module is never unloaded */
    module_state_t state;       /**< Current module state */

    /**
     * @brief Load callback
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t (*load)(void);

    /**
     * @brief Unload callback
     * @return ESP_OK on success, error code on failure
     */
    esp_err_t (*unload)(void);

    /**
     * @brief Check if module can be safely unloaded
     * @return true if safe to unload, false otherwise
     */
    bool (*can_unload)(void);
} module_def_t;

/**
 * @brief Module iterator callback
 * @param def Module definition (read-only)
 * @param user_ctx User context
 * @return true to continue iteration, false to stop
 */
typedef bool (*module_iterator_fn)(const module_def_t *def, void *user_ctx);

/* ============================================================================
 * Module Manager API
 * ============================================================================ */

/**
 * @brief Initialize the module manager
 *
 * Must be called before any other module manager functions.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if mutex creation fails
 */
esp_err_t module_manager_init(void);

/**
 * @brief Deinitialize the module manager
 *
 * Unloads all modules and releases resources.
 */
void module_manager_deinit(void);

/**
 * @brief Register a module with the manager
 *
 * @param def Module definition (copied internally)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if def is NULL or invalid
 * @return ESP_ERR_INVALID_STATE if manager not initialized
 * @return ESP_ERR_NO_MEM if registry is full
 * @return ESP_ERR_INVALID_STATE if module name already registered
 */
esp_err_t module_register(const module_def_t *def);

/**
 * @brief Unregister a module from the manager
 *
 * Module must be unloaded before unregistering.
 *
 * @param name Module name
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if module not found
 * @return ESP_ERR_INVALID_STATE if module is still loaded
 */
esp_err_t module_unregister(const char *name);

/**
 * @brief Load a module
 *
 * Checks heap availability, calls load callback, and updates state.
 *
 * @param name Module name
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if module not found
 * @return ESP_ERR_INVALID_STATE if module is already loaded or loading
 * @return ESP_ERR_NO_MEM if insufficient heap
 * @return Other errors from load callback
 */
esp_err_t module_load(const char *name);

/**
 * @brief Unload a module
 *
 * Checks can_unload callback, calls unload callback, and updates state.
 *
 * @param name Module name
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if module not found
 * @return ESP_ERR_INVALID_STATE if module is not loaded or is essential
 * @return ESP_ERR_NOT_ALLOWED if can_unload returns false
 * @return Other errors from unload callback
 */
esp_err_t module_unload(const char *name);

/**
 * @brief Ensure a module is loaded
 *
 * Loads the module if not already loaded. No-op if already loaded.
 *
 * @param name Module name
 * @return ESP_OK on success (or if already loaded)
 * @return Other errors from module_load
 */
esp_err_t module_ensure_loaded(const char *name);

/**
 * @brief Get the current state of a module
 *
 * @param name Module name
 * @return Module state, or MODULE_ERROR if not found
 */
module_state_t module_get_state(const char *name);

/**
 * @brief Set memory threshold for auto-unload consideration
 *
 * When free heap drops below this threshold, non-essential modules
 * may be unloaded based on priority.
 *
 * @param min_free_heap Minimum free heap threshold (bytes)
 */
void module_manager_set_memory_threshold(size_t min_free_heap);

/**
 * @brief Get current memory threshold
 *
 * @return Current memory threshold in bytes
 */
size_t module_manager_get_memory_threshold(void);

/* ============================================================================
 * Query & Iteration API
 * ============================================================================ */

/**
 * @brief Get number of registered modules
 *
 * @return Number of registered modules
 */
size_t module_get_count(void);

/**
 * @brief Get number of loaded modules
 *
 * @return Number of modules in MODULE_LOADED state
 */
size_t module_get_loaded_count(void);

/**
 * @brief Get module definition by name
 *
 * @param name Module name
 * @return Pointer to module definition, or NULL if not found
 * @note The returned pointer is valid until module is unregistered
 */
const module_def_t *module_get_def(const char *name);

/**
 * @brief Iterate over all registered modules
 *
 * @param callback Iterator callback
 * @param user_ctx User context passed to callback
 */
void module_iterate(module_iterator_fn callback, void *user_ctx);

/**
 * @brief Find modules that can be unloaded
 *
 * Returns modules sorted by unload priority (lowest first).
 *
 * @param names Output array of module names
 * @param max_count Maximum entries in names array
 * @return Number of unloadable modules found
 */
size_t module_find_unloadable(const char **names, size_t max_count);

/**
 * @brief Unload modules by priority until heap threshold is met
 *
 * Unloads non-essential modules starting from lowest priority
 * until free heap exceeds the memory threshold or no more
 * modules can be unloaded.
 *
 * @return Number of modules unloaded
 */
size_t module_unload_by_priority(void);

/**
 * @brief Print module status to log
 */
void module_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_MANAGER_H */
