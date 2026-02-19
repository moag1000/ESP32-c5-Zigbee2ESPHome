/**
 * @file perf_metrics.c
 * @brief Performance Metrics Module Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "perf_metrics.h"
#include "task_manager.h"
#include "gateway_mqtt.h"
#include "mqtt/mqtt_helpers.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "perf_metrics";

/* ============================================================================
 * Internal Data Structures
 * ============================================================================ */

/**
 * @brief Internal metric data for tracking
 */
typedef struct {
    uint32_t count;             /**< Number of samples */
    uint64_t sum;               /**< Sum of all values */
    uint32_t min;               /**< Minimum value */
    uint32_t max;               /**< Maximum value */
    uint32_t last;              /**< Last recorded value */

    /* Rolling window for rate calculation */
    uint32_t window_counts[PERF_METRICS_WINDOW_SIZE];
    uint32_t window_times[PERF_METRICS_WINDOW_SIZE];
    uint8_t window_index;
    uint32_t last_rate_time_ms;
} metric_data_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Module initialization state */
static bool s_initialized = false;

/** @brief Periodic publishing active state */
static bool s_running = false;

/** @brief Timer handle for periodic publishing */
static esp_timer_handle_t s_timer_handle = NULL;

/** @brief Mutex for thread-safe access */
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Metric data array */
static metric_data_t s_metrics[PERF_METRIC_COUNT];

/** @brief Module initialization timestamp (microseconds) */
static int64_t s_init_time_us = 0;

/** @brief MQTT connection timestamp (microseconds) */
static int64_t s_mqtt_connect_time_us = 0;

/** @brief MQTT connected state */
static bool s_mqtt_connected = false;

/** @brief Last rate calculation timestamp */
static uint32_t s_last_rate_calc_ms = 0;

/* ============================================================================
 * Metric Name Strings
 * ============================================================================ */

static const char *s_metric_names[PERF_METRIC_COUNT] = {
    [PERF_METRIC_EVENT_DISPATCHED]       = "event_dispatched",
    [PERF_METRIC_EVENT_DISPATCH_TIME_US] = "event_dispatch_time_us",
    [PERF_METRIC_EVENT_QUEUE_DEPTH]      = "event_queue_depth",
    [PERF_METRIC_MQTT_PUBLISHED]         = "mqtt_published",
    [PERF_METRIC_MQTT_PUBLISH_LATENCY_US]= "mqtt_publish_latency_us",
    [PERF_METRIC_MQTT_RECEIVED]          = "mqtt_received",
    [PERF_METRIC_ZB_MSG_SENT]            = "zb_msg_sent",
    [PERF_METRIC_ZB_MSG_RECEIVED]        = "zb_msg_received",
    [PERF_METRIC_ZB_RESPONSE_TIME_US]    = "zb_response_time_us",
    [PERF_METRIC_ZB_NETWORK_QUALITY]     = "zb_network_quality",
    [PERF_METRIC_SYSTEM_UPTIME_SEC]      = "system_uptime_sec",
    [PERF_METRIC_CPU_USAGE_PERCENT]      = "cpu_usage_percent",
    [PERF_METRIC_FREE_HEAP]              = "free_heap",
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void timer_callback(void *arg);
static void update_rate_calculations(void);
static uint32_t get_timestamp_ms(void);
static uint32_t calculate_rate(metric_data_t *m);
static cJSON *build_metrics_json(void);
static void collect_system_metrics(perf_metrics_snapshot_t *snapshot);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t perf_metrics_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing performance metrics module...");

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create periodic timer */
    esp_timer_create_args_t timer_args = {
        .callback = timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "perf_metrics_timer",
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ret;
    }

    /* Initialize metric data */
    memset(s_metrics, 0, sizeof(s_metrics));
    for (int i = 0; i < PERF_METRIC_COUNT; i++) {
        s_metrics[i].min = UINT32_MAX;
    }

    /* Record initialization time */
    s_init_time_us = esp_timer_get_time();
    s_last_rate_calc_ms = get_timestamp_ms();

    s_initialized = true;
    s_running = false;
    s_mqtt_connected = false;

    ESP_LOGI(TAG, "Performance metrics initialized");
    return ESP_OK;
}

