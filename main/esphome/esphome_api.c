/**
 * @file esphome_api.c
 * @brief ESPHome Native API Server - Public API and Coordination
 *
 * This is the main entry point for the ESPHome API module. It provides:
 * - Public API functions (init, start, stop, deinit)
 * - mDNS service registration
 * - High-level coordination between modules
 * - Log streaming to subscribed clients
 *
 * The implementation is split across three files:
 * - esphome_api.c (this file): Public API, mDNS, log streaming
 * - esphome_api_server.c: TCP server, socket handling, client management
 * - esphome_api_handlers.c: Message type handlers and dispatch
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_api.h"
#include "esphome_api_internal.h"
#include "esphome_protocol.h"
#include "esphome_ble_proxy.h"
#include "esphome_device_registry.h"
#include "core/memory/memory_manager_ng.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "core/monitoring/system_monitor.h"

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
#include "esphome_noise.h"
#endif

/* Log tag */
static const char *TAG = "ESPHOME_API";

/* Forward declarations */
static void esphome_api_state_change_callback(esphome_entity_type_t entity_type,
                                               esphome_entity_key_t key, const void *state);

/* ============================================================================
 * System Entity Keys
 * ============================================================================ */

/** @brief Unique entity key for free heap sensor */
#define ENTITY_KEY_FREE_HEAP    0x3B9F7A2Du
/** @brief Unique entity key for WiFi signal sensor */
#define ENTITY_KEY_WIFI_SIGNAL  0x7C4E8B1Fu
/** @brief Unique entity key for uptime sensor */
#define ENTITY_KEY_UPTIME       0x2D1A5E03u
/** @brief Unique entity key for CPU usage sensor */
#define ENTITY_KEY_CPU_USAGE    0x5E6D9C41u

/** @brief System stats update interval in seconds */
#define SYSTEM_STATS_UPDATE_INTERVAL_SEC    60

/* ============================================================================
 * Module State
 * ============================================================================ */

/**
 * @brief Global API server state
 */
static esphome_api_state_t s_api = {
    .server_socket = -1,
    .ha_state_callback = NULL,
    .initialized = false,
    .running = false,
};

/**
 * @brief Get pointer to global API state (for internal modules)
 */
