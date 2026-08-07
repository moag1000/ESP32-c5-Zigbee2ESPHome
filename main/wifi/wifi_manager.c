/**
 * @file wifi_manager.c
 * @brief WiFi Connection Manager Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "wifi_manager.h"
#include "wifi_config.h"
#include "core/memory/memory_manager_ng.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "utils/version.h"

/* Event Bus for WiFi connectivity events */
#include "core/events/event_bus.h"

/* LED status integration */
#if CONFIG_GW_LED_ENABLED
#include "core/led_status_manager.h"
#endif

/* Log tag */
static const char *TAG = "WIFI_MGR";

/* WiFi event bits */
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1
#define WIFI_DISCONNECTED_BIT BIT2

/* Mutex timeout for timer callbacks (shorter for esp_timer context) */
#define WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS     50

/* Mutex timeout for event handlers */
#define WIFI_MGR_EVENT_MUTEX_TIMEOUT_MS     100

/* Listen interval constants (in DTIM periods) for power management */
#define WIFI_LISTEN_INTERVAL_NORMAL   3   /**< Balanced mode - reasonable power/latency tradeoff */
#define WIFI_LISTEN_INTERVAL_POWER    10  /**< Power save mode - lower power, higher latency */
#define WIFI_LISTEN_INTERVAL_PERF     1   /**< Performance mode - lowest latency, higher power */

#ifdef CONFIG_WIFI_AUTO_BAND_SWITCH
/* Band switching constants */
#define WIFI_RSSI_POOR_THRESHOLD        (-85)   /**< dBm - consider switching bands (relaxed for Zigbee coex) */
#define WIFI_RSSI_CHECK_COUNT           6       /**< Consecutive poor readings before switch (60s at 10s interval) */
#define WIFI_BAND_SWITCH_COOLDOWN_SEC   600     /**< Seconds between band switches (10 minutes) */
#define WIFI_RSSI_MONITOR_INTERVAL_US   (10 * 1000 * 1000)  /**< RSSI check interval (10 seconds) */

/**
 * @brief WiFi frequency band enumeration (local, avoids ESP-IDF conflict)
 */
typedef enum {
    WIFI_MGR_BAND_2_4GHZ,   /**< 2.4 GHz band (channels 1-14) */
    WIFI_MGR_BAND_5GHZ      /**< 5 GHz band (channels 36+) */
} wifi_mgr_band_t;
#endif /* CONFIG_WIFI_AUTO_BAND_SWITCH */

/* Consecutive reconnect failures before switching to AUTO band mode */
#define WIFI_RECONNECT_FAIL_THRESHOLD   3

/* WiFi manager state */
static struct {
    wifi_state_t state;
    wifi_manager_config_t config;
    wifi_stats_t stats;
    EventGroupHandle_t event_group;
    SemaphoreHandle_t state_mutex;
    esp_netif_t *netif;
    esp_timer_handle_t uptime_timer;
    esp_timer_handle_t reconnect_timer;
    uint8_t retry_count;
    uint32_t reconnect_delay_ms;
    bool auto_reconnect;
    bool initialized;
    esp_ip4_addr_t ip_addr;
    int8_t current_rssi;         /**< Cached RSSI value (updated periodically) */
    uint8_t signal_quality;      /**< Signal quality percentage 0-100% */
    bool mdns_initialized;       /**< mDNS service initialized flag */
    bool sntp_initialized;       /**< SNTP service initialized flag */
    uint8_t consecutive_failures; /**< Consecutive reconnect failures (for band fallback) */
    bool band_fallback_active;   /**< True if AUTO band mode fallback is active */
    esp_timer_handle_t watchdog_timer; /**< Safety watchdog: full WiFi restart if disconnected too long */
#ifdef CONFIG_WIFI_AUTO_BAND_SWITCH
    esp_timer_handle_t rssi_monitor_timer;  /**< Timer for RSSI monitoring */
    uint8_t poor_rssi_count;                /**< Consecutive poor RSSI readings */
    uint32_t last_band_switch_time;         /**< Timestamp of last band switch (uptime seconds) */
    wifi_mgr_band_t current_band;               /**< Current WiFi band */
    uint32_t band_switch_count;             /**< Total band switches performed */
#endif
} s_wifi = {
    .state = WIFI_STATE_DISCONNECTED,
    .auto_reconnect = true,
    .initialized = false,
    .reconnect_delay_ms = WIFI_MGR_RECONNECT_BASE_DELAY_MS,
    .consecutive_failures = 0,
    .band_fallback_active = false,
#ifdef CONFIG_WIFI_AUTO_BAND_SWITCH
    .poor_rssi_count = 0,
    .last_band_switch_time = 0,
    .current_band = WIFI_MGR_BAND_2_4GHZ,
    .band_switch_count = 0,
#endif
};

/* Forward declarations */
/* ============================================================================
 * "These credentials have worked before"
 *
 * The captive portal exists to obtain credentials, not to wait out a slow
 * access point. Measured on this gateway, association is wildly variable —
 * 7s on one boot, 356-372s on several, and not at all within 450s on another —
 * while the credentials were correct the whole time. A fixed grace period
 * cannot separate those cases, so it is the wrong question. Whether the SSID
 * has ever authenticated us is the right one.
 * ============================================================================ */

#define WIFI_KNOWN_GOOD_NVS_NS  "wifi_mgr"
#define WIFI_KNOWN_GOOD_NVS_KEY "ssid_ok"

bool wifi_manager_credentials_known_good(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(WIFI_KNOWN_GOOD_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    char stored[WIFI_SSID_MAX_LEN + 1] = {0};
    size_t len = sizeof(stored);
    esp_err_t ret = nvs_get_str(h, WIFI_KNOWN_GOOD_NVS_KEY, stored, &len);
    nvs_close(h);

    return (ret == ESP_OK) && (strcmp(stored, ssid) == 0);
}

/** @brief Record that @p ssid authenticated us. Best effort. */
static void mark_credentials_known_good(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0' || wifi_manager_credentials_known_good(ssid)) {
        return;                  /* Unchanged — do not wear the flash */
    }

    nvs_handle_t h;
    if (nvs_open(WIFI_KNOWN_GOOD_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, WIFI_KNOWN_GOOD_NVS_KEY, ssid);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Credentials for '%s' recorded as known-good", ssid);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
static void set_state(wifi_state_t new_state);
static void reconnect_timer_callback(void *arg);
static void wifi_watchdog_callback(void *arg);
static void schedule_reconnect(uint32_t delay_ms);
static void uptime_timer_callback(void *arg);
static esp_err_t wifi_manager_setup_mdns(void);
static void wifi_manager_cleanup_mdns(void);
static esp_err_t wifi_manager_setup_sntp(void);
#ifdef CONFIG_WIFI_AUTO_BAND_SWITCH
static void rssi_monitor_timer_callback(void *arg);
static void wifi_manager_check_band_switch(int8_t rssi);
static esp_err_t wifi_manager_switch_band(void);
static wifi_mgr_band_t wifi_manager_get_current_band(void);
#endif

/**
 * @brief Set WiFi state (thread-safe)
 */
static void set_state(wifi_state_t new_state)
{
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (s_wifi.state != new_state) {
            ESP_LOGI(TAG, "State: %d -> %d", s_wifi.state, new_state);
            s_wifi.state = new_state;
        }
        xSemaphoreGive(s_wifi.state_mutex);
    }
}

/**
 * @brief Uptime timer callback (increments uptime counter)
 *
 * Uses short timeout since this runs in esp_timer context.
 * If mutex cannot be acquired, skip this increment (non-critical).
 * Also triggers RSSI update every WIFI_MGR_RSSI_UPDATE_INTERVAL_SEC seconds.
 */
static void uptime_timer_callback(void *arg)
{
    if (s_wifi.state_mutex == NULL) {
        return;
    }

    uint32_t uptime = 0;

    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (s_wifi.state == WIFI_STATE_CONNECTED) {
            s_wifi.stats.uptime_seconds++;
            uptime = s_wifi.stats.uptime_seconds;
        }
        xSemaphoreGive(s_wifi.state_mutex);
    }
    /* If mutex not acquired, skip this increment - uptime is non-critical */

    /* Update RSSI every WIFI_MGR_RSSI_UPDATE_INTERVAL_SEC seconds */
    if (uptime > 0 && (uptime % WIFI_MGR_RSSI_UPDATE_INTERVAL_SEC) == 0) {
        wifi_manager_update_rssi();
    }
}

