/**
 * @file esphome_entities.h
 * @brief ESPHome Entity Management API
 *
 * Provides entity registration and state management for the ESPHome
 * Native API. Supports all 15 entity types: Sensor, Binary Sensor, Switch,
 * Text Sensor, Number, Button, Select, Light, Cover, Fan, Climate, Lock,
 * Media Player, Alarm Control Panel, and Text (Input Text).
 *
 * This header includes esphome_entities_types.h which contains all type
 * definitions (structs, enums, typedefs). For faster compilation when only
 * types are needed, include esphome_entities_types.h directly.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_ENTITIES_H
#define ESPHOME_ENTITIES_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esphome_common.h"
#include "esphome_protocol.h"
#include "esphome_entities_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Entity Manager Functions
 * ============================================================================ */

/**
 * @brief Initialize entity manager
 *
 * Must be called before registering any entities.
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_entities_init(void);

/**
 * @brief Deinitialize entity manager
 *
 * Frees all resources and clears registered entities.
 *
 * @return ESP_OK on success
 */
esp_err_t esphome_entities_deinit(void);

/**
 * @brief Set state change callback
 *
 * @param[in] callback Callback function
 * @return ESP_OK on success
 */
esp_err_t esphome_entities_set_state_callback(esphome_state_change_cb_t callback);

/* ============================================================================
 * Sensor Registration and State
 * ============================================================================ */

/**
 * @brief Register a sensor entity
 *
 * @param[in] config Sensor configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum sensors reached
 * @return ESP_ERR_INVALID_ARG if config is NULL
 */
esp_err_t esphome_entity_register_sensor(const esphome_sensor_config_t *config);

/**
 * @brief Update sensor state
 *
 * @param[in] key Entity key
 * @param[in] value New sensor value
 * @return ESP_OK on success
 * @return ESPHOME_ERR_ENTITY_NOT_FOUND if entity not found
 */
esp_err_t esphome_entity_update_sensor(esphome_entity_key_t key, float value);

/**
 * @brief Set sensor to missing/unavailable state
 *
 * @param[in] key Entity key
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_set_sensor_missing(esphome_entity_key_t key);

/**
 * @brief Get sensor state
 *
 * @param[in] key Entity key
 * @param[out] state Sensor state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_sensor(esphome_entity_key_t key, esphome_sensor_state_t *state);

/**
 * @brief Get sensor configuration
 *
 * @param[in] key Entity key
 * @param[out] config Sensor configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_sensor_config(esphome_entity_key_t key,
                                            esphome_sensor_config_t *config);

/**
 * @brief Get number of registered sensors
 *
 * @return Number of sensors
 */
size_t esphome_entity_get_sensor_count(void);

/* ============================================================================
 * Binary Sensor Registration and State
 * ============================================================================ */

/**
 * @brief Register a binary sensor entity
 *
 * @param[in] config Binary sensor configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum binary sensors reached
 */
esp_err_t esphome_entity_register_binary_sensor(const esphome_binary_sensor_config_t *config);

/**
 * @brief Update binary sensor state
 *
 * @param[in] key Entity key
 * @param[in] state New state (true=ON, false=OFF)
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_binary_sensor(esphome_entity_key_t key, bool state);

/**
 * @brief Set binary sensor to missing/unavailable state
 *
 * @param[in] key Entity key
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_set_binary_sensor_missing(esphome_entity_key_t key);

/**
 * @brief Get binary sensor state
 *
 * @param[in] key Entity key
 * @param[out] state Binary sensor state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_binary_sensor(esphome_entity_key_t key,
                                            esphome_binary_sensor_state_t *state);

/**
 * @brief Get binary sensor configuration
 *
 * @param[in] key Entity key
 * @param[out] config Binary sensor configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_binary_sensor_config(esphome_entity_key_t key,
                                                   esphome_binary_sensor_config_t *config);

/**
 * @brief Get number of registered binary sensors
 *
 * @return Number of binary sensors
 */
size_t esphome_entity_get_binary_sensor_count(void);

/* ============================================================================
 * Switch Registration and State
 * ============================================================================ */

/**
 * @brief Register a switch entity
 *
 * @param[in] config Switch configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum switches reached
 */
