/**
 * @file ble_scanner.c
 * @brief BLE Passive Scanner Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_scanner.h"
#include "ble_manager.h"
#include "core/memory/memory_manager_ng.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include <string.h>

#if CONFIG_BT_SCANNER_ENABLED

#include "host/ble_hs.h"
#include "host/ble_gap.h"

static const char *TAG = "BLE_SCANNER";

/* Maximum number of registered callbacks */
#define MAX_ADV_CALLBACKS       8

/* BLE time unit conversion
 * BLE uses 0.625ms units for intervals/windows
 * To convert ms to BLE units: (ms * 1000) / 625
 * Example: 100ms = (100 * 1000) / 625 = 160 BLE units
 */
#define BLE_TIME_UNIT_US        625     /* BLE unit = 0.625ms = 625us */
#define MS_TO_US                1000    /* Milliseconds to microseconds */

/* Scan window limits in milliseconds */
#define SCAN_WINDOW_MIN_MS      10      /* Minimum scan window */
#define SCAN_WINDOW_MAX_MS      100     /* Maximum scan window */
#define SCAN_WINDOW_PERCENT     10      /* Window as percentage of interval */

/* Retry configuration for transient controller errors (BLE_ERR_MEM_CAPACITY).
 * During startup the BLE controller may lack buffers while WiFi/Zigbee coexistence settles. */
#define SCAN_START_MAX_RETRIES  3       /* Maximum retry attempts */
#define SCAN_START_RETRY_MS     200     /* Delay between retries in ms */

/* NimBLE HCI error: Memory Capacity Exceeded (HCI 0x07 + BLE_HS base 0x200 = 519) */
#define NIMBLE_ERR_MEM_CAPACITY 519

/* RSSI exponential moving average alpha using fixed-point arithmetic
 * Alpha = 77/256 ≈ 0.30 (30% weight for new value)
 * Formula: new_avg = (alpha * new + (256 - alpha) * old) >> 8
 * This avoids floating-point operations on RISC-V single-core
 */
#define RSSI_EMA_ALPHA_FP       77      /* Fixed-point alpha (77/256 ≈ 0.30) */
#define RSSI_EMA_SCALE          256     /* Fixed-point scale factor */

/* Maximum extended advertisement callbacks */
#define MAX_EXT_ADV_CALLBACKS   4

/**
 * @brief Guard macro for public API functions requiring initialized scanner mutex
 *
 * Returns the specified error if the scanner mutex has not been created
 * (i.e., ble_scanner_init() was never called or failed).
 */
#define BLE_SCANNER_CHECK_MUTEX(error_return) \
    do { \
        if (s_scanner_mutex == NULL) { \
            ESP_LOGE(TAG, "%s: Scanner not initialized", __func__); \
            return error_return; \
        } \
    } while (0)

/* Scanner state */
static ble_scanner_state_t s_scanner_state = BLE_SCANNER_STATE_UNINITIALIZED;
static SemaphoreHandle_t s_scanner_mutex = NULL;

/* Scan parameters */
static uint32_t s_scan_interval_ms = BLE_DEFAULT_SCAN_INTERVAL_MS;
static uint32_t s_scan_window_ms = BLE_DEFAULT_SCAN_WINDOW_MS;

/* BLE 5.0 Extended Scanning configuration */
#if MYNEWT_VAL(BLE_EXT_ADV)
static bool s_extended_scanning_enabled = true;
#else
static bool s_extended_scanning_enabled = false;
#endif
static bool s_coded_phy_enabled = false;
static bool s_2m_phy_enabled = false;

/* Extended advertisement callbacks */
static ble_ext_adv_callback_t s_ext_callbacks[MAX_EXT_ADV_CALLBACKS] = {NULL};
static size_t s_ext_callback_count = 0;

/* Tracked devices - allocated in PSRAM to save internal RAM (~3.6KB) */
static ble_tracked_device_t *s_tracked_devices = NULL;
static size_t s_device_count = 0;

/** @brief Active scanning mode flag */
static bool s_active_scan_enabled = false;

/* Statistics */
static ble_scanner_stats_t s_stats = {0};

/* Callbacks */
static ble_adv_callback_t s_callbacks[MAX_ADV_CALLBACKS] = {NULL};
static size_t s_callback_count = 0;

/* State change callback (for ESPHome BLE proxy) */
static ble_scanner_state_callback_t s_state_callback = NULL;

/* State string lookup */
static const char *s_state_strings[] = {
    "UNINITIALIZED",
    "INITIALIZED",
    "STARTING",
    "RUNNING",
    "STOPPING",
    "ERROR"
};

/* Forward declarations */
static void set_scanner_state(ble_scanner_state_t state);
static int ble_gap_event_handler(struct ble_gap_event *event, void *arg);
static void process_advertisement(const struct ble_gap_disc_desc *disc);
static ble_tracked_device_t *find_or_create_device(const uint8_t *mac, ble_addr_type_t addr_type);
static void invoke_callbacks(const ble_adv_data_t *adv);
static uint32_t get_timestamp_ms(void);
#if MYNEWT_VAL(BLE_EXT_ADV)
static void process_ext_advertisement(const struct ble_gap_ext_disc_desc *ext_disc);
static void invoke_ext_callbacks(const ble_ext_adv_data_t *ext_adv);
static ble_phy_t nimble_phy_to_ble_phy(uint8_t nimble_phy);
#endif