esp_err_t perf_metrics_record(perf_metric_t metric, uint32_t value)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (metric >= PERF_METRIC_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Try to take mutex with short timeout for non-blocking behavior */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        /* Could not acquire mutex - skip this sample rather than blocking */
        return ESP_OK;
    }

    metric_data_t *m = &s_metrics[metric];

    /* Update statistics */
    m->count++;
    m->sum += value;
    m->last = value;

    if (value < m->min) {
        m->min = value;
    }
    if (value > m->max) {
        m->max = value;
    }

    /* Update rolling window for rate metrics */
    uint32_t now_ms = get_timestamp_ms();
    m->window_counts[m->window_index] = m->count;
    m->window_times[m->window_index] = now_ms;

    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t perf_metrics_get(perf_metric_t metric, perf_metric_stats_t *stats)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (metric >= PERF_METRIC_COUNT || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    metric_data_t *m = &s_metrics[metric];

    stats->count = m->count;
    stats->sum = m->sum;
    stats->min = (m->min == UINT32_MAX) ? 0 : m->min;
    stats->max = m->max;
    stats->last = m->last;
    stats->avg = (m->count > 0) ? (uint32_t)(m->sum / m->count) : 0;

    /* Calculate rate from rolling window */
    if (m->count >= 2 && m->window_index > 0) {
        uint8_t oldest_idx = (m->window_index + 1) % PERF_METRICS_WINDOW_SIZE;
        uint32_t oldest_count = m->window_counts[oldest_idx];
        uint32_t oldest_time = m->window_times[oldest_idx];
        uint32_t newest_count = m->window_counts[m->window_index];
        uint32_t newest_time = m->window_times[m->window_index];

        if (newest_time > oldest_time && oldest_time > 0) {
            uint32_t delta_count = newest_count - oldest_count;
            uint32_t delta_time_ms = newest_time - oldest_time;
            stats->rate_per_sec = (delta_count * 1000) / delta_time_ms;
        } else {
            stats->rate_per_sec = 0;
        }
    } else {
        stats->rate_per_sec = 0;
    }

    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t perf_metrics_publish_mqtt(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    REQUIRE_MQTT_CONNECTED(ESP_ERR_INVALID_STATE);

    /* Update rate calculations before publishing */
    update_rate_calculations();

    /* Build JSON payload */
    cJSON *json = build_metrics_json();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to build JSON payload");
        return ESP_ERR_NO_MEM;
    }

    /* Convert JSON to string */
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return ESP_ERR_NO_MEM;
    }

    /* Ownership transfer: strdup topic (constant), json_str (heap) transfers directly */
    char *topic_heap = strdup(PERF_METRICS_TOPIC);
    if (topic_heap == NULL) {
        free(json_str);
        ESP_LOGE(TAG, "Failed to strdup topic");
        return ESP_ERR_NO_MEM;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_heap,
        .payload_json = json_str,
        .qos = 0,
        .retain = false
    };
    esp_err_t ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret == ESP_OK) {
        /* Ownership transferred to handler - do NOT free */
        ESP_LOGD(TAG, "Published performance metrics");
    } else {
        /* event_publish failed - ownership NOT transferred, free here */
        free(topic_heap);
        free(json_str);
        ESP_LOGW(TAG, "Failed to publish metrics: %s", esp_err_to_name(ret));
    }

    return ret;
}

void perf_metrics_reset(void)
{
    if (!s_initialized) {
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for reset");
        return;
    }

    /* Reset all metrics */
    memset(s_metrics, 0, sizeof(s_metrics));
    for (int i = 0; i < PERF_METRIC_COUNT; i++) {
        s_metrics[i].min = UINT32_MAX;
    }

    /* Reset timing */
    s_init_time_us = esp_timer_get_time();
    s_last_rate_calc_ms = get_timestamp_ms();

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Performance metrics reset");
}

