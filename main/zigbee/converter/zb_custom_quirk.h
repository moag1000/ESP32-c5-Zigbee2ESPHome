/**
 * @file zb_custom_quirk.h
 * @brief Custom Community Quirks — User-defined device definitions
 *
 * Allows users to upload custom device definitions as JSON files.
 * Custom quirks have highest priority (pri=0) and override all other sources.
 *
 * Storage: /littlefs/converters/custom/<name>.json
 * Format: Same compact JSON as the main converter DB.
 *
 * Operations:
 *   - Add:    Write JSON file + add to in-memory index
 *   - Remove: Delete file + remove from index
 *   - List:   Enumerate all custom quirks
 *   - Find:   Lookup by manufacturer+model (priority over DB)
 */

#ifndef ZB_CUSTOM_QUIRK_H
#define ZB_CUSTOM_QUIRK_H

#include "esp_err.h"
#include "zb_converter_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum custom quirks stored */
#define ZB_CUSTOM_QUIRK_MAX     16

/**
 * @brief Initialize the custom quirk module
 *
 * Scans /littlefs/converters/custom/ for existing quirk files,
 * parses them, and builds the in-memory index.
 *
 * @return ESP_OK on success
 */
esp_err_t zb_custom_quirk_init(void);

/**
 * @brief Find a custom quirk by manufacturer and model
 *
 * @param manufacturer Manufacturer string
 * @param model Model identifier string
 * @return Pointer to converter def, or NULL if no custom quirk matches
 */
const zb_converter_def_t *zb_custom_quirk_find(
    const char *manufacturer, const char *model);

/**
 * @brief Add a custom quirk from JSON string
 *
 * Parses the JSON, writes to LittleFS, and adds to in-memory index.
 * If a quirk with the same name exists, it is replaced.
 *
 * @param name Unique quirk name (used as filename, no path chars)
 * @param json_str JSON string in compact converter format
 * @return ESP_OK on success
 */
esp_err_t zb_custom_quirk_add(const char *name, const char *json_str);

/**
 * @brief Remove a custom quirk by name
 *
 * Deletes from LittleFS and removes from in-memory index.
 *
 * @param name Quirk name
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t zb_custom_quirk_remove(const char *name);

/**
 * @brief List info for a custom quirk entry
 */
typedef struct {
    const char *name;           /**< Quirk name (filename without .json) */
    const char *manufacturer;   /**< Matched manufacturer */
    const char *model;          /**< Matched model */
    const char *description;    /**< Description string */
} zb_custom_quirk_info_t;

/**
 * @brief Get list of all custom quirks
 *
 * @param[out] out Array of info structs to fill
 * @param max_count Maximum entries to return
 * @return Number of entries filled
 */
size_t zb_custom_quirk_list(zb_custom_quirk_info_t *out, size_t max_count);

/**
 * @brief Get count of loaded custom quirks
 */
size_t zb_custom_quirk_count(void);

/**
 * @brief Hot-reload: re-bind active devices using updated custom quirk
 *
 * After adding/removing a custom quirk, this function:
 * 1. Finds active devices matching the quirk's manufacturer+model
 * 2. Re-binds them to the new (or next-best) converter
 * 3. Disconnects ESPHome clients to force entity list refresh
 *
 * @param manufacturer Manufacturer string of affected devices
 * @param model Model string of affected devices
 */
void zb_custom_quirk_hot_reload(const char *manufacturer, const char *model);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CUSTOM_QUIRK_H */