esp_err_t ble_scanner_init(void)
{
    if (s_scanner_state != BLE_SCANNER_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "Scanner already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing BLE scanner...");

    /* Create scanner mutex */
    s_scanner_mutex = xSemaphoreCreateMutex();
    if (s_scanner_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create scanner mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate tracked devices array in PSRAM (saves ~3.6KB internal RAM) */
    if (s_tracked_devices == NULL) {
        s_tracked_devices = mem_ng_calloc(BLE_MAX_TRACKED_DEVICES, sizeof(ble_tracked_device_t),
                                           MEM_CAP_PSRAM);
        if (!s_tracked_devices) {
            ESP_LOGW(TAG, "PSRAM alloc failed, falling back to internal RAM");
            s_tracked_devices = mem_ng_calloc(BLE_MAX_TRACKED_DEVICES, sizeof(ble_tracked_device_t),
                                               MEM_CAP_DEFAULT);
        }
        if (!s_tracked_devices) {
            ESP_LOGE(TAG, "Failed to allocate tracked devices array");
            vSemaphoreDelete(s_scanner_mutex);
            s_scanner_mutex = NULL;
            return ESP_ERR_NO_MEM;
        }
    } else {
        memset(s_tracked_devices, 0, BLE_MAX_TRACKED_DEVICES * sizeof(ble_tracked_device_t));
    }
    s_device_count = 0;

    /* Reset statistics */
    memset(&s_stats, 0, sizeof(s_stats));

    /* Clear callbacks */
    memset(s_callbacks, 0, sizeof(s_callbacks));
    s_callback_count = 0;

    /* Read active scan configuration from Kconfig */
#ifdef CONFIG_BT_ACTIVE_SCAN_ENABLED
    s_active_scan_enabled = true;
#endif

    set_scanner_state(BLE_SCANNER_STATE_INITIALIZED);
    ESP_LOGI(TAG, "BLE scanner initialized (max devices: %d, extended: %s)",
             BLE_MAX_TRACKED_DEVICES, s_extended_scanning_enabled ? "yes" : "no");

    return ESP_OK;
}

esp_err_t ble_scanner_init_with_config(const ble_scanner_config_t *config)
{
    if (s_scanner_state != BLE_SCANNER_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "Scanner already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Apply configuration */
    if (config != NULL) {
        s_extended_scanning_enabled = config->enable_extended_scanning;
        s_coded_phy_enabled = config->enable_coded_phy;
        s_2m_phy_enabled = config->enable_2m_phy;
        s_scan_interval_ms = config->scan_interval_ms;
        s_scan_window_ms = config->scan_window_ms;
    }

    ESP_LOGI(TAG, "BLE 5.0 config: extended=%d, 2M_PHY=%d, coded_PHY=%d",
             s_extended_scanning_enabled, s_2m_phy_enabled, s_coded_phy_enabled);

    return ble_scanner_init();
}

esp_err_t ble_scanner_deinit(void)
{
    if (s_scanner_state == BLE_SCANNER_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "Scanner not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing BLE scanner...");

    /* Stop scanning if running */
    if (s_scanner_state == BLE_SCANNER_STATE_RUNNING) {
        ble_scanner_stop();
    }

    /* Clear tracked devices */
    ble_scanner_clear_devices();

    /* Delete mutex */
    if (s_scanner_mutex != NULL) {
        vSemaphoreDelete(s_scanner_mutex);
        s_scanner_mutex = NULL;
    }

    set_scanner_state(BLE_SCANNER_STATE_UNINITIALIZED);
    ESP_LOGI(TAG, "BLE scanner deinitialized");

    return ESP_OK;
}

/**
 * @brief Attempt a single scan start (extended or legacy)
 *
 * @param[out] used_extended Set to true if extended scanning was started
 * @return NimBLE error code (0 = success)
 */
static int attempt_scan_start(bool *used_extended)
{
    int rc = -1;

    *used_extended = false;

#if MYNEWT_VAL(BLE_EXT_ADV)
    /* Use BLE 5.0 Extended Scanning if enabled and supported */
    if (s_extended_scanning_enabled) {
        struct ble_gap_ext_disc_params ext_params_1m = {
            .passive = !s_active_scan_enabled,
            .itvl = (uint16_t)(s_scan_interval_ms * MS_TO_US / BLE_TIME_UNIT_US),
            .window = (uint16_t)(s_scan_window_ms * MS_TO_US / BLE_TIME_UNIT_US),
        };

        struct ble_gap_ext_disc_params ext_params_coded = {
            .passive = !s_active_scan_enabled,
            .itvl = (uint16_t)(s_scan_interval_ms * MS_TO_US / BLE_TIME_UNIT_US),
            .window = (uint16_t)(s_scan_window_ms * MS_TO_US / BLE_TIME_UNIT_US),
        };

        ESP_LOGD(TAG, "Trying extended scan (PHYs: 1M%s)",
                 s_coded_phy_enabled ? "+Coded" : "");

        rc = ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC,
                              0,  /* Duration: indefinite */
                              0,  /* Period: 0 = continuous */
                              1,  /* filter_duplicates - reduce CPU load */
                              BLE_HCI_SCAN_FILT_NO_WL,
                              0,  /* limited */
                              &ext_params_1m,
                              s_coded_phy_enabled ? &ext_params_coded : NULL,
                              ble_gap_event_handler,
                              NULL);

        if (rc == 0) {
            *used_extended = true;
            return 0;
        }

        if (rc == BLE_HS_ENOTSUP) {
            ESP_LOGW(TAG, "Extended scanning not supported, falling back to legacy");
            s_extended_scanning_enabled = false;
        } else {
            ESP_LOGW(TAG, "Extended scan failed: %d (%s), trying legacy",
                     rc, ble_hs_err_to_str(rc));
        }
    }
#endif

    /* Legacy scan (BLE 4.x compatible) */
    struct ble_gap_disc_params disc_params = {
        .filter_duplicates = 1,
        .passive = !s_active_scan_enabled,
        .itvl = (uint16_t)(s_scan_interval_ms * MS_TO_US / BLE_TIME_UNIT_US),
        .window = (uint16_t)(s_scan_window_ms * MS_TO_US / BLE_TIME_UNIT_US),
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0
    };

    rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                      &disc_params, ble_gap_event_handler, NULL);

    return rc;
}

