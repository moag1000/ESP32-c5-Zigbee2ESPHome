/**
 * @file esphome_entity_core.c
 * @brief ESPHome Entity Core - Initialization, Enumeration, and Utilities
 *
 * Core entity management functions including initialization, deinitialization,
 * state callback management, entity enumeration, and type lookup.
 *
 * Integration with the ESPHome entity mirror:
 *   - State changes are recorded by the esphome_entity_mirror module
 *   - EVT_ESPHOME_ENTITY_STATE events are published for each state update
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_entity_internal.h"
#include "core/memory/memory_manager_ng.h"
#include "esphome_crypto_constants.h"
#include "esphome_entity_mirror.h"

/* Log tag */
static const char *TAG = "ESPHOME_ENTITY";

/* Global entity log tag (referenced by entity sub-modules via extern) */
const char *ENTITY_TAG = "ESPHOME_ENTITY";

/* ============================================================================
 * Module State - Global Definition
 * ============================================================================ */

esphome_entity_state_t *s_entities = NULL;

/* ============================================================================
 * Internal Helpers - Find Functions (Generated via macro)
 * ============================================================================ */

ENTITY_FIND_BY_KEY_IMPL(sensor_entry_t, find_sensor, sensors, sensor_count)
ENTITY_FIND_BY_KEY_IMPL(binary_sensor_entry_t, find_binary_sensor, binary_sensors, binary_sensor_count)
ENTITY_FIND_BY_KEY_IMPL(switch_entry_t, find_switch, switches, switch_count)
ENTITY_FIND_BY_KEY_IMPL(select_entry_t, find_select, selects, select_count)
ENTITY_FIND_BY_KEY_IMPL(light_entry_t, find_light, lights, light_count)
ENTITY_FIND_BY_KEY_IMPL(cover_entry_t, find_cover, covers, cover_count)
ENTITY_FIND_BY_KEY_IMPL(fan_entry_t, find_fan, fans, fan_count)
ENTITY_FIND_BY_KEY_IMPL(climate_entry_t, find_climate, climates, climate_count)
ENTITY_FIND_BY_KEY_IMPL(text_sensor_entry_t, find_text_sensor, text_sensors, text_sensor_count)
ENTITY_FIND_BY_KEY_IMPL(number_entry_t, find_number, numbers, number_count)
ENTITY_FIND_BY_KEY_IMPL(button_entry_t, find_button, buttons, button_count)
ENTITY_FIND_BY_KEY_IMPL(lock_entry_t, find_lock, locks, lock_count)
ENTITY_FIND_BY_KEY_IMPL(media_player_entry_t, find_media_player, media_players, media_player_count)
ENTITY_FIND_BY_KEY_IMPL(alarm_entry_t, find_alarm, alarms, alarm_count)
ENTITY_FIND_BY_KEY_IMPL(text_entry_t, find_text, texts, text_count)

/**
 * @brief Notify state change callback
 *
 * Notifies both the ESPHome API callback (for client broadcasting) and
 * records the state in the entity mirror, which publishes the change event.
 */
void notify_state_change(esphome_entity_type_t type, esphome_entity_key_t key,
                         const void *state)
{
    /* Notify ESPHome API callback for client broadcasting */
    if (s_entities && s_entities->state_callback) {
        s_entities->state_callback(type, key, state);
    }

    /* Record in the entity mirror, which publishes EVT_ESPHOME_ENTITY_STATE */
    if (esphome_entity_mirror_is_initialized()) {
        esphome_entity_mirror_sync_state(type, key, state);
    }
}

/* ============================================================================
 * Entity Manager Functions
 * ============================================================================ */

/**
 * @brief Initialize entity manager
 */
