/**
 * @file zb_converter_loader.c
 * @brief Runtime Converter Loader — JSON DB on LittleFS
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_converter_loader.h"
#include "zb_converter_fn_registry.h"
#include "zb_quirk_engine.h"
#include "core/littlefs_mount.h"
#include "core/memory/memory_manager_ng.h"
#include "core/memory/string_intern.h"
#include "core/gateway_timeouts.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "CONV_LOAD";

#define CONVERTER_DB_PATH       LITTLEFS_MOUNT_POINT "/converters"
#define INDEX_FILE_PATH         CONVERTER_DB_PATH "/index.json"
/* The converter database ships 447 manufacturer files; at 256 the index
 * silently stopped taking entries ("Index full at 256 entries") and those
 * devices simply never matched a converter. The table is 8 bytes per entry. */
/* Every manufacturer in the database needs a slot. Overrunning this is not an
 * error the gateway reports to anyone — the loader logs one line and the
 * manufacturers past the limit simply cannot be found, so their devices pair
 * and then sit there with no converter. It has happened twice: at 256 the
 * database had 1109 manufacturers and 853 were silently unreachable, and a
 * database with the fingerprint identities restored carries 2545, which would
 * have overrun 2048 the same way. 4096 leaves room; the table is 8 bytes an
 * entry in PSRAM, so the headroom costs 32 KB of memory that is otherwise
 * unused. */
#define MAX_INDEX_ENTRIES        4096

/** @brief Largest converter file the loader will read, in bytes */
#define CONVERTER_FILE_MAX_BYTES (512 * 1024)
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
    const char *manufacturer;       /* interned — part of the cache key */
    const char *model;              /* interned — part of the cache key */
    zb_converter_def_t *def;        /* heap-allocated definition */
} cache_entry_t;

/* ============================================================================
 * Static state
 * ============================================================================ */

static index_entry_t *s_index;   /* PSRAM, allocated in load_index() */
static size_t s_index_skipped = 0;
static size_t s_index_count = 0;
/** Revision string from index.json, for comparing against an update source. */
static char s_db_revision[40] = "";
static size_t s_db_device_count = 0;

static cache_entry_t s_cache[MAX_CACHED_CONVERTERS];
static size_t s_cache_count = 0;
static size_t s_cache_bytes = 0;

static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;
static bool s_available = false;

/* Quirk data associated with cached converter definitions */
#define MAX_QUIRK_DATA 32
static struct {
    const zb_converter_def_t *def;
    quirk_data_t *data;
} s_quirk_assoc[MAX_QUIRK_DATA];
static size_t s_quirk_assoc_count = 0;

/* ============================================================================
 * Quirk association helpers
 * ============================================================================ */

static void quirk_assoc_set(const zb_converter_def_t *def, quirk_data_t *data)
{
    if (s_quirk_assoc_count < MAX_QUIRK_DATA) {
        s_quirk_assoc[s_quirk_assoc_count].def = def;
        s_quirk_assoc[s_quirk_assoc_count].data = data;
        s_quirk_assoc_count++;
    }
}

