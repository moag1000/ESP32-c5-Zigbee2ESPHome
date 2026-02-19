/**
 * @file mock_zigbee.h
 * @brief Mock Zigbee Coordinator for Testing
 *
 * Provides a mock Zigbee coordinator that simulates Zigbee operations
 * without requiring actual Zigbee hardware.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MOCK_ZIGBEE_H
#define MOCK_ZIGBEE_H

#include "esp_err.h"
#include "esp_zigbee_core.h"
#include "zigbee/zb_device_handler.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mock Zigbee statistics
 */
typedef struct {
    uint32_t devices_joined;
    uint32_t devices_left;
    uint32_t commands_sent;
    uint32_t attributes_read;
    uint32_t attributes_written;
    bool network_formed;
    bool permit_join_enabled;
} mock_zigbee_stats_t;

/**
 * @brief Mock device event callback
 */
typedef void (*mock_zigbee_device_callback_t)(uint16_t short_addr, bool joined);

/**
 * @brief Mock attribute change callback
 */
typedef void (*mock_zigbee_attribute_callback_t)(uint16_t short_addr, uint16_t cluster_id,
                                                  uint16_t attr_id, const void *value, size_t len);

/**
 * @brief Initialize mock Zigbee coordinator
 *
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_init(void);

/**
 * @brief Deinitialize mock Zigbee coordinator
 *
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_deinit(void);

/**
 * @brief Simulate starting Zigbee network
 *
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_start_network(void);

/**
 * @brief Simulate stopping Zigbee network
 *
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_stop_network(void);

/**
 * @brief Check if network is formed
 *
 * @return true if network formed, false otherwise
 */
bool mock_zigbee_is_network_formed(void);

/**
 * @brief Simulate permit join
 *
 * @param[in] duration Duration in seconds (0 to disable)
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_permit_join(uint8_t duration);

/**
 * @brief Check if permit join is enabled
 *
 * @return true if enabled, false otherwise
 */
bool mock_zigbee_is_permit_join_enabled(void);

/**
 * @brief Simulate device join
 *
 * Creates a mock device and triggers join callback.
 *
 * @param[in] short_addr Device short address
 * @param[in] ieee_addr IEEE address
 * @param[in] device_type Device type
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_simulate_device_join(uint16_t short_addr,
                                           esp_zb_ieee_addr_t ieee_addr,
                                           zb_device_type_t device_type);

/**
 * @brief Simulate device leave
 *
 * Triggers device leave callback.
 *
 * @param[in] short_addr Device short address
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_simulate_device_leave(uint16_t short_addr);

/**
 * @brief Simulate attribute change
 *
 * Triggers attribute change callback.
 *
 * @param[in] short_addr Device short address
 * @param[in] cluster_id Cluster ID
 * @param[in] attr_id Attribute ID
 * @param[in] value Attribute value
 * @param[in] value_len Value length
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_simulate_attribute_change(uint16_t short_addr,
                                               uint16_t cluster_id,
                                               uint16_t attr_id,
                                               const void *value,
                                               size_t value_len);

/**
 * @brief Simulate sending command to device
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Endpoint number
 * @param[in] cluster_id Cluster ID
 * @param[in] cmd_id Command ID
 * @param[in] value Command value
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_send_command(uint16_t short_addr, uint8_t endpoint,
                                   uint16_t cluster_id, uint8_t cmd_id,
                                   uint8_t value);

/**
 * @brief Simulate reading attribute
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Endpoint number
 * @param[in] cluster_id Cluster ID
 * @param[in] attr_id Attribute ID
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_read_attribute(uint16_t short_addr, uint8_t endpoint,
                                     uint16_t cluster_id, uint16_t attr_id);

/**
 * @brief Get mock Zigbee statistics
 *
 * @return Statistics structure
 */
mock_zigbee_stats_t mock_zigbee_get_stats(void);

/**
 * @brief Reset mock Zigbee statistics
 */
void mock_zigbee_reset_stats(void);

/**
 * @brief Register device event callback
 *
 * @param[in] callback Callback function
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_register_device_callback(mock_zigbee_device_callback_t callback);

/**
 * @brief Register attribute change callback
 *
 * @param[in] callback Callback function
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_register_attribute_callback(mock_zigbee_attribute_callback_t callback);

/**
 * @brief Get coordinator IEEE address
 *
 * @param[out] ieee_addr IEEE address buffer (8 bytes)
 * @return ESP_OK on success
 */
esp_err_t mock_zigbee_get_coordinator_ieee(esp_zb_ieee_addr_t ieee_addr);

/**
 * @brief Get PAN ID
 *
 * @return PAN ID
 */
uint16_t mock_zigbee_get_pan_id(void);

/**
 * @brief Get channel
 *
 * @return Channel number
 */
uint8_t mock_zigbee_get_channel(void);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_ZIGBEE_H */