esp_err_t ble_scanner_start(void)
{
    if (s_scanner_state == BLE_SCANNER_STATE_UNINITIALIZED) {
        ESP_LOGE(TAG, "Scanner not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_scanner_state == BLE_SCANNER_STATE_RUNNING) {
        ESP_LOGW(TAG, "Scanner already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting BLE scanner (interval: %lu ms, window: %lu ms, extended: %s)",
             (unsigned long)s_scan_interval_ms, (unsigned long)s_scan_window_ms,
             s_extended_scanning_enabled ? "yes" : "no");

    set_scanner_state(BLE_SCANNER_STATE_STARTING);

    int rc;
    bool used_extended = false;

    /* Retry loop for transient controller memory errors during startup.
     * BLE_ERR_MEM_CAPACITY (HCI 0x07) can occur when WiFi/Zigbee coexistence
     * is still settling and the controller has not freed its init buffers. */
    for (int attempt = 0; attempt < SCAN_START_MAX_RETRIES; attempt++) {
        rc = attempt_scan_start(&used_extended);
        if (rc == 0) {
            break;
        }

        /* Only retry on memory capacity errors */
        if (rc != NIMBLE_ERR_MEM_CAPACITY) {
            break;
        }

        if (attempt < SCAN_START_MAX_RETRIES - 1) {
            ESP_LOGW(TAG, "Controller memory exhausted, retry %d/%d after %d ms",
                     attempt + 1, SCAN_START_MAX_RETRIES, SCAN_START_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(SCAN_START_RETRY_MS));
        }
    }

    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start BLE discovery: %d (%s)", rc, ble_hs_err_to_str(rc));
        set_scanner_state(BLE_SCANNER_STATE_ERROR);
        return ESP_FAIL;
    }

    /* Update statistics */
    s_stats.start_time = get_timestamp_ms();
    s_stats.scan_cycles = 0;

    set_scanner_state(BLE_SCANNER_STATE_RUNNING);
    ESP_LOGI(TAG, "BLE scanner started (%s%s)",
             used_extended ? "BLE 5.0 extended" : "BLE 4.x legacy",
             used_extended ? "" : " (fallback)");

    return ESP_OK;
}

esp_err_t ble_scanner_stop(void)
{
    if (s_scanner_state != BLE_SCANNER_STATE_RUNNING) {
        ESP_LOGW(TAG, "Scanner not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping BLE scanner...");
    set_scanner_state(BLE_SCANNER_STATE_STOPPING);

    /* Stop discovery */
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "Failed to cancel BLE discovery: %d (%s)", rc, ble_hs_err_to_str(rc));
    }

    set_scanner_state(BLE_SCANNER_STATE_INITIALIZED);
    ESP_LOGI(TAG, "BLE scanner stopped (total advs: %lu, devices: %lu)",
             (unsigned long)s_stats.total_advertisements,
             (unsigned long)s_stats.unique_devices);

    return ESP_OK;
}

bool ble_scanner_is_running(void)
{
    return (s_scanner_state == BLE_SCANNER_STATE_RUNNING);
}

ble_scanner_state_t ble_scanner_get_state(void)
{
    return s_scanner_state;
}

const char *ble_scanner_get_state_str(void)
{
    if (s_scanner_state >= 0 && s_scanner_state < sizeof(s_state_strings) / sizeof(s_state_strings[0])) {
        return s_state_strings[s_scanner_state];
    }
    return "UNKNOWN";
}

esp_err_t ble_scanner_set_interval(uint32_t interval_ms)
{
    if (interval_ms < BLE_MIN_SCAN_INTERVAL_MS || interval_ms > BLE_MAX_SCAN_INTERVAL_MS) {
        ESP_LOGE(TAG, "Invalid scan interval: %lu (range: %d-%d)",
                 (unsigned long)interval_ms, BLE_MIN_SCAN_INTERVAL_MS, BLE_MAX_SCAN_INTERVAL_MS);
        return ESP_ERR_INVALID_ARG;
    }

    BLE_SCANNER_CHECK_MUTEX(ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);

    s_scan_interval_ms = interval_ms;
    /* Adjust window as percentage of interval, clamped to min/max limits */
    s_scan_window_ms = interval_ms / SCAN_WINDOW_PERCENT;
    if (s_scan_window_ms < SCAN_WINDOW_MIN_MS) {
        s_scan_window_ms = SCAN_WINDOW_MIN_MS;
    } else if (s_scan_window_ms > SCAN_WINDOW_MAX_MS) {
        s_scan_window_ms = SCAN_WINDOW_MAX_MS;
    }

    xSemaphoreGive(s_scanner_mutex);

    ESP_LOGI(TAG, "Scan interval set to %lu ms (window: %lu ms)",
             (unsigned long)s_scan_interval_ms, (unsigned long)s_scan_window_ms);

    /* If running, restart to apply new parameters */
    if (s_scanner_state == BLE_SCANNER_STATE_RUNNING) {
        ble_scanner_stop();
        esp_err_t restart_err = ble_scanner_start();
        if (restart_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restart scanner after interval change: %s",
                     esp_err_to_name(restart_err));
            return restart_err;
        }
    }

    return ESP_OK;
}

