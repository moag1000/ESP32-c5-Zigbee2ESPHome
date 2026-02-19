/**
 * @file ble_manager.c
 * @brief BLE Stack Management Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_manager.h"
#include "ble_scanner.h"
#include "ble_constants.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"

#if CONFIG_BT_SCANNER_ENABLED

#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "BLE_MANAGER";

/* BLE address type (set during sync) */
static uint8_t ble_hs_id_addr_type;

/* Manager state */
static ble_manager_state_t s_manager_state = BLE_MANAGER_STATE_UNINITIALIZED;
static SemaphoreHandle_t s_manager_mutex = NULL;
static ble_manager_config_t s_config = BLE_MANAGER_CONFIG_DEFAULT();
static bool s_coexist_enabled = true;
static bool s_host_synced = false;

/* NimBLE host task handle */
static TaskHandle_t s_nimble_host_task_handle = NULL;

/* Forward declarations */
static void set_manager_state(ble_manager_state_t state);
static void ble_host_task(void *param);
static void ble_on_sync(void);
static void ble_on_reset(int reason);

/**
 * @brief State string lookup table
 */
static const char *s_state_strings[] = {
    "UNINITIALIZED",
    "INITIALIZED",
    "RUNNING",
    "ERROR",
    "DEINITIALIZING"
};

esp_err_t ble_manager_init(void)
{
    ble_manager_config_t config = BLE_MANAGER_CONFIG_DEFAULT();
    return ble_manager_init_with_config(&config);
}

esp_err_t ble_manager_init_with_config(const ble_manager_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Configuration is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_manager_state != BLE_MANAGER_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "BLE manager already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing BLE manager...");

    /* Store configuration */
    s_config = *config;
    s_coexist_enabled = config->coexist_with_zigbee;

    /* Create manager mutex */
    s_manager_mutex = xSemaphoreCreateMutex();
    if (s_manager_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create manager mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret;

    /* Initialize NimBLE */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NimBLE port: %s", esp_err_to_name(ret));
        goto cleanup_mutex;
    }

    /* Configure the NimBLE host */
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.store_status_cb = NULL;

    /* Start NimBLE host task */
    nimble_port_freertos_init(ble_host_task);

    /* Wait for host sync with timeout */
    int timeout_ms = BLE_INIT_TIMEOUT_MS;
    int elapsed_ms = 0;
    while (!s_host_synced && elapsed_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(BLE_INIT_POLL_DELAY_MS));
        elapsed_ms += BLE_INIT_POLL_DELAY_MS;
    }

    if (!s_host_synced) {
        ESP_LOGE(TAG, "NimBLE host sync timeout");
        ret = ESP_ERR_TIMEOUT;
        goto cleanup_nimble;
    }

    /* Initialize scanner module */
    ret = ble_scanner_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize scanner: %s", esp_err_to_name(ret));
        goto cleanup_nimble;
    }

    /* Set initial scan interval */
    ble_scanner_set_interval(config->scan_interval_ms);

    set_manager_state(BLE_MANAGER_STATE_INITIALIZED);
    ESP_LOGI(TAG, "BLE manager initialized successfully");

    /* Auto-start scanner if configured */
    if (config->enable_scanner) {
        ret = ble_scanner_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to auto-start scanner: %s", esp_err_to_name(ret));
            /* Don't fail init, scanner can be started later */
        } else {
            set_manager_state(BLE_MANAGER_STATE_RUNNING);
        }
    }

    return ESP_OK;

cleanup_nimble:
    nimble_port_deinit();

cleanup_mutex:
    if (s_manager_mutex != NULL) {
        vSemaphoreDelete(s_manager_mutex);
        s_manager_mutex = NULL;
    }
    set_manager_state(BLE_MANAGER_STATE_ERROR);
    return ret;
}

esp_err_t ble_manager_deinit(void)
{
    if (s_manager_state == BLE_MANAGER_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "BLE manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing BLE manager...");
    set_manager_state(BLE_MANAGER_STATE_DEINITIALIZING);

    /* Stop scanner first */
    if (ble_scanner_is_running()) {
        esp_err_t ret = ble_scanner_stop();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop scanner: %s", esp_err_to_name(ret));
        }
    }

    /* Deinitialize scanner module */
    ble_scanner_deinit();

    /* Stop NimBLE host */
    int nimble_ret = nimble_port_stop();
    if (nimble_ret != 0) {
        ESP_LOGW(TAG, "Failed to stop NimBLE port: %d", nimble_ret);
    }

    /* Wait for host task to complete */
    vTaskDelay(pdMS_TO_TICKS(BLE_DEINIT_WAIT_DELAY_MS));

    /* Deinitialize NimBLE */
    esp_err_t deinit_ret = nimble_port_deinit();
    if (deinit_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to deinit NimBLE port: %s", esp_err_to_name(deinit_ret));
    }

    /* Delete mutex */
    if (s_manager_mutex != NULL) {
        vSemaphoreDelete(s_manager_mutex);
        s_manager_mutex = NULL;
    }

    s_host_synced = false;
    s_nimble_host_task_handle = NULL;
    set_manager_state(BLE_MANAGER_STATE_UNINITIALIZED);

    ESP_LOGI(TAG, "BLE manager deinitialized");
    return ESP_OK;
}

bool ble_manager_is_initialized(void)
{
    /* Thread-safe state check */
    if (s_manager_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_manager_mutex, GW_TIMEOUT_LONG_TICKS);
    bool initialized = (s_manager_state == BLE_MANAGER_STATE_INITIALIZED ||
                        s_manager_state == BLE_MANAGER_STATE_RUNNING);
    xSemaphoreGive(s_manager_mutex);
    return initialized;
}

