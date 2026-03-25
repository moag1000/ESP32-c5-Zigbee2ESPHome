/**
 * @file led_status_manager.c
 * @brief LED Status Manager Implementation
 *
 * Centralized LED status coordination with priority-based status handling
 * and thread-safe condition management.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "led_status_manager.h"
#include "led_controller.h"
#include "core/events/event_bus.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "LED_STATUS";

/* ============================================================================
 * Static State
 * ============================================================================ */

/** @brief Condition bitmap - each bit represents an active condition */
static uint32_t s_conditions = 0;

/** @brief Mutex for thread-safe condition updates */
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Initialization flag */
static bool s_initialized = false;

/* ============================================================================
 * Priority Evaluation
 * ============================================================================ */

/**
 * @brief Evaluate current conditions and determine appropriate LED status
 *
 * Uses a priority-based evaluation where higher-priority conditions
 * take precedence over lower ones.
 *
 * Priority table:
 * 10 - OTA_ACTIVE      -> LED_STATUS_OTA (purple breathing)
 *  9 - SYSTEM_ERROR    -> LED_STATUS_ERROR (red fast blink)
 *  8 - SYSTEM_WARNING  -> LED_STATUS_WARNING (orange slow blink)
 *  7 - PERMIT_JOIN     -> LED_STATUS_PAIRING (cyan breathing)
 *  6 - WIFI_CONNECTING -> LED_STATUS_WIFI_CONNECTING (yellow slow blink)
 *  5 - NORMAL (WiFi+Boot) -> LED_STATUS_NORMAL (green solid)
 *  4 - BOOT (default)  -> LED_STATUS_BOOT (blue breathing)
 *
 * @return The appropriate LED status based on current conditions
 */
static led_status_t evaluate_status(void)
{
    /* Priority 10: OTA active takes highest priority */
    if (s_conditions & LED_COND_OTA_ACTIVE) {
        return LED_STATUS_OTA;
    }

    /* Priority 9: System error */
    if (s_conditions & LED_COND_SYSTEM_ERROR) {
        return LED_STATUS_ERROR;
    }

    /* Priority 8: System warning */
    if (s_conditions & LED_COND_SYSTEM_WARNING) {
        return LED_STATUS_WARNING;
    }

    /* Priority 7: Permit join / pairing mode */
    if (s_conditions & LED_COND_PERMIT_JOIN) {
        return LED_STATUS_PAIRING;
    }

    /* Priority 6: WiFi connecting (only if not already connected) */
    if ((s_conditions & LED_COND_WIFI_CONNECTING) &&
        !(s_conditions & LED_COND_WIFI_CONNECTED)) {
        return LED_STATUS_WIFI_CONNECTING;
    }

    /* Priority 5: Normal operation (WiFi connected, boot complete) */
    if ((s_conditions & LED_COND_BOOT_COMPLETE) &&
        (s_conditions & LED_COND_WIFI_CONNECTED)) {
        return LED_STATUS_NORMAL;
    }

    /* Priority 4: Boot phase (default) */
    return LED_STATUS_BOOT;
}

/**
 * @brief Apply the evaluated status to the LED controller
 */
static void apply_status(void)
{
#if CONFIG_GW_LED_ENABLED
    led_status_t status = evaluate_status();
    led_set_status(status);
    ESP_LOGD(TAG, "LED status updated to %d (conditions=0x%08lx)",
             status, (unsigned long)s_conditions);
#endif
}

/* ============================================================================
 * Event Bus Handler
 * ============================================================================ */

/**
 * @brief Handle network events from event bus
 *
 * Updates LED conditions based on WiFi/MQTT/Zigbee connectivity events.
 */
