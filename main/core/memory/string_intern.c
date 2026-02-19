/**
 * @file string_intern.c
 * @brief String Interning Module Implementation
 *
 * Implements string interning with:
 * - Hash table (FNV-1a) for O(1) lookup
 * - Contiguous string pool in PSRAM
 * - Pre-populated common strings
 * - Thread-safe operations with mutex
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "string_intern.h"
#include "memory_manager_ng.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "STR_INTERN";

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** @brief Mutex timeout for string operations (ms) */
#define STR_MUTEX_TIMEOUT_MS        100

/** @brief FNV-1a hash parameters */
#define FNV_OFFSET_BASIS            0x811c9dc5
#define FNV_PRIME                   0x01000193

/* ============================================================================
 * Internal Types
 * ============================================================================ */

/**
 * @brief Hash table entry for interned string
 */
typedef struct intern_entry {
    uint32_t hash;              /**< Precomputed hash value */
    const char *str;            /**< Pointer into string pool */
    struct intern_entry *next;  /**< Chain for hash collisions */
} intern_entry_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Module initialization flag */
static bool s_initialized = false;

/** @brief Hash table buckets */
static intern_entry_t **s_buckets = NULL;

/** @brief Number of hash buckets */
static size_t s_bucket_count = 0;

/** @brief Entry storage (pre-allocated array) */
static intern_entry_t *s_entries = NULL;

/** @brief Maximum number of entries */
static size_t s_max_entries = 0;

/** @brief Current number of entries */
static size_t s_entry_count = 0;

/** @brief String pool memory */
static char *s_pool = NULL;

/** @brief Total pool size */
static size_t s_pool_size = 0;

/** @brief Next write position in pool */
static size_t s_pool_offset = 0;

/** @brief Bytes saved by interning (duplicate string bytes not stored) */
static size_t s_bytes_saved = 0;

/** @brief Thread safety mutex */
static SemaphoreHandle_t s_mutex = NULL;

/* ============================================================================
 * Common Strings to Pre-populate
 * ============================================================================ */

/** @brief Manufacturer names commonly seen in Zigbee devices */
static const char *const s_manufacturers[] = {
    "LUMI",
    "IKEA",
    "Philips",
    "SONOFF",
    "Tuya",
    "Xiaomi",
    "Aqara",
    "OSRAM",
    "LEDVANCE",
    "Signify",
    "eWeLink",
    "BlitzWolf",
    "Moes",
    "Zemismart",
    NULL
};

/** @brief State keys used in device state reporting */
static const char *const s_state_keys[] = {
    "state",
    "brightness",
    "temperature",
    "humidity",
    "battery",
    "linkquality",
    "voltage",
    "power",
    "energy",
    "current",
    "action",
    "occupancy",
    "contact",
    "water_leak",
    "smoke",
    "tamper",
    "illuminance",
    "illuminance_lux",
    "pressure",
    "color",
    "color_temp",
    "color_mode",
    "x",
    "y",
    "hue",
    "saturation",
    NULL
};

/** @brief Device class names for Home Assistant integration */
static const char *const s_device_classes[] = {
    "light",
    "switch",
    "sensor",
    "binary_sensor",
    "cover",
    "climate",
    "fan",
    "lock",
    "button",
    "number",
    "select",
    "text",
    NULL
};

/** @brief Common state values */
static const char *const s_state_values[] = {
    "ON",
    "OFF",
    "true",
    "false",
    "open",
    "closed",
    "locked",
    "unlocked",
    "online",
    "offline",
    NULL
};

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * @brief Compute FNV-1a hash of a string
 *
 * FNV-1a provides good distribution for string keys with minimal collisions.
 *
 * @param str String to hash
 * @return 32-bit hash value
 */
static uint32_t fnv1a_hash(const char *str)
{
    uint32_t hash = FNV_OFFSET_BASIS;

    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= FNV_PRIME;
    }

    return hash;
}

/**
 * @brief Find entry by string (internal, no locking)
 *
 * @param str String to find
 * @param hash Precomputed hash value
 * @return Pointer to entry if found, NULL otherwise
 */
static intern_entry_t *find_entry_internal(const char *str, uint32_t hash)
{
    size_t bucket = hash % s_bucket_count;
    intern_entry_t *entry = s_buckets[bucket];

    while (entry != NULL) {
        /* Compare hash first (fast), then string (slower) */
        if (entry->hash == hash && strcmp(entry->str, str) == 0) {
            return entry;
        }
        entry = entry->next;
    }

    return NULL;
}

/**
 * @brief Add string to pool (internal, no locking)
 *
 * @param str String to add
 * @param len String length (not including null terminator)
 * @return Pointer to string in pool, NULL if pool full
 */