ble_manager_state_t ble_manager_get_state(void)
{
    /* Thread-safe state retrieval */
    if (s_manager_mutex == NULL) {
        return BLE_MANAGER_STATE_UNINITIALIZED;
    }
    xSemaphoreTake(s_manager_mutex, GW_TIMEOUT_LONG_TICKS);
    ble_manager_state_t state = s_manager_state;
    xSemaphoreGive(s_manager_mutex);
    return state;
}

const char *ble_manager_get_state_str(void)
{
    /* Thread-safe string retrieval */
    ble_manager_state_t state = ble_manager_get_state();
    if (state >= 0 && state < sizeof(s_state_strings) / sizeof(s_state_strings[0])) {
        return s_state_strings[state];
    }
    return "UNKNOWN";
}

esp_err_t ble_manager_set_coexist_mode(bool enable)
{
    if (!ble_manager_is_initialized()) {
        ESP_LOGE(TAG, "BLE manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_manager_mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_coexist_enabled != enable) {
        s_coexist_enabled = enable;
        ESP_LOGI(TAG, "Zigbee coexistence mode %s", enable ? "enabled" : "disabled");

        /* Adjust scan parameters based on coexistence mode */
        if (ble_scanner_is_running()) {
            /* Restart scanner with new parameters */
            ble_scanner_stop();
            if (enable) {
                /* Reduce scan duty cycle for coexistence */
                ble_scanner_set_interval(s_config.scan_interval_ms * 2);
            } else {
                ble_scanner_set_interval(s_config.scan_interval_ms);
            }
            ble_scanner_start();
        }
    }

    xSemaphoreGive(s_manager_mutex);
    return ESP_OK;
}

bool ble_manager_is_coexist_enabled(void)
{
    return s_coexist_enabled;
}

uint32_t ble_manager_get_free_heap(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}

esp_err_t ble_manager_self_test(void)
{
    ESP_LOGI(TAG, "Running BLE manager self-test...");

    /* Check initialization */
    if (s_manager_state == BLE_MANAGER_STATE_UNINITIALIZED) {
        ESP_LOGE(TAG, "BLE manager not initialized");
        return ESP_FAIL;
    }

    /* Check mutex */
    if (s_manager_mutex == NULL) {
        ESP_LOGE(TAG, "Manager mutex not created");
        return ESP_FAIL;
    }

    /* Check NimBLE host sync */
    if (!s_host_synced) {
        ESP_LOGE(TAG, "NimBLE host not synced");
        return ESP_FAIL;
    }

    /* Check available heap */
    uint32_t free_heap = ble_manager_get_free_heap();
    if (free_heap < BLE_MANAGER_LOW_HEAP_THRESHOLD) {
        ESP_LOGW(TAG, "Low heap memory: %lu bytes", (unsigned long)free_heap);
    }

    /* Check NimBLE host task stack usage */
    if (s_nimble_host_task_handle != NULL) {
        UBaseType_t high_water = uxTaskGetStackHighWaterMark(s_nimble_host_task_handle);
        if (high_water < BLE_STACK_WARNING_THRESHOLD) {
            ESP_LOGW(TAG, "BLE host task stack low: %u bytes remaining", high_water);
        } else {
            ESP_LOGI(TAG, "BLE host task stack: %u bytes remaining", high_water);
        }
    }

    ESP_LOGI(TAG, "BLE manager self-test PASSED (heap: %lu bytes)", (unsigned long)free_heap);
    return ESP_OK;
}

/* Internal functions */

static void set_manager_state(ble_manager_state_t state)
{
    s_manager_state = state;
    ESP_LOGI(TAG, "State: %s", ble_manager_get_state_str());
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");

    s_nimble_host_task_handle = xTaskGetCurrentTaskHandle();

    /* Log initial stack high water mark */
    UBaseType_t initial_hwm = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "BLE host task stack high water mark: %u bytes", initial_hwm);

    /* Run the NimBLE host */
    nimble_port_run();

    /* Task cleanup - should not reach here normally */
    ESP_LOGW(TAG, "NimBLE host task exiting");
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced");

    /* Use privacy - set random address */
    int rc = ble_hs_id_infer_auto(0, &ble_hs_id_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type: %d", rc);
        return;
    }

    /* Log our address */
    uint8_t addr[6];
    rc = ble_hs_id_copy_addr(ble_hs_id_addr_type, addr, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE address: %02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    }

    s_host_synced = true;
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset, reason: %d", reason);
    s_host_synced = false;

    if (s_manager_state == BLE_MANAGER_STATE_RUNNING) {
        set_manager_state(BLE_MANAGER_STATE_ERROR);
    }
}

#else /* CONFIG_BT_SCANNER_ENABLED */

/* Stub implementations when BLE is disabled */
static const char *TAG = "BLE_MANAGER";

esp_err_t ble_manager_init(void)
{
    ESP_LOGW(TAG, "BLE scanner disabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_manager_init_with_config(const ble_manager_config_t *config)
{
    (void)config;
    ESP_LOGW(TAG, "BLE scanner disabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_manager_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_manager_is_initialized(void)
{
    return false;
}

ble_manager_state_t ble_manager_get_state(void)
{
    return BLE_MANAGER_STATE_UNINITIALIZED;
}

const char *ble_manager_get_state_str(void)
{
    return "DISABLED";
}

esp_err_t ble_manager_set_coexist_mode(bool enable)
{
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_manager_is_coexist_enabled(void)
{
    return false;
}

uint32_t ble_manager_get_free_heap(void)
{
    return 0;
}

esp_err_t ble_manager_self_test(void)
{
    ESP_LOGW(TAG, "BLE scanner disabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_BT_SCANNER_ENABLED */
