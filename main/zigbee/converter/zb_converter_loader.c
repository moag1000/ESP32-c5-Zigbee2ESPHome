/**
 * @file zb_converter_loader.c
 * @brief Runtime Converter Loader — JSON DB on LittleFS
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_converter_loader.h"
#include "zb_converter_fn_registry.h"
#include "core/littlefs_mount.h"
#include "core/memory/memory_manager_ng.h"
#include "core/memory/string_intern.h"
#include "core/gateway_timeouts.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "CONV_LOAD";

#define CONVERTER_DB_PATH       LITTLEFS_MOUNT_POINT "/converters"
#define INDEX_FILE_PATH         CONVERTER_DB_PATH "/index.json"
#define MAX_INDEX_ENTRIES        128
#define MAX_CACHED_CONVERTERS    32
#define MAX_FZ_PER_DEVICE        16
#define MAX_TZ_PER_DEVICE        8
#define MAX_EXPOSES_PER_DEVICE   16

/* ============================================================================
 * Index entry: manufacturer -> filename
 * ============================================================================ */

typedef struct {
    const char *manufacturer;   /* interned */
    const char *filename;       /* interned */
} index_entry_t;

/* ============================================================================
 * Converter cache entry
 * ============================================================================ */

typedef struct {
    const char *model;              /* interned — cache key */
    zb_converter_def_t *def;        /* heap-allocated definition */
} cache_entry_t;

/* ============================================================================
 * Static state
 * ============================================================================ */

static index_entry_t s_index[MAX_INDEX_ENTRIES];
static size_t s_index_count = 0;
static size_t s_db_device_count = 0;

static cache_entry_t s_cache[MAX_CACHED_CONVERTERS];
static size_t s_cache_count = 0;
static size_t s_cache_bytes = 0;

static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;
static bool s_available = false;

/* ============================================================================
 * File I/O helper
 * ============================================================================ */

static char *read_file_to_psram(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGD(TAG, "Cannot open %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 128 * 1024) {
        ESP_LOGW(TAG, "File %s: invalid size %ld", path, size);
        fclose(f);
        return NULL;
    }

    char *buf = mem_alloc((size_t)size + 1, MEM_CAP_DEFAULT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes for %s", size, path);
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);

    buf[read] = '\0';
    if (out_len) *out_len = read;
    return buf;
}

/* ============================================================================
 * Index loading
 * ============================================================================ */