/**
 * @brief Safe helper to schedule a reconnect timer.
 * Always stops the timer first to avoid ESP_ERR_INVALID_STATE.
 */
static void schedule_reconnect(uint32_t delay_ms)
{
    if (!s_wifi.reconnect_timer) return;
    esp_timer_stop(s_wifi.reconnect_timer);  /* safe even if not running */
    esp_err_t ret = esp_timer_start_once(s_wifi.reconnect_timer, (uint64_t)delay_ms * 1000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start reconnect timer: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief WiFi watchdog — full WiFi restart if disconnected for too long.
 * Fires 5 minutes after disconnect. Stops+starts WiFi driver to reset state.
 */
/** Consecutive watchdog firings without a successful connection in between */
static uint8_t s_watchdog_strikes = 0;

static void wifi_watchdog_callback(void *arg)
{
    (void)arg;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        /* Already connected — nothing to do */
        s_watchdog_strikes = 0;
        return;
    }

    s_watchdog_strikes++;

    /* Escalate to a reboot when restarting the driver keeps not helping.
     *
     * The driver restart below is the right first move but it is not proven to
     * recover this chip from the state observed on hardware: reason 201,
     * NO_AP_FOUND, repeated for nine hours while the access point sat there at
     * -50 dBm. What is proven is that a reboot recovers it — the same device
     * associated 25 seconds after one.
     *
     * Three strikes is roughly fifteen minutes offline. By then the gateway is
     * useless to Home Assistant anyway, and a reboot is cheap now that device
     * state survives one. Deliberately not sooner: an access point that is
     * simply switched off overnight should not cost a reboot every five
     * minutes, and this project has had a restart loop before. */
    if (s_watchdog_strikes >= WIFI_MGR_WATCHDOG_MAX_STRIKES) {
        ESP_LOGE(TAG, "WiFi watchdog: %u driver restarts did not help — rebooting",
                 s_watchdog_strikes);
        vTaskDelay(pdMS_TO_TICKS(200));  /* let the log drain */
        esp_restart();
    }

    ESP_LOGW(TAG, "WiFi watchdog: still disconnected after 5 min (strike %u/%u) — "
             "full WiFi restart", s_watchdog_strikes, WIFI_MGR_WATCHDOG_MAX_STRIKES);

    /* Full WiFi restart: disconnect → stop → start → connect */
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t ret = esp_wifi_connect();
    if (ret == ESP_OK) {
        set_state(WIFI_STATE_RECONNECTING);
    } else {
        ESP_LOGE(TAG, "WiFi watchdog: connect failed: %s — will retry via timer", esp_err_to_name(ret));
    }

    /* Reset counters so reconnect logic starts fresh */
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_wifi.retry_count = 0;
        s_wifi.consecutive_failures = 0;
        s_wifi.reconnect_delay_ms = WIFI_MGR_RECONNECT_BASE_DELAY_MS;
        s_wifi.band_fallback_active = false;
        s_wifi.auto_reconnect = true;
        xSemaphoreGive(s_wifi.state_mutex);
    }

    /* Schedule another watchdog in case this restart also fails. Restarting the
     * timer is correct here — this callback only runs when the previous one
     * already expired, so there is nothing to reset away. */
    if (s_wifi.watchdog_timer) {
        esp_timer_stop(s_wifi.watchdog_timer);
        esp_timer_start_once(s_wifi.watchdog_timer, 5ULL * 60 * 1000000);
    }
}

/**
 * @brief Reconnection timer callback
 *
 * Uses short timeout since this runs in esp_timer context.
 * If mutex cannot be acquired, logs warning and skips this attempt.
 *
 * After WIFI_RECONNECT_FAIL_THRESHOLD consecutive failures, switches
 * to AUTO band mode to improve chances of finding the AP.
 */
static void reconnect_timer_callback(void *arg)
{
    ESP_LOGI(TAG, "Reconnection timer expired, attempting reconnect...");

    if (s_wifi.state_mutex == NULL) {
        ESP_LOGW(TAG, "State mutex not initialized, skipping reconnect");
        return;
    }

    /* Check if already connected before attempting reconnect */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "Already connected to AP, skipping reconnect");
        /* Reset reconnect delay and failure counter since we're connected */
        if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            s_wifi.reconnect_delay_ms = WIFI_MGR_RECONNECT_BASE_DELAY_MS;
            s_wifi.retry_count = 0;
            s_wifi.consecutive_failures = 0;
            s_wifi.band_fallback_active = false;
            xSemaphoreGive(s_wifi.state_mutex);
        }
        return;
    }

    /* Check consecutive failure count and switch to AUTO band if needed */
    uint8_t failures = 0;
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        failures = s_wifi.consecutive_failures;
        xSemaphoreGive(s_wifi.state_mutex);
    }

#ifdef CONFIG_WIFI_PREFER_5GHZ
    if (failures >= WIFI_RECONNECT_FAIL_THRESHOLD && !s_wifi.band_fallback_active) {
        ESP_LOGW(TAG, "Multiple reconnect failures (%d), switching to AUTO band mode", failures);
        esp_err_t band_ret = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        if (band_ret == ESP_OK) {
            if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                s_wifi.band_fallback_active = true;
                xSemaphoreGive(s_wifi.state_mutex);
            }
            ESP_LOGI(TAG, "Band mode set to AUTO for better AP discovery");
        }
    }
#endif

    esp_err_t ret = esp_wifi_connect();
    if (ret == ESP_OK) {
        set_state(WIFI_STATE_RECONNECTING);
        return;
    }

    /* Handle ESP_ERR_WIFI_CONN specially - already connecting, just wait */
    if (ret == ESP_ERR_WIFI_CONN) {
        ESP_LOGD(TAG, "WiFi already connecting, waiting for result...");
        set_state(WIFI_STATE_RECONNECTING);
        /* Don't schedule another timer - the ongoing connection will trigger events */
        return;
    }

    /* esp_wifi_connect() API error — may need WiFi restart */
    ESP_LOGE(TAG, "Failed to initiate reconnection: %s", esp_err_to_name(ret));

    uint32_t next_delay_ms = WIFI_MGR_RECONNECT_MAX_DELAY_MS; /* Default if mutex fails */

    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_wifi.reconnect_delay_ms *= 2;
        if (s_wifi.reconnect_delay_ms > WIFI_MGR_RECONNECT_MAX_DELAY_MS) {
            s_wifi.reconnect_delay_ms = WIFI_MGR_RECONNECT_MAX_DELAY_MS;
        }
        next_delay_ms = s_wifi.reconnect_delay_ms;
        s_wifi.consecutive_failures++;
        failures = s_wifi.consecutive_failures;
        xSemaphoreGive(s_wifi.state_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex for backoff update");
    }

    /* If API keeps failing, try a WiFi stop/start cycle to reset driver state */
    if (failures >= WIFI_RECONNECT_FAIL_THRESHOLD * 2) {
        ESP_LOGW(TAG, "Persistent API errors (%d) — restarting WiFi driver", failures);
        esp_wifi_disconnect();
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_start();
    }

    ESP_LOGI(TAG, "Next reconnect attempt in %lu ms (failures: %d)", next_delay_ms, failures);
    schedule_reconnect(next_delay_ms);
}

/**
 * @brief WiFi event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        /* Log all WiFi events for debugging */
        ESP_LOGI(TAG, "WiFi event received: %ld", event_id);

        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started (event 2)");
                break;

            case WIFI_EVENT_STA_CONNECTED: {
                wifi_ap_record_t ap_info;
                bool connected_5ghz = false;
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                    /* Determine band from channel: 1-14 = 2.4GHz, 36+ = 5GHz */
                    connected_5ghz = (ap_info.primary >= WIFI_MGR_5GHZ_CHANNEL_MIN);
                    const char *band_str = connected_5ghz ? "5GHz" : "2.4GHz";
                    ESP_LOGI(TAG, "Connected to AP (SSID: %s, Channel: %d, Band: %s, RSSI: %d dBm)",
                             ap_info.ssid, ap_info.primary, band_str, ap_info.rssi);
                    mark_credentials_known_good((const char *)ap_info.ssid);

                } else {
                    ESP_LOGI(TAG, "Connected to AP");
                }

                /* Stop watchdog — we're connected */
                if (s_wifi.watchdog_timer) {
                    esp_timer_stop(s_wifi.watchdog_timer);
                }
                s_watchdog_strikes = 0;

                /* Update stats and reset retry counters with mutex protection */
                if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_EVENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    s_wifi.stats.connect_count++;
                    s_wifi.retry_count = 0;
                    s_wifi.reconnect_delay_ms = WIFI_MGR_RECONNECT_BASE_DELAY_MS;
                    s_wifi.consecutive_failures = 0;
                    /* Only clear band_fallback if we successfully connected to 5GHz */
                    if (connected_5ghz) {
                        s_wifi.band_fallback_active = false;
                    }
                    xSemaphoreGive(s_wifi.state_mutex);
                } else {
                    ESP_LOGW(TAG, "Failed to acquire mutex for connect stats update");
                }

