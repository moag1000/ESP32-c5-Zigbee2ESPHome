/**
 * @file esphome_entities_types.h
 * @brief ESPHome Entity Type Definitions
 *
 * This header contains all type definitions (structs, enums, typedefs, and
 * callback types) for the ESPHome entity management module. It is included
 * by esphome_entities.h and can also be included separately when only type
 * definitions are needed (for faster compilation).
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_ENTITIES_TYPES_H
#define ESPHOME_ENTITIES_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esphome_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Sensor Entity Types
 * ============================================================================ */

/**
 * @brief Sensor entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    char unit_of_measurement[ESPHOME_MAX_UNIT_LEN];     /**< Unit (e.g., "C", "%") */
    int32_t accuracy_decimals;                          /**< Decimal precision */
    bool force_update;                                  /**< Force update even if unchanged */
    esphome_sensor_device_class_t device_class;         /**< Device class */
    esphome_state_class_t state_class;                  /**< State class */
    bool disabled_by_default;                           /**< Disabled by default */
    uint8_t entity_category;                            /**< Entity category (0=NONE, 1=CONFIG, 2=DIAGNOSTIC) */
} esphome_sensor_config_t;

/**
 * @brief Sensor state structure
 */
typedef struct {
    esphome_entity_key_t key;   /**< Entity key */
    float state;                /**< Current sensor value */
    bool missing_state;         /**< True if state is unknown/unavailable */
} esphome_sensor_state_t;

/* ============================================================================
 * Binary Sensor Entity Types
 * ============================================================================ */

/**
 * @brief Binary sensor entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    esphome_binary_device_class_t device_class;         /**< Device class */
    bool is_status_binary_sensor;                       /**< Is status sensor */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_binary_sensor_config_t;

/**
 * @brief Binary sensor state structure
 */
typedef struct {
    esphome_entity_key_t key;   /**< Entity key */
    bool state;                 /**< Current state (on/off) */
    bool missing_state;         /**< True if state is unknown/unavailable */
} esphome_binary_sensor_state_t;

/* ============================================================================
 * Switch Entity Types
 * ============================================================================ */

/**
 * @brief Switch command callback type
 *
 * @param[in] key Entity key
 * @param[in] state Requested state (true=ON, false=OFF)
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_switch_command_cb_t)(esphome_entity_key_t key, bool state);

/**
 * @brief Switch entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    bool assumed_state;                                 /**< Use assumed state */
    bool disabled_by_default;                           /**< Disabled by default */
    esphome_switch_command_cb_t command_callback;       /**< Command callback */
} esphome_switch_config_t;

/**
 * @brief Switch state structure
 */
typedef struct {
    esphome_entity_key_t key;   /**< Entity key */
    bool state;                 /**< Current state (on/off) */
} esphome_switch_state_t;

/* ============================================================================
 * Text Sensor Entity Types
 * ============================================================================ */

/**
 * @brief Text sensor entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_text_sensor_config_t;

/**
 * @brief Text sensor state structure
 */
typedef struct {
    esphome_entity_key_t key;                   /**< Entity key */
    char state[ESPHOME_MAX_STRING_LEN];         /**< Current string state */
    bool missing_state;                         /**< True if state is unknown/unavailable */
} esphome_text_sensor_state_t;

/* ============================================================================
 * Number Entity Types
 * ============================================================================ */

/**
 * @brief Number command callback type
 *
 * @param[in] key Entity key
 * @param[in] value Requested numeric value
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_number_command_cb_t)(esphome_entity_key_t key, float value);

/**
 * @brief Number entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    char unit_of_measurement[ESPHOME_MAX_UNIT_LEN];     /**< Unit (e.g., "C", "%") */
    float min_value;                                    /**< Minimum value */
    float max_value;                                    /**< Maximum value */
    float step;                                         /**< Step size */
    esphome_number_mode_t mode;                         /**< Display mode (box/slider) */
    esphome_number_command_cb_t command_callback;       /**< Command callback */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_number_config_t;