esphome_api_state_t *esphome_api_get_state(void)
{
    return &s_api;
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * @brief Get current timestamp in milliseconds
 */
uint32_t esphome_api_get_timestamp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * @brief Get MAC address string
 */
void esphome_api_get_mac_address(char *mac_str, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, len, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * @brief Find free client slot
 */
int esphome_api_find_free_client_slot(void)
{
    for (int i = 0; i < s_api.config.max_clients; i++) {
        if (s_api.clients[i].socket < 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Count active clients
 */
uint8_t esphome_api_count_active_clients(void)
{
    uint8_t count = 0;
    for (int i = 0; i < s_api.config.max_clients; i++) {
        if (s_api.clients[i].socket >= 0) {
            count++;
        }
    }
    return count;
}

/**
 * @brief Get client ID from client pointer
 */
uint8_t esphome_api_get_client_index(esphome_client_t *client)
{
    /* Calculate index from pointer offset */
    ptrdiff_t offset = client - s_api.clients;
    if (offset >= 0 && offset < s_api.config.max_clients) {
        return (uint8_t)offset;
    }
    return 0;  /* Default to first client if invalid */
}

/**
 * @brief Close client connection
 */
void esphome_api_close_client(esphome_client_t *client, uint8_t client_id)
{
    if (client->socket >= 0) {
        ESP_LOGI(TAG, "Closing client %d connection", client_id);

        /* Publish disconnect event before clearing state */
        evt_esphome_disconnected_t evt = {
            .client_id = client_id,
            .client_info = client->client_info[0] ? client->client_info : NULL,
        };
        event_publish(EVT_ESPHOME_DISCONNECTED, &evt, sizeof(evt));

        close(client->socket);
        client->socket = -1;
        client->state = ESPHOME_CLIENT_DISCONNECTED;

        /* Clear all subscription states */
        client->subscribed_states = false;
        client->subscribed_logs = false;
        client->subscribed_ha_states = false;
        client->log_level = ESPHOME_LOG_LEVEL_NONE;

        /* Clear keepalive state */
        client->ping_pending = false;
        client->last_ping_sent = 0;
        client->last_pong_received = 0;

        /* Clear buffers */
        client->rx_buffer_len = 0;
        memset(client->client_info, 0, sizeof(client->client_info));

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
        /* Cleanup Noise encryption context */
        if (client->noise_ctx) {
            esphome_noise_destroy(client->noise_ctx);
            client->noise_ctx = NULL;
        }
        client->encryption_enabled = false;
#endif

        s_api.stats.active_connections = esphome_api_count_active_clients();

        /* Notify callback */
        if (s_api.connection_callback) {
            s_api.connection_callback(client_id, false, false);
        }

        /* Cleanup BLE proxy resources for this client */
        esphome_ble_proxy_client_disconnected(client_id);
    }
}

/**
 * @brief Uptime timer callback (1s period)
 */
static void uptime_timer_callback(void *arg)
{
    s_api.stats.uptime_seconds++;
}

/**
 * @brief System stats timer callback (60s period)
 *
 * Updates system monitoring entities with current values.
 * Runs in esp_timer task context — mutexes and blocking calls are allowed.
 */
static void system_stats_timer_callback(void *arg)
{
    system_stats_t stats = system_monitor_get_stats();

    esphome_entity_update_sensor(ENTITY_KEY_FREE_HEAP,
                                  (float)stats.free_heap / 1024.0f);
    esphome_entity_update_sensor(ENTITY_KEY_WIFI_SIGNAL,
                                  (float)stats.wifi_rssi);
    esphome_entity_update_sensor(ENTITY_KEY_UPTIME,
                                  (float)stats.uptime_seconds);
    esphome_entity_update_sensor(ENTITY_KEY_CPU_USAGE,
                                  (float)stats.cpu_usage);
}

/**
 * @brief Register system monitoring entities
 *
 * Best-effort registration — logs warnings on failure but does not abort.
 */
static void register_system_entities(void)
{
    esp_err_t ret;

    /* Free Heap sensor */
    esphome_sensor_config_t heap_cfg = {
        .key = ENTITY_KEY_FREE_HEAP,
        .name = "Free Heap",
        .unique_id = "esp32c5_gw_free_heap",
        .icon = "mdi:memory",
        .unit_of_measurement = "kB",
        .accuracy_decimals = 1,
        .force_update = false,
        .device_class = ESPHOME_SENSOR_CLASS_NONE,
        .state_class = ESPHOME_STATE_CLASS_MEASUREMENT,
        .disabled_by_default = false,
        .entity_category = 2, /* DIAGNOSTIC */
    };
    ret = esphome_entity_register_sensor(&heap_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register free_heap entity: %s", esp_err_to_name(ret));
    }

    /* WiFi Signal sensor */
    esphome_sensor_config_t wifi_cfg = {
        .key = ENTITY_KEY_WIFI_SIGNAL,
        .name = "WiFi Signal",
        .unique_id = "esp32c5_gw_wifi_signal",
        .icon = "mdi:wifi",
        .unit_of_measurement = "dBm",
        .accuracy_decimals = 0,
        .force_update = false,
        .device_class = ESPHOME_SENSOR_CLASS_SIGNAL_STRENGTH,
        .state_class = ESPHOME_STATE_CLASS_MEASUREMENT,
        .disabled_by_default = false,
        .entity_category = 2, /* DIAGNOSTIC */
    };
    ret = esphome_entity_register_sensor(&wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register wifi_signal entity: %s", esp_err_to_name(ret));
    }

    /* Uptime sensor */
    esphome_sensor_config_t uptime_cfg = {
        .key = ENTITY_KEY_UPTIME,
        .name = "Uptime",
        .unique_id = "esp32c5_gw_uptime",
        .icon = "mdi:timer-outline",
        .unit_of_measurement = "s",
        .accuracy_decimals = 0,
        .force_update = false,
        .device_class = ESPHOME_SENSOR_CLASS_NONE,
        .state_class = ESPHOME_STATE_CLASS_MEASUREMENT,
        .disabled_by_default = false,
        .entity_category = 2, /* DIAGNOSTIC */
    };
    ret = esphome_entity_register_sensor(&uptime_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register uptime entity: %s", esp_err_to_name(ret));
    }

    /* CPU Usage sensor */
    esphome_sensor_config_t cpu_cfg = {
        .key = ENTITY_KEY_CPU_USAGE,
        .name = "CPU Usage",
        .unique_id = "esp32c5_gw_cpu_usage",
        .icon = "mdi:cpu-64-bit",
        .unit_of_measurement = "%",
        .accuracy_decimals = 0,
        .force_update = false,
        .device_class = ESPHOME_SENSOR_CLASS_NONE,
        .state_class = ESPHOME_STATE_CLASS_MEASUREMENT,
        .disabled_by_default = false,
        .entity_category = 2, /* DIAGNOSTIC */
    };
    ret = esphome_entity_register_sensor(&cpu_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register cpu_usage entity: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "System monitoring entities registered");
}

/* ============================================================================
 * mDNS Functions
 * ============================================================================ */

/**
 * @brief Initialize mDNS service
 */
static esp_err_t init_mdns(void)
{
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set hostname */
    ret = mdns_hostname_set(s_api.config.device_name);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS hostname set failed: %s", esp_err_to_name(ret));
    }

    /* Set instance name */
    ret = mdns_instance_name_set(s_api.config.friendly_name);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance name set failed: %s", esp_err_to_name(ret));
    }

    /* Add ESPHome service */
    ret = mdns_service_add(s_api.config.friendly_name, "_esphomelib", "_tcp",
                          s_api.config.port, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS service add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Add TXT records */
    char mac[18];
    esphome_api_get_mac_address(mac, sizeof(mac));

    mdns_txt_item_t txt_records[] = {
        {"mac", mac},
        {"version", s_api.device_version[0] ? s_api.device_version : "1.0.0"},
        {"friendly_name", s_api.config.friendly_name},
        {"platform", "ESP32-C5"},
        {"board", "esp32c5"},
        {"network", "wifi"},
    };

    ret = mdns_service_txt_set("_esphomelib", "_tcp", txt_records,
                               sizeof(txt_records) / sizeof(txt_records[0]));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS TXT set failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "mDNS service announced: %s._esphomelib._tcp.local:%d",
            s_api.config.device_name, s_api.config.port);

    return ESP_OK;
}

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Initialize ESPHome API server
 */
esp_err_t esphome_api_init(const esphome_api_config_t *config)
{
    if (s_api.initialized) {
        ESP_LOGW(TAG, "API server already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing ESPHome API server...");

    /* Copy or use default configuration */
    if (config) {
        memcpy(&s_api.config, config, sizeof(esphome_api_config_t));
    } else {
        esphome_api_config_t default_config = ESPHOME_API_CONFIG_DEFAULT();
        memcpy(&s_api.config, &default_config, sizeof(esphome_api_config_t));
    }

    /* Validate configuration */
    if (s_api.config.max_clients > ESPHOME_MAX_CLIENTS) {
        s_api.config.max_clients = ESPHOME_MAX_CLIENTS;
    }
    if (s_api.config.max_clients == 0) {
        s_api.config.max_clients = 1;
    }

    /* Create mutex */
    s_api.mutex = xSemaphoreCreateMutex();
    if (!s_api.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create event group */
    s_api.event_group = xEventGroupCreate();
    if (!s_api.event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(s_api.mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Initialize client slots */
    for (int i = 0; i < ESPHOME_MAX_CLIENTS; i++) {
        s_api.clients[i].socket = -1;
        s_api.clients[i].state = ESPHOME_CLIENT_DISCONNECTED;
        memset(&s_api.clients[i].psram_task, 0, sizeof(psram_task_handle_t));
#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
        s_api.clients[i].noise_ctx = NULL;
        s_api.clients[i].encryption_enabled = false;
#endif
    }

    /* Create uptime timer (1s period) */
    const esp_timer_create_args_t timer_args = {
        .callback = uptime_timer_callback,
        .name = "esphome_uptime"
    };
    esp_err_t ret = esp_timer_create(&timer_args, &s_api.uptime_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create uptime timer");
        vEventGroupDelete(s_api.event_group);
        vSemaphoreDelete(s_api.mutex);
        return ret;
    }

    /* Create system stats timer (60s period) */
    const esp_timer_create_args_t stats_timer_args = {
        .callback = system_stats_timer_callback,
        .name = "esphome_sys_stats"
    };
    ret = esp_timer_create(&stats_timer_args, &s_api.stats_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create stats timer");
        esp_timer_delete(s_api.uptime_timer);
        vEventGroupDelete(s_api.event_group);
        vSemaphoreDelete(s_api.mutex);
        return ret;
    }

    /* Initialize statistics */
    memset(&s_api.stats, 0, sizeof(esphome_api_stats_t));

    /* Set default device info */
    strlcpy(s_api.device_version, "1.0.0", sizeof(s_api.device_version));
    strlcpy(s_api.device_model, "ESP32-C5", sizeof(s_api.device_model));

    /* Initialize entity manager */
    ret = esphome_entities_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize entity manager");
        esp_timer_delete(s_api.stats_timer);
        esp_timer_delete(s_api.uptime_timer);
        vEventGroupDelete(s_api.event_group);
        vSemaphoreDelete(s_api.mutex);
        return ret;
    }

#if CONFIG_ESPHOME_ENTITY_REGISTRY_MIRROR
    /* Mirror ESPHome entities into the unified device_registry.
     *
     * This is what makes entity states show up as MQTT topics, but it costs one
     * registry slot per entity — measured at 54 of 64 slots on a gateway with
     * two paired Zigbee devices. Every other caller of this module is already
     * guarded by esphome_device_registry_is_initialized(), so skipping the init
     * is all that is needed to turn the whole mirror off.
     *
     * See CONFIG_ESPHOME_ENTITY_REGISTRY_MIRROR for the trade-off. */
    ret = esphome_device_registry_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Device registry integration not available: %s",
                 esp_err_to_name(ret));
        /* Continue without device registry integration - not critical */
    }
#else
    ESP_LOGI(TAG, "ESPHome entity mirror disabled — entity states are not "
                  "written to the device registry or published over MQTT");
#endif

    /* Register state change callback for broadcasting to clients */
    esphome_entities_set_state_callback(esphome_api_state_change_callback);

    /* Register system monitoring entities */
    register_system_entities();

    /* Initialize protocol module */
    esphome_protocol_init();

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
    /* Initialize Noise encryption module */
    ret = esphome_noise_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Noise encryption");
        esphome_entities_deinit();
        esp_timer_delete(s_api.stats_timer);
        esp_timer_delete(s_api.uptime_timer);
        vEventGroupDelete(s_api.event_group);
        vSemaphoreDelete(s_api.mutex);
        return ret;
    }
    ESP_LOGI(TAG, "Noise Protocol encryption enabled");
#endif

    s_api.initialized = true;
    ESP_LOGI(TAG, "ESPHome API server initialized (port=%d, max_clients=%d)",
            s_api.config.port, s_api.config.max_clients);

    return ESP_OK;
}

/**
 * @brief Start ESPHome API server
 */
esp_err_t esphome_api_start(void)
{
    if (!s_api.initialized) {
        ESP_LOGE(TAG, "API server not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_api.running) {
        ESP_LOGW(TAG, "API server already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting ESPHome API server...");

    /* Create server socket */
    s_api.server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_api.server_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno=%d", errno);
        return ESP_FAIL;
    }

    /* Set socket options */
    int opt = 1;
    setsockopt(s_api.server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Set non-blocking with timeout for accept */
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_api.server_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /* Bind socket */
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(s_api.config.port),
    };

    if (bind(s_api.server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket: errno=%d", errno);
        close(s_api.server_socket);
        s_api.server_socket = -1;
        return ESP_FAIL;
    }

    /* Listen for connections */
    if (listen(s_api.server_socket, s_api.config.max_clients) < 0) {
        ESP_LOGE(TAG, "Failed to listen: errno=%d", errno);
        close(s_api.server_socket);
        s_api.server_socket = -1;
        return ESP_FAIL;
    }

    s_api.running = true;

    /* Start server task with PSRAM stack (saves ~4KB internal RAM) */
    esp_err_t err = psram_task_create(
        esphome_api_server_task,
        "esphome_srv",
        ESPHOME_SERVER_TASK_STACK,
        NULL,
        ESPHOME_SERVER_TASK_PRIO,
        &s_api.server_psram_task
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create server task: %s", esp_err_to_name(err));
        close(s_api.server_socket);
        s_api.server_socket = -1;
        s_api.running = false;
        return err;
    }
    /* Keep server_task updated for API compatibility */
    s_api.server_task = psram_task_get_handle(&s_api.server_psram_task);

    /* Start uptime timer */
    esp_err_t timer_ret = esp_timer_start_periodic(s_api.uptime_timer, 1000000); /* 1 second */
    if (timer_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start uptime timer: %s", esp_err_to_name(timer_ret));
    }

    /* Fire initial system stats update immediately */
    system_stats_timer_callback(NULL);

    /* Start system stats timer (60s) */
    timer_ret = esp_timer_start_periodic(s_api.stats_timer,
                                          (uint64_t)SYSTEM_STATS_UPDATE_INTERVAL_SEC * 1000000);
    if (timer_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start stats timer: %s", esp_err_to_name(timer_ret));
    }

    /* Initialize mDNS if enabled */
    if (s_api.config.use_mdns) {
        init_mdns();
    }

    ESP_LOGI(TAG, "ESPHome API server started on port %d", s_api.config.port);
    return ESP_OK;
}

/**
 * @brief Stop ESPHome API server
 */
esp_err_t esphome_api_stop(void)
{
    if (!s_api.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping ESPHome API server...");

    s_api.running = false;

    /* Signal server task to stop */
    xEventGroupSetBits(s_api.event_group, EVENT_STOP_SERVER);

    /* Stop uptime timer */
    esp_timer_stop(s_api.uptime_timer);
    esp_timer_stop(s_api.stats_timer);

    /* Close all client connections */
    esphome_api_disconnect_all_clients();

    /* Close server socket */
    if (s_api.server_socket >= 0) {
        close(s_api.server_socket);
        s_api.server_socket = -1;
    }

    /* Poll for server task to exit gracefully (up to 3 seconds) */
    uint32_t stop_timeout = 3000;
    while (psram_task_is_valid(&s_api.server_psram_task) && stop_timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        stop_timeout -= 50;
    }

    if (psram_task_is_valid(&s_api.server_psram_task)) {
        /* Task may hold s_api.mutex — force-delete would corrupt FreeRTOS
         * priority inheritance and trigger xTaskPriorityDisinherit assert.
         * Abandon the task instead (leaks ~4KB PSRAM, prevents crash). */
        ESP_LOGW(TAG, "Server task did not exit in time, abandoning");
        psram_task_mark_deleted(&s_api.server_psram_task);
    }

    /* Free PSRAM resources (stack and TCB) if task exited cleanly */
    psram_task_delete(&s_api.server_psram_task);
    s_api.server_task = NULL;

    /* Remove mDNS service */
    if (s_api.config.use_mdns) {
        esphome_api_remove_mdns();
    }

    ESP_LOGI(TAG, "ESPHome API server stopped");
    return ESP_OK;
}

/**
 * @brief Deinitialize ESPHome API server
 */
esp_err_t esphome_api_deinit(void)
{
    if (!s_api.initialized) {
        return ESP_OK;
    }

    /* Stop server if running */
    esphome_api_stop();

    /* Delete timers */
    if (s_api.stats_timer) {
        esp_timer_delete(s_api.stats_timer);
        s_api.stats_timer = NULL;
    }
    if (s_api.uptime_timer) {
        esp_timer_delete(s_api.uptime_timer);
        s_api.uptime_timer = NULL;
    }

    /* Delete event group */
    if (s_api.event_group) {
        vEventGroupDelete(s_api.event_group);
        s_api.event_group = NULL;
    }

    /* Delete mutex */
    if (s_api.mutex) {
        vSemaphoreDelete(s_api.mutex);
        s_api.mutex = NULL;
    }

    /* Free PSRAM task resources (stack and TCB) */
    psram_task_delete(&s_api.server_psram_task);
    s_api.server_task = NULL;

    /* Deinitialize device registry integration */
#if CONFIG_ESPHOME_ENTITY_REGISTRY_MIRROR
    esphome_device_registry_deinit();
#endif

    /* Deinitialize entity manager */
    esphome_entities_deinit();

    s_api.initialized = false;
    ESP_LOGI(TAG, "ESPHome API server deinitialized");

    return ESP_OK;
}

/**
 * @brief Check if API server is running
 */
bool esphome_api_is_running(void)
{
    return s_api.running;
}

/**
 * @brief Get number of connected clients
 */
uint8_t esphome_api_get_client_count(void)
{
    return esphome_api_count_active_clients();
}

/**
 * @brief Get server statistics
 */
esp_err_t esphome_api_get_stats(esphome_api_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(stats, &s_api.stats, sizeof(esphome_api_stats_t));
    return ESP_OK;
}

/**
 * @brief Reset server statistics
 */
esp_err_t esphome_api_reset_stats(void)
{
    memset(&s_api.stats, 0, sizeof(esphome_api_stats_t));
    return ESP_OK;
}

/**
 * @brief Disconnect a specific client
 */
esp_err_t esphome_api_disconnect_client(uint8_t client_id)
{
    if (client_id >= s_api.config.max_clients) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_api.mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esphome_api_close_client(&s_api.clients[client_id], client_id);

    xSemaphoreGive(s_api.mutex);
    return ESP_OK;
}

/**
 * @brief Disconnect all clients
 */
esp_err_t esphome_api_disconnect_all_clients(void)
{
    if (xSemaphoreTake(s_api.mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < s_api.config.max_clients; i++) {
        esphome_api_close_client(&s_api.clients[i], i);
    }

    xSemaphoreGive(s_api.mutex);
    return ESP_OK;
}

/* ============================================================================
 * State Broadcast Functions
 * ============================================================================ */

/**
 * @brief State change callback wrapper for entity manager
 */
static void esphome_api_state_change_callback(esphome_entity_type_t entity_type,
                                               esphome_entity_key_t key, const void *state)
{
    esphome_api_broadcast_state(entity_type, key, state);
}

/**
 * @brief Broadcast entity state to all subscribed clients
 */
esp_err_t esphome_api_broadcast_state(esphome_entity_type_t entity_type,
                                       esphome_entity_key_t key, const void *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Allocate buffer on heap to reduce stack usage */
    uint8_t *buffer = mem_alloc(ESPHOME_BUFFER_MEDIUM, MEM_CAP_DEFAULT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate broadcast state buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t buffer_len;
    esp_err_t ret = ESP_OK;

    switch (entity_type) {
        case ESPHOME_ENTITY_SENSOR:
            ret = esphome_encode_sensor_state((const esphome_sensor_state_t *)state,
                                              buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_BINARY_SENSOR:
            ret = esphome_encode_binary_sensor_state((const esphome_binary_sensor_state_t *)state,
                                                      buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_SWITCH:
            ret = esphome_encode_switch_state((const esphome_switch_state_t *)state,
                                              buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_TEXT_SENSOR:
            ret = esphome_encode_text_sensor_state((const esphome_text_sensor_state_t *)state,
                                                    buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_NUMBER:
            ret = esphome_encode_number_state((const esphome_number_state_t *)state,
                                              buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_SELECT:
            ret = esphome_encode_select_state((const esphome_select_state_t *)state,
                                              buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_LIGHT:
            ret = esphome_encode_light_state((const esphome_light_state_t *)state,
                                             buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_COVER:
            ret = esphome_encode_cover_state((const esphome_cover_state_t *)state,
                                             buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_FAN:
            ret = esphome_encode_fan_state((const esphome_fan_state_t *)state,
                                           buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_CLIMATE:
            ret = esphome_encode_climate_state((const esphome_climate_state_t *)state,
                                               buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_LOCK:
            ret = esphome_encode_lock_state((const esphome_lock_entity_state_t *)state,
                                            buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_TEXT:
            ret = esphome_encode_text_state((const esphome_text_entity_state_t *)state,
                                            buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_MEDIA_PLAYER:
            ret = esphome_encode_media_player_state((const esphome_media_player_entity_state_t *)state,
                                                     buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_ALARM_PANEL:
            ret = esphome_encode_alarm_state((const esphome_alarm_entity_state_t *)state,
                                              buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
            break;

        case ESPHOME_ENTITY_BUTTON:
            /* Buttons have no state to broadcast */
            mem_ng_free(buffer);
            return ESP_OK;

        default:
            mem_ng_free(buffer);
            return ESPHOME_ERR_INVALID_TYPE;
    }

    if (ret != ESP_OK) {
        mem_ng_free(buffer);
        return ret;
    }

    /* Send to all subscribed clients.
     * Use trylock (timeout=0) to avoid FreeRTOS priority-inheritance assertion
     * on single-core ESP32-C5 when the event dispatcher (prio 5) contends
     * with the ESPHome client task (prio 4) — both end up at the same
     * effective priority after inheritance, triggering
     * vTaskPriorityDisinheritAfterTimeout assert.  Skipping one broadcast
     * is harmless; the next state change will deliver the update. */
    if (xSemaphoreTake(s_api.mutex, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Broadcast skipped — API mutex busy");
        mem_ng_free(buffer);
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < s_api.config.max_clients; i++) {
        esphome_client_t *client = &s_api.clients[i];
        if (client->socket >= 0 && client->state == ESPHOME_CLIENT_AUTHENTICATED &&
            client->subscribed_states) {
            esphome_api_send_message(client, buffer, buffer_len);
        }
    }

    xSemaphoreGive(s_api.mutex);
    mem_ng_free(buffer);
    return ESP_OK;
}

/**
 * @brief Broadcast all entity states
 */
esp_err_t esphome_api_broadcast_all_states(void)
{
    if (xSemaphoreTake(s_api.mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < s_api.config.max_clients; i++) {
        esphome_client_t *client = &s_api.clients[i];
        if (client->socket >= 0 && client->state == ESPHOME_CLIENT_AUTHENTICATED &&
            client->subscribed_states) {
            esphome_broadcast_states_ctx_t ctx = { .client = client };
            esphome_entities_enumerate(esphome_api_broadcast_states_callback, &ctx);
        }
    }

    xSemaphoreGive(s_api.mutex);
    return ESP_OK;
}

/* ============================================================================
 * Device Information Functions
 * ============================================================================ */

/**
 * @brief Set device information
 */
esp_err_t esphome_api_set_device_info(const char *name, const char *version, const char *model)
{
    if (name) {
        strlcpy(s_api.config.device_name, name, sizeof(s_api.config.device_name));
    }
    if (version) {
        strlcpy(s_api.device_version, version, sizeof(s_api.device_version));
    }
    if (model) {
        strlcpy(s_api.device_model, model, sizeof(s_api.device_model));
    }
    return ESP_OK;
}

/**
 * @brief Update mDNS service TXT records
 */
esp_err_t esphome_api_update_mdns(void)
{
    if (!s_api.config.use_mdns) {
        return ESP_OK;
    }

    char mac[18];
    esphome_api_get_mac_address(mac, sizeof(mac));

    mdns_txt_item_t txt_records[] = {
        {"mac", mac},
        {"version", s_api.device_version},
        {"platform", "ESP32-C5"},
    };

    return mdns_service_txt_set("_esphomelib", "_tcp", txt_records,
                                sizeof(txt_records) / sizeof(txt_records[0]));
}

/**
 * @brief Remove mDNS service announcement
 */
esp_err_t esphome_api_remove_mdns(void)
{
    mdns_service_remove("_esphomelib", "_tcp");
    mdns_free();
    return ESP_OK;
}

/* ============================================================================
 * Callback Registration
 * ============================================================================ */

/**
 * @brief Register connection callback
 */
esp_err_t esphome_api_register_connection_callback(esphome_api_connection_cb_t callback)
{
    s_api.connection_callback = callback;
    return ESP_OK;
}

/* ============================================================================
 * Home Assistant Integration
 * ============================================================================ */

/**
 * @brief Register Home Assistant state callback
 */
esp_err_t esphome_api_register_ha_state_callback(esphome_ha_state_cb_t callback)
{
    s_api.ha_state_callback = callback;
    return ESP_OK;
}

/**
 * @brief Subscribe to Home Assistant entity state
 */
esp_err_t esphome_api_subscribe_ha_state(const char *entity_id)
{
    if (!entity_id || strlen(entity_id) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_api.running) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    /* Allocate buffers on heap to reduce stack usage */
    uint8_t *payload = mem_alloc(ESPHOME_BUFFER_MEDIUM, MEM_CAP_DEFAULT);
    uint8_t *output = mem_alloc(ESPHOME_BUFFER_LARGE, MEM_CAP_DEFAULT);
    if (payload == NULL || output == NULL) {
        ESP_LOGE(TAG, "Failed to allocate HA subscribe buffers");
        mem_ng_free(payload);
        mem_ng_free(output);
        return ESP_ERR_NO_MEM;
    }

    /* Build SubscribeHomeAssistantStateRequest message */
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, ESPHOME_BUFFER_MEDIUM);

    /* Field 1: entity_id (string) */
    esphome_encode_string(&buf, 1, entity_id);

    /* Build and send to all authenticated clients that support HA state */
    size_t output_len;
    ret = esphome_build_message(ESPHOME_MSG_SUBSCRIBE_HOME_ASSISTANT_STATES,
                                payload, buf.position,
                                output, ESPHOME_BUFFER_LARGE, &output_len);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    /* Send to all authenticated clients */
    for (int i = 0; i < s_api.config.max_clients; i++) {
        esphome_client_t *client = &s_api.clients[i];
        if (client->socket >= 0 && client->state == ESPHOME_CLIENT_AUTHENTICATED) {
            esphome_api_send_message(client, output, output_len);
        }
    }

    ESP_LOGD(TAG, "Subscribed to HA state: %s", entity_id);

cleanup:
    mem_ng_free(payload);
    mem_ng_free(output);
    return ret;
}

/**
 * @brief Call Home Assistant service
 */
esp_err_t esphome_api_call_ha_service(const char *domain, const char *service,
                                       const char *data_json)
{
    if (!domain || !service) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_api.running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Buffer size for HA service call output */
    #define HA_SERVICE_OUTPUT_SIZE 600

    esp_err_t ret = ESP_OK;

    /* Allocate buffers on heap to reduce stack usage */
    uint8_t *payload = mem_alloc(ESPHOME_BUFFER_LARGE, MEM_CAP_DEFAULT);
    uint8_t *output = mem_alloc(HA_SERVICE_OUTPUT_SIZE, MEM_CAP_DEFAULT);
    if (payload == NULL || output == NULL) {
        ESP_LOGE(TAG, "Failed to allocate HA service buffers");
        mem_ng_free(payload);
        mem_ng_free(output);
        return ESP_ERR_NO_MEM;
    }

    /* Build HomeAssistantServiceResponse message */
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, ESPHOME_BUFFER_LARGE);

    /* Field 1: service (domain.service format) */
    char service_name[ESPHOME_STRING_BUFFER_LONG];
    snprintf(service_name, sizeof(service_name), "%s.%s", domain, service);
    esphome_encode_string(&buf, 1, service_name);

    /* Field 2: data (as JSON string if provided) */
    if (data_json && strlen(data_json) > 0) {
        esphome_encode_string(&buf, 2, data_json);
    }

    /* Build message */
    size_t output_len;
    ret = esphome_build_message(ESPHOME_MSG_HA_SERVICE_RESPONSE,
                                payload, buf.position,
                                output, HA_SERVICE_OUTPUT_SIZE, &output_len);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    /* Send to all authenticated clients */
    for (int i = 0; i < s_api.config.max_clients; i++) {
        esphome_client_t *client = &s_api.clients[i];
        if (client->socket >= 0 && client->state == ESPHOME_CLIENT_AUTHENTICATED) {
            esphome_api_send_message(client, output, output_len);
        }
    }

    ESP_LOGI(TAG, "Called HA service: %s", service_name);

cleanup:
    mem_ng_free(payload);
    mem_ng_free(output);
    return ret;

    #undef HA_SERVICE_OUTPUT_SIZE
}

/* ============================================================================
 * Direct Client Messaging (for BLE Proxy)
 * ============================================================================ */

/**
 * @brief Send raw message to a specific client
 */
esp_err_t esphome_api_send_to_client(uint8_t client_id, const uint8_t *data, size_t len)
{
    if (client_id >= s_api.config.max_clients) {
        ESP_LOGW(TAG, "Invalid client ID: %u", client_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_client_t *client = &s_api.clients[client_id];

    if (client->socket < 0) {
        ESP_LOGD(TAG, "Client %u not connected", client_id);
        return ESP_ERR_INVALID_STATE;
    }

    if (client->state != ESPHOME_CLIENT_AUTHENTICATED) {
        ESP_LOGD(TAG, "Client %u not authenticated", client_id);
        return ESP_ERR_INVALID_STATE;
    }

    return esphome_api_send_message(client, data, len);
}

/**
 * @brief Get client ID for a client context
 */
uint8_t esphome_api_get_client_id(void *client_ptr)
{
    if (!client_ptr) {
        return 0;
    }

    esphome_client_t *client = (esphome_client_t *)client_ptr;
    return esphome_api_get_client_index(client);
}

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

/**
 * @brief Log server status and statistics
 */
void esphome_api_log_status(void)
{
    ESP_LOGI(TAG, "=== ESPHome API Server Status ===");
    ESP_LOGI(TAG, "Running: %s", s_api.running ? "yes" : "no");
    ESP_LOGI(TAG, "Port: %d", s_api.config.port);
    ESP_LOGI(TAG, "Active clients: %d / %d", esphome_api_count_active_clients(), s_api.config.max_clients);
    ESP_LOGI(TAG, "Total connections: %lu", s_api.stats.total_connections);
    ESP_LOGI(TAG, "Messages: rx=%lu, tx=%lu", s_api.stats.messages_received,
            s_api.stats.messages_sent);
    ESP_LOGI(TAG, "Bytes: rx=%llu, tx=%llu", s_api.stats.bytes_received, s_api.stats.bytes_sent);
    ESP_LOGI(TAG, "Auth failures: %lu", s_api.stats.authentication_failures);
    ESP_LOGI(TAG, "Protocol errors: %lu", s_api.stats.protocol_errors);
    ESP_LOGI(TAG, "Uptime: %lu seconds", s_api.stats.uptime_seconds);
    ESP_LOGI(TAG, "Entities: %zu", esphome_entities_get_total_count());
}

/**
 * @brief Run API server self-test
 */
esp_err_t esphome_api_test(void)
{
    ESP_LOGI(TAG, "=== ESPHome API Server Test ===");

    /* Test protocol module */
    esp_err_t ret = esphome_protocol_test();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Protocol test failed");
        return ESP_FAIL;
    }

    /* Test entity module */
    ret = esphome_entities_test();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Entity test failed");
        return ESP_FAIL;
    }

    /* Test initialization */
    ESP_LOGI(TAG, "Testing API initialization...");
    esphome_api_config_t config = ESPHOME_API_CONFIG_DEFAULT();
    config.port = ESPHOME_API_TEST_PORT; /* Use alternate port for testing */

    ret = esphome_api_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "API init failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    /* Check state */
    if (esphome_api_is_running()) {
        ESP_LOGE(TAG, "Server should not be running after init");
        esphome_api_deinit();
        return ESP_FAIL;
    }

    /* Cleanup */
    esphome_api_deinit();

    ESP_LOGI(TAG, "=== All API Server Tests PASSED ===");
    return ESP_OK;
}

/* ============================================================================
 * Log Subscription (ES-005)
 * ============================================================================ */

/* Original vprintf function pointer */
static vprintf_like_t s_original_vprintf = NULL;
static bool s_log_handler_registered = false;

/**
 * @brief Encode and send a log message to a client
 */
esp_err_t esphome_api_encode_and_send_log(esphome_client_t *client, esphome_log_level_t level,
                                          const char *tag, const char *message)
{
    uint8_t payload[300];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: level (enum as uint32) */
    esphome_encode_uint32(&buf, 1, (uint32_t)level);

    /* Field 2: tag (string) - ESPHome calls this "message" but sends tag here */
    esphome_encode_string(&buf, 2, tag);

    /* Field 3: message (string) */
    esphome_encode_string(&buf, 3, message);

    if (esphome_buffer_overflow(&buf)) {
        return ESPHOME_ERR_BUFFER_OVERFLOW;
    }

    uint8_t output[350];
    size_t output_len;
    esp_err_t ret = esphome_build_message(ESPHOME_MSG_SUBSCRIBE_LOGS_RESPONSE,
                                           payload, buf.position,
                                           output, sizeof(output), &output_len);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Send to client - use raw send to avoid mutex issues */
    ssize_t sent = send(client->socket, output, output_len, 0);
    if (sent < 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Custom vprintf function to intercept logs
 */
static int esphome_log_vprintf(const char *fmt, va_list args)
{
    /* First, call the original vprintf to print to console */
    int ret = 0;
    if (s_original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    /* Don't process if API not running or no clients */
    if (!s_api.running || esphome_api_count_active_clients() == 0) {
        return ret;
    }

    /* Use stack buffer - this function may be called from ISR context
     * where malloc() is not allowed */
    char message[ESPHOME_STRING_BUFFER_XLARGE];

    /* Format the message */
    vsnprintf(message, sizeof(message), fmt, args);

    /* Remove trailing newline if present */
    size_t len = strlen(message);
    while (len > 0 && (message[len - 1] == '\n' || message[len - 1] == '\r')) {
        message[--len] = '\0';
    }

    /* Parse log level and tag from ESP-IDF format: "I (12345) TAG: message" */
    esphome_log_level_t level = ESPHOME_LOG_LEVEL_INFO;
    char tag[ESPHOME_STRING_BUFFER_SHORT] = "UNKNOWN";
    char *msg_start = message;

    if (len > 0) {
        /* Parse log level from first character */
        switch (message[0]) {
            case 'E': level = ESPHOME_LOG_LEVEL_ERROR; break;
            case 'W': level = ESPHOME_LOG_LEVEL_WARN; break;
            case 'I': level = ESPHOME_LOG_LEVEL_INFO; break;
            case 'D': level = ESPHOME_LOG_LEVEL_DEBUG; break;
            case 'V': level = ESPHOME_LOG_LEVEL_VERBOSE; break;
        }

        /* Try to parse tag from format "X (time) TAG: message" */
        char *paren_start = strchr(message, '(');
        char *paren_end = paren_start ? strchr(paren_start, ')') : NULL;
        char *colon = paren_end ? strchr(paren_end, ':') : NULL;

        if (paren_end && colon && colon > paren_end + 2) {
            /* Extract tag between ) and : */
            size_t tag_len = colon - paren_end - 2;
            if (tag_len > 0 && tag_len < sizeof(tag)) {
                strncpy(tag, paren_end + 2, tag_len);
                tag[tag_len] = '\0';
            }
            /* Message starts after ": " */
            msg_start = colon + 2;
        }
    }

    /* Send to subscribed clients */
    esphome_api_send_log(level, tag, msg_start);

    return ret;
}

/**
 * @brief Register log handler
 */
esp_err_t esphome_api_register_log_handler(void)
{
    if (s_log_handler_registered) {
        return ESP_OK;
    }

    s_original_vprintf = esp_log_set_vprintf(esphome_log_vprintf);
    s_log_handler_registered = true;

    ESP_LOGI(TAG, "Log handler registered for ESPHome API");
    return ESP_OK;
}

/**
 * @brief Unregister log handler
 */
esp_err_t esphome_api_unregister_log_handler(void)
{
    if (!s_log_handler_registered) {
        return ESP_OK;
    }

    if (s_original_vprintf) {
        esp_log_set_vprintf(s_original_vprintf);
        s_original_vprintf = NULL;
    }

    s_log_handler_registered = false;
    ESP_LOGI(TAG, "Log handler unregistered");
    return ESP_OK;
}

/**
 * @brief Send log message to subscribed clients
 */
esp_err_t esphome_api_send_log(esphome_log_level_t level, const char *tag, const char *message)
{
    if (!tag || !message) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_api.running) {
        return ESP_OK;
    }

    /* Don't take mutex here to avoid deadlocks with logging inside mutex */
    for (int i = 0; i < s_api.config.max_clients; i++) {
        esphome_client_t *client = &s_api.clients[i];

        /* Check if client wants this log level */
        if (client->socket >= 0 &&
            client->state == ESPHOME_CLIENT_AUTHENTICATED &&
            client->subscribed_logs &&
            level <= client->log_level) {

            esphome_api_encode_and_send_log(client, level, tag, message);
        }
    }

    return ESP_OK;
}