#ifdef CONFIG_WIFI_PREFER_5GHZ
                /* Only restore 5GHz preference if we're actually connected to 5GHz
                 * If we're on 2.4GHz (via fallback), keep AUTO mode to maintain stability */
                if (connected_5ghz) {
                    esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
                    ESP_LOGD(TAG, "Restored 5GHz-only band mode");
                } else if (s_wifi.band_fallback_active) {
                    ESP_LOGI(TAG, "Keeping AUTO band mode (connected to 2.4GHz via fallback)");
                }
#endif
                break;
            }

            case WIFI_EVENT_STA_DISCONNECTED: {
                /* Check event_data before dereferencing */
                if (event_data == NULL) {
                    ESP_LOGE(TAG, "Disconnected event data is NULL");
                    set_state(WIFI_STATE_DISCONNECTED);
                    break;
                }

                wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "Disconnected from AP (reason: %d)", event->reason);

                /* Local variables to hold mutex-protected state for use outside critical section */
                bool auto_reconnect = false;
                uint8_t retry_count = 0;
                uint8_t max_retry = 0;
                uint32_t reconnect_delay_ms = WIFI_MGR_RECONNECT_BASE_DELAY_MS;
                uint8_t consecutive_failures = 0;

                /* Update stats and clear IP with mutex protection */
                if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_EVENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    s_wifi.stats.disconnect_count++;
                    s_wifi.ip_addr.addr = 0;
                    auto_reconnect = s_wifi.auto_reconnect;
                    retry_count = s_wifi.retry_count;
                    max_retry = s_wifi.config.max_retry;
                    reconnect_delay_ms = s_wifi.reconnect_delay_ms;
                    consecutive_failures = s_wifi.consecutive_failures;
                    xSemaphoreGive(s_wifi.state_mutex);
                } else {
                    ESP_LOGW(TAG, "Failed to acquire mutex for disconnect handling");
                    /* Fall back to disconnected state without auto-reconnect */
                    set_state(WIFI_STATE_DISCONNECTED);
                    break;
                }

                xEventGroupSetBits(s_wifi.event_group, WIFI_DISCONNECTED_BIT);
                xEventGroupClearBits(s_wifi.event_group, WIFI_CONNECTED_BIT);

                /* Arm the watchdog: full WiFi restart if still disconnected
                 * after 5 minutes.
                 *
                 * Only if it is not already running. Restarting it here looked
                 * harmless and disarmed it completely: a failed reconnect
                 * produces another DISCONNECTED event, so in a reconnect loop
                 * the timer was reset every ~50 s and could never reach five
                 * minutes. Measured on hardware: 87 consecutive failures with
                 * reason 201 over nine hours, and the watchdog never fired
                 * once. It has to measure time since the connection was lost,
                 * not time since the last failed attempt. */
                if (s_wifi.watchdog_timer && !esp_timer_is_active(s_wifi.watchdog_timer)) {
                    esp_timer_start_once(s_wifi.watchdog_timer, 5ULL * 60 * 1000000);
                }

                /* Publish WiFi disconnected event to event bus */
                event_publish(EVT_WIFI_DISCONNECTED, NULL, 0);

#if CONFIG_GW_LED_ENABLED
                /* Update LED status: WiFi disconnected, trying to reconnect */
                led_status_manager_set_condition(LED_COND_WIFI_CONNECTED, false);
                led_status_manager_set_condition(LED_COND_WIFI_CONNECTING, true);
#endif

                /* Stop uptime timer */
                if (s_wifi.uptime_timer) {
                    esp_timer_stop(s_wifi.uptime_timer);
                }

                /* Increment consecutive failure counter for ALL disconnect reasons */
                if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_EVENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    s_wifi.consecutive_failures++;
                    consecutive_failures = s_wifi.consecutive_failures;
                    xSemaphoreGive(s_wifi.state_mutex);
                }

                ESP_LOGW(TAG, "Disconnect reason %d (consecutive failures: %d)", event->reason, consecutive_failures);


                /* Handle NO_AP_FOUND and BEACON_TIMEOUT: band fallback */
                bool ap_not_reachable = (event->reason == WIFI_REASON_NO_AP_FOUND ||
                                         event->reason == WIFI_REASON_BEACON_TIMEOUT);

#ifdef CONFIG_WIFI_PREFER_5GHZ
                /* After multiple failures, switch to AUTO band mode */
                if (consecutive_failures >= WIFI_RECONNECT_FAIL_THRESHOLD && !s_wifi.band_fallback_active) {
                    ESP_LOGW(TAG, "Switching to AUTO band mode after %d failures", consecutive_failures);
                    esp_err_t band_ret = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
                    if (band_ret == ESP_OK) {
                        if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_EVENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                            s_wifi.band_fallback_active = true;
                            xSemaphoreGive(s_wifi.state_mutex);
                        }
                    }
                }
#endif

                /* Auto-reconnect logic — ALWAYS timer-based to give radio time to settle */
                if (auto_reconnect) {
                    /* Calculate delay with exponential backoff */
                    uint32_t delay_ms = reconnect_delay_ms;

                    /* For AP-not-reachable, use longer delays to allow AP to come back */
                    if (ap_not_reachable && consecutive_failures > 1) {
                        delay_ms = delay_ms * consecutive_failures;
                    }

                    /* Cap at max delay */
                    if (delay_ms > WIFI_MGR_RECONNECT_MAX_DELAY_MS) {
                        delay_ms = WIFI_MGR_RECONNECT_MAX_DELAY_MS;
                    }

                    if (retry_count < max_retry) {
                        ESP_LOGI(TAG, "Scheduling reconnect in %lu ms (retry %d/%d)...",
                                delay_ms, retry_count + 1, max_retry);

                        if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_EVENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                            s_wifi.retry_count++;
                            s_wifi.stats.reconnect_count++;
                            /* Apply exponential backoff for next attempt */
                            s_wifi.reconnect_delay_ms = reconnect_delay_ms * 2;
                            if (s_wifi.reconnect_delay_ms > WIFI_MGR_RECONNECT_MAX_DELAY_MS) {
                                s_wifi.reconnect_delay_ms = WIFI_MGR_RECONNECT_MAX_DELAY_MS;
                            }
                            xSemaphoreGive(s_wifi.state_mutex);
                        }
                    } else {
                        ESP_LOGW(TAG, "Max retries (%d) reached, continuing with max backoff",
                                 max_retry);

                        /* Reset retry count but keep backoff delay — never give up */
                        delay_ms = WIFI_MGR_RECONNECT_MAX_DELAY_MS;
                        if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_EVENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                            s_wifi.stats.fail_count++;
                            s_wifi.retry_count = 0;
                            /* Keep delay at max — don't reset to base */
                            s_wifi.reconnect_delay_ms = WIFI_MGR_RECONNECT_MAX_DELAY_MS;
                            xSemaphoreGive(s_wifi.state_mutex);
                        }
                    }

                    set_state(WIFI_STATE_RECONNECTING);
                    schedule_reconnect(delay_ms);
                } else {
                    set_state(WIFI_STATE_DISCONNECTED);
                }
                break;
            }

            case WIFI_EVENT_STA_BSS_RSSI_LOW:
                ESP_LOGI(TAG, "RSSI low - roaming may occur");
                break;

            default:
                break;
        }
    }
}

