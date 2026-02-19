/**
 * @file task_manager.c
 * @brief Task Manager Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "task_manager.h"
#include "gateway_defaults.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "TASK_MGR";

/**
 * @brief Task state names for display
 */
const char *task_state_names[] = {
    "Running",   /* eRunning */
    "Ready",     /* eReady */
    "Blocked",   /* eBlocked */
    "Suspended", /* eSuspended */
    "Deleted",   /* eDeleted */
    "Invalid"    /* eInvalid */
};

/* Runtime stats enabled flag */
static bool s_runtime_stats_enabled = false;

/* CPU Profiling state (moved here for forward reference from task_manager_get_cpu_usage) */
static uint32_t s_profiling_counters[GW_TASK_MAX_COUNT];
static char s_profiling_names[GW_TASK_MAX_COUNT][TASK_MANAGER_MAX_NAME_LEN];
static size_t s_profiling_task_count = 0;
static uint32_t s_profiling_total_runtime = 0;
static bool s_profiling_started = false;

/**
 * @brief Initialize task manager
 */
esp_err_t task_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing task manager...");

    /* Check if runtime stats are available */
#if (configGENERATE_RUN_TIME_STATS == 1)
    s_runtime_stats_enabled = true;
    ESP_LOGI(TAG, "Runtime statistics enabled");
#else
    s_runtime_stats_enabled = false;
    ESP_LOGW(TAG, "Runtime statistics not enabled in FreeRTOSConfig.h");
#endif

    /* Log initial task statistics */
    task_manager_log_stats();

    ESP_LOGI(TAG, "Task manager initialized successfully");
    return ESP_OK;
}

/**
 * @brief Get information about all running tasks
 */
size_t task_manager_get_all_tasks(task_info_t *tasks, size_t max_count)
{
    if (tasks == NULL || max_count == 0) {
        return 0;
    }

    /* Get number of tasks */
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        return 0;
    }

    /* Allocate temporary array for TaskStatus_t */
    TaskStatus_t *task_status_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
    if (task_status_array == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for task array");
        return 0;
    }

    /* Get task information */
    uint32_t total_runtime;
    task_count = uxTaskGetSystemState(task_status_array, task_count, &total_runtime);

    /* Copy to our task_info_t structure */
    size_t copy_count = (task_count < max_count) ? task_count : max_count;
    for (size_t i = 0; i < copy_count; i++) {
        /* Copy task name */
        strncpy(tasks[i].name, task_status_array[i].pcTaskName, TASK_MANAGER_MAX_NAME_LEN - 1);
        tasks[i].name[TASK_MANAGER_MAX_NAME_LEN - 1] = '\0';

        /* Copy task information */
        tasks[i].handle = task_status_array[i].xHandle;
        tasks[i].priority = task_status_array[i].uxCurrentPriority;
        tasks[i].state = task_status_array[i].eCurrentState;
        tasks[i].runtime_counter = task_status_array[i].ulRunTimeCounter;

        /* Get stack information */
        tasks[i].stack_high_watermark = uxTaskGetStackHighWaterMark(task_status_array[i].xHandle);

        /* Calculate stack size and usage (approximate) */
        /* Note: FreeRTOS doesn't directly expose stack size, we estimate from watermark */
        tasks[i].stack_size = task_status_array[i].usStackHighWaterMark * 4; /* Approximate */

        if (tasks[i].stack_size > 0) {
            uint32_t used_stack = tasks[i].stack_size - (tasks[i].stack_high_watermark * sizeof(StackType_t));
            tasks[i].stack_usage_percent = (uint8_t)((used_stack * 100) / tasks[i].stack_size);
        } else {
            tasks[i].stack_usage_percent = 0;
        }

        /* Get core affinity
         * ESP32-C5 is single-core RISC-V, so core_id is always 0.
         * Note: xTaskGetAffinity is deprecated in ESP-IDF v6.0+
         * For multi-core targets, use xTaskGetCoreID() instead. */
#if CONFIG_FREERTOS_UNICORE || defined(CONFIG_IDF_TARGET_ESP32C5)
        tasks[i].core_id = 0;
#else
        /* Multi-core targets: use xTaskGetCoreID (v6.0+) or fallback */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        tasks[i].core_id = xTaskGetCoreID(task_status_array[i].xHandle);
#else
        tasks[i].core_id = xTaskGetAffinity(task_status_array[i].xHandle);
#endif
        if (tasks[i].core_id == tskNO_AFFINITY) {
            tasks[i].core_id = -1; /* No affinity */
        }
#endif
    }

    /* Free temporary array */
    vPortFree(task_status_array);

    return copy_count;
}

/**
 * @brief Get information about a specific task by name
 */