/**
 * @brief Number state structure
 */
typedef struct {
    esphome_entity_key_t key;   /**< Entity key */
    float state;                /**< Current numeric value */
    bool missing_state;         /**< True if state is unknown/unavailable */
} esphome_number_state_t;

/* ============================================================================
 * Button Entity Types
 * ============================================================================ */

/**
 * @brief Button press callback type
 *
 * @param[in] key Entity key
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_button_press_cb_t)(esphome_entity_key_t key);

/**
 * @brief Button entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    esphome_button_device_class_t device_class;         /**< Device class */
    esphome_button_press_cb_t press_callback;           /**< Press callback */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_button_config_t;

/* ============================================================================
 * Select Entity Types
 * ============================================================================ */

/**
 * @brief Select command callback type
 *
 * @param[in] key Entity key
 * @param[in] option Selected option string
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_select_command_cb_t)(esphome_entity_key_t key, const char *option);

/**
 * @brief Select entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];                          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                                    /**< MDI icon */
    char options[ESPHOME_MAX_SELECT_OPTIONS][32];                       /**< Available options */
    uint8_t option_count;                                               /**< Number of options */
    esphome_select_command_cb_t command_callback;                       /**< Command callback */
    bool disabled_by_default;                                           /**< Disabled by default */
} esphome_select_config_t;

/**
 * @brief Select state structure
 */
typedef struct {
    esphome_entity_key_t key;   /**< Entity key */
    char state[32];             /**< Currently selected option */
    bool missing_state;         /**< True if state is unknown/unavailable */
} esphome_select_state_t;

/* ============================================================================
 * Light Entity Types
 * ============================================================================ */

/**
 * @brief Light command structure
 */
typedef struct {
    bool has_state;             /**< State field is valid */
    bool state;                 /**< Requested on/off state */
    bool has_brightness;        /**< Brightness field is valid */
    float brightness;           /**< Brightness level (0.0-1.0) */
    bool has_color_mode;        /**< Color mode field is valid */
    esphome_color_mode_t color_mode;    /**< Requested color mode */
    bool has_color_temp;        /**< Color temperature field is valid */
    float color_temp;           /**< Color temperature in mireds */
    bool has_rgb;               /**< RGB fields are valid */
    float red;                  /**< Red component (0.0-1.0) */
    float green;                /**< Green component (0.0-1.0) */
    float blue;                 /**< Blue component (0.0-1.0) */
    bool has_white;             /**< White field is valid */
    float white;                /**< White brightness (0.0-1.0) */
    bool has_effect;            /**< Effect field is valid */
    char effect[32];            /**< Effect name */
    bool has_transition_length; /**< Transition length field is valid */
    uint32_t transition_length; /**< Transition time in ms */
    bool has_flash_length;      /**< Flash length field is valid */
    uint32_t flash_length;      /**< Flash time in ms */
} esphome_light_command_t;

/**
 * @brief Light command callback type
 *
 * @param[in] key Entity key
 * @param[in] command Light command structure
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_light_command_cb_t)(esphome_entity_key_t key,
                                                 const esphome_light_command_t *command);

/**
 * @brief Light entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];                          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                                    /**< MDI icon */
    uint32_t supported_color_modes;                                     /**< Bitmask of esphome_color_mode_t */
    float min_mireds;                                                   /**< Minimum color temperature */
    float max_mireds;                                                   /**< Maximum color temperature */
    char effects[ESPHOME_MAX_LIGHT_EFFECTS][32];                        /**< Available effects */
    uint8_t effect_count;                                               /**< Number of effects */
    esphome_light_command_cb_t command_callback;                        /**< Command callback */
    bool disabled_by_default;                                           /**< Disabled by default */
} esphome_light_config_t;

/**
 * @brief Light state structure
 */
