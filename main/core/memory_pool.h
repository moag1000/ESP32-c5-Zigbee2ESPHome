/**
 * @file memory_pool.h
 * @brief Memory Pool for Pre-allocated Buffers
 *
 * Provides fixed-size block memory pools to reduce heap fragmentation
 * for frequently allocated buffers such as MQTT messages and JSON objects.
 *
 * Features:
 * - Pre-allocated blocks for O(1) allocation
 * - Support for both internal RAM and PSRAM
 * - Thread-safe allocation/deallocation
 * - Automatic fallback to heap when pool exhausted
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Memory Pool Constants
 * ============================================================================ */

/** @brief Maximum number of blocks per pool */
#define MEMORY_POOL_MAX_BLOCKS          64

/** @brief MQTT message pool block size (bytes) */
#define MEMORY_POOL_MQTT_BLOCK_SIZE     256

/** @brief MQTT message pool block count */
#define MEMORY_POOL_MQTT_BLOCK_COUNT    32

/** @brief JSON object pool block size (bytes) */
#define MEMORY_POOL_JSON_BLOCK_SIZE     512

/** @brief JSON object pool block count */
#define MEMORY_POOL_JSON_BLOCK_COUNT    16

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * @brief Memory pool configuration
 */
typedef struct {
    size_t block_size;      /**< Size of each block in bytes */
    size_t block_count;     /**< Number of blocks in pool */
    bool use_psram;         /**< Allocate pool in PSRAM if available */
    const char *name;       /**< Pool name for logging */
} memory_pool_config_t;

/**
 * @brief Memory pool statistics
 */
typedef struct {
    size_t total_blocks;        /**< Total blocks in pool */
    size_t free_blocks;         /**< Currently free blocks */
    size_t allocations;         /**< Total allocations from pool */
    size_t heap_fallbacks;      /**< Allocations that fell back to heap */
    size_t block_size;          /**< Size of each block */
    bool use_psram;             /**< Whether pool uses PSRAM */
} memory_pool_stats_t;

/**
 * @brief Memory pool handle (opaque)
 */
typedef struct memory_pool *memory_pool_t;

/* ============================================================================
 * Pool Management Functions
 * ============================================================================ */

/**
 * @brief Initialize a memory pool
 *
 * Creates a new memory pool with the specified configuration.
 * Memory is pre-allocated during initialization.
 *
 * @param[out] pool Pointer to store pool handle
 * @param[in] config Pool configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if memory allocation fails
 * @return ESP_ERR_INVALID_ARG if parameters are invalid
 */
esp_err_t memory_pool_init(memory_pool_t *pool, const memory_pool_config_t *config);

/**
 * @brief Deinitialize a memory pool
 *
 * Frees all pool memory. Any outstanding allocations become invalid.
 *
 * @param[in] pool Pool handle
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if pool is NULL
 */
esp_err_t memory_pool_deinit(memory_pool_t pool);

/* ============================================================================
 * Allocation Functions
 * ============================================================================ */

/**
 * @brief Allocate a block from pool
 *
 * Returns a pre-allocated block from the pool. If pool is exhausted,
 * falls back to heap allocation.
 *
 * @param[in] pool Pool handle
 * @return Pointer to allocated block, or NULL if allocation fails
 *
 * @note Caller must call memory_pool_free() to return the block
 * @note Block is NOT zeroed - use memory_pool_calloc() for zeroed memory
 */
void *memory_pool_alloc(memory_pool_t pool);

/**
 * @brief Allocate and zero a block from pool
 *
 * Same as memory_pool_alloc() but zeroes the returned block.
 *
 * @param[in] pool Pool handle
 * @return Pointer to zeroed block, or NULL if allocation fails
 */
void *memory_pool_calloc(memory_pool_t pool);

/**
 * @brief Free a block back to pool
 *
 * Returns a previously allocated block to the pool.
 * If the block was a heap fallback, it is freed to heap.
 *
 * @param[in] pool Pool handle
 * @param[in] ptr Pointer to block to free (NULL is safely ignored)
 */
void memory_pool_free(memory_pool_t pool, void *ptr);

/* ============================================================================
 * Statistics Functions
 * ============================================================================ */

/**
 * @brief Get pool statistics
 *
 * @param[in] pool Pool handle
 * @param[out] stats Pointer to stats structure
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if parameters are invalid
 */
esp_err_t memory_pool_get_stats(memory_pool_t pool, memory_pool_stats_t *stats);

/**
 * @brief Log pool statistics
 *
 * Outputs pool statistics to ESP_LOG.
 *
 * @param[in] pool Pool handle
 */
void memory_pool_log_stats(memory_pool_t pool);

/* ============================================================================
 * Global Pool Instances
 * ============================================================================ */

/**
 * @brief Initialize global memory pools
 *
 * Creates the MQTT and JSON memory pools with default configuration.
 * Should be called during system initialization.
 *
 * @return ESP_OK on success
 */
esp_err_t memory_pools_init(void);

/**
 * @brief Deinitialize global memory pools
 *
 * @return ESP_OK on success
 */
esp_err_t memory_pools_deinit(void);

/**
 * @brief Allocate from MQTT message pool
 *
 * @return Pointer to 256-byte block, or NULL on failure
 */
void *memory_pool_mqtt_alloc(void);

/**
 * @brief Free to MQTT message pool
 *
 * @param[in] ptr Block to free
 */
void memory_pool_mqtt_free(void *ptr);

/**
 * @brief Allocate from JSON object pool
 *
 * @return Pointer to 512-byte block, or NULL on failure
 */
void *memory_pool_json_alloc(void);

/**
 * @brief Free to JSON object pool
 *
 * @param[in] ptr Block to free
 */
void memory_pool_json_free(void *ptr);

/**
 * @brief Log all global pool statistics
 */
void memory_pools_log_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_POOL_H */
