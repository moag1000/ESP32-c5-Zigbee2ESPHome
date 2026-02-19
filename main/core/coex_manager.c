/**
 * @file coex_manager.c
 * @brief WiFi/Zigbee/BLE Coexistence Manager Implementation
 *
 * Implements radio coordination for ESP32-C5's shared 2.4 GHz radio.
 * Uses ESP-IDF software coexistence when available and provides
 * application-level scheduling hints through BLE duty cycle adjustment.
 *
 * @copyright Copyright (c) 2024-2025
 * @license Apache License 2.0
 */

#include "coex_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include <string.h>

/* Include ESP-IDF coexistence header if available */
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif

/* Include BLE scanner for duty control integration */
#include "bluetooth/ble_scanner.h"

static const char *TAG = "coex_mgr";

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

/**
 * @brief Radio request tracking entry
 */
typedef struct {
    char name[COEX_REQUESTER_NAME_MAX];  /**< Requester identifier */
    coex_priority_t priority;             /**< Requested priority */
    uint32_t timestamp;                   /**< Request time (ms since boot) */
    bool active;                          /**< Slot is in use */
} coex_request_t;

/**
 * @brief Event callback registration
 */
typedef struct {
    coex_event_callback_t callback;
    void *ctx;
    bool active;
} coex_callback_entry_t;

/**
 * @brief Coexistence manager state
 */
typedef struct {
    bool initialized;
    SemaphoreHandle_t mutex;

    /* Radio activity tracking */
    bool wifi_active;
    bool zigbee_active;
    bool ble_scanning;
    bool permit_join_active;

    /* BLE control */
    uint8_t ble_scan_duty;
    bool ble_paused;
    esp_timer_handle_t ble_pause_timer;
    uint32_t ble_pause_end_time;

    /* Radio requests */
    coex_request_t requests[COEX_MAX_REQUESTERS];
    coex_priority_t highest_priority;

    /* Statistics */
    coex_stats_t stats;
    uint32_t init_time;

    /* Callbacks */
    coex_callback_entry_t callbacks[4];
} coex_state_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

static coex_state_t s_state = {0};

/* ============================================================================
 * Internal Functions - Forward Declarations
 * ============================================================================ */

static void coex_update_ble_duty_internal(void);
static void coex_recalc_highest_priority(void);
static void coex_fire_event(const char *event_type);
static void coex_ble_pause_timer_cb(void *arg);
static uint32_t coex_get_time_ms(void);

/* ============================================================================
 * Internal Functions - Implementation
 * ============================================================================ */

/**
 * @brief Get current time in milliseconds since boot
 */
static uint32_t coex_get_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * @brief Fire event to all registered callbacks
 */
static void coex_fire_event(const char *event_type)
{
    for (int i = 0; i < 4; i++) {
        if (s_state.callbacks[i].active && s_state.callbacks[i].callback) {
            s_state.callbacks[i].callback(event_type, s_state.callbacks[i].ctx);
        }
    }
}

/**
 * @brief Recalculate highest active priority
 */
static void coex_recalc_highest_priority(void)
{
    coex_priority_t highest = COEX_PRIO_LOW;

    for (int i = 0; i < COEX_MAX_REQUESTERS; i++) {
        if (s_state.requests[i].active) {
            if (s_state.requests[i].priority > highest) {
                highest = s_state.requests[i].priority;
            }
        }
    }

    s_state.highest_priority = highest;
}

/**
 * @brief Update BLE duty cycle based on current conditions
 *
 * Algorithm:
 *   - Base duty = 50%
 *   - If permit_join active: reduce to 30%
 *   - If critical priority request: reduce to 20%
 *   - If WiFi is very busy: reduce to 40%
 *   - If only BLE active (no Zigbee/WiFi): allow up to 100%
 */
