/**
 * @file esphome_entity_internal.h
 * @brief ESPHome Entity Internal Shared State
 *
 * Internal header for sharing state structures between entity module files.
 * This file should NOT be included by external modules.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_ENTITY_INTERNAL_H
#define ESPHOME_ENTITY_INTERNAL_H

#include "esphome_entities.h"
#include "esphome_entity_macros.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "gateway_defaults.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Internal Storage Structures
 * ============================================================================ */

/**
 * @brief Sensor storage entry
 */
typedef struct {
    esphome_sensor_config_t config;
    esphome_sensor_state_t state;
    bool registered;
} sensor_entry_t;

/**
 * @brief Binary sensor storage entry
 */
typedef struct {
    esphome_binary_sensor_config_t config;
    esphome_binary_sensor_state_t state;
    bool registered;
} binary_sensor_entry_t;

/**
 * @brief Switch storage entry
 */
typedef struct {
    esphome_switch_config_t config;
    esphome_switch_state_t state;
    bool registered;
} switch_entry_t;

/**
 * @brief Select storage entry
 */
typedef struct {
    esphome_select_config_t config;
    esphome_select_state_t state;
    bool registered;
} select_entry_t;

/**
 * @brief Light storage entry
 */
typedef struct {
    esphome_light_config_t config;
    esphome_light_state_t state;
    bool registered;
} light_entry_t;

/**
 * @brief Cover storage entry
 */
typedef struct {
    esphome_cover_config_t config;
    esphome_cover_state_t state;
    bool registered;
} cover_entry_t;

/**
 * @brief Fan storage entry
 */
typedef struct {
    esphome_fan_config_t config;
    esphome_fan_state_t state;
    bool registered;
} fan_entry_t;

/**
 * @brief Climate storage entry
 */
typedef struct {
    esphome_climate_config_t config;
    esphome_climate_state_t state;
    bool registered;
} climate_entry_t;

/**
 * @brief Text sensor storage entry
 */
typedef struct {
    esphome_text_sensor_config_t config;
    esphome_text_sensor_state_t state;
    bool registered;
} text_sensor_entry_t;

/**
 * @brief Number storage entry
 */
typedef struct {
    esphome_number_config_t config;
    esphome_number_state_t state;
    bool registered;
} number_entry_t;

/**
 * @brief Button storage entry
 */
typedef struct {
    esphome_button_config_t config;
    bool registered;
} button_entry_t;

/**
 * @brief Lock storage entry
 */
typedef struct {
    esphome_lock_config_t config;
    esphome_lock_entity_state_t state;
    bool registered;
} lock_entry_t;

/**
 * @brief Media player storage entry
 */
typedef struct {
    esphome_media_player_config_t config;
    esphome_media_player_entity_state_t state;
    bool registered;
} media_player_entry_t;

/**
 * @brief Alarm control panel storage entry
 */
typedef struct {
    esphome_alarm_config_t config;
    esphome_alarm_entity_state_t state;
    bool registered;
} alarm_entry_t;

/**
 * @brief Text entity storage entry
 */
typedef struct {
    esphome_text_config_t config;
    esphome_text_entity_state_t state;
    bool registered;
} text_entry_t;

/* ============================================================================
 * Module State Structure
 * ============================================================================ */

/**
 * @brief Entity manager global state structure
 */
typedef struct {
    sensor_entry_t sensors[ESPHOME_MAX_SENSORS];
    binary_sensor_entry_t binary_sensors[ESPHOME_MAX_BINARY_SENSORS];
    switch_entry_t switches[ESPHOME_MAX_SWITCHES];
    select_entry_t selects[ESPHOME_MAX_SELECTS];
    light_entry_t lights[ESPHOME_MAX_LIGHTS];
    cover_entry_t covers[ESPHOME_MAX_COVERS];
    fan_entry_t fans[ESPHOME_MAX_FANS];
    climate_entry_t climates[ESPHOME_MAX_CLIMATES];
    text_sensor_entry_t text_sensors[ESPHOME_MAX_TEXT_SENSORS];
    number_entry_t numbers[ESPHOME_MAX_NUMBERS];
    button_entry_t buttons[ESPHOME_MAX_BUTTONS];
    lock_entry_t locks[ESPHOME_MAX_LOCKS];
    media_player_entry_t media_players[ESPHOME_MAX_MEDIA_PLAYERS];
    alarm_entry_t alarms[ESPHOME_MAX_ALARM_PANELS];
    text_entry_t texts[ESPHOME_MAX_TEXTS];
    size_t sensor_count;
    size_t binary_sensor_count;
    size_t switch_count;
    size_t select_count;
    size_t light_count;
    size_t cover_count;
    size_t fan_count;
    size_t climate_count;
    size_t text_sensor_count;
    size_t number_count;
    size_t button_count;
    size_t lock_count;
    size_t media_player_count;
    size_t alarm_count;
    size_t text_count;
    SemaphoreHandle_t mutex;
    esphome_state_change_cb_t state_callback;
    bool initialized;
} esphome_entity_state_t;

