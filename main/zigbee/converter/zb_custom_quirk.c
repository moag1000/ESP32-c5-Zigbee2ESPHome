/**
 * @file zb_custom_quirk.c
 * @brief Custom Community Quirks — User-defined device definitions
 *
 * Storage layout:
 *   /littlefs/converters/custom/<name>.json
 *
 * Each file contains a single device definition in the same compact JSON
 * format used by the main converter DB (keys: "m", "mf", "fz", "tz", "e", etc.).
 *
 * In-memory index holds parsed converter definitions (max 16).
 * Custom quirks have highest priority — checked before DB and hardcoded.
 */

#include "zb_custom_quirk.h"
#include "zb_converter_loader.h"
#include "zb_converter.h"
#include "core/device/device_registry.h"
#include "core/device/unified_device.h"
#include "core/memory/memory_manager_ng.h"
#include "core/littlefs_mount.h"
#include "esphome/esphome_api.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "CUSTOM_Q";

#define CUSTOM_QUIRK_DIR    LITTLEFS_MOUNT_POINT "/converters/custom"
#define MAX_FILE_SIZE       4096

/* ============================================================================
 * In-Memory Index
 * ============================================================================ */

typedef struct {
    char name[32];                  /**< Quirk name (filename without .json) */
    zb_converter_def_t *def;        /**< Parsed converter definition */
} custom_quirk_entry_t;

static custom_quirk_entry_t s_quirks[ZB_CUSTOM_QUIRK_MAX];
static size_t s_quirk_count = 0;
static bool s_initialized = false;

/* ============================================================================
 * Internal: Find entry by name
 * ============================================================================ */