esp_err_t esphome_entities_init(void)
{
    /* s_entities is NULL until the allocation below — checking ->initialized
     * first would dereference NULL on the very first call. */
    if (s_entities && s_entities->initialized) {
        ESP_LOGW(TAG, "Entity manager already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing entity manager...");

    /* Storage goes to PSRAM: 53.9 KB that internal RAM does not have to spare.
     * mem_ng_calloc zeroes it, which the per-array memsets below then repeat
     * harmlessly and explicitly. */
    s_entities = mem_ng_calloc(1, sizeof(esphome_entity_state_t), MEM_CAP_PSRAM);
    if (!s_entities) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes of entity storage",
                 sizeof(esphome_entity_state_t));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Entity storage: %zu bytes in PSRAM", sizeof(esphome_entity_state_t));

    /* Create mutex */
    s_entities->mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_entities->mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        mem_ng_free(s_entities);
        s_entities = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Clear storage */
    memset(s_entities->sensors, 0, sizeof(s_entities->sensors));
    memset(s_entities->binary_sensors, 0, sizeof(s_entities->binary_sensors));
    memset(s_entities->switches, 0, sizeof(s_entities->switches));
    memset(s_entities->selects, 0, sizeof(s_entities->selects));
    memset(s_entities->lights, 0, sizeof(s_entities->lights));
    memset(s_entities->covers, 0, sizeof(s_entities->covers));
    memset(s_entities->fans, 0, sizeof(s_entities->fans));
    memset(s_entities->climates, 0, sizeof(s_entities->climates));
    memset(s_entities->text_sensors, 0, sizeof(s_entities->text_sensors));
    memset(s_entities->numbers, 0, sizeof(s_entities->numbers));
    memset(s_entities->buttons, 0, sizeof(s_entities->buttons));
    memset(s_entities->locks, 0, sizeof(s_entities->locks));
    memset(s_entities->media_players, 0, sizeof(s_entities->media_players));
    memset(s_entities->alarms, 0, sizeof(s_entities->alarms));
    memset(s_entities->texts, 0, sizeof(s_entities->texts));

    s_entities->sensor_count = 0;
    s_entities->binary_sensor_count = 0;
    s_entities->switch_count = 0;
    s_entities->select_count = 0;
    s_entities->light_count = 0;
    s_entities->cover_count = 0;
    s_entities->fan_count = 0;
    s_entities->climate_count = 0;
    s_entities->text_sensor_count = 0;
    s_entities->number_count = 0;
    s_entities->button_count = 0;
    s_entities->lock_count = 0;
    s_entities->media_player_count = 0;
    s_entities->alarm_count = 0;
    s_entities->text_count = 0;
    s_entities->state_callback = NULL;
    s_entities->initialized = true;

    ESP_LOGI(TAG, "Entity manager initialized (max: %d sensors, %d binary, %d switches, "
            "%d text_sensors, %d numbers, %d buttons)",
            ESPHOME_MAX_SENSORS, ESPHOME_MAX_BINARY_SENSORS, ESPHOME_MAX_SWITCHES,
            ESPHOME_MAX_TEXT_SENSORS, ESPHOME_MAX_NUMBERS, ESPHOME_MAX_BUTTONS);

    return ESP_OK;
}

/**
 * @brief Deinitialize entity manager
 */
