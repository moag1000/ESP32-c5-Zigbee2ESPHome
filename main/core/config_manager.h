/**
 * @file config_manager.h
 * @brief Configuration Manager for Centralized Settings
 *
 * Manages all gateway configuration with NVS persistence.
 * Provides runtime configuration changes via MQTT without recompilation.
 * Falls back to Kconfig defaults when NVS is empty.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum string lengths for configuration
 */
#define CONFIG_SSID_MAX_LEN         32
#define CONFIG_PASSWORD_MAX_LEN     64
#define CONFIG_URL_MAX_LEN          128
#define CONFIG_ID_MAX_LEN           64

/**
 * @brief Gateway configuration structure
 *
 * Contains all configurable parameters for the gateway
 */
typedef struct {
    /* Network Configuration */
    char wifi_ssid[CONFIG_SSID_MAX_LEN];           /**< WiFi SSID */
    char wifi_password[CONFIG_PASSWORD_MAX_LEN];   /**< WiFi password */

    /* MQTT Configuration */
    char mqtt_broker_url[CONFIG_URL_MAX_LEN];      /**< MQTT broker URL */
    uint16_t mqtt_port;                            /**< MQTT broker port */
    char mqtt_username[CONFIG_ID_MAX_LEN];         /**< MQTT username */
    char mqtt_password[CONFIG_PASSWORD_MAX_LEN];   /**< MQTT password */
    char mqtt_client_id[CONFIG_ID_MAX_LEN];        /**< MQTT client ID */
    uint16_t mqtt_keepalive;                       /**< MQTT keepalive seconds */
    uint8_t mqtt_qos;                              /**< MQTT QoS level */

    /* Zigbee Configuration */
    uint16_t zigbee_pan_id;                        /**< Zigbee PAN ID */
    uint8_t zigbee_channel;                        /**< Zigbee channel (11-26) */
    uint8_t zigbee_max_children;                   /**< Max Zigbee children */
    bool zigbee_permit_join_on_boot;               /**< Permit join on boot */

    /* Gateway Configuration */
    uint32_t device_publish_interval_ms;           /**< Device publish interval */
    bool ha_discovery_enabled;                     /**< Home Assistant discovery */
    bool bridge_logging_enabled;                   /**< Bridge logging to MQTT */

    /* System Configuration */
    uint8_t log_level;                             /**< ESP log level (0-5) */
    bool ota_enabled;                              /**< OTA updates enabled */
    char ota_url[CONFIG_URL_MAX_LEN];              /**< OTA firmware URL */

    /* Internal */
    uint32_t config_version;                       /**< Config version for migration */
} gateway_config_t;

/**
 * @brief Configuration version for compatibility
 */
#define CONFIG_VERSION  1

/**
 * @brief NVS namespace for configuration
 */
#define CONFIG_NVS_NAMESPACE  "gateway_cfg"

/**
 * @brief Initialize configuration manager
 *
 * Loads configuration from NVS or uses Kconfig defaults.
 *
 * @return ESP_OK on success
 *         ESP_FAIL on failure
 */
esp_err_t config_manager_init(void);

/**
 * @brief Load configuration from NVS
 *
 * Loads stored configuration from NVS. If NVS is empty or corrupt,
 * returns ESP_ERR_NOT_FOUND and uses defaults.
 *
 * @param[out] config Pointer to configuration structure
 * @return ESP_OK on success
 *         ESP_ERR_NOT_FOUND if no config in NVS
 *         ESP_ERR_INVALID_ARG if config is NULL
 */
esp_err_t config_manager_load(gateway_config_t *config);

/**
 * @brief Save configuration to NVS
 *
 * Persists current configuration to NVS for future boots.
 *
 * @param[in] config Pointer to configuration structure
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if config is NULL
 *         ESP_FAIL on NVS error
 */
esp_err_t config_manager_save(const gateway_config_t *config);

/**
 * @brief Reset configuration to defaults
 *
 * Loads defaults from Kconfig and erases NVS configuration.
 *
 * @return ESP_OK on success
 */
esp_err_t config_manager_reset_to_defaults(void);

/**
 * @brief Get configuration value by key
 *
 * Retrieves a single configuration value by string key.
 *
 * @param[in] key Configuration key (e.g., "wifi_ssid")
 * @param[out] value Buffer to store value
 * @param[in,out] len Size of buffer on input, actual size on output
 * @return ESP_OK on success
 *         ESP_ERR_NOT_FOUND if key not found
 *         ESP_ERR_INVALID_SIZE if buffer too small
 */
esp_err_t config_manager_get(const char *key, void *value, size_t *len);

/**
 * @brief Set configuration value by key
 *
 * Sets a single configuration value by string key and saves to NVS.
 *
 * @param[in] key Configuration key (e.g., "wifi_ssid")
 * @param[in] value Pointer to value to set
 * @param[in] len Size of value
 * @return ESP_OK on success
 *         ESP_ERR_NOT_FOUND if key not found
 *         ESP_ERR_INVALID_ARG if value invalid
 */
esp_err_t config_manager_set(const char *key, const void *value, size_t len);

/**
 * @brief Get current configuration
 *
 * Returns pointer to internal configuration structure (read-only).
 *
 * @return Pointer to current configuration
 */
const gateway_config_t* config_manager_get_config(void);

/**
 * @brief Apply configuration changes
 *
 * Applies configuration changes to running system.
 * May trigger reconnections (WiFi, MQTT, Zigbee).
 *
 * @param[in] config Configuration to apply
 * @return ESP_OK on success
 *         ESP_FAIL if changes require restart
 */
esp_err_t config_manager_apply(const gateway_config_t *config);

/**
 * @brief Export configuration as JSON
 *
 * Generates JSON representation of current configuration.
 * Passwords are masked for security.
 *
 * @param[out] buffer Buffer to store JSON string
 * @param[in] buffer_size Size of buffer
 * @return ESP_OK on success
 *         ESP_ERR_NO_MEM if buffer too small
 */
esp_err_t config_manager_export_json(char *buffer, size_t buffer_size);

/**
 * @brief Import configuration from JSON
 *
 * Parses JSON string and updates configuration.
 * Validates all fields before applying.
 *
 * @param[in] json JSON string
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if JSON invalid
 */
esp_err_t config_manager_import_json(const char *json);

/**
 * @brief Validate configuration
 *
 * Checks if configuration values are valid.
 *
 * @param[in] config Configuration to validate
 * @return ESP_OK if valid
 *         ESP_ERR_INVALID_ARG if invalid
 */
esp_err_t config_manager_validate(const gateway_config_t *config);

/**
 * @brief Load defaults from Kconfig
 *
 * Populates configuration structure with Kconfig compile-time defaults.
 *
 * @param[out] config Configuration structure to populate
 * @return ESP_OK on success
 */
esp_err_t config_manager_load_defaults(gateway_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_MANAGER_H */
