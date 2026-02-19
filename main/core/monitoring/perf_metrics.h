/**
 * @file perf_metrics.h
 * @brief Performance Metrics Module - Track and Report System Performance
 *
 * Tracks performance metrics for monitoring and debugging including:
 * - Event bus: events/sec, avg dispatch time, queue depth
 * - MQTT: messages/sec, avg publish latency, connection uptime
 * - Zigbee: messages/sec, avg response time, network quality
 * - System: uptime, task stack high water marks, CPU usage estimate
 *
 * Supports periodic MQTT publishing and on-demand retrieval.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef PERF_METRICS_H
#define PERF_METRICS_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

/** @brief MQTT topic for performance metrics */
#define PERF_METRICS_TOPIC              "zigbee2mqtt/bridge/metrics"

/** @brief Default publish interval in milliseconds (60 seconds) */
#define PERF_METRICS_DEFAULT_INTERVAL_MS    (60 * 1000)

/** @brief Minimum publish interval in milliseconds (10 seconds) */
#define PERF_METRICS_MIN_INTERVAL_MS        (10 * 1000)

/** @brief Maximum publish interval in milliseconds (10 minutes) */
#define PERF_METRICS_MAX_INTERVAL_MS        (10 * 60 * 1000)

/** @brief Rolling window size for rate calculations (samples) */
#define PERF_METRICS_WINDOW_SIZE            10

/** @brief Maximum number of tasks to track for stack monitoring */
#define PERF_METRICS_MAX_TASKS              16

/* ============================================================================
 * Metric Types
 * ============================================================================ */

/**
 * @brief Performance metric types
 *
 * Identifies different categories of performance metrics that can be recorded.
 */
typedef enum {
    /* Event Bus Metrics */
    PERF_METRIC_EVENT_DISPATCHED,       /**< Event was dispatched */
    PERF_METRIC_EVENT_DISPATCH_TIME_US, /**< Time to dispatch event (microseconds) */
    PERF_METRIC_EVENT_QUEUE_DEPTH,      /**< Current event queue depth */

    /* MQTT Metrics */
    PERF_METRIC_MQTT_PUBLISHED,         /**< MQTT message published */
    PERF_METRIC_MQTT_PUBLISH_LATENCY_US,/**< Publish latency (microseconds) */
    PERF_METRIC_MQTT_RECEIVED,          /**< MQTT message received */

    /* Zigbee Metrics */
    PERF_METRIC_ZB_MSG_SENT,            /**< Zigbee message sent */
    PERF_METRIC_ZB_MSG_RECEIVED,        /**< Zigbee message received */
    PERF_METRIC_ZB_RESPONSE_TIME_US,    /**< Zigbee response time (microseconds) */
    PERF_METRIC_ZB_NETWORK_QUALITY,     /**< Network quality indicator (0-100) */

    /* System Metrics (recorded automatically) */
    PERF_METRIC_SYSTEM_UPTIME_SEC,      /**< System uptime in seconds */
    PERF_METRIC_CPU_USAGE_PERCENT,      /**< CPU usage percentage */
    PERF_METRIC_FREE_HEAP,              /**< Free heap bytes */

    PERF_METRIC_COUNT                   /**< Number of metric types (must be last) */
} perf_metric_t;

/* ============================================================================
 * Statistics Structures
 * ============================================================================ */

/**
 * @brief Statistics for a single metric
 *
 * Contains aggregated statistics including count, sum, min, max, and average.
 */
typedef struct {
    uint32_t count;             /**< Number of samples recorded */
    uint64_t sum;               /**< Sum of all values */
    uint32_t min;               /**< Minimum value recorded */
    uint32_t max;               /**< Maximum value recorded */
    uint32_t last;              /**< Most recent value */
    uint32_t rate_per_sec;      /**< Calculated rate per second (for count metrics) */
    uint32_t avg;               /**< Calculated average value */
} perf_metric_stats_t;

/**
 * @brief Task stack monitoring entry
 */
typedef struct {
    char name[16];              /**< Task name */
    uint32_t high_watermark;    /**< Stack high water mark (bytes) */
    uint8_t usage_percent;      /**< Estimated stack usage percentage */
} perf_task_stack_t;

/**
 * @brief Complete performance metrics snapshot
 *
 * Contains all metrics at a point in time for MQTT publishing.
 */
