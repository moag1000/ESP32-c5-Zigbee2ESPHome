/**
 * @file adaptive_memory.c
 * @brief Adaptive Memory Response Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "adaptive_memory.h"
#include "memory_manager.h"
#include "esp_log.h"

/* Optional includes - check if modules are available */
#if CONFIG_BT_SCANNER_ENABLED
#include "bluetooth/ble_scanner.h"
#endif

#if CONFIG_ENABLE_HOMEASSISTANT_DISCOVERY
#include "core/discovery/ha_discovery_ng.h"
#endif

static const char *TAG = "ADAPTIVE_MEM";

/* Current adaptive mode */
static memory_status_t s_current_mode = MEMORY_STATUS_NORMAL;

/* Track if we've modified system settings */
static bool s_ble_interval_modified = false;
static bool s_ha_discovery_paused = false;

/**
 * @brief Apply WARNING mode adjustments
 */
static void apply_warning_mode(void)
{
    ESP_LOGW(TAG, "Entering WARNING mode - reducing resource usage");

#if CONFIG_BT_SCANNER_ENABLED
    /* Reduce BLE scan frequency to conserve memory */
    if (!s_ble_interval_modified) {
        esp_err_t ret = ble_scanner_set_interval(ADAPTIVE_BLE_SCAN_WARNING_MS);
        if (ret == ESP_OK) {
            s_ble_interval_modified = true;
            ESP_LOGI(TAG, "BLE scan interval increased to %d ms", ADAPTIVE_BLE_SCAN_WARNING_MS);
        }
    }
#endif

    /* Trigger standard optimization */
    memory_manager_optimize();
}

/**
 * @brief Apply CRITICAL mode adjustments
 */
static void apply_critical_mode(void)
{
    ESP_LOGE(TAG, "Entering CRITICAL mode - aggressive resource reduction");

    /* First apply warning adjustments */
    apply_warning_mode();

#if CONFIG_ENABLE_HOMEASSISTANT_DISCOVERY
    /* Pause HA discovery to prevent new allocations */
    if (!s_ha_discovery_paused) {
        esp_err_t ret = ha_discovery_ng_set_enabled(false);
        if (ret == ESP_OK) {
            s_ha_discovery_paused = true;
            ESP_LOGW(TAG, "HA discovery paused due to critical memory");
        }
    }
#endif

    /* Trigger aggressive optimization */
    memory_manager_optimize_aggressive();
}

/**
 * @brief Restore NORMAL mode settings
 */
static void apply_normal_mode(void)
{
    ESP_LOGI(TAG, "Entering NORMAL mode - restoring normal operation");

#if CONFIG_BT_SCANNER_ENABLED
    /* Restore normal BLE scan interval */
    if (s_ble_interval_modified) {
        esp_err_t ret = ble_scanner_set_interval(ADAPTIVE_BLE_SCAN_NORMAL_MS);
        if (ret == ESP_OK) {
            s_ble_interval_modified = false;
            ESP_LOGI(TAG, "BLE scan interval restored to %d ms", ADAPTIVE_BLE_SCAN_NORMAL_MS);
        }
    }
#endif

#if CONFIG_ENABLE_HOMEASSISTANT_DISCOVERY
    /* Resume HA discovery */
    if (s_ha_discovery_paused) {
        esp_err_t ret = ha_discovery_ng_set_enabled(true);
        if (ret == ESP_OK) {
            s_ha_discovery_paused = false;
            ESP_LOGI(TAG, "HA discovery resumed");
        }
    }
#endif
}

/**
 * @brief Memory status change callback
 */
void adaptive_memory_on_status_change(memory_status_t status)
{
    if (status == s_current_mode) {
        return;  /* No change */
    }

    ESP_LOGI(TAG, "Memory status changed: %d -> %d", s_current_mode, status);
    s_current_mode = status;

    switch (status) {
        case MEMORY_STATUS_NORMAL:
            apply_normal_mode();
            break;

        case MEMORY_STATUS_WARNING:
            apply_warning_mode();
            break;

        case MEMORY_STATUS_CRITICAL:
            apply_critical_mode();
            break;

        default:
            ESP_LOGW(TAG, "Unknown memory status: %d", status);
            break;
    }
}

esp_err_t adaptive_memory_init(void)
{
    ESP_LOGI(TAG, "Initializing adaptive memory management");

    /* Register callback with memory manager */
    esp_err_t ret = memory_manager_register_callback(adaptive_memory_on_status_change);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register memory callback: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initialize to current status */
    s_current_mode = memory_manager_get_status();

    ESP_LOGI(TAG, "Adaptive memory management initialized (current mode: %d)", s_current_mode);
    return ESP_OK;
}

memory_status_t adaptive_memory_get_mode(void)
{
    return s_current_mode;
}

esp_err_t adaptive_memory_restore_normal(void)
{
    ESP_LOGI(TAG, "Forcing restoration of normal operation");

    s_current_mode = MEMORY_STATUS_NORMAL;
    apply_normal_mode();

    return ESP_OK;
}