typedef struct {
    esphome_entity_key_t key;           /**< Entity key */
    bool state;                         /**< On/off state */
    float brightness;                   /**< Brightness level (0.0-1.0) */
    esphome_color_mode_t color_mode;    /**< Current color mode */
    float color_temp;                   /**< Color temperature in mireds */
    float red;                          /**< Red component (0.0-1.0) */
    float green;                        /**< Green component (0.0-1.0) */
    float blue;                         /**< Blue component (0.0-1.0) */
    float white;                        /**< White brightness (0.0-1.0) */
    char effect[32];                    /**< Current effect name */
} esphome_light_state_t;

/* ============================================================================
 * Cover Entity Types
 * ============================================================================ */

/**
 * @brief Cover command structure
 */
typedef struct {
    bool has_position;          /**< Position field is valid */
    float position;             /**< Target position (0.0-1.0, 0=closed, 1=open) */
    bool has_tilt;              /**< Tilt field is valid */
    float tilt;                 /**< Target tilt (0.0-1.0) */
    bool stop;                  /**< Stop current movement */
} esphome_cover_command_t;

/**
 * @brief Cover command callback type
 *
 * @param[in] key Entity key
 * @param[in] command Cover command structure
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_cover_command_cb_t)(esphome_entity_key_t key,
                                                 const esphome_cover_command_t *command);

/**
 * @brief Cover entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    esphome_cover_device_class_t device_class;          /**< Device class */
    bool assumed_state;                                 /**< Use assumed state */
    bool supports_position;                             /**< Supports position control */
    bool supports_tilt;                                 /**< Supports tilt control */
    esphome_cover_command_cb_t command_callback;        /**< Command callback */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_cover_config_t;

/**
 * @brief Cover state structure
 */
typedef struct {
    esphome_entity_key_t key;               /**< Entity key */
    float position;                         /**< Current position (0.0-1.0) */
    float tilt;                             /**< Current tilt (0.0-1.0) */
    esphome_cover_operation_t current_operation;    /**< Current operation state */
} esphome_cover_state_t;

/* ============================================================================
 * Fan Entity Types
 * ============================================================================ */

/**
 * @brief Fan command structure
 */
typedef struct {
    bool has_state;             /**< State field is valid */
    bool state;                 /**< Requested on/off state */
    bool has_speed_level;       /**< Speed level field is valid */
    int32_t speed_level;        /**< Speed level (1 to supported_speed_count) */
    bool has_oscillating;       /**< Oscillating field is valid */
    bool oscillating;           /**< Oscillation state */
    bool has_direction;         /**< Direction field is valid */
    esphome_fan_direction_t direction;  /**< Fan direction */
} esphome_fan_command_t;

/**
 * @brief Fan command callback type
 *
 * @param[in] key Entity key
 * @param[in] command Fan command structure
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_fan_command_cb_t)(esphome_entity_key_t key,
                                               const esphome_fan_command_t *command);

/**
 * @brief Fan entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    bool supports_oscillation;                          /**< Supports oscillation */
    bool supports_speed;                                /**< Supports speed control */
    bool supports_direction;                            /**< Supports direction control */
    int32_t supported_speed_count;                      /**< Number of speed levels */
    esphome_fan_command_cb_t command_callback;          /**< Command callback */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_fan_config_t;

/**
 * @brief Fan state structure
 */
typedef struct {
    esphome_entity_key_t key;               /**< Entity key */
    bool state;                             /**< On/off state */
    bool oscillating;                       /**< Oscillation state */
    int32_t speed_level;                    /**< Current speed level */
    esphome_fan_direction_t direction;      /**< Current direction */
} esphome_fan_state_t;

/* ============================================================================
 * Climate Entity Types
 * ============================================================================ */

/**
 * @brief Climate command structure
 */