/**
 * @brief IP event handler
 */
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* Check event_data before dereferencing */
        if (event_data == NULL) {
            ESP_LOGE(TAG, "Got IP event data is NULL");
            return;
        }

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        /* Update IP address and reset uptime with mutex protection */
        if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_EVENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            s_wifi.ip_addr = event->ip_info.ip;
            s_wifi.stats.uptime_seconds = 0;
            xSemaphoreGive(s_wifi.state_mutex);
        } else {
            ESP_LOGW(TAG, "Failed to acquire mutex for IP update");
        }

        xEventGroupSetBits(s_wifi.event_group, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(s_wifi.event_group, WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);
        set_state(WIFI_STATE_CONNECTED);

#if CONFIG_GW_LED_ENABLED
        /* Update LED status: WiFi connected */
        led_status_manager_set_condition(LED_COND_WIFI_CONNECTING, false);
        led_status_manager_set_condition(LED_COND_WIFI_CONNECTED, true);
#endif

        /* Publish WiFi connected event to event bus */
        event_publish(EVT_WIFI_CONNECTED, NULL, 0);

        /* Start uptime timer (stop first in case of IP renewal without disconnect) */
        if (s_wifi.uptime_timer) {
            esp_timer_stop(s_wifi.uptime_timer);  /* safe even if not running */
            esp_err_t timer_ret = esp_timer_start_periodic(s_wifi.uptime_timer, 1000000); /* 1 second */
            if (timer_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start uptime timer: %s", esp_err_to_name(timer_ret));
            }
        }

        /* Setup mDNS for network discovery */
        esp_err_t mdns_ret = wifi_manager_setup_mdns();
        if (mdns_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to setup mDNS: %s", esp_err_to_name(mdns_ret));
        }

        /* Setup SNTP for time synchronization */
        esp_err_t sntp_ret = wifi_manager_setup_sntp();
        if (sntp_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to setup SNTP: %s", esp_err_to_name(sntp_ret));
        }

        /* Ensure power save is still disabled after IP acquisition
         * This is set in init, but reconfirm to ensure stability */
        esp_err_t ps_ret = wifi_manager_set_power_mode(WIFI_POWER_MODE_PERFORMANCE);
        if (ps_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set performance power mode: %s", esp_err_to_name(ps_ret));
        }

#ifdef CONFIG_WIFI_AUTO_BAND_SWITCH
        /* Start RSSI monitoring for potential band switching */
        if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            s_wifi.poor_rssi_count = 0;
            xSemaphoreGive(s_wifi.state_mutex);
        }
#endif
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP address");

        /* Cleanup mDNS when IP is lost */
        wifi_manager_cleanup_mdns();

        /* Clear IP address with mutex protection */
        if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_EVENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            s_wifi.ip_addr.addr = 0;
            xSemaphoreGive(s_wifi.state_mutex);
        }
    }
}

/**
 * @brief Initialize WiFi manager
 *
 * Uses goto cleanup pattern for proper resource cleanup on failure.
 */
#if CONFIG_WIFI_SCAN_ON_BOOT
/**
 * @brief Log every AP the radio can currently see
 *
 * Diagnostic aid for "it will not connect" cases. Association failures alone
 * cannot distinguish between a wrong password, an AP on a channel this
 * regulatory domain forbids, and an AP that is simply not visible — a scan
 * can. Costs a couple of seconds of boot time, hence opt-in.
 */
/**
 * @brief Run one blocking scan and log what it found
 *
 * NULL means "every channel the regulatory domain allows", which is what a
 * diagnostic wants.
 *
 * This used to pass a wifi_scan_config_t that was zero apart from show_hidden.
 * That is not a full scan: channel_bitmap.ghz_2_channels bit0 selects between
 * "scan as bitmap" (0) and "bypass this band" (1), and the remaining bits name
 * the channels. All-zero therefore asks to scan by bitmap with no channels in
 * it. The scan duly reported "no access points at all", which reads like an RF
 * problem and is nothing of the kind.
 *
 * @param label Band description for the log line
 * @return Number of access points found
 */
/** Dwell time per channel for the passive boot scan.
 *  Beacons are typically every 100ms; 150 gives margin without
 *  dragging a full dual-band sweep out too far. */
#define WIFI_MGR_SCAN_PASSIVE_DWELL_MS 150

static uint16_t scan_and_log(const char *label)
{
    /* Passive, not active.
     *
     * An active scan sends probe requests, which regulations forbid on DFS
     * channels — so an access point on one of them never answers and the scan
     * reports nothing. That is exactly what happened here: this gateway's AP
     * is on channel 100, the diagnostic said "no access points at all" on
     * every band, and that was read as an RF fault when the radio was fine.
     * A passive scan just listens for beacons and hears every channel. It
     * dwells longer, which for a boot diagnostic is the right trade. */
    wifi_scan_config_t cfg = {
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time   = { .passive = WIFI_MGR_SCAN_PASSIVE_DWELL_MS },
    };

    esp_err_t ret = esp_wifi_scan_start(&cfg, true);  /* blocking, all channels */
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "  %s: scan failed: %s", label, esp_err_to_name(ret));
        return 0;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        ESP_LOGW(TAG, "  %s: no access points", label);
        esp_wifi_clear_ap_list();
        return 0;
    }

    /* PSRAM: a full record set is a few KB and must not land on the stack. */
    wifi_ap_record_t *records =
        mem_alloc(sizeof(wifi_ap_record_t) * count, MEM_CAP_PSRAM);
    if (records == NULL) {
        ESP_LOGW(TAG, "  %s: %u APs but the record buffer did not fit", label, count);
        esp_wifi_clear_ap_list();
        return count;
    }

    uint16_t got = count;
    if (esp_wifi_scan_get_ap_records(&got, records) == ESP_OK) {
        ESP_LOGI(TAG, "  %s: %u access point(s)", label, got);
        for (uint16_t i = 0; i < got; i++) {
            const wifi_ap_record_t *ap = &records[i];
            ESP_LOGI(TAG, "    ch%-3d %4d dBm  auth=%d  %s%s",
                     ap->primary, ap->rssi, (int)ap->authmode,
                     (const char *)ap->ssid,
                     ap->primary >= WIFI_MGR_5GHZ_CHANNEL_MIN ? "  [5GHz]" : "");
        }
    }

    mem_ng_free(records);
    esp_wifi_clear_ap_list();
    return count;
}

/**
 * @brief Scan each band separately, then together
 *
 * One combined scan cannot distinguish "nothing on the air" from "this band
 * mode does not scan what you think it does". On a dual-band part that
 * difference is the whole question: an empty result under AUTO says nothing
 * about whether 2.4 GHz alone would have found the access point.
 *
 * Leaves the band mode as it found it.
 */
static void log_visible_aps(void)
{
    static const struct {
        wifi_band_mode_t mode;
        const char      *label;
    } passes[] = {
        { WIFI_BAND_MODE_2G_ONLY, "2.4 GHz only" },
        { WIFI_BAND_MODE_5G_ONLY, "5 GHz only"   },
        { WIFI_BAND_MODE_AUTO,    "both bands"   },
    };

    wifi_band_mode_t saved = WIFI_BAND_MODE_AUTO;
    esp_wifi_get_band_mode(&saved);

    ESP_LOGI(TAG, "Scanning for access points, one pass per band:");

    uint16_t total = 0;
    for (size_t i = 0; i < sizeof(passes) / sizeof(passes[0]); i++) {
        esp_err_t ret = esp_wifi_set_band_mode(passes[i].mode);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "  %s: band mode not settable: %s",
                     passes[i].label, esp_err_to_name(ret));
            continue;
        }
        total += scan_and_log(passes[i].label);
    }

    if (total == 0) {
        ESP_LOGW(TAG, "No access points on any band. Either nothing is on the "
                      "air within range, or this radio is not receiving.");
    }

    esp_wifi_set_band_mode(saved);
}
#endif /* CONFIG_WIFI_SCAN_ON_BOOT */

