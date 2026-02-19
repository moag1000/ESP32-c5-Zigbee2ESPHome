/**
 * @file task_manager.h
 * @brief Task Manager for FreeRTOS Task Monitoring
 *
 * Provides comprehensive monitoring of FreeRTOS tasks including:
 * - Task enumeration and status
 * - Stack usage and high watermark tracking
 * - CPU usage estimation
 * - Task health monitoring
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum task name length
 */
#define TASK_MANAGER_MAX_NAME_LEN  16

/**
 * @brief Task information structure
 *
 * Contains detailed information about a FreeRTOS task
 */
typedef struct {
    char name[TASK_MANAGER_MAX_NAME_LEN];   /**< Task name */
    TaskHandle_t handle;                     /**< Task handle */
    UBaseType_t priority;                    /**< Task priority */
    uint32_t stack_size;                     /**< Total stack size in bytes */
    uint32_t stack_high_watermark;           /**< Minimum free stack ever seen */
    eTaskState state;                        /**< Current task state */
    uint32_t runtime_counter;                /**< Task runtime counter (if enabled) */
    uint8_t stack_usage_percent;             /**< Stack usage percentage */
    BaseType_t core_id;                      /**< Core ID (for multi-core systems) */
} task_info_t;

/**
 * @brief Task state strings for logging
 */
extern const char *task_state_names[];

/**
 * @brief Stack usage warning threshold (percentage)
 */
#define TASK_STACK_WARNING_THRESHOLD  80

/**
 * @brief Initialize task manager
 *
 * Sets up task monitoring infrastructure.
 *
 * @return ESP_OK on success
 *         ESP_FAIL on failure
 */
esp_err_t task_manager_init(void);

/**
 * @brief Get information about all running tasks
 *
 * Populates an array with information about all FreeRTOS tasks.
 *
 * @param[out] tasks Array to store task information
 * @param[in] max_count Maximum number of tasks to retrieve
 * @return Number of tasks actually retrieved (may be less than max_count)
 */
size_t task_manager_get_all_tasks(task_info_t *tasks, size_t max_count);

/**
 * @brief Get information about a specific task by name
 *
 * @param[in] task_name Name of the task to query
 * @param[out] task_info Pointer to store task information
 * @return ESP_OK if task found
 *         ESP_ERR_NOT_FOUND if task not found
 */
esp_err_t task_manager_get_task_by_name(const char *task_name, task_info_t *task_info);

/**
 * @brief Log statistics for all tasks
 *
 * Outputs detailed statistics for all running tasks to console.
 * Includes warnings for tasks with high stack usage.
 */
void task_manager_log_stats(void);

/**
 * @brief Get approximate CPU usage percentage
 *
 * Calculates CPU usage based on idle task runtime.
 * Requires configGENERATE_RUN_TIME_STATS to be enabled.
 *
 * @return CPU usage percentage (0-100)
 *         Returns 0 if runtime stats are not enabled
 */
uint32_t task_manager_get_cpu_usage(void);

/**
 * @brief Check if any task has stack usage above threshold
 *
 * @param[in] threshold Stack usage percentage threshold (0-100)
 * @return true if any task exceeds threshold
 */
bool task_manager_check_stack_usage(uint8_t threshold);

/**
 * @brief Get total number of tasks
 *
 * @return Number of tasks currently in the system
 */
size_t task_manager_get_task_count(void);

/**
 * @brief Get task state as string
 *
 * @param[in] state Task state enumeration
 * @return String representation of task state
 */
const char* task_manager_get_state_string(eTaskState state);

/**
 * @brief Enable/disable runtime statistics collection
 *
 * Note: Requires configGENERATE_RUN_TIME_STATS=1 in FreeRTOSConfig.h
 *
 * @param[in] enable true to enable, false to disable
 * @return ESP_OK on success
 *         ESP_ERR_NOT_SUPPORTED if not compiled with runtime stats
 */
esp_err_t task_manager_set_runtime_stats(bool enable);

/**
 * @brief Get formatted task list string
 *
 * Generates a formatted string with all task information suitable
 * for publishing to MQTT or logging.
 *
 * @param[out] buffer Buffer to store formatted string
 * @param[in] buffer_size Size of buffer
 * @return ESP_OK on success
 *         ESP_ERR_NO_MEM if buffer too small
 */
esp_err_t task_manager_get_formatted_list(char *buffer, size_t buffer_size);

/**
 * @brief Log CPU usage per task for diagnostics
 *
 * Calculates and logs CPU usage percentage for each task.
 * Useful for identifying CPU-intensive tasks.
 * Requires configGENERATE_RUN_TIME_STATS to be enabled.
 *
 * Output format:
 *   Task Name       CPU%   State    Stack
 *   IDLE            85%    Ready    512
 *   zb_main         10%    Blocked  2048
 *   ...
 */
void task_manager_log_cpu_usage(void);

/**
 * @brief Start CPU profiling period
 *
 * Captures initial runtime counters for delta calculation.
 * Call this, then wait some time, then call task_manager_log_cpu_usage()
 * to get accurate CPU usage over that period.
 */
void task_manager_start_profiling(void);

#ifdef __cplusplus
}
#endif

#endif /* TASK_MANAGER_H */