quirk_data_t *zb_converter_loader_get_quirk_data(const zb_converter_def_t *def)
{
    for (size_t i = 0; i < s_quirk_assoc_count; i++) {
        if (s_quirk_assoc[i].def == def) return s_quirk_assoc[i].data;
    }
    return NULL;
}

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

    /* The ceiling is a sanity check on a corrupt filesystem, not a budget: the
     * buffer is PSRAM, of which six megabytes sit idle. It used to be 128 KB,
     * and index.json crossed it at 135934 bytes when the database grew to 2538
     * manufacturers. The result was one warning and a gateway where every
     * device fell back to a generic converter — an Aqara vibration sensor came
     * up as "Generic temperature/humidity sensor". Sized well clear of the
     * largest file the database produces (index.json, then the per-vendor files
     * at about 100 KB). */
    if (size <= 0 || size > CONVERTER_FILE_MAX_BYTES) {
        ESP_LOGE(TAG, "File %s: size %ld is outside the readable range (max %d)",
                 path, size, CONVERTER_FILE_MAX_BYTES);
        fclose(f);
        return NULL;
    }

    /* PSRAM, as the function name has always claimed.
     *
     * This allocated MEM_CAP_DEFAULT — internal RAM — for a buffer that may be
     * up to 128 KB. Measured on boot, loading the converter index drove
     * internal free heap from 190 KB down to 32 KB in 60 ms, which was the
     * firmware's low-water mark for the whole run. The data is read once,
     * parsed, and freed; it has no business in the scarce heap. */
    char *buf = mem_alloc((size_t)size + 1, MEM_CAP_PSRAM);
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
    if (s_index == NULL) {
        s_index = mem_ng_calloc(MAX_INDEX_ENTRIES, sizeof(index_entry_t), MEM_CAP_PSRAM);
        if (s_index == NULL) {
            ESP_LOGE(TAG, "Cannot allocate index table");
            return ESP_ERR_NO_MEM;
        }
    }

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

    /* Detect index version: v2 uses "version", v1 uses "v" */
    int index_version = 0;
    cJSON *ver2 = cJSON_GetObjectItem(root, "version");
    cJSON *ver1 = cJSON_GetObjectItem(root, "v");

    if (ver2 && cJSON_IsNumber(ver2)) {
        index_version = (int)cJSON_GetNumberValue(ver2);
    } else if (ver1 && cJSON_IsNumber(ver1)) {
        index_version = (int)cJSON_GetNumberValue(ver1);
    }

    if (index_version != 1 && index_version != 2) {
        ESP_LOGE(TAG, "Unsupported index version: %d", index_version);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_VERSION;
    }

    /* The revision this database was built at. Absent in older databases, which
     * is not an error — it only means an update source cannot be compared
     * against it and every check will report "unknown" rather than "current". */
    cJSON *rev = cJSON_GetObjectItem(root, "db_revision");
    if (rev && cJSON_IsString(rev) && cJSON_GetStringValue(rev)) {
        strlcpy(s_db_revision, cJSON_GetStringValue(rev), sizeof(s_db_revision));
    } else {
        s_db_revision[0] = '\0';
    }

    s_index_count = 0;

    if (index_version == 2) {
        /* v2 format: "manufacturers" object, "total_devices" */
        cJSON *total = cJSON_GetObjectItem(root, "total_devices");
        if (total && cJSON_IsNumber(total)) {
            s_db_device_count = (size_t)cJSON_GetNumberValue(total);
        }

        cJSON *manufacturers = cJSON_GetObjectItem(root, "manufacturers");
        if (manufacturers == NULL || !cJSON_IsObject(manufacturers)) {
            ESP_LOGE(TAG, "No 'manufacturers' object in v2 index.json");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }

        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, manufacturers) {
            if (s_index_count >= MAX_INDEX_ENTRIES) {
                ESP_LOGW(TAG, "Index full at %d entries", MAX_INDEX_ENTRIES);
                break;
            }
            if (!cJSON_IsObject(entry)) continue;

            const char *mfr = string_intern(entry->string);

            /* An entry without a usable name is not an index entry.
             *
             * string_intern() returns NULL for a name it cannot take — too
             * long, pool exhausted, or NULL to begin with — and storing that
             * NULL here is how a single bad record took the whole gateway
             * down. find_filename() walks this array with strcmp(), so one
             * NULL manufacturer means a load access fault on the very next
             * device lookup, every time, forever.
             *
             * Seen on 2026-08-22 while pairing: the merged database carries a
             * Legrand name padded to 146 characters with NUL bytes, which is
             * how those devices report themselves. The gateway crash-looped
             * and the sensor never got identified. */
            if (mfr == NULL) {
                s_index_skipped++;
                continue;
            }

            /* Check for "files" array (split manufacturers like LUMI → lumi_1..3) */
            cJSON *files_arr = cJSON_GetObjectItem(entry, "files");
            if (files_arr != NULL && cJSON_IsArray(files_arr)) {
                cJSON *f = NULL;
                cJSON_ArrayForEach(f, files_arr) {
                    if (s_index_count >= MAX_INDEX_ENTRIES) break;
                    if (!cJSON_IsString(f)) continue;
                    const char *fn = string_intern(cJSON_GetStringValue(f));
                    if (fn == NULL) { s_index_skipped++; continue; }
                    s_index[s_index_count].manufacturer = mfr;
                    s_index[s_index_count].filename = fn;
                    s_index_count++;
                }
                continue;
            }

            /* Single "file" string (non-split manufacturer) */
            cJSON *file_j = cJSON_GetObjectItem(entry, "file");
            if (file_j == NULL || !cJSON_IsString(file_j)) continue;

            const char *fn = string_intern(cJSON_GetStringValue(file_j));
            if (fn == NULL) { s_index_skipped++; continue; }
            s_index[s_index_count].manufacturer = mfr;
            s_index[s_index_count].filename = fn;
            s_index_count++;
        }
    } else {
        /* v1 format: "files" object, "count" */
        cJSON *count = cJSON_GetObjectItem(root, "count");
        if (count && cJSON_IsNumber(count)) {
            s_db_device_count = (size_t)cJSON_GetNumberValue(count);
        }

        cJSON *files = cJSON_GetObjectItem(root, "files");
        if (files == NULL || !cJSON_IsObject(files)) {
            ESP_LOGE(TAG, "No 'files' object in index.json");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }

        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, files) {
            if (s_index_count >= MAX_INDEX_ENTRIES) {
                ESP_LOGW(TAG, "Index full at %d entries", MAX_INDEX_ENTRIES);
                break;
            }
            if (!cJSON_IsString(entry)) continue;

            {
                const char *m1 = string_intern(entry->string);
                const char *f1 = string_intern(cJSON_GetStringValue(entry));
                if (m1 == NULL || f1 == NULL) { s_index_skipped++; continue; }
                s_index[s_index_count].manufacturer = m1;
                s_index[s_index_count].filename = f1;
                s_index_count++;
            }
        }
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