esp_err_t wifi_manager_init(void)
{
    esp_err_t ret = ESP_OK;

    /* Track what resources have been created for cleanup */
    bool event_group_created = false;
    bool state_mutex_created = false;
    bool netif_created = false;
    bool wifi_initialized = false;
    bool wifi_event_registered = false;
    bool ip_got_event_registered = false;
    bool ip_lost_event_registered = false;
    bool wifi_started = false;
    bool uptime_timer_created = false;
    bool reconnect_timer_created = false;

    if (s_wifi.initialized) {
        ESP_LOGW(TAG, "WiFi manager already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WiFi manager...");

    /* Create event group */
    s_wifi.event_group = xEventGroupCreate();
    if (!s_wifi.event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    event_group_created = true;

    /* Create state mutex */
    s_wifi.state_mutex = xSemaphoreCreateMutex();
    if (!s_wifi.state_mutex) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    state_mutex_created = true;

    /* Initialize TCP/IP stack */
    ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TCP/IP stack: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    /* Create default WiFi station */
    s_wifi.netif = esp_netif_create_default_wifi_sta();
    if (!s_wifi.netif) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA interface");
        ret = ESP_FAIL;
        goto cleanup;
    }
    netif_created = true;

    /* Initialize WiFi with default config */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    wifi_initialized = true;

    /* Register event handlers */
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    wifi_event_registered = true;

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ip_got_event_registered = true;

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                     &ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP lost event handler: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ip_lost_event_registered = true;

    /* Set WiFi mode to station */
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    /* Start WiFi */
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    wifi_started = true;

    /* Set the regulatory domain explicitly.
     *
     * Without this call ESP-IDF stays on country "01" (world safe mode), which
     * permits 2.4GHz channels 1-11 and no 5GHz channels at all. On a dual-band
     * part that silently rules out the entire 5GHz band — which is the band we
     * actually want, since 2.4GHz collides with Zigbee. */
    esp_err_t cc_ret = esp_wifi_set_country_code(CONFIG_WIFI_COUNTRY_CODE, true);
    if (cc_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set country code '%s': %s",
                 CONFIG_WIFI_COUNTRY_CODE, esp_err_to_name(cc_ret));
    } else {
        /* esp_wifi_get_country_code() writes 3 bytes: two country characters
         * plus an environment character (' ', 'I' or 'O'). It does NOT
         * null-terminate, so the buffer needs a fourth byte of its own. */
        char cc[4] = {0};
        if (esp_wifi_get_country_code(cc) == ESP_OK) {
            ESP_LOGI(TAG, "Regulatory domain: %.2s (env '%c')", cc, cc[2]);
        }
        wifi_country_t country;
        if (esp_wifi_get_country(&country) == ESP_OK) {
            ESP_LOGI(TAG, "Channel policy: start=%u count=%u policy=%s",
                     country.schan, country.nchan,
                     country.policy == WIFI_COUNTRY_POLICY_AUTO ? "AUTO" : "MANUAL");
        }
    }

#ifdef CONFIG_WIFI_PREFER_5GHZ
    /* Use AUTO band mode with rssi_5g_adjustment to prefer 5GHz.
     * Previously used 5G_ONLY which caused a 45s timeout when 5GHz AP
     * had weak signal (DHCP fails), delaying Zigbee coordinator start. */
    esp_err_t band_ret = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (band_ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi band mode set to AUTO (5GHz preferred via RSSI adjustment)");
    } else {
        ESP_LOGW(TAG, "AUTO band mode failed: %s", esp_err_to_name(band_ret));
    }
#endif

    /* Scan only after the band mode is set.
     *
     * This used to run before it, which made the diagnostic lie: the scan
     * covered whatever band mode the driver happened to start in rather than
     * the one the gateway actually connects with, so "no access points at all"
     * said nothing about why association was failing. The scan is here to
     * answer that question, so it has to see the same bands. */
#if CONFIG_WIFI_SCAN_ON_BOOT
    log_visible_aps();
#endif

    /* Disable power save BEFORE connecting to ensure fast response to management frames
     * This is critical for SA Query response timing in tri-radio operation */
    esp_err_t ps_ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable WiFi power save: %s", esp_err_to_name(ps_ret));
    } else {
        ESP_LOGI(TAG, "WiFi power save disabled for tri-radio stability");
    }

    /* Create uptime timer */
    const esp_timer_create_args_t uptime_timer_args = {
        .callback = &uptime_timer_callback,
        .name = "wifi_uptime"
    };
    ret = esp_timer_create(&uptime_timer_args, &s_wifi.uptime_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create uptime timer: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    uptime_timer_created = true;

    /* Create reconnect timer */
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = &reconnect_timer_callback,
        .name = "wifi_reconnect"
    };
    ret = esp_timer_create(&reconnect_timer_args, &s_wifi.reconnect_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create reconnect timer: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    reconnect_timer_created = true;

    /* Create WiFi watchdog timer — full restart if disconnected > 5 min */
    const esp_timer_create_args_t watchdog_args = {
        .callback = &wifi_watchdog_callback,
        .name = "wifi_wd"
    };
    esp_timer_create(&watchdog_args, &s_wifi.watchdog_timer);

    /* Initialize stats */
    memset(&s_wifi.stats, 0, sizeof(wifi_stats_t));

    s_wifi.initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized successfully");

    return ESP_OK;

cleanup:
    /* Cleanup in reverse order of creation */
    if (reconnect_timer_created && s_wifi.reconnect_timer) {
        esp_timer_delete(s_wifi.reconnect_timer);
        s_wifi.reconnect_timer = NULL;
    }
    if (uptime_timer_created && s_wifi.uptime_timer) {
        esp_timer_delete(s_wifi.uptime_timer);
        s_wifi.uptime_timer = NULL;
    }
    if (wifi_started) {
        esp_wifi_stop();
    }
    if (ip_lost_event_registered) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler);
    }
    if (ip_got_event_registered) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler);
    }
    if (wifi_event_registered) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    }
    if (wifi_initialized) {
        esp_wifi_deinit();
    }
    if (netif_created && s_wifi.netif) {
        esp_netif_destroy_default_wifi(s_wifi.netif);
        s_wifi.netif = NULL;
    }
    if (state_mutex_created && s_wifi.state_mutex) {
        vSemaphoreDelete(s_wifi.state_mutex);
        s_wifi.state_mutex = NULL;
    }
    if (event_group_created && s_wifi.event_group) {
        vEventGroupDelete(s_wifi.event_group);
        s_wifi.event_group = NULL;
    }

    ESP_LOGE(TAG, "WiFi manager initialization failed");
    return ret;
}

/**
 * @brief Connect to WiFi
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_wifi.initialized) {
        ESP_LOGE(TAG, "WiFi manager not initialized");
        return ESP_ERR_WIFI_NOT_INIT;
    }

    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);

    /* Store configuration */
    memset(&s_wifi.config, 0, sizeof(wifi_manager_config_t));
    strlcpy(s_wifi.config.ssid, ssid, sizeof(s_wifi.config.ssid));
    if (password) {
        strlcpy(s_wifi.config.password, password, sizeof(s_wifi.config.password));
    }
    s_wifi.config.max_retry = CONFIG_WIFI_MAXIMUM_RETRY;
#ifdef CONFIG_WIFI_PREFER_5GHZ
    s_wifi.config.dual_band = true;
#else
    s_wifi.config.dual_band = false;
#endif

    /* Validate configuration before connecting */
    esp_err_t validate_ret = wifi_config_validate(&s_wifi.config);
    if (validate_ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi configuration validation failed: %s", esp_err_to_name(validate_ret));
        return validate_ret;
    }

    /* Configure WiFi connection settings
     * PMF (Protected Management Frames) is controlled by CONFIG_WIFI_DISABLE_PMF
     * because SA Query timeout issues occur with tri-radio coexistence
     */
    wifi_config_t wifi_config = {
        .sta = {
#ifdef CONFIG_WIFI_DISABLE_PMF
            /* PMF disabled to avoid SA Query timeout on tri-radio ESP32-C5
             * SA Query requires fast management frame response (~100-200ms)
             * which can be missed when Zigbee/BLE is active */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED,
            .pmf_cfg = {
                .capable = false,
                .required = false
            },
#else
            /* PMF enabled - use WPA2/WPA3 with protection */
            .threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
#endif
#if CONFIG_ESP_WIFI_11KV_SUPPORT
            .btm_enabled = true,   /* BSS Transition Management (802.11v) */
            .rm_enabled = true,    /* Radio Measurement (802.11k) */
#endif
#if CONFIG_ESP_WIFI_MBO_SUPPORT
            .mbo_enabled = true,   /* Multi-Band Operation */
#endif
            .listen_interval = WIFI_LISTEN_INTERVAL_PERF,  /* Performance mode for tri-radio coexistence */
            /* Scan every channel, do not stop at the first hit.
             *
             * The default (WIFI_FAST_SCAN, 0) stops at the first matching AP and
             * probes actively. Active probing is not permitted on DFS channels,
             * so an access point on one of them is simply not found — this
             * gateway's AP sits on channel 100. The symptom was association
             * failing with reason 201 (NO_AP_FOUND) over and over until some
             * attempt happened to catch a beacon, i.e. "WiFi works, just often
             * not immediately". ALL_CHANNEL_SCAN covers the DFS channels
             * passively, which is the only way to hear them.
             *
             * Sorting by signal together with threshold.rssi_5g_adjustment is
             * what expresses the 5 GHz preference. */
            .scan_method = (CONFIG_WIFI_SCAN_METHOD == 0) ? WIFI_FAST_SCAN
                                                          : WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
#ifdef CONFIG_WIFI_PREFER_5GHZ
            /* Enable 5GHz preference via RSSI adjustment */
            .threshold.rssi_5g_adjustment = CONFIG_WIFI_5GHZ_RSSI_ADJUSTMENT,
#endif
        },
    };

    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }

    /* Log security configuration for debugging */
