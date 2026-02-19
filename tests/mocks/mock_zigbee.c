/**
 * @file mock_zigbee.c
 * @brief Mock Zigbee Coordinator Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "mock_zigbee.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "MOCK_ZIGBEE";

/* Mock state */
static struct {
    bool initialized;
    bool network_formed;
    bool permit_join_enabled;
    uint8_t permit_join_duration;
    esp_zb_ieee_addr_t coordinator_ieee;
    uint16_t pan_id;
    uint8_t channel;
    mock_zigbee_stats_t stats;
    mock_zigbee_device_callback_t device_callback;
    mock_zigbee_attribute_callback_t attribute_callback;
} g_mock = {0};

/**
 * @brief Initialize mock Zigbee
 */
esp_err_t mock_zigbee_init(void)
{
    if (g_mock.initialized) {
        ESP_LOGW(TAG, "Mock Zigbee already initialized");
        return ESP_OK;
    }

    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.initialized = true;

    /* Set default coordinator IEEE address */
    uint8_t default_ieee[8] = {0x00, 0x12, 0x4B, 0x00, 0x01, 0x23, 0x45, 0x67};
    memcpy(g_mock.coordinator_ieee, default_ieee, 8);

    /* Set default PAN ID and channel */
    g_mock.pan_id = 0x1234;
    g_mock.channel = 15;

    ESP_LOGI(TAG, "Mock Zigbee initialized");
    return ESP_OK;
}

/**
 * @brief Deinitialize mock Zigbee
 */
esp_err_t mock_zigbee_deinit(void)
{
    g_mock.initialized = false;
    g_mock.network_formed = false;
    g_mock.permit_join_enabled = false;
    g_mock.device_callback = NULL;
    g_mock.attribute_callback = NULL;

    ESP_LOGI(TAG, "Mock Zigbee deinitialized");
    return ESP_OK;
}

/**
 * @brief Start network
 */
esp_err_t mock_zigbee_start_network(void)
{
    if (!g_mock.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    g_mock.network_formed = true;
    g_mock.stats.network_formed = true;

    ESP_LOGI(TAG, "Mock Zigbee network started");
    return ESP_OK;
}

/**
 * @brief Stop network
 */
esp_err_t mock_zigbee_stop_network(void)
{
    g_mock.network_formed = false;
    g_mock.permit_join_enabled = false;
    g_mock.stats.network_formed = false;

    ESP_LOGI(TAG, "Mock Zigbee network stopped");
    return ESP_OK;
}

/**
 * @brief Check if network formed
 */
bool mock_zigbee_is_network_formed(void)
{
    return g_mock.network_formed;
}

/**
 * @brief Permit join
 */
esp_err_t mock_zigbee_permit_join(uint8_t duration)
{
    if (!g_mock.network_formed) {
        return ESP_ERR_INVALID_STATE;
    }

    g_mock.permit_join_enabled = (duration > 0);
    g_mock.permit_join_duration = duration;
    g_mock.stats.permit_join_enabled = g_mock.permit_join_enabled;

    ESP_LOGI(TAG, "Mock Zigbee permit join: %s (duration=%d)",
             g_mock.permit_join_enabled ? "enabled" : "disabled", duration);
    return ESP_OK;
}

/**
 * @brief Check permit join status
 */
bool mock_zigbee_is_permit_join_enabled(void)
{
    return g_mock.permit_join_enabled;
}

/**
 * @brief Simulate device join
 */
esp_err_t mock_zigbee_simulate_device_join(uint16_t short_addr,
                                           esp_zb_ieee_addr_t ieee_addr,
                                           zb_device_type_t device_type)
{
    if (!g_mock.network_formed) {
        return ESP_ERR_INVALID_STATE;
    }

    g_mock.stats.devices_joined++;

    /* Trigger callback */
    if (g_mock.device_callback) {
        g_mock.device_callback(short_addr, true);
    }

    ESP_LOGI(TAG, "Mock Zigbee device joined: 0x%04X", short_addr);
    return ESP_OK;
}

/**
 * @brief Simulate device leave
 */
esp_err_t mock_zigbee_simulate_device_leave(uint16_t short_addr)
{
    g_mock.stats.devices_left++;

    /* Trigger callback */
    if (g_mock.device_callback) {
        g_mock.device_callback(short_addr, false);
    }

    ESP_LOGI(TAG, "Mock Zigbee device left: 0x%04X", short_addr);
    return ESP_OK;
}

/**
 * @brief Simulate attribute change
 */
esp_err_t mock_zigbee_simulate_attribute_change(uint16_t short_addr,
                                               uint16_t cluster_id,
                                               uint16_t attr_id,
                                               const void *value,
                                               size_t value_len)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Trigger callback */
    if (g_mock.attribute_callback) {
        g_mock.attribute_callback(short_addr, cluster_id, attr_id, value, value_len);
    }

    ESP_LOGI(TAG, "Mock Zigbee attribute changed: addr=0x%04X cluster=0x%04X attr=0x%04X",
             short_addr, cluster_id, attr_id);
    return ESP_OK;
}