uint32_t ble_scanner_get_interval(void)
{
    return s_scan_interval_ms;
}

esp_err_t ble_scanner_register_callback(ble_adv_callback_t callback)
{
    if (callback == NULL) {
        ESP_LOGE(TAG, "Callback is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_scanner_mutex != NULL) {
        xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);
    }

    /* Check if already registered */
    for (size_t i = 0; i < s_callback_count; i++) {
        if (s_callbacks[i] == callback) {
            if (s_scanner_mutex != NULL) {
                xSemaphoreGive(s_scanner_mutex);
            }
            ESP_LOGW(TAG, "Callback already registered");
            return ESP_OK;
        }
    }

    /* Find empty slot */
    if (s_callback_count >= MAX_ADV_CALLBACKS) {
        if (s_scanner_mutex != NULL) {
            xSemaphoreGive(s_scanner_mutex);
        }
        ESP_LOGE(TAG, "Maximum callbacks reached (%d)", MAX_ADV_CALLBACKS);
        return ESP_ERR_NO_MEM;
    }

    s_callbacks[s_callback_count++] = callback;
    if (s_scanner_mutex != NULL) {
        xSemaphoreGive(s_scanner_mutex);
    }

    ESP_LOGI(TAG, "Callback registered (total: %d)", (int)s_callback_count);
    return ESP_OK;
}