static void coex_update_ble_duty_internal(void)
{
    uint8_t new_duty = COEX_DEFAULT_BLE_DUTY;

    /* Determine optimal duty based on system state */
    if (s_state.permit_join_active) {
        /* Zigbee pairing needs more radio time */
        new_duty = COEX_BLE_DUTY_ZB_PAIRING;
        ESP_LOGD(TAG, "Reducing BLE duty for permit_join: %u%%", new_duty);
    } else if (s_state.highest_priority >= COEX_PRIO_CRITICAL) {
        /* Critical operations need maximum radio time */
        new_duty = COEX_BLE_DUTY_ZB_FORMING;
        ESP_LOGD(TAG, "Reducing BLE duty for critical priority: %u%%", new_duty);
    } else if (s_state.wifi_active && s_state.zigbee_active) {
        /* Both WiFi and Zigbee competing, give BLE less time */
        new_duty = COEX_BLE_DUTY_WIFI_BUSY;
        ESP_LOGD(TAG, "Reducing BLE duty for WiFi+Zigbee: %u%%", new_duty);
    } else if (!s_state.zigbee_active && !s_state.wifi_active) {
        /* Only BLE active, can use full duty */
        new_duty = COEX_MAX_BLE_DUTY;
        ESP_LOGD(TAG, "Increasing BLE duty (radio free): %u%%", new_duty);
    }

    /* Apply change if different */
    if (new_duty != s_state.ble_scan_duty) {
        uint8_t old_duty = s_state.ble_scan_duty;
        s_state.ble_scan_duty = new_duty;
        s_state.stats.duty_adjustments++;

        /* Calculate scan interval/window based on duty */
        /* Assuming base interval of 3000ms (from BLE_DEFAULT_SCAN_INTERVAL_MS) */
        uint32_t interval_ms = BLE_DEFAULT_SCAN_INTERVAL_MS;
        uint32_t window_ms = (interval_ms * new_duty) / 100;

        /* Clamp window to reasonable bounds */
        if (window_ms < 50) window_ms = 50;
        if (window_ms > interval_ms) window_ms = interval_ms;

        ESP_LOGI(TAG, "BLE duty changed: %u%% -> %u%% (window=%lums/%lums)",
                 old_duty, new_duty, (unsigned long)window_ms, (unsigned long)interval_ms);

        /* Notify BLE scanner of new parameters if it's running */
        if (s_state.ble_scanning && ble_scanner_is_running()) {
            esp_err_t err = ble_scanner_set_interval(interval_ms);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to update BLE scan interval: %s", esp_err_to_name(err));
            }
        }

        coex_fire_event("duty_changed");
    }
}

/**
 * @brief Timer callback for BLE pause expiration
 */
static void coex_ble_pause_timer_cb(void *arg)
{
    (void)arg;

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    s_state.ble_paused = false;
    s_state.ble_pause_end_time = 0;

    ESP_LOGI(TAG, "BLE pause expired, resuming scanning");

    /* Resume BLE scanning if it was active before */
    if (s_state.ble_scanning) {
        esp_err_t err = ble_scanner_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to resume BLE scanner: %s", esp_err_to_name(err));
        }
    }

    xSemaphoreGive(s_state.mutex);

    coex_fire_event("ble_resumed");
}

/* ============================================================================
 * Public API - Initialization
 * ============================================================================ */