esp_err_t task_manager_get_task_by_name(const char *task_name, task_info_t *task_info)
{
    if (task_name == NULL || task_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Get all tasks */
    task_info_t tasks[GW_TASK_MAX_COUNT];
    size_t task_count = task_manager_get_all_tasks(tasks, GW_TASK_MAX_COUNT);

    /* Search for matching name */
    for (size_t i = 0; i < task_count; i++) {
        if (strcmp(tasks[i].name, task_name) == 0) {
            memcpy(task_info, &tasks[i], sizeof(task_info_t));
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Get task state as string
 */
const char* task_manager_get_state_string(eTaskState state)
{
    if (state >= eRunning && state <= eInvalid) {
        return task_state_names[state];
    }
    return "Unknown";
}

/**
 * @brief Log statistics for all tasks
 */
void task_manager_log_stats(void)
{
    task_info_t tasks[GW_TASK_MAX_COUNT];
    size_t task_count = task_manager_get_all_tasks(tasks, GW_TASK_MAX_COUNT);

    ESP_LOGI(TAG, "=== Task Statistics ===");
    ESP_LOGI(TAG, "Total Tasks: %u", task_count);
    ESP_LOGI(TAG, "%-16s %-10s %-8s %-10s %-12s %-8s",
             "Name", "State", "Priority", "Stack HWM", "Stack Usage", "Core");
    ESP_LOGI(TAG, "--------------------------------------------------------------------------------");

    for (size_t i = 0; i < task_count; i++) {
        const char *state_str = task_manager_get_state_string(tasks[i].state);

        ESP_LOGI(TAG, "%-16s %-10s %-8lu %-10lu %-12u%% %-8ld",
                 tasks[i].name,
                 state_str,
                 tasks[i].priority,
                 tasks[i].stack_high_watermark,
                 tasks[i].stack_usage_percent,
                 tasks[i].core_id);

        /* Warning for high stack usage */
        if (tasks[i].stack_usage_percent >= TASK_STACK_WARNING_THRESHOLD) {
            ESP_LOGW(TAG, "  WARNING: Task '%s' stack usage is %u%% (HWM: %lu bytes)",
                     tasks[i].name, tasks[i].stack_usage_percent,
                     tasks[i].stack_high_watermark);
        }

        /* Warning for very low stack watermark */
        if (tasks[i].stack_high_watermark < GW_TASK_STACK_WARNING_THRESHOLD) {
            ESP_LOGW(TAG, "  WARNING: Task '%s' has very low stack watermark: %lu bytes",
                     tasks[i].name, tasks[i].stack_high_watermark);
        }
    }

    /* CPU usage if available */
    if (s_runtime_stats_enabled) {
        uint32_t cpu_usage = task_manager_get_cpu_usage();
        ESP_LOGI(TAG, "CPU Usage: ~%lu%%", cpu_usage);
    }
}

/**
 * @brief Get approximate CPU usage percentage (delta since last profiling)
 *
 * Note: This function uses delta values since last profiling start for accuracy.
 * The cumulative calculation (since boot) was misleading because it approaches
 * 100% over time as all runtime counters accumulate.
 */
uint32_t task_manager_get_cpu_usage(void)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
    TaskStatus_t *task_status_array;
    UBaseType_t task_count = uxTaskGetNumberOfTasks();

    task_status_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
    if (task_status_array == NULL) {
        return 0;
    }

    uint32_t total_runtime;
    task_count = uxTaskGetSystemState(task_status_array, task_count, &total_runtime);

    /* Calculate delta since profiling start if available */
    uint32_t delta_total = 0;
    if (s_profiling_started && total_runtime > s_profiling_total_runtime) {
        delta_total = total_runtime - s_profiling_total_runtime;
    } else {
        /* No profiling data - use total runtime but warn this is cumulative */
        delta_total = total_runtime;
    }

    if (delta_total == 0) {
        vPortFree(task_status_array);
        return 0;
    }

    /* Find IDLE task delta runtime */
    uint32_t idle_delta = 0;
    for (UBaseType_t i = 0; i < task_count; i++) {
        if (strstr(task_status_array[i].pcTaskName, "IDLE") != NULL) {
            uint32_t idle_runtime = task_status_array[i].ulRunTimeCounter;

            /* Calculate delta if profiling was started */
            if (s_profiling_started) {
                for (size_t j = 0; j < s_profiling_task_count; j++) {
                    if (strcmp(s_profiling_names[j], task_status_array[i].pcTaskName) == 0) {
                        if (idle_runtime > s_profiling_counters[j]) {
                            idle_delta = idle_runtime - s_profiling_counters[j];
                        }
                        break;
                    }
                }
            } else {
                idle_delta = idle_runtime;
            }
            break;
        }
    }

    vPortFree(task_status_array);

    /* CPU usage = 100 - (idle_delta / delta_total * 100) */
    uint32_t cpu_usage = 100 - ((idle_delta * 100) / delta_total);
    return cpu_usage;
#else
    return 0; /* Not supported */
#endif
}

/**
 * @brief Check if any task has stack usage above threshold
 */
bool task_manager_check_stack_usage(uint8_t threshold)
{
    task_info_t tasks[GW_TASK_MAX_COUNT];
    size_t task_count = task_manager_get_all_tasks(tasks, GW_TASK_MAX_COUNT);

    for (size_t i = 0; i < task_count; i++) {
        if (tasks[i].stack_usage_percent >= threshold) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Get total number of tasks
 */
size_t task_manager_get_task_count(void)
{
    return (size_t)uxTaskGetNumberOfTasks();
}

/**
 * @brief Enable/disable runtime statistics collection
 */
esp_err_t task_manager_set_runtime_stats(bool enable)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
    s_runtime_stats_enabled = enable;
    ESP_LOGI(TAG, "Runtime statistics %s", enable ? "enabled" : "disabled");
    return ESP_OK;
#else
    ESP_LOGE(TAG, "Runtime statistics not supported (configGENERATE_RUN_TIME_STATS=0)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * @brief Get formatted task list string
 */
esp_err_t task_manager_get_formatted_list(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    task_info_t tasks[GW_TASK_MAX_COUNT];
    size_t task_count = task_manager_get_all_tasks(tasks, GW_TASK_MAX_COUNT);

    size_t offset = 0;
    int written;

    /* Header */
    written = snprintf(buffer + offset, buffer_size - offset,
                      "Tasks: %u\n", task_count);
    if (written < 0 || (size_t)written >= buffer_size - offset) {
        return ESP_ERR_NO_MEM;
    }
    offset += written;

    /* Task list */
    for (size_t i = 0; i < task_count && offset < buffer_size; i++) {
        written = snprintf(buffer + offset, buffer_size - offset,
                          "%s: prio=%u hwm=%u usage=%u%% state=%s\n",
                          tasks[i].name,
                          (unsigned int)tasks[i].priority,
                          (unsigned int)tasks[i].stack_high_watermark,
                          tasks[i].stack_usage_percent,
                          task_manager_get_state_string(tasks[i].state));

        if (written < 0 || (size_t)written >= buffer_size - offset) {
            return ESP_ERR_NO_MEM;
        }
        offset += written;
    }

    return ESP_OK;
}

/* ============================================================================
 * CPU Profiling Functions
 * ============================================================================ */

/* Note: Profiling state variables declared near top of file for forward reference */

/**
 * @brief Start CPU profiling period
 */
void task_manager_start_profiling(void)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
    TaskStatus_t *task_status_array;
    UBaseType_t task_count = uxTaskGetNumberOfTasks();

    task_status_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
    if (task_status_array == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for profiling");
        return;
    }

    uint32_t total_runtime;
    task_count = uxTaskGetSystemState(task_status_array, task_count, &total_runtime);

    /* Store initial values */
    s_profiling_task_count = (task_count < GW_TASK_MAX_COUNT) ? task_count : GW_TASK_MAX_COUNT;
    s_profiling_total_runtime = total_runtime;

    for (size_t i = 0; i < s_profiling_task_count; i++) {
        s_profiling_counters[i] = task_status_array[i].ulRunTimeCounter;
        strncpy(s_profiling_names[i], task_status_array[i].pcTaskName, TASK_MANAGER_MAX_NAME_LEN - 1);
        s_profiling_names[i][TASK_MANAGER_MAX_NAME_LEN - 1] = '\0';
    }

    vPortFree(task_status_array);
    s_profiling_started = true;

    ESP_LOGI(TAG, "CPU profiling started (total runtime: %lu)", total_runtime);
#else
    ESP_LOGW(TAG, "Runtime stats not enabled - profiling unavailable");
#endif
}

/**
 * @brief Log CPU usage per task
 */
void task_manager_log_cpu_usage(void)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
    TaskStatus_t *task_status_array;
    UBaseType_t task_count = uxTaskGetNumberOfTasks();

    task_status_array = pvPortMalloc(task_count * sizeof(TaskStatus_t));
    if (task_status_array == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for CPU stats");
        return;
    }

    uint32_t total_runtime;
    task_count = uxTaskGetSystemState(task_status_array, task_count, &total_runtime);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "+------------------+------+-----------+-----------+--------+");
    ESP_LOGI(TAG, "| CPU USAGE PER TASK (DIAGNOSTIC)                          |");
    ESP_LOGI(TAG, "+------------------+------+-----------+-----------+--------+");
    ESP_LOGI(TAG, "| %-16s | %4s | %-9s | %9s | %6s |", "Task", "CPU%", "State", "Runtime", "StkFre");
    ESP_LOGI(TAG, "+------------------+------+-----------+-----------+--------+");

    /* Calculate delta if profiling was started */
    uint32_t delta_total = 0;
    if (s_profiling_started && total_runtime > s_profiling_total_runtime) {
        delta_total = total_runtime - s_profiling_total_runtime;
    } else {
        delta_total = total_runtime;
    }

    if (delta_total == 0) {
        delta_total = 1; /* Avoid division by zero */
    }

    /* Sort by CPU usage (simple bubble sort for small array) */
    for (UBaseType_t i = 0; i < task_count - 1; i++) {
        for (UBaseType_t j = 0; j < task_count - i - 1; j++) {
            if (task_status_array[j].ulRunTimeCounter < task_status_array[j + 1].ulRunTimeCounter) {
                TaskStatus_t temp = task_status_array[j];
                task_status_array[j] = task_status_array[j + 1];
                task_status_array[j + 1] = temp;
            }
        }
    }

    /* Print each task */
    uint32_t non_idle_total = 0;
    for (UBaseType_t i = 0; i < task_count; i++) {
        uint32_t runtime = task_status_array[i].ulRunTimeCounter;

        /* Calculate delta if profiling was started */
        if (s_profiling_started) {
            for (size_t j = 0; j < s_profiling_task_count; j++) {
                if (strcmp(s_profiling_names[j], task_status_array[i].pcTaskName) == 0) {
                    if (runtime > s_profiling_counters[j]) {
                        runtime = runtime - s_profiling_counters[j];
                    }
                    break;
                }
            }
        }

        /* Use 64-bit arithmetic to prevent overflow (runtime * 100 can exceed uint32_t) */
        uint32_t cpu_percent = (uint32_t)(((uint64_t)runtime * 100) / delta_total);
        const char *state_str = task_manager_get_state_string(task_status_array[i].eCurrentState);

        /* Track non-idle CPU */
        if (strstr(task_status_array[i].pcTaskName, "IDLE") == NULL) {
            non_idle_total += cpu_percent;
        }

        /* Stack high water mark in bytes */
        uint32_t stk_free = (uint32_t)task_status_array[i].usStackHighWaterMark * sizeof(StackType_t);

        /* Highlight high CPU usage tasks */
        if (cpu_percent >= 10 && strstr(task_status_array[i].pcTaskName, "IDLE") == NULL) {
            ESP_LOGW(TAG, "| %-16s | %3lu%% | %-9s | %9lu | %5lu |<<",
                     task_status_array[i].pcTaskName,
                     cpu_percent, state_str, runtime, stk_free);
        } else if (cpu_percent >= 1) {
            ESP_LOGI(TAG, "| %-16s | %3lu%% | %-9s | %9lu | %5lu |",
                     task_status_array[i].pcTaskName,
                     cpu_percent, state_str, runtime, stk_free);
        } else {
            ESP_LOGI(TAG, "| %-16s |  <1%% | %-9s | %9lu | %5lu |",
                     task_status_array[i].pcTaskName,
                     state_str, runtime, stk_free);
        }
    }

    ESP_LOGI(TAG, "+------------------+------+-----------+-----------+--------+");
    ESP_LOGI(TAG, "| Non-idle CPU: %3lu%% | Total runtime: %10lu              |", non_idle_total, total_runtime);
    if (s_profiling_started) {
        ESP_LOGI(TAG, "| Delta: %10lu                                         |", delta_total);
    }
    ESP_LOGI(TAG, "+----------------------------------------------------------+");

    /* Identify potential CPU hogs using DELTA runtime, not cumulative */
    for (UBaseType_t i = 0; i < task_count; i++) {
        uint32_t runtime = task_status_array[i].ulRunTimeCounter;

        /* Calculate delta runtime (same logic as table display) */
        if (s_profiling_started) {
            for (size_t j = 0; j < s_profiling_task_count; j++) {
                if (strcmp(s_profiling_names[j], task_status_array[i].pcTaskName) == 0) {
                    if (runtime > s_profiling_counters[j]) {
                        runtime = runtime - s_profiling_counters[j];
                    }
                    break;
                }
            }
        }

        /* Use 64-bit arithmetic to prevent overflow (runtime * 100 can exceed uint32_t) */
        uint32_t cpu_percent = (uint32_t)(((uint64_t)runtime * 100) / delta_total);

        /* Only warn for >= 30% CPU in delta period (not 20% which gives too many false positives) */
        if (cpu_percent >= 30 && strstr(task_status_array[i].pcTaskName, "IDLE") == NULL) {
            ESP_LOGW(TAG, "*** HIGH CPU: '%s' using %lu%% in last period ***",
                     task_status_array[i].pcTaskName, cpu_percent);
        }
    }

    vPortFree(task_status_array);
#else
    ESP_LOGW(TAG, "Runtime stats not enabled in FreeRTOSConfig.h");
    ESP_LOGW(TAG, "Enable CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y");
#endif
}