typedef struct {
    /* Timestamps */
    uint32_t timestamp_ms;              /**< Snapshot timestamp (uptime ms) */
    uint32_t uptime_sec;                /**< System uptime in seconds */

    /* Event Bus Metrics */
    struct {
        uint32_t events_per_sec;        /**< Events dispatched per second */
        uint32_t avg_dispatch_time_us;  /**< Average dispatch time (us) */
        uint32_t queue_depth;           /**< Current queue depth */
        uint32_t total_events;          /**< Total events since init */
    } event_bus;

    /* MQTT Metrics */
    struct {
        uint32_t messages_per_sec;      /**< Messages published per second */
        uint32_t avg_latency_us;        /**< Average publish latency (us) */
        uint32_t connection_uptime_sec; /**< Connection uptime (seconds) */
        uint32_t total_published;       /**< Total messages published */
        uint32_t total_received;        /**< Total messages received */
    } mqtt;

    /* Zigbee Metrics */
    struct {
        uint32_t messages_per_sec;      /**< Messages per second */
        uint32_t avg_response_time_us;  /**< Average response time (us) */
        uint32_t network_quality;       /**< Network quality (0-100) */
        uint32_t total_sent;            /**< Total messages sent */
        uint32_t total_received;        /**< Total messages received */
    } zigbee;

    /* System Metrics */
    struct {
        uint32_t cpu_usage_percent;     /**< CPU usage percentage */
        uint32_t free_heap;             /**< Free internal heap */
        uint32_t min_free_heap;         /**< Minimum free heap ever */
        uint32_t free_psram;            /**< Free PSRAM */
        uint8_t task_count;             /**< Number of tasks */
        perf_task_stack_t tasks[PERF_METRICS_MAX_TASKS]; /**< Task stack info */
    } system;
} perf_metrics_snapshot_t;

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Initialize the performance metrics module
 *
 * Allocates resources and initializes all metric counters.
 * Does not start periodic publishing.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if already initialized
 *     - ESP_ERR_NO_MEM if memory allocation failed
 */
esp_err_t perf_metrics_init(void);

/**
 * @brief Record a performance metric value
 *
 * Records a value for the specified metric. For count-based metrics (like
 * PERF_METRIC_EVENT_DISPATCHED), call with value=1 for each occurrence.
 * For measurement metrics (like PERF_METRIC_EVENT_DISPATCH_TIME_US), call
 * with the actual measured value.
 *
 * Thread-safe: Can be called from any task or ISR context.
 *
 * @param[in] metric Metric type to record
 * @param[in] value Value to record
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized
 *     - ESP_ERR_INVALID_ARG if metric type is invalid
 */
esp_err_t perf_metrics_record(perf_metric_t metric, uint32_t value);

/**
 * @brief Get statistics for a specific metric
 *
 * Retrieves aggregated statistics for the specified metric type.
 *
 * @param[in] metric Metric type to query
 * @param[out] stats Pointer to statistics structure to fill
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized
 *     - ESP_ERR_INVALID_ARG if metric is invalid or stats is NULL
 */
esp_err_t perf_metrics_get(perf_metric_t metric, perf_metric_stats_t *stats);

/**
 * @brief Publish all metrics to MQTT
 *
 * Collects all metrics, formats them as JSON, and publishes to
 * PERF_METRICS_TOPIC. Updates rate calculations before publishing.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized or MQTT not connected
 *     - ESP_ERR_NO_MEM if JSON creation failed
 *     - ESP_FAIL if MQTT publish failed
 */
esp_err_t perf_metrics_publish_mqtt(void);

/**
 * @brief Reset all performance metrics
 *
 * Clears all metric counters and statistics. Useful after configuration
 * changes or for starting fresh measurement periods.
 */
void perf_metrics_reset(void);

/**
 * @brief Start periodic MQTT publishing
 *
 * Starts a timer to periodically publish metrics to MQTT.
 *
 * @param[in] interval_ms Publishing interval in milliseconds
 *                        (clamped to MIN/MAX range)
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t perf_metrics_start(uint32_t interval_ms);

/**
 * @brief Stop periodic MQTT publishing
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized or not running
 */
esp_err_t perf_metrics_stop(void);

/**
 * @brief Get a complete metrics snapshot
 *
 * Fills the provided structure with current values for all metrics.
 * Useful for custom processing or display.
 *
 * @param[out] snapshot Pointer to snapshot structure to fill
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized
 *     - ESP_ERR_INVALID_ARG if snapshot is NULL
 */
esp_err_t perf_metrics_get_snapshot(perf_metrics_snapshot_t *snapshot);

/**
 * @brief Log metrics summary to console
 *
 * Outputs a formatted summary of all metrics to the ESP log.
 */
void perf_metrics_log_summary(void);

/**
 * @brief Check if performance metrics module is initialized
 *
 * @return true if initialized, false otherwise
 */
bool perf_metrics_is_initialized(void);

/**
 * @brief Check if periodic publishing is active
 *
 * @return true if timer is running, false otherwise
 */
bool perf_metrics_is_running(void);

/**
 * @brief Deinitialize the performance metrics module
 *
 * Stops publishing, frees resources, and resets state.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t perf_metrics_deinit(void);

/**
 * @brief Get the name of a metric type
 *
 * @param[in] metric Metric type
 *
 * @return String name of the metric, or "UNKNOWN" if invalid
 */
const char *perf_metric_to_str(perf_metric_t metric);

/**
 * @brief Record MQTT connection start time
 *
 * Should be called when MQTT connects to track connection uptime.
 */
void perf_metrics_mqtt_connected(void);

/**
 * @brief Record MQTT disconnection
 *
 * Should be called when MQTT disconnects.
 */
void perf_metrics_mqtt_disconnected(void);

#ifdef __cplusplus
}
#endif

#endif /* PERF_METRICS_H */