#ifdef CONFIG_WIFI_DISABLE_PMF
    ESP_LOGI(TAG, "Security: WPA2-PSK (PMF disabled for tri-radio stability)");
#else
    ESP_LOGI(TAG, "Security: WPA2/WPA3-PSK (PMF capable)");
#endif

    /* Note: Band mode is set in wifi_manager_init() before esp_wifi_start() */

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Clear event bits */
    xEventGroupClearBits(s_wifi.event_group,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);

    /* Enable auto-reconnect and reset retry count with mutex protection */
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_wifi.auto_reconnect = true;
        s_wifi.retry_count = 0;
        xSemaphoreGive(s_wifi.state_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex for connect setup");
        /* Continue anyway - these are initialization values */
    }

    /* Initiate connection */
    set_state(WIFI_STATE_CONNECTING);
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to WiFi: %s", esp_err_to_name(ret));
        set_state(WIFI_STATE_FAILED);
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief Disconnect from WiFi
 */
esp_err_t wifi_manager_disconnect(void)
{
    if (!s_wifi.initialized) {
        return ESP_ERR_WIFI_NOT_INIT;
    }

    ESP_LOGI(TAG, "Disconnecting from WiFi...");

    /* Disable auto-reconnect with mutex protection */
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_wifi.auto_reconnect = false;
        xSemaphoreGive(s_wifi.state_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex for disconnect");
        /* Continue anyway - best effort to disable auto-reconnect */
    }

    /* Stop timers */
    if (s_wifi.uptime_timer) {
        esp_timer_stop(s_wifi.uptime_timer);
    }
    if (s_wifi.reconnect_timer) {
        esp_timer_stop(s_wifi.reconnect_timer);
    }

    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disconnect: %s", esp_err_to_name(ret));
        return ret;
    }

    set_state(WIFI_STATE_DISCONNECTED);
    return ESP_OK;
}

/**
 * @brief Get WiFi state
 */
wifi_state_t wifi_manager_get_state(void)
{
    /* Return disconnected if not initialized to avoid mutex NULL pointer */
    if (!s_wifi.initialized || s_wifi.state_mutex == NULL) {
        return WIFI_STATE_DISCONNECTED;
    }

    wifi_state_t state;

    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        state = s_wifi.state;
        xSemaphoreGive(s_wifi.state_mutex);
    } else {
        /* Mutex timeout - return actual state without mutex protection
         * This is a fallback to avoid false "disconnected" reports during high CPU load */
        ESP_LOGD(TAG, "State mutex timeout, using unprotected read");
        state = s_wifi.state;
    }

    return state;
}

/**
 * @brief Check if WiFi is connected
 */
bool wifi_manager_is_connected(void)
{
    return (wifi_manager_get_state() == WIFI_STATE_CONNECTED);
}

/**
 * @brief Get IP address
 */
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len)
{
    if (!ip_str || len < WIFI_IP_STRING_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_wifi.initialized || s_wifi.state_mutex == NULL) {
        return ESP_ERR_WIFI_NOT_INIT;
    }

    esp_ip4_addr_t ip_copy;

    /* Read IP address with mutex protection */
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ip_copy = s_wifi.ip_addr;
        xSemaphoreGive(s_wifi.state_mutex);
    } else {
        return ESP_ERR_TIMEOUT;
    }

    if (ip_copy.addr == 0) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    snprintf(ip_str, len, IPSTR, IP2STR(&ip_copy));
    return ESP_OK;
}

/**
 * @brief Update RSSI and signal quality values
 *
 * Queries the WiFi driver for current AP info and updates cached RSSI
 * and signal quality values. Thread-safe; uses mutex protection.
 */
void wifi_manager_update_rssi(void)
{
    if (!s_wifi.initialized || s_wifi.state_mutex == NULL) {
        return;
    }

    /* Only update if connected */
    if (s_wifi.state != WIFI_STATE_CONNECTED) {
        /* Clear cached values when not connected */
        if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            s_wifi.current_rssi = 0;
            s_wifi.signal_quality = 0;
            s_wifi.stats.rssi = 0;
            s_wifi.stats.signal_quality = 0;
            xSemaphoreGive(s_wifi.state_mutex);
        }
        return;
    }

    /* Query current AP info */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return;
    }

    /* Calculate signal quality: 2 * (rssi + 100), clamped to 0-100
     * -50 dBm = 100%, -100 dBm = 0% */
    int quality = 2 * (ap_info.rssi + 100);
    if (quality < 0) {
        quality = 0;
    } else if (quality > 100) {
        quality = 100;
    }

    /* Update cached values with mutex protection */
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_wifi.current_rssi = ap_info.rssi;
        s_wifi.signal_quality = (uint8_t)quality;
        s_wifi.stats.rssi = ap_info.rssi;
        s_wifi.stats.signal_quality = (uint8_t)quality;
        xSemaphoreGive(s_wifi.state_mutex);
    }

    ESP_LOGD(TAG, "RSSI: %d dBm, Quality: %d%%", ap_info.rssi, quality);

#ifdef CONFIG_WIFI_AUTO_BAND_SWITCH
    /* Check if band switch is needed based on current RSSI */
    wifi_manager_check_band_switch(ap_info.rssi);
#endif
}

/**
 * @brief Get WiFi RSSI (cached value)
 */
int8_t wifi_manager_get_rssi(void)
{
    if (!s_wifi.initialized || s_wifi.state_mutex == NULL) {
        return 0;
    }

    int8_t rssi = 0;

    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        rssi = s_wifi.current_rssi;
        xSemaphoreGive(s_wifi.state_mutex);
    }

    return rssi;
}

/**
 * @brief Get WiFi signal quality as percentage
 */
uint8_t wifi_manager_get_signal_quality(void)
{
    if (!s_wifi.initialized || s_wifi.state_mutex == NULL) {
        return 0;
    }

    uint8_t quality = 0;

    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        quality = s_wifi.signal_quality;
        xSemaphoreGive(s_wifi.state_mutex);
    }

    return quality;
}

/**
 * @brief Get WiFi statistics
 */
