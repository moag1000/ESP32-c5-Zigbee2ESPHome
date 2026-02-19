/**
 * @file zb_converter_loader.h
 * @brief Runtime Converter Loader — Loads device definitions from LittleFS JSON DB
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef ZB_CONVERTER_LOADER_H
#define ZB_CONVERTER_LOADER_H

#include "esp_err.h"
#include "zb_converter_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the converter loader
 *
 * Reads index.json from LittleFS, validates version.
 * Non-fatal if DB doesn't exist — loader simply won't find anything.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no DB present
 */
esp_err_t zb_converter_loader_init(void);

/**
 * @brief Reload the converter DB index after an update
 *
 * Re-reads index.json from LittleFS. Call this after uploading
 * a new converter DB via MQTT or other means.
 *
 * @return ESP_OK on success
 */
esp_err_t zb_converter_loader_reload_index(void);

/**
 * @brief Check if converter DB is available
 * @return true if index.json was loaded successfully
 */
bool zb_converter_loader_is_available(void);

/**
 * @brief Find a device definition by manufacturer and model
 *
 * Looks up manufacturer in index to find the right JSON file,
 * then searches for the model within that file.
 * Results are cached (max 32 entries).
 *
 * @param manufacturer Manufacturer string
 * @param model Model identifier string
 * @return Pointer to dynamically allocated converter def, or NULL
 */
const zb_converter_def_t *zb_converter_loader_find(
    const char *manufacturer, const char *model);

/**
 * @brief Free a runtime-loaded converter definition
 * @param def Definition returned by zb_converter_loader_find()
 */
void zb_converter_loader_free(const zb_converter_def_t *def);

/**
 * @brief Check if a converter was loaded at runtime (vs hard-coded)
 * @param def Converter definition pointer
 * @return true if loaded from LittleFS
 */
bool zb_converter_loader_is_runtime(const zb_converter_def_t *def);

/**
 * @brief Get loader statistics
 * @param[out] db_count Total devices in DB (from index.json)
 * @param[out] loaded Currently loaded/cached converters
 * @param[out] bytes_used Approximate PSRAM bytes used by cache
 */
void zb_converter_loader_get_stats(size_t *db_count, size_t *loaded, size_t *bytes_used);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CONVERTER_LOADER_H */
