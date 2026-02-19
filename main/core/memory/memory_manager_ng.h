/**
 * @file memory_manager_ng.h
 * @brief Next-Gen Memory Management with Auto-PSRAM and Buffer Pools
 *
 * Provides centralized memory allocation with:
 * - Automatic PSRAM usage for large allocations (>1KB)
 * - Buffer pools for frequently allocated sizes
 * - Memory statistics and watermark tracking
 * - Low-memory callbacks
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef MEMORY_MANAGER_NG_H
#define MEMORY_MANAGER_NG_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** @brief Threshold for automatic PSRAM allocation (bytes) */
#define MEM_PSRAM_THRESHOLD         1024

/** @brief Minimum free heap to maintain (bytes) */
#define MEM_MIN_FREE_HEAP           (20 * 1024)

/** @brief Maximum number of buffer pools */
#define MEM_MAX_POOLS               8

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * @brief Memory allocation capabilities
 */
typedef enum {
    MEM_CAP_DEFAULT = 0,        /**< Auto-select based on size */
    MEM_CAP_INTERNAL,           /**< Force internal SRAM */
    MEM_CAP_PSRAM,              /**< Force PSRAM */
    MEM_CAP_DMA,                /**< DMA-capable memory */
    MEM_CAP_EXEC,               /**< Executable memory (IRAM) */
} mem_caps_t;

/**
 * @brief Memory statistics
 */
typedef struct {
    size_t total_internal;      /**< Total internal heap */
    size_t free_internal;       /**< Free internal heap */
    size_t min_free_internal;   /**< Minimum free internal (watermark) */
    size_t total_psram;         /**< Total PSRAM */
    size_t free_psram;          /**< Free PSRAM */
    size_t min_free_psram;      /**< Minimum free PSRAM (watermark) */
    size_t largest_free_block;  /**< Largest contiguous free block */
    uint32_t alloc_count;       /**< Total allocations since init */
    uint32_t free_count;        /**< Total frees since init */
    uint32_t pool_hits;         /**< Allocations served from pools */
    uint32_t pool_misses;       /**< Allocations that missed pools */
} mem_stats_t;

/**
 * @brief Buffer pool handle
 */
typedef struct buffer_pool *buffer_pool_t;

/**
 * @brief Low memory callback
 * @param free_heap Current free heap
 * @param user_ctx User context
 */
typedef void (*mem_low_callback_t)(size_t free_heap, void *user_ctx);

/* ============================================================================
 * Core Memory API
 * ============================================================================ */

/**
 * @brief Initialize memory manager
 * @return ESP_OK on success
 */
esp_err_t mem_manager_init(void);

/**
 * @brief Allocate memory with automatic PSRAM selection
 *
 * Allocates from PSRAM if size > MEM_PSRAM_THRESHOLD and PSRAM available,
 * otherwise from internal heap.
 *
 * @param size Allocation size
 * @param caps Memory capabilities (MEM_CAP_DEFAULT for auto)
 * @return Pointer to allocated memory, or NULL on failure
 */
void *mem_alloc(size_t size, mem_caps_t caps);

/**
 * @brief Allocate zeroed memory
 * @param count Number of elements
 * @param size Size of each element
 * @param caps Memory capabilities
 * @return Pointer to zeroed memory, or NULL on failure
 */
void *mem_ng_calloc(size_t count, size_t size, mem_caps_t caps);

/**
 * @brief Reallocate memory
 * @param ptr Existing pointer (can be NULL)
 * @param size New size
 * @param caps Memory capabilities
 * @return Pointer to reallocated memory, or NULL on failure
 */
void *mem_realloc(void *ptr, size_t size, mem_caps_t caps);

/**
 * @brief Free allocated memory
 * @param ptr Pointer to free (NULL safe)
 */
void mem_ng_free(void *ptr);

/**
 * @brief Duplicate string to memory
 * @param str String to duplicate
 * @param caps Memory capabilities
 * @return Duplicated string, or NULL on failure
 */
char *mem_strdup(const char *str, mem_caps_t caps);

/* ============================================================================
 * Buffer Pool API
 * ============================================================================ */

/**
 * @brief Create a buffer pool
 *
 * Pre-allocates a fixed number of same-size buffers for fast allocation.
 *
 * @param name Pool name (for debugging)
 * @param buffer_size Size of each buffer
 * @param count Number of buffers
 * @param caps Memory capabilities for buffers
 * @return Pool handle, or NULL on failure
 */
buffer_pool_t mem_pool_create(const char *name, size_t buffer_size,
                               size_t count, mem_caps_t caps);

/**
 * @brief Destroy a buffer pool
 * @param pool Pool handle
 */
void mem_pool_destroy(buffer_pool_t pool);

/**
 * @brief Acquire a buffer from pool
 *
 * Fast O(1) allocation if buffers available.
 *
 * @param pool Pool handle
 * @return Buffer pointer, or NULL if pool exhausted
 */
void *mem_pool_acquire(buffer_pool_t pool);

/**
 * @brief Release a buffer back to pool
 * @param pool Pool handle
 * @param buffer Buffer to release
 */
void mem_pool_release(buffer_pool_t pool, void *buffer);

/**
 * @brief Get pool statistics
 * @param pool Pool handle
 * @param total Output: total buffers
 * @param available Output: available buffers
 */
void mem_pool_stats(buffer_pool_t pool, size_t *total, size_t *available);

/* ============================================================================
 * Statistics & Monitoring
 * ============================================================================ */

/**
 * @brief Get current memory statistics
 * @param stats Output statistics structure
 */
void mem_get_stats(mem_stats_t *stats);

/**
 * @brief Get free heap (internal + PSRAM)
 * @return Total free heap in bytes
 */
size_t mem_get_free(void);

/**
 * @brief Get free internal heap only
 * @return Free internal heap in bytes
 */
size_t mem_get_free_internal(void);

/**
 * @brief Check if memory is low
 * @return true if free heap < MEM_MIN_FREE_HEAP
 */
bool mem_is_low(void);

/**
 * @brief Register low-memory callback
 *
 * Callback is invoked when free heap drops below threshold.
 *
 * @param callback Callback function
 * @param threshold Free heap threshold in bytes
 * @param user_ctx User context passed to callback
 * @return ESP_OK on success
 */
esp_err_t mem_register_low_callback(mem_low_callback_t callback,
                                     size_t threshold, void *user_ctx);

/**
 * @brief Print memory statistics to log
 */
void mem_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_MANAGER_NG_H */