static const char *add_to_pool(const char *str, size_t len)
{
    /* Check if we have space for string + null terminator */
    if (s_pool_offset + len + 1 > s_pool_size) {
        ESP_LOGW(TAG, "String pool full (need %u, have %u)",
                 len + 1, s_pool_size - s_pool_offset);
        return NULL;
    }

    /* Check if we have entry slots */
    if (s_entry_count >= s_max_entries) {
        ESP_LOGW(TAG, "Entry table full (%u max)", s_max_entries);
        return NULL;
    }

    /* Copy string to pool */
    char *pool_str = &s_pool[s_pool_offset];
    memcpy(pool_str, str, len);
    pool_str[len] = '\0';
    s_pool_offset += len + 1;

    return pool_str;
}

/**
 * @brief Create entry in hash table (internal, no locking)
 *
 * @param str Pointer to string in pool
 * @param hash Hash value
 * @return ESP_OK on success
 */
static esp_err_t create_entry(const char *str, uint32_t hash)
{
    /* Get next free entry slot */
    intern_entry_t *entry = &s_entries[s_entry_count++];
    entry->hash = hash;
    entry->str = str;

    /* Insert at head of bucket chain */
    size_t bucket = hash % s_bucket_count;
    entry->next = s_buckets[bucket];
    s_buckets[bucket] = entry;

    return ESP_OK;
}

/**
 * @brief Pre-populate common strings (internal, no locking)
 *
 * @param strings Null-terminated array of strings
 * @param category Name for logging
 * @return Number of strings added
 */
static size_t prepopulate_strings(const char *const *strings, const char *category)
{
    size_t count = 0;

    for (size_t i = 0; strings[i] != NULL; i++) {
        const char *str = strings[i];
        size_t len = strlen(str);

        if (len > STRING_INTERN_MAX_LENGTH) {
            ESP_LOGW(TAG, "Skipping %s string (too long): %s", category, str);
            continue;
        }

        uint32_t hash = fnv1a_hash(str);

        /* Check if already exists */
        if (find_entry_internal(str, hash) != NULL) {
            continue;
        }

        /* Add to pool and create entry */
        const char *pool_str = add_to_pool(str, len);
        if (pool_str == NULL) {
            ESP_LOGW(TAG, "Failed to add %s string: %s", category, str);
            break;
        }

        if (create_entry(pool_str, hash) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create entry for %s string: %s", category, str);
            break;
        }

        count++;
    }

    return count;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t string_intern_init(size_t max_strings, size_t pool_size)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (max_strings == 0 || pool_size == 0) {
        ESP_LOGE(TAG, "Invalid parameters: max_strings=%u, pool_size=%u",
                 max_strings, pool_size);
        return ESP_ERR_INVALID_ARG;
    }

    /* Calculate bucket count (aim for ~70% load factor) */
    s_bucket_count = (max_strings * 10) / 7;
    if (s_bucket_count < 32) {
        s_bucket_count = 32;
    }

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate hash table buckets (in PSRAM for larger tables) */
    size_t buckets_size = s_bucket_count * sizeof(intern_entry_t *);
    s_buckets = mem_ng_calloc(s_bucket_count, sizeof(intern_entry_t *), MEM_CAP_DEFAULT);
    if (s_buckets == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buckets (%u bytes)", buckets_size);
        goto cleanup;
    }

    /* Allocate entry storage (in PSRAM) */
    s_max_entries = max_strings;
    size_t entries_size = max_strings * sizeof(intern_entry_t);
    s_entries = mem_alloc(entries_size, MEM_CAP_PSRAM);
    if (s_entries == NULL) {
        ESP_LOGE(TAG, "Failed to allocate entries (%u bytes)", entries_size);
        goto cleanup;
    }
    memset(s_entries, 0, entries_size);
    s_entry_count = 0;

    /* Allocate string pool (in PSRAM) */
    s_pool_size = pool_size;
    s_pool = mem_alloc(pool_size, MEM_CAP_PSRAM);
    if (s_pool == NULL) {
        ESP_LOGE(TAG, "Failed to allocate string pool (%u bytes)", pool_size);
        goto cleanup;
    }
    s_pool_offset = 0;
    s_bytes_saved = 0;

    s_initialized = true;

    /* Pre-populate common strings */
    size_t mfr_count = prepopulate_strings(s_manufacturers, "manufacturer");
    size_t key_count = prepopulate_strings(s_state_keys, "state_key");
    size_t class_count = prepopulate_strings(s_device_classes, "device_class");
    size_t val_count = prepopulate_strings(s_state_values, "state_value");

    ESP_LOGI(TAG, "Initialized with %u buckets, %u max strings, %u KB pool",
             s_bucket_count, max_strings, pool_size / 1024);
    ESP_LOGI(TAG, "Pre-populated: %u manufacturers, %u state keys, %u device classes, %u state values",
             mfr_count, key_count, class_count, val_count);
    ESP_LOGI(TAG, "Pool usage after prepopulate: %u/%u bytes (%u%%)",
             s_pool_offset, s_pool_size, (s_pool_offset * 100) / s_pool_size);

    return ESP_OK;

cleanup:
    if (s_pool != NULL) {
        mem_ng_free(s_pool);
        s_pool = NULL;
    }
    if (s_entries != NULL) {
        mem_ng_free(s_entries);
        s_entries = NULL;
    }
    if (s_buckets != NULL) {
        mem_ng_free(s_buckets);
        s_buckets = NULL;
    }
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t string_intern_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing (had %u strings, saved %u bytes)",
             s_entry_count, s_bytes_saved);

    if (s_pool != NULL) {
        mem_ng_free(s_pool);
        s_pool = NULL;
    }

    if (s_entries != NULL) {
        mem_ng_free(s_entries);
        s_entries = NULL;
    }

    if (s_buckets != NULL) {
        mem_ng_free(s_buckets);
        s_buckets = NULL;
    }

    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_bucket_count = 0;
    s_max_entries = 0;
    s_entry_count = 0;
    s_pool_size = 0;
    s_pool_offset = 0;
    s_bytes_saved = 0;
    s_initialized = false;

    return ESP_OK;
}

