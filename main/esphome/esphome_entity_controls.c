/**
 * @file esphome_entity_controls.c
 * @brief ESPHome Control Entities - Switch, Light, Cover, Fan, Climate, Number, Select
 *
 * Registration, state management, and encoding for control-type entities.
 *
 * Integration with the ESPHome entity mirror:
 *   - Entity registration creates a virtual device in device_registry
 *   - Capabilities are set based on entity type (ON_OFF, BRIGHTNESS, etc.)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_entity_internal.h"
#include "esphome_protocol.h"
#include "esphome_entity_mirror.h"

/* ============================================================================
 * Switch Registration and State
 * ============================================================================ */

/**
 * @brief Register a switch entity
 */
esp_err_t esphome_entity_register_switch(const esphome_switch_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Check for duplicate key */
    if (find_switch(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Switch with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities->switch_count >= ESPHOME_MAX_SWITCHES) {
        ESP_LOGE(ENTITY_TAG, "Maximum switches reached (%d)", ESPHOME_MAX_SWITCHES);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add switch */
    size_t idx = s_entities->switch_count;
    memcpy(&s_entities->switches[idx].config, config, sizeof(esphome_switch_config_t));
    s_entities->switches[idx].state.key = config->key;
    s_entities->switches[idx].state.device_id = config->device_id;
    s_entities->switches[idx].state.state = false;
    s_entities->switches[idx].registered = true;
    s_entities->switch_count++;

    ESP_LOGI(ENTITY_TAG, "Registered switch: key=%lu, name='%s'", config->key, config->name);

    /* Mirror into the entity state store (best-effort, non-blocking) */
    if (esphome_entity_mirror_is_initialized()) {
        esp_err_t reg_ret = esphome_entity_mirror_register(
            ESPHOME_ENTITY_SWITCH, config->key, config->name, config->unique_id);
        if (reg_ret != ESP_OK) {
            ESP_LOGD(ENTITY_TAG, "Entity mirror registration skipped: %s",
                     esp_err_to_name(reg_ret));
        }
    }

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Update switch state
 */
esp_err_t esphome_entity_update_switch(esphome_entity_key_t key, bool state)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    switch_entry_t *entry = find_switch(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.state = state;

    ESP_LOGD(ENTITY_TAG, "Switch %lu state: %s", key, state ? "ON" : "OFF");

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_SWITCH, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get switch state
 */
esp_err_t esphome_entity_get_switch(esphome_entity_key_t key, esphome_switch_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    switch_entry_t *entry = find_switch(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(state, &entry->state, sizeof(esphome_switch_state_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get switch configuration
 */
esp_err_t esphome_entity_get_switch_config(esphome_entity_key_t key,
                                            esphome_switch_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    switch_entry_t *entry = find_switch(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_switch_config_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number of registered switches
 */
size_t esphome_entity_get_switch_count(void)
{
    return s_entities->switch_count;
}

/**
 * @brief Execute switch command
 */
esp_err_t esphome_entity_execute_switch_command(esphome_entity_key_t key, bool state)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    switch_entry_t *entry = find_switch(key);
    esphome_switch_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities->mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing switch command: key=%lu, state=%s", key,
                 state ? "ON" : "OFF");
        return callback(key, state);
    }

    /* No callback, just update state directly */
    return esphome_entity_update_switch(key, state);
}

/* ============================================================================
 * Number Registration and State
 * ============================================================================ */

/**
 * @brief Register a number entity
 */
esp_err_t esphome_entity_register_number(const esphome_number_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Check for duplicate key */
    if (find_number(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Number with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities->number_count >= ESPHOME_MAX_NUMBERS) {
        ESP_LOGE(ENTITY_TAG, "Maximum numbers reached (%d)", ESPHOME_MAX_NUMBERS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add number */
    size_t idx = s_entities->number_count;
    memcpy(&s_entities->numbers[idx].config, config, sizeof(esphome_number_config_t));
    s_entities->numbers[idx].state.key = config->key;
    s_entities->numbers[idx].state.device_id = config->device_id;
    s_entities->numbers[idx].state.state = 0.0f;
    s_entities->numbers[idx].state.missing_state = true;
    s_entities->numbers[idx].registered = true;
    s_entities->number_count++;

    ESP_LOGI(ENTITY_TAG, "Registered number: key=%lu, name='%s', range=[%.1f, %.1f]",
             config->key, config->name, config->min_value, config->max_value);

    /* Mirror into the entity state store (best-effort, non-blocking) */
    if (esphome_entity_mirror_is_initialized()) {
        esp_err_t reg_ret = esphome_entity_mirror_register(
            ESPHOME_ENTITY_NUMBER, config->key, config->name, config->unique_id);
        if (reg_ret != ESP_OK) {
            ESP_LOGD(ENTITY_TAG, "Entity mirror registration skipped: %s",
                     esp_err_to_name(reg_ret));
        }
    }

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Update number state
 */
esp_err_t esphome_entity_update_number(esphome_entity_key_t key, float value)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    number_entry_t *entry = find_number(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.state = value;
    entry->state.missing_state = false;

    ESP_LOGD(ENTITY_TAG, "Number %lu state: %.2f", key, value);

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_NUMBER, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Set number to missing/unavailable state
 */
esp_err_t esphome_entity_set_number_missing(esphome_entity_key_t key)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    number_entry_t *entry = find_number(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.missing_state = true;

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_NUMBER, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number state
 */
esp_err_t esphome_entity_get_number(esphome_entity_key_t key, esphome_number_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    number_entry_t *entry = find_number(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(state, &entry->state, sizeof(esphome_number_state_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number configuration
 */
esp_err_t esphome_entity_get_number_config(esphome_entity_key_t key,
                                            esphome_number_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    number_entry_t *entry = find_number(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_number_config_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number of registered numbers
 */
size_t esphome_entity_get_number_count(void)
{
    return s_entities->number_count;
}

/**
 * @brief Execute number command
 */
esp_err_t esphome_entity_execute_number_command(esphome_entity_key_t key, float value)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    number_entry_t *entry = find_number(key);
    esphome_number_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities->mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing number command: key=%lu, value=%.2f", key, value);
        return callback(key, value);
    }

    /* No callback, just update state directly */
    return esphome_entity_update_number(key, value);
}

/* ============================================================================
 * Select Registration and State
 * ============================================================================ */

/**
 * @brief Register a select entity
 */
esp_err_t esphome_entity_register_select(const esphome_select_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Check for duplicate key */
    if (find_select(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Select with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities->select_count >= ESPHOME_MAX_SELECTS) {
        ESP_LOGE(ENTITY_TAG, "Maximum selects reached (%d)", ESPHOME_MAX_SELECTS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add select */
    size_t idx = s_entities->select_count;
    memcpy(&s_entities->selects[idx].config, config, sizeof(esphome_select_config_t));
    s_entities->selects[idx].state.key = config->key;
    s_entities->selects[idx].state.device_id = config->device_id;
    s_entities->selects[idx].state.state[0] = '\0';
    s_entities->selects[idx].state.missing_state = true;
    s_entities->selects[idx].registered = true;
    s_entities->select_count++;

    ESP_LOGI(ENTITY_TAG, "Registered select: key=%lu, name='%s', options=%d",
             config->key, config->name, config->option_count);

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Update select state
 */
esp_err_t esphome_entity_update_select(esphome_entity_key_t key, const char *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    select_entry_t *entry = find_select(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    strncpy(entry->state.state, state, sizeof(entry->state.state) - 1);
    entry->state.state[sizeof(entry->state.state) - 1] = '\0';
    entry->state.missing_state = false;

    ESP_LOGD(ENTITY_TAG, "Select %lu state: '%s'", key, state);

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_SELECT, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Set select to missing/unavailable state
 */
esp_err_t esphome_entity_set_select_missing(esphome_entity_key_t key)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    select_entry_t *entry = find_select(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.missing_state = true;

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_SELECT, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get select state
 */
esp_err_t esphome_entity_get_select(esphome_entity_key_t key, esphome_select_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    select_entry_t *entry = find_select(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(state, &entry->state, sizeof(esphome_select_state_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get select configuration
 */
esp_err_t esphome_entity_get_select_config(esphome_entity_key_t key,
                                            esphome_select_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    select_entry_t *entry = find_select(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_select_config_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number of registered selects
 */
size_t esphome_entity_get_select_count(void)
{
    return s_entities->select_count;
}

/**
 * @brief Execute select command
 */
esp_err_t esphome_entity_execute_select_command(esphome_entity_key_t key, const char *option)
{
    if (!option) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    select_entry_t *entry = find_select(key);
    esphome_select_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities->mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing select command: key=%lu, option='%s'", key, option);
        return callback(key, option);
    }

    /* No callback, just update state directly */
    return esphome_entity_update_select(key, option);
}

/* ============================================================================
 * Light Registration and State
 * ============================================================================ */

/**
 * @brief Register a light entity
 */
esp_err_t esphome_entity_register_light(const esphome_light_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Check for duplicate key */
    if (find_light(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Light with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities->light_count >= ESPHOME_MAX_LIGHTS) {
        ESP_LOGE(ENTITY_TAG, "Maximum lights reached (%d)", ESPHOME_MAX_LIGHTS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add light */
    size_t idx = s_entities->light_count;
    memcpy(&s_entities->lights[idx].config, config, sizeof(esphome_light_config_t));
    s_entities->lights[idx].state.key = config->key;
    s_entities->lights[idx].state.device_id = config->device_id;
    s_entities->lights[idx].state.state = false;
    s_entities->lights[idx].state.brightness = 0.0f;
    s_entities->lights[idx].state.color_mode = ESPHOME_COLOR_MODE_UNKNOWN;
    s_entities->lights[idx].state.color_temp = 0.0f;
    s_entities->lights[idx].state.red = 0.0f;
    s_entities->lights[idx].state.green = 0.0f;
    s_entities->lights[idx].state.blue = 0.0f;
    s_entities->lights[idx].state.white = 0.0f;
    s_entities->lights[idx].state.effect[0] = '\0';
    s_entities->lights[idx].registered = true;
    s_entities->light_count++;

    ESP_LOGI(ENTITY_TAG, "Registered light: key=%lu, name='%s', color_modes=0x%lx",
             config->key, config->name, config->supported_color_modes);

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Update light state
 */
esp_err_t esphome_entity_update_light(esphome_entity_key_t key, const esphome_light_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    light_entry_t *entry = find_light(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    /* Copy the entire state structure, preserving the key */
    entry->state.state = state->state;
    entry->state.brightness = state->brightness;
    entry->state.color_mode = state->color_mode;
    entry->state.color_temp = state->color_temp;
    entry->state.red = state->red;
    entry->state.green = state->green;
    entry->state.blue = state->blue;
    entry->state.white = state->white;
    strncpy(entry->state.effect, state->effect, sizeof(entry->state.effect) - 1);
    entry->state.effect[sizeof(entry->state.effect) - 1] = '\0';

    ESP_LOGD(ENTITY_TAG, "Light %lu state: %s, brightness=%.2f", key,
             entry->state.state ? "ON" : "OFF", entry->state.brightness);

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_LIGHT, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get light state
 */
esp_err_t esphome_entity_get_light(esphome_entity_key_t key, esphome_light_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    light_entry_t *entry = find_light(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(state, &entry->state, sizeof(esphome_light_state_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get light configuration
 */
esp_err_t esphome_entity_get_light_config(esphome_entity_key_t key,
                                           esphome_light_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    light_entry_t *entry = find_light(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_light_config_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number of registered lights
 */
size_t esphome_entity_get_light_count(void)
{
    return s_entities->light_count;
}

/**
 * @brief Execute light command
 */
esp_err_t esphome_entity_execute_light_command(esphome_entity_key_t key,
                                                const esphome_light_command_t *command)
{
    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    light_entry_t *entry = find_light(key);
    esphome_light_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities->mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing light command: key=%lu, state=%s, brightness=%.2f",
                 key, command->has_state ? (command->state ? "ON" : "OFF") : "N/A",
                 command->has_brightness ? command->brightness : -1.0f);
        return callback(key, command);
    }

    /* No callback - apply command directly to state */
    esphome_light_state_t new_state;
    esp_err_t ret = esphome_entity_get_light(key, &new_state);
    if (ret != ESP_OK) {
        return ret;
    }

    if (command->has_state) {
        new_state.state = command->state;
    }
    if (command->has_brightness) {
        new_state.brightness = command->brightness;
    }
    if (command->has_color_mode) {
        new_state.color_mode = command->color_mode;
    }
    if (command->has_color_temp) {
        new_state.color_temp = command->color_temp;
    }
    if (command->has_rgb) {
        new_state.red = command->red;
        new_state.green = command->green;
        new_state.blue = command->blue;
    }
    if (command->has_white) {
        new_state.white = command->white;
    }
    if (command->has_effect) {
        strncpy(new_state.effect, command->effect, sizeof(new_state.effect) - 1);
        new_state.effect[sizeof(new_state.effect) - 1] = '\0';
    }

    return esphome_entity_update_light(key, &new_state);
}

/* ============================================================================
 * Cover Registration and State
 * ============================================================================ */

/**
 * @brief Register a cover entity
 */
esp_err_t esphome_entity_register_cover(const esphome_cover_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Check for duplicate key */
    if (find_cover(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Cover with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities->cover_count >= ESPHOME_MAX_COVERS) {
        ESP_LOGE(ENTITY_TAG, "Maximum covers reached (%d)", ESPHOME_MAX_COVERS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add cover */
    size_t idx = s_entities->cover_count;
    memcpy(&s_entities->covers[idx].config, config, sizeof(esphome_cover_config_t));
    s_entities->covers[idx].state.key = config->key;
    s_entities->covers[idx].state.device_id = config->device_id;
    s_entities->covers[idx].state.position = 0.0f;
    s_entities->covers[idx].state.tilt = 0.0f;
    s_entities->covers[idx].state.current_operation = ESPHOME_COVER_OPERATION_IDLE;
    s_entities->covers[idx].registered = true;
    s_entities->cover_count++;

    ESP_LOGI(ENTITY_TAG, "Registered cover: key=%lu, name='%s', pos=%d, tilt=%d",
             config->key, config->name, config->supports_position, config->supports_tilt);

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Update cover state
 */
esp_err_t esphome_entity_update_cover(esphome_entity_key_t key,
                                       const esphome_cover_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    cover_entry_t *entry = find_cover(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.position = state->position;
    entry->state.tilt = state->tilt;
    entry->state.current_operation = state->current_operation;

    ESP_LOGD(ENTITY_TAG, "Cover %lu state: pos=%.2f, tilt=%.2f, op=%d",
             key, state->position, state->tilt, state->current_operation);

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_COVER, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get cover state
 */
esp_err_t esphome_entity_get_cover(esphome_entity_key_t key, esphome_cover_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    cover_entry_t *entry = find_cover(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(state, &entry->state, sizeof(esphome_cover_state_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get cover configuration
 */
esp_err_t esphome_entity_get_cover_config(esphome_entity_key_t key,
                                           esphome_cover_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    cover_entry_t *entry = find_cover(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_cover_config_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number of registered covers
 */
size_t esphome_entity_get_cover_count(void)
{
    return s_entities->cover_count;
}

/**
 * @brief Execute cover command
 */
esp_err_t esphome_entity_execute_cover_command(esphome_entity_key_t key,
                                                const esphome_cover_command_t *command)
{
    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    cover_entry_t *entry = find_cover(key);
    esphome_cover_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities->mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing cover command: key=%lu, pos=%.2f, tilt=%.2f, stop=%d",
                 key, command->has_position ? command->position : -1.0f,
                 command->has_tilt ? command->tilt : -1.0f, command->stop);
        return callback(key, command);
    }

    /* No callback - apply command directly to state */
    esphome_cover_state_t current_state;
    esp_err_t ret = esphome_entity_get_cover(key, &current_state);
    if (ret != ESP_OK) {
        return ret;
    }

    esphome_cover_state_t new_state = current_state;

    if (command->stop) {
        new_state.current_operation = ESPHOME_COVER_OPERATION_IDLE;
    } else if (command->has_position) {
        new_state.position = command->position;
        /* Determine operation based on direction */
        if (command->position > current_state.position) {
            new_state.current_operation = ESPHOME_COVER_OPERATION_OPENING;
        } else if (command->position < current_state.position) {
            new_state.current_operation = ESPHOME_COVER_OPERATION_CLOSING;
        } else {
            new_state.current_operation = ESPHOME_COVER_OPERATION_IDLE;
        }
    }

    if (command->has_tilt) {
        new_state.tilt = command->tilt;
    }

    return esphome_entity_update_cover(key, &new_state);
}

/* ============================================================================
 * Fan Registration and State
 * ============================================================================ */

/**
 * @brief Register a fan entity
 */
esp_err_t esphome_entity_register_fan(const esphome_fan_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Check for duplicate key */
    if (find_fan(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Fan with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities->fan_count >= ESPHOME_MAX_FANS) {
        ESP_LOGE(ENTITY_TAG, "Maximum fans reached (%d)", ESPHOME_MAX_FANS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add fan */
    size_t idx = s_entities->fan_count;
    memcpy(&s_entities->fans[idx].config, config, sizeof(esphome_fan_config_t));
    s_entities->fans[idx].state.key = config->key;
    s_entities->fans[idx].state.device_id = config->device_id;
    s_entities->fans[idx].state.state = false;
    s_entities->fans[idx].state.oscillating = false;
    s_entities->fans[idx].state.speed_level = 0;
    s_entities->fans[idx].state.direction = ESPHOME_FAN_DIRECTION_FORWARD;
    s_entities->fans[idx].registered = true;
    s_entities->fan_count++;

    ESP_LOGI(ENTITY_TAG, "Registered fan: key=%lu, name='%s'", config->key, config->name);

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Update fan state
 */
esp_err_t esphome_entity_update_fan(esphome_entity_key_t key, const esphome_fan_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    fan_entry_t *entry = find_fan(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.state = state->state;
    entry->state.oscillating = state->oscillating;
    entry->state.speed_level = state->speed_level;
    entry->state.direction = state->direction;

    ESP_LOGD(ENTITY_TAG, "Fan %lu state: %s, speed=%d", key, state->state ? "ON" : "OFF",
             state->speed_level);

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_FAN, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get fan state
 */
esp_err_t esphome_entity_get_fan(esphome_entity_key_t key, esphome_fan_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    fan_entry_t *entry = find_fan(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(state, &entry->state, sizeof(esphome_fan_state_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get fan configuration
 */
esp_err_t esphome_entity_get_fan_config(esphome_entity_key_t key, esphome_fan_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    fan_entry_t *entry = find_fan(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_fan_config_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number of registered fans
 */
size_t esphome_entity_get_fan_count(void)
{
    return s_entities->fan_count;
}

/**
 * @brief Execute fan command
 */
esp_err_t esphome_entity_execute_fan_command(esphome_entity_key_t key,
                                              const esphome_fan_command_t *cmd)
{
    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    fan_entry_t *entry = find_fan(key);
    esphome_fan_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities->mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing fan command: key=%lu", key);
        return callback(key, cmd);
    }

    /* No callback, just update state directly */
    esphome_fan_state_t new_state;
    esphome_entity_get_fan(key, &new_state);

    if (cmd->has_state) {
        new_state.state = cmd->state;
    }
    if (cmd->has_oscillating) {
        new_state.oscillating = cmd->oscillating;
    }
    if (cmd->has_speed_level) {
        new_state.speed_level = cmd->speed_level;
    }
    if (cmd->has_direction) {
        new_state.direction = cmd->direction;
    }

    return esphome_entity_update_fan(key, &new_state);
}

/* ============================================================================
 * Climate Registration and State
 * ============================================================================ */

/**
 * @brief Register a climate entity
 */
esp_err_t esphome_entity_register_climate(const esphome_climate_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Check for duplicate key */
    if (find_climate(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Climate with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities->climate_count >= ESPHOME_MAX_CLIMATES) {
        ESP_LOGE(ENTITY_TAG, "Maximum climates reached (%d)", ESPHOME_MAX_CLIMATES);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add climate */
    size_t idx = s_entities->climate_count;
    memcpy(&s_entities->climates[idx].config, config, sizeof(esphome_climate_config_t));
    s_entities->climates[idx].state.key = config->key;
    s_entities->climates[idx].state.device_id = config->device_id;
    s_entities->climates[idx].state.mode = ESPHOME_CLIMATE_MODE_OFF;
    s_entities->climates[idx].state.current_temperature = 0.0f;
    s_entities->climates[idx].state.target_temperature = 0.0f;
    s_entities->climates[idx].state.target_temperature_low = 0.0f;
    s_entities->climates[idx].state.target_temperature_high = 0.0f;
    s_entities->climates[idx].state.action = ESPHOME_CLIMATE_ACTION_OFF;
    s_entities->climates[idx].state.fan_mode = ESPHOME_CLIMATE_FAN_AUTO;
    s_entities->climates[idx].state.swing_mode = ESPHOME_CLIMATE_SWING_OFF;
    s_entities->climates[idx].state.preset = ESPHOME_CLIMATE_PRESET_NONE;
    s_entities->climates[idx].registered = true;
    s_entities->climate_count++;

    ESP_LOGI(ENTITY_TAG, "Registered climate: key=%lu, name='%s'", config->key, config->name);

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Update climate state
 */
esp_err_t esphome_entity_update_climate(esphome_entity_key_t key,
                                         const esphome_climate_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    climate_entry_t *entry = find_climate(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(&entry->state, state, sizeof(esphome_climate_state_t));
    entry->state.key = key;

    ESP_LOGD(ENTITY_TAG, "Climate %lu state: mode=%d, target=%.1f", key, state->mode,
             state->target_temperature);

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_CLIMATE, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get climate state
 */
esp_err_t esphome_entity_get_climate(esphome_entity_key_t key, esphome_climate_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    climate_entry_t *entry = find_climate(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(state, &entry->state, sizeof(esphome_climate_state_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get climate configuration
 */
esp_err_t esphome_entity_get_climate_config(esphome_entity_key_t key,
                                             esphome_climate_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    climate_entry_t *entry = find_climate(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_climate_config_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number of registered climates
 */
size_t esphome_entity_get_climate_count(void)
{
    return s_entities->climate_count;
}

/**
 * @brief Execute climate command
 */
esp_err_t esphome_entity_execute_climate_command(esphome_entity_key_t key,
                                                  const esphome_climate_command_t *cmd)
{
    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    climate_entry_t *entry = find_climate(key);
    esphome_climate_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities->mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing climate command: key=%lu", key);
        return callback(key, cmd);
    }

    /* No callback, just update state directly */
    esphome_climate_state_t new_state;
    esphome_entity_get_climate(key, &new_state);

    if (cmd->has_mode) {
        new_state.mode = cmd->mode;
    }
    if (cmd->has_target_temperature) {
        new_state.target_temperature = cmd->target_temperature;
    }
    if (cmd->has_target_temperature_low) {
        new_state.target_temperature_low = cmd->target_temperature_low;
    }
    if (cmd->has_target_temperature_high) {
        new_state.target_temperature_high = cmd->target_temperature_high;
    }
    if (cmd->has_fan_mode) {
        new_state.fan_mode = cmd->fan_mode;
    }
    if (cmd->has_swing_mode) {
        new_state.swing_mode = cmd->swing_mode;
    }
    if (cmd->has_preset) {
        new_state.preset = cmd->preset;
    }

    return esphome_entity_update_climate(key, &new_state);
}

/* ============================================================================
 * Message Encoding - Switch
 * ============================================================================ */

/**
 * @brief Encode switch entity info for ListEntities response
 */
esp_err_t esphome_encode_switch_list_entry(const esphome_switch_config_t *config,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len)
{
    if (!config || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_MEDIUM];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: object_id (string) */
    esphome_encode_string(&buf, 1, config->unique_id);

    /* Field 2: key (fixed32) */
    esphome_encode_fixed32(&buf, 2, config->key);

    /* Field 3: name (string) */
    esphome_encode_string(&buf, 3, config->name);

    /* Field 4: unique_id (string) */
    esphome_encode_string(&buf, 4, config->unique_id);

    /* Field 5: icon (string) */
    if (config->icon[0] != '\0') {
        esphome_encode_string(&buf, 5, config->icon);
    }

    /* Field 6: assumed_state (bool) */
    if (config->assumed_state) {
        esphome_encode_bool(&buf, 6, config->assumed_state);
    }

    /* Field 7: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 7, config->disabled_by_default);
    }
    /* Field 8: entity_category (0=NONE, 1=CONFIG, 2=DIAGNOSTIC)
     *
     * Without this Home Assistant shows everything a device exposes as an
     * equal, primary control — a Fingerbot listed 'Reverse', 'Program
     * Enable' and 'Down Movement' beside its actual switch. Categorised,
     * HA folds settings into the device's Configuration section.
     *
     * The number is the gap this encoder already left: it writes
     * disabled_by_default and then skips to the next field, exactly where
     * api.proto places entity_category. */
    if (config->entity_category != 0) {
        esphome_encode_uint32(&buf, 8, (uint32_t)config->entity_category);
    }

    /* Field 10: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 10, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_SWITCH, payload, buf.position,
                                 output, output_size, output_len);
}

/**
 * @brief Encode switch state message
 */
esp_err_t esphome_encode_switch_state(const esphome_switch_state_t *state,
                                       uint8_t *output, size_t output_size,
                                       size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_PAYLOAD_SMALL];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: key (fixed32) */
    esphome_encode_fixed32(&buf, 1, state->key);

    /* Field 2: state (bool) */
    esphome_encode_bool(&buf, 2, state->state);

    /* Field 3: device_id (uint32) - required for sub-device entity availability */
    if (state->device_id != 0) {
        esphome_encode_uint32(&buf, 3, state->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_SWITCH_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Number
 * ============================================================================ */

/**
 * @brief Encode number entity info for ListEntities response
 */
esp_err_t esphome_encode_number_list_entry(const esphome_number_config_t *config,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len)
{
    if (!config || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_MEDIUM];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: object_id (string) */
    esphome_encode_string(&buf, 1, config->unique_id);

    /* Field 2: key (fixed32) */
    esphome_encode_fixed32(&buf, 2, config->key);

    /* Field 3: name (string) */
    esphome_encode_string(&buf, 3, config->name);

    /* Field 4: unique_id (string) */
    esphome_encode_string(&buf, 4, config->unique_id);

    /* Field 5: icon (string) */
    if (config->icon[0] != '\0') {
        esphome_encode_string(&buf, 5, config->icon);
    }

    /* Field 6: min_value (float) */
    esphome_encode_float(&buf, 6, config->min_value);

    /* Field 7: max_value (float) */
    esphome_encode_float(&buf, 7, config->max_value);

    /* Field 8: step (float) */
    esphome_encode_float(&buf, 8, config->step);

    /* Field 9: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 9, config->disabled_by_default);
    }
    /* Field 10: entity_category (0=NONE, 1=CONFIG, 2=DIAGNOSTIC)
     *
     * Without this Home Assistant shows everything a device exposes as an
     * equal, primary control — a Fingerbot listed 'Reverse', 'Program
     * Enable' and 'Down Movement' beside its actual switch. Categorised,
     * HA folds settings into the device's Configuration section.
     *
     * The number is the gap this encoder already left: it writes
     * disabled_by_default and then skips to the next field, exactly where
     * api.proto places entity_category. */
    if (config->entity_category != 0) {
        esphome_encode_uint32(&buf, 10, (uint32_t)config->entity_category);
    }

    /* Field 11: unit_of_measurement (string) */
    if (config->unit_of_measurement[0] != '\0') {
        esphome_encode_string(&buf, 11, config->unit_of_measurement);
    }

    /* Field 12: mode (enum) */
    if (config->mode != ESPHOME_NUMBER_MODE_AUTO) {
        esphome_encode_uint32(&buf, 12, (uint32_t)config->mode);
    }

    /* Field 14: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 14, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_NUMBER, payload, buf.position,
                                 output, output_size, output_len);
}

/**
 * @brief Encode number state message
 */
esp_err_t esphome_encode_number_state(const esphome_number_state_t *state,
                                       uint8_t *output, size_t output_size,
                                       size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_OUTPUT_BUFFER_STANDARD];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: key (fixed32) */
    esphome_encode_fixed32(&buf, 1, state->key);

    /* Field 2: state (float) */
    esphome_encode_float(&buf, 2, state->state);

    /* Field 3: missing_state (bool) */
    if (state->missing_state) {
        esphome_encode_bool(&buf, 3, state->missing_state);
    }

    /* Field 4: device_id (uint32) - required for sub-device entity availability */
    if (state->device_id != 0) {
        esphome_encode_uint32(&buf, 4, state->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_NUMBER_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Select
 * ============================================================================ */

/**
 * @brief Encode select entity info for ListEntities response
 */
esp_err_t esphome_encode_select_list_entry(const esphome_select_config_t *config,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len)
{
    if (!config || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_LARGE];  /* Larger buffer for options array */
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: object_id (string) */
    esphome_encode_string(&buf, 1, config->unique_id);

    /* Field 2: key (fixed32) */
    esphome_encode_fixed32(&buf, 2, config->key);

    /* Field 3: name (string) */
    esphome_encode_string(&buf, 3, config->name);

    /* Field 4: unique_id (string) */
    esphome_encode_string(&buf, 4, config->unique_id);

    /* Field 5: icon (string) */
    if (config->icon[0] != '\0') {
        esphome_encode_string(&buf, 5, config->icon);
    }

    /* Field 6: options[] (repeated string) */
    for (int i = 0; i < config->option_count && i < ESPHOME_MAX_SELECT_OPTIONS; i++) {
        if (config->options[i][0] != '\0') {
            esphome_encode_string(&buf, 6, config->options[i]);
        }
    }

    /* Field 7: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 7, config->disabled_by_default);
    }
    /* Field 8: entity_category (0=NONE, 1=CONFIG, 2=DIAGNOSTIC)
     *
     * Without this Home Assistant shows everything a device exposes as an
     * equal, primary control — a Fingerbot listed 'Reverse', 'Program
     * Enable' and 'Down Movement' beside its actual switch. Categorised,
     * HA folds settings into the device's Configuration section.
     *
     * The number is the gap this encoder already left: it writes
     * disabled_by_default and then skips to the next field, exactly where
     * api.proto places entity_category. */
    if (config->entity_category != 0) {
        esphome_encode_uint32(&buf, 8, (uint32_t)config->entity_category);
    }

    /* Field 9: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 9, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_SELECT, payload, buf.position,
                                 output, output_size, output_len);
}

/**
 * @brief Encode select state message
 */
esp_err_t esphome_encode_select_state(const esphome_select_state_t *state,
                                       uint8_t *output, size_t output_size,
                                       size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_SMALL];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: key (fixed32) */
    esphome_encode_fixed32(&buf, 1, state->key);

    /* Field 2: state (string) */
    esphome_encode_string(&buf, 2, state->state);

    /* Field 3: missing_state (bool) */
    if (state->missing_state) {
        esphome_encode_bool(&buf, 3, state->missing_state);
    }

    /* Field 4: device_id (uint32) - required for sub-device entity availability */
    if (state->device_id != 0) {
        esphome_encode_uint32(&buf, 4, state->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_SELECT_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Light
 * ============================================================================ */

/**
 * @brief Encode light entity info for ListEntities response
 */
esp_err_t esphome_encode_light_list_entry(const esphome_light_config_t *config,
                                           uint8_t *output, size_t output_size,
                                           size_t *output_len)
{
    if (!config || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_LARGE];  /* Larger buffer for effects array */
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Protobuf: ListEntitiesLightResponse
     *   1: object_id, 2: key, 3: name, 4: unique_id,
     *   5: legacy_supports_brightness (bool), 6: legacy_supports_rgb (bool),
     *   7: legacy_supports_white_value (bool), 8: legacy_supports_color_temperature (bool),
     *   9: min_mireds (float), 10: max_mireds (float),
     *   11: effects (repeated string), 12: supported_color_modes (repeated ColorMode),
     *   13: disabled_by_default (bool), 14: icon (string),
     *   15: entity_category (enum), 16: device_id (uint32) */

    /* Field 1: object_id (string) */
    esphome_encode_string(&buf, 1, config->unique_id);

    /* Field 2: key (fixed32) */
    esphome_encode_fixed32(&buf, 2, config->key);

    /* Field 3: name (string) */
    esphome_encode_string(&buf, 3, config->name);

    /* Field 4: unique_id (string) */
    esphome_encode_string(&buf, 4, config->unique_id);

    /* Fields 5-8: legacy fields - skip (deprecated) */

    /* Field 9: min_mireds (float) */
    if (config->min_mireds > 0) {
        esphome_encode_float(&buf, 9, config->min_mireds);
    }

    /* Field 10: max_mireds (float) */
    if (config->max_mireds > 0) {
        esphome_encode_float(&buf, 10, config->max_mireds);
    }

    /* Field 11: effects[] (repeated string) */
    for (int i = 0; i < config->effect_count && i < ESPHOME_MAX_LIGHT_EFFECTS; i++) {
        if (config->effects[i][0] != '\0') {
            esphome_encode_string(&buf, 11, config->effects[i]);
        }
    }

    /* Field 12: supported_color_modes[] (repeated ColorMode) */
    if (config->supported_color_modes & ESPHOME_COLOR_MODE_ON_OFF) {
        esphome_encode_uint32(&buf, 12, ESPHOME_COLOR_MODE_ON_OFF);
    }
    if (config->supported_color_modes & ESPHOME_COLOR_MODE_BRIGHTNESS) {
        esphome_encode_uint32(&buf, 12, ESPHOME_COLOR_MODE_BRIGHTNESS);
    }
    if (config->supported_color_modes & ESPHOME_COLOR_MODE_WHITE) {
        esphome_encode_uint32(&buf, 12, ESPHOME_COLOR_MODE_WHITE);
    }
    if (config->supported_color_modes & ESPHOME_COLOR_MODE_COLOR_TEMP) {
        esphome_encode_uint32(&buf, 12, ESPHOME_COLOR_MODE_COLOR_TEMP);
    }
    if (config->supported_color_modes & ESPHOME_COLOR_MODE_RGB) {
        esphome_encode_uint32(&buf, 12, ESPHOME_COLOR_MODE_RGB);
    }
    if (config->supported_color_modes & ESPHOME_COLOR_MODE_RGB_WHITE) {
        esphome_encode_uint32(&buf, 12, ESPHOME_COLOR_MODE_RGB_WHITE);
    }
    if (config->supported_color_modes & ESPHOME_COLOR_MODE_RGB_CT) {
        esphome_encode_uint32(&buf, 12, ESPHOME_COLOR_MODE_RGB_CT);
    }

    /* Field 13: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 13, config->disabled_by_default);
    }

    /* Field 14: icon (string) */
    if (config->icon[0] != '\0') {
        esphome_encode_string(&buf, 14, config->icon);
    }

    /* Field 16: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 16, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_LIGHT, payload, buf.position,
                                 output, output_size, output_len);
}

/**
 * @brief Encode light state message
 */
esp_err_t esphome_encode_light_state(const esphome_light_state_t *state,
                                      uint8_t *output, size_t output_size,
                                      size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_SMALL];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* LightStateResponse proto fields:
     *   1: key (fixed32), 2: state (bool), 3: brightness (float),
     *   4: red (float), 5: green (float), 6: blue (float),
     *   7: white (float), 8: color_temperature (float), 9: effect (string),
     *   10: color_brightness (float), 11: color_mode (ColorMode),
     *   14: device_id (uint32) */

    /* Field 1: key (fixed32) */
    esphome_encode_fixed32(&buf, 1, state->key);

    /* Field 2: state (bool) */
    esphome_encode_bool(&buf, 2, state->state);

    /* Field 3: brightness (float) */
    if (state->brightness > 0.0f) {
        esphome_encode_float(&buf, 3, state->brightness);
    }

    /* Field 4: red (float) */
    if (state->red > 0.0f) {
        esphome_encode_float(&buf, 4, state->red);
    }

    /* Field 5: green (float) */
    if (state->green > 0.0f) {
        esphome_encode_float(&buf, 5, state->green);
    }

    /* Field 6: blue (float) */
    if (state->blue > 0.0f) {
        esphome_encode_float(&buf, 6, state->blue);
    }

    /* Field 7: white (float) */
    if (state->white > 0.0f) {
        esphome_encode_float(&buf, 7, state->white);
    }

    /* Field 8: color_temperature (float) */
    if (state->color_temp > 0.0f) {
        esphome_encode_float(&buf, 8, state->color_temp);
    }

    /* Field 9: effect (string) */
    if (state->effect[0] != '\0') {
        esphome_encode_string(&buf, 9, state->effect);
    }

    /* Field 11: color_mode (ColorMode enum) */
    if (state->color_mode != ESPHOME_COLOR_MODE_UNKNOWN) {
        esphome_encode_uint32(&buf, 11, (uint32_t)state->color_mode);
    }

    /* Field 14: device_id (uint32) - required for sub-device entity availability */
    if (state->device_id != 0) {
        esphome_encode_uint32(&buf, 14, state->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIGHT_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Cover
 * ============================================================================ */

/**
 * @brief Encode cover entity info for ListEntities response
 */
esp_err_t esphome_encode_cover_list_entry(const esphome_cover_config_t *config,
                                           uint8_t *output, size_t output_size,
                                           size_t *output_len)
{
    if (!config || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_MEDIUM];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Protobuf: ListEntitiesCoverResponse
     *   1: object_id, 2: key, 3: name, 4: unique_id,
     *   5: assumed_state (bool), 6: supports_position (bool),
     *   7: supports_tilt (bool), 8: device_class (string),
     *   9: disabled_by_default (bool), 10: icon (string),
     *   11: entity_category (enum), 12: supports_stop (bool),
     *   13: device_id (uint32) */

    /* Field 1: object_id (string) */
    esphome_encode_string(&buf, 1, config->unique_id);

    /* Field 2: key (fixed32) */
    esphome_encode_fixed32(&buf, 2, config->key);

    /* Field 3: name (string) */
    esphome_encode_string(&buf, 3, config->name);

    /* Field 4: unique_id (string) */
    esphome_encode_string(&buf, 4, config->unique_id);

    /* Field 5: assumed_state (bool) */
    if (config->assumed_state) {
        esphome_encode_bool(&buf, 5, config->assumed_state);
    }

    /* Field 6: supports_position (bool) */
    if (config->supports_position) {
        esphome_encode_bool(&buf, 6, config->supports_position);
    }

    /* Field 7: supports_tilt (bool) */
    if (config->supports_tilt) {
        esphome_encode_bool(&buf, 7, config->supports_tilt);
    }

    /* Field 8: device_class (string) */
    if (config->device_class != ESPHOME_COVER_CLASS_NONE) {
        /* Cover device_class is a string in the proto */
        const char *dc_str = "";
        switch (config->device_class) {
            case ESPHOME_COVER_CLASS_AWNING:   dc_str = "awning"; break;
            case ESPHOME_COVER_CLASS_BLIND:    dc_str = "blind"; break;
            case ESPHOME_COVER_CLASS_CURTAIN:  dc_str = "curtain"; break;
            case ESPHOME_COVER_CLASS_DAMPER:   dc_str = "damper"; break;
            case ESPHOME_COVER_CLASS_DOOR:     dc_str = "door"; break;
            case ESPHOME_COVER_CLASS_GARAGE:   dc_str = "garage"; break;
            case ESPHOME_COVER_CLASS_GATE:     dc_str = "gate"; break;
            case ESPHOME_COVER_CLASS_SHADE:    dc_str = "shade"; break;
            case ESPHOME_COVER_CLASS_SHUTTER:  dc_str = "shutter"; break;
            case ESPHOME_COVER_CLASS_WINDOW:   dc_str = "window"; break;
            default: break;
        }
        if (dc_str[0] != '\0') {
            esphome_encode_string(&buf, 8, dc_str);
        }
    }

    /* Field 9: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 9, config->disabled_by_default);
    }

    /* Field 10: icon (string) */
    if (config->icon[0] != '\0') {
        esphome_encode_string(&buf, 10, config->icon);
    }

    /* Field 13: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 13, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_COVER, payload, buf.position,
                                 output, output_size, output_len);
}

/**
 * @brief Encode cover state message
 */
esp_err_t esphome_encode_cover_state(const esphome_cover_state_t *state,
                                      uint8_t *output, size_t output_size,
                                      size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_OUTPUT_BUFFER_STANDARD];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* CoverStateResponse proto fields:
     *   1: key (fixed32), 2: legacy_state (deprecated, skip),
     *   3: position (float), 4: tilt (float),
     *   5: current_operation (CoverOperation), 6: device_id (uint32) */

    /* Field 1: key (fixed32) */
    esphome_encode_fixed32(&buf, 1, state->key);

    /* Field 2: legacy_state - skipped (deprecated) */

    /* Field 3: position (float) */
    esphome_encode_float(&buf, 3, state->position);

    /* Field 4: tilt (float) */
    esphome_encode_float(&buf, 4, state->tilt);

    /* Field 5: current_operation (enum) */
    esphome_encode_uint32(&buf, 5, (uint32_t)state->current_operation);

    /* Field 6: device_id (uint32) - required for sub-device entity availability */
    if (state->device_id != 0) {
        esphome_encode_uint32(&buf, 6, state->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_COVER_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Fan
 * ============================================================================ */

/**
 * @brief Encode fan entity info for ListEntities response
 */
esp_err_t esphome_encode_fan_list_entry(const esphome_fan_config_t *config,
                                         uint8_t *output, size_t output_size,
                                         size_t *output_len)
{
    if (!config || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_MEDIUM];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Protobuf: ListEntitiesFanResponse
     *   1: object_id, 2: key, 3: name, 4: unique_id,
     *   5: supports_oscillation (bool), 6: supports_speed (bool),
     *   7: supports_direction (bool), 8: supported_speed_count (int32),
     *   9: disabled_by_default (bool), 10: icon (string),
     *   11: entity_category (enum), 12: supported_preset_modes (repeated string),
     *   13: device_id (uint32) */

    /* Field 1: object_id (string) */
    esphome_encode_string(&buf, 1, config->unique_id);

    /* Field 2: key (fixed32) */
    esphome_encode_fixed32(&buf, 2, config->key);

    /* Field 3: name (string) */
    esphome_encode_string(&buf, 3, config->name);

    /* Field 4: unique_id (string) */
    esphome_encode_string(&buf, 4, config->unique_id);

    /* Field 5: supports_oscillation (bool) */
    if (config->supports_oscillation) {
        esphome_encode_bool(&buf, 5, config->supports_oscillation);
    }

    /* Field 6: supports_speed (bool) */
    if (config->supports_speed) {
        esphome_encode_bool(&buf, 6, config->supports_speed);
    }

    /* Field 7: supports_direction (bool) */
    if (config->supports_direction) {
        esphome_encode_bool(&buf, 7, config->supports_direction);
    }

    /* Field 8: supported_speed_count (int32) */
    if (config->supported_speed_count > 0) {
        esphome_encode_int32(&buf, 8, config->supported_speed_count);
    }

    /* Field 9: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 9, config->disabled_by_default);
    }

    /* Field 10: icon (string) */
    if (config->icon[0] != '\0') {
        esphome_encode_string(&buf, 10, config->icon);
    }

    /* Field 13: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 13, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_FAN, payload, buf.position,
                                 output, output_size, output_len);
}

/**
 * @brief Encode fan state message
 */
esp_err_t esphome_encode_fan_state(const esphome_fan_state_t *state,
                                    uint8_t *output, size_t output_size,
                                    size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_OUTPUT_BUFFER_STANDARD];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* FanStateResponse proto fields:
     *   1: key (fixed32), 2: state (bool), 3: oscillating (bool),
     *   4: speed (legacy FanSpeed, skip), 5: direction (FanDirection),
     *   6: speed_level (int32), 7: preset_mode (string),
     *   8: device_id (uint32) */

    /* Field 1: key (fixed32) */
    esphome_encode_fixed32(&buf, 1, state->key);

    /* Field 2: state (bool) */
    esphome_encode_bool(&buf, 2, state->state);

    /* Field 3: oscillating (bool) */
    if (state->oscillating) {
        esphome_encode_bool(&buf, 3, state->oscillating);
    }

    /* Field 4: speed (legacy) - skipped (deprecated) */

    /* Field 5: direction (enum) */
    if (state->direction != ESPHOME_FAN_DIRECTION_FORWARD) {
        esphome_encode_uint32(&buf, 5, (uint32_t)state->direction);
    }

    /* Field 6: speed_level (int32) */
    if (state->speed_level > 0) {
        esphome_encode_int32(&buf, 6, state->speed_level);
    }

    /* Field 8: device_id (uint32) - required for sub-device entity availability */
    if (state->device_id != 0) {
        esphome_encode_uint32(&buf, 8, state->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_FAN_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Climate
 * ============================================================================ */

/**
 * @brief Encode climate entity info for ListEntities response
 */
esp_err_t esphome_encode_climate_list_entry(const esphome_climate_config_t *config,
                                             uint8_t *output, size_t output_size,
                                             size_t *output_len)
{
    if (!config || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_LARGE];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: object_id (string) */
    esphome_encode_string(&buf, 1, config->unique_id);

    /* Field 2: key (fixed32) */
    esphome_encode_fixed32(&buf, 2, config->key);

    /* Field 3: name (string) */
    esphome_encode_string(&buf, 3, config->name);

    /* Field 4: unique_id (string) */
    esphome_encode_string(&buf, 4, config->unique_id);

    /* Field 5: supports_current_temperature (bool) */
    if (config->supports_current_temperature) {
        esphome_encode_bool(&buf, 5, config->supports_current_temperature);
    }

    /* Field 6: supports_two_point_target_temperature (bool) */
    if (config->supports_two_point_target_temperature) {
        esphome_encode_bool(&buf, 6, config->supports_two_point_target_temperature);
    }

    /* Field 7: supported_modes[] (repeated enum from bitmask) */
    for (int mode = ESPHOME_CLIMATE_MODE_OFF; mode <= ESPHOME_CLIMATE_MODE_AUTO; mode++) {
        if (config->supported_modes & (1 << mode)) {
            esphome_encode_uint32(&buf, 7, (uint32_t)mode);
        }
    }

    /* Field 8: visual_min_temperature (float) */
    esphome_encode_float(&buf, 8, config->visual_min_temperature);

    /* Field 9: visual_max_temperature (float) */
    esphome_encode_float(&buf, 9, config->visual_max_temperature);

    /* Field 10: visual_temperature_step (float) */
    esphome_encode_float(&buf, 10, config->visual_temperature_step);

    /* Field 11: legacy_supports_away (bool) - deprecated, skip */

    /* Field 12: supports_action (bool) */
    if (config->supports_action) {
        esphome_encode_bool(&buf, 12, config->supports_action);
    }

    /* Field 13: supported_fan_modes[] (repeated enum from bitmask) */
    for (int mode = ESPHOME_CLIMATE_FAN_ON; mode <= ESPHOME_CLIMATE_FAN_QUIET; mode++) {
        if (config->supported_fan_modes & (1 << mode)) {
            esphome_encode_uint32(&buf, 13, (uint32_t)mode);
        }
    }

    /* Field 14: supported_swing_modes[] (repeated enum from bitmask) */
    for (int mode = ESPHOME_CLIMATE_SWING_OFF; mode <= ESPHOME_CLIMATE_SWING_HORIZONTAL; mode++) {
        if (config->supported_swing_modes & (1 << mode)) {
            esphome_encode_uint32(&buf, 14, (uint32_t)mode);
        }
    }

    /* Field 16: supported_presets[] (repeated enum from bitmask) */
    for (int preset = ESPHOME_CLIMATE_PRESET_NONE; preset <= ESPHOME_CLIMATE_PRESET_ACTIVITY; preset++) {
        if (config->supported_presets & (1 << preset)) {
            esphome_encode_uint32(&buf, 16, (uint32_t)preset);
        }
    }

    /* Field 18: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 18, config->disabled_by_default);
    }

    /* Field 19: icon (string) */
    if (config->icon[0] != '\0') {
        esphome_encode_string(&buf, 19, config->icon);
    }

    /* Field 26: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 26, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_CLIMATE, payload, buf.position,
                                 output, output_size, output_len);
}

/**
 * @brief Encode climate state message
 */
esp_err_t esphome_encode_climate_state(const esphome_climate_state_t *state,
                                        uint8_t *output, size_t output_size,
                                        size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_STRING_BUFFER_MEDIUM];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* ClimateStateResponse proto fields:
     *   1: key (fixed32), 2: mode (ClimateMode), 3: current_temperature (float),
     *   4: target_temperature (float), 5: target_temperature_low (float),
     *   6: target_temperature_high (float), 7: unused_legacy_away (skip),
     *   8: action (ClimateAction), 9: fan_mode (ClimateFanMode),
     *   10: swing_mode (ClimateSwingMode), 11: custom_fan_mode (string),
     *   12: preset (ClimatePreset), 13: custom_preset (string),
     *   16: device_id (uint32) */

    /* Field 1: key (fixed32) */
    esphome_encode_fixed32(&buf, 1, state->key);

    /* Field 2: mode (enum) */
    esphome_encode_uint32(&buf, 2, (uint32_t)state->mode);

    /* Field 3: current_temperature (float) */
    esphome_encode_float(&buf, 3, state->current_temperature);

    /* Field 4: target_temperature (float) */
    esphome_encode_float(&buf, 4, state->target_temperature);

    /* Field 5: target_temperature_low (float) */
    if (state->target_temperature_low > 0.0f) {
        esphome_encode_float(&buf, 5, state->target_temperature_low);
    }

    /* Field 6: target_temperature_high (float) */
    if (state->target_temperature_high > 0.0f) {
        esphome_encode_float(&buf, 6, state->target_temperature_high);
    }

    /* Field 7: unused_legacy_away - skipped (deprecated) */

    /* Field 8: action (enum) */
    if (state->action != ESPHOME_CLIMATE_ACTION_OFF) {
        esphome_encode_uint32(&buf, 8, (uint32_t)state->action);
    }

    /* Field 9: fan_mode (enum) */
    if (state->fan_mode != ESPHOME_CLIMATE_FAN_AUTO) {
        esphome_encode_uint32(&buf, 9, (uint32_t)state->fan_mode);
    }

    /* Field 10: swing_mode (enum) */
    if (state->swing_mode != ESPHOME_CLIMATE_SWING_OFF) {
        esphome_encode_uint32(&buf, 10, (uint32_t)state->swing_mode);
    }

    /* Field 12: preset (enum) */
    if (state->preset != ESPHOME_CLIMATE_PRESET_NONE) {
        esphome_encode_uint32(&buf, 12, (uint32_t)state->preset);
    }

    /* Field 16: device_id (uint32) - required for sub-device entity availability */
    if (state->device_id != 0) {
        esphome_encode_uint32(&buf, 16, state->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_CLIMATE_STATE, payload, buf.position,
                                 output, output_size, output_len);
}