static esp_err_t load_index(void)
{
    size_t len = 0;
    char *json_str = read_file_to_psram(INDEX_FILE_PATH, &len);
    if (json_str == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = cJSON_Parse(json_str);
    mem_ng_free(json_str);

    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse index.json");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Validate version */
    cJSON *ver = cJSON_GetObjectItem(root, "v");
    if (ver == NULL || !cJSON_IsNumber(ver) || (int)cJSON_GetNumberValue(ver) != 1) {
        ESP_LOGE(TAG, "Unsupported index version");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_VERSION;
    }

    /* Read device count */
    cJSON *count = cJSON_GetObjectItem(root, "count");
    if (count && cJSON_IsNumber(count)) {
        s_db_device_count = (size_t)cJSON_GetNumberValue(count);
    }

    /* Parse files mapping */
    cJSON *files = cJSON_GetObjectItem(root, "files");
    if (files == NULL || !cJSON_IsObject(files)) {
        ESP_LOGE(TAG, "No 'files' object in index.json");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_index_count = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, files) {
        if (s_index_count >= MAX_INDEX_ENTRIES) {
            ESP_LOGW(TAG, "Index full at %d entries", MAX_INDEX_ENTRIES);
            break;
        }
        if (!cJSON_IsString(entry)) continue;

        s_index[s_index_count].manufacturer = string_intern(entry->string);
        s_index[s_index_count].filename = string_intern(cJSON_GetStringValue(entry));
        s_index_count++;
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Loaded index: %zu manufacturers, %zu devices in DB",
             s_index_count, s_db_device_count);
    return ESP_OK;
}

/* ============================================================================
 * Expose type mapping
 * ============================================================================ */

static zb_expose_type_t map_expose_type(int t)
{
    if (t >= 0 && t < ZB_EXPOSE_MAX) {
        return (zb_expose_type_t)t;
    }
    return ZB_EXPOSE_SENSOR;  /* fallback */
}

/* ============================================================================
 * Parse a single device from cJSON
 * ============================================================================ */

static zb_converter_def_t *parse_device(const cJSON *dev_json)
{
    cJSON *model_j = cJSON_GetObjectItem(dev_json, "m");
    cJSON *manuf_j = cJSON_GetObjectItem(dev_json, "mf");
    cJSON *vendor_j = cJSON_GetObjectItem(dev_json, "v");
    cJSON *desc_j = cJSON_GetObjectItem(dev_json, "d");
    cJSON *fz_arr = cJSON_GetObjectItem(dev_json, "fz");
    cJSON *tz_arr = cJSON_GetObjectItem(dev_json, "tz");
    cJSON *exp_arr = cJSON_GetObjectItem(dev_json, "e");
    cJSON *quirks_j = cJSON_GetObjectItem(dev_json, "q");

    if (model_j == NULL || !cJSON_IsString(model_j)) {
        return NULL;
    }

    /* Allocate definition struct */
    zb_converter_def_t *def = mem_ng_calloc(1, sizeof(zb_converter_def_t), MEM_CAP_DEFAULT);
    if (def == NULL) return NULL;

    /* Intern strings */
    def->manufacturer = manuf_j && cJSON_IsString(manuf_j) ?
        string_intern(cJSON_GetStringValue(manuf_j)) : NULL;
    def->model = string_intern(cJSON_GetStringValue(model_j));
    def->vendor = vendor_j && cJSON_IsString(vendor_j) ?
        string_intern(cJSON_GetStringValue(vendor_j)) : NULL;
    def->description = desc_j && cJSON_IsString(desc_j) ?
        string_intern(cJSON_GetStringValue(desc_j)) : NULL;

    /* Quirk flags */
    def->quirk_flags = (quirks_j && cJSON_IsNumber(quirks_j)) ?
        (uint32_t)cJSON_GetNumberValue(quirks_j) : ZB_QUIRK_NONE;

    /* Parse fromZigbee array */
    if (fz_arr && cJSON_IsArray(fz_arr)) {
        int fz_count = cJSON_GetArraySize(fz_arr);
        if (fz_count > MAX_FZ_PER_DEVICE) fz_count = MAX_FZ_PER_DEVICE;

        zb_from_zigbee_t *fz = mem_ng_calloc((size_t)fz_count, sizeof(zb_from_zigbee_t),
                                              MEM_CAP_DEFAULT);
        if (fz != NULL) {
            int valid = 0;
            for (int i = 0; i < fz_count; i++) {
                cJSON *item = cJSON_GetArrayItem(fz_arr, i);
                cJSON *c = cJSON_GetObjectItem(item, "c");
                cJSON *a = cJSON_GetObjectItem(item, "a");
                cJSON *k = cJSON_GetObjectItem(item, "k");
                cJSON *fn = cJSON_GetObjectItem(item, "fn");

                if (c == NULL || k == NULL || fn == NULL) continue;

                const char *fn_name = cJSON_GetStringValue(fn);
                fz_convert_fn_t fn_ptr = zb_fn_registry_find_fz(fn_name);
                if (fn_ptr == NULL) {
                    ESP_LOGW(TAG, "Unknown fz function: %s (skipped)", fn_name ? fn_name : "null");
                    continue;
                }

                const char *interned_key = string_intern(cJSON_GetStringValue(k));
                if (interned_key == NULL) {
                    ESP_LOGW(TAG, "String intern failed for fz key (pool full?)");
                    continue;
                }
                fz[valid].cluster_id = (uint16_t)cJSON_GetNumberValue(c);
                fz[valid].attr_id = (a && cJSON_IsNumber(a)) ?
                    (uint16_t)cJSON_GetNumberValue(a) : 0xFFFF;
                fz[valid].endpoint = 0;
                fz[valid].json_key = interned_key;
                fz[valid].convert = fn_ptr;
                valid++;
            }
            def->from_zigbee = fz;
            def->from_zigbee_count = (uint8_t)valid;
        }
    }

    /* Parse toZigbee array */
    if (tz_arr && cJSON_IsArray(tz_arr)) {
        int tz_count = cJSON_GetArraySize(tz_arr);
        if (tz_count > MAX_TZ_PER_DEVICE) tz_count = MAX_TZ_PER_DEVICE;

        zb_to_zigbee_t *tz = mem_ng_calloc((size_t)tz_count, sizeof(zb_to_zigbee_t),
                                            MEM_CAP_DEFAULT);
        if (tz != NULL) {
            int valid = 0;
            for (int i = 0; i < tz_count; i++) {
                cJSON *item = cJSON_GetArrayItem(tz_arr, i);
                cJSON *k = cJSON_GetObjectItem(item, "k");
                cJSON *c = cJSON_GetObjectItem(item, "c");
                cJSON *fn = cJSON_GetObjectItem(item, "fn");

                if (k == NULL || fn == NULL) continue;

                const char *fn_name = cJSON_GetStringValue(fn);
                tz_convert_fn_t fn_ptr = zb_fn_registry_find_tz(fn_name);
                if (fn_ptr == NULL) {
                    ESP_LOGW(TAG, "Unknown tz function: %s (skipped)", fn_name ? fn_name : "null");
                    continue;
                }

                const char *interned_tz_key = string_intern(cJSON_GetStringValue(k));
                if (interned_tz_key == NULL) {
                    ESP_LOGW(TAG, "String intern failed for tz key (pool full?)");
                    continue;
                }
                tz[valid].json_key = interned_tz_key;
                tz[valid].cluster_id = (c && cJSON_IsNumber(c)) ?
                    (uint16_t)cJSON_GetNumberValue(c) : 0;
                tz[valid].endpoint = 0;
                tz[valid].convert = fn_ptr;
                valid++;
            }
            def->to_zigbee = tz;
            def->to_zigbee_count = (uint8_t)valid;
        }
    }

    /* Parse exposes array */
    if (exp_arr && cJSON_IsArray(exp_arr)) {
        int exp_count = cJSON_GetArraySize(exp_arr);
        if (exp_count > MAX_EXPOSES_PER_DEVICE) exp_count = MAX_EXPOSES_PER_DEVICE;

        zb_expose_t *exposes = mem_ng_calloc((size_t)exp_count, sizeof(zb_expose_t),
                                             MEM_CAP_DEFAULT);
        if (exposes != NULL) {
            for (int i = 0; i < exp_count; i++) {
                cJSON *item = cJSON_GetArrayItem(exp_arr, i);
                cJSON *t = cJSON_GetObjectItem(item, "t");
                cJSON *f = cJSON_GetObjectItem(item, "f");
                cJSON *n = cJSON_GetObjectItem(item, "n");
                cJSON *dc = cJSON_GetObjectItem(item, "dc");
                cJSON *u = cJSON_GetObjectItem(item, "u");
                cJSON *sc = cJSON_GetObjectItem(item, "sc");

                exposes[i].type = (t && cJSON_IsNumber(t)) ?
                    map_expose_type((int)cJSON_GetNumberValue(t)) : ZB_EXPOSE_SENSOR;
                exposes[i].features = (f && cJSON_IsNumber(f)) ?
                    (uint32_t)cJSON_GetNumberValue(f) : 0;
                exposes[i].name = (n && cJSON_IsString(n)) ?
                    string_intern(cJSON_GetStringValue(n)) : NULL;
                exposes[i].device_class = (dc && cJSON_IsString(dc)) ?
                    string_intern(cJSON_GetStringValue(dc)) : NULL;
                exposes[i].unit = (u && cJSON_IsString(u)) ?
                    string_intern(cJSON_GetStringValue(u)) : NULL;
                exposes[i].state_class = (sc && cJSON_IsString(sc)) ?
                    string_intern(cJSON_GetStringValue(sc)) : NULL;
                exposes[i].endpoint = 0;
            }
            def->exposes = exposes;
            def->expose_count = (uint8_t)exp_count;
        }
    }

    /* No init callback, no tuya_driver, no reporting for runtime converters */
    def->init = NULL;
    def->tuya_driver = NULL;
    def->reporting = NULL;
    def->reporting_count = 0;

    return def;
}

/* ============================================================================
 * Internal: free a cached converter
 * ============================================================================ */

static void free_cached_entry(cache_entry_t *entry)
{
    if (entry->def) {
        /* fz/tz/expose arrays are separately allocated */
        if (entry->def->from_zigbee) {
            mem_ng_free((void *)entry->def->from_zigbee);
        }
        if (entry->def->to_zigbee) {
            mem_ng_free((void *)entry->def->to_zigbee);
        }
        if (entry->def->exposes) {
            mem_ng_free((void *)entry->def->exposes);
        }
        mem_ng_free(entry->def);
    }
    memset(entry, 0, sizeof(*entry));
}

/* ============================================================================
 * Internal: find filename for manufacturer
 * ============================================================================ */

static const char *find_filename(const char *manufacturer)
{
    if (manufacturer == NULL) return NULL;

    for (size_t i = 0; i < s_index_count; i++) {
        if (strcmp(s_index[i].manufacturer, manufacturer) == 0) {
            return s_index[i].filename;
        }
    }

    /* Try prefix match for Tuya (_TZE200, _TZ3000, etc.) */
    size_t mlen = strlen(manufacturer);
    if (mlen >= 4 && manufacturer[0] == '_') {
        for (size_t i = 0; i < s_index_count; i++) {
            size_t idx_len = strlen(s_index[i].manufacturer);
            if (idx_len <= mlen &&
                strncmp(s_index[i].manufacturer, manufacturer, idx_len) == 0) {
                return s_index[i].filename;
            }
        }
    }

    return NULL;
}

/* ============================================================================
 * Internal: search cache
 * ============================================================================ */

static const zb_converter_def_t *cache_lookup(const char *model)
{
    for (size_t i = 0; i < s_cache_count; i++) {
        if (s_cache[i].model && strcmp(s_cache[i].model, model) == 0) {
            return s_cache[i].def;
        }
    }
    return NULL;
}

/* ============================================================================
 * Internal: add to cache
 * ============================================================================ */

static void cache_add(const char *model, zb_converter_def_t *def)
{
    if (s_cache_count >= MAX_CACHED_CONVERTERS) {
        /* Evict oldest (index 0), shift everything down */
        /* Calculate bytes being evicted for accurate stats */
        zb_converter_def_t *evicted = s_cache[0].def;
        if (evicted) {
            size_t evicted_bytes = sizeof(zb_converter_def_t);
            evicted_bytes += evicted->from_zigbee_count * sizeof(zb_from_zigbee_t);
            evicted_bytes += evicted->to_zigbee_count * sizeof(zb_to_zigbee_t);
            evicted_bytes += evicted->expose_count * sizeof(zb_expose_t);
            if (s_cache_bytes >= evicted_bytes) {
                s_cache_bytes -= evicted_bytes;
            } else {
                s_cache_bytes = 0;
            }
        }
        free_cached_entry(&s_cache[0]);
        memmove(&s_cache[0], &s_cache[1],
                (MAX_CACHED_CONVERTERS - 1) * sizeof(cache_entry_t));
        s_cache_count--;
    }

    size_t entry_bytes = sizeof(zb_converter_def_t);
    entry_bytes += def->from_zigbee_count * sizeof(zb_from_zigbee_t);
    entry_bytes += def->to_zigbee_count * sizeof(zb_to_zigbee_t);
    entry_bytes += def->expose_count * sizeof(zb_expose_t);

    s_cache[s_cache_count].model = string_intern(model);
    s_cache[s_cache_count].def = def;
    s_cache_count++;
    s_cache_bytes += entry_bytes;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t zb_converter_loader_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create loader mutex");
        return ESP_ERR_NO_MEM;
    }

    if (!littlefs_is_mounted()) {
        ESP_LOGW(TAG, "LittleFS not mounted, converter DB unavailable");
        s_initialized = true;
        s_available = false;
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = load_index();
    s_initialized = true;
    s_available = (ret == ESP_OK);

    if (s_available) {
        ESP_LOGI(TAG, "Converter loader ready: %zu manufacturers indexed", s_index_count);
    } else {
        ESP_LOGW(TAG, "Converter DB not found or invalid (non-fatal)");
    }

    return ret;
}

esp_err_t zb_converter_loader_reload_index(void)
{
    if (!s_initialized) {
        return zb_converter_loader_init();
    }

    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for reload");
        return ESP_ERR_TIMEOUT;
    }

    /* Clear old index */
    s_index_count = 0;
    s_db_device_count = 0;

    xSemaphoreGive(s_mutex);

    esp_err_t ret = load_index();
    s_available = (ret == ESP_OK);

    if (s_available) {
        ESP_LOGI(TAG, "Converter DB reloaded: %zu manufacturers indexed", s_index_count);
    } else {
        ESP_LOGW(TAG, "Converter DB reload failed");
    }

    return ret;
}