typedef struct {
    bool has_mode;                      /**< Mode field is valid */
    esphome_climate_mode_t mode;        /**< Requested mode */
    bool has_target_temperature;        /**< Target temperature field is valid */
    float target_temperature;           /**< Target temperature */
    bool has_target_temperature_low;    /**< Target temperature low field is valid */
    float target_temperature_low;       /**< Target temperature low (for dual setpoint) */
    bool has_target_temperature_high;   /**< Target temperature high field is valid */
    float target_temperature_high;      /**< Target temperature high (for dual setpoint) */
    bool has_fan_mode;                  /**< Fan mode field is valid */
    esphome_climate_fan_mode_t fan_mode;    /**< Requested fan mode */
    bool has_swing_mode;                /**< Swing mode field is valid */
    esphome_climate_swing_mode_t swing_mode;    /**< Requested swing mode */
    bool has_preset;                    /**< Preset field is valid */
    esphome_climate_preset_t preset;    /**< Requested preset */
} esphome_climate_command_t;

/**
 * @brief Climate command callback type
 *
 * @param[in] key Entity key
 * @param[in] command Climate command structure
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_climate_command_cb_t)(esphome_entity_key_t key,
                                                   const esphome_climate_command_t *command);

/**
 * @brief Climate entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    bool supports_current_temperature;                  /**< Reports current temperature */
    bool supports_two_point_target_temperature;         /**< Dual setpoint mode */
    uint32_t supported_modes;                           /**< Bitmask of climate modes */
    float visual_min_temperature;                       /**< Minimum displayed temperature */
    float visual_max_temperature;                       /**< Maximum displayed temperature */
    float visual_temperature_step;                      /**< Temperature step */
    bool supports_action;                               /**< Reports current action */
    uint32_t supported_fan_modes;                       /**< Bitmask of fan modes */
    uint32_t supported_swing_modes;                     /**< Bitmask of swing modes */
    uint32_t supported_presets;                         /**< Bitmask of presets */
    esphome_climate_command_cb_t command_callback;      /**< Command callback */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_climate_config_t;

/**
 * @brief Climate state structure
 */
typedef struct {
    esphome_entity_key_t key;               /**< Entity key */
    esphome_climate_mode_t mode;            /**< Current mode */
    esphome_climate_action_t action;        /**< Current action */
    float current_temperature;              /**< Current temperature */
    float target_temperature;               /**< Target temperature */
    float target_temperature_low;           /**< Target temperature low */
    float target_temperature_high;          /**< Target temperature high */
    esphome_climate_fan_mode_t fan_mode;    /**< Current fan mode */
    esphome_climate_swing_mode_t swing_mode;    /**< Current swing mode */
    esphome_climate_preset_t preset;        /**< Current preset */
} esphome_climate_state_t;

/* ============================================================================
 * Lock Entity Types
 * ============================================================================ */

/**
 * @brief Lock command callback type
 *
 * @param[in] key Entity key
 * @param[in] command Lock command (lock, unlock, open)
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_lock_command_cb_t)(esphome_entity_key_t key,
                                                esphome_lock_command_t command);

/**
 * @brief Lock entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    bool assumed_state;                                 /**< Use assumed state */
    bool supports_open;                                 /**< Supports open command */
    bool requires_code;                                 /**< Requires code to operate */
    esphome_lock_command_cb_t command_callback;         /**< Command callback */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_lock_config_t;

/**
 * @brief Lock state structure
 */
typedef struct {
    esphome_entity_key_t key;               /**< Entity key */
    esphome_lock_state_t state;             /**< Current lock state */
} esphome_lock_entity_state_t;

/* ============================================================================
 * Media Player Entity Types
 * ============================================================================ */

/**
 * @brief Media player command structure
 */
typedef struct {
    bool has_command;                           /**< Command field is valid */
    esphome_media_player_command_t command;     /**< Command to execute */
    bool has_volume;                            /**< Volume field is valid */
    float volume;                               /**< Volume level (0.0-1.0) */
    bool has_media_url;                         /**< Media URL field is valid */
    char media_url[ESPHOME_MAX_STRING_LEN];     /**< Media URL to play */
} esphome_media_player_cmd_t;