esp_err_t wifi_manager_get_stats(wifi_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_wifi.initialized || s_wifi.state_mutex == NULL) {
        return ESP_ERR_WIFI_NOT_INIT;
    }

    /* Read stats with mutex protection */
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        memcpy(stats, &s_wifi.stats, sizeof(wifi_stats_t));
        xSemaphoreGive(s_wifi.state_mutex);
    } else {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

/**
 * @brief Set auto-reconnect
 */
esp_err_t wifi_manager_set_auto_reconnect(bool enable)
{
    if (!s_wifi.initialized || s_wifi.state_mutex == NULL) {
        return ESP_ERR_WIFI_NOT_INIT;
    }

    /* Set auto-reconnect flag with mutex protection */
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_wifi.auto_reconnect = enable;
        xSemaphoreGive(s_wifi.state_mutex);
    } else {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Auto-reconnect %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t wifi_manager_reconnect(void)
{
    if (!s_wifi.initialized) {
        ESP_LOGE(TAG, "WiFi manager not initialized");
        return ESP_ERR_WIFI_NOT_INIT;
    }

    /* Check if we have stored credentials */
    if (s_wifi.config.ssid[0] == '\0') {
        ESP_LOGE(TAG, "No stored credentials for reconnect");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Reconnecting to WiFi '%s'...", s_wifi.config.ssid);

    /* Debug: Check WiFi state before reconnect */
    wifi_mode_t mode;
    esp_err_t mode_ret = esp_wifi_get_mode(&mode);
    if (mode_ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi mode: %d (STA=1, AP=2, APSTA=3)", mode);
    } else {
        ESP_LOGW(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(mode_ret));
    }

    /* Debug: Check if WiFi is started */
    wifi_ap_record_t ap_info;
    esp_err_t ap_ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ap_ret == ESP_OK) {
        ESP_LOGI(TAG, "Already connected to AP: %s (ch:%d, rssi:%d)",
                 ap_info.ssid, ap_info.primary, ap_info.rssi);
        /* Already connected, just return success */
        set_state(WIFI_STATE_CONNECTED);
        return ESP_OK;
    } else if (ap_ret == ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGI(TAG, "WiFi not connected, will attempt connection");
    } else {
        ESP_LOGW(TAG, "esp_wifi_sta_get_ap_info: %s", esp_err_to_name(ap_ret));
    }

    /* Reset all retry counters and enable auto_reconnect
     * This is a fresh start after explicit reconnect request */
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_wifi.retry_count = 0;
        s_wifi.consecutive_failures = 0;
        s_wifi.reconnect_delay_ms = WIFI_MGR_RECONNECT_BASE_DELAY_MS;
        s_wifi.auto_reconnect = true;  /* Enable auto-reconnect for subsequent events */
        xSemaphoreGive(s_wifi.state_mutex);
    }

    /* Cancel any pending connection attempt first.
     * esp_wifi_set_config() fails with ESP_ERR_WIFI_STATE if WiFi is connecting.
     * We ignore errors here - disconnect may fail if not connected. */
    esp_err_t disc_ret = esp_wifi_disconnect();
    ESP_LOGD(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(disc_ret));
    vTaskDelay(pdMS_TO_TICKS(50));  /* Brief delay for disconnect to complete */

    /* Re-apply WiFi configuration - MUST match wifi_manager_connect() exactly!
     * After esp_wifi_stop() or stack reset, the configuration is lost.
     * Using different settings (especially PMF/SAE) causes authentication failures. */
    wifi_config_t wifi_config = {
        .sta = {
#ifdef CONFIG_WIFI_DISABLE_PMF
            /* PMF disabled - same as wifi_manager_connect() */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED,
            .pmf_cfg = {
                .capable = false,
                .required = false
            },
#else
            /* PMF enabled - same as wifi_manager_connect() */
            .threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
#endif
#if CONFIG_ESP_WIFI_11KV_SUPPORT
            .btm_enabled = true,
            .rm_enabled = true,
#endif
#if CONFIG_ESP_WIFI_MBO_SUPPORT
            .mbo_enabled = true,
#endif
            .listen_interval = WIFI_LISTEN_INTERVAL_PERF,
            /* All channels, sorted by signal — see the connect path for why
             * FAST_SCAN misses an AP on a DFS channel. */
            .scan_method = (CONFIG_WIFI_SCAN_METHOD == 0) ? WIFI_FAST_SCAN
                                                          : WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
#ifdef CONFIG_WIFI_PREFER_5GHZ
            .threshold.rssi_5g_adjustment = CONFIG_WIFI_5GHZ_RSSI_ADJUSTMENT,
#endif
        },
    };

    strlcpy((char *)wifi_config.sta.ssid, s_wifi.config.ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_wifi.config.password, sizeof(wifi_config.sta.password));

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "WiFi config set successfully for SSID '%s'", s_wifi.config.ssid);

    /* Initiate connection */
    set_state(WIFI_STATE_RECONNECTING);
    ret = esp_wifi_connect();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "esp_wifi_connect() called successfully - waiting for events...");
    } else if (ret == ESP_ERR_WIFI_CONN) {
        ESP_LOGI(TAG, "WiFi already connecting (ESP_ERR_WIFI_CONN)");
    } else {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Reset the WiFi retry counter
 *
 * Resets the internal retry counter and consecutive failures to zero.
 * This is useful after transitioning out of a special mode (like Zigbee PAIRING)
 * where WiFi retries may have been exhausted due to radio contention.
 * Also resets band fallback state.
 *
 * Note: Does NOT change band mode - the caller should set band mode as needed.
 */
void wifi_manager_reset_retry_count(void)
{
    if (!s_wifi.initialized) {
        return;
    }

    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_STATE_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_wifi.retry_count = 0;
        s_wifi.consecutive_failures = 0;
        s_wifi.reconnect_delay_ms = WIFI_MGR_RECONNECT_BASE_DELAY_MS;
        s_wifi.band_fallback_active = false;
        ESP_LOGI(TAG, "WiFi retry counters reset (retry, failures, delay, band_fallback)");
        xSemaphoreGive(s_wifi.state_mutex);
    }
}

/**
 * @brief Set WiFi power saving mode
 *
 * Adjusts WiFi power saving behavior to optimize for either
 * performance (low latency) or power consumption. Uses adaptive
 * listen intervals based on DTIM periods.
 *
 * @param mode Power saving mode to set
 * @return ESP_OK on success
 * @return ESP_ERR_WIFI_NOT_INIT if WiFi not initialized
 * @return ESP_ERR_INVALID_ARG if invalid power mode
 */
esp_err_t wifi_manager_set_power_mode(wifi_power_mode_t mode)
{
    if (!s_wifi.initialized) {
        return ESP_ERR_WIFI_NOT_INIT;
    }

    wifi_ps_type_t ps_type;
    const char *mode_str;

    switch (mode) {
        case WIFI_POWER_MODE_PERFORMANCE:
            ps_type = WIFI_PS_NONE;
            mode_str = "PERFORMANCE (PS disabled)";
            break;
        case WIFI_POWER_MODE_BALANCED:
            ps_type = WIFI_PS_MIN_MODEM;
            mode_str = "BALANCED (MIN_MODEM)";
            break;
        case WIFI_POWER_MODE_POWERSAVE:
            ps_type = WIFI_PS_MAX_MODEM;
            mode_str = "POWERSAVE (MAX_MODEM)";
            break;
        default:
            ESP_LOGE(TAG, "Invalid power mode: %d", mode);
            return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_wifi_set_ps(ps_type);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set power save mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi power mode set to %s", mode_str);
    return ESP_OK;
}

/**
 * @brief Get WiFi event group
 */
EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_wifi.event_group;
}

/**
 * @brief Test WiFi manager
 */
esp_err_t wifi_manager_test(void)
{
    ESP_LOGI(TAG, "=== WiFi Manager Test ===");

    /* Check initialization */
    if (!s_wifi.initialized) {
        ESP_LOGE(TAG, "Test FAILED: Not initialized");
        return ESP_FAIL;
    }

    /* Print current state */
    wifi_state_t state = wifi_manager_get_state();
    ESP_LOGI(TAG, "Current state: %d", state);

    /* Print stats */
    wifi_stats_t stats;
    wifi_manager_get_stats(&stats);
    ESP_LOGI(TAG, "Stats: connects=%lu, disconnects=%lu, reconnects=%lu, fails=%lu",
            stats.connect_count, stats.disconnect_count,
            stats.reconnect_count, stats.fail_count);

    /* Print IP if connected */
    if (wifi_manager_is_connected()) {
        char ip_str[WIFI_IP_STRING_LEN];
        if (wifi_manager_get_ip(ip_str, sizeof(ip_str)) == ESP_OK) {
            ESP_LOGI(TAG, "IP Address: %s", ip_str);
        }

        int8_t rssi = wifi_manager_get_rssi();
        ESP_LOGI(TAG, "RSSI: %d dBm", rssi);
    }

    ESP_LOGI(TAG, "Test PASSED");
    return ESP_OK;
}

/**
 * @brief Get mDNS hostname
 *
 * Returns the hostname for mDNS discovery. Prefers the ESPHome device name
 * for consistency with Home Assistant discovery, falling back to the legacy
 * GATEWAY_MDNS_HOSTNAME config if ESPHome is not enabled.
 */
const char *wifi_manager_get_hostname(void)
{
#ifdef CONFIG_ESPHOME_DEVICE_NAME
    return CONFIG_ESPHOME_DEVICE_NAME;
#elif defined(CONFIG_GATEWAY_MDNS_HOSTNAME)
    return CONFIG_GATEWAY_MDNS_HOSTNAME;
#else
    return "zigbee-gateway";
#endif
}

/**
 * @brief Setup mDNS service for network discovery
 *
 * Initializes mDNS with hostname, instance name, and HTTP service.
 * Called when IP address is obtained.
 */
static esp_err_t wifi_manager_setup_mdns(void)
{
    esp_err_t ret;

    /* Check if already initialized */
    if (s_wifi.mdns_initialized) {
        ESP_LOGD(TAG, "mDNS already initialized");
        return ESP_OK;
    }

    /* Initialize mDNS */
    ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set hostname */
    const char *hostname = wifi_manager_get_hostname();
    ret = mdns_hostname_set(hostname);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(ret));
        mdns_free();
        return ret;
    }

    /* Set instance name (friendly name) */
#ifdef CONFIG_ESPHOME_FRIENDLY_NAME
    ret = mdns_instance_name_set(CONFIG_ESPHOME_FRIENDLY_NAME);
#else
    ret = mdns_instance_name_set("ESP32-C5 Zigbee Gateway");
