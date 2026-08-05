/**
 * @file wifi_config.c
 * @brief WiFi Configuration Helpers Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "wifi_config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

/* Log tag */
static const char *TAG = "WIFI_CFG";

/**
 * @brief Load WiFi configuration from Kconfig
 */
esp_err_t wifi_config_load_from_kconfig(wifi_manager_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Loading WiFi configuration from Kconfig...");

    memset(config, 0, sizeof(wifi_manager_config_t));

    /* Load from Kconfig */
    strlcpy(config->ssid, CONFIG_WIFI_SSID, sizeof(config->ssid));
    strlcpy(config->password, CONFIG_WIFI_PASSWORD, sizeof(config->password));
    config->max_retry = CONFIG_WIFI_MAXIMUM_RETRY;
    config->dual_band = false; /* Future enhancement */

    ESP_LOGI(TAG, "Loaded from Kconfig: SSID=%s, max_retry=%d",
            config->ssid, config->max_retry);

    return ESP_OK;
}

/**
 * @brief Load WiFi configuration from NVS
 */
esp_err_t wifi_config_load_from_nvs(wifi_manager_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Loading WiFi configuration from NVS...");

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS namespace not found, using Kconfig defaults");
            return wifi_config_load_from_kconfig(config);
        }
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    memset(config, 0, sizeof(wifi_manager_config_t));

    /* Read SSID */
    size_t ssid_len = sizeof(config->ssid);
    ret = nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, config->ssid, &ssid_len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read SSID from NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return wifi_config_load_from_kconfig(config);
    }

    /* Read password */
    size_t password_len = sizeof(config->password);
    ret = nvs_get_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, config->password, &password_len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read password from NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return wifi_config_load_from_kconfig(config);
    }

    /* Read max_retry */
    uint8_t max_retry;
    ret = nvs_get_u8(nvs_handle, WIFI_NVS_KEY_MAX_RETRY, &max_retry);
    if (ret == ESP_OK) {
        config->max_retry = max_retry;
    } else {
        config->max_retry = CONFIG_WIFI_MAXIMUM_RETRY;
    }

    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Loaded from NVS: SSID=%s, max_retry=%d",
            config->ssid, config->max_retry);

    return ESP_OK;
}

/**
 * @brief Save WiFi configuration to NVS
 */
esp_err_t wifi_config_save_to_nvs(const wifi_manager_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate configuration */
    esp_err_t ret = wifi_config_validate(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Invalid WiFi configuration");
        return ret;
    }

    ESP_LOGI(TAG, "Saving WiFi configuration to NVS...");

    nvs_handle_t nvs_handle;
    ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Write SSID */
    ret = nvs_set_str(nvs_handle, WIFI_NVS_KEY_SSID, config->ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write SSID to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    /* Write password */
    ret = nvs_set_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, config->password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write password to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    /* Write max_retry */
    ret = nvs_set_u8(nvs_handle, WIFI_NVS_KEY_MAX_RETRY, config->max_retry);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write max_retry to NVS: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    /* Commit changes */
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi configuration saved to NVS successfully");
    return ESP_OK;
}

/**
 * @brief Erase WiFi configuration from NVS
 */
esp_err_t wifi_config_erase_nvs(void)
{
    ESP_LOGI(TAG, "Erasing WiFi configuration from NVS...");

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Erase all keys in namespace */
    ret = nvs_erase_all(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS namespace: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    /* Commit changes */
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi configuration erased from NVS successfully");
    return ESP_OK;
}

/**
 * @brief Validate WiFi configuration
 */
esp_err_t wifi_config_validate(const wifi_manager_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check SSID */
    if (strlen(config->ssid) == 0) {
        ESP_LOGE(TAG, "SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    /* Check SSID length */
    if (strlen(config->ssid) > (WIFI_SSID_MAX_LEN - 1)) {
        ESP_LOGE(TAG, "SSID too long (max %d characters)", WIFI_SSID_MAX_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    /* Check password length */
    size_t pass_len = strlen(config->password);
    if (pass_len > 0 && (pass_len < WIFI_PASSWORD_MIN_LEN || pass_len > (WIFI_PASSWORD_MAX_LEN - 1))) {
        ESP_LOGE(TAG, "Password must be %d-%d characters or empty",
                 WIFI_PASSWORD_MIN_LEN, WIFI_PASSWORD_MAX_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    /* Check max_retry */
    if (config->max_retry == 0 || config->max_retry > WIFI_MAX_RETRY_LIMIT) {
        ESP_LOGE(TAG, "Invalid max_retry value (must be 1-%d)", WIFI_MAX_RETRY_LIMIT);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/* ============================================================================
 * ESPHome API key
 * ============================================================================ */

#define ESPHOME_PSK_NVS_KEY "esph_psk"

esp_err_t wifi_config_save_esphome_psk(const char *psk_base64)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }

    if (psk_base64 == NULL || psk_base64[0] == '\0') {
        ret = nvs_erase_key(h, ESPHOME_PSK_NVS_KEY);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;               /* Nothing stored is not a failure */
        }
    } else {
        ret = nvs_set_str(h, ESPHOME_PSK_NVS_KEY, psk_base64);
    }

    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

esp_err_t wifi_config_load_esphome_psk(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;                  /* No namespace yet — simply unset */
    }

    size_t len = buf_size;
    esp_err_t ret = nvs_get_str(h, ESPHOME_PSK_NVS_KEY, buf, &len);
    nvs_close(h);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        return ESP_OK;
    }
    return ret;
}
