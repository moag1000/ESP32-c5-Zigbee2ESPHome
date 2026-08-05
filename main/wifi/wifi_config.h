/**
 * @file wifi_config.h
 * @brief WiFi Configuration Helpers
 *
 * Provides functions for loading and saving WiFi configuration
 * from Kconfig and NVS storage.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include "esp_err.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS namespace for WiFi configuration */
#define WIFI_NVS_NAMESPACE "wifi_config"

/* NVS keys */
#define WIFI_NVS_KEY_SSID      "ssid"
#define WIFI_NVS_KEY_PASSWORD  "password"
#define WIFI_NVS_KEY_MAX_RETRY "max_retry"

/* ============================================================================
 * WiFi Validation Constants
 * ============================================================================ */

/** @brief Minimum password length for WPA/WPA2 (IEEE 802.11i) */
#define WIFI_PASSWORD_MIN_LEN               8

/** @brief Maximum allowed retry limit for connection attempts */
#define WIFI_MAX_RETRY_LIMIT                100

/**
 * @brief Load WiFi configuration from Kconfig
 *
 * Loads compile-time WiFi configuration from menuconfig settings.
 *
 * @param[out] config Pointer to wifi_manager_config_t structure to fill
 *
 * @return
 *     - ESP_OK: Configuration loaded successfully
 *     - ESP_ERR_INVALID_ARG: Invalid config pointer
 */
esp_err_t wifi_config_load_from_kconfig(wifi_manager_config_t *config);

/**
 * @brief Load WiFi configuration from NVS
 *
 * Loads runtime WiFi configuration from non-volatile storage.
 * Falls back to Kconfig if NVS data is not available.
 *
 * @param[out] config Pointer to wifi_manager_config_t structure to fill
 *
 * @return
 *     - ESP_OK: Configuration loaded from NVS
 *     - ESP_ERR_NVS_NOT_FOUND: NVS data not found, using Kconfig defaults
 *     - ESP_ERR_INVALID_ARG: Invalid config pointer
 *     - Other NVS error codes
 */
esp_err_t wifi_config_load_from_nvs(wifi_manager_config_t *config);

/**
 * @brief Save WiFi configuration to NVS
 *
 * Saves WiFi configuration to non-volatile storage for persistence
 * across reboots.
 *
 * @param[in] config Pointer to wifi_manager_config_t structure to save
 *
 * @return
 *     - ESP_OK: Configuration saved successfully
 *     - ESP_ERR_INVALID_ARG: Invalid config pointer
 *     - Other NVS error codes
 */
esp_err_t wifi_config_save_to_nvs(const wifi_manager_config_t *config);

/**
 * @brief Erase WiFi configuration from NVS
 *
 * Removes stored WiFi configuration from NVS.
 * Next load will fall back to Kconfig defaults.
 *
 * @return
 *     - ESP_OK: Configuration erased successfully
 *     - Other NVS error codes
 */
esp_err_t wifi_config_erase_nvs(void);

/**
 * @brief Validate WiFi configuration
 *
 * Checks if WiFi configuration is valid (non-empty SSID, etc.)
 *
 * @param[in] config Pointer to wifi_manager_config_t structure to validate
 *
 * @return
 *     - ESP_OK: Configuration is valid
 *     - ESP_ERR_INVALID_ARG: Configuration is invalid
 */
esp_err_t wifi_config_validate(const wifi_manager_config_t *config);

/**
 * @brief Store the ESPHome API encryption key entered in the captive portal
 *
 * Released firmware images carry no key — shipping one would mean every device
 * flashed from the same image shares it. Compiling your own just to set it
 * defeats the point of a pre-built image, so the portal takes it instead and
 * this puts it in NVS, where esphome_api_server.c looks first.
 *
 * @param[in] psk_base64 Base64 of 32 bytes, or NULL/"" to clear
 * @return ESP_OK, or an NVS error
 */
esp_err_t wifi_config_save_esphome_psk(const char *psk_base64);
/**
 * @brief Read the stored ESPHome API key
 *
 * @param[out] buf       Receives the Base64 key, empty string when unset
 * @param[in]  buf_size  Size of @p buf; 48 bytes is enough
 * @return ESP_OK even when nothing is stored — check for an empty string
 */
esp_err_t wifi_config_load_esphome_psk(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONFIG_H */