/**
 * @file foundation_init.c
 * @brief Foundation Initialization for ESP32-C5 Zigbee2MQTT NG
 *
 * Implements centralized initialization and deinitialization of all
 * Phase 1-3 foundation components in the correct order.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "foundation_init.h"
#include "core/memory/lifecycle.h"
#include "core/memory/memory_pressure.h"
#include "core/memory/memory_manager_ng.h"
#include "core/events/event_bus.h"
#include "core/device/device_registry.h"
#include "core/device/device_persistence.h"
#include "core/discovery/ha_discovery_ng.h"
#if CONFIG_EVENT_TRACE_ENABLE
#include "core/monitoring/event_trace.h"
#endif
#if CONFIG_MEMORY_DASHBOARD_ENABLE
#include "core/monitoring/memory_dashboard.h"
#endif
#include "core/adapters/zigbee_adapter.h"
#include "core/adapters/mqtt_adapter.h"
#include "core/adapters/ble_adapter.h"
#include "core/adapters/esphome_adapter.h"
#include "core/adapters/adapter_interface.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "foundation";

/* ============================================================================
 * Buffer Pool Configuration
 * ============================================================================ */

/** @brief JSON pool: 4 buffers x 2KB for JSON serialization */
#define JSON_POOL_BUFFER_SIZE   2048
#define JSON_POOL_BUFFER_COUNT  4

/** @brief MQTT pool: 8 buffers x 512B for MQTT messages */
#define MQTT_POOL_BUFFER_SIZE   512
#define MQTT_POOL_BUFFER_COUNT  8

/* ============================================================================
 * Buffer Pool Handles
 * ============================================================================ */

static buffer_pool_t s_json_pool = NULL;
static buffer_pool_t s_mqtt_pool = NULL;

/* ============================================================================
 * Module State
 * ============================================================================ */

/**
 * @brief Foundation module state
 */
typedef struct {
    bool initialized;            /**< foundation_init() completed */
    bool running;                /**< foundation_start_adapters() completed */
    uint32_t init_mask;          /**< Successfully initialized components */
    uint32_t started_mask;       /**< Successfully started adapters */
    uint32_t failed_mask;        /**< Components that failed to initialize */
    esp_err_t first_error;       /**< First error encountered */
    const char *first_error_comp; /**< Name of component with first error */
} foundation_state_t;

static foundation_state_t s_state = {0};

/* ============================================================================
 * Helper Macros
 * ============================================================================ */

/**
 * @brief Initialize a component and track result
 *
 * @param init_func Initialization function to call
 * @param comp_flag Component bitmask flag
 * @param comp_name Component name string
 * @param is_core True if this is a core component (failure is fatal)
 */
#define INIT_COMPONENT(init_func, comp_flag, comp_name, is_core) \
    do { \
        ESP_LOGI(TAG, "Initializing %s...", comp_name); \
        esp_err_t _err = init_func(); \
        if (_err == ESP_OK) { \
            s_state.init_mask |= comp_flag; \
            ESP_LOGI(TAG, "%s initialized successfully", comp_name); \
        } else { \
            s_state.failed_mask |= comp_flag; \
            ESP_LOGE(TAG, "%s initialization failed: %s (0x%x)", \
                     comp_name, esp_err_to_name(_err), _err); \
            if (s_state.first_error == ESP_OK) { \
                s_state.first_error = _err; \
                s_state.first_error_comp = comp_name; \
            } \
            if (is_core) { \
                first_error = (first_error == ESP_OK) ? _err : first_error; \
            } \
        } \
    } while (0)

/**
 * @brief Initialize an optional component (failure is not fatal)
 */
#define INIT_OPTIONAL_COMPONENT(init_func, comp_flag, comp_name) \
    INIT_COMPONENT(init_func, comp_flag, comp_name, false)

/**
 * @brief Initialize a core component (failure is fatal)
 */
#define INIT_CORE_COMPONENT(init_func, comp_flag, comp_name) \
    INIT_COMPONENT(init_func, comp_flag, comp_name, true)

/**
 * @brief Start an adapter and track result
 */
