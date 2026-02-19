/**
 * @file string_intern.h
 * @brief String Interning Module for Memory Optimization
 *
 * Provides string interning to save memory for frequently repeated strings.
 * In embedded systems, strings like manufacturer names, model names, and
 * state keys repeat often. String interning stores unique strings once and
 * returns pointers to them, significantly reducing memory usage.
 *
 * Features:
 * - Hash table for O(1) lookup
 * - Contiguous string pool in PSRAM
 * - Pre-populated common strings
 * - Memory savings tracking
 * - Thread-safe with mutex
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef STRING_INTERN_H
#define STRING_INTERN_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** @brief Default maximum number of interned strings */
#define STRING_INTERN_DEFAULT_MAX_STRINGS   256

/** @brief Default string pool size in bytes */
#define STRING_INTERN_DEFAULT_POOL_SIZE     (8 * 1024)

/** @brief Maximum string length that can be interned */
#define STRING_INTERN_MAX_LENGTH            128

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * @brief String interning statistics
 */
typedef struct {
    size_t strings_count;   /**< Number of unique strings interned */
    size_t max_strings;     /**< Maximum strings capacity */
    size_t bytes_used;      /**< Bytes used in string pool */
    size_t bytes_saved;     /**< Bytes saved compared to duplicates */
    size_t pool_size;       /**< Total pool size */
} string_intern_stats_t;

/* ============================================================================
 * Initialization & Cleanup
 * ============================================================================ */

/**
 * @brief Initialize the string interning module
 *
 * Allocates hash table and string pool. Pre-populates with common strings
 * for manufacturers, state keys, and device classes.
 *
 * @param max_strings Maximum number of unique strings to store
 * @param pool_size Size of the string pool in bytes (allocated in PSRAM)
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if already initialized
 *     - ESP_ERR_NO_MEM if allocation fails
 *     - ESP_ERR_INVALID_ARG if parameters are invalid
 */
esp_err_t string_intern_init(size_t max_strings, size_t pool_size);

/**
 * @brief Deinitialize the string interning module
 *
 * Frees all allocated memory. All previously interned string pointers
 * become invalid after this call.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t string_intern_deinit(void);

/* ============================================================================
 * Core API
 * ============================================================================ */

/**
 * @brief Get an interned string
 *
 * Returns a pointer to the interned version of the string. If the string
 * is not yet interned, it will be added to the pool. If the string is
 * already interned, the existing pointer is returned.
 *
 * The returned pointer is valid until string_intern_deinit() is called.
 *
 * @param str String to intern (must not be NULL)
 * @return
 *     - Pointer to interned string on success
 *     - NULL if str is NULL, too long, or pool is full
 */
const char *string_intern(const char *str);

/**
 * @brief Check if a string is interned
 *
 * Checks whether a string exists in the intern pool.
 *
 * @param str String to check
 * @return true if the string is interned, false otherwise
 */
bool string_is_interned(const char *str);

/**
 * @brief Check if a pointer points to an interned string
 *
 * Checks whether a pointer points within the string pool.
 * This is useful to determine if a string needs to be freed or not.
 *
 * @param ptr Pointer to check
 * @return true if ptr points to an interned string, false otherwise
 */
bool string_ptr_is_interned(const char *ptr);

/* ============================================================================
 * Statistics & Debugging
 * ============================================================================ */

/**
 * @brief Get string interning statistics
 *
 * @param stats Output structure for statistics (must not be NULL)
 */
void string_intern_get_stats(string_intern_stats_t *stats);

/**
 * @brief Print statistics to log
 */
void string_intern_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* STRING_INTERN_H */
