/**
 * @file esphome_entity_macros.h
 * @brief Macros for ESPHome Entity Registry Operations
 *
 * This file provides generic macros to consolidate the repetitive
 * entity storage, lookup, and registration patterns in esphome_entities.c.
 *
 * CQ-016: Entity Registry Refactoring
 * Reduces ~120-160 LOC by eliminating 15 near-identical find_* functions
 * and standardizing entity operations.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_ENTITY_MACROS_H
#define ESPHOME_ENTITY_MACROS_H

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gateway_defaults.h"
#include <string.h>

/**
 * @brief Generate a find-by-key function for an entity type
 *
 * Creates a function to search for an entity by its key.
 * Note: This generates a non-static function for use across split modules.
 *
 * @param entry_type The entry struct type (e.g., sensor_entry_t)
 * @param func_name The function name to generate
 * @param array_name The array in s_entities to search
 * @param count_name The count variable in s_entities
 */
#define ENTITY_FIND_BY_KEY_IMPL(entry_type, func_name, array_name, count_name)    \
    entry_type *func_name(esphome_entity_key_t key)                               \
    {                                                                              \
        for (size_t i = 0; i < s_entities.count_name; i++) {                      \
            if (s_entities.array_name[i].registered &&                            \
                s_entities.array_name[i].config.key == key) {                     \
                return &s_entities.array_name[i];                                 \
            }                                                                      \
        }                                                                          \
        return NULL;                                                               \
    }

/**
 * @brief Standard mutex acquisition with timeout check
 */
#define ENTITY_MUTEX_TAKE()                                                        \
    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS)       \
        != pdTRUE) {                                                               \
        return ESP_ERR_TIMEOUT;                                                    \
    }

/**
 * @brief Standard mutex release
 */
#define ENTITY_MUTEX_GIVE()                                                        \
    xSemaphoreGiveRecursive(s_entities.mutex)

/**
 * @brief Standard initialization check
 */
#define ENTITY_CHECK_INIT()                                                        \
    if (!s_entities.initialized) {                                                 \
        return ESP_ERR_INVALID_STATE;                                              \
    }

/**
 * @brief Standard initialization check with error log
 */
#define ENTITY_CHECK_INIT_LOG(tag)                                                 \
    if (!s_entities.initialized) {                                                 \
        ESP_LOGE(tag, "Entity manager not initialized");                           \
        return ESP_ERR_INVALID_STATE;                                              \
    }

/**
 * @brief Standard NULL argument check
 */
#define ENTITY_CHECK_ARG(ptr)                                                      \
    if (!(ptr)) {                                                                  \
        return ESP_ERR_INVALID_ARG;                                                \
    }

/**
 * @brief Get entity state implementation
 *
 * Generates the body of a get_entity function.
 *
 * @param find_func The find function to use
 * @param state_type The state struct type
 */
#define ENTITY_GET_STATE_IMPL(find_func, state_type, key, state_ptr)              \
    do {                                                                           \
        ENTITY_CHECK_ARG(state_ptr);                                               \
        ENTITY_CHECK_INIT();                                                       \
        ENTITY_MUTEX_TAKE();                                                       \
        esp_err_t ret = ESP_OK;                                                    \
        void *entry = find_func(key);                                              \
        if (!entry) {                                                              \
            ret = ESPHOME_ERR_ENTITY_NOT_FOUND;                                    \
        } else {                                                                   \
            memcpy(state_ptr, &((typeof(entry))entry)->state, sizeof(state_type)); \
        }                                                                          \
        ENTITY_MUTEX_GIVE();                                                       \
        return ret;                                                                \
    } while (0)

/**
 * @brief Get entity config implementation
 *
 * Generates the body of a get_entity_config function.
 *
 * @param find_func The find function to use
 * @param config_type The config struct type
 */
#define ENTITY_GET_CONFIG_IMPL(find_func, config_type, key, config_ptr)           \
    do {                                                                           \
        ENTITY_CHECK_ARG(config_ptr);                                              \
        ENTITY_CHECK_INIT();                                                       \
        ENTITY_MUTEX_TAKE();                                                       \
        esp_err_t ret = ESP_OK;                                                    \
        void *entry = find_func(key);                                              \
        if (!entry) {                                                              \
            ret = ESPHOME_ERR_ENTITY_NOT_FOUND;                                    \
        } else {                                                                   \
            memcpy(config_ptr, &((typeof(entry))entry)->config, sizeof(config_type)); \
        }                                                                          \
        ENTITY_MUTEX_GIVE();                                                       \
        return ret;                                                                \
    } while (0)

/**
 * @brief Set entity missing state implementation
 *
 * Standard pattern for marking an entity as unavailable/missing.
 *
 * @param find_func The find function to use
 * @param entity_type The esphome_entity_type_t value
 */
#define ENTITY_SET_MISSING_IMPL(find_func, entity_type, key)                      \
    do {                                                                           \
        ENTITY_CHECK_INIT();                                                       \
        ENTITY_MUTEX_TAKE();                                                       \
        esp_err_t ret = ESP_OK;                                                    \
        void *entry = find_func(key);                                              \
        if (!entry) {                                                              \
            ret = ESPHOME_ERR_ENTITY_NOT_FOUND;                                    \
            ENTITY_MUTEX_GIVE();                                                   \
            return ret;                                                            \
        }                                                                          \
        ((typeof(entry))entry)->state.missing_state = true;                        \
        ENTITY_MUTEX_GIVE();                                                       \
        notify_state_change(entity_type, key, &((typeof(entry))entry)->state);     \
        return ESP_OK;                                                             \
    } while (0)

#endif /* ESPHOME_ENTITY_MACROS_H */