esp_err_t esphome_entity_register_switch(const esphome_switch_config_t *config);

/**
 * @brief Update switch state
 *
 * @param[in] key Entity key
 * @param[in] state New state (true=ON, false=OFF)
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_switch(esphome_entity_key_t key, bool state);

/**
 * @brief Get switch state
 *
 * @param[in] key Entity key
 * @param[out] state Switch state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_switch(esphome_entity_key_t key, esphome_switch_state_t *state);

/**
 * @brief Get switch configuration
 *
 * @param[in] key Entity key
 * @param[out] config Switch configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_switch_config(esphome_entity_key_t key,
                                            esphome_switch_config_t *config);

/**
 * @brief Get number of registered switches
 *
 * @return Number of switches
 */
size_t esphome_entity_get_switch_count(void);

/**
 * @brief Execute switch command
 *
 * Called when a switch command is received from a client.
 *
 * @param[in] key Entity key
 * @param[in] state Requested state
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_switch_command(esphome_entity_key_t key, bool state);

/* ============================================================================
 * Text Sensor Registration and State
 * ============================================================================ */

/**
 * @brief Register a text sensor entity
 *
 * @param[in] config Text sensor configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum text sensors reached
 */
esp_err_t esphome_entity_register_text_sensor(const esphome_text_sensor_config_t *config);

/**
 * @brief Update text sensor state
 *
 * @param[in] key Entity key
 * @param[in] state New string state
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_text_sensor(esphome_entity_key_t key, const char *state);

/**
 * @brief Set text sensor to missing/unavailable state
 *
 * @param[in] key Entity key
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_set_text_sensor_missing(esphome_entity_key_t key);

/**
 * @brief Get text sensor state
 *
 * @param[in] key Entity key
 * @param[out] state Text sensor state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_text_sensor(esphome_entity_key_t key,
                                          esphome_text_sensor_state_t *state);

/**
 * @brief Get text sensor configuration
 *
 * @param[in] key Entity key
 * @param[out] config Text sensor configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_text_sensor_config(esphome_entity_key_t key,
                                                 esphome_text_sensor_config_t *config);

/**
 * @brief Get number of registered text sensors
 *
 * @return Number of text sensors
 */
size_t esphome_entity_get_text_sensor_count(void);

/* ============================================================================
 * Number Registration and State
 * ============================================================================ */

/**
 * @brief Register a number entity
 *
 * @param[in] config Number configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum numbers reached
 */
esp_err_t esphome_entity_register_number(const esphome_number_config_t *config);

/**
 * @brief Update number state
 *
 * @param[in] key Entity key
 * @param[in] value New numeric value
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_number(esphome_entity_key_t key, float value);

/**
 * @brief Set number to missing/unavailable state
 *
 * @param[in] key Entity key
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_set_number_missing(esphome_entity_key_t key);

/**
 * @brief Get number state
 *
 * @param[in] key Entity key
 * @param[out] state Number state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_number(esphome_entity_key_t key, esphome_number_state_t *state);

/**
 * @brief Get number configuration
 *
 * @param[in] key Entity key
 * @param[out] config Number configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_number_config(esphome_entity_key_t key,
                                            esphome_number_config_t *config);

/**
 * @brief Get number of registered numbers
 *
 * @return Number of number entities
 */
size_t esphome_entity_get_number_count(void);

/**
 * @brief Execute number command
 *
 * Called when a number command is received from a client.
 *
 * @param[in] key Entity key
 * @param[in] value Requested value
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_number_command(esphome_entity_key_t key, float value);

/* ============================================================================
 * Button Registration
 * ============================================================================ */

/**
 * @brief Register a button entity
 *
 * @param[in] config Button configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum buttons reached
 */
esp_err_t esphome_entity_register_button(const esphome_button_config_t *config);

/**
 * @brief Get button configuration
 *
 * @param[in] key Entity key
 * @param[out] config Button configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_button_config(esphome_entity_key_t key,
                                            esphome_button_config_t *config);

/**
 * @brief Get number of registered buttons
 *
 * @return Number of buttons
 */
size_t esphome_entity_get_button_count(void);