esp_err_t perf_metrics_start(uint32_t interval_ms)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        ESP_LOGW(TAG, "Already running, stopping first");
        perf_metrics_stop();
    }

    /* Clamp interval to valid range */
    if (interval_ms < PERF_METRICS_MIN_INTERVAL_MS) {
        ESP_LOGW(TAG, "Interval %lu ms too small, using minimum %d ms",
                 (unsigned long)interval_ms, PERF_METRICS_MIN_INTERVAL_MS);
        interval_ms = PERF_METRICS_MIN_INTERVAL_MS;
    } else if (interval_ms > PERF_METRICS_MAX_INTERVAL_MS) {
        ESP_LOGW(TAG, "Interval %lu ms too large, using maximum %d ms",
                 (unsigned long)interval_ms, PERF_METRICS_MAX_INTERVAL_MS);
        interval_ms = PERF_METRICS_MAX_INTERVAL_MS;
    }

    /* Start periodic timer */
    esp_err_t ret = esp_timer_start_periodic(s_timer_handle, interval_ms * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = true;
    ESP_LOGI(TAG, "Performance metrics publishing started (interval: %lu ms)",
             (unsigned long)interval_ms);

    /* Publish initial metrics */
    perf_metrics_publish_mqtt();

    return ESP_OK;
}

esp_err_t perf_metrics_stop(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_running) {
        ESP_LOGW(TAG, "Not running");
        return ESP_OK;
    }

    esp_err_t ret = esp_timer_stop(s_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop timer: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = false;
    ESP_LOGI(TAG, "Performance metrics publishing stopped");
    return ESP_OK;
}

esp_err_t perf_metrics_get_snapshot(perf_metrics_snapshot_t *snapshot)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memset(snapshot, 0, sizeof(perf_metrics_snapshot_t));

    /* Timestamps */
    snapshot->timestamp_ms = get_timestamp_ms();
    snapshot->uptime_sec = (uint32_t)((esp_timer_get_time() - s_init_time_us) / 1000000);

    /* Event Bus Metrics */
    metric_data_t *m = &s_metrics[PERF_METRIC_EVENT_DISPATCHED];
    snapshot->event_bus.total_events = m->count;
    snapshot->event_bus.events_per_sec = (m->count > 0 && m->window_index > 0) ?
        calculate_rate(m) : 0;

    m = &s_metrics[PERF_METRIC_EVENT_DISPATCH_TIME_US];
    snapshot->event_bus.avg_dispatch_time_us = (m->count > 0) ?
        (uint32_t)(m->sum / m->count) : 0;

    m = &s_metrics[PERF_METRIC_EVENT_QUEUE_DEPTH];
    snapshot->event_bus.queue_depth = m->last;

    /* MQTT Metrics */
    m = &s_metrics[PERF_METRIC_MQTT_PUBLISHED];
    snapshot->mqtt.total_published = m->count;
    snapshot->mqtt.messages_per_sec = (m->count > 0 && m->window_index > 0) ?
        calculate_rate(m) : 0;

    m = &s_metrics[PERF_METRIC_MQTT_PUBLISH_LATENCY_US];
    snapshot->mqtt.avg_latency_us = (m->count > 0) ?
        (uint32_t)(m->sum / m->count) : 0;

    m = &s_metrics[PERF_METRIC_MQTT_RECEIVED];
    snapshot->mqtt.total_received = m->count;

    /* MQTT connection uptime */
    if (s_mqtt_connected && s_mqtt_connect_time_us > 0) {
        snapshot->mqtt.connection_uptime_sec =
            (uint32_t)((esp_timer_get_time() - s_mqtt_connect_time_us) / 1000000);
    }

    /* Zigbee Metrics */
    m = &s_metrics[PERF_METRIC_ZB_MSG_SENT];
    snapshot->zigbee.total_sent = m->count;

    m = &s_metrics[PERF_METRIC_ZB_MSG_RECEIVED];
    snapshot->zigbee.total_received = m->count;
    snapshot->zigbee.messages_per_sec = (m->count > 0 && m->window_index > 0) ?
        calculate_rate(m) : 0;

    m = &s_metrics[PERF_METRIC_ZB_RESPONSE_TIME_US];
    snapshot->zigbee.avg_response_time_us = (m->count > 0) ?
        (uint32_t)(m->sum / m->count) : 0;

    m = &s_metrics[PERF_METRIC_ZB_NETWORK_QUALITY];
    snapshot->zigbee.network_quality = m->last;

    xSemaphoreGive(s_mutex);

    /* Collect system metrics (doesn't need the metrics mutex) */
    collect_system_metrics(snapshot);

    return ESP_OK;
}

