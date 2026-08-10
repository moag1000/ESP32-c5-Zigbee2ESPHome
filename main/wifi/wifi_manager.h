/**
 * @file wifi_manager.h
 * @brief WiFi Connection Manager with Auto-Reconnect
 *
 * Manages WiFi connection lifecycle with automatic reconnection,
 * dual-band support, and connection monitoring.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * WiFi Manager Constants
 * ============================================================================ */

/** @brief Base delay for reconnection attempts (ms) */
#define WIFI_MGR_RECONNECT_BASE_DELAY_MS    1000

/** @brief Maximum delay for reconnection attempts (ms) */
#define WIFI_MGR_RECONNECT_MAX_DELAY_MS     30000

/** @brief Driver restarts the watchdog will try before rebooting instead
 *
 * Restarting the Wi-Fi driver is the cheap move and is tried first; a reboot is
 * the recovery this chip has actually been observed to respond to once it stops
 * finding its access point.
 *
 * Two, not three, and the difference is measured: across four outages the
 * driver restart ran eight times and recovered the link zero times — every one
 * of them ended at the reboot. Three strikes made every outage about fourteen
 * minutes, of which the first ten were spent on a step that has never worked.
 * Two halves that to roughly eight.
 *
 * Not one, because eight samples are enough to call the driver restart rarely
 * useful and not enough to call it useless, and it costs a few seconds against
 * a reboot that costs the Zigbee network a re-form.
 */
#define WIFI_MGR_WATCHDOG_MAX_STRIKES       2

/** @brief Mutex acquisition timeout for state operations (ms) */
#define WIFI_MGR_STATE_MUTEX_TIMEOUT_MS     100

/** @brief Minimum channel number for 5GHz band */
#define WIFI_MGR_5GHZ_CHANNEL_MIN           36

/** @brief Maximum SSID length */
#define WIFI_SSID_MAX_LEN                   32

/** @brief Maximum password length */
#define WIFI_PASSWORD_MAX_LEN               64

/** @brief IP address string length */
#define WIFI_IP_STRING_LEN                  16

/** @brief MAC address string length "XX:XX:XX:XX:XX:XX" */
#define WIFI_MAC_STRING_LEN                 18

/** @brief RSSI update interval in seconds */
#define WIFI_MGR_RSSI_UPDATE_INTERVAL_SEC   10

/**
 * @brief WiFi connection states
 */
typedef enum {
    WIFI_STATE_DISCONNECTED,   /*!< Not connected to any AP */
    WIFI_STATE_CONNECTING,     /*!< Attempting to connect */
    WIFI_STATE_CONNECTED,      /*!< Successfully connected and has IP */
    WIFI_STATE_FAILED,         /*!< Connection failed after max retries */
    WIFI_STATE_RECONNECTING    /*!< Attempting to reconnect after disconnect */
} wifi_state_t;

/**
 * @brief WiFi power saving modes
 *
 * Controls the trade-off between power consumption and latency.
 * Uses adaptive listen intervals based on DTIM (Delivery Traffic
 * Indication Message) periods.
 */
typedef enum {
    WIFI_POWER_MODE_PERFORMANCE,  /*!< Low latency, higher power (WIFI_PS_NONE) */
    WIFI_POWER_MODE_BALANCED,     /*!< Default balanced mode (WIFI_PS_MIN_MODEM) */
    WIFI_POWER_MODE_POWERSAVE     /*!< Lower power, higher latency (WIFI_PS_MAX_MODEM) */
} wifi_power_mode_t;

/**
 * @brief WiFi manager configuration structure
 */
typedef struct {
    char ssid[WIFI_SSID_MAX_LEN];      /*!< SSID of the target AP */
    char password[WIFI_PASSWORD_MAX_LEN]; /*!< Password for WPA/WPA2 */
    uint8_t max_retry;                  /*!< Maximum connection retry attempts */
    bool dual_band;                     /*!< 5GHz preferred via RSSI adjustment (CONFIG_WIFI_PREFER_5GHZ) */
} wifi_manager_config_t;

/**
 * @brief WiFi statistics structure
 */