/**
 * @brief Execute button press command
 *
 * Called when a button press command is received from a client.
 *
 * @param[in] key Entity key
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_button_command(esphome_entity_key_t key);

/* ============================================================================
 * Select Registration and State
 * ============================================================================ */

/**
 * @brief Register a select entity
 *
 * @param[in] config Select configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum selects reached
 */
esp_err_t esphome_entity_register_select(const esphome_select_config_t *config);

/**
 * @brief Update select state
 *
 * @param[in] key Entity key
 * @param[in] state Selected option string
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_select(esphome_entity_key_t key, const char *state);

/**
 * @brief Set select to missing/unavailable state
 *
 * @param[in] key Entity key
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_set_select_missing(esphome_entity_key_t key);

/**
 * @brief Get select state
 *
 * @param[in] key Entity key
 * @param[out] state Select state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_select(esphome_entity_key_t key, esphome_select_state_t *state);

/**
 * @brief Get select configuration
 *
 * @param[in] key Entity key
 * @param[out] config Select configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_select_config(esphome_entity_key_t key,
                                            esphome_select_config_t *config);

/**
 * @brief Get number of registered selects
 *
 * @return Number of select entities
 */
size_t esphome_entity_get_select_count(void);

/**
 * @brief Execute select command
 *
 * Called when a select command is received from a client.
 *
 * @param[in] key Entity key
 * @param[in] option Selected option
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_select_command(esphome_entity_key_t key, const char *option);

/* ============================================================================
 * Light Registration and State
 * ============================================================================ */

/**
 * @brief Register a light entity
 *
 * @param[in] config Light configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum lights reached
 */
esp_err_t esphome_entity_register_light(const esphome_light_config_t *config);

/**
 * @brief Update light state
 *
 * @param[in] key Entity key
 * @param[in] state Light state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_light(esphome_entity_key_t key,
                                       const esphome_light_state_t *state);

/**
 * @brief Get light state
 *
 * @param[in] key Entity key
 * @param[out] state Light state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_light(esphome_entity_key_t key, esphome_light_state_t *state);

/**
 * @brief Get light configuration
 *
 * @param[in] key Entity key
 * @param[out] config Light configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_light_config(esphome_entity_key_t key,
                                           esphome_light_config_t *config);

/**
 * @brief Get number of registered lights
 *
 * @return Number of lights
 */
size_t esphome_entity_get_light_count(void);

/**
 * @brief Execute light command
 *
 * Called when a light command is received from a client.
 *
 * @param[in] key Entity key
 * @param[in] command Light command structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_light_command(esphome_entity_key_t key,
                                                const esphome_light_command_t *command);

/* ============================================================================
 * Cover Registration and State
 * ============================================================================ */

/**
 * @brief Register a cover entity
 *
 * @param[in] config Cover configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum covers reached
 */
esp_err_t esphome_entity_register_cover(const esphome_cover_config_t *config);

/**
 * @brief Update cover state
 *
 * @param[in] key Entity key
 * @param[in] state Cover state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_cover(esphome_entity_key_t key,
                                       const esphome_cover_state_t *state);

/**
 * @brief Get cover state
 *
 * @param[in] key Entity key
 * @param[out] state Cover state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_cover(esphome_entity_key_t key, esphome_cover_state_t *state);

/**
 * @brief Get cover configuration
 *
 * @param[in] key Entity key
 * @param[out] config Cover configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_cover_config(esphome_entity_key_t key,
                                           esphome_cover_config_t *config);

/**
 * @brief Get number of registered covers
 *
 * @return Number of covers
 */
size_t esphome_entity_get_cover_count(void);

/**
 * @brief Execute cover command
 *
 * Called when a cover command is received from a client.
 *
 * @param[in] key Entity key
 * @param[in] command Cover command structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_cover_command(esphome_entity_key_t key,
                                                const esphome_cover_command_t *command);

/* ============================================================================
 * Fan Registration and State
 * ============================================================================ */

/**
 * @brief Register a fan entity
 *
 * @param[in] config Fan configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum fans reached
 */
esp_err_t esphome_entity_register_fan(const esphome_fan_config_t *config);