void perf_metrics_log_summary(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return;
    }

    perf_metrics_snapshot_t snapshot;
    if (perf_metrics_get_snapshot(&snapshot) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get metrics snapshot");
        return;
    }

    ESP_LOGI(TAG, "=== Performance Metrics Summary ===");
    ESP_LOGI(TAG, "Uptime: %lu sec", (unsigned long)snapshot.uptime_sec);
    ESP_LOGI(TAG, "");

    ESP_LOGI(TAG, "Event Bus:");
    ESP_LOGI(TAG, "  Events/sec: %lu, Avg dispatch: %lu us, Queue depth: %lu",
             (unsigned long)snapshot.event_bus.events_per_sec,
             (unsigned long)snapshot.event_bus.avg_dispatch_time_us,
             (unsigned long)snapshot.event_bus.queue_depth);
    ESP_LOGI(TAG, "  Total events: %lu",
             (unsigned long)snapshot.event_bus.total_events);
    ESP_LOGI(TAG, "");

    ESP_LOGI(TAG, "MQTT:");
    ESP_LOGI(TAG, "  Msg/sec: %lu, Avg latency: %lu us, Connection uptime: %lu sec",
             (unsigned long)snapshot.mqtt.messages_per_sec,
             (unsigned long)snapshot.mqtt.avg_latency_us,
             (unsigned long)snapshot.mqtt.connection_uptime_sec);
    ESP_LOGI(TAG, "  Published: %lu, Received: %lu",
             (unsigned long)snapshot.mqtt.total_published,
             (unsigned long)snapshot.mqtt.total_received);
    ESP_LOGI(TAG, "");

    ESP_LOGI(TAG, "Zigbee:");
    ESP_LOGI(TAG, "  Msg/sec: %lu, Avg response: %lu us, Quality: %lu%%",
             (unsigned long)snapshot.zigbee.messages_per_sec,
             (unsigned long)snapshot.zigbee.avg_response_time_us,
             (unsigned long)snapshot.zigbee.network_quality);
    ESP_LOGI(TAG, "  Sent: %lu, Received: %lu",
             (unsigned long)snapshot.zigbee.total_sent,
             (unsigned long)snapshot.zigbee.total_received);
    ESP_LOGI(TAG, "");

    ESP_LOGI(TAG, "System:");
    ESP_LOGI(TAG, "  CPU: %lu%%, Free heap: %lu, Min heap: %lu, Tasks: %u",
             (unsigned long)snapshot.system.cpu_usage_percent,
             (unsigned long)snapshot.system.free_heap,
             (unsigned long)snapshot.system.min_free_heap,
             snapshot.system.task_count);

    ESP_LOGI(TAG, "=== End Metrics Summary ===");
}

bool perf_metrics_is_initialized(void)
{
    return s_initialized;
}

bool perf_metrics_is_running(void)
{
    return s_running;
}

esp_err_t perf_metrics_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop if running */
    if (s_running) {
        perf_metrics_stop();
    }

    /* Delete timer */
    if (s_timer_handle != NULL) {
        esp_timer_delete(s_timer_handle);
        s_timer_handle = NULL;
    }

    /* Delete mutex */
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Performance metrics deinitialized");
    return ESP_OK;
}

const char *perf_metric_to_str(perf_metric_t metric)
{
    if (metric < PERF_METRIC_COUNT) {
        return s_metric_names[metric];
    }
    return "UNKNOWN";
}

