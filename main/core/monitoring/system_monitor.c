/**
 * @file system_monitor.c
 * @brief System Monitor Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "system_monitor.h"
#include "memory_manager.h"
#include "task_manager.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "utils/freertos_helpers.h"  /* For PSRAM task creation */
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"

/* Include WiFi and MQTT headers for status checking */
#include "wifi/wifi_manager.h"
#include "gateway_mqtt.h"
#include "mqtt/mqtt_topics.h"
#include "gateway_defaults.h"
#include "ha_bridge_discovery.h"
#include "zigbee/zb_cmd_retry.h"

/* LED status integration */
#if CONFIG_GW_LED_ENABLED
#include "core/led_status_manager.h"
#endif

#if CONFIG_BT_SCANNER_ENABLED
#include "bluetooth/ble_manager.h"
#include "bluetooth/ble_scanner.h"
#endif
#if CONFIG_ESPHOME_API_ENABLE
#include "esphome/esphome_api.h"
#include "esphome/esphome_ble_proxy.h"
#endif

static const char *TAG = "SYS_MON";

/* Monitoring task handle (PSRAM-backed for memory optimization) */
static psram_task_handle_t s_monitor_task = {0};
static bool s_monitor_running = false;
static uint32_t s_monitor_interval_sec = SYSMON_INTERVAL_DEFAULT_SEC;

/* MQTT publishing enabled flag */
static bool s_mqtt_publish_enabled = false;

/* Boot time reference */
static int64_t s_boot_time_us = 0;

/* Health status cache */
static system_health_t s_cached_health = SYSTEM_HEALTH_GOOD;

/* Track if we're subscribed to the task watchdog */
static bool s_wdt_subscribed = false;

/* Forward declarations */
void system_monitor_log_compact(void);

/**
 * @brief Calculate system health based on current conditions
 */
static system_health_t calculate_system_health(void)
{
    memory_status_t mem_status = memory_manager_get_status();
    bool stack_warning = task_manager_check_stack_usage(TASK_STACK_WARNING_THRESHOLD);

    /* Critical conditions */
    if (mem_status == MEMORY_STATUS_CRITICAL) {
        return SYSTEM_HEALTH_CRITICAL;
    }

    /* Warning conditions */
    if (mem_status == MEMORY_STATUS_WARNING || stack_warning) {
        return SYSTEM_HEALTH_WARNING;
    }

    return SYSTEM_HEALTH_GOOD;
}

/* Counter for comprehensive logging (every N intervals) */
static uint32_t s_log_counter = 0;
#define COMPREHENSIVE_LOG_INTERVAL 5  /* Full stats every 5th iteration */

/**
 * @brief System monitoring task
 *
 * Uses ulTaskNotifyTake() instead of vTaskDelayUntil() to allow graceful
 * shutdown. The stop function sends a notification to wake the task
 * immediately instead of waiting for the full interval.
 */