const char *string_intern(const char *str)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return NULL;
    }

    if (str == NULL) {
        return NULL;
    }

    size_t len = strlen(str);
    if (len > STRING_INTERN_MAX_LENGTH) {
        ESP_LOGD(TAG, "String too long (%u > %u): %.32s...",
                 len, STRING_INTERN_MAX_LENGTH, str);
        return NULL;
    }

    uint32_t hash = fnv1a_hash(str);
    const char *result = NULL;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(STR_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Mutex timeout");
        return NULL;
    }

    /* Check if already interned */
    intern_entry_t *entry = find_entry_internal(str, hash);
    if (entry != NULL) {
        /* Found existing entry - track bytes saved */
        s_bytes_saved += len + 1;
        result = entry->str;
        goto done;
    }

    /* Need to add new entry */
    const char *pool_str = add_to_pool(str, len);
    if (pool_str == NULL) {
        goto done;
    }

    if (create_entry(pool_str, hash) != ESP_OK) {
        /* Roll back pool offset on failure */
        s_pool_offset -= len + 1;
        goto done;
    }

    result = pool_str;

done:
    xSemaphoreGive(s_mutex);
    return result;
}

bool string_is_interned(const char *str)
{
    if (!s_initialized || str == NULL) {
        return false;
    }

    uint32_t hash = fnv1a_hash(str);
    bool found = false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(STR_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }

    found = (find_entry_internal(str, hash) != NULL);

    xSemaphoreGive(s_mutex);
    return found;
}

bool string_ptr_is_interned(const char *ptr)
{
    if (!s_initialized || ptr == NULL || s_pool == NULL) {
        return false;
    }

    /* Check if pointer is within pool bounds */
    return (ptr >= s_pool && ptr < (s_pool + s_pool_offset));
}

void string_intern_get_stats(string_intern_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(string_intern_stats_t));

    if (!s_initialized) {
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(STR_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        /* Return partial info without lock */
        stats->max_strings = s_max_entries;
        stats->pool_size = s_pool_size;
        return;
    }

    stats->strings_count = s_entry_count;
    stats->max_strings = s_max_entries;
    stats->bytes_used = s_pool_offset;
    stats->bytes_saved = s_bytes_saved;
    stats->pool_size = s_pool_size;

    xSemaphoreGive(s_mutex);
}

void string_intern_print_stats(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return;
    }

    string_intern_stats_t stats;
    string_intern_get_stats(&stats);

    size_t pool_pct = (stats.pool_size > 0) ?
                      (stats.bytes_used * 100) / stats.pool_size : 0;
    size_t strings_pct = (stats.max_strings > 0) ?
                         (stats.strings_count * 100) / stats.max_strings : 0;

    ESP_LOGI(TAG, "=== String Intern Statistics ===");
    ESP_LOGI(TAG, "Strings:    %u / %u (%u%%)",
             stats.strings_count, stats.max_strings, strings_pct);
    ESP_LOGI(TAG, "Pool:       %u / %u bytes (%u%%)",
             stats.bytes_used, stats.pool_size, pool_pct);
    ESP_LOGI(TAG, "Bytes saved: %u bytes", stats.bytes_saved);

    if (stats.bytes_used > 0) {
        size_t efficiency = (stats.bytes_saved * 100) /
                           (stats.bytes_used + stats.bytes_saved);
        ESP_LOGI(TAG, "Efficiency: %u%% (would have used %u bytes without interning)",
                 efficiency, stats.bytes_used + stats.bytes_saved);
    }
}
