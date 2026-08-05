/**
 * @file esphome_entity_sensors.c
 * @brief ESPHome Sensor Entities - Sensor, Binary Sensor, Text Sensor
 *
 * Registration, state management, and encoding for sensor-type entities.
 *
 * Integration with the ESPHome entity mirror:
 *   - Entity registration creates a virtual device in device_registry
 *   - Capabilities are set based on entity type
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_entity_internal.h"
#include "esphome_protocol.h"
#include "esphome_entity_mirror.h"

/* ============================================================================
 * Sensor Registration and State
 * ============================================================================ */

/**
 * @brief Register a sensor entity
 */
esp_err_t esphome_entity_register_sensor(const esphome_sensor_config_t *config)
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
    if (find_sensor(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Sensor with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities->sensor_count >= ESPHOME_MAX_SENSORS) {
        ESP_LOGE(ENTITY_TAG, "Maximum sensors reached (%d)", ESPHOME_MAX_SENSORS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add sensor */
    size_t idx = s_entities->sensor_count;
    memcpy(&s_entities->sensors[idx].config, config, sizeof(esphome_sensor_config_t));
    s_entities->sensors[idx].state.key = config->key;
    s_entities->sensors[idx].state.state = 0.0f;
    s_entities->sensors[idx].state.missing_state = true;
    s_entities->sensors[idx].state.device_id = config->device_id;
    s_entities->sensors[idx].registered = true;
    s_entities->sensor_count++;

    ESP_LOGI(ENTITY_TAG, "Registered sensor: key=%lu, name='%s'", config->key, config->name);

    /* Mirror into the entity state store (best-effort, non-blocking) */
    if (esphome_entity_mirror_is_initialized()) {
        esp_err_t reg_ret = esphome_entity_mirror_register(
            ESPHOME_ENTITY_SENSOR, config->key, config->name, config->unique_id);
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
 * @brief Update sensor state
 */
esp_err_t esphome_entity_update_sensor(esphome_entity_key_t key, float value)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    sensor_entry_t *entry = find_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.state = value;
    entry->state.missing_state = false;

    ESP_LOGD(ENTITY_TAG, "Sensor %lu state: %.2f", key, value);

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_SENSOR, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Set sensor to missing/unavailable state
 */
esp_err_t esphome_entity_set_sensor_missing(esphome_entity_key_t key)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    sensor_entry_t *entry = find_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.missing_state = true;

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_SENSOR, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get sensor state
 */
esp_err_t esphome_entity_get_sensor(esphome_entity_key_t key, esphome_sensor_state_t *state)
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
    sensor_entry_t *entry = find_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(state, &entry->state, sizeof(esphome_sensor_state_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get sensor configuration
 */
esp_err_t esphome_entity_get_sensor_config(esphome_entity_key_t key,
                                            esphome_sensor_config_t *config)
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
    sensor_entry_t *entry = find_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_sensor_config_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number of registered sensors
 */
size_t esphome_entity_get_sensor_count(void)
{
    return s_entities->sensor_count;
}

/* ============================================================================
 * Binary Sensor Registration and State
 * ============================================================================ */

/**
 * @brief Register a binary sensor entity
 */
esp_err_t esphome_entity_register_binary_sensor(const esphome_binary_sensor_config_t *config)
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
    if (find_binary_sensor(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Binary sensor with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities->binary_sensor_count >= ESPHOME_MAX_BINARY_SENSORS) {
        ESP_LOGE(ENTITY_TAG, "Maximum binary sensors reached (%d)", ESPHOME_MAX_BINARY_SENSORS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add binary sensor */
    size_t idx = s_entities->binary_sensor_count;
    memcpy(&s_entities->binary_sensors[idx].config, config,
           sizeof(esphome_binary_sensor_config_t));
    s_entities->binary_sensors[idx].state.key = config->key;
    s_entities->binary_sensors[idx].state.state = false;
    s_entities->binary_sensors[idx].state.missing_state = true;
    s_entities->binary_sensors[idx].state.device_id = config->device_id;
    s_entities->binary_sensors[idx].registered = true;
    s_entities->binary_sensor_count++;

    ESP_LOGI(ENTITY_TAG, "Registered binary sensor: key=%lu, name='%s'", config->key, config->name);

    /* Mirror into the entity state store (best-effort, non-blocking) */
    if (esphome_entity_mirror_is_initialized()) {
        esp_err_t reg_ret = esphome_entity_mirror_register(
            ESPHOME_ENTITY_BINARY_SENSOR, config->key, config->name, config->unique_id);
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
 * @brief Update binary sensor state
 */
esp_err_t esphome_entity_update_binary_sensor(esphome_entity_key_t key, bool state)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    binary_sensor_entry_t *entry = find_binary_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.state = state;
    entry->state.missing_state = false;

    ESP_LOGD(ENTITY_TAG, "Binary sensor %lu state: %s", key, state ? "ON" : "OFF");

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_BINARY_SENSOR, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Set binary sensor to missing/unavailable state
 */
esp_err_t esphome_entity_set_binary_sensor_missing(esphome_entity_key_t key)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    binary_sensor_entry_t *entry = find_binary_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.missing_state = true;

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_BINARY_SENSOR, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get binary sensor state
 */
esp_err_t esphome_entity_get_binary_sensor(esphome_entity_key_t key,
                                            esphome_binary_sensor_state_t *state)
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
    binary_sensor_entry_t *entry = find_binary_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(state, &entry->state, sizeof(esphome_binary_sensor_state_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get binary sensor configuration
 */
esp_err_t esphome_entity_get_binary_sensor_config(esphome_entity_key_t key,
                                                   esphome_binary_sensor_config_t *config)
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
    binary_sensor_entry_t *entry = find_binary_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_binary_sensor_config_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number of registered binary sensors
 */
size_t esphome_entity_get_binary_sensor_count(void)
{
    return s_entities->binary_sensor_count;
}

/* ============================================================================
 * Text Sensor Registration and State
 * ============================================================================ */

/**
 * @brief Register a text sensor entity
 */
esp_err_t esphome_entity_register_text_sensor(const esphome_text_sensor_config_t *config)
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
    if (find_text_sensor(config->key)) {
        ESP_LOGW(ENTITY_TAG, "Text sensor with key %lu already exists", config->key);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* Check capacity */
    if (s_entities->text_sensor_count >= ESPHOME_MAX_TEXT_SENSORS) {
        ESP_LOGE(ENTITY_TAG, "Maximum text sensors reached (%d)", ESPHOME_MAX_TEXT_SENSORS);
        ret = ESPHOME_ERR_MAX_ENTITIES;
        goto done;
    }

    /* Add text sensor */
    size_t idx = s_entities->text_sensor_count;
    memcpy(&s_entities->text_sensors[idx].config, config, sizeof(esphome_text_sensor_config_t));
    s_entities->text_sensors[idx].state.key = config->key;
    s_entities->text_sensors[idx].state.state[0] = '\0';
    s_entities->text_sensors[idx].state.missing_state = true;
    s_entities->text_sensors[idx].state.device_id = config->device_id;
    s_entities->text_sensors[idx].registered = true;
    s_entities->text_sensor_count++;

    ESP_LOGI(ENTITY_TAG, "Registered text sensor: key=%lu, name='%s'", config->key, config->name);

    /* Mirror into the entity state store (best-effort, non-blocking) */
    if (esphome_entity_mirror_is_initialized()) {
        esp_err_t reg_ret = esphome_entity_mirror_register(
            ESPHOME_ENTITY_TEXT_SENSOR, config->key, config->name, config->unique_id);
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
 * @brief Update text sensor state
 */
esp_err_t esphome_entity_update_text_sensor(esphome_entity_key_t key, const char *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    text_sensor_entry_t *entry = find_text_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    strncpy(entry->state.state, value, sizeof(entry->state.state) - 1);
    entry->state.state[sizeof(entry->state.state) - 1] = '\0';
    entry->state.missing_state = false;

    ESP_LOGD(ENTITY_TAG, "Text sensor %lu state: '%s'", key, value);

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_TEXT_SENSOR, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Set text sensor to missing/unavailable state
 */
esp_err_t esphome_entity_set_text_sensor_missing(esphome_entity_key_t key)
{
    if (!s_entities->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTakeRecursive(s_entities->mutex, GW_DEFAULT_MUTEX_TIMEOUT_1S_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    text_sensor_entry_t *entry = find_text_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    entry->state.missing_state = true;

    /* Notify callback */
    xSemaphoreGiveRecursive(s_entities->mutex);
    notify_state_change(ESPHOME_ENTITY_TEXT_SENSOR, key, &entry->state);
    return ESP_OK;

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get text sensor state
 */
esp_err_t esphome_entity_get_text_sensor(esphome_entity_key_t key,
                                          esphome_text_sensor_state_t *state)
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
    text_sensor_entry_t *entry = find_text_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(state, &entry->state, sizeof(esphome_text_sensor_state_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get text sensor configuration
 */
esp_err_t esphome_entity_get_text_sensor_config(esphome_entity_key_t key,
                                                 esphome_text_sensor_config_t *config)
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
    text_sensor_entry_t *entry = find_text_sensor(key);

    if (!entry) {
        ret = ESPHOME_ERR_ENTITY_NOT_FOUND;
        goto done;
    }

    memcpy(config, &entry->config, sizeof(esphome_text_sensor_config_t));

done:
    xSemaphoreGiveRecursive(s_entities->mutex);
    return ret;
}

/**
 * @brief Get number of registered text sensors
 */
size_t esphome_entity_get_text_sensor_count(void)
{
    return s_entities->text_sensor_count;
}

/* ============================================================================
 * Device Class String Conversion
 * ============================================================================ */

/**
 * @brief Convert sensor device class enum to HA-compatible string
 */
static const char *sensor_device_class_to_string(esphome_sensor_device_class_t dc)
{
    switch (dc) {
        case ESPHOME_SENSOR_CLASS_TEMPERATURE:      return "temperature";
        case ESPHOME_SENSOR_CLASS_HUMIDITY:          return "humidity";
        case ESPHOME_SENSOR_CLASS_PRESSURE:         return "pressure";
        case ESPHOME_SENSOR_CLASS_POWER:             return "power";
        case ESPHOME_SENSOR_CLASS_ENERGY:            return "energy";
        case ESPHOME_SENSOR_CLASS_VOLTAGE:           return "voltage";
        case ESPHOME_SENSOR_CLASS_CURRENT:           return "current";
        case ESPHOME_SENSOR_CLASS_BATTERY:           return "battery";
        case ESPHOME_SENSOR_CLASS_ILLUMINANCE:       return "illuminance";
        case ESPHOME_SENSOR_CLASS_SIGNAL_STRENGTH:   return "signal_strength";
        case ESPHOME_SENSOR_CLASS_TIMESTAMP:         return "timestamp";
        case ESPHOME_SENSOR_CLASS_CARBON_DIOXIDE:    return "carbon_dioxide";
        case ESPHOME_SENSOR_CLASS_VOLATILE_ORGANIC_COMPOUNDS: return "volatile_organic_compounds";
        case ESPHOME_SENSOR_CLASS_PM25:              return "pm25";
        case ESPHOME_SENSOR_CLASS_MOISTURE:          return "moisture";
        case ESPHOME_SENSOR_CLASS_DISTANCE:          return "distance";
        default:                                     return "";
    }
}

/* ============================================================================
 * Message Encoding - Sensor
 * ============================================================================ */

/**
 * @brief Encode sensor entity info for ListEntities response
 */
esp_err_t esphome_encode_sensor_list_entry(const esphome_sensor_config_t *config,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len)
{
    if (!config || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_BUFFER_MEDIUM];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: object_id (string) - use unique_id */
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

    /* Field 6: unit_of_measurement (string) */
    if (config->unit_of_measurement[0] != '\0') {
        esphome_encode_string(&buf, 6, config->unit_of_measurement);
    }

    /* Field 7: accuracy_decimals (int32) */
    esphome_encode_int32(&buf, 7, config->accuracy_decimals);

    /* Field 8: force_update (bool) */
    if (config->force_update) {
        esphome_encode_bool(&buf, 8, config->force_update);
    }

    /* Field 9: device_class (string) */
    {
        const char *dc_str = sensor_device_class_to_string(config->device_class);
        if (dc_str[0] != '\0') {
            esphome_encode_string(&buf, 9, dc_str);
        }
    }

    /* Field 10: state_class (enum) */
    if (config->state_class != ESPHOME_STATE_CLASS_NONE) {
        esphome_encode_uint32(&buf, 10, (uint32_t)config->state_class);
    }

    /* Field 12: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 12, config->disabled_by_default);
    }

    /* Field 13: entity_category (0=NONE, 1=CONFIG, 2=DIAGNOSTIC) */
    if (config->entity_category != 0) {
        esphome_encode_uint32(&buf, 13, (uint32_t)config->entity_category);
    }

    /* Field 14: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 14, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_SENSOR, payload, buf.position,
                                 output, output_size, output_len);
}

/**
 * @brief Encode sensor state message
 */
esp_err_t esphome_encode_sensor_state(const esphome_sensor_state_t *state,
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

    return esphome_build_message(ESPHOME_MSG_SENSOR_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Binary Sensor
 * ============================================================================ */

/**
 * @brief Convert binary sensor device class enum to HA-compatible string
 */
static const char *binary_sensor_device_class_to_string(esphome_binary_device_class_t dc)
{
    switch (dc) {
        case ESPHOME_BINARY_CLASS_BATTERY:       return "battery";
        case ESPHOME_BINARY_CLASS_COLD:          return "cold";
        case ESPHOME_BINARY_CLASS_CONNECTIVITY:  return "connectivity";
        case ESPHOME_BINARY_CLASS_DOOR:          return "door";
        case ESPHOME_BINARY_CLASS_GARAGE_DOOR:   return "garage_door";
        case ESPHOME_BINARY_CLASS_GAS:           return "gas";
        case ESPHOME_BINARY_CLASS_HEAT:          return "heat";
        case ESPHOME_BINARY_CLASS_LIGHT:         return "light";
        case ESPHOME_BINARY_CLASS_LOCK:          return "lock";
        case ESPHOME_BINARY_CLASS_MOISTURE:      return "moisture";
        case ESPHOME_BINARY_CLASS_MOTION:        return "motion";
        case ESPHOME_BINARY_CLASS_MOVING:        return "moving";
        case ESPHOME_BINARY_CLASS_OCCUPANCY:     return "occupancy";
        case ESPHOME_BINARY_CLASS_OPENING:       return "opening";
        case ESPHOME_BINARY_CLASS_PLUG:          return "plug";
        case ESPHOME_BINARY_CLASS_POWER:         return "power";
        case ESPHOME_BINARY_CLASS_PRESENCE:      return "presence";
        case ESPHOME_BINARY_CLASS_PROBLEM:       return "problem";
        case ESPHOME_BINARY_CLASS_SAFETY:        return "safety";
        case ESPHOME_BINARY_CLASS_SMOKE:         return "smoke";
        case ESPHOME_BINARY_CLASS_SOUND:         return "sound";
        case ESPHOME_BINARY_CLASS_VIBRATION:     return "vibration";
        case ESPHOME_BINARY_CLASS_WINDOW:        return "window";
        case ESPHOME_BINARY_CLASS_TAMPER:        return "tamper";
        case ESPHOME_BINARY_CLASS_BATTERY_LOW:   return "battery";
        default:                                 return "";
    }
}

/**
 * @brief Encode binary sensor entity info for ListEntities response
 *
 * Protobuf: ListEntitiesBinarySensorResponse
 *   1: object_id (string), 2: key (fixed32), 3: name (string),
 *   4: unique_id (string), 5: device_class (string),
 *   6: is_status_binary_sensor (bool), 7: disabled_by_default (bool),
 *   8: icon (string), 9: entity_category (enum), 10: device_id (uint32)
 */
esp_err_t esphome_encode_binary_sensor_list_entry(const esphome_binary_sensor_config_t *config,
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

    /* Field 5: device_class (string) */
    {
        const char *dc_str = binary_sensor_device_class_to_string(config->device_class);
        if (dc_str[0] != '\0') {
            esphome_encode_string(&buf, 5, dc_str);
        }
    }

    /* Field 6: is_status_binary_sensor (bool) */
    if (config->is_status_binary_sensor) {
        esphome_encode_bool(&buf, 6, config->is_status_binary_sensor);
    }

    /* Field 7: disabled_by_default (bool) */
    if (config->disabled_by_default) {
        esphome_encode_bool(&buf, 7, config->disabled_by_default);
    }

    /* Field 8: icon (string) */
    if (config->icon[0] != '\0') {
        esphome_encode_string(&buf, 8, config->icon);
    }

    /* Field 9: entity_category (0=NONE, 1=CONFIG, 2=DIAGNOSTIC) */
    if (config->entity_category != 0) {
        esphome_encode_uint32(&buf, 9, (uint32_t)config->entity_category);
    }

    /* Field 10: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 10, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_BINARY_SENSOR, payload, buf.position,
                                 output, output_size, output_len);
}

/**
 * @brief Encode binary sensor state message
 */
esp_err_t esphome_encode_binary_sensor_state(const esphome_binary_sensor_state_t *state,
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

    return esphome_build_message(ESPHOME_MSG_BINARY_SENSOR_STATE, payload, buf.position,
                                 output, output_size, output_len);
}

/* ============================================================================
 * Message Encoding - Text Sensor
 * ============================================================================ */

/**
 * @brief Encode text sensor entity info for ListEntities response
 */
esp_err_t esphome_encode_text_sensor_list_entry(const esphome_text_sensor_config_t *config,
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

    /* Field 9: device_id (sub-device grouping) */
    if (config->device_id != 0) {
        esphome_encode_uint32(&buf, 9, config->device_id);
    }

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    return esphome_build_message(ESPHOME_MSG_LIST_ENTITIES_TEXT_SENSOR, payload, buf.position,
                                 output, output_size, output_len);
}

/**
 * @brief Encode text sensor state message
 */
esp_err_t esphome_encode_text_sensor_state(const esphome_text_sensor_state_t *state,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len)
{
    if (!state || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[ESPHOME_MAX_STRING_LEN + 16];
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

    return esphome_build_message(ESPHOME_MSG_TEXT_SENSOR_STATE, payload, buf.position,
                                 output, output_size, output_len);
}