#define START_ADAPTER(start_func, comp_flag, comp_name) \
    do { \
        if (s_state.init_mask & comp_flag) { \
            ESP_LOGI(TAG, "Starting %s...", comp_name); \
            esp_err_t _err = start_func(); \
            if (_err == ESP_OK) { \
                s_state.started_mask |= comp_flag; \
                ESP_LOGI(TAG, "%s started successfully", comp_name); \
            } else if (_err == ESP_ERR_NOT_SUPPORTED) { \
                ESP_LOGW(TAG, "%s not supported on this platform", comp_name); \
            } else { \
                ESP_LOGE(TAG, "%s start failed: %s (0x%x)", \
                         comp_name, esp_err_to_name(_err), _err); \
                if (first_error == ESP_OK) { \
                    first_error = _err; \
                } \
            } \
        } else { \
            ESP_LOGW(TAG, "Skipping %s start (not initialized)", comp_name); \
        } \
    } while (0)

/**
 * @brief Deinitialize a component if it was initialized
 */
#define DEINIT_COMPONENT(deinit_func, comp_flag, comp_name) \
    do { \
        if (s_state.init_mask & comp_flag) { \
            ESP_LOGI(TAG, "Deinitializing %s...", comp_name); \
            esp_err_t _err = deinit_func(); \
            if (_err == ESP_OK) { \
                s_state.init_mask &= ~comp_flag; \
            } else { \
                ESP_LOGW(TAG, "%s deinit failed: %s", comp_name, esp_err_to_name(_err)); \
            } \
        } \
    } while (0)

/**
 * @brief Stop an adapter if it was started
 */
#define STOP_ADAPTER(stop_func, comp_flag, comp_name) \
    do { \
        if (s_state.started_mask & comp_flag) { \
            ESP_LOGI(TAG, "Stopping %s...", comp_name); \
            esp_err_t _err = stop_func(); \
            if (_err == ESP_OK) { \
                s_state.started_mask &= ~comp_flag; \
            } else if (_err != ESP_ERR_NOT_SUPPORTED) { \
                ESP_LOGW(TAG, "%s stop failed: %s", comp_name, esp_err_to_name(_err)); \
            } \
        } \
    } while (0)

/* ============================================================================
 * Stub Functions for Optional Components
 * ============================================================================ */

/**
 * @brief Initialize device persistence and load persisted devices
 *
 * Initializes the device_persistence module and loads any previously
 * persisted devices from NVS.
 */
static esp_err_t device_persistence_init_and_load(void)
{
    esp_err_t ret = device_persistence_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "device_persistence_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = device_persistence_load_all();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "device_persistence_load_all: %s", esp_err_to_name(ret));
        /* Non-fatal - continue even if no devices were loaded */
    }

    return ESP_OK;
}

/* ESPHome adapter is now implemented - using real esphome_adapter_init() */

/* ============================================================================
 * Buffer Pool Initialization
 * ============================================================================ */

/**
 * @brief Initialize buffer pools for frequent allocations
 *
 * Creates JSON and MQTT buffer pools. Falls back gracefully if
 * pool creation fails - the system can still operate with regular
 * heap allocations.
 */
static void init_buffer_pools(void)
{
    ESP_LOGI(TAG, "Initializing buffer pools...");

    /* Create JSON pool: 4 x 2KB for JSON serialization */
    s_json_pool = mem_pool_create("json", JSON_POOL_BUFFER_SIZE,
                                   JSON_POOL_BUFFER_COUNT, MEM_CAP_DEFAULT);
    if (s_json_pool != NULL) {
        ESP_LOGI(TAG, "JSON pool created: %d x %d bytes",
                 JSON_POOL_BUFFER_COUNT, JSON_POOL_BUFFER_SIZE);
    } else {
        ESP_LOGW(TAG, "JSON pool creation failed, will use heap allocations");
    }

    /* Create MQTT pool: 8 x 512B for MQTT messages */
    s_mqtt_pool = mem_pool_create("mqtt", MQTT_POOL_BUFFER_SIZE,
                                   MQTT_POOL_BUFFER_COUNT, MEM_CAP_DEFAULT);
    if (s_mqtt_pool != NULL) {
        ESP_LOGI(TAG, "MQTT pool created: %d x %d bytes",
                 MQTT_POOL_BUFFER_COUNT, MQTT_POOL_BUFFER_SIZE);
    } else {
        ESP_LOGW(TAG, "MQTT pool creation failed, will use heap allocations");
    }
}