esp_err_t ble_scanner_unregister_callback(ble_adv_callback_t callback)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    BLE_SCANNER_CHECK_MUTEX(ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);

    for (size_t i = 0; i < s_callback_count; i++) {
        if (s_callbacks[i] == callback) {
            /* Shift remaining callbacks */
            for (size_t j = i; j < s_callback_count - 1; j++) {
                s_callbacks[j] = s_callbacks[j + 1];
            }
            s_callbacks[--s_callback_count] = NULL;
            xSemaphoreGive(s_scanner_mutex);
            ESP_LOGI(TAG, "Callback unregistered (remaining: %d)", (int)s_callback_count);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_scanner_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_scanner_register_state_callback(ble_scanner_state_callback_t callback)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_state_callback = callback;
    ESP_LOGI(TAG, "State change callback registered");
    return ESP_OK;
}

esp_err_t ble_scanner_get_stats(ble_scanner_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    BLE_SCANNER_CHECK_MUTEX(ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);
    memcpy(stats, &s_stats, sizeof(ble_scanner_stats_t));
    xSemaphoreGive(s_scanner_mutex);

    return ESP_OK;
}

void ble_scanner_reset_stats(void)
{
    if (s_scanner_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);
    memset(&s_stats, 0, sizeof(ble_scanner_stats_t));
    if (s_scanner_state == BLE_SCANNER_STATE_RUNNING) {
        s_stats.start_time = get_timestamp_ms();
    }
    xSemaphoreGive(s_scanner_mutex);
    ESP_LOGI(TAG, "Statistics reset");
}

esp_err_t ble_scanner_get_tracked_device(size_t index, ble_tracked_device_t *device)
{
    if (device == NULL || index >= BLE_MAX_TRACKED_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }

    BLE_SCANNER_CHECK_MUTEX(ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);

    if (!s_tracked_devices[index].active) {
        xSemaphoreGive(s_scanner_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(device, &s_tracked_devices[index], sizeof(ble_tracked_device_t));
    xSemaphoreGive(s_scanner_mutex);

    return ESP_OK;
}

esp_err_t ble_scanner_get_device_by_mac(const uint8_t *mac, ble_tracked_device_t *device)
{
    if (mac == NULL || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    BLE_SCANNER_CHECK_MUTEX(ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);

    for (size_t i = 0; i < BLE_MAX_TRACKED_DEVICES; i++) {
        if (s_tracked_devices[i].active && ble_mac_equal(s_tracked_devices[i].mac, mac)) {
            memcpy(device, &s_tracked_devices[i], sizeof(ble_tracked_device_t));
            xSemaphoreGive(s_scanner_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_scanner_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t ble_scanner_get_device_count(void)
{
    return s_device_count;
}

void ble_scanner_clear_devices(void)
{
    if (s_scanner_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);
    if (s_tracked_devices) {
        memset(s_tracked_devices, 0, BLE_MAX_TRACKED_DEVICES * sizeof(ble_tracked_device_t));
    }
    s_device_count = 0;
    s_stats.unique_devices = 0;
    xSemaphoreGive(s_scanner_mutex);
    ESP_LOGI(TAG, "Tracked devices cleared");
}

size_t ble_scanner_prune_stale_devices(void)
{
    if (s_scanner_mutex == NULL) {
        return 0;
    }

    uint32_t now = get_timestamp_ms();
    size_t removed = 0;

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);

    for (size_t i = 0; i < BLE_MAX_TRACKED_DEVICES; i++) {
        if (s_tracked_devices[i].active) {
            uint32_t age = now - s_tracked_devices[i].last_seen;
            if (age > BLE_DEVICE_STALE_TIMEOUT_MS) {
                char mac_str[18];
                ble_mac_to_str(s_tracked_devices[i].mac, mac_str);
                ESP_LOGD(TAG, "Removing stale device: %s (age: %lu ms)", mac_str, (unsigned long)age);
                memset(&s_tracked_devices[i], 0, sizeof(ble_tracked_device_t));
                s_device_count--;
                removed++;
            }
        }
    }

    xSemaphoreGive(s_scanner_mutex);

    if (removed > 0) {
        ESP_LOGI(TAG, "Pruned %d stale devices (remaining: %d)", (int)removed, (int)s_device_count);
    }

    return removed;
}

esp_err_t ble_scanner_self_test(void)
{
    ESP_LOGI(TAG, "Running BLE scanner self-test...");

    /* Check initialization */
    if (s_scanner_state == BLE_SCANNER_STATE_UNINITIALIZED) {
        ESP_LOGE(TAG, "Scanner not initialized");
        return ESP_FAIL;
    }

    /* Check mutex */
    if (s_scanner_mutex == NULL) {
        ESP_LOGE(TAG, "Scanner mutex not created");
        return ESP_FAIL;
    }

    /* Check scan parameters */
    if (s_scan_interval_ms < BLE_MIN_SCAN_INTERVAL_MS ||
        s_scan_interval_ms > BLE_MAX_SCAN_INTERVAL_MS) {
        ESP_LOGE(TAG, "Invalid scan interval: %lu", (unsigned long)s_scan_interval_ms);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE scanner self-test PASSED (devices: %d, advs: %lu)",
             (int)s_device_count, (unsigned long)s_stats.total_advertisements);
    return ESP_OK;
}

/* Internal functions */

static void set_scanner_state(ble_scanner_state_t state)
{
    ble_scanner_state_t old_state = s_scanner_state;
    s_scanner_state = state;
    ESP_LOGD(TAG, "State: %s", ble_scanner_get_state_str());

    /* Notify state change callback (e.g., ESPHome BLE proxy → HA) */
    if (s_state_callback && old_state != state) {
        s_state_callback(state, old_state);
    }
}

static uint32_t get_timestamp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / MS_TO_US);
}

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            /* Legacy advertisement (BLE 4.x) */
            process_advertisement(&event->disc);
            s_stats.legacy_adv_count++;
            s_stats.phy_1m_count++;
            break;

#if MYNEWT_VAL(BLE_EXT_ADV)
        case BLE_GAP_EVENT_EXT_DISC:
            /* Extended advertisement (BLE 5.0) */
            process_ext_advertisement(&event->ext_disc);
            s_stats.extended_adv_count++;
            /* Track PHY usage */
            switch (event->ext_disc.prim_phy) {
                case BLE_GAP_LE_PHY_1M:
                    s_stats.phy_1m_count++;
                    break;
                case BLE_GAP_LE_PHY_CODED:
                    s_stats.phy_coded_count++;
                    break;
            }
            if (event->ext_disc.sec_phy == BLE_GAP_LE_PHY_2M) {
                s_stats.phy_2m_count++;
            }
            break;
#endif

        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGD(TAG, "Discovery complete, reason: %d", event->disc_complete.reason);
            s_stats.scan_cycles++;
            /* Restart scanning if we were supposed to be running */
            if (s_scanner_state == BLE_SCANNER_STATE_RUNNING) {
#if MYNEWT_VAL(BLE_EXT_ADV)
                if (s_extended_scanning_enabled) {
                    struct ble_gap_ext_disc_params ext_params = {
                        .passive = !s_active_scan_enabled,
                        .itvl = (uint16_t)(s_scan_interval_ms * MS_TO_US / BLE_TIME_UNIT_US),
                        .window = (uint16_t)(s_scan_window_ms * MS_TO_US / BLE_TIME_UNIT_US),
                    };
                    ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC, 0, 0, 0,
                                     BLE_HCI_SCAN_FILT_NO_WL, 0,
                                     &ext_params,
                                     s_coded_phy_enabled ? &ext_params : NULL,
                                     ble_gap_event_handler, NULL);
                } else
#endif
                {
                    struct ble_gap_disc_params disc_params = {
                        .filter_duplicates = 1,  /* Filter duplicates to reduce CPU load */
                        .passive = !s_active_scan_enabled,
                        .itvl = (uint16_t)(s_scan_interval_ms * MS_TO_US / BLE_TIME_UNIT_US),
                        .window = (uint16_t)(s_scan_window_ms * MS_TO_US / BLE_TIME_UNIT_US),
                        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
                        .limited = 0
                    };
                    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                                 &disc_params, ble_gap_event_handler, NULL);
                }
            }
            break;

        default:
            break;
    }

    return 0;
}

static void process_advertisement(const struct ble_gap_disc_desc *disc)
{
    if (disc == NULL) {
        return;
    }

    uint32_t now = get_timestamp_ms();

    /* Build advertisement data structure */
    ble_adv_data_t adv = {0};
    memcpy(adv.mac, disc->addr.val, BLE_MAC_ADDR_LEN);
    adv.addr_type = (ble_addr_type_t)disc->addr.type;
    adv.adv_type = (ble_adv_type_t)disc->event_type;
    adv.rssi = disc->rssi;
    adv.timestamp = now;

    /* Copy advertisement data */
    if (disc->length_data > 0 && disc->length_data <= BLE_ADV_DATA_MAX_LEN) {
        memcpy(adv.adv_data, disc->data, disc->length_data);
        adv.adv_data_len = disc->length_data;
    }

    /* Update statistics */
    s_stats.total_advertisements++;

    /* Update tracked device */
    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);
    ble_tracked_device_t *dev = find_or_create_device(adv.mac, adv.addr_type);
    if (dev != NULL) {
        /* Update RSSI with exponential moving average using fixed-point math */
        if (dev->adv_count == 0) {
            dev->rssi_avg = adv.rssi;
        } else {
            /* Fixed-point EMA: new_avg = (alpha * new + (256 - alpha) * old) >> 8 */
            int16_t ema = (RSSI_EMA_ALPHA_FP * (int16_t)adv.rssi +
                          (RSSI_EMA_SCALE - RSSI_EMA_ALPHA_FP) * (int16_t)dev->rssi_avg);
            dev->rssi_avg = (int8_t)(ema >> 8);
        }
        dev->rssi = adv.rssi;
        dev->last_seen = now;
        dev->adv_count++;
    }
    xSemaphoreGive(s_scanner_mutex);

    /* Invoke callbacks */
    invoke_callbacks(&adv);
}

