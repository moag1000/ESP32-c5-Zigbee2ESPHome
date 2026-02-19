/**
 * @file memory_manager_ng.c
 * @brief Next-Gen Memory Management Implementation
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

#include "memory_manager_ng.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "MEM_MGR";

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** @brief Maximum number of low-memory callbacks */
#define MEM_MAX_LOW_CALLBACKS       8

/** @brief Mutex timeout for memory operations (ms) */
#define MEM_MUTEX_TIMEOUT_MS        100

/** @brief Maximum pool name length */
#define MEM_POOL_NAME_MAX           16

/* ============================================================================
 * Internal Types
 * ============================================================================ */

/**
 * @brief Freelist node for O(1) buffer pool operations
 */
typedef struct freelist_node {
    struct freelist_node *next;
} freelist_node_t;

/**
 * @brief Buffer pool internal structure
 */
struct buffer_pool {
    char name[MEM_POOL_NAME_MAX];   /**< Pool name for debugging */
    uint8_t *buffer;                /**< Pre-allocated contiguous buffer */
    freelist_node_t *freelist;      /**< Head of free buffer list */
    size_t buffer_size;             /**< Size of each buffer */
    size_t count;                   /**< Total number of buffers */
    size_t available;               /**< Currently available buffers */
    uint32_t acquires;              /**< Total acquire operations */
    uint32_t releases;              /**< Total release operations */
    SemaphoreHandle_t mutex;        /**< Thread safety mutex */
};

/**
 * @brief Low-memory callback registration
 */
typedef struct {
    mem_low_callback_t callback;
    size_t threshold;
    void *user_ctx;
    bool triggered;                 /**< Prevent repeated triggers */
} low_mem_callback_entry_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Module initialization flag */
static bool s_initialized = false;

/** @brief PSRAM availability flag */
static bool s_psram_available = false;

/** @brief Global allocation statistics */
static uint32_t s_alloc_count = 0;
static uint32_t s_free_count = 0;
static uint32_t s_pool_hits = 0;
static uint32_t s_pool_misses = 0;

/** @brief Low-memory callbacks */
static low_mem_callback_entry_t s_low_callbacks[MEM_MAX_LOW_CALLBACKS];
static size_t s_low_callback_count = 0;

/** @brief Global statistics mutex */
static SemaphoreHandle_t s_stats_mutex = NULL;

/** @brief Registered buffer pools */
static buffer_pool_t s_pools[MEM_MAX_POOLS];
static size_t s_pool_count = 0;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * @brief Convert mem_caps_t to ESP-IDF heap capabilities
 */
static uint32_t caps_to_heap_caps(mem_caps_t caps, size_t size)
{
    switch (caps) {
        case MEM_CAP_INTERNAL:
            return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

        case MEM_CAP_PSRAM:
            if (s_psram_available) {
                return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
            }
            /* Fall back to internal if no PSRAM */
            return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

        case MEM_CAP_DMA:
            return MALLOC_CAP_DMA | MALLOC_CAP_8BIT;

        case MEM_CAP_EXEC:
            /* MALLOC_CAP_EXEC is not available in ESP-IDF v6.0+ with memory
             * protection enabled. Fall back to 32-bit aligned internal memory. */
#ifdef MALLOC_CAP_EXEC
            return MALLOC_CAP_EXEC | MALLOC_CAP_32BIT;
#else
            return MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT;
#endif

        case MEM_CAP_DEFAULT:
        default:
            /* Auto-select: PSRAM for large allocations if available */
            if (s_psram_available && size > MEM_PSRAM_THRESHOLD) {
                return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
            }
            return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }
}

/**
 * @brief Check and invoke low-memory callbacks
 */
static void check_low_memory_callbacks(void)
{
    size_t free_heap = mem_get_free_internal();

    for (size_t i = 0; i < s_low_callback_count; i++) {
        low_mem_callback_entry_t *entry = &s_low_callbacks[i];

        if (free_heap < entry->threshold) {
            if (!entry->triggered && entry->callback != NULL) {
                entry->triggered = true;
                entry->callback(free_heap, entry->user_ctx);
            }
        } else {
            /* Reset trigger when memory recovers */
            entry->triggered = false;
        }
    }
}

/**
 * @brief Check if pointer is within pool buffer range
 */
static bool is_pool_buffer(const struct buffer_pool *pool, const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    const uint8_t *start = pool->buffer;
    const uint8_t *end = pool->buffer + (pool->buffer_size * pool->count);
    return (p >= start && p < end);
}

/* ============================================================================
 * Core Memory API Implementation
 * ============================================================================ */

