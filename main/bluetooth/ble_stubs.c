/**
 * @file ble_stubs.c
 * @brief Stub implementations when Bluetooth is disabled
 *
 * Provides no-op implementations of BLE functions so that other modules
 * can compile without #if CONFIG_BT_ENABLED guards everywhere.
 */

#include "sdkconfig.h"

#if !CONFIG_BT_ENABLED

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* BLE Manager stubs */
bool ble_manager_is_initialized(void) { return false; }
const char *ble_manager_get_state_str(void) { return "disabled"; }
esp_err_t ble_manager_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ble_manager_init_with_config(void *config) { (void)config; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ble_manager_deinit(void) { return ESP_OK; }

/* BLE Scanner stubs */
bool ble_scanner_is_running(void) { return false; }
bool ble_scanner_is_active_enabled(void) { return false; }
int ble_scanner_get_device_count(void) { return 0; }
esp_err_t ble_scanner_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ble_scanner_stop(void) { return ESP_OK; }
int ble_scanner_get_state(void) { return 0; }
const char *ble_scanner_get_state_str(void) { return "disabled"; }
esp_err_t ble_scanner_register_state_callback(void *callback) { (void)callback; return ESP_OK; }

/* BLE GATT Client stubs */
esp_err_t ble_gatt_client_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ble_gatt_client_deinit(void) { return ESP_OK; }

/* ESPHome BLE Proxy stubs — signatures match esphome_ble_proxy.h */
typedef struct {
    uint32_t total_connections;
    uint32_t active_connections;
    uint32_t advertisements_forwarded;
    uint32_t gatt_operations;
} esphome_ble_proxy_stats_t;

typedef struct {
    uint8_t dummy;
} esphome_ble_proxy_config_t;

esp_err_t esphome_ble_proxy_init(const esphome_ble_proxy_config_t *config) {
    (void)config; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esphome_ble_proxy_deinit(void) { return ESP_OK; }
esp_err_t esphome_ble_proxy_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esphome_ble_proxy_stop(void) { return ESP_OK; }
bool esphome_ble_proxy_is_running(void) { return false; }
esp_err_t esphome_ble_proxy_get_stats(esphome_ble_proxy_stats_t *stats) {
    if (stats) {
        stats->total_connections = 0;
        stats->active_connections = 0;
        stats->advertisements_forwarded = 0;
        stats->gatt_operations = 0;
    }
    return ESP_OK;
}
void esphome_ble_proxy_reset_stats(void) { }
void esphome_ble_proxy_client_disconnected(int client_id) { (void)client_id; }
uint8_t esphome_ble_proxy_get_free_connections(void) { return 0; }
bool esphome_ble_proxy_is_device_connected(uint64_t address) { (void)address; return false; }

/* Message handlers — 3-param signatures matching esphome_ble_proxy.h */
esp_err_t esphome_ble_proxy_handle_subscribe_advertisements(
    uint8_t client_id, const uint8_t *payload, size_t len) {
    (void)client_id; (void)payload; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esphome_ble_proxy_handle_unsubscribe_advertisements(uint8_t client_id) {
    (void)client_id;
    return ESP_OK;
}

esp_err_t esphome_ble_proxy_handle_device_request(
    uint8_t client_id, const uint8_t *payload, size_t len) {
    (void)client_id; (void)payload; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esphome_ble_proxy_handle_connection_free(
    uint8_t client_id, const uint8_t *payload, size_t len) {
    (void)client_id; (void)payload; (void)len;
    return ESP_OK;
}

esp_err_t esphome_ble_proxy_handle_gatt_read(
    uint8_t client_id, const uint8_t *payload, size_t len) {
    (void)client_id; (void)payload; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esphome_ble_proxy_handle_gatt_write(
    uint8_t client_id, const uint8_t *payload, size_t len) {
    (void)client_id; (void)payload; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esphome_ble_proxy_handle_gatt_notify_request(
    uint8_t client_id, const uint8_t *payload, size_t len) {
    (void)client_id; (void)payload; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esphome_ble_proxy_handle_scanner_set_mode(
    uint8_t client_id, const uint8_t *payload, size_t len) {
    (void)client_id; (void)payload; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esphome_ble_proxy_send_scanner_state(uint8_t client_id) {
    (void)client_id;
    return ESP_ERR_NOT_SUPPORTED;
}

void esphome_ble_proxy_broadcast_scanner_state(void) { }

#endif /* !CONFIG_BT_ENABLED */