typedef struct {
    uint32_t connect_count;    /*!< Total successful connections */
    uint32_t disconnect_count; /*!< Total disconnection events */
    uint32_t reconnect_count;  /*!< Total reconnection attempts */
    uint32_t fail_count;       /*!< Total connection failures */
    uint32_t uptime_seconds;   /*!< Current connection uptime */
    int8_t rssi;               /*!< Current RSSI in dBm (0 if not connected) */
    uint8_t signal_quality;    /*!< Signal quality percentage (0-100%) */
    uint8_t last_disconnect_reason; /*!< esp_wifi reason code of the last disconnect (0 = none yet) */
} wifi_stats_t;

/**
 * @brief Initialize WiFi manager
 *
 * Initializes WiFi subsystem, creates event handlers, and prepares
 * for connection. Must be called before any other WiFi operations.
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_NO_MEM: Out of memory
 *     - ESP_ERR_WIFI_NOT_INIT: WiFi not initialized
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Connect to WiFi access point
 *
 * Initiates connection to specified WiFi network. If CONFIG_WIFI_PREFER_5GHZ
 * is enabled, 5GHz networks are preferred via RSSI adjustment to minimize
 * interference with Zigbee (which operates on 2.4GHz).
 *
 * @param[in] ssid WiFi SSID (null-terminated string)
 * @param[in] password WiFi password (null-terminated string)
 *
 * @return
 *     - ESP_OK: Connection initiated successfully
 *     - ESP_ERR_INVALID_ARG: Invalid SSID or password
 *     - ESP_ERR_WIFI_NOT_INIT: WiFi not initialized
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Disconnect from WiFi
 *
 * Gracefully disconnects from current WiFi network.
 * Disables auto-reconnect until connect is called again.
 *
 * @return
 *     - ESP_OK: Disconnected successfully
 *     - ESP_ERR_WIFI_NOT_INIT: WiFi not initialized
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Get current WiFi connection state
 *
 * Thread-safe retrieval of current WiFi state.
 *
 * @return Current WiFi state
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * @brief Check if WiFi is connected
 *
 * Convenience function to check if WiFi is in CONNECTED state.
 *
 * @return
 *     - true: WiFi is connected
 *     - false: WiFi is not connected
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Whether this SSID has ever authenticated this device
 *
 * Recorded in NVS on the first successful association. Lets the boot path tell
 * "the credentials are wrong" apart from "the access point is being slow",
 * which a timeout cannot: measured association on this gateway ranged from 7s
 * to over 450s with correct credentials throughout.
 *
 * @param[in] ssid SSID to check
 * @return true if @p ssid has previously connected successfully
 */
bool wifi_manager_credentials_known_good(const char *ssid);

/**
 * @brief Get current IP address
 *
 * Retrieves the current IP address as a string.
 *
 * @param[out] ip_str Buffer to store IP address string
 * @param[in] len Length of the buffer (minimum WIFI_IP_STRING_LEN bytes for IPv4)
 *
 * @return
 *     - ESP_OK: IP address retrieved successfully
 *     - ESP_ERR_INVALID_ARG: Invalid buffer or length
 *     - ESP_ERR_WIFI_NOT_CONNECT: Not connected to WiFi
 */
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len);

/**
 * @brief Get WiFi signal strength (RSSI)
 *
 * Returns the cached RSSI value, updated periodically every
 * WIFI_MGR_RSSI_UPDATE_INTERVAL_SEC seconds.
 *
 * @return
 *     - RSSI value in dBm (negative value, e.g., -50)
 *     - 0: Not connected or error
 */
int8_t wifi_manager_get_rssi(void);

/**
 * @brief Get WiFi signal quality as percentage
 *
 * Returns the cached signal quality value (0-100%), calculated from RSSI.
 * Updated periodically every WIFI_MGR_RSSI_UPDATE_INTERVAL_SEC seconds.
 *
 * Quality is calculated as: 2 * (rssi + 100), clamped to 0-100.
 * This gives:
 *   - -50 dBm = 100% (excellent)
 *   - -60 dBm = 80% (good)
 *   - -70 dBm = 60% (fair)
 *   - -80 dBm = 40% (weak)
 *   - -90 dBm = 20% (very weak)
 *   - -100 dBm = 0% (unusable)
 *
 * @return Signal quality percentage (0-100), or 0 if not connected
 */