/* ============================================================================
 * Global State - Defined in esphome_entity_core.c
 * ============================================================================ */

/**
 * @brief Global entity manager state
 */
extern esphome_entity_state_t s_entities;

/**
 * @brief Log tag for entity module
 */
extern const char *ENTITY_TAG;

/* ============================================================================
 * Entity Mutex Wrapper Macros (CQ-061)
 *
 * These macros reduce code duplication for mutex acquisition/release patterns
 * in entity operations.
 * ============================================================================ */

/**
 * @brief Acquire entity mutex with timeout
 *
 * Returns ESP_ERR_TIMEOUT if mutex acquisition fails.
 * Uses GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS from gateway_defaults.h
 */
#define ENTITY_MUTEX_ACQUIRE() \
    do { \
        if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) { \
            return ESP_ERR_TIMEOUT; \
        } \
    } while(0)

/**
 * @brief Acquire entity mutex with timeout, returning custom error
 *
 * @param ret_val Value to return on timeout (e.g., NULL, -1, ESP_ERR_TIMEOUT)
 */
#define ENTITY_MUTEX_ACQUIRE_OR_RETURN(ret_val) \
    do { \
        if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) { \
            return (ret_val); \
        } \
    } while(0)

/**
 * @brief Release entity mutex
 */
#define ENTITY_MUTEX_RELEASE() \
    xSemaphoreGiveRecursive(s_entities.mutex)

/**
 * @brief Scoped mutex guard for entity operations
 *
 * Executes the code block while holding the mutex.
 * Automatically releases the mutex after the block.
 *
 * Note: This macro returns ESP_ERR_TIMEOUT if mutex acquisition fails.
 * Use ENTITY_MUTEX_GUARD_RET for custom return values.
 *
 * @param code Code block to execute under mutex protection
 */
#define ENTITY_MUTEX_GUARD(code) \
    do { \
        ENTITY_MUTEX_ACQUIRE(); \
        { code } \
        ENTITY_MUTEX_RELEASE(); \
    } while(0)

/**
 * @brief Scoped mutex guard with custom return value on timeout
 *
 * @param ret_val Value to return on mutex timeout
 * @param code Code block to execute under mutex protection
 */
#define ENTITY_MUTEX_GUARD_RET(ret_val, code) \
    do { \
        ENTITY_MUTEX_ACQUIRE_OR_RETURN(ret_val); \
        { code } \
        ENTITY_MUTEX_RELEASE(); \
    } while(0)

/* ============================================================================
 * Internal Helper Functions - Implemented in esphome_entity_core.c
 * ============================================================================ */

/**
 * @brief Notify state change callback
 *
 * @param[in] type Entity type
 * @param[in] key Entity key
 * @param[in] state Pointer to state structure
 */
void notify_state_change(esphome_entity_type_t type, esphome_entity_key_t key,
                         const void *state);

/* ============================================================================
 * Find Functions - Generated via ENTITY_FIND_BY_KEY_IMPL macro
 * Each module defines the find functions it needs
 * ============================================================================ */

/**
 * @brief Find sensor by key
 */
sensor_entry_t *find_sensor(esphome_entity_key_t key);

/**
 * @brief Find binary sensor by key
 */
binary_sensor_entry_t *find_binary_sensor(esphome_entity_key_t key);

/**
 * @brief Find switch by key
 */
switch_entry_t *find_switch(esphome_entity_key_t key);

/**
 * @brief Find select by key
 */
select_entry_t *find_select(esphome_entity_key_t key);

/**
 * @brief Find light by key
 */
light_entry_t *find_light(esphome_entity_key_t key);

/**
 * @brief Find cover by key
 */
cover_entry_t *find_cover(esphome_entity_key_t key);

/**
 * @brief Find fan by key
 */
fan_entry_t *find_fan(esphome_entity_key_t key);

/**
 * @brief Find climate by key
 */
climate_entry_t *find_climate(esphome_entity_key_t key);

/**
 * @brief Find text sensor by key
 */
text_sensor_entry_t *find_text_sensor(esphome_entity_key_t key);

/**
 * @brief Find number by key
 */
number_entry_t *find_number(esphome_entity_key_t key);

/**
 * @brief Find button by key
 */
button_entry_t *find_button(esphome_entity_key_t key);

/**
 * @brief Find lock by key
 */
lock_entry_t *find_lock(esphome_entity_key_t key);

/**
 * @brief Find media player by key
 */
media_player_entry_t *find_media_player(esphome_entity_key_t key);

/**
 * @brief Find alarm by key
 */
alarm_entry_t *find_alarm(esphome_entity_key_t key);

/**
 * @brief Find text entity by key
 */
text_entry_t *find_text(esphome_entity_key_t key);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ENTITY_INTERNAL_H */
