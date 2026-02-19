/**
 * @file lifecycle.c
 * @brief Lifecycle state machine implementation for ESP32-C5 Zigbee2MQTT NG
 *
 * Implements the lifecycle state machine with valid transitions,
 * callback management, and state tracking.
 *
 * @copyright Copyright (c) 2024-2025
 */

#include "lifecycle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "LIFECYCLE";

/** Maximum number of callbacks that can be registered */
#define LIFECYCLE_MAX_CALLBACKS 8

/** Number of lifecycle states */
#define LIFECYCLE_STATE_COUNT 7

/**
 * @brief Callback entry structure
 */
typedef struct {
    lifecycle_callback_t callback;
    void *ctx;
} lifecycle_callback_entry_t;

/** Current lifecycle state */
static lifecycle_state_t s_current_state = LIFECYCLE_BOOT;

/** Timestamp when current state was entered (microseconds) */
static int64_t s_state_entered_time_us = 0;

/** Free heap when current state was entered */
static size_t s_heap_at_entry = 0;

/** Whether lifecycle has been initialized */
static bool s_initialized = false;

/** Registered callbacks */
static lifecycle_callback_entry_t s_callbacks[LIFECYCLE_MAX_CALLBACKS];

/** Number of registered callbacks */
static size_t s_callback_count = 0;

/**
 * @brief State transition table
 *
 * Each row represents a source state, each column a target state.
 * true = transition allowed, false = transition not allowed.
 *
 * Valid transitions:
 *   BOOT -> PROVISIONING, RUNNING
 *   PROVISIONING -> RUNNING
 *   RUNNING -> PAIRING, LOW_MEMORY, OTA, SHUTDOWN
 *   PAIRING -> RUNNING
 *   LOW_MEMORY -> RUNNING
 *   OTA -> SHUTDOWN, RUNNING
 */
static const bool s_transition_table[LIFECYCLE_STATE_COUNT][LIFECYCLE_STATE_COUNT] = {
    /*                      BOOT   PROV   PAIR   RUN    LOW    OTA    SHUT  */
    /* BOOT         */ {   false, true,  false, true,  false, false, false },
    /* PROVISIONING */ {   false, false, false, true,  false, false, false },
    /* PAIRING      */ {   false, false, false, true,  false, false, false },
    /* RUNNING      */ {   false, false, true,  false, true,  true,  true  },
    /* LOW_MEMORY   */ {   false, false, false, true,  false, false, false },
    /* OTA          */ {   false, false, false, true,  false, false, true  },
    /* SHUTDOWN     */ {   false, false, false, false, false, false, false },
};

/**
 * @brief State name strings for logging
 */
static const char *s_state_names[] = {
    "BOOT",
    "PROVISIONING",
    "PAIRING",
    "RUNNING",
    "LOW_MEMORY",
    "OTA",
    "SHUTDOWN",
};

/**
 * @brief Check if a state value is valid
 *
 * @param state State to validate
 * @return true if valid, false otherwise
 */
static bool lifecycle_is_valid_state(lifecycle_state_t state)
{
    return state >= LIFECYCLE_BOOT && state <= LIFECYCLE_SHUTDOWN;
}

/**
 * @brief Check if a transition is valid
 *
 * @param from Source state
 * @param to Target state
 * @return true if transition is allowed, false otherwise
 */
static bool lifecycle_is_valid_transition(lifecycle_state_t from, lifecycle_state_t to)
{
    if (!lifecycle_is_valid_state(from) || !lifecycle_is_valid_state(to)) {
        return false;
    }
    return s_transition_table[from][to];
}

/**
 * @brief Notify all registered callbacks
 *
 * @param old_state Previous state
 * @param new_state New state
 */
static void lifecycle_notify_callbacks(lifecycle_state_t old_state, lifecycle_state_t new_state)
{
    for (size_t i = 0; i < s_callback_count; i++) {
        if (s_callbacks[i].callback != NULL) {
            s_callbacks[i].callback(old_state, new_state, s_callbacks[i].ctx);
        }
    }
}

/**
 * @brief Update state tracking information
 */
static void lifecycle_update_state_info(void)
{
    s_state_entered_time_us = esp_timer_get_time();
    s_heap_at_entry = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

esp_err_t lifecycle_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Initialize to BOOT state */
    s_current_state = LIFECYCLE_BOOT;
    s_callback_count = 0;
    memset(s_callbacks, 0, sizeof(s_callbacks));

    /* Record initial state info */
    lifecycle_update_state_info();

    s_initialized = true;

    ESP_LOGI(TAG, "Initialized in BOOT state, free heap: %u bytes",
             (unsigned int)s_heap_at_entry);

    return ESP_OK;
}

esp_err_t lifecycle_transition(lifecycle_state_t new_state)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!lifecycle_is_valid_state(new_state)) {
        ESP_LOGE(TAG, "Invalid state: %d", (int)new_state);
        return ESP_ERR_INVALID_ARG;
    }

    lifecycle_state_t old_state = s_current_state;

    /* Check if transition is allowed */
    if (!lifecycle_is_valid_transition(old_state, new_state)) {
        ESP_LOGE(TAG, "Invalid transition: %s -> %s",
                 lifecycle_state_to_str(old_state),
                 lifecycle_state_to_str(new_state));
        return ESP_ERR_INVALID_STATE;
    }

    /* Notify callbacks before transition */
    lifecycle_notify_callbacks(old_state, new_state);

    /* Update state */
    s_current_state = new_state;
    lifecycle_update_state_info();

    ESP_LOGI(TAG, "State transition: %s -> %s (free heap: %u bytes)",
             lifecycle_state_to_str(old_state),
             lifecycle_state_to_str(new_state),
             (unsigned int)s_heap_at_entry);

    return ESP_OK;
}

lifecycle_state_t lifecycle_get_state(void)
{
    return s_current_state;
}

void lifecycle_get_info(lifecycle_info_t *info)
{
    if (info == NULL) {
        return;
    }

    info->state = s_current_state;
    /* Convert from microseconds to milliseconds */
    info->state_entered_time = (uint32_t)(s_state_entered_time_us / 1000);
    info->heap_at_entry = s_heap_at_entry;
}

esp_err_t lifecycle_register_callback(lifecycle_callback_t cb, void *ctx)
{
    if (cb == NULL) {
        ESP_LOGE(TAG, "Callback is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_callback_count >= LIFECYCLE_MAX_CALLBACKS) {
        ESP_LOGE(TAG, "Maximum callbacks (%d) reached", LIFECYCLE_MAX_CALLBACKS);
        return ESP_ERR_NO_MEM;
    }

    s_callbacks[s_callback_count].callback = cb;
    s_callbacks[s_callback_count].ctx = ctx;
    s_callback_count++;

    ESP_LOGD(TAG, "Callback registered (%zu/%d)", s_callback_count, LIFECYCLE_MAX_CALLBACKS);

    return ESP_OK;
}

const char *lifecycle_state_to_str(lifecycle_state_t state)
{
    if (!lifecycle_is_valid_state(state)) {
        return "UNKNOWN";
    }
    return s_state_names[state];
}