zb_converter_def_t *zb_converter_loader_parse_device(const cJSON *dev_json)
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
                cJSON *p = cJSON_GetObjectItem(item, "p");
                cJSON *ac = cJSON_GetObjectItem(item, "ac");
                cJSON *ep = cJSON_GetObjectItem(item, "ep");
                cJSON *dc = cJSON_GetObjectItem(item, "dc");
                cJSON *u = cJSON_GetObjectItem(item, "u");
                cJSON *sc = cJSON_GetObjectItem(item, "sc");
                /* "cat": Home Assistant entity category, mirroring
                 * zigbee2mqtt's .withCategory(). Absent for now on most
                 * entries; esphome_adapter_exposes.c falls back to a
                 * property-name heuristic when it is. */
                cJSON *cat = cJSON_GetObjectItem(item, "cat");

                exposes[i].type = (t && cJSON_IsNumber(t)) ?
                    map_expose_type((int)cJSON_GetNumberValue(t)) : ZB_EXPOSE_SENSOR;
                exposes[i].features = (f && cJSON_IsNumber(f)) ?
                    (uint32_t)cJSON_GetNumberValue(f) : 0;
                /* Fall back to the property when no display name is given.
                 *
                 * esphome_adapter.c skips any expose with a NULL name, so an
                 * entry carrying only "p" produced no entity at all — the NEO
                 * siren declared alarm, melody, duration, volume and
                 * battpercentage and Home Assistant got a switch and a battery
                 * sensor, both inferred from capability bits instead. Upstream
                 * an expose's name *is* its property unless one is set
                 * explicitly, so this reproduces their behaviour rather than
                 * inventing a name. */
                exposes[i].name = (n && cJSON_IsString(n))
                    ? string_intern(cJSON_GetStringValue(n))
                    : ((p && cJSON_IsString(p))
                        ? string_intern(cJSON_GetStringValue(p)) : NULL);
                exposes[i].property = (p && cJSON_IsString(p)) ?
                    string_intern(cJSON_GetStringValue(p)) : NULL;
                exposes[i].access = (ac && cJSON_IsNumber(ac)) ?
                    (uint8_t)cJSON_GetNumberValue(ac) : EA_STATE;
                exposes[i].endpoint = (ep && cJSON_IsNumber(ep)) ?
                    (uint8_t)cJSON_GetNumberValue(ep) : 0;
                exposes[i].device_class = (dc && cJSON_IsString(dc)) ?
                    string_intern(cJSON_GetStringValue(dc)) : NULL;
                exposes[i].unit = (u && cJSON_IsString(u)) ?
                    string_intern(cJSON_GetStringValue(u)) : NULL;
                exposes[i].state_class = (sc && cJSON_IsString(sc)) ?
                    string_intern(cJSON_GetStringValue(sc)) : NULL;
                exposes[i].category = (cat && cJSON_IsString(cat)) ?
                    string_intern(cJSON_GetStringValue(cat)) : NULL;

                cJSON *icon_j = cJSON_GetObjectItem(item, "icon");
                cJSON *desc_exp = cJSON_GetObjectItem(item, "desc");
                exposes[i].icon = (icon_j && cJSON_IsString(icon_j)) ?
                    string_intern(cJSON_GetStringValue(icon_j)) : NULL;
                exposes[i].description = (desc_exp && cJSON_IsString(desc_exp)) ?
                    string_intern(cJSON_GetStringValue(desc_exp)) : NULL;

                /* Parse type-specific extension data */
                cJSON *num_j = cJSON_GetObjectItem(item, "num");
                if (num_j && cJSON_IsObject(num_j)) {
                    cJSON *vmin = cJSON_GetObjectItem(num_j, "min");
                    cJSON *vmax = cJSON_GetObjectItem(num_j, "max");
                    cJSON *vstep = cJSON_GetObjectItem(num_j, "step");
                    exposes[i].ext.numeric.min = (vmin && cJSON_IsNumber(vmin)) ?
                        (float)cJSON_GetNumberValue(vmin) : 0.0f;
                    exposes[i].ext.numeric.max = (vmax && cJSON_IsNumber(vmax)) ?
                        (float)cJSON_GetNumberValue(vmax) : 0.0f;
                    exposes[i].ext.numeric.step = (vstep && cJSON_IsNumber(vstep)) ?
                        (float)cJSON_GetNumberValue(vstep) : 0.0f;
                }

                cJSON *sel_j = cJSON_GetObjectItem(item, "sel");
                if (sel_j && cJSON_IsObject(sel_j)) {
                    cJSON *vals = cJSON_GetObjectItem(sel_j, "v");
                    if (vals && cJSON_IsArray(vals)) {
                        int vcount = cJSON_GetArraySize(vals);
                        if (vcount > 0 && vcount <= 32) {
                            const char **values = mem_ng_calloc((size_t)vcount,
                                sizeof(const char *), MEM_CAP_DEFAULT);
                            if (values) {
                                for (int vi = 0; vi < vcount; vi++) {
                                    cJSON *v = cJSON_GetArrayItem(vals, vi);
                                    values[vi] = (v && cJSON_IsString(v)) ?
                                        string_intern(cJSON_GetStringValue(v)) : "";
                                }
                                exposes[i].ext.select.values = values;
                                exposes[i].ext.select.count = (uint8_t)vcount;
                            }
                        }
                    }
                }

                cJSON *bin_j = cJSON_GetObjectItem(item, "bin");
                if (bin_j && cJSON_IsObject(bin_j)) {
                    cJSON *von = cJSON_GetObjectItem(bin_j, "von");
                    cJSON *voff = cJSON_GetObjectItem(bin_j, "voff");
                    exposes[i].ext.binary.val_on = (von && cJSON_IsString(von)) ?
                        string_intern(cJSON_GetStringValue(von)) : NULL;
                    exposes[i].ext.binary.val_off = (voff && cJSON_IsString(voff)) ?
                        string_intern(cJSON_GetStringValue(voff)) : NULL;
                }
            }
            def->exposes = exposes;
            def->expose_count = (uint8_t)exp_count;
        }
    }

    /* Parse quirks */
    cJSON *quirks_obj = cJSON_GetObjectItem(dev_json, "quirks");
    if (quirks_obj && cJSON_IsObject(quirks_obj)) {
        quirk_data_t *qd = quirk_data_parse(quirks_obj);
        if (qd) {
            quirk_assoc_set(def, qd);
        }
    }

    /* Parse tuya_dp map: {"1": {"k":"state","t":"bool"}, "2": {"k":"mode","t":"enum","v":{"auto":0}}} */
    cJSON *tuya_dp = cJSON_GetObjectItem(dev_json, "tuya_dp");
    if (tuya_dp && cJSON_IsObject(tuya_dp)) {
        int dp_count = 0;
        {   /* Count children explicitly (cJSON_GetArraySize is for arrays) */
            cJSON *_c = NULL;
            cJSON_ArrayForEach(_c, tuya_dp) { dp_count++; }
        }
        if (dp_count > 0 && dp_count <= 32) {
            tuya_dp_entry_t *entries = mem_ng_calloc((size_t)dp_count,
                sizeof(tuya_dp_entry_t), MEM_CAP_DEFAULT);
            if (entries) {
                int valid = 0;
                cJSON *dp_item = NULL;
                cJSON_ArrayForEach(dp_item, tuya_dp) {
                    /* dp_item->string is the DP ID as string ("1", "2", etc.) */
                    int dp_id = atoi(dp_item->string);
                    if (dp_id <= 0 || dp_id > 255 || !cJSON_IsObject(dp_item)) continue;

                    entries[valid].dp_id = (uint8_t)dp_id;

                    /* Key ("k") */
                    cJSON *k_j = cJSON_GetObjectItem(dp_item, "k");
                    if (k_j && cJSON_IsString(k_j)) {
                        entries[valid].key = string_intern(cJSON_GetStringValue(k_j));
                    } else {
                        continue;  /* key is required */
                    }

                    /* Type ("t") -> dp_type */
                    cJSON *t_j = cJSON_GetObjectItem(dp_item, "t");
                    if (t_j && cJSON_IsString(t_j)) {
                        const char *ts = cJSON_GetStringValue(t_j);
                        if (strcmp(ts, "bool") == 0)        entries[valid].dp_type = 1;
                        else if (strcmp(ts, "int") == 0)    entries[valid].dp_type = 2;
                        else if (strcmp(ts, "str") == 0)    entries[valid].dp_type = 3;
                        else if (strcmp(ts, "enum") == 0)   entries[valid].dp_type = 4;
                        else if (strcmp(ts, "bitmap") == 0) entries[valid].dp_type = 5;
                        else                                entries[valid].dp_type = 0;
                    }

                    /* Scale, min, max */
                    cJSON *scale_j = cJSON_GetObjectItem(dp_item, "scale");
                    if (scale_j && cJSON_IsNumber(scale_j)) {
                        entries[valid].scale = (float)cJSON_GetNumberValue(scale_j);
                    }
                    cJSON *min_j = cJSON_GetObjectItem(dp_item, "min");
                    if (min_j && cJSON_IsNumber(min_j)) {
                        entries[valid].min = (float)cJSON_GetNumberValue(min_j);
                    }
                    cJSON *max_j = cJSON_GetObjectItem(dp_item, "max");
                    if (max_j && cJSON_IsNumber(max_j)) {
                        entries[valid].max = (float)cJSON_GetNumberValue(max_j);
                    }

                    /* Enum values ("v"): {"auto":0, "heat":1, "off":2} */
                    cJSON *v_j = cJSON_GetObjectItem(dp_item, "v");
                    if (v_j && cJSON_IsObject(v_j)) {
                        int vcount = 0;
                        { cJSON *_vc = NULL; cJSON_ArrayForEach(_vc, v_j) { vcount++; } }
                        if (vcount > 0 && vcount <= 32) {
                            const char **names = mem_ng_calloc((size_t)vcount,
                                sizeof(const char *), MEM_CAP_DEFAULT);
                            uint8_t *values = mem_ng_calloc((size_t)vcount,
                                sizeof(uint8_t), MEM_CAP_DEFAULT);
                            if (names && values) {
                                int vi = 0;
                                cJSON *v_entry = NULL;
                                cJSON_ArrayForEach(v_entry, v_j) {
                                    if (vi >= vcount) break;
                                    names[vi] = string_intern(v_entry->string);
                                    values[vi] = (uint8_t)cJSON_GetNumberValue(v_entry);
                                    vi++;
                                }
                                entries[valid].enum_names = names;
                                entries[valid].enum_values = values;
                                entries[valid].enum_count = (uint8_t)vi;
                            } else {
                                /* Both must succeed — free whichever was allocated */
                                if (names) mem_ng_free((void *)names);
                                if (values) mem_ng_free(values);
                                /* Skip this DP entry entirely on OOM */
                                continue;
                            }
                        }
                    }

                    valid++;
                }
                def->tuya_dp_map = entries;
                def->tuya_dp_count = (uint8_t)valid;
            }
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
            /* Free select option arrays within exposes */
            for (uint8_t i = 0; i < entry->def->expose_count; i++) {
                const zb_expose_t *e = &entry->def->exposes[i];
                if (e->type == ZB_EXPOSE_SELECT && e->ext.select.values) {
                    mem_ng_free((void *)e->ext.select.values);
                }
            }
            mem_ng_free((void *)entry->def->exposes);
        }
        /* Free Tuya DP map entries */
        if (entry->def->tuya_dp_map) {
            for (uint8_t i = 0; i < entry->def->tuya_dp_count; i++) {
                const tuya_dp_entry_t *dp = &entry->def->tuya_dp_map[i];
                if (dp->enum_names) mem_ng_free((void *)dp->enum_names);
                if (dp->enum_values) mem_ng_free((void *)dp->enum_values);
            }
            mem_ng_free((void *)entry->def->tuya_dp_map);
        }
        mem_ng_free(entry->def);
    }
    memset(entry, 0, sizeof(*entry));
}