static void system_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System monitoring task started (interval: %lu sec, PSRAM stack)",
             (unsigned long)s_monitor_interval_sec);

    const TickType_t interval_ticks = pdMS_TO_TICKS(s_monitor_interval_sec * 1000);

    while (s_monitor_running) {
        /* Feed watchdog */
        system_monitor_feed_watchdog();

        s_log_counter++;

        /* Log compact summary every interval, comprehensive every N intervals */
        if (s_log_counter % COMPREHENSIVE_LOG_INTERVAL == 0) {
            system_monitor_log_all();
        } else {
            system_monitor_log_compact();
        }

        /* Check health */
        system_health_t prev_health = s_cached_health;
        s_cached_health = calculate_system_health();

        /* Publish memory events on health state changes */
        if (s_cached_health != prev_health) {
            memory_stats_t mem_stats = memory_manager_get_stats();

            if (s_cached_health == SYSTEM_HEALTH_CRITICAL) {
                /* Publish critical memory event */
                evt_memory_pressure_t evt = {
                    .level = 2, /* MEMORY_PRESSURE_CRITICAL */
                    .free_heap = mem_stats.free_heap,
                };
                event_publish(EVT_SYSTEM_MEMORY_CRITICAL, &evt, sizeof(evt));
                ESP_LOGW(TAG, "Memory CRITICAL: Publishing EVT_SYSTEM_MEMORY_CRITICAL");
            } else if (s_cached_health == SYSTEM_HEALTH_WARNING) {
                /* Publish warning memory event */
                evt_memory_pressure_t evt = {
                    .level = 1, /* MEMORY_PRESSURE_WARNING */
                    .free_heap = mem_stats.free_heap,
                };
                event_publish(EVT_SYSTEM_MEMORY_WARNING, &evt, sizeof(evt));
                ESP_LOGD(TAG, "Memory WARNING: Publishing EVT_SYSTEM_MEMORY_WARNING");
            }
        }

#if CONFIG_GW_LED_ENABLED
        /* Update LED status if health changed */
        if (s_cached_health != prev_health) {
            led_status_manager_set_condition(LED_COND_SYSTEM_ERROR,
                                              s_cached_health == SYSTEM_HEALTH_CRITICAL);
            led_status_manager_set_condition(LED_COND_SYSTEM_WARNING,
                                              s_cached_health == SYSTEM_HEALTH_WARNING);
        }
#endif

        /* Check for timed-out pending ZCL commands and retry */
        cmd_retry_check_timeouts();

        /* Publish to MQTT if enabled */
        if (s_mqtt_publish_enabled) {
            /* Trigger MQTT latency measurement before publishing state */
            mqtt_latency_measure_start();

            system_monitor_publish_mqtt();

            /* Publish bridge state for HA sensors */
            ha_bridge_publish_state();
        }

        /* Wait for next interval OR notification to stop.
         * Using xTaskNotifyWait instead of vTaskDelayUntil allows the stop
         * function to wake us immediately by sending a notification.
         * v6.0 compatible: xTaskNotifyWait replaces deprecated ulTaskNotifyTake */
        uint32_t notification_value;
        xTaskNotifyWait(0, ULONG_MAX, &notification_value, interval_ticks);
    }

    ESP_LOGI(TAG, "System monitoring task stopped");
    /* Mark task as self-deleted - resources freed by stop function */
    psram_task_mark_deleted(&s_monitor_task);
    vTaskDelete(NULL);
}

/**
 * @brief Initialize system monitor
 */
esp_err_t system_monitor_init(bool enable_mqtt_publish)
{
    ESP_LOGI(TAG, "Initializing system monitor...");

    /* Store boot time */
    s_boot_time_us = esp_timer_get_time();

    /* Initialize memory manager */
    esp_err_t ret = memory_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize memory manager: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initialize task manager */
    ret = task_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize task manager: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set MQTT publishing preference */
    s_mqtt_publish_enabled = enable_mqtt_publish;

    ESP_LOGI(TAG, "System monitor initialized successfully");
    return ESP_OK;
}

/**
 * @brief Start system monitoring task
 */
esp_err_t system_monitor_start(uint32_t interval_sec)
{
    if (s_monitor_running) {
        ESP_LOGW(TAG, "System monitor already running");
        return ESP_OK;
    }

    s_monitor_interval_sec = interval_sec;
    s_monitor_running = true;

    /* Create monitoring task with PSRAM stack (saves ~6KB internal RAM) */
    esp_err_t ret = psram_task_create(
        system_monitor_task,
        "sys_monitor",
        SYSMON_TASK_STACK_SIZE,
        NULL,
        SYSMON_TASK_PRIORITY,
        &s_monitor_task
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create system monitoring task: %s", esp_err_to_name(ret));
        s_monitor_running = false;
        return ret;
    }

    ESP_LOGI(TAG, "System monitoring task started (interval: %lu sec, PSRAM stack)", interval_sec);
    return ESP_OK;
}

/**
 * @brief Stop system monitoring task
 */
esp_err_t system_monitor_stop(void)
{
    if (!s_monitor_running) {
        ESP_LOGW(TAG, "System monitor not running");
        return ESP_OK;
    }

    /* Disable MQTT publishing first to prevent blocking on network I/O
     * during the stop sequence. This ensures the monitor task can exit
     * gracefully without holding the MQTT publish mutex. */
    s_mqtt_publish_enabled = false;

    s_monitor_running = false;

    /* Send notification to wake up the task immediately.
     * The task uses ulTaskNotifyTake() which will return immediately
     * when notified, allowing it to check s_monitor_running and exit. */
    if (psram_task_is_valid(&s_monitor_task)) {
        xTaskNotifyGive(s_monitor_task.task_handle);
    }

    /* Wait for task to finish gracefully */
    uint32_t timeout = SYSMON_TASK_STOP_TIMEOUT_MS;
    while (psram_task_is_valid(&s_monitor_task) && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(GW_DEFAULT_TASK_STOP_POLL_MS));
        timeout -= GW_DEFAULT_TASK_STOP_POLL_MS;
    }

    if (psram_task_is_valid(&s_monitor_task)) {
        /* Do NOT force-delete: task may hold a mutex (e.g. MQTT publish).
         * Abandon instead to prevent xTaskPriorityDisinherit assert. */
        ESP_LOGW(TAG, "Monitoring task did not stop gracefully, abandoning");
        psram_task_mark_deleted(&s_monitor_task);
    }

    /* Free PSRAM resources (stack and TCB) if task exited cleanly */
    psram_task_delete(&s_monitor_task);

    ESP_LOGI(TAG, "System monitor stopped");
    return ESP_OK;
}