bool zb_converter_loader_is_available(void)
{
    return s_available;
}

const zb_converter_def_t *zb_converter_loader_find(
    const char *manufacturer, const char *model)
{
    if (!s_initialized || !s_available || model == NULL || s_mutex == NULL) {
        return NULL;
    }

    if (xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire loader mutex for find");
        return NULL;
    }

    /* Check cache first */
    const zb_converter_def_t *cached = cache_lookup(model);
    if (cached != NULL) {
        xSemaphoreGive(s_mutex);
        ESP_LOGD(TAG, "Cache hit: %s", model);
        return cached;
    }

    /* Find the right file */
    const char *filename = find_filename(manufacturer);
    if (filename == NULL) {
        xSemaphoreGive(s_mutex);
        ESP_LOGD(TAG, "No DB file for manufacturer: %s", manufacturer ? manufacturer : "null");
        return NULL;
    }

    /* Build full path */
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", CONVERTER_DB_PATH, filename);

    /* Release mutex during file I/O */
    xSemaphoreGive(s_mutex);

    size_t file_len = 0;
    char *json_str = read_file_to_psram(path, &file_len);
    if (json_str == NULL) {
        ESP_LOGW(TAG, "Failed to read %s", path);
        return NULL;
    }

    cJSON *root = cJSON_Parse(json_str);
    mem_ng_free(json_str);

    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse %s", path);
        return NULL;
    }

    /* Search devices array for matching model */
    cJSON *devices = cJSON_GetObjectItem(root, "devices");
    if (devices == NULL || !cJSON_IsArray(devices)) {
        ESP_LOGW(TAG, "No 'devices' array in %s", path);
        cJSON_Delete(root);
        return NULL;
    }

    zb_converter_def_t *found_def = NULL;
    cJSON *dev_json = NULL;
    cJSON_ArrayForEach(dev_json, devices) {
        cJSON *m = cJSON_GetObjectItem(dev_json, "m");
        if (m && cJSON_IsString(m) && strcmp(cJSON_GetStringValue(m), model) == 0) {
            found_def = parse_device(dev_json);
            break;
        }
    }

    cJSON_Delete(root);

    if (found_def == NULL) {
        ESP_LOGD(TAG, "Model %s not found in %s", model, path);
        return NULL;
    }

    /* Re-acquire mutex to update cache */
    if (xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to re-acquire loader mutex — returning uncached");
        return found_def;
    }

    /* Double-check cache (another thread may have loaded it) */
    cached = cache_lookup(model);
    if (cached != NULL) {
        /* Another thread beat us — free our copy and return theirs */
        if (found_def->from_zigbee) mem_ng_free((void *)found_def->from_zigbee);
        if (found_def->to_zigbee) mem_ng_free((void *)found_def->to_zigbee);
        if (found_def->exposes) mem_ng_free((void *)found_def->exposes);
        mem_ng_free(found_def);
        xSemaphoreGive(s_mutex);
        return cached;
    }

    cache_add(model, found_def);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Loaded converter from LittleFS: %s (%s)",
             found_def->model ? found_def->model : model,
             found_def->description ? found_def->description : "");

    return found_def;
}

void zb_converter_loader_free(const zb_converter_def_t *def)
{
    /* Runtime converters are cached — don't free them individually.
     * This is a no-op; cleanup happens on cache eviction or shutdown. */
    (void)def;
}

bool zb_converter_loader_is_runtime(const zb_converter_def_t *def)
{
    if (def == NULL || s_mutex == NULL) return false;

    if (xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        return false;
    }
    for (size_t i = 0; i < s_cache_count; i++) {
        if (s_cache[i].def == def) {
            xSemaphoreGive(s_mutex);
            return true;
        }
    }
    xSemaphoreGive(s_mutex);
    return false;
}

void zb_converter_loader_get_stats(size_t *db_count, size_t *loaded, size_t *bytes_used)
{
    if (s_mutex == NULL) {
        if (db_count) *db_count = 0;
        if (loaded) *loaded = 0;
        if (bytes_used) *bytes_used = 0;
        return;
    }

    if (xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        return;
    }
    if (db_count) *db_count = s_db_device_count;
    if (loaded) *loaded = s_cache_count;
    if (bytes_used) *bytes_used = s_cache_bytes;
    xSemaphoreGive(s_mutex);
}