/* ============================================================================
 * Public: Free a standalone parsed converter definition
 * ============================================================================ */

void zb_converter_loader_free_def(zb_converter_def_t *def)
{
    if (!def) return;

    if (def->from_zigbee) mem_ng_free((void *)def->from_zigbee);
    if (def->to_zigbee)   mem_ng_free((void *)def->to_zigbee);
    if (def->exposes) {
        for (uint8_t i = 0; i < def->expose_count; i++) {
            const zb_expose_t *e = &def->exposes[i];
            if (e->type == ZB_EXPOSE_SELECT && e->ext.select.values) {
                mem_ng_free((void *)e->ext.select.values);
            }
        }
        mem_ng_free((void *)def->exposes);
    }
    if (def->tuya_dp_map) {
        for (uint8_t i = 0; i < def->tuya_dp_count; i++) {
            const tuya_dp_entry_t *dp = &def->tuya_dp_map[i];
            if (dp->enum_names) mem_ng_free((void *)dp->enum_names);
            if (dp->enum_values) mem_ng_free((void *)dp->enum_values);
        }
        mem_ng_free((void *)def->tuya_dp_map);
    }
    mem_ng_free(def);
}

/* ============================================================================
 * Internal: find filename for manufacturer
 * ============================================================================ */

static const char *find_filename(const char *manufacturer)
{
    if (manufacturer == NULL) return NULL;

    /* Exact match */
    for (size_t i = 0; i < s_index_count; i++) {
        if (s_index[i].manufacturer == NULL) continue;
        if (strcmp(s_index[i].manufacturer, manufacturer) == 0) {
            return s_index[i].filename;
        }
    }

    /* Case-insensitive match (e.g. "LUMI" vs "lumi", "innr" vs "Innr") */
    for (size_t i = 0; i < s_index_count; i++) {
        if (s_index[i].manufacturer == NULL) continue;
        if (strcasecmp(s_index[i].manufacturer, manufacturer) == 0) {
            return s_index[i].filename;
        }
    }

    /* Try prefix match for Tuya (_TZE200, _TZ3000, etc.) */
    size_t mlen = strlen(manufacturer);
    if (mlen >= 4 && manufacturer[0] == '_') {
        for (size_t i = 0; i < s_index_count; i++) {
            if (s_index[i].manufacturer == NULL) continue;
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

static bool cache_key_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

/**
 * @brief Look a converter up by manufacturer and model
 *
 * Both halves are the key. Keyed on the model alone, the first Tuya "TS0601"
 * to be looked up answered for every other one — and that is most of the Tuya
 * catalogue.
 */
static const zb_converter_def_t *cache_lookup(const char *manufacturer, const char *model)
{
    for (size_t i = 0; i < s_cache_count; i++) {
        if (s_cache[i].model && strcmp(s_cache[i].model, model) == 0 &&
            cache_key_equal(s_cache[i].manufacturer, manufacturer)) {
            return s_cache[i].def;
        }
    }
    return NULL;
}

/* ============================================================================
 * Internal: add to cache
 * ============================================================================ */

static void cache_add(const char *manufacturer, const char *model,
                      zb_converter_def_t *def)
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
            evicted_bytes += evicted->tuya_dp_count * sizeof(tuya_dp_entry_t);
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
    entry_bytes += def->tuya_dp_count * sizeof(tuya_dp_entry_t);

    s_cache[s_cache_count].manufacturer =
        (manufacturer != NULL && manufacturer[0] != '\0') ? string_intern(manufacturer) : NULL;
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
        /* "non-fatal" is true of the boot and misleading about everything
         * else: without the database every device that pairs gets a generic
         * converter chosen from its capability bits, so a vibration sensor
         * comes up as a temperature sensor and a siren as an on/off light.
         * Worth an error, not a warning. */
        ESP_LOGE(TAG, "Converter database unavailable — every device will fall "
                 "back to a generic converter");
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

/**
 * @brief Load and search a single JSON file for a model.
 * @return Parsed converter definition, or NULL if not found.
 */
/**
 * @brief Does this entry's manufacturer field match the device's?
 *
 * The database mixes exact ids ("_TZE204_rkbxtclc") with prefixes ("_TZE200"),
 * which is why an entry is allowed to be a prefix of what the device reports.
 */
static bool manufacturer_matches(const char *entry_mf, const char *manufacturer, bool exact_only)
{
    if (entry_mf == NULL || manufacturer == NULL || manufacturer[0] == '\0') {
        return false;
    }
    if (strcmp(entry_mf, manufacturer) == 0) {
        return true;
    }
    if (exact_only) {
        return false;
    }
    size_t elen = strlen(entry_mf);
    return elen >= 4 && entry_mf[0] == '_' && strncmp(entry_mf, manufacturer, elen) == 0;
}


/** @brief Profiles per profiles_N.json, mirroring tools/merge_converter_dbs.py */
#define PROFILES_PER_FILE 256

/**
 * @brief Attach a device's shared behaviour profile to its JSON entry
 *
 * 8321 devices in the database share 1962 behaviours — the same converters,
 * exposes and datapoint maps repeated for every manufacturer id selling the
 * same hardware. Written out per device that was 5844 KB and the database sat
 * at 96.4 % of its partition; stored once and referenced it is 1732 KB.
 *
 * A device entry therefore carries what identifies it and a profile id:
 *
 *     {"m":"TS0601","mf":"_TZE204_t1blo2bj","v":"NEO","d":"Alarm","p":417}
 *
 * The file follows from the id, so no second index is needed. The members are
 * copied into the device object before parsing, which keeps
 * zb_converter_loader_parse_device() unaware that any of this happens.
 *
 * A device with its behaviour inline still works: without "p" this does
 * nothing.
 *
 * @return true if a profile was attached or none was needed
 */
static bool attach_profile(cJSON *dev_json)
{
    cJSON *pid_j = cJSON_GetObjectItem(dev_json, "p");
    if (pid_j == NULL || !cJSON_IsNumber(pid_j)) {
        return true;   /* behaviour is inline */
    }

    int pid = (int)cJSON_GetNumberValue(pid_j);
    if (pid < 0) {
        return false;
    }

    char path[80];
    snprintf(path, sizeof(path), "%s/profiles_%d.json",
             CONVERTER_DB_PATH, pid / PROFILES_PER_FILE);

    size_t len = 0;
    char *json_str = read_file_to_psram(path, &len);
    if (json_str == NULL) {
        ESP_LOGW(TAG, "Profile %d: %s is missing", pid, path);
        return false;
    }

    cJSON *root = cJSON_Parse(json_str);
    mem_ng_free(json_str);
    if (root == NULL) {
        ESP_LOGW(TAG, "Profile %d: %s does not parse", pid, path);
        return false;
    }

    cJSON *first_j = cJSON_GetObjectItem(root, "first");
    cJSON *list = cJSON_GetObjectItem(root, "profiles");
    int first = (first_j && cJSON_IsNumber(first_j)) ? (int)cJSON_GetNumberValue(first_j) : 0;

    cJSON *profile = (list && cJSON_IsArray(list))
                     ? cJSON_GetArrayItem(list, pid - first) : NULL;
    if (profile == NULL) {
        ESP_LOGW(TAG, "Profile %d not in %s", pid, path);
        cJSON_Delete(root);
        return false;
    }

    bool ok = true;
    cJSON *member = NULL;
    cJSON_ArrayForEach(member, profile) {
        if (member->string == NULL || cJSON_GetObjectItem(dev_json, member->string)) {
            continue;   /* an inline value on the device wins */
        }
        cJSON *copy = cJSON_Duplicate(member, true);
        if (copy == NULL) {
            ok = false;
            break;
        }
        cJSON_AddItemToObject(dev_json, member->string, copy);
    }

    cJSON_Delete(root);
    return ok;
}

static zb_converter_def_t *load_model_from_file(const char *path,
                                                const char *manufacturer,
                                                const char *model)
{
    size_t file_len = 0;
    char *json_str = read_file_to_psram(path, &file_len);
    if (json_str == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(json_str);
    mem_ng_free(json_str);
    if (root == NULL) {
        return NULL;
    }

    cJSON *devices = cJSON_GetObjectItem(root, "devices");
    if (devices == NULL || !cJSON_IsArray(devices)) {
        cJSON_Delete(root);
        return NULL;
    }

    /* Three passes, most specific first.
     *
     * Matching on the model alone was enough to bind the wrong device: Tuya
     * ships hundreds of unrelated products — thermostats, blind motors,
     * sensors, switches — all reporting model "TS0601", and they are told apart
     * only by the manufacturer id. _other_16.json holds both _TZE204_rkbxtclc
     * and _TZE204_t1blo2bj under that model, so whichever came first in the
     * array won for both, and a device was presented in Home Assistant as
     * something it is not.
     *
     * The manufacturer picked the file and was then discarded. It is used here
     * too now — but the model-only pass stays as a last resort, because the
     * index maps some devices to a file through a vendor name the device never
     * reports, and those matches were working. */
    zb_converter_def_t *found = NULL;
    cJSON *dev_json = NULL;

    for (int pass = 0; pass < 3 && found == NULL; pass++) {
        cJSON_ArrayForEach(dev_json, devices) {
            cJSON *m = cJSON_GetObjectItem(dev_json, "m");
            if (m == NULL || !cJSON_IsString(m) ||
                strcmp(cJSON_GetStringValue(m), model) != 0) {
                continue;
            }

            if (pass < 2) {
                cJSON *mf = cJSON_GetObjectItem(dev_json, "mf");
                const char *entry_mf = (mf && cJSON_IsString(mf))
                                       ? cJSON_GetStringValue(mf) : NULL;
                if (!manufacturer_matches(entry_mf, manufacturer, pass == 0)) {
                    continue;
                }
            }

            if (!attach_profile(dev_json)) {
                ESP_LOGW(TAG, "Could not attach the profile for %s/%s", manufacturer ? manufacturer : "?", model);
            }
            found = zb_converter_loader_parse_device(dev_json);
            if (found != NULL) {
                if (pass == 2 && manufacturer != NULL && manufacturer[0] != '\0') {
                    ESP_LOGW(TAG, "No entry for manufacturer '%s' with model '%s' — "
                             "using the first '%s' in %s instead",
                             manufacturer, model, model, path);
                }
                break;
            }
        }
    }

    cJSON_Delete(root);
    return found;
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
    const zb_converter_def_t *cached = cache_lookup(manufacturer, model);
    if (cached != NULL) {
        xSemaphoreGive(s_mutex);
        ESP_LOGD(TAG, "Cache hit: %s", model);
        return cached;
    }

    /* Find the right file by manufacturer name */
    const char *filename = find_filename(manufacturer);
    xSemaphoreGive(s_mutex);

    zb_converter_def_t *found_def = NULL;

    if (filename != NULL) {
        /* Direct lookup: load manufacturer's file and search for model */
        char path[80];
        snprintf(path, sizeof(path), "%s/%s", CONVERTER_DB_PATH, filename);
        found_def = load_model_from_file(path, manufacturer, model);
    }

    /* Fallback: manufacturer known but not in index, or model not in expected file.
     * Scan ALL index files — happens at most once per device (then cached).
     * This handles z2m vendor/Zigbee manufacturer mismatches (e.g. the z2m
     * vendor "Aqara" maps to file aqara.json, but the device reports "LUMI"
     * which isn't in the index, even though the model IS in aqara.json).
     *
     * IMPORTANT: Only brute-force when manufacturer is known (non-empty).
     * Empty manufacturer means the device hasn't been fully interviewed yet —
     * the converter will be found once the interview provides the manufacturer.
     * Brute-forcing with empty manufacturer blocks the Zigbee callback thread
     * and triggers the task watchdog on single-core devices like ESP32-C5. */
    bool has_manufacturer = (manufacturer != NULL && manufacturer[0] != '\0');
    if (found_def == NULL && has_manufacturer) {
        if (filename != NULL) {
            ESP_LOGD(TAG, "Model %s not in %s, trying brute-force", model, filename);
        } else {
            ESP_LOGI(TAG, "Manufacturer '%s' not in index, brute-force for %s",
                     manufacturer, model);
        }

        if (xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
            return NULL;
        }
        /* Collect unique filenames from index */
        const char *tried = filename;  /* skip file we already searched */
        for (size_t i = 0; i < s_index_count && found_def == NULL; i++) {
            const char *fn = s_index[i].filename;
            if (fn == NULL || (tried && strcmp(fn, tried) == 0)) {
                continue;
            }
            /* Avoid re-scanning same file (multiple manufacturers → same file) */
            bool dup = false;
            for (size_t j = 0; j < i; j++) {
                if (s_index[j].filename && strcmp(s_index[j].filename, fn) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            xSemaphoreGive(s_mutex);

            char path[80];
            snprintf(path, sizeof(path), "%s/%s", CONVERTER_DB_PATH, fn);
            found_def = load_model_from_file(path, manufacturer, model);

            if (xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
                return found_def;  /* Got it but can't cache — still return */
            }
        }
        xSemaphoreGive(s_mutex);

        if (found_def != NULL) {
            ESP_LOGI(TAG, "Brute-force found %s (mfr=%s)", model, manufacturer);
        }
    }

    if (found_def == NULL) {
        ESP_LOGD(TAG, "Model %s not found in DB", model);
        return NULL;
    }

    /* Re-acquire mutex to update cache */
    if (xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to re-acquire loader mutex — returning uncached");
        return found_def;
    }

    /* Double-check cache (another thread may have loaded it) */
    cached = cache_lookup(manufacturer, model);
    if (cached != NULL) {
        /* Another thread beat us — free our copy and return theirs */
        if (found_def->from_zigbee) mem_ng_free((void *)found_def->from_zigbee);
        if (found_def->to_zigbee) mem_ng_free((void *)found_def->to_zigbee);
        if (found_def->exposes) mem_ng_free((void *)found_def->exposes);
        if (found_def->tuya_dp_map) {
            for (uint8_t i = 0; i < found_def->tuya_dp_count; i++) {
                const tuya_dp_entry_t *dp = &found_def->tuya_dp_map[i];
                if (dp->enum_names) mem_ng_free((void *)dp->enum_names);
                if (dp->enum_values) mem_ng_free((void *)dp->enum_values);
            }
            mem_ng_free((void *)found_def->tuya_dp_map);
        }
        mem_ng_free(found_def);
        xSemaphoreGive(s_mutex);
        return cached;
    }

    cache_add(manufacturer, model, found_def);
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

const char *zb_converter_loader_get_revision(void)
{
    return s_db_revision;
}
