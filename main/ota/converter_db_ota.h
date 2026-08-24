/**
 * @file converter_db_ota.h
 * @brief Replace the converter database over HTTP
 *
 * The database lives in its own LittleFS partition, which the application OTA
 * does not touch: a firmware update over port 3232 leaves the device knowledge
 * exactly as it was. Until now the only ways to change it were a serial
 * write-flash of the whole partition image, or MQTT — one base64 file per
 * message, on a gateway meant to run without a broker.
 *
 * This fetches it over HTTP instead. What makes that safe is space rather than
 * cleverness: deduplicating the database took it from 6876 KB to 2700 KB of a
 * 7036 KB partition, so a second complete copy now fits alongside the first.
 * The new database is downloaded to its own directory, every file is checked to
 * parse, and only then does it change places with the one in use. A download
 * that fails, or arrives corrupt, leaves the gateway on the database it already
 * had.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef CONVERTER_DB_OTA_H
#define CONVERTER_DB_OTA_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Directory the gateway reads its converter database from */
#define CONVERTER_DB_DIR      "/littlefs/converters"

/** @brief Directory a download is staged in until it is known to be complete */
#define CONVERTER_DB_STAGING  "/littlefs/converters_new"

/** @brief Where the previous database is kept until the new one has loaded */
#define CONVERTER_DB_PREVIOUS "/littlefs/converters_old"

/**
 * @brief Download a converter database and swap it in
 *
 * Runs on its own task and returns immediately. Progress and the outcome go to
 * the log and to the gateway's Converter DB Status text sensor.
 *
 * The URL is a directory, not a file: index.json is fetched from it first and
 * names everything else, exactly as on the device.
 *
 * @param base_url Directory URL, without a trailing slash
 * @return ESP_OK if the download started
 * @return ESP_ERR_INVALID_STATE if one is already running
 * @return ESP_ERR_NO_MEM if the task could not be created
 */
esp_err_t converter_db_ota_start(const char *base_url);

/** @brief Whether a download is in progress */
bool converter_db_ota_in_progress(void);

/** @brief Last outcome, for the status sensor. Never NULL. */
const char *converter_db_ota_status(void);

#ifdef __cplusplus
}
#endif

#endif /* CONVERTER_DB_OTA_H */