esp_err_t esphome_entities_deinit(void)
{
    if (!s_entities || !s_entities->initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing entity manager...");

    if (s_entities->mutex) {
        vSemaphoreDelete(s_entities->mutex);
        s_entities->mutex = NULL;
    }

    s_entities->initialized = false;

    mem_ng_free(s_entities);
    s_entities = NULL;
    return ESP_OK;
}

/**
 * @brief Set state change callback
 */
esp_err_t esphome_entities_set_state_callback(esphome_state_change_cb_t callback)
{
    if (!s_entities) {
        return ESP_ERR_INVALID_STATE;
    }
    s_entities->state_callback = callback;
    return ESP_OK;
}

/* ============================================================================
 * Generic Entity State Update
 * ============================================================================ */

/**
 * @brief Update entity state by key (generic)
 */
esp_err_t esphome_entity_update_state(esphome_entity_key_t key, const void *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_entity_type_t type;
    esp_err_t ret = esphome_entity_get_type(key, &type);
    if (ret != ESP_OK) {
        return ret;
    }

    switch (type) {
        case ESPHOME_ENTITY_SENSOR: {
            const esphome_sensor_state_t *s = (const esphome_sensor_state_t *)state;
            return esphome_entity_update_sensor(key, s->state);
        }
        case ESPHOME_ENTITY_BINARY_SENSOR: {
            const esphome_binary_sensor_state_t *s = (const esphome_binary_sensor_state_t *)state;
            return esphome_entity_update_binary_sensor(key, s->state);
        }
        case ESPHOME_ENTITY_SWITCH: {
            const esphome_switch_state_t *s = (const esphome_switch_state_t *)state;
            return esphome_entity_update_switch(key, s->state);
        }
        default:
            return ESPHOME_ERR_INVALID_TYPE;
    }
}

/**
 * @brief Find entity type by key
 */
esp_err_t esphome_entity_get_type(esphome_entity_key_t key, esphome_entity_type_t *type)
{
    if (!type) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities || !s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESPHOME_ERR_ENTITY_NOT_FOUND;

    if (find_sensor(key)) {
        *type = ESPHOME_ENTITY_SENSOR;
        ret = ESP_OK;
    } else if (find_binary_sensor(key)) {
        *type = ESPHOME_ENTITY_BINARY_SENSOR;
        ret = ESP_OK;
    } else if (find_switch(key)) {
        *type = ESPHOME_ENTITY_SWITCH;
        ret = ESP_OK;
    } else if (find_text_sensor(key)) {
        *type = ESPHOME_ENTITY_TEXT_SENSOR;
        ret = ESP_OK;
    } else if (find_number(key)) {
        *type = ESPHOME_ENTITY_NUMBER;
        ret = ESP_OK;
    } else if (find_button(key)) {
        *type = ESPHOME_ENTITY_BUTTON;
        ret = ESP_OK;
    } else if (find_select(key)) {
        *type = ESPHOME_ENTITY_SELECT;
        ret = ESP_OK;
    } else if (find_light(key)) {
        *type = ESPHOME_ENTITY_LIGHT;
        ret = ESP_OK;
    } else if (find_cover(key)) {
        *type = ESPHOME_ENTITY_COVER;
        ret = ESP_OK;
    } else if (find_fan(key)) {
        *type = ESPHOME_ENTITY_FAN;
        ret = ESP_OK;
    } else if (find_climate(key)) {
        *type = ESPHOME_ENTITY_CLIMATE;
        ret = ESP_OK;
    } else if (find_lock(key)) {
        *type = ESPHOME_ENTITY_LOCK;
        ret = ESP_OK;
    } else if (find_media_player(key)) {
        *type = ESPHOME_ENTITY_MEDIA_PLAYER;
        ret = ESP_OK;
    } else if (find_alarm(key)) {
        *type = ESPHOME_ENTITY_ALARM_PANEL;
        ret = ESP_OK;
    } else if (find_text(key)) {
        *type = ESPHOME_ENTITY_TEXT;
        ret = ESP_OK;
    }

    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/* ============================================================================
 * Entity Enumeration
 * ============================================================================ */

/**
 * @brief Enumerate all registered entities
 */
void esphome_entities_enumerate(esphome_entity_enum_cb_t callback, void *user_data)
{
    if (!callback || !s_entities || !s_entities->initialized) {
        return;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return;
    }

    /* Enumerate sensors */
    for (size_t i = 0; i < s_entities->sensor_count; i++) {
        if (s_entities->sensors[i].registered) {
            if (!callback(ESPHOME_ENTITY_SENSOR, s_entities->sensors[i].config.key,
                         &s_entities->sensors[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate binary sensors */
    for (size_t i = 0; i < s_entities->binary_sensor_count; i++) {
        if (s_entities->binary_sensors[i].registered) {
            if (!callback(ESPHOME_ENTITY_BINARY_SENSOR,
                         s_entities->binary_sensors[i].config.key,
                         &s_entities->binary_sensors[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate switches */
    for (size_t i = 0; i < s_entities->switch_count; i++) {
        if (s_entities->switches[i].registered) {
            if (!callback(ESPHOME_ENTITY_SWITCH, s_entities->switches[i].config.key,
                         &s_entities->switches[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate text sensors */
    for (size_t i = 0; i < s_entities->text_sensor_count; i++) {
        if (s_entities->text_sensors[i].registered) {
            if (!callback(ESPHOME_ENTITY_TEXT_SENSOR, s_entities->text_sensors[i].config.key,
                         &s_entities->text_sensors[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate numbers */
    for (size_t i = 0; i < s_entities->number_count; i++) {
        if (s_entities->numbers[i].registered) {
            if (!callback(ESPHOME_ENTITY_NUMBER, s_entities->numbers[i].config.key,
                         &s_entities->numbers[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate buttons */
    for (size_t i = 0; i < s_entities->button_count; i++) {
        if (s_entities->buttons[i].registered) {
            if (!callback(ESPHOME_ENTITY_BUTTON, s_entities->buttons[i].config.key,
                         &s_entities->buttons[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate selects */
    for (size_t i = 0; i < s_entities->select_count; i++) {
        if (s_entities->selects[i].registered) {
            if (!callback(ESPHOME_ENTITY_SELECT, s_entities->selects[i].config.key,
                         &s_entities->selects[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate lights */
    for (size_t i = 0; i < s_entities->light_count; i++) {
        if (s_entities->lights[i].registered) {
            if (!callback(ESPHOME_ENTITY_LIGHT, s_entities->lights[i].config.key,
                         &s_entities->lights[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate covers */
    for (size_t i = 0; i < s_entities->cover_count; i++) {
        if (s_entities->covers[i].registered) {
            if (!callback(ESPHOME_ENTITY_COVER, s_entities->covers[i].config.key,
                         &s_entities->covers[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate fans */
    for (size_t i = 0; i < s_entities->fan_count; i++) {
        if (s_entities->fans[i].registered) {
            if (!callback(ESPHOME_ENTITY_FAN, s_entities->fans[i].config.key,
                         &s_entities->fans[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate climates */
    for (size_t i = 0; i < s_entities->climate_count; i++) {
        if (s_entities->climates[i].registered) {
            if (!callback(ESPHOME_ENTITY_CLIMATE, s_entities->climates[i].config.key,
                         &s_entities->climates[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate locks */
    for (size_t i = 0; i < s_entities->lock_count; i++) {
        if (s_entities->locks[i].registered) {
            if (!callback(ESPHOME_ENTITY_LOCK, s_entities->locks[i].config.key,
                         &s_entities->locks[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate media players */
    for (size_t i = 0; i < s_entities->media_player_count; i++) {
        if (s_entities->media_players[i].registered) {
            if (!callback(ESPHOME_ENTITY_MEDIA_PLAYER, s_entities->media_players[i].config.key,
                         &s_entities->media_players[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate alarm control panels */
    for (size_t i = 0; i < s_entities->alarm_count; i++) {
        if (s_entities->alarms[i].registered) {
            if (!callback(ESPHOME_ENTITY_ALARM_PANEL, s_entities->alarms[i].config.key,
                         &s_entities->alarms[i].config, user_data)) {
                goto done;
            }
        }
    }

    /* Enumerate text entities */
    for (size_t i = 0; i < s_entities->text_count; i++) {
        if (s_entities->texts[i].registered) {
            if (!callback(ESPHOME_ENTITY_TEXT, s_entities->texts[i].config.key,
                         &s_entities->texts[i].config, user_data)) {
                goto done;
            }
        }
    }

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
}

/**
 * @brief Get total number of registered entities
 */
size_t esphome_entities_get_total_count(void)
{
    return s_entities->sensor_count + s_entities->binary_sensor_count + s_entities->switch_count +
           s_entities->text_sensor_count + s_entities->number_count + s_entities->button_count +
           s_entities->select_count + s_entities->light_count + s_entities->cover_count +
           s_entities->fan_count + s_entities->climate_count + s_entities->lock_count +
           s_entities->media_player_count + s_entities->alarm_count + s_entities->text_count;
}

/* ============================================================================
 * Testing
 * ============================================================================ */

/**
 * @brief Test entity management
 */
esp_err_t esphome_entities_test(void)
{
    ESP_LOGI(TAG, "=== ESPHome Entities Test ===");

    esp_err_t ret;

    /* Initialize entity manager */
    ret = esphome_entities_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init failed");
        return ESP_FAIL;
    }

    /* Test sensor registration */
    ESP_LOGI(TAG, "Testing sensor registration...");
    esphome_sensor_config_t sensor_cfg = {
        .key = 1001,
        .name = "Test Temperature",
        .unique_id = "test_temp_001",
        .icon = "mdi:thermometer",
        .unit_of_measurement = "C",
        .accuracy_decimals = 1,
        .device_class = ESPHOME_SENSOR_CLASS_TEMPERATURE,
        .state_class = ESPHOME_STATE_CLASS_MEASUREMENT,
    };

    ret = esphome_entity_register_sensor(&sensor_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sensor registration failed");
        return ESP_FAIL;
    }

    /* Test sensor state update */
    ret = esphome_entity_update_sensor(1001, ESPHOME_SENSOR_TEST_VALUE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sensor update failed");
        return ESP_FAIL;
    }

    esphome_sensor_state_t sensor_state;
    ret = esphome_entity_get_sensor(1001, &sensor_state);
    if (ret != ESP_OK || sensor_state.state != ESPHOME_SENSOR_TEST_VALUE || sensor_state.missing_state) {
        ESP_LOGE(TAG, "Sensor state read failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Sensor tests passed");

    /* Test binary sensor registration */
    ESP_LOGI(TAG, "Testing binary sensor registration...");
    esphome_binary_sensor_config_t binary_cfg = {
        .key = 2001,
        .name = "Test Motion",
        .unique_id = "test_motion_001",
        .icon = "mdi:motion-sensor",
        .device_class = ESPHOME_BINARY_CLASS_MOTION,
    };

    ret = esphome_entity_register_binary_sensor(&binary_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Binary sensor registration failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Binary sensor tests passed");
    ESP_LOGI(TAG, "=== All entity tests passed ===");

    return ESP_OK;
}
