/**
 * @file memory_pool.c
 * @brief Memory Pool Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "memory_pool.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "MEM_POOL";

/** @brief Mutex timeout for pool operations (ms) */
#define MEMORY_POOL_MUTEX_TIMEOUT_MS  500

/* ============================================================================
 * Internal Types
 * ============================================================================ */

/**
 * @brief Memory pool internal structure
 */
struct memory_pool {
    uint8_t *buffer;                /**< Pre-allocated buffer */
    uint8_t *free_bitmap;           /**< Bitmap tracking free blocks */
    size_t block_size;              /**< Size of each block */
    size_t block_count;             /**< Number of blocks */
    size_t free_count;              /**< Number of free blocks */
    size_t allocations;             /**< Total allocations */
    size_t heap_fallbacks;          /**< Heap fallback count */
    bool use_psram;                 /**< Using PSRAM */
    const char *name;               /**< Pool name */
    SemaphoreHandle_t mutex;        /**< Thread safety mutex */
};

/* ============================================================================
 * Global Pool Instances
 * ============================================================================ */

static memory_pool_t s_mqtt_pool = NULL;
static memory_pool_t s_json_pool = NULL;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * @brief Find first free block in bitmap
 *
 * @param[in] bitmap Bitmap array
 * @param[in] count Number of blocks
 * @return Index of first free block, or -1 if none
 */