#endif
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance name set failed: %s", esp_err_to_name(ret));
        /* Non-fatal - continue */
    }

    /* Register HTTP service on port 80 (for future web interface) */
    ret = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS HTTP service add failed: %s", esp_err_to_name(ret));
        /* Non-fatal - continue */
    }

    /* Add TXT records for service identification */
    mdns_txt_item_t txt_records[] = {
        {"version", FIRMWARE_VERSION},
        {"type", "zigbee-gateway"},
    };

    ret = mdns_service_txt_set("_http", "_tcp", txt_records,
                               sizeof(txt_records) / sizeof(txt_records[0]));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS TXT records set failed: %s", esp_err_to_name(ret));
        /* Non-fatal - continue */
    }

    s_wifi.mdns_initialized = true;
    ESP_LOGI(TAG, "mDNS initialized: %s.local", hostname);

    return ESP_OK;
}

/**
 * @brief Cleanup mDNS service
 *
 * Frees mDNS resources. Called when disconnected from WiFi.
 */
static void wifi_manager_cleanup_mdns(void)
{
    if (!s_wifi.mdns_initialized) {
        return;
    }

    mdns_free();
    s_wifi.mdns_initialized = false;
    ESP_LOGI(TAG, "mDNS freed");
}

/**
 * @brief Setup SNTP for time synchronization
 *
 * Initializes SNTP client to synchronize time with NTP servers.
 * Called when IP address is obtained.
 *
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t wifi_manager_setup_sntp(void)
{
    /* Check if already initialized */
    if (s_wifi.sntp_initialized) {
        ESP_LOGD(TAG, "SNTP already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SNTP...");

    /* Configure SNTP operating mode */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

    /* Set primary NTP server - pool.ntp.org is widely available */
    esp_sntp_setservername(0, "pool.ntp.org");

    /* Set secondary NTP server for redundancy */
    esp_sntp_setservername(1, "time.google.com");

    /* Initialize SNTP */
    esp_sntp_init();

    /* Set timezone to UTC - applications can override with setenv("TZ", ...) */
    setenv("TZ", "UTC0", 1);
    tzset();

    s_wifi.sntp_initialized = true;
    ESP_LOGI(TAG, "SNTP initialized (servers: pool.ntp.org, time.google.com)");

    return ESP_OK;
}

#ifdef CONFIG_WIFI_AUTO_BAND_SWITCH
/**
 * @brief Get current WiFi band from AP info
 *
 * @return Current band (WIFI_MGR_BAND_2_4GHZ or WIFI_MGR_BAND_5GHZ)
 */
static wifi_mgr_band_t wifi_manager_get_current_band(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return (ap_info.primary >= WIFI_MGR_5GHZ_CHANNEL_MIN) ? WIFI_MGR_BAND_5GHZ : WIFI_MGR_BAND_2_4GHZ;
    }
    return WIFI_MGR_BAND_2_4GHZ;  /* Default to 2.4GHz if unknown */
}

/**
 * @brief Switch to alternate WiFi band
 *
 * Disconnects from current AP and reconnects with preference for the alternate band.
 * If currently on 5GHz, switches to 2.4GHz preference and vice versa.
 *
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t wifi_manager_switch_band(void)
{
    esp_err_t ret;
    wifi_mgr_band_t current_band = wifi_manager_get_current_band();
    wifi_mgr_band_t target_band = (current_band == WIFI_MGR_BAND_5GHZ) ? WIFI_MGR_BAND_2_4GHZ : WIFI_MGR_BAND_5GHZ;

    const char *current_str = (current_band == WIFI_MGR_BAND_5GHZ) ? "5GHz" : "2.4GHz";
    const char *target_str = (target_band == WIFI_MGR_BAND_5GHZ) ? "5GHz" : "2.4GHz";

    ESP_LOGI(TAG, "Band switch: %s -> %s (poor signal detected)", current_str, target_str);

    /* Set band mode based on target */
    wifi_band_mode_t band_mode;
    if (target_band == WIFI_MGR_BAND_5GHZ) {
        band_mode = WIFI_BAND_MODE_5G_ONLY;
    } else {
        band_mode = WIFI_BAND_MODE_2G_ONLY;
    }

    ret = esp_wifi_set_band_mode(band_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set band mode: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Disconnect and reconnect to trigger band change */
    ret = esp_wifi_disconnect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disconnect for band switch: %s", esp_err_to_name(ret));
        /* Restore AUTO mode on failure */
        esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        return ret;
    }

    /* Update state with mutex protection */
    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_EVENT_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_wifi.current_band = target_band;
        s_wifi.last_band_switch_time = s_wifi.stats.uptime_seconds;
        s_wifi.poor_rssi_count = 0;
        s_wifi.band_switch_count++;
        xSemaphoreGive(s_wifi.state_mutex);
    }

    ESP_LOGI(TAG, "Band switch #%lu initiated, reconnecting on %s...",
             s_wifi.band_switch_count, target_str);

    /* Trigger reconnection */
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reconnect after band switch: %s", esp_err_to_name(ret));
        /* Restore AUTO mode on failure */
        esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief Check if band switch is needed based on RSSI
 *
 * Called periodically when connected (every WIFI_MGR_RSSI_UPDATE_INTERVAL_SEC seconds).
 * Tracks consecutive poor RSSI readings and triggers band switch after
 * WIFI_RSSI_CHECK_COUNT consecutive poor readings.
 * Respects cooldown period (WIFI_BAND_SWITCH_COOLDOWN_SEC) between switches.
 *
 * @param rssi Current RSSI value in dBm
 */
static void wifi_manager_check_band_switch(int8_t rssi)
{
    /* Only check if we're connected */
    if (s_wifi.state != WIFI_STATE_CONNECTED) {
        return;
    }

    /* Check cooldown period */
    uint32_t uptime = 0;
    uint32_t last_switch = 0;
    uint8_t poor_count = 0;

    if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        uptime = s_wifi.stats.uptime_seconds;
        last_switch = s_wifi.last_band_switch_time;
        poor_count = s_wifi.poor_rssi_count;
        xSemaphoreGive(s_wifi.state_mutex);
    } else {
        return;  /* Skip if mutex not available */
    }

    /* Check if still in cooldown period */
    if ((uptime - last_switch) < WIFI_BAND_SWITCH_COOLDOWN_SEC) {
        /* Reset poor count during cooldown - signal might improve */
        if (rssi >= WIFI_RSSI_POOR_THRESHOLD) {
            if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                s_wifi.poor_rssi_count = 0;
                xSemaphoreGive(s_wifi.state_mutex);
            }
        }
        return;
    }

    /* Check RSSI threshold */
    if (rssi < WIFI_RSSI_POOR_THRESHOLD) {
        /* Poor signal - increment counter */
        poor_count++;
        ESP_LOGD(TAG, "Poor RSSI: %d dBm (count: %d/%d)", rssi, poor_count, WIFI_RSSI_CHECK_COUNT);

        if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            s_wifi.poor_rssi_count = poor_count;
            xSemaphoreGive(s_wifi.state_mutex);
        }

        /* Check if threshold reached */
        if (poor_count >= WIFI_RSSI_CHECK_COUNT) {
            wifi_mgr_band_t band = wifi_manager_get_current_band();
            const char *band_str = (band == WIFI_MGR_BAND_5GHZ) ? "5GHz" : "2.4GHz";
            ESP_LOGW(TAG, "Poor RSSI (%d dBm) on %s after %d checks, switching band",
                     rssi, band_str, WIFI_RSSI_CHECK_COUNT);
            wifi_manager_switch_band();
        }
    } else {
        /* Good signal - reset counter */
        if (poor_count > 0) {
            if (xSemaphoreTake(s_wifi.state_mutex, pdMS_TO_TICKS(WIFI_MGR_TIMER_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                s_wifi.poor_rssi_count = 0;
                xSemaphoreGive(s_wifi.state_mutex);
            }
        }
    }
}

/**
 * @brief RSSI monitoring timer callback (unused - integrated into wifi_manager_update_rssi)
 *
 * This callback is not currently used as RSSI monitoring is integrated
 * into the uptime timer via wifi_manager_update_rssi().
 * Kept for potential future use with separate monitoring timer.
 */
static void __attribute__((unused)) rssi_monitor_timer_callback(void *arg)
{
    (void)arg;  /* Unused parameter */
    if (s_wifi.state != WIFI_STATE_CONNECTED) {
        return;
    }

    int8_t rssi = wifi_manager_get_rssi();
    if (rssi != 0) {
        wifi_manager_check_band_switch(rssi);
    }
}
#endif /* CONFIG_WIFI_AUTO_BAND_SWITCH */
