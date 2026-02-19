/**
 * @file state_persistence.h
 * @brief Device State Persistence for Zigbee2MQTT Gateway
 *
 * Persists device runtime state (on/off, brightness, temperature, etc.)
 * to LittleFS so that Home Assistant sees last known values after reboot
 * instead of "unavailable".
 */
#ifndef STATE_PERSISTENCE_H
#define STATE_PERSISTENCE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Default save interval in milliseconds (5 minutes) */
#define STATE_PERSIST_DEFAULT_INTERVAL_MS   (300 * 1000)

/** @brief Maximum state file size */
#define STATE_PERSIST_MAX_FILE_SIZE         (32 * 1024)

/** @brief State file path */
#define STATE_PERSIST_FILE_PATH             "/littlefs/state/state.json"

/** @brief Temporary file for atomic writes */
#define STATE_PERSIST_TEMP_PATH             "/littlefs/state/state.tmp"

/** @brief State directory */
#define STATE_PERSIST_DIR                   "/littlefs/state"

/**
 * @brief Initialize state persistence
 *
 * Mounts LittleFS (if not already), creates state directory,
 * starts periodic save timer.
 *
 * @return ESP_OK on success
 */
esp_err_t state_persistence_init(void);

/**
 * @brief Deinitialize state persistence
 *
 * Saves any dirty state and stops the timer.
 */
void state_persistence_deinit(void);

/**
 * @brief Save all device states to LittleFS
 *
 * Only writes if dirty flag is set. Uses atomic write (tmp + rename).
 * Allocates JSON buffer from PSRAM.
 *
 * @return ESP_OK on success
 */
esp_err_t state_persistence_save(void);

/**
 * @brief Load persisted state and publish to MQTT
 *
 * Reads state.json, parses it, and publishes each device's
 * cached state to zigbee2mqtt/{friendly_name}.
 * Call after MQTT bridge is started.
 *
 * @return ESP_OK on success
 */
esp_err_t state_persistence_load_and_publish(void);

/**
 * @brief Mark state as dirty (needs saving)
 *
 * Called after each device state publish. The actual save
 * happens on the next timer tick.
 */
void state_persistence_mark_dirty(void);

/**
 * @brief Erase all persisted state
 *
 * Deletes state.json. Used by factory reset.
 *
 * @return ESP_OK on success
 */
esp_err_t state_persistence_erase_all(void);

/**
 * @brief Check if state persistence is initialized
 * @return true if initialized
 */
bool state_persistence_is_initialized(void);

/**
 * @brief Emergency save during PAIRING phase
 *
 * Mounts LittleFS temporarily, saves device states, then unmounts.
 * Works even when state_persistence is deinitialized (during PAIRING).
 * Use this to persist devices that join during PAIRING phase.
 *
 * @return ESP_OK on success
 */
esp_err_t state_persistence_save_immediate(void);

#ifdef __cplusplus
}
#endif

#endif /* STATE_PERSISTENCE_H */