/**
 * @brief Media player command callback type
 *
 * @param[in] key Entity key
 * @param[in] command Media player command structure
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_media_player_command_cb_t)(esphome_entity_key_t key,
                                                        const esphome_media_player_cmd_t *cmd);

/**
 * @brief Media player entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    bool supports_pause;                                /**< Supports pause command */
    esphome_media_player_command_cb_t command_callback; /**< Command callback */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_media_player_config_t;

/**
 * @brief Media player state structure
 */
typedef struct {
    esphome_entity_key_t key;                   /**< Entity key */
    esphome_media_player_state_t state;         /**< Current state */
    float volume;                               /**< Current volume (0.0-1.0) */
    bool muted;                                 /**< Muted state */
} esphome_media_player_entity_state_t;

/* ============================================================================
 * Alarm Control Panel Entity Types
 * ============================================================================ */

/**
 * @brief Alarm control panel command callback type
 *
 * @param[in] key Entity key
 * @param[in] command Alarm command (arm, disarm, etc.)
 * @param[in] code Optional code string (may be NULL)
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_alarm_command_cb_t)(esphome_entity_key_t key,
                                                 esphome_alarm_command_t command,
                                                 const char *code);

/**
 * @brief Alarm control panel entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    uint32_t supported_features;                        /**< Bitmask of supported commands */
    bool requires_code;                                 /**< Requires code for all operations */
    bool requires_code_to_arm;                          /**< Requires code to arm */
    esphome_alarm_command_cb_t command_callback;        /**< Command callback */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_alarm_config_t;

/**
 * @brief Alarm control panel state structure
 */
typedef struct {
    esphome_entity_key_t key;               /**< Entity key */
    esphome_alarm_state_t state;            /**< Current alarm state */
} esphome_alarm_entity_state_t;

/* ============================================================================
 * Text Entity (Input Text) Types
 * ============================================================================ */

/**
 * @brief Text command callback type
 *
 * @param[in] key Entity key
 * @param[in] value Text value
 * @return ESP_OK if command accepted
 */
typedef esp_err_t (*esphome_text_command_cb_t)(esphome_entity_key_t key, const char *value);

/**
 * @brief Text entity configuration
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon */
    uint32_t min_length;                                /**< Minimum text length */
    uint32_t max_length;                                /**< Maximum text length */
    char pattern[64];                                   /**< Regex validation pattern */
    esphome_text_mode_t mode;                           /**< Display mode (text/password) */
    esphome_text_command_cb_t command_callback;         /**< Command callback */
    bool disabled_by_default;                           /**< Disabled by default */
} esphome_text_config_t;

/**
 * @brief Text entity state structure
 */
typedef struct {
    esphome_entity_key_t key;                   /**< Entity key */
    char state[ESPHOME_MAX_STRING_LEN];         /**< Current text value */
    bool missing_state;                         /**< True if state is unknown/unavailable */
} esphome_text_entity_state_t;

/* ============================================================================
 * State Update Callback Type
 * ============================================================================ */

/**
 * @brief State update callback type
 *
 * Called when entity state changes and needs to be broadcast to clients.
 *
 * @param[in] entity_type Entity type
 * @param[in] key Entity key
 * @param[in] state Pointer to state structure
 */
typedef void (*esphome_state_change_cb_t)(esphome_entity_type_t entity_type,
                                          esphome_entity_key_t key, const void *state);

/* ============================================================================
 * Entity Enumeration Callback Type
 * ============================================================================ */

/**
 * @brief Callback for entity enumeration
 *
 * @param[in] type Entity type
 * @param[in] key Entity key
 * @param[in] config Pointer to entity config (type-specific)
 * @param[in] user_data User data passed to enumerate function
 * @return true to continue enumeration, false to stop
 */
typedef bool (*esphome_entity_enum_cb_t)(esphome_entity_type_t type, esphome_entity_key_t key,
                                         const void *config, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ENTITIES_TYPES_H */
