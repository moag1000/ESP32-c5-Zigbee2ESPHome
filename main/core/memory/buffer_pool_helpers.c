/**
 * @file buffer_pool_helpers.c
 * @brief Buffer Pool Helper Functions Implementation
 *
 * Provides convenient wrapper functions for using buffer pools with cJSON
 * serialization, enabling zero-allocation hot-path JSON operations.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "buffer_pool_helpers.h"
#include "memory_manager_ng.h"
#include "core/foundation_init.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "POOL_HELPERS";

/* Statistics */
static uint32_t s_pool_hits = 0;
static uint32_t s_pool_misses = 0;

char *pool_json_print(cJSON *json, bool *from_pool)
{
    if (json == NULL || from_pool == NULL) {
        return NULL;
    }

    *from_pool = false;

    /* Get JSON pool */
    buffer_pool_t pool = foundation_get_json_pool();
    if (pool == NULL) {
        /* Pool not available, fall back to malloc */
        s_pool_misses++;
        return cJSON_Print(json);
    }

    /* Try to acquire a pool buffer */
    char *buffer = (char *)mem_pool_acquire(pool);
    if (buffer == NULL) {
        /* Pool exhausted, fall back to malloc */
        s_pool_misses++;
        ESP_LOGD(TAG, "JSON pool exhausted, using malloc");
        return cJSON_Print(json);
    }

    /* Print JSON to pool buffer with size limit */
    if (!cJSON_PrintPreallocated(json, buffer, POOL_JSON_BUFFER_SIZE, false)) {
        /* JSON too large for pool buffer, release and fall back */
        mem_pool_release(pool, buffer);
        s_pool_misses++;
        ESP_LOGD(TAG, "JSON too large for pool (%d bytes), using malloc", POOL_JSON_BUFFER_SIZE);
        return cJSON_Print(json);
    }

    s_pool_hits++;
    *from_pool = true;
    return buffer;
}

char *pool_json_print_unformatted(cJSON *json, bool *from_pool)
{
    if (json == NULL || from_pool == NULL) {
        return NULL;
    }

    *from_pool = false;

    /* Get JSON pool */
    buffer_pool_t pool = foundation_get_json_pool();
    if (pool == NULL) {
        /* Pool not available, fall back to malloc */
        s_pool_misses++;
        return cJSON_PrintUnformatted(json);
    }

    /* Try to acquire a pool buffer */
    char *buffer = (char *)mem_pool_acquire(pool);
    if (buffer == NULL) {
        /* Pool exhausted, fall back to malloc */
        s_pool_misses++;
        ESP_LOGD(TAG, "JSON pool exhausted, using malloc");
        return cJSON_PrintUnformatted(json);
    }

    /* Print unformatted JSON to pool buffer with size limit */
    if (!cJSON_PrintPreallocated(json, buffer, POOL_JSON_BUFFER_SIZE, false)) {
        /* JSON too large for pool buffer, release and fall back */
        mem_pool_release(pool, buffer);
        s_pool_misses++;
        ESP_LOGD(TAG, "JSON too large for pool (%d bytes), using malloc", POOL_JSON_BUFFER_SIZE);
        return cJSON_PrintUnformatted(json);
    }

    s_pool_hits++;
    *from_pool = true;
    return buffer;
}

void pool_json_free(char *buffer, bool from_pool)
{
    if (buffer == NULL) {
        return;
    }

    if (from_pool) {
        buffer_pool_t pool = foundation_get_json_pool();
        if (pool != NULL) {
            mem_pool_release(pool, buffer);
        } else {
            /* Pool was destroyed, free as regular memory */
            cJSON_free(buffer);
        }
    } else {
        cJSON_free(buffer);
    }
}

void pool_json_get_stats(uint32_t *pool_hits, uint32_t *pool_misses)
{
    if (pool_hits != NULL) {
        *pool_hits = s_pool_hits;
    }
    if (pool_misses != NULL) {
        *pool_misses = s_pool_misses;
    }
}

char *pool_mqtt_acquire(void)
{
    buffer_pool_t pool = foundation_get_mqtt_pool();
    if (pool == NULL) {
        return NULL;
    }
    return (char *)mem_pool_acquire(pool);
}

void pool_mqtt_release(char *buffer)
{
    if (buffer == NULL) {
        return;
    }

    buffer_pool_t pool = foundation_get_mqtt_pool();
    if (pool != NULL) {
        mem_pool_release(pool, buffer);
    }
}
