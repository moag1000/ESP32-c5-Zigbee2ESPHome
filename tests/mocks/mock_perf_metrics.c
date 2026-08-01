/**
 * @file mock_perf_metrics.c
 * @brief Stub for the single perf_metrics symbol event_bus.c needs
 *
 * event_bus.c calls perf_metrics_record() and nothing else. Linking the real
 * perf_metrics.c would drag in task_manager, gateway_mqtt and mqtt_helpers —
 * the entire MQTT stack — just to satisfy one call that records a number.
 *
 * Recorded values are kept so a test can assert on them if it ever needs to.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "core/monitoring/perf_metrics.h"

static uint32_t s_record_calls = 0;

esp_err_t perf_metrics_record(perf_metric_t metric, uint32_t value)
{
    (void)metric;
    (void)value;
    s_record_calls++;
    return ESP_OK;
}

/** @brief How often perf_metrics_record() was called since boot */
uint32_t mock_perf_metrics_record_count(void)
{
    return s_record_calls;
}