static ble_tracked_device_t *find_or_create_device(const uint8_t *mac, ble_addr_type_t addr_type)
{
    /* Search for existing device */
    for (size_t i = 0; i < BLE_MAX_TRACKED_DEVICES; i++) {
        if (s_tracked_devices[i].active && ble_mac_equal(s_tracked_devices[i].mac, mac)) {
            return &s_tracked_devices[i];
        }
    }

    /* Find empty slot for new device */
    for (size_t i = 0; i < BLE_MAX_TRACKED_DEVICES; i++) {
        if (!s_tracked_devices[i].active) {
            /* Initialize new device entry */
            memset(&s_tracked_devices[i], 0, sizeof(ble_tracked_device_t));
            memcpy(s_tracked_devices[i].mac, mac, BLE_MAC_ADDR_LEN);
            s_tracked_devices[i].addr_type = addr_type;
            s_tracked_devices[i].device_type = BLE_DEVICE_TYPE_UNKNOWN;
            s_tracked_devices[i].first_seen = get_timestamp_ms();
            s_tracked_devices[i].last_seen = s_tracked_devices[i].first_seen;
            s_tracked_devices[i].active = true;
            s_device_count++;
            s_stats.unique_devices++;

            char mac_str[18];
            ble_mac_to_str(mac, mac_str);
            ESP_LOGD(TAG, "New device: %s (total: %d)", mac_str, (int)s_device_count);

            return &s_tracked_devices[i];
        }
    }

    /* No empty slots - perform LRU eviction */
    size_t lru_idx = 0;
    uint32_t oldest_seen = UINT32_MAX;

    for (size_t i = 0; i < BLE_MAX_TRACKED_DEVICES; i++) {
        if (s_tracked_devices[i].last_seen < oldest_seen) {
            oldest_seen = s_tracked_devices[i].last_seen;
            lru_idx = i;
        }
    }

    /* Log the eviction */
    char evict_mac_str[18];
    ble_mac_to_str(s_tracked_devices[lru_idx].mac, evict_mac_str);
    ESP_LOGW(TAG, "Device tracking limit reached (%d), evicting LRU device: %s",
             BLE_MAX_TRACKED_DEVICES, evict_mac_str);

    /* Clear the evicted slot and initialize with new device */
    memset(&s_tracked_devices[lru_idx], 0, sizeof(ble_tracked_device_t));
    memcpy(s_tracked_devices[lru_idx].mac, mac, BLE_MAC_ADDR_LEN);
    s_tracked_devices[lru_idx].addr_type = addr_type;
    s_tracked_devices[lru_idx].device_type = BLE_DEVICE_TYPE_UNKNOWN;
    s_tracked_devices[lru_idx].first_seen = get_timestamp_ms();
    s_tracked_devices[lru_idx].last_seen = s_tracked_devices[lru_idx].first_seen;
    s_tracked_devices[lru_idx].active = true;
    /* Note: s_device_count stays the same since we evicted one and added one */
    s_stats.unique_devices++;

    char new_mac_str[18];
    ble_mac_to_str(mac, new_mac_str);
    ESP_LOGD(TAG, "New device (via LRU eviction): %s", new_mac_str);

    return &s_tracked_devices[lru_idx];
}

static void invoke_callbacks(const ble_adv_data_t *adv)
{
    /* Don't hold mutex during callbacks to avoid deadlock */
    ble_adv_callback_t callbacks[MAX_ADV_CALLBACKS];
    size_t count;

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);
    count = s_callback_count;
    memcpy(callbacks, s_callbacks, sizeof(callbacks));
    xSemaphoreGive(s_scanner_mutex);

    for (size_t i = 0; i < count; i++) {
        if (callbacks[i] != NULL) {
            callbacks[i](adv);
        }
    }
}

#if MYNEWT_VAL(BLE_EXT_ADV)
static void invoke_ext_callbacks(const ble_ext_adv_data_t *ext_adv)
{
    ble_ext_adv_callback_t callbacks[MAX_EXT_ADV_CALLBACKS];
    size_t count;

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);
    count = s_ext_callback_count;
    memcpy(callbacks, s_ext_callbacks, sizeof(callbacks));
    xSemaphoreGive(s_scanner_mutex);

    for (size_t i = 0; i < count; i++) {
        if (callbacks[i] != NULL) {
            callbacks[i](ext_adv);
        }
    }
}