static int find_free_block(const uint8_t *bitmap, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        size_t byte_idx = i / 8;
        size_t bit_idx = i % 8;
        if ((bitmap[byte_idx] & (1 << bit_idx)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Mark block as allocated in bitmap
 */
static void set_block_allocated(uint8_t *bitmap, size_t index)
{
    size_t byte_idx = index / 8;
    size_t bit_idx = index % 8;
    bitmap[byte_idx] |= (1 << bit_idx);
}

/**
 * @brief Mark block as free in bitmap
 */
static void set_block_free(uint8_t *bitmap, size_t index)
{
    size_t byte_idx = index / 8;
    size_t bit_idx = index % 8;
    bitmap[byte_idx] &= ~(1 << bit_idx);
}

/**
 * @brief Check if pointer is within pool buffer
 */
static bool is_pool_pointer(const struct memory_pool *pool, const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    const uint8_t *start = pool->buffer;
    const uint8_t *end = pool->buffer + (pool->block_size * pool->block_count);
    return (p >= start && p < end);
}

/**
 * @brief Get block index from pointer
 */
static int get_block_index(const struct memory_pool *pool, const void *ptr)
{
    if (!is_pool_pointer(pool, ptr)) {
        return -1;
    }
    size_t offset = (const uint8_t *)ptr - pool->buffer;
    if (offset % pool->block_size != 0) {
        return -1;  /* Misaligned pointer */
    }
    return (int)(offset / pool->block_size);
}

/* ============================================================================
 * Pool Management
 * ============================================================================ */

esp_err_t memory_pool_init(memory_pool_t *pool, const memory_pool_config_t *config)
{
    if (pool == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->block_size == 0 || config->block_count == 0) {
        ESP_LOGE(TAG, "Invalid pool configuration");
        return ESP_ERR_INVALID_ARG;
    }

    if (config->block_count > MEMORY_POOL_MAX_BLOCKS) {
        ESP_LOGE(TAG, "Block count exceeds maximum (%d)", MEMORY_POOL_MAX_BLOCKS);
        return ESP_ERR_INVALID_ARG;
    }

    /* Allocate pool structure */
    struct memory_pool *p = calloc(1, sizeof(struct memory_pool));
    if (p == NULL) {
        ESP_LOGE(TAG, "Failed to allocate pool structure");
        return ESP_ERR_NO_MEM;
    }

    p->block_size = config->block_size;
    p->block_count = config->block_count;
    p->use_psram = config->use_psram;
    p->name = config->name ? config->name : "unnamed";

    /* Calculate bitmap size (1 bit per block, rounded up to bytes) */
    size_t bitmap_size = (config->block_count + 7) / 8;
    p->free_bitmap = calloc(1, bitmap_size);
    if (p->free_bitmap == NULL) {
        ESP_LOGE(TAG, "Failed to allocate bitmap");
        free(p);
        return ESP_ERR_NO_MEM;
    }

    /* Allocate buffer (optionally in PSRAM) */
    size_t buffer_size = config->block_size * config->block_count;
    uint32_t caps = MALLOC_CAP_8BIT;
    if (config->use_psram) {
        caps |= MALLOC_CAP_SPIRAM;
    } else {
        caps |= MALLOC_CAP_INTERNAL;
    }

    p->buffer = heap_caps_malloc(buffer_size, caps);
    if (p->buffer == NULL) {
        /* Fallback to any available memory */
        ESP_LOGW(TAG, "PSRAM allocation failed, falling back to internal RAM");
        p->buffer = malloc(buffer_size);
        if (p->buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate pool buffer (%u bytes)", buffer_size);
            free(p->free_bitmap);
            free(p);
            return ESP_ERR_NO_MEM;
        }
        p->use_psram = false;
    }

    /* Create mutex */
    p->mutex = xSemaphoreCreateMutex();
    if (p->mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create pool mutex");
        free(p->buffer);
        free(p->free_bitmap);
        free(p);
        return ESP_ERR_NO_MEM;
    }

    /* Initialize as all free */
    p->free_count = config->block_count;
    p->allocations = 0;
    p->heap_fallbacks = 0;

    *pool = p;

    ESP_LOGI(TAG, "Pool '%s' initialized: %u blocks x %u bytes = %u KB (%s)",
             p->name, p->block_count, p->block_size,
             (p->block_count * p->block_size) / 1024,
             p->use_psram ? "PSRAM" : "internal");

    return ESP_OK;
}

esp_err_t memory_pool_deinit(memory_pool_t pool)
{
    if (pool == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct memory_pool *p = pool;

    ESP_LOGI(TAG, "Deinitializing pool '%s' (allocations: %u, fallbacks: %u)",
             p->name, p->allocations, p->heap_fallbacks);

    if (p->mutex != NULL) {
        vSemaphoreDelete(p->mutex);
    }
    if (p->buffer != NULL) {
        free(p->buffer);
    }
    if (p->free_bitmap != NULL) {
        free(p->free_bitmap);
    }
    free(p);

    return ESP_OK;
}

/* ============================================================================
 * Allocation Functions
 * ============================================================================ */

void *memory_pool_alloc(memory_pool_t pool)
{
    if (pool == NULL) {
        return NULL;
    }

    struct memory_pool *p = pool;
    void *result = NULL;

    if (xSemaphoreTake(p->mutex, pdMS_TO_TICKS(MEMORY_POOL_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Pool '%s' mutex timeout in alloc - falling back to heap", p->name);
        return malloc(p->block_size);
    }

    int idx = find_free_block(p->free_bitmap, p->block_count);
    if (idx >= 0) {
        /* Allocate from pool */
        set_block_allocated(p->free_bitmap, idx);
        p->free_count--;
        p->allocations++;
        result = p->buffer + (idx * p->block_size);
    }

    xSemaphoreGive(p->mutex);

    /* Fallback to heap if pool exhausted */
    if (result == NULL) {
        result = malloc(p->block_size);
        if (result != NULL) {
            if (xSemaphoreTake(p->mutex, pdMS_TO_TICKS(MEMORY_POOL_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                p->heap_fallbacks++;
                p->allocations++;
                xSemaphoreGive(p->mutex);
            }
            ESP_LOGD(TAG, "Pool '%s' exhausted, using heap fallback", p->name);
        }
    }

    return result;
}

void *memory_pool_calloc(memory_pool_t pool)
{
    void *ptr = memory_pool_alloc(pool);
    if (ptr != NULL && pool != NULL) {
        memset(ptr, 0, pool->block_size);
    }
    return ptr;
}

void memory_pool_free(memory_pool_t pool, void *ptr)
{
    if (pool == NULL || ptr == NULL) {
        return;
    }

    struct memory_pool *p = pool;

    if (xSemaphoreTake(p->mutex, pdMS_TO_TICKS(MEMORY_POOL_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Pool '%s' mutex timeout in free - freeing via heap", p->name);
        free(ptr);
        return;
    }

    int idx = get_block_index(p, ptr);
    if (idx >= 0) {
        /* Return to pool */
        set_block_free(p->free_bitmap, idx);
        p->free_count++;
    } else {
        /* Was a heap fallback */
        free(ptr);
    }

    xSemaphoreGive(p->mutex);
}

/* ============================================================================
 * Statistics Functions
 * ============================================================================ */

esp_err_t memory_pool_get_stats(memory_pool_t pool, memory_pool_stats_t *stats)
{
    if (pool == NULL || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct memory_pool *p = pool;

    if (xSemaphoreTake(p->mutex, pdMS_TO_TICKS(MEMORY_POOL_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Pool mutex timeout in get_stats");
        return ESP_ERR_TIMEOUT;
    }

    stats->total_blocks = p->block_count;
    stats->free_blocks = p->free_count;
    stats->allocations = p->allocations;
    stats->heap_fallbacks = p->heap_fallbacks;
    stats->block_size = p->block_size;
    stats->use_psram = p->use_psram;

    xSemaphoreGive(p->mutex);

    return ESP_OK;
}

void memory_pool_log_stats(memory_pool_t pool)
{
    if (pool == NULL) {
        return;
    }

    memory_pool_stats_t stats;
    if (memory_pool_get_stats(pool, &stats) != ESP_OK) {
        return;
    }

    struct memory_pool *p = pool;
    size_t used = stats.total_blocks - stats.free_blocks;
    size_t usage_pct = (used * 100) / stats.total_blocks;

    ESP_LOGI(TAG, "Pool '%s': %u/%u blocks used (%u%%), %u allocs, %u fallbacks",
             p->name, used, stats.total_blocks, usage_pct,
             stats.allocations, stats.heap_fallbacks);
}

/* ============================================================================
 * Global Pool Instances
 * ============================================================================ */

esp_err_t memory_pools_init(void)
{
    esp_err_t ret;

    /* Initialize MQTT message pool (32 x 256 = 8KB) */
    memory_pool_config_t mqtt_config = {
        .block_size = MEMORY_POOL_MQTT_BLOCK_SIZE,
        .block_count = MEMORY_POOL_MQTT_BLOCK_COUNT,
        .use_psram = false,  /* Keep MQTT buffers in fast internal RAM */
        .name = "mqtt"
    };

    ret = memory_pool_init(&s_mqtt_pool, &mqtt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MQTT pool");
        return ret;
    }

    /* Initialize JSON object pool (16 x 512 = 8KB) */
    memory_pool_config_t json_config = {
        .block_size = MEMORY_POOL_JSON_BLOCK_SIZE,
        .block_count = MEMORY_POOL_JSON_BLOCK_COUNT,
        .use_psram = true,   /* Use PSRAM to save 8KB internal RAM */
        .name = "json"
    };

    ret = memory_pool_init(&s_json_pool, &json_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize JSON pool");
        memory_pool_deinit(s_mqtt_pool);
        s_mqtt_pool = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Global memory pools initialized (16KB total)");
    return ESP_OK;
}

esp_err_t memory_pools_deinit(void)
{
    if (s_mqtt_pool != NULL) {
        memory_pool_deinit(s_mqtt_pool);
        s_mqtt_pool = NULL;
    }
    if (s_json_pool != NULL) {
        memory_pool_deinit(s_json_pool);
        s_json_pool = NULL;
    }
    ESP_LOGI(TAG, "Global memory pools deinitialized");
    return ESP_OK;
}

void *memory_pool_mqtt_alloc(void)
{
    if (s_mqtt_pool == NULL) {
        /* Pool not initialized, use heap */
        return malloc(MEMORY_POOL_MQTT_BLOCK_SIZE);
    }
    return memory_pool_alloc(s_mqtt_pool);
}

void memory_pool_mqtt_free(void *ptr)
{
    if (s_mqtt_pool == NULL) {
        free(ptr);
        return;
    }
    memory_pool_free(s_mqtt_pool, ptr);
}

void *memory_pool_json_alloc(void)
{
    if (s_json_pool == NULL) {
        /* Pool not initialized, use heap */
        return malloc(MEMORY_POOL_JSON_BLOCK_SIZE);
    }
    return memory_pool_alloc(s_json_pool);
}

void memory_pool_json_free(void *ptr)
{
    if (s_json_pool == NULL) {
        free(ptr);
        return;
    }
    memory_pool_free(s_json_pool, ptr);
}

void memory_pools_log_stats(void)
{
    ESP_LOGI(TAG, "=== Memory Pool Statistics ===");
    if (s_mqtt_pool != NULL) {
        memory_pool_log_stats(s_mqtt_pool);
    }
    if (s_json_pool != NULL) {
        memory_pool_log_stats(s_json_pool);
    }
}