/**
 * @brief Send command
 */
esp_err_t mock_zigbee_send_command(uint16_t short_addr, uint8_t endpoint,
                                   uint16_t cluster_id, uint8_t cmd_id,
                                   uint8_t value)
{
    if (!g_mock.network_formed) {
        return ESP_ERR_INVALID_STATE;
    }

    g_mock.stats.commands_sent++;

    ESP_LOGI(TAG, "Mock Zigbee send command: addr=0x%04X ep=%d cluster=0x%04X cmd=%d val=%d",
             short_addr, endpoint, cluster_id, cmd_id, value);
    return ESP_OK;
}

/**
 * @brief Read attribute
 */
esp_err_t mock_zigbee_read_attribute(uint16_t short_addr, uint8_t endpoint,
                                     uint16_t cluster_id, uint16_t attr_id)
{
    if (!g_mock.network_formed) {
        return ESP_ERR_INVALID_STATE;
    }

    g_mock.stats.attributes_read++;

    ESP_LOGI(TAG, "Mock Zigbee read attribute: addr=0x%04X ep=%d cluster=0x%04X attr=0x%04X",
             short_addr, endpoint, cluster_id, attr_id);
    return ESP_OK;
}

/**
 * @brief Get statistics
 */
mock_zigbee_stats_t mock_zigbee_get_stats(void)
{
    return g_mock.stats;
}

/**
 * @brief Reset statistics
 */
void mock_zigbee_reset_stats(void)
{
    memset(&g_mock.stats, 0, sizeof(g_mock.stats));
    g_mock.stats.network_formed = g_mock.network_formed;
    g_mock.stats.permit_join_enabled = g_mock.permit_join_enabled;
}

/**
 * @brief Register device callback
 */
esp_err_t mock_zigbee_register_device_callback(mock_zigbee_device_callback_t callback)
{
    g_mock.device_callback = callback;
    return ESP_OK;
}

/**
 * @brief Register attribute callback
 */
esp_err_t mock_zigbee_register_attribute_callback(mock_zigbee_attribute_callback_t callback)
{
    g_mock.attribute_callback = callback;
    return ESP_OK;
}

/**
 * @brief Get coordinator IEEE address
 */
esp_err_t mock_zigbee_get_coordinator_ieee(esp_zb_ieee_addr_t ieee_addr)
{
    if (!ieee_addr) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(ieee_addr, g_mock.coordinator_ieee, 8);
    return ESP_OK;
}

/**
 * @brief Get PAN ID
 */
uint16_t mock_zigbee_get_pan_id(void)
{
    return g_mock.pan_id;
}

/**
 * @brief Get channel
 */
uint8_t mock_zigbee_get_channel(void)
{
    return g_mock.channel;
}