esp_err_t coex_manager_init(void)
{
    if (s_state.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing coexistence manager");

    /* Create mutex */
    s_state.mutex = xSemaphoreCreateMutex();
    if (s_state.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create BLE pause timer */
    esp_timer_create_args_t timer_args = {
        .callback = coex_ble_pause_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "coex_ble_pause"
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_state.ble_pause_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create BLE pause timer: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_state.mutex);
        return err;
    }

    /* Initialize state */
    memset(s_state.requests, 0, sizeof(s_state.requests));
    memset(s_state.callbacks, 0, sizeof(s_state.callbacks));
    memset(&s_state.stats, 0, sizeof(s_state.stats));

    s_state.ble_scan_duty = COEX_DEFAULT_BLE_DUTY;
    s_state.highest_priority = COEX_PRIO_LOW;
    s_state.init_time = coex_get_time_ms();

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    /* ESP-IDF software coexistence is enabled via menuconfig */
    /* The coexistence module handles most arbitration automatically */
    ESP_LOGI(TAG, "ESP-IDF SW coexistence enabled");
#else
    ESP_LOGW(TAG, "ESP-IDF SW coexistence not enabled in menuconfig");
#endif

    s_state.initialized = true;

    ESP_LOGI(TAG, "Coexistence manager initialized (BLE duty=%u%%)", s_state.ble_scan_duty);

    return ESP_OK;
}

esp_err_t coex_manager_deinit(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing coexistence manager");

    /* Stop and delete timer */
    if (s_state.ble_pause_timer) {
        esp_timer_stop(s_state.ble_pause_timer);
        esp_timer_delete(s_state.ble_pause_timer);
        s_state.ble_pause_timer = NULL;
    }

    /* Delete mutex */
    if (s_state.mutex) {
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
    }

    s_state.initialized = false;

    return ESP_OK;
}

bool coex_manager_is_initialized(void)
{
    return s_state.initialized;
}

/* ============================================================================
 * Public API - Radio Access Management
 * ============================================================================ */

esp_err_t coex_request_radio(coex_priority_t prio, const char *requester)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (requester == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    /* Check if requester already exists - update priority */
    for (int i = 0; i < COEX_MAX_REQUESTERS; i++) {
        if (s_state.requests[i].active &&
            strncmp(s_state.requests[i].name, requester, COEX_REQUESTER_NAME_MAX - 1) == 0) {
            s_state.requests[i].priority = prio;
            s_state.requests[i].timestamp = coex_get_time_ms();
            coex_recalc_highest_priority();
            coex_update_ble_duty_internal();
            xSemaphoreGive(s_state.mutex);
            ESP_LOGD(TAG, "Updated priority for '%s': %s", requester, coex_priority_to_str(prio));
            return ESP_OK;
        }
    }

    /* Find empty slot */
    int slot = -1;
    for (int i = 0; i < COEX_MAX_REQUESTERS; i++) {
        if (!s_state.requests[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGW(TAG, "No free slots for requester '%s'", requester);
        return ESP_ERR_NO_MEM;
    }

    /* Add new request */
    strncpy(s_state.requests[slot].name, requester, COEX_REQUESTER_NAME_MAX - 1);
    s_state.requests[slot].name[COEX_REQUESTER_NAME_MAX - 1] = '\0';
    s_state.requests[slot].priority = prio;
    s_state.requests[slot].timestamp = coex_get_time_ms();
    s_state.requests[slot].active = true;

    s_state.stats.radio_requests_total++;

    coex_recalc_highest_priority();
    coex_update_ble_duty_internal();

    xSemaphoreGive(s_state.mutex);

    ESP_LOGD(TAG, "Radio requested by '%s' at priority %s", requester, coex_priority_to_str(prio));

    return ESP_OK;
}

void coex_release_radio(const char *requester)
{
    if (!s_state.initialized || requester == NULL) {
        return;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    for (int i = 0; i < COEX_MAX_REQUESTERS; i++) {
        if (s_state.requests[i].active &&
            strncmp(s_state.requests[i].name, requester, COEX_REQUESTER_NAME_MAX - 1) == 0) {
            s_state.requests[i].active = false;
            s_state.requests[i].name[0] = '\0';

            ESP_LOGD(TAG, "Radio released by '%s'", requester);

            coex_recalc_highest_priority();
            coex_update_ble_duty_internal();
            break;
        }
    }

    xSemaphoreGive(s_state.mutex);
}

esp_err_t coex_update_priority(const char *requester, coex_priority_t new_prio)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (requester == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    esp_err_t ret = ESP_ERR_NOT_FOUND;

    for (int i = 0; i < COEX_MAX_REQUESTERS; i++) {
        if (s_state.requests[i].active &&
            strncmp(s_state.requests[i].name, requester, COEX_REQUESTER_NAME_MAX - 1) == 0) {
            s_state.requests[i].priority = new_prio;
            coex_recalc_highest_priority();
            coex_update_ble_duty_internal();
            ret = ESP_OK;
            ESP_LOGD(TAG, "Priority updated for '%s': %s", requester, coex_priority_to_str(new_prio));
            break;
        }
    }

    xSemaphoreGive(s_state.mutex);

    return ret;
}

/* ============================================================================
 * Public API - BLE Scan Control
 * ============================================================================ */

void coex_set_ble_scan_duty(uint8_t duty_percent)
{
    if (!s_state.initialized) {
        return;
    }

    /* Clamp to valid range */
    if (duty_percent < COEX_MIN_BLE_DUTY) {
        duty_percent = COEX_MIN_BLE_DUTY;
    } else if (duty_percent > COEX_MAX_BLE_DUTY) {
        duty_percent = COEX_MAX_BLE_DUTY;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    if (duty_percent != s_state.ble_scan_duty) {
        ESP_LOGI(TAG, "BLE scan duty manually set: %u%% -> %u%%", s_state.ble_scan_duty, duty_percent);
        s_state.ble_scan_duty = duty_percent;
        s_state.stats.duty_adjustments++;
        coex_fire_event("duty_changed");
    }

    xSemaphoreGive(s_state.mutex);
}

uint8_t coex_get_ble_scan_duty(void)
{
    return s_state.ble_scan_duty;
}

void coex_pause_ble_scan(uint32_t duration_ms)
{
    if (!s_state.initialized) {
        return;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    if (duration_ms == 0) {
        /* Immediate resume */
        if (s_state.ble_paused) {
            esp_timer_stop(s_state.ble_pause_timer);
            s_state.ble_paused = false;
            s_state.ble_pause_end_time = 0;

            /* Resume scanner */
            if (s_state.ble_scanning) {
                ble_scanner_start();
            }

            coex_fire_event("ble_resumed");
        }
        xSemaphoreGive(s_state.mutex);
        return;
    }

    ESP_LOGI(TAG, "Pausing BLE scan for %lu ms", (unsigned long)duration_ms);

    /* Stop existing timer if running */
    esp_timer_stop(s_state.ble_pause_timer);

    s_state.ble_paused = true;
    s_state.ble_pause_end_time = coex_get_time_ms() + duration_ms;
    s_state.stats.ble_pauses++;

    /* Stop BLE scanner */
    if (ble_scanner_is_running()) {
        ble_scanner_stop();
    }

    /* Start resume timer */
    esp_timer_start_once(s_state.ble_pause_timer, duration_ms * 1000);

    xSemaphoreGive(s_state.mutex);

    coex_fire_event("ble_paused");
}

void coex_resume_ble_scan(void)
{
    coex_pause_ble_scan(0);  /* duration 0 = immediate resume */
}

bool coex_is_ble_paused(void)
{
    return s_state.ble_paused;
}

/* ============================================================================
 * Public API - Radio Activity Notifications
 * ============================================================================ */

void coex_notify_wifi_activity(bool active)
{
    if (!s_state.initialized) {
        return;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_state.wifi_active != active) {
        s_state.wifi_active = active;
        ESP_LOGD(TAG, "WiFi activity: %s", active ? "active" : "idle");
        coex_update_ble_duty_internal();
    }

    xSemaphoreGive(s_state.mutex);
}

void coex_notify_zigbee_activity(bool active)
{
    if (!s_state.initialized) {
        return;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_state.zigbee_active != active) {
        s_state.zigbee_active = active;
        ESP_LOGD(TAG, "Zigbee activity: %s", active ? "active" : "idle");
        coex_update_ble_duty_internal();
    }

    xSemaphoreGive(s_state.mutex);
}

void coex_notify_permit_join(bool active)
{
    if (!s_state.initialized) {
        return;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_state.permit_join_active != active) {
        s_state.permit_join_active = active;
        ESP_LOGI(TAG, "Zigbee permit_join: %s", active ? "enabled" : "disabled");
        coex_update_ble_duty_internal();

        /* Optionally pause BLE during permit_join for better pairing reliability */
        if (active) {
            /* Could pause BLE here, but we'll just reduce duty instead */
            /* coex_pause_ble_scan(30000); // 30 seconds */
        }
    }

    xSemaphoreGive(s_state.mutex);
}

void coex_notify_ble_scanning(bool scanning)
{
    if (!s_state.initialized) {
        return;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_state.ble_scanning != scanning) {
        s_state.ble_scanning = scanning;
        ESP_LOGD(TAG, "BLE scanning: %s", scanning ? "started" : "stopped");

        /* If scanning started and we're paused, stop it again */
        if (scanning && s_state.ble_paused) {
            ESP_LOGD(TAG, "BLE started while paused, stopping");
            ble_scanner_stop();
        }
    }

    xSemaphoreGive(s_state.mutex);
}

/* ============================================================================
 * Public API - Status & Statistics
 * ============================================================================ */

void coex_get_status(coex_status_t *status)
{
    if (status == NULL) {
        return;
    }

    if (!s_state.initialized) {
        memset(status, 0, sizeof(coex_status_t));
        return;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    status->wifi_active = s_state.wifi_active;
    status->zigbee_active = s_state.zigbee_active;
    status->ble_scanning = s_state.ble_scanning && !s_state.ble_paused;
    status->ble_scan_duty = s_state.ble_scan_duty;
    status->current_priority = s_state.highest_priority;
    status->permit_join_active = s_state.permit_join_active;
    status->ble_paused = s_state.ble_paused;

    /* Calculate remaining pause time */
    if (s_state.ble_paused && s_state.ble_pause_end_time > 0) {
        uint32_t now = coex_get_time_ms();
        if (s_state.ble_pause_end_time > now) {
            status->ble_pause_remaining_ms = s_state.ble_pause_end_time - now;
        } else {
            status->ble_pause_remaining_ms = 0;
        }
    } else {
        status->ble_pause_remaining_ms = 0;
    }

    /* Count active requesters */
    status->active_requesters = 0;
    for (int i = 0; i < COEX_MAX_REQUESTERS; i++) {
        if (s_state.requests[i].active) {
            status->active_requesters++;
        }
    }

    xSemaphoreGive(s_state.mutex);
}

void coex_get_stats(coex_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    if (!s_state.initialized) {
        memset(stats, 0, sizeof(coex_stats_t));
        return;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    *stats = s_state.stats;
    stats->uptime_ms = coex_get_time_ms() - s_state.init_time;

    xSemaphoreGive(s_state.mutex);
}

void coex_reset_stats(void)
{
    if (!s_state.initialized) {
        return;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    memset(&s_state.stats, 0, sizeof(s_state.stats));

    xSemaphoreGive(s_state.mutex);

    ESP_LOGI(TAG, "Statistics reset");
}

const char *coex_priority_to_str(coex_priority_t prio)
{
    switch (prio) {
        case COEX_PRIO_LOW:      return "LOW";
        case COEX_PRIO_NORMAL:   return "NORMAL";
        case COEX_PRIO_HIGH:     return "HIGH";
        case COEX_PRIO_CRITICAL: return "CRITICAL";
        default:                 return "UNKNOWN";
    }
}

const char *coex_radio_to_str(coex_radio_t radio)
{
    switch (radio) {
        case COEX_RADIO_WIFI:   return "WIFI";
        case COEX_RADIO_ZIGBEE: return "ZIGBEE";
        case COEX_RADIO_BLE:    return "BLE";
        default:                return "UNKNOWN";
    }
}

/* ============================================================================
 * Public API - Event Callbacks
 * ============================================================================ */

esp_err_t coex_register_callback(coex_event_callback_t callback, void *ctx)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    /* Find empty slot */
    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (!s_state.callbacks[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NO_MEM;
    }

    s_state.callbacks[slot].callback = callback;
    s_state.callbacks[slot].ctx = ctx;
    s_state.callbacks[slot].active = true;

    xSemaphoreGive(s_state.mutex);

    return ESP_OK;
}

esp_err_t coex_unregister_callback(coex_event_callback_t callback)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    esp_err_t ret = ESP_ERR_NOT_FOUND;

    for (int i = 0; i < 4; i++) {
        if (s_state.callbacks[i].active && s_state.callbacks[i].callback == callback) {
            s_state.callbacks[i].active = false;
            s_state.callbacks[i].callback = NULL;
            s_state.callbacks[i].ctx = NULL;
            ret = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_state.mutex);

    return ret;
}