/**
 * @brief Destroy buffer pools
 */
static void deinit_buffer_pools(void)
{
    ESP_LOGI(TAG, "Destroying buffer pools...");

    if (s_mqtt_pool != NULL) {
        mem_pool_destroy(s_mqtt_pool);
        s_mqtt_pool = NULL;
    }

    if (s_json_pool != NULL) {
        mem_pool_destroy(s_json_pool);
        s_json_pool = NULL;
    }
}

/* ============================================================================
 * Initialization / Deinitialization
 * ============================================================================ */

esp_err_t foundation_init(void)
{
    if (s_state.initialized) {
        ESP_LOGW(TAG, "Foundation already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Foundation Initialization Starting");
    ESP_LOGI(TAG, "========================================");

    /* Reset state */
    memset(&s_state, 0, sizeof(s_state));
    esp_err_t first_error = ESP_OK;

    /* Phase 0: Buffer Pools (early initialization for other components to use) */
    ESP_LOGI(TAG, "--- Phase 0: Buffer Pools ---");
    init_buffer_pools();

    /* Phase 1: Core Components (order matters!) */
    ESP_LOGI(TAG, "--- Phase 1: Core Components ---");

    /* 1. Lifecycle Manager - State machine first */
    INIT_CORE_COMPONENT(lifecycle_init, FOUNDATION_COMP_LIFECYCLE, "lifecycle");

    /* 2. Memory Pressure Monitor */
    INIT_CORE_COMPONENT(memory_pressure_init, FOUNDATION_COMP_MEMORY_PRESSURE, "memory_pressure");

    /* 3. Event Bus - Event system */
    INIT_CORE_COMPONENT(event_bus_init, FOUNDATION_COMP_EVENT_BUS, "event_bus");

    /* 4. Device Registry - Device storage */
    INIT_CORE_COMPONENT(device_registry_init, FOUNDATION_COMP_DEVICE_REGISTRY, "device_registry");

    /* 5. Device Persistence - Initialize and load saved devices */
    INIT_CORE_COMPONENT(device_persistence_init_and_load, FOUNDATION_COMP_DEVICE_PERSIST, "device_persistence");

    /* Phase 2: Discovery */
    ESP_LOGI(TAG, "--- Phase 2: Discovery ---");

    /* 6. HA Discovery NG - Subscribe to device events */
    INIT_OPTIONAL_COMPONENT(ha_discovery_ng_init, FOUNDATION_COMP_HA_DISCOVERY, "ha_discovery_ng");

#if CONFIG_EVENT_TRACE_ENABLE
    /* 6a. Event trace — subscribes to every event and keeps a ring buffer.
     * Off by default: it sees the full event stream, so it costs CPU and log
     * volume on a busy gateway. Built and never wired until now. */
    INIT_OPTIONAL_COMPONENT(event_trace_init, FOUNDATION_COMP_EVENT_TRACE, "event_trace");
#endif

#if CONFIG_MEMORY_DASHBOARD_ENABLE
    /* 6b. Memory dashboard — periodic memory statistics over MQTT.
     * Safe to start here: it checks mqtt_client_is_connected() itself and
     * simply skips a cycle while the broker is unreachable. */
    INIT_OPTIONAL_COMPONENT(memory_dashboard_init, FOUNDATION_COMP_MEMORY_DASHBOARD,
                            "memory_dashboard");
    if ((s_state.init_mask & FOUNDATION_COMP_MEMORY_DASHBOARD) != 0) {
        esp_err_t dash_ret =
            memory_dashboard_start(CONFIG_MEMORY_DASHBOARD_INTERVAL_SEC * 1000);
        if (dash_ret != ESP_OK) {
            ESP_LOGW(TAG, "memory_dashboard_start failed: %s", esp_err_to_name(dash_ret));
        }
    }
#endif

    /* Phase 3: Adapters via Adapter Registry (init only, don't start yet) */
    ESP_LOGI(TAG, "--- Phase 3: Adapters ---");

    /* Register all adapters with the unified registry */
    adapter_registry_register("zigbee", &zigbee_adapter_ops);
    adapter_registry_register("mqtt", &mqtt_adapter_ops);
#if CONFIG_BT_ENABLED
    adapter_registry_register("ble", &ble_adapter_ops);
#endif
    adapter_registry_register("esphome", &esphome_adapter_ops);

    /* Initialize all registered adapters */
    esp_err_t adapter_err = adapter_registry_init_all();
    if (adapter_err != ESP_OK && first_error == ESP_OK) {
        first_error = adapter_err;
    }

    /* Update init_mask for backwards compatibility with existing status checks */
    for (size_t i = 0; i < adapter_registry_count(); i++) {
        const adapter_desc_t *ad = adapter_registry_get(i);
        if (ad && ad->initialized) {
            if (strcmp(ad->name, "zigbee") == 0) s_state.init_mask |= FOUNDATION_COMP_ZIGBEE_ADAPTER;
            else if (strcmp(ad->name, "mqtt") == 0) s_state.init_mask |= FOUNDATION_COMP_MQTT_ADAPTER;
            else if (strcmp(ad->name, "ble") == 0) s_state.init_mask |= FOUNDATION_COMP_BLE_ADAPTER;
            else if (strcmp(ad->name, "esphome") == 0) s_state.init_mask |= FOUNDATION_COMP_ESPHOME_ADAPTER;
        }
    }

    /* Mark as initialized */
    s_state.initialized = true;

    /* Log summary */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Foundation Initialization Complete");
    ESP_LOGI(TAG, "  Initialized: 0x%04lx", (unsigned long)s_state.init_mask);
    ESP_LOGI(TAG, "  Failed:      0x%04lx", (unsigned long)s_state.failed_mask);
    if (first_error != ESP_OK) {
        ESP_LOGE(TAG, "  First error: %s from %s",
                 esp_err_to_name(first_error),
                 s_state.first_error_comp ? s_state.first_error_comp : "unknown");
    }
    ESP_LOGI(TAG, "========================================");

    return first_error;
}

esp_err_t foundation_deinit(void)
{
    if (!s_state.initialized) {
        ESP_LOGW(TAG, "Foundation not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Foundation Deinitialization Starting");
    ESP_LOGI(TAG, "========================================");

    /* Stop adapters first if running */
    if (s_state.running) {
        foundation_stop_adapters();
    }

    /* Deinitialize all adapters via registry (reverse order) */
    adapter_registry_deinit_all();

    /* HA Discovery NG */
    DEINIT_COMPONENT(ha_discovery_ng_deinit, FOUNDATION_COMP_HA_DISCOVERY, "ha_discovery_ng");

    /* Device Persistence (no deinit needed for stub) */

    /* Device Registry */
    DEINIT_COMPONENT(device_registry_deinit, FOUNDATION_COMP_DEVICE_REGISTRY, "device_registry");

    /* Event Bus */
    DEINIT_COMPONENT(event_bus_deinit, FOUNDATION_COMP_EVENT_BUS, "event_bus");

    /* Memory Pressure - no explicit deinit function */
    if (s_state.init_mask & FOUNDATION_COMP_MEMORY_PRESSURE) {
        memory_pressure_stop();
        s_state.init_mask &= ~FOUNDATION_COMP_MEMORY_PRESSURE;
    }

    /* Lifecycle - no explicit deinit function */
    s_state.init_mask &= ~FOUNDATION_COMP_LIFECYCLE;

    /* Destroy buffer pools last */
    deinit_buffer_pools();

    /* Clear state */
    s_state.initialized = false;
    s_state.running = false;

    ESP_LOGI(TAG, "Foundation deinitialization complete");
    return ESP_OK;
}

/* ============================================================================
 * Adapter Start / Stop
 * ============================================================================ */

esp_err_t foundation_start_adapters(void)
{
    if (!s_state.initialized) {
        ESP_LOGE(TAG, "Foundation not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state.running) {
        ESP_LOGW(TAG, "Adapters already started");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Starting Foundation Adapters");
    ESP_LOGI(TAG, "========================================");

    esp_err_t first_error = ESP_OK;

    /* 1. Start memory pressure monitoring */
    if (s_state.init_mask & FOUNDATION_COMP_MEMORY_PRESSURE) {
        ESP_LOGI(TAG, "Starting memory pressure monitoring...");
        esp_err_t err = memory_pressure_start();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Memory pressure monitoring started");
        } else {
            ESP_LOGW(TAG, "Memory pressure start failed: %s", esp_err_to_name(err));
        }
    }

    /* 2. Transition to running state */
    if (s_state.init_mask & FOUNDATION_COMP_LIFECYCLE) {
        ESP_LOGI(TAG, "Transitioning to RUNNING state...");
        esp_err_t err = lifecycle_transition(LIFECYCLE_RUNNING);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Lifecycle now in RUNNING state");
        } else {
            ESP_LOGW(TAG, "Lifecycle transition failed: %s", esp_err_to_name(err));
        }
    }

    /* 3. Start all adapters via registry (in registration order) */
    esp_err_t adapter_start_err = adapter_registry_start_all();
    if (adapter_start_err != ESP_OK && first_error == ESP_OK) {
        first_error = adapter_start_err;
    }

    /* Update started_mask for backwards compatibility */
    for (size_t i = 0; i < adapter_registry_count(); i++) {
        const adapter_desc_t *ad = adapter_registry_get(i);
        if (ad && ad->started) {
            if (strcmp(ad->name, "zigbee") == 0) s_state.started_mask |= FOUNDATION_COMP_ZIGBEE_ADAPTER;
            else if (strcmp(ad->name, "ble") == 0) s_state.started_mask |= FOUNDATION_COMP_BLE_ADAPTER;
        }
    }

    /* 4. Sync all Zigbee devices to unified registry (adapter-specific post-start) */
    if (s_state.started_mask & FOUNDATION_COMP_ZIGBEE_ADAPTER) {
        ESP_LOGI(TAG, "Syncing Zigbee devices to unified registry...");
        esp_err_t err = zigbee_adapter_sync_all_devices();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Zigbee devices synced successfully");
        } else {
            ESP_LOGW(TAG, "Zigbee device sync failed: %s", esp_err_to_name(err));
        }
    }

    /* Log adapter status */
    adapter_registry_log_status();

    /* Mark as running */
    s_state.running = true;

    /* Log summary */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Foundation Adapters Started");
    ESP_LOGI(TAG, "  Started: 0x%04lx", (unsigned long)s_state.started_mask);
    ESP_LOGI(TAG, "========================================");

    return first_error;
}

esp_err_t foundation_stop_adapters(void)
{
    if (!s_state.running) {
        ESP_LOGW(TAG, "Adapters not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Stopping Foundation Adapters");
    ESP_LOGI(TAG, "========================================");

    /* Stop all adapters in reverse registration order via registry */
    adapter_registry_stop_all();
    s_state.started_mask = 0;

    /* Stop memory pressure monitoring */
    if (s_state.init_mask & FOUNDATION_COMP_MEMORY_PRESSURE) {
        ESP_LOGI(TAG, "Stopping memory pressure monitoring...");
        memory_pressure_stop();
    }

    /* Transition to shutdown state */
    if (s_state.init_mask & FOUNDATION_COMP_LIFECYCLE) {
        ESP_LOGI(TAG, "Transitioning to SHUTDOWN state...");
        lifecycle_transition(LIFECYCLE_SHUTDOWN);
    }

    /* Mark as not running */
    s_state.running = false;

    ESP_LOGI(TAG, "Foundation adapters stopped");
    return ESP_OK;
}

/* ============================================================================
 * Status & Diagnostics
 * ============================================================================ */

bool foundation_is_initialized(void)
{
    return s_state.initialized;
}

bool foundation_is_running(void)
{
    return s_state.running;
}

void foundation_get_status(foundation_status_t *status)
{
    if (status == NULL) {
        return;
    }

    status->initialized_mask = s_state.init_mask;
    status->started_mask = s_state.started_mask;
    status->failed_mask = s_state.failed_mask;
    status->first_error = s_state.first_error;
    status->first_error_comp = s_state.first_error_comp;
}

bool foundation_is_component_initialized(foundation_component_t component)
{
    return (s_state.init_mask & component) != 0;
}

const char *foundation_component_to_str(foundation_component_t component)
{
    switch (component) {
        case FOUNDATION_COMP_LIFECYCLE:       return "lifecycle";
        case FOUNDATION_COMP_MEMORY_PRESSURE: return "memory_pressure";
        case FOUNDATION_COMP_EVENT_BUS:       return "event_bus";
        case FOUNDATION_COMP_DEVICE_REGISTRY: return "device_registry";
        case FOUNDATION_COMP_DEVICE_PERSIST:  return "device_persistence";
        case FOUNDATION_COMP_HA_DISCOVERY:    return "ha_discovery_ng";
        case FOUNDATION_COMP_ZIGBEE_ADAPTER:  return "zigbee_adapter";
        case FOUNDATION_COMP_MQTT_ADAPTER:    return "mqtt_adapter";
        case FOUNDATION_COMP_BLE_ADAPTER:     return "ble_adapter";
        case FOUNDATION_COMP_ESPHOME_ADAPTER: return "esphome_adapter";
        default:                              return "UNKNOWN";
    }
}

void foundation_print_status(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Foundation Status");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Initialized: %s", s_state.initialized ? "yes" : "no");
    ESP_LOGI(TAG, "Running:     %s", s_state.running ? "yes" : "no");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Component Status:");

    /* Define component list for iteration */
    static const struct {
        foundation_component_t flag;
        const char *name;
    } components[] = {
        { FOUNDATION_COMP_LIFECYCLE,       "lifecycle" },
        { FOUNDATION_COMP_MEMORY_PRESSURE, "memory_pressure" },
        { FOUNDATION_COMP_EVENT_BUS,       "event_bus" },
        { FOUNDATION_COMP_DEVICE_REGISTRY, "device_registry" },
        { FOUNDATION_COMP_DEVICE_PERSIST,  "device_persistence" },
        { FOUNDATION_COMP_HA_DISCOVERY,    "ha_discovery_ng" },
        { FOUNDATION_COMP_ZIGBEE_ADAPTER,  "zigbee_adapter" },
        { FOUNDATION_COMP_MQTT_ADAPTER,    "mqtt_adapter" },
        { FOUNDATION_COMP_BLE_ADAPTER,     "ble_adapter" },
        { FOUNDATION_COMP_ESPHOME_ADAPTER, "esphome_adapter" },
    };

    for (size_t i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
        const char *status;
        if (s_state.failed_mask & components[i].flag) {
            status = "FAILED";
        } else if (s_state.started_mask & components[i].flag) {
            status = "RUNNING";
        } else if (s_state.init_mask & components[i].flag) {
            status = "INITIALIZED";
        } else {
            status = "NOT INIT";
        }
        ESP_LOGI(TAG, "  %-20s %s", components[i].name, status);
    }

    if (s_state.first_error != ESP_OK) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "First Error: %s (0x%x) from %s",
                 esp_err_to_name(s_state.first_error),
                 s_state.first_error,
                 s_state.first_error_comp ? s_state.first_error_comp : "unknown");
    }

    /* Buffer pool statistics */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Buffer Pools:");

    if (s_json_pool != NULL) {
        size_t total, available;
        mem_pool_stats(s_json_pool, &total, &available);
        size_t used = total - available;
        ESP_LOGI(TAG, "  JSON pool:  %u/%u used (%u x %d bytes)",
                 (unsigned)used, (unsigned)total, JSON_POOL_BUFFER_COUNT, JSON_POOL_BUFFER_SIZE);
    } else {
        ESP_LOGI(TAG, "  JSON pool:  not initialized");
    }

    if (s_mqtt_pool != NULL) {
        size_t total, available;
        mem_pool_stats(s_mqtt_pool, &total, &available);
        size_t used = total - available;
        ESP_LOGI(TAG, "  MQTT pool:  %u/%u used (%u x %d bytes)",
                 (unsigned)used, (unsigned)total, MQTT_POOL_BUFFER_COUNT, MQTT_POOL_BUFFER_SIZE);
    } else {
        ESP_LOGI(TAG, "  MQTT pool:  not initialized");
    }

    ESP_LOGI(TAG, "========================================");
}

/* ============================================================================
 * Buffer Pool Access
 * ============================================================================ */

buffer_pool_t foundation_get_json_pool(void)
{
    return s_json_pool;
}

buffer_pool_t foundation_get_mqtt_pool(void)
{
    return s_mqtt_pool;
}