static int find_by_name(const char *name)
{
    for (size_t i = 0; i < s_quirk_count; i++) {
        if (strcmp(s_quirks[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ============================================================================
 * Internal: Free a quirk entry
 * ============================================================================ */

static void free_entry(size_t idx)
{
    if (idx >= s_quirk_count) return;
    if (s_quirks[idx].def) {
        zb_converter_loader_free_def(s_quirks[idx].def);
        s_quirks[idx].def = NULL;
    }
    /* Shift remaining entries down */
    for (size_t i = idx; i + 1 < s_quirk_count; i++) {
        s_quirks[i] = s_quirks[i + 1];
    }
    s_quirk_count--;
    memset(&s_quirks[s_quirk_count], 0, sizeof(custom_quirk_entry_t));
}

/* ============================================================================
 * Internal: Load a single quirk file
 * ============================================================================ */

static esp_err_t load_quirk_file(const char *name, const char *path)
{
    if (s_quirk_count >= ZB_CUSTOM_QUIRK_MAX) {
        ESP_LOGW(TAG, "Max custom quirks reached (%d), skipping %s",
                 ZB_CUSTOM_QUIRK_MAX, name);
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > MAX_FILE_SIZE) {
        fclose(f);
        ESP_LOGW(TAG, "File %s size %ld invalid (max %d)", path, fsize, MAX_FILE_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = mem_alloc((size_t)fsize + 1, MEM_CAP_DEFAULT);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[read] = '\0';

    /* Parse JSON */
    cJSON *json = cJSON_Parse(buf);
    mem_ng_free(buf);

    if (!json) {
        ESP_LOGW(TAG, "Invalid JSON in %s", path);
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse device definition using shared parser */
    zb_converter_def_t *def = zb_converter_loader_parse_device(json);
    cJSON_Delete(json);

    if (!def) {
        ESP_LOGW(TAG, "Failed to parse device in %s", path);
        return ESP_ERR_INVALID_ARG;
    }

    /* Add to index */
    custom_quirk_entry_t *entry = &s_quirks[s_quirk_count];
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->def = def;
    s_quirk_count++;

    ESP_LOGI(TAG, "Loaded custom quirk '%s': %s %s",
             name,
             def->manufacturer ? def->manufacturer : "?",
             def->model ? def->model : "?");

    return ESP_OK;
}

/* ============================================================================
 * Init: scan directory for existing quirks
 * ============================================================================ */

esp_err_t zb_custom_quirk_init(void)
{
    if (s_initialized) return ESP_OK;

    s_quirk_count = 0;
    memset(s_quirks, 0, sizeof(s_quirks));

    /* Create directory if it doesn't exist */
    mkdir(CUSTOM_QUIRK_DIR, 0755);

    DIR *dir = opendir(CUSTOM_QUIRK_DIR);
    if (!dir) {
        ESP_LOGI(TAG, "No custom quirks directory, starting empty");
        s_initialized = true;
        return ESP_OK;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        /* Only process .json files */
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6) continue;
        if (strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;

        /* Extract name without extension */
        char name[32];
        size_t name_len = nlen - 5;
        if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
        memcpy(name, ent->d_name, name_len);
        name[name_len] = '\0';

        /* Skip overly long filenames */
        if (nlen > 40) continue;

        char path[72];
        snprintf(path, sizeof(path), "%s/%.40s", CUSTOM_QUIRK_DIR, ent->d_name);

        load_quirk_file(name, path);

        if (s_quirk_count >= ZB_CUSTOM_QUIRK_MAX) break;
    }

    closedir(dir);
    s_initialized = true;

    ESP_LOGI(TAG, "Custom quirks initialized: %zu loaded", s_quirk_count);
    return ESP_OK;
}

/* ============================================================================
 * Find: lookup by manufacturer + model
 * ============================================================================ */

const zb_converter_def_t *zb_custom_quirk_find(
    const char *manufacturer, const char *model)
{
    if (!model || model[0] == '\0') return NULL;

    for (size_t i = 0; i < s_quirk_count; i++) {
        const zb_converter_def_t *def = s_quirks[i].def;
        if (!def || !def->model) continue;

        /* Exact manufacturer + model match */
        if (manufacturer && manufacturer[0] != '\0' &&
            def->manufacturer && strcmp(def->manufacturer, manufacturer) == 0 &&
            strcmp(def->model, model) == 0) {
            return def;
        }

        /* Model-only match (for devices without manufacturer) */
        if (strcmp(def->model, model) == 0) {
            return def;
        }
    }

    return NULL;
}

/* ============================================================================
 * Add: parse JSON, write to LittleFS, add to index
 * ============================================================================ */

esp_err_t zb_custom_quirk_add(const char *name, const char *json_str)
{
    if (!name || !json_str || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate name: no path chars, max 31 chars */
    if (strlen(name) > 31 || strchr(name, '/') || strchr(name, '\\') || strcmp(name, "..") == 0) {
        ESP_LOGW(TAG, "Invalid quirk name: %s", name);
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse and validate JSON first */
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        ESP_LOGW(TAG, "Invalid JSON for quirk '%s'", name);
        return ESP_ERR_INVALID_ARG;
    }

    zb_converter_def_t *def = zb_converter_loader_parse_device(json);
    cJSON_Delete(json);

    if (!def) {
        ESP_LOGW(TAG, "Failed to parse device definition for quirk '%s'", name);
        return ESP_ERR_INVALID_ARG;
    }

    if (!def->model || def->model[0] == '\0') {
        ESP_LOGW(TAG, "Quirk '%s' missing model field", name);
        zb_converter_loader_free_def(def);
        return ESP_ERR_INVALID_ARG;
    }

    /* Save manufacturer/model for hot-reload before replacing */
    const char *mfr = def->manufacturer;
    const char *mdl = def->model;

    /* Remove existing entry with same name */
    int existing = find_by_name(name);
    if (existing >= 0) {
        free_entry((size_t)existing);
    }

    /* Check capacity */
    if (s_quirk_count >= ZB_CUSTOM_QUIRK_MAX) {
        ESP_LOGW(TAG, "Max custom quirks reached (%d)", ZB_CUSTOM_QUIRK_MAX);
        zb_converter_loader_free_def(def);
        return ESP_ERR_NO_MEM;
    }

    /* Write JSON to LittleFS */
    mkdir(CUSTOM_QUIRK_DIR, 0755);

    char path[80];
    snprintf(path, sizeof(path), "%s/%s.json", CUSTOM_QUIRK_DIR, name);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write %s", path);
        zb_converter_loader_free_def(def);
        return ESP_FAIL;
    }

    size_t json_len = strlen(json_str);
    size_t written = fwrite(json_str, 1, json_len, f);
    fclose(f);

    if (written != json_len) {
        ESP_LOGE(TAG, "Short write to %s", path);
        zb_converter_loader_free_def(def);
        return ESP_FAIL;
    }

    /* Add to in-memory index */
    custom_quirk_entry_t *entry = &s_quirks[s_quirk_count];
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->def = def;
    s_quirk_count++;

    ESP_LOGI(TAG, "Added custom quirk '%s': %s %s",
             name, mfr ? mfr : "?", mdl);

    /* Hot-reload active devices */
    zb_custom_quirk_hot_reload(mfr, mdl);

    return ESP_OK;
}

/* ============================================================================
 * Remove: delete file, remove from index
 * ============================================================================ */

esp_err_t zb_custom_quirk_remove(const char *name)
{
    if (!name || name[0] == '\0') return ESP_ERR_INVALID_ARG;

    int idx = find_by_name(name);
    if (idx < 0) {
        ESP_LOGW(TAG, "Custom quirk '%s' not found", name);
        return ESP_ERR_NOT_FOUND;
    }

    /* Save mfr/model for hot-reload before freeing */
    const char *mfr = s_quirks[idx].def ? s_quirks[idx].def->manufacturer : NULL;
    const char *mdl = s_quirks[idx].def ? s_quirks[idx].def->model : NULL;

    /* Copy strings since they'll be freed */
    char mfr_buf[64] = {0};
    char mdl_buf[64] = {0};
    if (mfr) strncpy(mfr_buf, mfr, sizeof(mfr_buf) - 1);
    if (mdl) strncpy(mdl_buf, mdl, sizeof(mdl_buf) - 1);

    /* Remove from index */
    free_entry((size_t)idx);

    /* Delete file */
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.json", CUSTOM_QUIRK_DIR, name);
    remove(path);

    ESP_LOGI(TAG, "Removed custom quirk '%s'", name);

    /* Hot-reload: re-bind affected devices to next-best converter */
    if (mdl_buf[0] != '\0') {
        zb_custom_quirk_hot_reload(mfr_buf[0] ? mfr_buf : NULL, mdl_buf);
    }

    return ESP_OK;
}

/* ============================================================================
 * List: enumerate all custom quirks
 * ============================================================================ */

size_t zb_custom_quirk_list(zb_custom_quirk_info_t *out, size_t max_count)
{
    size_t count = 0;
    for (size_t i = 0; i < s_quirk_count && count < max_count; i++) {
        if (!s_quirks[i].def) continue;

        out[count].name = s_quirks[i].name;
        out[count].manufacturer = s_quirks[i].def->manufacturer;
        out[count].model = s_quirks[i].def->model;
        out[count].description = s_quirks[i].def->description;
        count++;
    }
    return count;
}

size_t zb_custom_quirk_count(void)
{
    return s_quirk_count;
}

/* ============================================================================
 * Hot-Reload: re-bind active devices + force ESPHome reconnect
 * ============================================================================ */

typedef struct {
    const char *manufacturer;
    const char *model;
    uint16_t rebind_count;
} hot_reload_ctx_t;

static bool hot_reload_device_cb(device_t *dev, void *ctx)
{
    hot_reload_ctx_t *hr = (hot_reload_ctx_t *)ctx;

    /* Skip devices that don't match */
    if (dev->model[0] == '\0') return true;

    bool match = false;
    if (hr->manufacturer && hr->manufacturer[0] != '\0' &&
        dev->manufacturer[0] != '\0') {
        match = (strcmp(dev->manufacturer, hr->manufacturer) == 0 &&
                 strcmp(dev->model, hr->model) == 0);
    } else {
        match = (strcmp(dev->model, hr->model) == 0);
    }

    if (!match) return true;

    /* Re-bind: find best converter (custom > DB > hardcoded) */
    const zb_converter_def_t *new_def = zb_converter_find(
        dev->manufacturer[0] ? dev->manufacturer : NULL, dev->model);

    if (new_def) {
        zb_converter_bind(dev->proto.zigbee.short_addr, new_def);
        device_registry_set_converter(dev->id, new_def);
        ESP_LOGI("CUSTOM_Q", "Re-bound 0x%04X to %s",
                 dev->proto.zigbee.short_addr,
                 new_def->description ? new_def->description : new_def->model);
    }

    hr->rebind_count++;
    return true;
}

void zb_custom_quirk_hot_reload(const char *manufacturer, const char *model)
{
    if (!model || model[0] == '\0') return;

    hot_reload_ctx_t ctx = {
        .manufacturer = manufacturer,
        .model = model,
        .rebind_count = 0,
    };

    device_registry_iterate_zigbee(hot_reload_device_cb, &ctx);

    if (ctx.rebind_count > 0) {
        ESP_LOGI(TAG, "Hot-reload: re-bound %u devices, disconnecting ESPHome clients",
                 ctx.rebind_count);
        esphome_api_disconnect_all_clients();
    }
}
