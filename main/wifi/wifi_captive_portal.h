/**
 * @file wifi_captive_portal.h
 * @brief WiFi Captive Portal for Initial Configuration
 *
 * Provides a captive portal (open AP + web interface) for WiFi configuration
 * when no credentials are available or connection fails. The portal:
 * - Starts an open AP: "ESP32-C5-Setup-XXXX" (last 4 hex of MAC)
 * - Runs a DNS redirect server for captive portal detection
 * - Serves a web configuration page via esp_http_server
 * - Tests credentials before saving to NVS
 * - Stops BLE/ESPHome during portal operation to free memory
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef WIFI_CAPTIVE_PORTAL_H
#define WIFI_CAPTIVE_PORTAL_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Captive Portal Constants
 * ============================================================================ */

/** @brief AP SSID prefix (MAC suffix appended automatically) */
#define CAPTIVE_PORTAL_AP_SSID_PREFIX   "ESP32-C5-Setup-"

/** @brief AP IP address */
#define CAPTIVE_PORTAL_AP_IP            "192.168.4.1"

/** @brief Maximum AP client connections */
#define CAPTIVE_PORTAL_AP_MAX_CONN      4

/** @brief DNS redirect task stack size (bytes, allocated in PSRAM) */
#define CAPTIVE_PORTAL_DNS_STACK_SIZE   3072

/** @brief DNS redirect task priority */
#define CAPTIVE_PORTAL_DNS_PRIORITY     4

/** @brief HTTP server stack size (bytes) */
#define CAPTIVE_PORTAL_HTTP_STACK_SIZE  4096  /* Reduced from 6144 */

/** @brief WiFi connection test timeout (ms) */
#define CAPTIVE_PORTAL_CONNECT_TIMEOUT_MS  15000

/** @brief WiFi scan maximum APs */
#define CAPTIVE_PORTAL_MAX_SCAN_APS     20

/** @brief Response buffer size for HTML generation (PSRAM, ~3KB HTML + ~3KB scan) */
#define CAPTIVE_PORTAL_RESPONSE_BUF_SIZE  8192

/**
 * @brief Captive portal states
 */
typedef enum {
    CAPTIVE_PORTAL_INACTIVE,    /*!< Portal not running */
    CAPTIVE_PORTAL_STARTING,    /*!< Portal initializing */
    CAPTIVE_PORTAL_ACTIVE,      /*!< Portal running, waiting for user */
    CAPTIVE_PORTAL_TESTING,     /*!< Testing submitted credentials */
    CAPTIVE_PORTAL_SUCCESS,     /*!< Credentials verified, saving */
    CAPTIVE_PORTAL_STOPPING,    /*!< Portal shutting down */
} captive_portal_state_t;

/**
 * @brief Subsystem callback for stopping/starting BLE and ESPHome
 *
 * Called when the portal starts (portal_active=true) and stops
 * (portal_active=false). The callback should stop BLE/ESPHome when
 * active and restart them when inactive.
 *
 * @param portal_active true when portal is starting, false when stopping
 */
typedef void (*captive_portal_subsystem_cb_t)(bool portal_active);

/**
 * @brief Start the captive portal
 *
 * Blocks until either:
 * - User successfully configures WiFi (credentials saved to NVS)
 * - Timeout expires (CONFIG_WIFI_CAPTIVE_PORTAL_TIMEOUT_SEC)
 *
 * The subsystem_cb is called to stop BLE/ESPHome before starting
 * and to restart them after stopping.
 *
 * @param subsystem_cb Callback to stop/start subsystems (may be NULL)
 *
 * @return
 *     - ESP_OK: WiFi configured successfully
 *     - ESP_ERR_TIMEOUT: Portal timed out without configuration
 *     - ESP_FAIL: Portal failed to start
 *     - ESP_ERR_NOT_SUPPORTED: Portal disabled in Kconfig
 */
esp_err_t captive_portal_start(captive_portal_subsystem_cb_t subsystem_cb);

/**
 * @brief Stop the captive portal
 *
 * Stops HTTP server, DNS redirect, and AP. Switches WiFi back to STA mode.
 * Called automatically on successful configuration or timeout.
 *
 * @return
 *     - ESP_OK: Portal stopped successfully
 *     - ESP_ERR_INVALID_STATE: Portal not running
 */
esp_err_t captive_portal_stop(void);

/**
 * @brief Get current portal state
 *
 * @return Current captive portal state
 */
captive_portal_state_t captive_portal_get_state(void);

/**
 * @brief Check if portal is active
 *
 * @return true if portal is running (ACTIVE or TESTING state)
 */
bool captive_portal_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CAPTIVE_PORTAL_H */