void perf_metrics_mqtt_connected(void)
{
    if (!s_initialized) {
        return;
    }

    s_mqtt_connect_time_us = esp_timer_get_time();
    s_mqtt_connected = true;
    ESP_LOGD(TAG, "MQTT connected, tracking uptime");
}

void perf_metrics_mqtt_disconnected(void)
{
    if (!s_initialized) {
        return;
    }

    s_mqtt_connected = false;
    ESP_LOGD(TAG, "MQTT disconnected");
}

/* ============================================================================
 * Private Functions
 * ============================================================================ */

/**
 * @brief Timer callback for periodic publishing
 */
static void timer_callback(void *arg)
{
    (void)arg;
    perf_metrics_publish_mqtt();
}

/**
 * @brief Get current timestamp in milliseconds (since boot)
 */
static uint32_t get_timestamp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * @brief Calculate rate from rolling window
 */
static uint32_t calculate_rate(metric_data_t *m)
{
    if (m->window_index == 0) {
        return 0;
    }

    /* Find oldest valid entry in window */
    uint8_t oldest_idx = 0;
    uint32_t oldest_time = UINT32_MAX;

    for (int i = 0; i < PERF_METRICS_WINDOW_SIZE; i++) {
        if (m->window_times[i] > 0 && m->window_times[i] < oldest_time) {
            oldest_time = m->window_times[i];
            oldest_idx = i;
        }
    }

    uint32_t newest_count = m->window_counts[m->window_index];
    uint32_t newest_time = m->window_times[m->window_index];
    uint32_t oldest_count = m->window_counts[oldest_idx];

    if (newest_time > oldest_time && oldest_time > 0) {
        uint32_t delta_count = newest_count - oldest_count;
        uint32_t delta_time_ms = newest_time - oldest_time;
        if (delta_time_ms > 0) {
            return (delta_count * 1000) / delta_time_ms;
        }
    }

    return 0;
}

/**
 * @brief Update rate calculations for all metrics
 */
static void update_rate_calculations(void)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    uint32_t now_ms = get_timestamp_ms();

    /* Update window index for all metrics */
    for (int i = 0; i < PERF_METRIC_COUNT; i++) {
        metric_data_t *m = &s_metrics[i];

        /* Advance window index */
        m->window_index = (m->window_index + 1) % PERF_METRICS_WINDOW_SIZE;
        m->window_counts[m->window_index] = m->count;
        m->window_times[m->window_index] = now_ms;
    }

    s_last_rate_calc_ms = now_ms;

    xSemaphoreGive(s_mutex);
}

/**
 * @brief Collect system metrics (heap, CPU, tasks)
 */
static void collect_system_metrics(perf_metrics_snapshot_t *snapshot)
{
    /* Heap statistics */
    snapshot->system.free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snapshot->system.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    snapshot->system.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    /* CPU usage from task manager */
    snapshot->system.cpu_usage_percent = task_manager_get_cpu_usage();

    /* Task information */
    task_info_t tasks[PERF_METRICS_MAX_TASKS];
    size_t task_count = task_manager_get_all_tasks(tasks, PERF_METRICS_MAX_TASKS);
    snapshot->system.task_count = (uint8_t)task_count;

    for (size_t i = 0; i < task_count && i < PERF_METRICS_MAX_TASKS; i++) {
        strncpy(snapshot->system.tasks[i].name, tasks[i].name,
                sizeof(snapshot->system.tasks[i].name) - 1);
        snapshot->system.tasks[i].name[sizeof(snapshot->system.tasks[i].name) - 1] = '\0';
        snapshot->system.tasks[i].high_watermark = tasks[i].stack_high_watermark;
        snapshot->system.tasks[i].usage_percent = tasks[i].stack_usage_percent;
    }
}

/**
 * @brief Build JSON object with all metrics
 */