static ble_phy_t nimble_phy_to_ble_phy(uint8_t nimble_phy)
{
    switch (nimble_phy) {
        case 1:  return BLE_PHY_1M;
        case 2:  return BLE_PHY_2M;
        case 3:  return BLE_PHY_CODED;
        default: return BLE_PHY_UNKNOWN;
    }
}
static void process_ext_advertisement(const struct ble_gap_ext_disc_desc *ext_disc)
{
    if (ext_disc == NULL) {
        return;
    }

    uint32_t now = get_timestamp_ms();

    /* Check if this is a legacy advertisement wrapped in extended format */
    bool is_legacy = (ext_disc->props & BLE_HCI_ADV_LEGACY_MASK) != 0;

    /* Build standard advertisement structure for compatibility */
    ble_adv_data_t adv = {0};
    memcpy(adv.mac, ext_disc->addr.val, BLE_MAC_ADDR_LEN);
    adv.addr_type = (ble_addr_type_t)ext_disc->addr.type;
    adv.rssi = ext_disc->rssi;
    adv.timestamp = now;
    adv.is_extended = !is_legacy;
    adv.primary_phy = nimble_phy_to_ble_phy(ext_disc->prim_phy);
    adv.secondary_phy = nimble_phy_to_ble_phy(ext_disc->sec_phy);
    adv.ext_adv_props = ext_disc->props;
    adv.tx_power = ext_disc->tx_power;
    adv.sid = ext_disc->sid;

    /* Copy advertisement data (up to legacy limit for standard struct) */
    if (ext_disc->length_data > 0) {
        uint8_t copy_len = ext_disc->length_data;
        if (copy_len > BLE_ADV_DATA_MAX_LEN) {
            copy_len = BLE_ADV_DATA_MAX_LEN;
        }
        memcpy(adv.adv_data, ext_disc->data, copy_len);
        adv.adv_data_len = copy_len;
    }

    /* Determine legacy adv type from extended properties */
    if (ext_disc->props & BLE_HCI_ADV_CONN_MASK) {
        if (ext_disc->props & BLE_HCI_ADV_DIRECT_MASK) {
            adv.adv_type = BLE_ADV_TYPE_DIRECT_IND_HIGH;
        } else {
            adv.adv_type = BLE_ADV_TYPE_IND;
        }
    } else if (ext_disc->props & BLE_HCI_ADV_SCAN_MASK) {
        adv.adv_type = BLE_ADV_TYPE_SCAN_IND;
    } else {
        adv.adv_type = BLE_ADV_TYPE_NONCONN_IND;
    }

    /* Update statistics */
    s_stats.total_advertisements++;

    /* Update tracked device */
    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);
    ble_tracked_device_t *dev = find_or_create_device(adv.mac, adv.addr_type);
    if (dev != NULL) {
        /* Update RSSI with exponential moving average using fixed-point math */
        if (dev->adv_count == 0) {
            dev->rssi_avg = adv.rssi;
        } else {
            /* Fixed-point EMA: new_avg = (alpha * new + (256 - alpha) * old) >> 8 */
            int16_t ema = (RSSI_EMA_ALPHA_FP * (int16_t)adv.rssi +
                          (RSSI_EMA_SCALE - RSSI_EMA_ALPHA_FP) * (int16_t)dev->rssi_avg);
            dev->rssi_avg = (int8_t)(ema >> 8);
        }
        dev->rssi = adv.rssi;
        dev->last_seen = now;
        dev->adv_count++;
        /* Update BLE 5.0 fields */
        dev->supports_ble5 = !is_legacy;
        dev->preferred_phy = adv.secondary_phy != BLE_PHY_UNKNOWN ?
                             adv.secondary_phy : adv.primary_phy;
        dev->tx_power = ext_disc->tx_power;
    }
    xSemaphoreGive(s_scanner_mutex);

    /* Invoke standard callbacks (for compatibility) */
    invoke_callbacks(&adv);

    /* For extended advertisements with large payloads, also invoke ext callbacks */
    if (!is_legacy && ext_disc->length_data > BLE_ADV_DATA_MAX_LEN && s_ext_callback_count > 0) {
        /* Use static buffer for extended data to avoid dangling pointer
         * This is safe because callbacks are invoked synchronously */
        static uint8_t s_ext_adv_buffer[BLE_EXT_ADV_DATA_MAX_LEN];

        ble_ext_adv_data_t ext_adv = {0};
        memcpy(ext_adv.mac, ext_disc->addr.val, BLE_MAC_ADDR_LEN);
        ext_adv.addr_type = (ble_addr_type_t)ext_disc->addr.type;
        ext_adv.rssi = ext_disc->rssi;
        ext_adv.primary_phy = adv.primary_phy;
        ext_adv.secondary_phy = adv.secondary_phy;
        ext_adv.ext_adv_props = ext_disc->props;
        ext_adv.tx_power = ext_disc->tx_power;
        ext_adv.sid = ext_disc->sid;

        /* Copy extended data to static buffer to ensure validity during callbacks */
        uint16_t copy_len = ext_disc->length_data;
        if (copy_len > BLE_EXT_ADV_DATA_MAX_LEN) {
            copy_len = BLE_EXT_ADV_DATA_MAX_LEN;
        }
        memcpy(s_ext_adv_buffer, ext_disc->data, copy_len);
        ext_adv.ext_adv_data = s_ext_adv_buffer;
        ext_adv.ext_adv_data_len = copy_len;
        ext_adv.timestamp = now;

        invoke_ext_callbacks(&ext_adv);
    }
}
#endif /* MYNEWT_VAL(BLE_EXT_ADV) */

/* BLE 5.0 API implementations */

bool ble_scanner_is_extended_enabled(void)
{
    return s_extended_scanning_enabled;
}

esp_err_t ble_scanner_set_extended_mode(bool enable)
{
    BLE_SCANNER_CHECK_MUTEX(ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);
    s_extended_scanning_enabled = enable;
    xSemaphoreGive(s_scanner_mutex);

    ESP_LOGI(TAG, "Extended scanning %s", enable ? "enabled" : "disabled");

    /* Restart if running */
    if (s_scanner_state == BLE_SCANNER_STATE_RUNNING) {
        ble_scanner_stop();
        return ble_scanner_start();
    }
    return ESP_OK;
}

