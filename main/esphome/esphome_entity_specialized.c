/**
 * @file esphome_entity_specialized.c
 * @brief ESPHome Specialized Entities - Lock, Media Player, Alarm, Button, Text
 *
 * Registration, state management, and encoding for specialized entities.
 *
 * Integration with unified device_registry:
 *   - Entity registration creates a virtual device in device_registry
 *   - Capabilities are set based on entity type (LOCK, etc.)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_entity_internal.h"
#include "esphome_protocol.h"
#include "esphome_device_registry.h"

/* ============================================================================
 * Button Registration and State
 * ============================================================================ */

/**
 * @brief Register a button entity
 */
esp_err_t esphome_entity_register_button(const esphome_button_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Check for duplicate key */
    if (find_button(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Button with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities.button_count >= ESPHOME_MAX_BUTTONS) {
        ESP_LOGE(ENTITY_TAG, "Maximum buttons reached (%d)", ESPHOME_MAX_BUTTONS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add button */
    size_t idx = s_entities.button_count;
    memcpy(&s_entities.buttons[idx].config, config, sizeof(esphome_button_config_t));
    s_entities.buttons[idx].registered = true;
    s_entities.button_count++;

    ESP_LOGI(ENTITY_TAG, "Registered button: key=%lu, name='%s'", config->key, config->name);

    /* Register with unified device_registry (best-effort, non-blocking) */
    if (esphome_device_registry_is_initialized()) {
        esp_err_t reg_ret = esphome_device_registry_register(
            ESPHOME_ENTITY_BUTTON, config->key, config->name, config->unique_id);
        if (reg_ret != ESP_OK) {
            ESP_LOGD(ENTITY_TAG, "Device registry registration skipped: %s",
                     esp_err_to_name(reg_ret));
        }
    }

done:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get button configuration
 */
esp_err_t esphome_entity_get_button_config(esphome_entity_key_t key,
                                            esphome_button_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    button_entry_t *entry = find_button(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_button_config_t));

done:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get number of registered buttons
 */
size_t esphome_entity_get_button_count(void)
{
    return s_entities.button_count;
}

/**
 * @brief Execute button press command
 */
esp_err_t esphome_entity_execute_button_press(esphome_entity_key_t key)
{
    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    button_entry_t *entry = find_button(key);
    esphome_button_press_cb_t callback = NULL;

    if (entry && entry->config.press_callback) {
        callback = entry->config.press_callback;
    }

    xSemaphoreGiveRecursive(s_entities.mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing button press: key=%lu", key);
        return callback(key);
    }

    /* No callback - just log the press */
    ESP_LOGI(ENTITY_TAG, "Button pressed: key=%lu (no handler)", key);
    return ESP_OK;
}

/**
 * @brief Execute button command (API handler entry point)
 *
 * Delegates to esphome_entity_execute_button_press. This is the function
 * called by the API handler when a ButtonCommand message is received.
 *
 * @param[in] key Entity key of the button
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_button_command(esphome_entity_key_t key)
{
    return esphome_entity_execute_button_press(key);
}

/* ============================================================================
 * Lock Registration and State
 * ============================================================================ */

/**
 * @brief Register a lock entity
 */
esp_err_t esphome_entity_register_lock(const esphome_lock_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (find_lock(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Lock with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    if (s_entities.lock_count >= ESPHOME_MAX_LOCKS) {
        ESP_LOGE(ENTITY_TAG, "Maximum locks reached (%d)", ESPHOME_MAX_LOCKS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto cleanup;
    }

    size_t idx = s_entities.lock_count;
    memcpy(&s_entities.locks[idx].config, config, sizeof(esphome_lock_config_t));
    s_entities.locks[idx].state.key = config->key;
    s_entities.locks[idx].state.device_id = config->device_id;
    s_entities.locks[idx].state.state = ESPHOME_LOCK_STATE_LOCKED;
    s_entities.locks[idx].registered = true;
    s_entities.lock_count++;

    ESP_LOGI(ENTITY_TAG, "Registered lock: key=%lu, name='%s'", config->key, config->name);

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Update lock state
 */
esp_err_t esphome_entity_update_lock(esphome_entity_key_t key, esphome_lock_state_t state)
{
    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    lock_entry_t *entry = find_lock(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    entry->state.state = state;
    ESP_LOGD(ENTITY_TAG, "Lock %lu state: %d", key, state);

    xSemaphoreGiveRecursive(s_entities.mutex);
    notify_state_change(ESPHOME_ENTITY_LOCK, key, &entry->state);
    return ESP_OK;

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get lock state
 */
esp_err_t esphome_entity_get_lock(esphome_entity_key_t key, esphome_lock_entity_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    lock_entry_t *entry = find_lock(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    memcpy(state, &entry->state, sizeof(esphome_lock_entity_state_t));

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get lock configuration
 */
esp_err_t esphome_entity_get_lock_config(esphome_entity_key_t key, esphome_lock_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    lock_entry_t *entry = find_lock(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    memcpy(config, &entry->config, sizeof(esphome_lock_config_t));

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get number of registered locks
 */
size_t esphome_entity_get_lock_count(void)
{
    return s_entities.lock_count;
}

/**
 * @brief Execute lock command
 */
esp_err_t esphome_entity_execute_lock_command(esphome_entity_key_t key,
                                               esphome_lock_command_t command)
{
    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    lock_entry_t *entry = find_lock(key);
    esphome_lock_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities.mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing lock command: key=%lu, cmd=%d", key, command);
        return callback(key, command);
    }

    /* No callback - apply command directly */
    esphome_lock_state_t new_state;
    switch (command) {
        case ESPHOME_LOCK_COMMAND_LOCK:
            new_state = ESPHOME_LOCK_STATE_LOCKED;
            break;
        case ESPHOME_LOCK_COMMAND_UNLOCK:
        case ESPHOME_LOCK_COMMAND_OPEN:
            new_state = ESPHOME_LOCK_STATE_UNLOCKED;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return esphome_entity_update_lock(key, new_state);
}

/* ============================================================================
 * Media Player Registration and State
 * ============================================================================ */

/**
 * @brief Register a media player entity
 */
esp_err_t esphome_entity_register_media_player(const esphome_media_player_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (find_media_player(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Media player with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    if (s_entities.media_player_count >= ESPHOME_MAX_MEDIA_PLAYERS) {
        ESP_LOGE(ENTITY_TAG, "Maximum media players reached (%d)", ESPHOME_MAX_MEDIA_PLAYERS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto cleanup;
    }

    size_t idx = s_entities.media_player_count;
    memcpy(&s_entities.media_players[idx].config, config, sizeof(esphome_media_player_config_t));
    s_entities.media_players[idx].state.key = config->key;
    s_entities.media_players[idx].state.device_id = config->device_id;
    s_entities.media_players[idx].state.state = ESPHOME_MEDIA_PLAYER_STATE_IDLE;
    s_entities.media_players[idx].state.volume = 1.0f;
    s_entities.media_players[idx].state.muted = false;
    s_entities.media_players[idx].registered = true;
    s_entities.media_player_count++;

    ESP_LOGI(ENTITY_TAG, "Registered media player: key=%lu, name='%s'", config->key, config->name);

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Update media player state
 */
esp_err_t esphome_entity_update_media_player(esphome_entity_key_t key,
                                              const esphome_media_player_entity_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    media_player_entry_t *entry = find_media_player(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    memcpy(&entry->state, state, sizeof(esphome_media_player_entity_state_t));
    entry->state.key = key;
    ESP_LOGD(ENTITY_TAG, "Media player %lu state: %d, vol=%.2f", key, state->state, state->volume);

    xSemaphoreGiveRecursive(s_entities.mutex);
    notify_state_change(ESPHOME_ENTITY_MEDIA_PLAYER, key, &entry->state);
    return ESP_OK;

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get media player state
 */
esp_err_t esphome_entity_get_media_player(esphome_entity_key_t key,
                                           esphome_media_player_entity_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    media_player_entry_t *entry = find_media_player(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    memcpy(state, &entry->state, sizeof(esphome_media_player_entity_state_t));

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get media player configuration
 */
esp_err_t esphome_entity_get_media_player_config(esphome_entity_key_t key,
                                                  esphome_media_player_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    media_player_entry_t *entry = find_media_player(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    memcpy(config, &entry->config, sizeof(esphome_media_player_config_t));

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get number of registered media players
 */
size_t esphome_entity_get_media_player_count(void)
{
    return s_entities.media_player_count;
}

/**
 * @brief Execute media player command
 */
esp_err_t esphome_entity_execute_media_player_command(esphome_entity_key_t key,
                                                       const esphome_media_player_cmd_t *command)
{
    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    media_player_entry_t *entry = find_media_player(key);
    esphome_media_player_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities.mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing media player command: key=%lu", key);
        return callback(key, command);
    }

    /* No callback - apply command directly */
    esphome_media_player_entity_state_t new_state;
    esphome_entity_get_media_player(key, &new_state);

    if (command->has_command) {
        switch (command->command) {
            case ESPHOME_MEDIA_PLAYER_CMD_PLAY:
                new_state.state = ESPHOME_MEDIA_PLAYER_STATE_PLAYING;
                break;
            case ESPHOME_MEDIA_PLAYER_CMD_PAUSE:
                new_state.state = ESPHOME_MEDIA_PLAYER_STATE_PAUSED;
                break;
            case ESPHOME_MEDIA_PLAYER_CMD_STOP:
                new_state.state = ESPHOME_MEDIA_PLAYER_STATE_IDLE;
                break;
            case ESPHOME_MEDIA_PLAYER_CMD_MUTE:
                new_state.muted = true;
                break;
            case ESPHOME_MEDIA_PLAYER_CMD_UNMUTE:
                new_state.muted = false;
                break;
        }
    }
    if (command->has_volume) {
        new_state.volume = command->volume;
    }

    return esphome_entity_update_media_player(key, &new_state);
}

/* ============================================================================
 * Alarm Control Panel Registration and State
 * ============================================================================ */

/**
 * @brief Register an alarm control panel entity
 */
esp_err_t esphome_entity_register_alarm(const esphome_alarm_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (find_alarm(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Alarm panel with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    if (s_entities.alarm_count >= ESPHOME_MAX_ALARM_PANELS) {
        ESP_LOGE(ENTITY_TAG, "Maximum alarm panels reached (%d)", ESPHOME_MAX_ALARM_PANELS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto cleanup;
    }

    size_t idx = s_entities.alarm_count;
    memcpy(&s_entities.alarms[idx].config, config, sizeof(esphome_alarm_config_t));
    s_entities.alarms[idx].state.key = config->key;
    s_entities.alarms[idx].state.device_id = config->device_id;
    s_entities.alarms[idx].state.state = ESPHOME_ALARM_STATE_DISARMED;
    s_entities.alarms[idx].registered = true;
    s_entities.alarm_count++;

    ESP_LOGI(ENTITY_TAG, "Registered alarm panel: key=%lu, name='%s'", config->key, config->name);

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Update alarm control panel state
 */
esp_err_t esphome_entity_update_alarm(esphome_entity_key_t key, esphome_alarm_state_t state)
{
    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    alarm_entry_t *entry = find_alarm(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    entry->state.state = state;
    ESP_LOGD(ENTITY_TAG, "Alarm %lu state: %d", key, state);

    xSemaphoreGiveRecursive(s_entities.mutex);
    notify_state_change(ESPHOME_ENTITY_ALARM_PANEL, key, &entry->state);
    return ESP_OK;

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get alarm control panel state
 */
esp_err_t esphome_entity_get_alarm(esphome_entity_key_t key, esphome_alarm_entity_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    alarm_entry_t *entry = find_alarm(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    memcpy(state, &entry->state, sizeof(esphome_alarm_entity_state_t));

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get alarm control panel configuration
 */
esp_err_t esphome_entity_get_alarm_config(esphome_entity_key_t key, esphome_alarm_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    alarm_entry_t *entry = find_alarm(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    memcpy(config, &entry->config, sizeof(esphome_alarm_config_t));

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get number of registered alarm control panels
 */
size_t esphome_entity_get_alarm_count(void)
{
    return s_entities.alarm_count;
}

/**
 * @brief Execute alarm control panel command
 */
esp_err_t esphome_entity_execute_alarm_command(esphome_entity_key_t key,
                                                esphome_alarm_command_t command,
                                                const char *code)
{
    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    alarm_entry_t *entry = find_alarm(key);
    esphome_alarm_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities.mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing alarm command: key=%lu, cmd=%d", key, command);
        return callback(key, command, code);
    }

    /* No callback - apply command directly */
    esphome_alarm_state_t new_state;
    switch (command) {
        case ESPHOME_ALARM_CMD_DISARM:
            new_state = ESPHOME_ALARM_STATE_DISARMED;
            break;
        case ESPHOME_ALARM_CMD_ARM_HOME:
            new_state = ESPHOME_ALARM_STATE_ARMED_HOME;
            break;
        case ESPHOME_ALARM_CMD_ARM_AWAY:
            new_state = ESPHOME_ALARM_STATE_ARMED_AWAY;
            break;
        case ESPHOME_ALARM_CMD_ARM_NIGHT:
            new_state = ESPHOME_ALARM_STATE_ARMED_NIGHT;
            break;
        case ESPHOME_ALARM_CMD_ARM_VACATION:
            new_state = ESPHOME_ALARM_STATE_ARMED_VACATION;
            break;
        case ESPHOME_ALARM_CMD_ARM_CUSTOM_BYPASS:
            new_state = ESPHOME_ALARM_STATE_ARMED_CUSTOM_BYPASS;
            break;
        case ESPHOME_ALARM_CMD_TRIGGER:
            new_state = ESPHOME_ALARM_STATE_TRIGGERED;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return esphome_entity_update_alarm(key, new_state);
}

/* ============================================================================
 * Text Entity Registration and State
 * ============================================================================ */

/**
 * @brief Register a text entity
 */
esp_err_t esphome_entity_register_text(const esphome_text_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        ESP_LOGE(ENTITY_TAG, "Entity manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (find_text(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Text entity with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    if (s_entities.text_count >= ESPHOME_MAX_TEXTS) {
        ESP_LOGE(ENTITY_TAG, "Maximum text entities reached (%d)", ESPHOME_MAX_TEXTS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto cleanup;
    }

    size_t idx = s_entities.text_count;
    memcpy(&s_entities.texts[idx].config, config, sizeof(esphome_text_config_t));
    s_entities.texts[idx].state.key = config->key;
    s_entities.texts[idx].state.device_id = config->device_id;
    s_entities.texts[idx].state.state[0] = '\0';
    s_entities.texts[idx].state.missing_state = true;
    s_entities.texts[idx].registered = true;
    s_entities.text_count++;

    ESP_LOGI(ENTITY_TAG, "Registered text entity: key=%lu, name='%s'", config->key, config->name);

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Update text entity state
 */
esp_err_t esphome_entity_update_text(esphome_entity_key_t key, const char *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    text_entry_t *entry = find_text(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    strncpy(entry->state.state, value, sizeof(entry->state.state) - 1);
    entry->state.state[sizeof(entry->state.state) - 1] = '\0';
    entry->state.missing_state = false;

    ESP_LOGD(ENTITY_TAG, "Text %lu state: '%s'", key, entry->state.state);

    xSemaphoreGiveRecursive(s_entities.mutex);
    notify_state_change(ESPHOME_ENTITY_TEXT, key, &entry->state);
    return ESP_OK;

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Set text entity to missing/unavailable state
 */
esp_err_t esphome_entity_set_text_missing(esphome_entity_key_t key)
{
    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    text_entry_t *entry = find_text(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    entry->state.missing_state = true;

    xSemaphoreGiveRecursive(s_entities.mutex);
    notify_state_change(ESPHOME_ENTITY_TEXT, key, &entry->state);
    return ESP_OK;

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get text entity state
 */
esp_err_t esphome_entity_get_text(esphome_entity_key_t key, esphome_text_entity_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    text_entry_t *entry = find_text(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    memcpy(state, &entry->state, sizeof(esphome_text_entity_state_t));

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get text entity configuration
 */
esp_err_t esphome_entity_get_text_config(esphome_entity_key_t key, esphome_text_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    text_entry_t *entry = find_text(key);
    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto cleanup;
    }

    memcpy(config, &entry->config, sizeof(esphome_text_config_t));

cleanup:
    xSemaphoreGiveRecursive(s_entities.mutex);
    return ret;
}

/**
 * @brief Get number of registered text entities
 */
size_t esphome_entity_get_text_count(void)
{
    return s_entities.text_count;
}

/**
 * @brief Execute text entity command
 */
esp_err_t esphome_entity_execute_text_command(esphome_entity_key_t key, const char *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities.mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    text_entry_t *entry = find_text(key);
    esphome_text_command_cb_t callback = NULL;

    if (entry && entry->config.command_callback) {
        callback = entry->config.command_callback;
    }

    xSemaphoreGiveRecursive(s_entities.mutex);

    if (!entry) {
        return ESPHOME_ERR_ENTITY_NOT_FOUND;
    }

    if (callback) {
        ESP_LOGI(ENTITY_TAG, "Executing text command: key=%lu", key);
        return callback(key, value);
    }

    /* No callback - apply value directly */
    return esphome_entity_update_text(key, value);
}

/* ============================================================================
 * Message Encoding - Button
 * ============================================================================ */

/**
 * @brief Encode button entity info for ListEntities response
 */
esp_err_t esphome_encode_button_list_entry(const esphome_button_config_t *config,
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

    /* Field 6: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 6, config->disabled_by_default);
    }

    /* Field 7: entity_category (0=NONE, 1=CONFIG, 2=DIAGNOSTIC) */
    if (config->entity_category != 0) {
        esphome_encode_uint32(&buf, 7, (uint32_t)config->entity_category);
    }

    /* Field 8: device_class (string) */
    if (config->device_class != ESPHOME_BUTTON_CLASS_NONE) {
        const char *dc_str = "";
        switch (config->device_class) {
            case ESPHOME_BUTTON_CLASS_RESTART: dc_str = "restart"; break;
            case ESPHOME_BUTTON_CLASS_UPDATE:  dc_str = "update"; break;
            case ESPHOME_BUTTON_CLASS_IDENTIFY: dc_str = "identify"; break;
            default: break;
        }
        if (dc_str[0] != '\0') {
            esphome_encode_string(&buf, 8, dc_str);
        }
    }

    /* Field 9: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 9, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_BUTTON, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Lock
 * ============================================================================ */

esp_err_t esphome_encode_lock_list_entry(const esphome_lock_config_t *config,
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

    /* Field 6: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 6, config->disabled_by_default);
    }

    /* Field 8: assumed_state (bool) */
    if (config->assumed_state) {
        esphome_encode_bool(&buf, 8, config->assumed_state);
    }

    /* Field 9: supports_open (bool) */
    if (config->supports_open) {
        esphome_encode_bool(&buf, 9, config->supports_open);
    }

    /* Field 10: requires_code (bool) */
    if (config->requires_code) {
        esphome_encode_bool(&buf, 10, config->requires_code);
    }

    /* Field 12: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 12, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_LOCK, payload, buf.position,
                                 output, output_size, output_len);
}

esp_err_t esphome_encode_lock_state(const esphome_lock_entity_state_t *state,
                                     uint8_t *output, size_t output_size,
                                     size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_PAYLOAD_MEDIUM];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: key (fixed32) */
    esphome_encode_fixed32(&buf, 1, state->key);

    /* Field 2: state (enum) */
    esphome_encode_uint32(&buf, 2, (uint32_t)state->state);

    /* Field 3: device_id (uint32) - required for sub-device entity availability */
    if (state->device_id != 0) {
        esphome_encode_uint32(&buf, 3, state->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LOCK_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Text Entity
 * ============================================================================ */

esp_err_t esphome_encode_text_list_entry(const esphome_text_config_t *config,
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

    /* Field 6: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 6, config->disabled_by_default);
    }

    /* Field 8: min_length (uint32) */
    esphome_encode_uint32(&buf, 8, config->min_length);

    /* Field 9: max_length (uint32) */
    esphome_encode_uint32(&buf, 9, config->max_length);

    /* Field 10: pattern (string) */
    if (config->pattern[0] != '\0') {
        esphome_encode_string(&buf, 10, config->pattern);
    }

    /* Field 11: mode (enum) */
    esphome_encode_uint32(&buf, 11, (uint32_t)config->mode);

    /* Field 12: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 12, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_TEXT, payload, buf.position,
                                 output, output_size, output_len);
}

esp_err_t esphome_encode_text_state(const esphome_text_entity_state_t *state,
                                     uint8_t *output, size_t output_size,
                                     size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_MEDIUM];
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

    return esphome_build_message(ESPHOME_MSG_TEXT_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Media Player
 * ============================================================================ */

esp_err_t esphome_encode_media_player_list_entry(const esphome_media_player_config_t *config,
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

    /* Field 6: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 6, config->disabled_by_default);
    }

    /* Field 8: supports_pause (bool) */
    if (config->supports_pause) {
        esphome_encode_bool(&buf, 8, config->supports_pause);
    }

    /* Field 10: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 10, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_MEDIA_PLAYER, payload, buf.position,
                                 output, output_size, output_len);
}

esp_err_t esphome_encode_media_player_state(const esphome_media_player_entity_state_t *state,
                                             uint8_t *output, size_t output_size,
                                             size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_PAYLOAD_MEDIUM];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: key (fixed32) */
    esphome_encode_fixed32(&buf, 1, state->key);

    /* Field 2: state (enum) */
    esphome_encode_uint32(&buf, 2, (uint32_t)state->state);

    /* Field 3: volume (float) */
    esphome_encode_float(&buf, 3, state->volume);

    /* Field 4: muted (bool) */
    if (state->muted) {
        esphome_encode_bool(&buf, 4, state->muted);
    }

    /* Field 5: device_id (uint32) - required for sub-device entity availability */
    if (state->device_id != 0) {
        esphome_encode_uint32(&buf, 5, state->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_MEDIA_PLAYER_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Alarm Control Panel
 * ============================================================================ */

esp_err_t esphome_encode_alarm_list_entry(const esphome_alarm_config_t *config,
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

    /* Field 6: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 6, config->disabled_by_default);
    }

    /* Field 8: supported_features (bitmask) */
    esphome_encode_uint32(&buf, 8, config->supported_features);

    /* Field 9: requires_code (bool) */
    if (config->requires_code) {
        esphome_encode_bool(&buf, 9, config->requires_code);
    }

    /* Field 10: requires_code_to_arm (bool) */
    if (config->requires_code_to_arm) {
        esphome_encode_bool(&buf, 10, config->requires_code_to_arm);
    }

    /* Field 11: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 11, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_ALARM_PANEL, payload, buf.position,
                                 output, output_size, output_len);
}

esp_err_t esphome_encode_alarm_state(const esphome_alarm_entity_state_t *state,
                                      uint8_t *output, size_t output_size,
                                      size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_PAYLOAD_MEDIUM];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: key (fixed32) */
    esphome_encode_fixed32(&buf, 1, state->key);

    /* Field 2: state (enum) */
    esphome_encode_uint32(&buf, 2, (uint32_t)state->state);

    /* Field 3: device_id (uint32) - required for sub-device entity availability */
    if (state->device_id != 0) {
        esphome_encode_uint32(&buf, 3, state->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_ALARM_PANEL_STATE, payload, buf.position,
                                 output, output_size, output_len);
}