static void led_event_handler(event_type_t type, void *data, size_t data_size, void *user_ctx)
{
    (void)data;
    (void)data_size;
    (void)user_ctx;

    switch (type) {
        case EVT_WIFI_CONNECTED:
            led_status_manager_set_condition(LED_COND_WIFI_CONNECTING, false);
            led_status_manager_set_condition(LED_COND_WIFI_CONNECTED, true);
            break;

        case EVT_WIFI_DISCONNECTED:
            led_status_manager_set_condition(LED_COND_WIFI_CONNECTED, false);
            led_status_manager_set_condition(LED_COND_WIFI_CONNECTING, true);
            break;

        case EVT_MQTT_CONNECTED:
            led_status_manager_set_condition(LED_COND_MQTT_CONNECTED, true);
            break;

        case EVT_MQTT_DISCONNECTED:
            led_status_manager_set_condition(LED_COND_MQTT_CONNECTED, false);
            break;

        case EVT_NETWORK_READY:
            led_status_manager_set_condition(LED_COND_ZIGBEE_RUNNING, true);
            break;

        default:
            break;
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t led_status_manager_init(void)
{
#if CONFIG_GW_LED_ENABLED
    if (s_initialized) {
        ESP_LOGW(TAG, "LED status manager already initialized");
        return ESP_OK;
    }

    /* Create mutex for thread safety */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize with boot status (no conditions set yet) */
    s_conditions = 0;
    s_initialized = true;

    /* Set initial boot status */
    led_set_status(LED_STATUS_BOOT);

    /* Subscribe to network events from event bus */
    event_subscribe(EVT_WIFI_CONNECTED, led_event_handler, NULL);
    event_subscribe(EVT_WIFI_DISCONNECTED, led_event_handler, NULL);
    event_subscribe(EVT_MQTT_CONNECTED, led_event_handler, NULL);
    event_subscribe(EVT_MQTT_DISCONNECTED, led_event_handler, NULL);
    event_subscribe(EVT_NETWORK_READY, led_event_handler, NULL);

    ESP_LOGI(TAG, "LED status manager initialized (event bus subscribed)");
    return ESP_OK;
#else
    ESP_LOGD(TAG, "LED disabled in config");
    return ESP_OK;
#endif
}

void led_status_manager_deinit(void)
{
#if CONFIG_GW_LED_ENABLED
    if (!s_initialized) {
        return;
    }

    /* Unsubscribe event bus handlers */
    event_unsubscribe(EVT_WIFI_CONNECTED, led_event_handler);
    event_unsubscribe(EVT_WIFI_DISCONNECTED, led_event_handler);
    event_unsubscribe(EVT_MQTT_CONNECTED, led_event_handler);
    event_unsubscribe(EVT_MQTT_DISCONNECTED, led_event_handler);
    event_unsubscribe(EVT_NETWORK_READY, led_event_handler);

    s_initialized = false;

    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    ESP_LOGI(TAG, "LED status manager deinitialized");
#endif
}

void led_status_manager_set_condition(led_condition_t condition, bool active)
{
#if CONFIG_GW_LED_ENABLED
    if (!s_initialized || s_mutex == NULL) {
        return;
    }

    /* Acquire mutex with timeout */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for set_condition");
        return;
    }

    uint32_t old_conditions = s_conditions;

    if (active) {
        s_conditions |= (uint32_t)condition;
    } else {
        s_conditions &= ~(uint32_t)condition;
    }

    /* Only update LED if conditions changed */
    if (s_conditions != old_conditions) {
        ESP_LOGD(TAG, "Condition %s: 0x%x -> conditions=0x%08lx",
                 active ? "set" : "clear", condition, (unsigned long)s_conditions);
        apply_status();
    }

    xSemaphoreGive(s_mutex);
#else
    (void)condition;
    (void)active;
#endif
}

bool led_status_manager_get_condition(led_condition_t condition)
{
#if CONFIG_GW_LED_ENABLED
    if (!s_initialized) {
        return false;
    }

    return (s_conditions & (uint32_t)condition) != 0;
#else
    (void)condition;
    return false;
#endif
}

void led_status_manager_notify(int notify)
{
#if CONFIG_GW_LED_ENABLED
    if (!s_initialized) {
        return;
    }

    /* Validate and cast to led_notify_t */
    if (notify < LED_NOTIFY_DEVICE_NEW || notify > LED_NOTIFY_DEVICE_FAILED) {
        ESP_LOGW(TAG, "Invalid notification type: %d", notify);
        return;
    }

    /* Trigger the blink notification */
    led_blink_notify((led_notify_t)notify);
    ESP_LOGD(TAG, "Triggered notification: %d", notify);
#else
    (void)notify;
#endif
}

void led_status_manager_update(void)
{
#if CONFIG_GW_LED_ENABLED
    if (!s_initialized || s_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        apply_status();
        xSemaphoreGive(s_mutex);
    }
#endif
}

uint32_t led_status_manager_get_conditions(void)
{
#if CONFIG_GW_LED_ENABLED
    return s_conditions;
#else
    return 0;
#endif
}