/**
 * @brief Update fan state
 *
 * @param[in] key Entity key
 * @param[in] state Fan state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_fan(esphome_entity_key_t key, const esphome_fan_state_t *state);

/**
 * @brief Get fan state
 *
 * @param[in] key Entity key
 * @param[out] state Fan state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_fan(esphome_entity_key_t key, esphome_fan_state_t *state);

/**
 * @brief Get fan configuration
 *
 * @param[in] key Entity key
 * @param[out] config Fan configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_fan_config(esphome_entity_key_t key, esphome_fan_config_t *config);

/**
 * @brief Get number of registered fans
 *
 * @return Number of fans
 */
size_t esphome_entity_get_fan_count(void);

/**
 * @brief Execute fan command
 *
 * Called when a fan command is received from a client.
 *
 * @param[in] key Entity key
 * @param[in] command Fan command structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_fan_command(esphome_entity_key_t key,
                                              const esphome_fan_command_t *command);

/* ============================================================================
 * Climate Registration and State
 * ============================================================================ */

/**
 * @brief Register a climate entity
 *
 * @param[in] config Climate configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum climates reached
 */
esp_err_t esphome_entity_register_climate(const esphome_climate_config_t *config);

/**
 * @brief Update climate state
 *
 * @param[in] key Entity key
 * @param[in] state Climate state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_climate(esphome_entity_key_t key,
                                         const esphome_climate_state_t *state);

/**
 * @brief Get climate state
 *
 * @param[in] key Entity key
 * @param[out] state Climate state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_climate(esphome_entity_key_t key, esphome_climate_state_t *state);

/**
 * @brief Get climate configuration
 *
 * @param[in] key Entity key
 * @param[out] config Climate configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_climate_config(esphome_entity_key_t key,
                                             esphome_climate_config_t *config);

/**
 * @brief Get number of registered climates
 *
 * @return Number of climate entities
 */
size_t esphome_entity_get_climate_count(void);

/**
 * @brief Execute climate command
 *
 * Called when a climate command is received from a client.
 *
 * @param[in] key Entity key
 * @param[in] command Climate command structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_climate_command(esphome_entity_key_t key,
                                                  const esphome_climate_command_t *command);

/* ============================================================================
 * Lock Registration and State
 * ============================================================================ */

/**
 * @brief Register a lock entity
 *
 * @param[in] config Lock configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum locks reached
 */
esp_err_t esphome_entity_register_lock(const esphome_lock_config_t *config);

/**
 * @brief Update lock state
 *
 * @param[in] key Entity key
 * @param[in] state Lock state
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_lock(esphome_entity_key_t key, esphome_lock_state_t state);

/**
 * @brief Get lock state
 *
 * @param[in] key Entity key
 * @param[out] state Lock state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_lock(esphome_entity_key_t key, esphome_lock_entity_state_t *state);

/**
 * @brief Get lock configuration
 *
 * @param[in] key Entity key
 * @param[out] config Lock configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_lock_config(esphome_entity_key_t key, esphome_lock_config_t *config);

/**
 * @brief Get number of registered locks
 *
 * @return Number of locks
 */
size_t esphome_entity_get_lock_count(void);

/**
 * @brief Execute lock command
 *
 * @param[in] key Entity key
 * @param[in] command Lock command (lock, unlock, open)
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_lock_command(esphome_entity_key_t key,
                                               esphome_lock_command_t command);

/* ============================================================================
 * Media Player Registration and State
 * ============================================================================ */

/**
 * @brief Register a media player entity
 *
 * @param[in] config Media player configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum media players reached
 */
esp_err_t esphome_entity_register_media_player(const esphome_media_player_config_t *config);

/**
 * @brief Update media player state
 *
 * @param[in] key Entity key
 * @param[in] state Media player state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_media_player(esphome_entity_key_t key,
                                              const esphome_media_player_entity_state_t *state);

/**
 * @brief Get media player state
 *
 * @param[in] key Entity key
 * @param[out] state Media player state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_media_player(esphome_entity_key_t key,
                                           esphome_media_player_entity_state_t *state);

/**
 * @brief Get media player configuration
 *
 * @param[in] key Entity key
 * @param[out] config Media player configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_media_player_config(esphome_entity_key_t key,
                                                  esphome_media_player_config_t *config);

/**
 * @brief Get number of registered media players
 *
 * @return Number of media players
 */