esp_err_t mem_manager_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Memory manager already initialized");
        return ESP_OK;
    }

    /* Create statistics mutex */
    s_stats_mutex = xSemaphoreCreateMutex();
    if (s_stats_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create statistics mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Detect PSRAM availability */
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    s_psram_available = (psram_size > 0);

    /* Initialize callback array */
    memset(s_low_callbacks, 0, sizeof(s_low_callbacks));
    s_low_callback_count = 0;

    /* Initialize pool array */
    memset(s_pools, 0, sizeof(s_pools));
    s_pool_count = 0;

    /* Reset statistics */
    s_alloc_count = 0;
    s_free_count = 0;
    s_pool_hits = 0;
    s_pool_misses = 0;

    s_initialized = true;

    ESP_LOGI(TAG, "Memory manager initialized");
    ESP_LOGI(TAG, "  Internal heap: %u KB total, %u KB free",
             heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);

    if (s_psram_available) {
        ESP_LOGI(TAG, "  PSRAM: %u KB total, %u KB free",
                 psram_size / 1024,
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    } else {
        ESP_LOGI(TAG, "  PSRAM: not available");
    }

    return ESP_OK;
}

void *mem_alloc(size_t size, mem_caps_t caps)
{
    if (size == 0) {
        return NULL;
    }

    uint32_t heap_caps = caps_to_heap_caps(caps, size);
    void *ptr = heap_caps_malloc(size, heap_caps);

    if (ptr != NULL) {
        if (xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(MEM_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            s_alloc_count++;
            s_pool_misses++;  /* Direct allocation = pool miss */
            xSemaphoreGive(s_stats_mutex);
        }
        check_low_memory_callbacks();
    } else {
        ESP_LOGW(TAG, "Allocation failed: size=%u, caps=0x%08x", size, heap_caps);
    }

    return ptr;
}

void *mem_ng_calloc(size_t count, size_t size, mem_caps_t caps)
{
    if (count == 0 || size == 0) {
        return NULL;
    }

    size_t total_size = count * size;

    /* Check for overflow */
    if (total_size / count != size) {
        ESP_LOGE(TAG, "Allocation size overflow: count=%u, size=%u", count, size);
        return NULL;
    }

    uint32_t heap_caps = caps_to_heap_caps(caps, total_size);
    void *ptr = heap_caps_calloc(count, size, heap_caps);

    if (ptr != NULL) {
        if (xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(MEM_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            s_alloc_count++;
            s_pool_misses++;
            xSemaphoreGive(s_stats_mutex);
        }
        check_low_memory_callbacks();
    } else {
        ESP_LOGW(TAG, "Calloc failed: count=%u, size=%u, caps=0x%08x", count, size, heap_caps);
    }

    return ptr;
}

void *mem_realloc(void *ptr, size_t size, mem_caps_t caps)
{
    if (size == 0) {
        mem_ng_free(ptr);
        return NULL;
    }

    if (ptr == NULL) {
        return mem_alloc(size, caps);
    }

    uint32_t heap_caps = caps_to_heap_caps(caps, size);
    void *new_ptr = heap_caps_realloc(ptr, size, heap_caps);

    if (new_ptr != NULL) {
        check_low_memory_callbacks();
    } else {
        ESP_LOGW(TAG, "Realloc failed: size=%u, caps=0x%08x", size, heap_caps);
    }

    return new_ptr;
}

void mem_ng_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    heap_caps_free(ptr);

    if (xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(MEM_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_free_count++;
        xSemaphoreGive(s_stats_mutex);
    }
}

char *mem_strdup(const char *str, mem_caps_t caps)
{
    if (str == NULL) {
        return NULL;
    }

    size_t len = strlen(str) + 1;
    char *dup = (char *)mem_alloc(len, caps);

    if (dup != NULL) {
        memcpy(dup, str, len);
    }

    return dup;
}

/* ============================================================================
 * Buffer Pool Implementation
 * ============================================================================ */

buffer_pool_t mem_pool_create(const char *name, size_t buffer_size,
                               size_t count, mem_caps_t caps)
{
    if (buffer_size == 0 || count == 0) {
        ESP_LOGE(TAG, "Invalid pool configuration: size=%u, count=%u", buffer_size, count);
        return NULL;
    }

    /* Ensure buffer size can hold a freelist node pointer */
    if (buffer_size < sizeof(freelist_node_t)) {
        buffer_size = sizeof(freelist_node_t);
    }

    /* Check pool limit */
    if (s_pool_count >= MEM_MAX_POOLS) {
        ESP_LOGE(TAG, "Maximum number of pools (%d) reached", MEM_MAX_POOLS);
        return NULL;
    }

    /* Allocate pool structure */
    struct buffer_pool *pool = calloc(1, sizeof(struct buffer_pool));
    if (pool == NULL) {
        ESP_LOGE(TAG, "Failed to allocate pool structure");
        return NULL;
    }

    /* Copy name */
    if (name != NULL) {
        strncpy(pool->name, name, MEM_POOL_NAME_MAX - 1);
        pool->name[MEM_POOL_NAME_MAX - 1] = '\0';
    } else {
        snprintf(pool->name, MEM_POOL_NAME_MAX, "pool_%u", s_pool_count);
    }

    pool->buffer_size = buffer_size;
    pool->count = count;
    pool->available = count;
    pool->acquires = 0;
    pool->releases = 0;

    /* Create mutex */
    pool->mutex = xSemaphoreCreateMutex();
    if (pool->mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create pool mutex for '%s'", pool->name);
        free(pool);
        return NULL;
    }

    /* Allocate contiguous buffer for all pool items */
    size_t total_size = buffer_size * count;
    uint32_t heap_caps = caps_to_heap_caps(caps, total_size);

    pool->buffer = heap_caps_malloc(total_size, heap_caps);
    if (pool->buffer == NULL) {
        /* Try fallback to any available memory */
        ESP_LOGW(TAG, "Pool '%s': preferred allocation failed, trying fallback", pool->name);
        pool->buffer = malloc(total_size);
        if (pool->buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate pool buffer for '%s' (%u bytes)",
                     pool->name, total_size);
            vSemaphoreDelete(pool->mutex);
            free(pool);
            return NULL;
        }
    }

    /* Initialize freelist - link all buffers together */
    pool->freelist = NULL;
    for (size_t i = 0; i < count; i++) {
        freelist_node_t *node = (freelist_node_t *)(pool->buffer + (i * buffer_size));
        node->next = pool->freelist;
        pool->freelist = node;
    }

    /* Register pool */
    s_pools[s_pool_count++] = pool;

    ESP_LOGI(TAG, "Pool '%s' created: %u x %u bytes = %u KB",
             pool->name, count, buffer_size, total_size / 1024);

    return pool;
}

void mem_pool_destroy(buffer_pool_t pool)
{
    if (pool == NULL) {
        return;
    }

    struct buffer_pool *p = pool;

    ESP_LOGI(TAG, "Destroying pool '%s' (acquires: %u, releases: %u)",
             p->name, p->acquires, p->releases);

    /* Remove from registered pools */
    for (size_t i = 0; i < s_pool_count; i++) {
        if (s_pools[i] == pool) {
            /* Shift remaining pools */
            for (size_t j = i; j < s_pool_count - 1; j++) {
                s_pools[j] = s_pools[j + 1];
            }
            s_pools[s_pool_count - 1] = NULL;
            s_pool_count--;
            break;
        }
    }

    if (p->mutex != NULL) {
        vSemaphoreDelete(p->mutex);
    }

    if (p->buffer != NULL) {
        heap_caps_free(p->buffer);
    }

    free(p);
}

void *mem_pool_acquire(buffer_pool_t pool)
{
    if (pool == NULL) {
        return NULL;
    }

    struct buffer_pool *p = pool;
    void *buffer = NULL;

    if (xSemaphoreTake(p->mutex, pdMS_TO_TICKS(MEM_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Pool '%s' mutex timeout in acquire", p->name);
        return NULL;
    }

    if (p->freelist != NULL) {
        /* O(1) acquire from freelist head */
        freelist_node_t *node = p->freelist;
        p->freelist = node->next;
        p->available--;
        p->acquires++;
        buffer = (void *)node;

        /* Track pool hit */
        if (xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(MEM_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            s_pool_hits++;
            xSemaphoreGive(s_stats_mutex);
        }
    }

    xSemaphoreGive(p->mutex);

    if (buffer == NULL) {
        ESP_LOGD(TAG, "Pool '%s' exhausted", p->name);
    }

    return buffer;
}

void mem_pool_release(buffer_pool_t pool, void *buffer)
{
    if (pool == NULL || buffer == NULL) {
        return;
    }

    struct buffer_pool *p = pool;

    /* Validate buffer belongs to this pool */
    if (!is_pool_buffer(p, buffer)) {
        ESP_LOGE(TAG, "Pool '%s': attempted to release invalid buffer %p", p->name, buffer);
        return;
    }

    if (xSemaphoreTake(p->mutex, pdMS_TO_TICKS(MEM_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Pool '%s' mutex timeout in release", p->name);
        return;
    }

    /* O(1) release to freelist head */
    freelist_node_t *node = (freelist_node_t *)buffer;
    node->next = p->freelist;
    p->freelist = node;
    p->available++;
    p->releases++;

    xSemaphoreGive(p->mutex);
}

void mem_pool_stats(buffer_pool_t pool, size_t *total, size_t *available)
{
    if (pool == NULL) {
        if (total != NULL) *total = 0;
        if (available != NULL) *available = 0;
        return;
    }

    struct buffer_pool *p = pool;

    if (xSemaphoreTake(p->mutex, pdMS_TO_TICKS(MEM_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (total != NULL) *total = p->count;
        if (available != NULL) *available = p->available;
        xSemaphoreGive(p->mutex);
    } else {
        /* Return cached values without lock on timeout */
        if (total != NULL) *total = p->count;
        if (available != NULL) *available = p->available;
    }
}

/* ============================================================================
 * Statistics & Monitoring Implementation
 * ============================================================================ */

void mem_get_stats(mem_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(mem_stats_t));

    /* Internal memory stats */
    stats->total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    stats->free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    stats->min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    /* PSRAM stats */
    if (s_psram_available) {
        stats->total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        stats->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        stats->min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    }

    /* Largest free block */
    stats->largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    /* Allocation statistics */
    if (xSemaphoreTake(s_stats_mutex, pdMS_TO_TICKS(MEM_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        stats->alloc_count = s_alloc_count;
        stats->free_count = s_free_count;
        stats->pool_hits = s_pool_hits;
        stats->pool_misses = s_pool_misses;
        xSemaphoreGive(s_stats_mutex);
    }
}

size_t mem_get_free(void)
{
    size_t free_total = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    if (s_psram_available) {
        free_total += heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }

    return free_total;
}

size_t mem_get_free_internal(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

bool mem_is_low(void)
{
    return mem_get_free_internal() < MEM_MIN_FREE_HEAP;
}

esp_err_t mem_register_low_callback(mem_low_callback_t callback,
                                     size_t threshold, void *user_ctx)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_low_callback_count >= MEM_MAX_LOW_CALLBACKS) {
        ESP_LOGE(TAG, "Maximum low-memory callbacks (%d) reached", MEM_MAX_LOW_CALLBACKS);
        return ESP_ERR_NO_MEM;
    }

    low_mem_callback_entry_t *entry = &s_low_callbacks[s_low_callback_count];
    entry->callback = callback;
    entry->threshold = threshold;
    entry->user_ctx = user_ctx;
    entry->triggered = false;

    s_low_callback_count++;

    ESP_LOGI(TAG, "Registered low-memory callback (threshold: %u bytes)", threshold);

    return ESP_OK;
}

void mem_print_stats(void)
{
    mem_stats_t stats;
    mem_get_stats(&stats);

    ESP_LOGI(TAG, "=== Memory Statistics ===");
    ESP_LOGI(TAG, "Internal Heap:");
    ESP_LOGI(TAG, "  Total:     %7u bytes (%u KB)",
             stats.total_internal, stats.total_internal / 1024);
    ESP_LOGI(TAG, "  Free:      %7u bytes (%u KB)",
             stats.free_internal, stats.free_internal / 1024);
    ESP_LOGI(TAG, "  Min Free:  %7u bytes (%u KB)",
             stats.min_free_internal, stats.min_free_internal / 1024);

    if (s_psram_available) {
        ESP_LOGI(TAG, "PSRAM:");
        ESP_LOGI(TAG, "  Total:     %7u bytes (%u KB)",
                 stats.total_psram, stats.total_psram / 1024);
        ESP_LOGI(TAG, "  Free:      %7u bytes (%u KB)",
                 stats.free_psram, stats.free_psram / 1024);
        ESP_LOGI(TAG, "  Min Free:  %7u bytes (%u KB)",
                 stats.min_free_psram, stats.min_free_psram / 1024);
    }

    ESP_LOGI(TAG, "Allocations:");
    ESP_LOGI(TAG, "  Allocs:    %u", stats.alloc_count);
    ESP_LOGI(TAG, "  Frees:     %u", stats.free_count);
    ESP_LOGI(TAG, "  Pool Hits: %u", stats.pool_hits);
    ESP_LOGI(TAG, "  Pool Miss: %u", stats.pool_misses);
    ESP_LOGI(TAG, "  Largest:   %u bytes", stats.largest_free_block);

    /* Print pool statistics */
    if (s_pool_count > 0) {
        ESP_LOGI(TAG, "Buffer Pools:");
        for (size_t i = 0; i < s_pool_count; i++) {
            struct buffer_pool *p = s_pools[i];
            if (p != NULL) {
                size_t total, available;
                mem_pool_stats(p, &total, &available);
                size_t used = total - available;
                size_t usage_pct = (total > 0) ? (used * 100 / total) : 0;
                ESP_LOGI(TAG, "  '%s': %u/%u used (%u%%), %u acquires, %u releases",
                         p->name, used, total, usage_pct, p->acquires, p->releases);
            }
        }
    }

    /* Memory warning */
    if (mem_is_low()) {
        ESP_LOGW(TAG, "WARNING: Memory is low! Free internal heap < %u bytes",
                 MEM_MIN_FREE_HEAP);
    }
}
