/**
 * @file buffer_pool_helpers.h
 * @brief Buffer Pool Helper Functions for JSON Serialization
 *
 * Provides convenient wrapper functions for using buffer pools with cJSON
 * serialization. These helpers attempt to use pooled buffers for common
 * JSON operations, falling back to malloc when needed.
 *
 * Usage:
 *   bool from_pool;
 *   char *json_str = pool_json_print(json, &from_pool);
 *   // ... use json_str ...
 *   pool_json_free(json_str, from_pool);
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BUFFER_POOL_HELPERS_H
#define BUFFER_POOL_HELPERS_H

#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief JSON pool buffer size (default 2KB)
 *
 * JSON payloads larger than this will fall back to malloc.
 */
#define POOL_JSON_BUFFER_SIZE   2048

/**
 * @brief Print cJSON object to pool buffer (with fallback)
 *
 * Attempts to serialize the cJSON object to a buffer from the JSON pool.
 * If the pool is exhausted or the JSON is larger than the pool buffer size,
 * falls back to cJSON_PrintUnformatted (malloc).
 *
 * @param[in] json cJSON object to serialize
 * @param[out] from_pool Set to true if buffer came from pool, false if malloc'd
 *
 * @return Pointer to JSON string, or NULL on failure
 *         Caller must call pool_json_free() to release the buffer
 */
char *pool_json_print(cJSON *json, bool *from_pool);

/**
 * @brief Print cJSON object to pool buffer (unformatted, with fallback)
 *
 * Same as pool_json_print() but uses unformatted output (no whitespace).
 *
 * @param[in] json cJSON object to serialize
 * @param[out] from_pool Set to true if buffer came from pool, false if malloc'd
 *
 * @return Pointer to JSON string, or NULL on failure
 */
char *pool_json_print_unformatted(cJSON *json, bool *from_pool);

/**
 * @brief Release pool buffer or free malloc'd buffer
 *
 * Releases a buffer back to the pool if from_pool is true,
 * otherwise frees it with cJSON_free.
 *
 * @param[in] buffer Buffer to release (NULL safe)
 * @param[in] from_pool True if buffer came from pool, false if malloc'd
 */
void pool_json_free(char *buffer, bool from_pool);

/**
 * @brief Get JSON pool statistics
 *
 * @param[out] pool_hits Number of successful pool allocations
 * @param[out] pool_misses Number of fallback malloc allocations
 */
void pool_json_get_stats(uint32_t *pool_hits, uint32_t *pool_misses);

/**
 * @brief Acquire a buffer from MQTT pool
 *
 * Returns a buffer from the MQTT pool (typically smaller buffers
 * suitable for MQTT topics and short payloads).
 *
 * @return Buffer pointer, or NULL if pool exhausted
 */
char *pool_mqtt_acquire(void);

/**
 * @brief Release buffer back to MQTT pool
 *
 * @param[in] buffer Buffer to release
 */
void pool_mqtt_release(char *buffer);

#ifdef __cplusplus
}
#endif

#endif /* BUFFER_POOL_HELPERS_H */