/**
 * @brief Get system uptime in seconds
 */
uint64_t system_monitor_get_uptime(void)
{
    int64_t uptime_us = esp_timer_get_time() - s_boot_time_us;
    return (uint64_t)(uptime_us / 1000000);
}

/**
 * @brief Get current system statistics
 */
system_stats_t system_monitor_get_stats(void)
{
    system_stats_t stats;
    memset(&stats, 0, sizeof(system_stats_t));

    /* Uptime */
    stats.uptime_seconds = system_monitor_get_uptime();

    /* Memory statistics */
    memory_stats_t mem_stats = memory_manager_get_stats();
    stats.free_heap = mem_stats.free_heap;
    stats.min_free_heap = mem_stats.min_free_heap;
    stats.largest_free_block = mem_stats.largest_free_block;
    stats.free_psram = mem_stats.free_psram;
    stats.heap_fragmentation = memory_manager_get_fragmentation();

    /* Task statistics */
    stats.task_count = task_manager_get_task_count();
    stats.cpu_usage = task_manager_get_cpu_usage();
    stats.stack_warning = task_manager_check_stack_usage(TASK_STACK_WARNING_THRESHOLD);

    /* Network status */
    stats.wifi_connected = wifi_manager_is_connected();
    stats.mqtt_connected = mqtt_client_is_connected();
    stats.wifi_rssi = wifi_manager_get_rssi();

    /* Overall health */
    stats.health = s_cached_health;

    return stats;
}

/**
 * @brief Get overall system health status
 */
system_health_t system_monitor_get_health(void)
{
    return calculate_system_health();
}

/**
 * @brief Log all system statistics
 */
void system_monitor_log_all(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  SYSTEM MONITOR - COMPREHENSIVE STATS");
    ESP_LOGI(TAG, "========================================");

    /* Uptime */
    uint64_t uptime = system_monitor_get_uptime();
    uint32_t days = uptime / GW_SECONDS_PER_DAY;
    uint32_t hours = (uptime % GW_SECONDS_PER_DAY) / GW_SECONDS_PER_HOUR;
    uint32_t minutes = (uptime % GW_SECONDS_PER_HOUR) / GW_SECONDS_PER_MINUTE;
    uint32_t seconds = uptime % GW_SECONDS_PER_MINUTE;

    ESP_LOGI(TAG, "Uptime: %lu days, %02lu:%02lu:%02lu", days, hours, minutes, seconds);

    /* Memory statistics */
    ESP_LOGI(TAG, "");
    memory_manager_log_stats();

    /* Task statistics */
    ESP_LOGI(TAG, "");
    task_manager_log_stats();

    /* CPU usage per task */
    ESP_LOGI(TAG, "");
    task_manager_log_cpu_usage();

    /* Network status */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=== Network Status ===");
    if (wifi_manager_is_connected()) {
        char ip_str[WIFI_IP_STRING_LEN];
        wifi_manager_get_ip(ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "WiFi: Connected (IP: %s, RSSI: %d dBm)",
                 ip_str, wifi_manager_get_rssi());
    } else {
        ESP_LOGI(TAG, "WiFi: Disconnected");
    }

    if (mqtt_client_is_connected()) {
        ESP_LOGI(TAG, "MQTT: Connected");
    } else {
        ESP_LOGI(TAG, "MQTT: Disconnected");
    }

    /* Overall health */
    ESP_LOGI(TAG, "");
    system_health_t health = system_monitor_get_health();
    const char *health_str;
    switch (health) {
        case SYSTEM_HEALTH_GOOD:
            health_str = "GOOD";
            break;
        case SYSTEM_HEALTH_WARNING:
            health_str = "WARNING";
            break;
        case SYSTEM_HEALTH_CRITICAL:
            health_str = "CRITICAL";
            break;
        default:
            health_str = "UNKNOWN";
    }
    ESP_LOGI(TAG, "Overall Health: %s", health_str);
    ESP_LOGI(TAG, "========================================");

    /* Start profiling for next interval's CPU measurement */
    task_manager_start_profiling();
}

