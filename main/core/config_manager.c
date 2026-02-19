/**
 * @file config_manager.c
 * @brief Configuration Manager Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "config_manager.h"
#include "zigbee/zb_constants.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

/* Fallback values for optional Kconfig entries */
#ifndef CONFIG_ENABLE_BRIDGE_LOGGING_TO_MQTT
#define CONFIG_ENABLE_BRIDGE_LOGGING_TO_MQTT 0
#endif

#ifndef CONFIG_ENABLE_HOMEASSISTANT_DISCOVERY
#define CONFIG_ENABLE_HOMEASSISTANT_DISCOVERY 1
#endif

static const char *TAG = "CONFIG_MGR";

/* Current configuration (cached in RAM) */
static gateway_config_t s_current_config;
static bool s_config_initialized = false;

/**
 * @brief Load defaults from Kconfig
 */
esp_err_t config_manager_load_defaults(gateway_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(gateway_config_t));

    /* Network Configuration */
    strncpy(config->wifi_ssid, CONFIG_WIFI_SSID, CONFIG_SSID_MAX_LEN - 1);
    strncpy(config->wifi_password, CONFIG_WIFI_PASSWORD, CONFIG_PASSWORD_MAX_LEN - 1);

    /* MQTT Configuration */
    strncpy(config->mqtt_broker_url, CONFIG_MQTT_BROKER_URL, CONFIG_URL_MAX_LEN - 1);
    config->mqtt_port = CONFIG_MQTT_BROKER_PORT;
    strncpy(config->mqtt_username, CONFIG_MQTT_USERNAME, CONFIG_ID_MAX_LEN - 1);
    strncpy(config->mqtt_password, CONFIG_MQTT_PASSWORD, CONFIG_PASSWORD_MAX_LEN - 1);
    strncpy(config->mqtt_client_id, CONFIG_MQTT_CLIENT_ID, CONFIG_ID_MAX_LEN - 1);
    config->mqtt_keepalive = CONFIG_MQTT_KEEPALIVE;
    config->mqtt_qos = CONFIG_MQTT_QOS;

    /* Zigbee Configuration */
    config->zigbee_pan_id = CONFIG_ZIGBEE_PAN_ID;
    config->zigbee_channel = CONFIG_ZIGBEE_CHANNEL;
    config->zigbee_max_children = CONFIG_ZIGBEE_MAX_CHILDREN;
    config->zigbee_permit_join_on_boot = false; /* Default to secure */

    /* Gateway Configuration */
    config->device_publish_interval_ms = CONFIG_MQTT_PUBLISH_INTERVAL;
    config->ha_discovery_enabled = CONFIG_ENABLE_HOMEASSISTANT_DISCOVERY;
    config->bridge_logging_enabled = CONFIG_ENABLE_BRIDGE_LOGGING_TO_MQTT;

    /* System Configuration */
    config->log_level = ESP_LOG_INFO;
    config->ota_enabled = (strlen(CONFIG_OTA_FIRMWARE_URL) > 0);
    strncpy(config->ota_url, CONFIG_OTA_FIRMWARE_URL, CONFIG_URL_MAX_LEN - 1);

    /* Version */
    config->config_version = CONFIG_VERSION;

    return ESP_OK;
}

/**
 * @brief Validate configuration
 */