size_t esphome_entity_get_media_player_count(void);

/**
 * @brief Execute media player command
 *
 * @param[in] key Entity key
 * @param[in] command Media player command structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_media_player_command(esphome_entity_key_t key,
                                                       const esphome_media_player_cmd_t *command);

/* ============================================================================
 * Alarm Control Panel Registration and State
 * ============================================================================ */

/**
 * @brief Register an alarm control panel entity
 *
 * @param[in] config Alarm configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum alarm panels reached
 */
esp_err_t esphome_entity_register_alarm(const esphome_alarm_config_t *config);

/**
 * @brief Update alarm control panel state
 *
 * @param[in] key Entity key
 * @param[in] state Alarm state
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_alarm(esphome_entity_key_t key, esphome_alarm_state_t state);

/**
 * @brief Get alarm control panel state
 *
 * @param[in] key Entity key
 * @param[out] state Alarm state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_alarm(esphome_entity_key_t key, esphome_alarm_entity_state_t *state);

/**
 * @brief Get alarm control panel configuration
 *
 * @param[in] key Entity key
 * @param[out] config Alarm configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_alarm_config(esphome_entity_key_t key, esphome_alarm_config_t *config);

/**
 * @brief Get number of registered alarm control panels
 *
 * @return Number of alarm panels
 */
size_t esphome_entity_get_alarm_count(void);

/**
 * @brief Execute alarm control panel command
 *
 * @param[in] key Entity key
 * @param[in] command Alarm command
 * @param[in] code Optional code string (may be NULL)
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_alarm_command(esphome_entity_key_t key,
                                                esphome_alarm_command_t command,
                                                const char *code);

/* ============================================================================
 * Text Entity Registration and State
 * ============================================================================ */

/**
 * @brief Register a text entity
 *
 * @param[in] config Text configuration
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if maximum text entities reached
 */
esp_err_t esphome_entity_register_text(const esphome_text_config_t *config);

/**
 * @brief Update text entity state
 *
 * @param[in] key Entity key
 * @param[in] value New text value
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_text(esphome_entity_key_t key, const char *value);

/**
 * @brief Set text entity to missing/unavailable state
 *
 * @param[in] key Entity key
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_set_text_missing(esphome_entity_key_t key);

/**
 * @brief Get text entity state
 *
 * @param[in] key Entity key
 * @param[out] state Text state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_text(esphome_entity_key_t key, esphome_text_entity_state_t *state);

/**
 * @brief Get text entity configuration
 *
 * @param[in] key Entity key
 * @param[out] config Text configuration
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_get_text_config(esphome_entity_key_t key, esphome_text_config_t *config);

/**
 * @brief Get number of registered text entities
 *
 * @return Number of text entities
 */
size_t esphome_entity_get_text_count(void);

/**
 * @brief Execute text entity command
 *
 * @param[in] key Entity key
 * @param[in] value Text value
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_execute_text_command(esphome_entity_key_t key, const char *value);

/* ============================================================================
 * Generic Entity State Update
 * ============================================================================ */

/**
 * @brief Update entity state by key (generic)
 *
 * Type is determined automatically from entity registration.
 *
 * @param[in] key Entity key
 * @param[in] state Pointer to appropriate state structure
 * @return ESP_OK on success
 */
esp_err_t esphome_entity_update_state(esphome_entity_key_t key, const void *state);

/**
 * @brief Find entity type by key
 *
 * @param[in] key Entity key
 * @param[out] type Entity type
 * @return ESP_OK if found
 */
esp_err_t esphome_entity_get_type(esphome_entity_key_t key, esphome_entity_type_t *type);

/* ============================================================================
 * Entity Enumeration
 * ============================================================================ */

/**
 * @brief Enumerate all registered entities
 *
 * Calls callback for each registered entity.
 *
 * @param[in] callback Enumeration callback
 * @param[in] user_data User data passed to callback
 */
void esphome_entities_enumerate(esphome_entity_enum_cb_t callback, void *user_data);

/**
 * @brief Get total number of registered entities
 *
 * @return Total entity count
 */
size_t esphome_entities_get_total_count(void);

/* ============================================================================
 * Message Encoding Helpers - Sensor
 * ============================================================================ */