static cJSON *build_metrics_json(void)
{
    perf_metrics_snapshot_t snapshot;
    if (perf_metrics_get_snapshot(&snapshot) != ESP_OK) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    /* Timestamps */
    cJSON_AddNumberToObject(root, "timestamp_ms", snapshot.timestamp_ms);
    cJSON_AddNumberToObject(root, "uptime_sec", snapshot.uptime_sec);

    /* Event Bus */
    cJSON *event_bus = cJSON_CreateObject();
    if (event_bus != NULL) {
        cJSON_AddNumberToObject(event_bus, "events_per_sec",
                                snapshot.event_bus.events_per_sec);
        cJSON_AddNumberToObject(event_bus, "avg_dispatch_time_us",
                                snapshot.event_bus.avg_dispatch_time_us);
        cJSON_AddNumberToObject(event_bus, "queue_depth",
                                snapshot.event_bus.queue_depth);
        cJSON_AddNumberToObject(event_bus, "total_events",
                                snapshot.event_bus.total_events);
        cJSON_AddItemToObject(root, "event_bus", event_bus);
    }

    /* MQTT */
    cJSON *mqtt = cJSON_CreateObject();
    if (mqtt != NULL) {
        cJSON_AddNumberToObject(mqtt, "messages_per_sec",
                                snapshot.mqtt.messages_per_sec);
        cJSON_AddNumberToObject(mqtt, "avg_latency_us",
                                snapshot.mqtt.avg_latency_us);
        cJSON_AddNumberToObject(mqtt, "connection_uptime_sec",
                                snapshot.mqtt.connection_uptime_sec);
        cJSON_AddNumberToObject(mqtt, "total_published",
                                snapshot.mqtt.total_published);
        cJSON_AddNumberToObject(mqtt, "total_received",
                                snapshot.mqtt.total_received);
        cJSON_AddItemToObject(root, "mqtt", mqtt);
    }

    /* Zigbee */
    cJSON *zigbee = cJSON_CreateObject();
    if (zigbee != NULL) {
        cJSON_AddNumberToObject(zigbee, "messages_per_sec",
                                snapshot.zigbee.messages_per_sec);
        cJSON_AddNumberToObject(zigbee, "avg_response_time_us",
                                snapshot.zigbee.avg_response_time_us);
        cJSON_AddNumberToObject(zigbee, "network_quality",
                                snapshot.zigbee.network_quality);
        cJSON_AddNumberToObject(zigbee, "total_sent",
                                snapshot.zigbee.total_sent);
        cJSON_AddNumberToObject(zigbee, "total_received",
                                snapshot.zigbee.total_received);
        cJSON_AddItemToObject(root, "zigbee", zigbee);
    }

    /* System */
    cJSON *system_obj = cJSON_CreateObject();
    if (system_obj != NULL) {
        cJSON_AddNumberToObject(system_obj, "cpu_usage_percent",
                                snapshot.system.cpu_usage_percent);
        cJSON_AddNumberToObject(system_obj, "free_heap",
                                snapshot.system.free_heap);
        cJSON_AddNumberToObject(system_obj, "min_free_heap",
                                snapshot.system.min_free_heap);
        cJSON_AddNumberToObject(system_obj, "free_psram",
                                snapshot.system.free_psram);
        cJSON_AddNumberToObject(system_obj, "task_count",
                                snapshot.system.task_count);

        /* Task stack info */
        cJSON *tasks_array = cJSON_CreateArray();
        if (tasks_array != NULL) {
            for (size_t i = 0; i < snapshot.system.task_count &&
                              i < PERF_METRICS_MAX_TASKS; i++) {
                cJSON *task = cJSON_CreateObject();
                if (task != NULL) {
                    cJSON_AddStringToObject(task, "name",
                                            snapshot.system.tasks[i].name);
                    cJSON_AddNumberToObject(task, "stack_hwm",
                                            snapshot.system.tasks[i].high_watermark);
                    cJSON_AddNumberToObject(task, "stack_usage_pct",
                                            snapshot.system.tasks[i].usage_percent);
                    cJSON_AddItemToArray(tasks_array, task);
                }
            }
            cJSON_AddItemToObject(system_obj, "tasks", tasks_array);
        }

        cJSON_AddItemToObject(root, "system", system_obj);
    }

    return root;
}