esp_err_t ble_scanner_set_phy_config(bool enable_1m, bool enable_2m, bool enable_coded)
{
    (void)enable_1m;  /* 1M is always enabled */

    BLE_SCANNER_CHECK_MUTEX(ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);
    s_2m_phy_enabled = enable_2m;
    s_coded_phy_enabled = enable_coded;
    xSemaphoreGive(s_scanner_mutex);

    ESP_LOGI(TAG, "PHY config: 1M=yes, 2M=%s, Coded=%s",
             enable_2m ? "yes" : "no", enable_coded ? "yes" : "no");

    return ESP_OK;
}

esp_err_t ble_scanner_register_ext_callback(ble_ext_adv_callback_t callback)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    BLE_SCANNER_CHECK_MUTEX(ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_ext_callback_count >= MAX_EXT_ADV_CALLBACKS) {
        xSemaphoreGive(s_scanner_mutex);
        ESP_LOGE(TAG, "Maximum ext callbacks reached");
        return ESP_ERR_NO_MEM;
    }

    s_ext_callbacks[s_ext_callback_count++] = callback;
    xSemaphoreGive(s_scanner_mutex);

    ESP_LOGI(TAG, "Extended callback registered");
    return ESP_OK;
}

esp_err_t ble_scanner_unregister_ext_callback(ble_ext_adv_callback_t callback)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    BLE_SCANNER_CHECK_MUTEX(ESP_ERR_INVALID_STATE);

    xSemaphoreTake(s_scanner_mutex, GW_TIMEOUT_LONG_TICKS);

    for (size_t i = 0; i < s_ext_callback_count; i++) {
        if (s_ext_callbacks[i] == callback) {
            for (size_t j = i; j < s_ext_callback_count - 1; j++) {
                s_ext_callbacks[j] = s_ext_callbacks[j + 1];
            }
            s_ext_callbacks[--s_ext_callback_count] = NULL;
            xSemaphoreGive(s_scanner_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_scanner_mutex);
    return ESP_ERR_NOT_FOUND;
}

const char *ble_scanner_get_ble_version_str(void)
{
#if MYNEWT_VAL(BLE_EXT_ADV)
    if (s_extended_scanning_enabled) {
        if (s_coded_phy_enabled) {
            return "BLE 4.x + 5.0 Extended (1M+2M+Coded)";
        } else if (s_2m_phy_enabled) {
            return "BLE 4.x + 5.0 Extended (1M+2M)";
        } else {
            return "BLE 4.x + 5.0 Extended (1M)";
        }
    }
#endif
    return "BLE 4.x Legacy";
}

void ble_scanner_set_active_mode(bool enable)
{
    s_active_scan_enabled = enable;
    ESP_LOGI(TAG, "Active scanning %s", enable ? "enabled" : "disabled");
}

bool ble_scanner_is_active_enabled(void)
{
    return s_active_scan_enabled;
}

#else /* CONFIG_BT_SCANNER_ENABLED */

/* Stub implementations when BLE is disabled */
static const char *TAG = "BLE_SCANNER";

esp_err_t ble_scanner_init(void)
{
    ESP_LOGW(TAG, "BLE scanner disabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_scanner_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_scanner_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_scanner_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_scanner_is_running(void)
{
    return false;
}

ble_scanner_state_t ble_scanner_get_state(void)
{
    return BLE_SCANNER_STATE_UNINITIALIZED;
}

const char *ble_scanner_get_state_str(void)
{
    return "DISABLED";
}

esp_err_t ble_scanner_set_interval(uint32_t interval_ms)
{
    (void)interval_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t ble_scanner_get_interval(void)
{
    return 0;
}

esp_err_t ble_scanner_register_callback(ble_adv_callback_t callback)
{
    (void)callback;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_scanner_unregister_callback(ble_adv_callback_t callback)
{
    (void)callback;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_scanner_register_state_callback(ble_scanner_state_callback_t callback)
{
    (void)callback;
    return ESP_OK;
}

esp_err_t ble_scanner_get_stats(ble_scanner_stats_t *stats)
{
    (void)stats;
    return ESP_ERR_NOT_SUPPORTED;
}

void ble_scanner_reset_stats(void)
{
}

esp_err_t ble_scanner_get_tracked_device(size_t index, ble_tracked_device_t *device)
{
    (void)index;
    (void)device;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_scanner_get_device_by_mac(const uint8_t *mac, ble_tracked_device_t *device)
{
    (void)mac;
    (void)device;
    return ESP_ERR_NOT_SUPPORTED;
}

size_t ble_scanner_get_device_count(void)
{
    return 0;
}

void ble_scanner_clear_devices(void)
{
}

size_t ble_scanner_prune_stale_devices(void)
{
    return 0;
}

esp_err_t ble_scanner_self_test(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_scanner_init_with_config(const ble_scanner_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_scanner_is_extended_enabled(void)
{
    return false;
}

esp_err_t ble_scanner_set_extended_mode(bool enable)
{
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_scanner_set_phy_config(bool enable_1m, bool enable_2m, bool enable_coded)
{
    (void)enable_1m;
    (void)enable_2m;
    (void)enable_coded;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_scanner_register_ext_callback(ble_ext_adv_callback_t callback)
{
    (void)callback;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_scanner_unregister_ext_callback(ble_ext_adv_callback_t callback)
{
    (void)callback;
    return ESP_ERR_NOT_SUPPORTED;
}

const char *ble_scanner_get_ble_version_str(void)
{
    return "DISABLED";
}

void ble_scanner_set_active_mode(bool enable)
{
    (void)enable;
}

bool ble_scanner_is_active_enabled(void)
{
    return false;
}

#endif /* CONFIG_BT_SCANNER_ENABLED */