/**
 * @brief Encode sensor entity info for ListEntities response
 *
 * @param[in] config Sensor configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_sensor_list_entry(const esphome_sensor_config_t *config,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len);

/**
 * @brief Encode sensor state message
 *
 * @param[in] state Sensor state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_sensor_state(const esphome_sensor_state_t *state,
                                       uint8_t *output, size_t output_size,
                                       size_t *output_len);

/* ============================================================================
 * Message Encoding Helpers - Binary Sensor
 * ============================================================================ */

/**
 * @brief Encode binary sensor entity info for ListEntities response
 *
 * @param[in] config Binary sensor configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_binary_sensor_list_entry(const esphome_binary_sensor_config_t *config,
                                                   uint8_t *output, size_t output_size,
                                                   size_t *output_len);

/**
 * @brief Encode binary sensor state message
 *
 * @param[in] state Binary sensor state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_binary_sensor_state(const esphome_binary_sensor_state_t *state,
                                              uint8_t *output, size_t output_size,
                                              size_t *output_len);

/* ============================================================================
 * Message Encoding Helpers - Switch
 * ============================================================================ */

/**
 * @brief Encode switch entity info for ListEntities response
 *
 * @param[in] config Switch configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_switch_list_entry(const esphome_switch_config_t *config,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len);

/**
 * @brief Encode switch state message
 *
 * @param[in] state Switch state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_switch_state(const esphome_switch_state_t *state,
                                       uint8_t *output, size_t output_size,
                                       size_t *output_len);

/* ============================================================================
 * Message Encoding Helpers - Text Sensor
 * ============================================================================ */

/**
 * @brief Encode text sensor entity info for ListEntities response
 *
 * @param[in] config Text sensor configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_text_sensor_list_entry(const esphome_text_sensor_config_t *config,
                                                 uint8_t *output, size_t output_size,
                                                 size_t *output_len);

/**
 * @brief Encode text sensor state message
 *
 * @param[in] state Text sensor state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_text_sensor_state(const esphome_text_sensor_state_t *state,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len);

/* ============================================================================
 * Message Encoding Helpers - Number
 * ============================================================================ */

/**
 * @brief Encode number entity info for ListEntities response
 *
 * @param[in] config Number configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_number_list_entry(const esphome_number_config_t *config,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len);

/**
 * @brief Encode number state message
 *
 * @param[in] state Number state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_number_state(const esphome_number_state_t *state,
                                       uint8_t *output, size_t output_size,
                                       size_t *output_len);

/* ============================================================================
 * Message Encoding Helpers - Button
 * ============================================================================ */

/**
 * @brief Encode button entity info for ListEntities response
 *
 * @param[in] config Button configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_button_list_entry(const esphome_button_config_t *config,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len);

/* ============================================================================
 * Message Encoding Helpers - Select
 * ============================================================================ */

/**
 * @brief Encode select entity info for ListEntities response
 *
 * @param[in] config Select configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_select_list_entry(const esphome_select_config_t *config,
                                            uint8_t *output, size_t output_size,
                                            size_t *output_len);

/**
 * @brief Encode select state message
 *
 * @param[in] state Select state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_select_state(const esphome_select_state_t *state,
                                       uint8_t *output, size_t output_size,
                                       size_t *output_len);

/* ============================================================================
 * Message Encoding Helpers - Light
 * ============================================================================ */

/**
 * @brief Encode light entity info for ListEntities response
 *
 * @param[in] config Light configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_light_list_entry(const esphome_light_config_t *config,
                                           uint8_t *output, size_t output_size,
                                           size_t *output_len);

/**
 * @brief Encode light state message
 *
 * @param[in] state Light state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_light_state(const esphome_light_state_t *state,
                                      uint8_t *output, size_t output_size,
                                      size_t *output_len);

/* ============================================================================
 * Message Encoding Helpers - Cover
 * ============================================================================ */

/**
 * @brief Encode cover entity info for ListEntities response
 *
 * @param[in] config Cover configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_cover_list_entry(const esphome_cover_config_t *config,
                                           uint8_t *output, size_t output_size,
                                           size_t *output_len);

/**
 * @brief Encode cover state message
 *
 * @param[in] state Cover state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_cover_state(const esphome_cover_state_t *state,
                                      uint8_t *output, size_t output_size,
                                      size_t *output_len);

/* ============================================================================
 * Message Encoding Helpers - Fan
 * ============================================================================ */