esp_err_t config_manager_validate(const gateway_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check WiFi SSID */
    if (strlen(config->wifi_ssid) == 0) {
        ESP_LOGE(TAG, "WiFi SSID cannot be empty");
        return ESP_ERR_INVALID_ARG;
    }

    /* Check MQTT broker URL */
    if (strlen(config->mqtt_broker_url) == 0) {
        ESP_LOGE(TAG, "MQTT broker URL cannot be empty");
        return ESP_ERR_INVALID_ARG;
    }

    /* Check MQTT port */
    if (config->mqtt_port == 0) {
        ESP_LOGE(TAG, "MQTT port cannot be 0");
        return ESP_ERR_INVALID_ARG;
    }

    /* Check Zigbee channel */
    if (config->zigbee_channel < ZB_CHANNEL_MIN || config->zigbee_channel > ZB_CHANNEL_MAX) {
        ESP_LOGE(TAG, "Zigbee channel must be %d-%d", ZB_CHANNEL_MIN, ZB_CHANNEL_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    /* Check log level */
    if (config->log_level > ESP_LOG_VERBOSE) {
        ESP_LOGE(TAG, "Invalid log level");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/**
 * @brief Load configuration from NVS
 */
esp_err_t config_manager_load(gateway_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret;

    /* Open NVS */
    ret = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    /* Load configuration blob */
    size_t required_size = sizeof(gateway_config_t);
    ret = nvs_get_blob(nvs_handle, "config", config, &required_size);

    nvs_close(nvs_handle);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load config from NVS: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    /* Sanitize MQTT broker URL - fix duplicate port issue (e.g., mqtt://host:1883:1883) */
    char *scheme_end = strstr(config->mqtt_broker_url, "://");
    if (scheme_end != NULL) {
        char *host_start = scheme_end + 3;
        /* Look for duplicate port pattern :digits:digits at end */
        char *first_colon = strchr(host_start, ':');
        if (first_colon != NULL) {
            char *second_colon = strchr(first_colon + 1, ':');
            if (second_colon != NULL && second_colon[1] >= '0' && second_colon[1] <= '9') {
                /* Found duplicate port, truncate at second colon */
                ESP_LOGW(TAG, "Fixing duplicate port in MQTT URL: %s", config->mqtt_broker_url);
                *second_colon = '\0';
                ESP_LOGI(TAG, "Fixed MQTT URL: %s", config->mqtt_broker_url);
            }
        }
    }

    /* Validate loaded configuration */
    ret = config_manager_validate(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loaded configuration is invalid");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Configuration loaded from NVS (version: %lu)", config->config_version);
    return ESP_OK;
}

/**
 * @brief Save configuration to NVS
 */
esp_err_t config_manager_save(const gateway_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate before saving */
    esp_err_t ret = config_manager_validate(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save invalid configuration");
        return ret;
    }

    nvs_handle_t nvs_handle;

    /* Open NVS */
    ret = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    /* Save configuration blob */
    ret = nvs_set_blob(nvs_handle, "config", config, sizeof(gateway_config_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    /* Commit */
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Configuration saved to NVS");
    return ESP_OK;
}

/**
 * @brief Reset configuration to defaults
 */
esp_err_t config_manager_reset_to_defaults(void)
{
    ESP_LOGI(TAG, "Resetting configuration to defaults...");

    /* Load defaults */
    esp_err_t ret = config_manager_load_defaults(&s_current_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load defaults");
        return ret;
    }

    /* Erase NVS configuration */
    nvs_handle_t nvs_handle;
    ret = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret == ESP_OK) {
        esp_err_t erase_ret = nvs_erase_all(nvs_handle);
        if (erase_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to erase NVS: %s", esp_err_to_name(erase_ret));
        }
        esp_err_t commit_ret = nvs_commit(nvs_handle);
        if (commit_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to commit NVS: %s", esp_err_to_name(commit_ret));
        }
        nvs_close(nvs_handle);
        if (erase_ret == ESP_OK && commit_ret == ESP_OK) {
            ESP_LOGI(TAG, "NVS configuration erased");
        }
    }

    ESP_LOGI(TAG, "Configuration reset to defaults");
    return ESP_OK;
}

/**
 * @brief Initialize configuration manager
 */
esp_err_t config_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing configuration manager...");

    /* Try to load from NVS first */
    esp_err_t ret = config_manager_load(&s_current_config);

    if (ret == ESP_ERR_NOT_FOUND) {
        /* No config in NVS, load defaults */
        ESP_LOGI(TAG, "No configuration in NVS, loading defaults from Kconfig");
        ret = config_manager_load_defaults(&s_current_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load defaults");
            return ret;
        }

        /* Save defaults to NVS for future */
        ret = config_manager_save(&s_current_config);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save defaults to NVS (non-fatal)");
        }
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load configuration");
        return ret;
    }

    s_config_initialized = true;

    /* Log current configuration (without passwords) */
    ESP_LOGI(TAG, "Configuration loaded:");
    ESP_LOGI(TAG, "  WiFi SSID: %s", s_current_config.wifi_ssid);
    /* Only show port separately if URL doesn't already contain one */
    if (strstr(s_current_config.mqtt_broker_url, "://") != NULL &&
        strchr(strstr(s_current_config.mqtt_broker_url, "://") + 3, ':') != NULL) {
        /* URL already has port embedded */
        ESP_LOGI(TAG, "  MQTT Broker: %s", s_current_config.mqtt_broker_url);
    } else {
        ESP_LOGI(TAG, "  MQTT Broker: %s:%u",
                 s_current_config.mqtt_broker_url, s_current_config.mqtt_port);
    }
    ESP_LOGI(TAG, "  MQTT Client ID: %s", s_current_config.mqtt_client_id);
    ESP_LOGI(TAG, "  Zigbee PAN ID: 0x%04X", s_current_config.zigbee_pan_id);
    ESP_LOGI(TAG, "  Zigbee Channel: %u", s_current_config.zigbee_channel);
    ESP_LOGI(TAG, "  HA Discovery: %s",
             s_current_config.ha_discovery_enabled ? "enabled" : "disabled");

    ESP_LOGI(TAG, "Configuration manager initialized successfully");
    return ESP_OK;
}

/**
 * @brief Get current configuration
 */
const gateway_config_t* config_manager_get_config(void)
{
    if (!s_config_initialized) {
        ESP_LOGW(TAG, "Configuration manager not initialized");
        return NULL;
    }
    return &s_current_config;
}

/**
 * @brief Apply configuration changes
 */
esp_err_t config_manager_apply(const gateway_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate new configuration */
    esp_err_t ret = config_manager_validate(config);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Check if critical settings changed (require restart) */
    bool requires_restart = false;

    if (strcmp(s_current_config.wifi_ssid, config->wifi_ssid) != 0 ||
        strcmp(s_current_config.wifi_password, config->wifi_password) != 0) {
        ESP_LOGW(TAG, "WiFi settings changed - restart required");
        requires_restart = true;
    }

    if (s_current_config.zigbee_pan_id != config->zigbee_pan_id ||
        s_current_config.zigbee_channel != config->zigbee_channel) {
        ESP_LOGW(TAG, "Zigbee settings changed - restart required");
        requires_restart = true;
    }

    /* Update current configuration */
    memcpy(&s_current_config, config, sizeof(gateway_config_t));

    /* Save to NVS */
    ret = config_manager_save(&s_current_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save configuration");
        return ret;
    }

    if (requires_restart) {
        ESP_LOGW(TAG, "Configuration changes require system restart");
        return ESP_FAIL; /* Indicate restart needed */
    }

    ESP_LOGI(TAG, "Configuration applied successfully");
    return ESP_OK;
}

/**
 * @brief Export configuration as JSON
 */
esp_err_t config_manager_export_json(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_config_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Generate JSON (with masked passwords) */
    int written = snprintf(buffer, buffer_size,
        "{"
        "\"network\":{"
            "\"wifi_ssid\":\"%s\","
            "\"wifi_password\":\"***\""
        "},"
        "\"mqtt\":{"
            "\"broker_url\":\"%s\","
            "\"port\":%u,"
            "\"username\":\"%s\","
            "\"password\":\"***\","
            "\"client_id\":\"%s\","
            "\"keepalive\":%u,"
            "\"qos\":%u"
        "},"
        "\"zigbee\":{"
            "\"pan_id\":\"0x%04X\","
            "\"channel\":%u,"
            "\"max_children\":%u,"
            "\"permit_join_on_boot\":%s"
        "},"
        "\"gateway\":{"
            "\"device_publish_interval_ms\":%lu,"
            "\"ha_discovery_enabled\":%s,"
            "\"bridge_logging_enabled\":%s"
        "},"
        "\"system\":{"
            "\"log_level\":%u,"
            "\"ota_enabled\":%s,"
            "\"ota_url\":\"%s\""
        "}"
        "}",
        s_current_config.wifi_ssid,
        s_current_config.mqtt_broker_url, s_current_config.mqtt_port,
        s_current_config.mqtt_username, s_current_config.mqtt_client_id,
        s_current_config.mqtt_keepalive, s_current_config.mqtt_qos,
        s_current_config.zigbee_pan_id, s_current_config.zigbee_channel,
        s_current_config.zigbee_max_children,
        s_current_config.zigbee_permit_join_on_boot ? "true" : "false",
        s_current_config.device_publish_interval_ms,
        s_current_config.ha_discovery_enabled ? "true" : "false",
        s_current_config.bridge_logging_enabled ? "true" : "false",
        s_current_config.log_level,
        s_current_config.ota_enabled ? "true" : "false",
        s_current_config.ota_url
    );

    if (written < 0 || (size_t)written >= buffer_size) {
        ESP_LOGE(TAG, "JSON buffer too small (need %d bytes)", written);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Get configuration value by key
 */
esp_err_t config_manager_get(const char *key, void *value, size_t *len)
{
    if (key == NULL || value == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_config_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Map keys to config fields (simplified implementation) */
    if (strcmp(key, "wifi_ssid") == 0) {
        size_t size = strlen(s_current_config.wifi_ssid) + 1;
        if (*len < size) return ESP_ERR_INVALID_SIZE;
        /* Use memcpy with explicit length instead of strcpy */
        memcpy(value, s_current_config.wifi_ssid, size);
        *len = size;
        return ESP_OK;
    }

    /* Add more key mappings as needed... */

    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Set configuration value by key
 */
esp_err_t config_manager_set(const char *key, const void *value, size_t len)
{
    if (key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_config_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Map keys to config fields */
    if (strcmp(key, "wifi_ssid") == 0) {
        if (len >= CONFIG_SSID_MAX_LEN) return ESP_ERR_INVALID_SIZE;
        strncpy(s_current_config.wifi_ssid, (const char*)value, CONFIG_SSID_MAX_LEN - 1);
        return config_manager_save(&s_current_config);
    }

    /* Add more key mappings as needed... */

    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Import configuration from JSON
 */
esp_err_t config_manager_import_json(const char *json)
{
    /* NOTE: Full JSON parsing would require cJSON library
     * This is a placeholder for the interface.
     * In production, use cJSON to parse and update config fields.
     */
    ESP_LOGW(TAG, "JSON import not fully implemented - requires cJSON");
    return ESP_ERR_NOT_SUPPORTED;
}