uint8_t wifi_manager_get_signal_quality(void);

/**
 * @brief Update RSSI and signal quality values
 *
 * Queries the WiFi driver for current AP info and updates cached RSSI
 * and signal quality values. Called automatically from the uptime timer.
 *
 * Can also be called manually to force an immediate update.
 *
 * @note Thread-safe; uses mutex protection for cached values.
 */
void wifi_manager_update_rssi(void);

/**
 * @brief Get WiFi statistics
 *
 * Retrieves connection statistics including connect/disconnect counts
 * and uptime.
 *
 * @param[out] stats Pointer to statistics structure to fill
 *
 * @return
 *     - ESP_OK: Statistics retrieved successfully
 *     - ESP_ERR_INVALID_ARG: Invalid stats pointer
 */
esp_err_t wifi_manager_get_stats(wifi_stats_t *stats);

/**
 * @brief Human-readable name for an esp_wifi disconnect reason
 *
 * Covers the codes this gateway actually meets. Anything else comes back as
 * "reason <n>" rather than a guess — a wrong name is worse than a number when
 * someone is trying to work out why their gateway keeps vanishing.
 *
 * @param[in] reason esp_wifi reason code
 * @return Static string, never NULL
 */
const char *wifi_manager_disconnect_reason_str(uint8_t reason);

/**
 * @brief Enable or disable auto-reconnect
 *
 * Controls automatic reconnection behavior on disconnect.
 *
 * @param[in] enable true to enable auto-reconnect, false to disable
 *
 * @return
 *     - ESP_OK: Setting applied successfully
 */
esp_err_t wifi_manager_set_auto_reconnect(bool enable);

/**
 * @brief Reconnect to WiFi using stored credentials
 *
 * Attempts to reconnect to WiFi using the previously configured SSID
 * and password. This function can be used after a full WiFi stack reset
 * or when the connection has been lost and needs to be re-established.
 *
 * @return
 *     - ESP_OK: Reconnection initiated successfully
 *     - ESP_ERR_WIFI_NOT_INIT: WiFi not initialized
 *     - ESP_ERR_INVALID_STATE: No stored credentials
 */
esp_err_t wifi_manager_reconnect(void);

/**
 * @brief Reset the WiFi retry counter
 *
 * Resets the internal retry counter to zero. This is useful after
 * transitioning out of a special mode (like Zigbee PAIRING) where
 * WiFi retries may have been exhausted due to radio contention.
 */
void wifi_manager_reset_retry_count(void);

/**
 * @brief Set WiFi power saving mode
 *
 * Adjusts WiFi power saving behavior to optimize for either
 * performance (low latency) or power consumption.
 *
 * @param[in] mode Power saving mode to set
 *
 * @return
 *     - ESP_OK: Power mode set successfully
 *     - ESP_ERR_WIFI_NOT_INIT: WiFi not initialized
 *     - ESP_ERR_INVALID_ARG: Invalid power mode
 */
esp_err_t wifi_manager_set_power_mode(wifi_power_mode_t mode);

/**
 * @brief Get WiFi event group handle
 *
 * Returns the internal event group handle for waiting on WiFi events.
 * WIFI_CONNECTED_BIT (BIT0) is set when connected with IP.
 *
 * @return Event group handle, or NULL if not initialized
 */
EventGroupHandle_t wifi_manager_get_event_group(void);

/**
 * @brief Get mDNS hostname
 *
 * Returns the configured mDNS hostname (without .local suffix).
 * The hostname is set via CONFIG_GATEWAY_MDNS_HOSTNAME.
 *
 * @return Pointer to hostname string (static)
 */
const char *wifi_manager_get_hostname(void);

/**
 * @brief Test WiFi manager functionality
 *
 * Performs basic connectivity test and reports results.
 * For debugging and verification purposes.
 *
 * @return
 *     - ESP_OK: Test passed
 *     - ESP_FAIL: Test failed
 */
esp_err_t wifi_manager_test(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