/**
 * @brief Encode fan entity info for ListEntities response
 *
 * @param[in] config Fan configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_fan_list_entry(const esphome_fan_config_t *config,
                                         uint8_t *output, size_t output_size,
                                         size_t *output_len);

/**
 * @brief Encode fan state message
 *
 * @param[in] state Fan state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_fan_state(const esphome_fan_state_t *state,
                                    uint8_t *output, size_t output_size,
                                    size_t *output_len);

/* ============================================================================
 * Message Encoding Helpers - Climate
 * ============================================================================ */

/**
 * @brief Encode climate entity info for ListEntities response
 *
 * @param[in] config Climate configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_climate_list_entry(const esphome_climate_config_t *config,
                                             uint8_t *output, size_t output_size,
                                             size_t *output_len);

/**
 * @brief Encode climate state message
 *
 * @param[in] state Climate state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_climate_state(const esphome_climate_state_t *state,
                                        uint8_t *output, size_t output_size,
                                        size_t *output_len);

/* ============================================================================
 * Lock Entity Encoding
 * ============================================================================ */

/**
 * @brief Encode lock entity list entry
 *
 * @param[in] config Lock configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_lock_list_entry(const esphome_lock_config_t *config,
                                          uint8_t *output, size_t output_size,
                                          size_t *output_len);

/**
 * @brief Encode lock entity state
 *
 * @param[in] state Lock state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_lock_state(const esphome_lock_entity_state_t *state,
                                     uint8_t *output, size_t output_size,
                                     size_t *output_len);

/* ============================================================================
 * Text Entity Encoding
 * ============================================================================ */

/**
 * @brief Encode text entity list entry
 *
 * @param[in] config Text configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_text_list_entry(const esphome_text_config_t *config,
                                          uint8_t *output, size_t output_size,
                                          size_t *output_len);

/**
 * @brief Encode text entity state
 *
 * @param[in] state Text state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_text_state(const esphome_text_entity_state_t *state,
                                     uint8_t *output, size_t output_size,
                                     size_t *output_len);

/* ============================================================================
 * Media Player Entity Encoding
 * ============================================================================ */

/**
 * @brief Encode media player entity list entry
 *
 * @param[in] config Media player configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_media_player_list_entry(const esphome_media_player_config_t *config,
                                                   uint8_t *output, size_t output_size,
                                                   size_t *output_len);

/**
 * @brief Encode media player entity state
 *
 * @param[in] state Media player state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_media_player_state(const esphome_media_player_entity_state_t *state,
                                             uint8_t *output, size_t output_size,
                                             size_t *output_len);

/* ============================================================================
 * Alarm Control Panel Entity Encoding
 * ============================================================================ */

/**
 * @brief Encode alarm control panel entity list entry
 *
 * @param[in] config Alarm configuration
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_alarm_list_entry(const esphome_alarm_config_t *config,
                                           uint8_t *output, size_t output_size,
                                           size_t *output_len);

/**
 * @brief Encode alarm control panel entity state
 *
 * @param[in] state Alarm state
 * @param[out] output Output buffer
 * @param[in] output_size Output buffer size
 * @param[out] output_len Actual output length
 * @return ESP_OK on success
 */
esp_err_t esphome_encode_alarm_state(const esphome_alarm_entity_state_t *state,
                                      uint8_t *output, size_t output_size,
                                      size_t *output_len);

/* ============================================================================
 * Testing
 * ============================================================================ */

/**
 * @brief Test entity management
 *
 * @return ESP_OK if tests pass
 */
esp_err_t esphome_entities_test(void);


/**
 * @brief Forget every entity belonging to one device, so it can be rebuilt
 *
 * Needed when a device is paired again and matches a different converter than
 * before: without this the adapter's "already registered" guard keeps the old,
 * wrong entities and never creates the right ones.
 *
 * @param device_id Sub-device to clear
 * @return Number of entities forgotten
 */
uint16_t esphome_entities_unregister_device(uint32_t device_id);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ENTITIES_H */