/**
 * @brief Log compact system summary (single line)
 *
 * Called on most intervals - full stats only every N intervals.
 */
void system_monitor_log_compact(void)
{
    memory_stats_t stats = memory_manager_get_stats();
    system_health_t health = system_monitor_get_health();
    uint8_t cpu_usage = task_manager_get_cpu_usage();

    const char *health_str;
    switch (health) {
        case SYSTEM_HEALTH_GOOD:     health_str = "OK"; break;
        case SYSTEM_HEALTH_WARNING:  health_str = "WARN"; break;
        case SYSTEM_HEALTH_CRITICAL: health_str = "CRIT"; break;
        default:                     health_str = "?"; break;
    }

    /* Single line compact summary */
    ESP_LOGI(TAG, "[%s] Heap: %luKB free/%luKB min | CPU: %u%% | WiFi: %s | MQTT: %s",
             health_str,
             (unsigned long)(stats.free_heap / 1024),
             (unsigned long)(stats.min_free_heap / 1024),
             cpu_usage,
             wifi_manager_is_connected() ? "OK" : "X",
             mqtt_client_is_connected() ? "OK" : "X");

    /* Start profiling for next interval's CPU measurement */
    task_manager_start_profiling();
}

/**
 * @brief Feed watchdog timer
 */
void system_monitor_feed_watchdog(void)
{
    /* Feed task watchdog only if actually subscribed */
    if (s_wdt_subscribed && psram_task_is_valid(&s_monitor_task)) {
        esp_task_wdt_reset();
    }
}

/**
 * @brief Trigger system health check
 */
esp_err_t system_monitor_health_check(void)
{
    system_health_t health = system_monitor_get_health();

    if (health == SYSTEM_HEALTH_CRITICAL) {
        ESP_LOGE(TAG, "Health check FAILED: Critical issues detected");
        return ESP_ERR_INVALID_STATE;
    } else if (health == SYSTEM_HEALTH_WARNING) {
        ESP_LOGW(TAG, "Health check WARNING: Minor issues detected");
        return ESP_OK;
    } else {
        ESP_LOGI(TAG, "Health check PASSED: All systems healthy");
        return ESP_OK;
    }
}

/**
 * @brief Publish system statistics to MQTT
 */
esp_err_t system_monitor_publish_mqtt(void)
{
    if (!s_mqtt_publish_enabled || !mqtt_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Get stats as JSON */
    char json_buffer[SYSMON_JSON_BUFFER_SIZE];
    esp_err_t ret = system_monitor_get_json(json_buffer, sizeof(json_buffer));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate JSON stats");
        return ret;
    }

    /* Ownership transfer: handler frees both topic and payload_json via free().
     * TOPIC_BRIDGE_SYSTEM is a constant and json_buffer is on stack -> strdup both. */
    char *topic_heap = strdup(TOPIC_BRIDGE_SYSTEM);
    char *payload_heap = strdup(json_buffer);
    if (topic_heap == NULL || payload_heap == NULL) {
        free(topic_heap);
        free(payload_heap);
        ESP_LOGE(TAG, "Failed to alloc for system stats publish");
        return ESP_ERR_NO_MEM;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_heap,
        .payload_json = payload_heap,
        .qos = 0,
        .retain = false
    };
    ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret != ESP_OK) {
        /* event_publish failed - handler will not run, free both */
        free(topic_heap);
        free(payload_heap);
        ESP_LOGE(TAG, "Failed to publish system stats event");
    } else {
        ESP_LOGD(TAG, "Published system stats via event bus");
    }
    /* On success: do NOT free - ownership transferred to handler */

    return ret;
}

/**
 * @brief Enable/disable MQTT publishing
 */
esp_err_t system_monitor_set_mqtt_publishing(bool enable)
{
    s_mqtt_publish_enabled = enable;
    ESP_LOGI(TAG, "MQTT publishing %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

/**
 * @brief Set monitoring interval
 */
esp_err_t system_monitor_set_interval(uint32_t interval_sec)
{
    if (interval_sec < SYSMON_INTERVAL_MIN_SEC || interval_sec > SYSMON_INTERVAL_MAX_SEC) {
        ESP_LOGE(TAG, "Invalid interval: %lu (must be %d-%d seconds)",
                 interval_sec, SYSMON_INTERVAL_MIN_SEC, SYSMON_INTERVAL_MAX_SEC);
        return ESP_ERR_INVALID_ARG;
    }

    s_monitor_interval_sec = interval_sec;
    ESP_LOGI(TAG, "Monitoring interval set to %lu seconds", interval_sec);

    /* If monitoring is running, restart with new interval */
    if (s_monitor_running) {
        ESP_LOGI(TAG, "Restarting monitor with new interval");
        system_monitor_stop();
        return system_monitor_start(interval_sec);
    }

    return ESP_OK;
}

/**
 * @brief Get formatted system stats as JSON
 */
esp_err_t system_monitor_get_json(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    system_stats_t stats = system_monitor_get_stats();

    const char *health_str;
    switch (stats.health) {
        case SYSTEM_HEALTH_GOOD: health_str = "good"; break;
        case SYSTEM_HEALTH_WARNING: health_str = "warning"; break;
        case SYSTEM_HEALTH_CRITICAL: health_str = "critical"; break;
        default: health_str = "unknown";
    }

    int written = snprintf(buffer, buffer_size,
        "{"
        "\"uptime\":%llu,"
        "\"memory\":{"
            "\"free_heap\":%u,"
            "\"min_free_heap\":%u,"
            "\"largest_free_block\":%u,"
            "\"free_psram\":%u,"
            "\"fragmentation\":%u"
        "},"
        "\"tasks\":{"
            "\"count\":%u,"
            "\"cpu_usage\":%lu,"
            "\"stack_warning\":%s"
        "},"
        "\"network\":{"
            "\"wifi_connected\":%s,"
            "\"mqtt_connected\":%s,"
            "\"rssi\":%d"
        "},"
        "\"health\":\"%s\"",
        stats.uptime_seconds,
        stats.free_heap, stats.min_free_heap, stats.largest_free_block, stats.free_psram, stats.heap_fragmentation,
        stats.task_count, stats.cpu_usage, stats.stack_warning ? "true" : "false",
        stats.wifi_connected ? "true" : "false",
        stats.mqtt_connected ? "true" : "false",
        stats.wifi_rssi,
        health_str
    );

    if (written < 0 || (size_t)written >= buffer_size) {
        ESP_LOGE(TAG, "JSON buffer too small");
        return ESP_ERR_NO_MEM;
    }

    /* BLE status */
#if CONFIG_BT_SCANNER_ENABLED
    {
        int ble_written = snprintf(buffer + written, buffer_size - written,
            ",\"ble\":{"
                "\"state\":\"%s\","
                "\"scanning\":%s,"
                "\"active_scan\":%s,"
                "\"devices_tracked\":%u",
            ble_manager_get_state_str(),
            ble_scanner_is_running() ? "true" : "false",
            ble_scanner_is_active_enabled() ? "true" : "false",
            (unsigned int)ble_scanner_get_device_count()
        );
        if (ble_written > 0 && (size_t)(written + ble_written) < buffer_size) {
            written += ble_written;
        }
#if CONFIG_ESPHOME_API_ENABLE
        esphome_ble_proxy_stats_t proxy_stats;
        if (esphome_ble_proxy_get_stats(&proxy_stats) == ESP_OK) {
            int proxy_written = snprintf(buffer + written, buffer_size - written,
                ",\"proxy_connections\":%u,"
                "\"proxy_adverts_fwd\":%lu,"
                "\"proxy_adverts_dropped\":%lu",
                proxy_stats.active_connections,
                (unsigned long)proxy_stats.advertisements_forwarded,
                (unsigned long)proxy_stats.advertisements_dropped
            );
            if (proxy_written > 0 && (size_t)(written + proxy_written) < buffer_size) {
                written += proxy_written;
            }
        }
#endif
        /* Close ble object */
        if ((size_t)(written + 1) < buffer_size) {
            buffer[written++] = '}';
        }
    }
#endif

    /* ESPHome status */
#if CONFIG_ESPHOME_API_ENABLE
    {
        int esphome_written = snprintf(buffer + written, buffer_size - written,
            ",\"esphome\":{"
                "\"running\":%s,"
                "\"clients\":%u"
            "}",
            esphome_api_is_running() ? "true" : "false",
            (unsigned int)esphome_api_get_client_count()
        );
        if (esphome_written > 0 && (size_t)(written + esphome_written) < buffer_size) {
            written += esphome_written;
        }
    }
#endif

    /* Close root object */
    if ((size_t)(written + 1) < buffer_size) {
        buffer[written++] = '}';
        buffer[written] = '\0';
    } else {
        ESP_LOGE(TAG, "JSON buffer too small");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
