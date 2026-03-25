/**
 * @file event_bus.c
 * @brief Event bus implementation for ESP32-C5 Zigbee2MQTT NG
 *
 * Implements a centralized event-driven architecture with FreeRTOS queue-based
 * asynchronous event dispatching. Uses a dedicated dispatcher task to process
 * events and notify subscribers.
 *
 * @copyright Copyright (c) 2024-2025
 */

#include "event_bus.h"
#include "core/memory/memory_manager_ng.h"
#include "core/monitoring/perf_metrics.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "utils/freertos_helpers.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "EVENT_BUS";

/** Maximum number of events in the normal priority queue */
#define EVENT_QUEUE_SIZE        64

/** Maximum number of events in the high priority queue */
#define EVENT_HIGH_PRIO_QUEUE_SIZE  16

/** Maximum number of subscribers per event type (max actual usage: 5) */
#define MAX_SUBSCRIBERS         8

/** Dispatcher task priority */
#define DISPATCHER_TASK_PRIO    (tskIDLE_PRIORITY + 5)

/** Dispatcher task stack size in bytes */
#define DISPATCHER_STACK_SIZE   4096

/** Maximum event data size to prevent unbounded allocation */
#define EVENT_DATA_MAX_SIZE     4096

/**
 * @brief Event item structure for queue
 */
typedef struct {
    event_type_t type;          /**< Event type */
    void *data;                 /**< Pointer to copied data (or NULL) */
    size_t data_size;           /**< Size of data (0 for no-copy/ISR events) */
    event_priority_t priority;  /**< Event priority level */
} event_item_t;

/**
 * @brief Subscriber entry structure
 */
typedef struct {
    event_handler_t handler;    /**< Handler callback */
    event_filter_t filter;      /**< Optional filter callback (NULL = no filter) */
    void *ctx;                  /**< User context */
    bool active;                /**< Whether this entry is active */
} subscriber_entry_t;

/**
 * @brief Subscriber list for one event type
 */
typedef struct {
    subscriber_entry_t entries[MAX_SUBSCRIBERS];
    size_t count;   /**< Number of active subscribers */
} subscriber_list_t;

/** Normal priority event queue handle */
static QueueHandle_t s_event_queue = NULL;

/** High priority event queue handle (for HIGH and CRITICAL priority events) */
static QueueHandle_t s_event_queue_high = NULL;

/** Dispatcher task handle */
static psram_task_handle_t s_dispatcher_task = {0};

/** Mutex for subscriber array modifications */
static SemaphoreHandle_t s_subscriber_mutex = NULL;

/** Subscriber arrays - one per event type */
static subscriber_list_t s_subscribers[EVT_COUNT];

/** Whether event bus is initialized */
static bool s_initialized = false;

/** Flag to stop dispatcher task */
static volatile bool s_stop_dispatcher = false;

/**
 * @brief Event type name strings
 *
 * IMPORTANT: This array must match the event_type_t enum order in event_bus.h
 */
static const char *s_event_names[] = {
    /* Device Events */
    "DEVICE_JOINED",
    "DEVICE_LEFT",
    "DEVICE_STATE_CHANGED",
    "DEVICE_INTERVIEWED",

    /* Network Events */
    "NETWORK_READY",
    "ZB_PERMIT_JOIN",
    "ZB_ATTRIBUTE_REPORT",
    "ZB_COMMAND_RECEIVED",
    "ZB_BIND_SUCCESS",
    "ZB_BIND_FAILED",
    "ZB_BINDINGS_LIST",
    "ZB_GROUP_ADDED",
    "ZB_SCENE_ACTIVATED",

    /* WiFi Events */
    "WIFI_CONNECTED",
    "WIFI_DISCONNECTED",
    "WIFI_SCAN_COMPLETE",
    "WIFI_AP_STARTED",
    "WIFI_AP_CLIENT_CONNECTED",

    /* MQTT Events */
    "MQTT_CONNECTED",
    "MQTT_DISCONNECTED",
    "MQTT_SUBSCRIBE_OK",
    "MQTT_PUBLISH_OK",
    "MQTT_ERROR",

    /* HA Discovery Events */
    "HA_DISCOVERY_PUBLISH",

    /* BLE Events */
    "BLE_DEVICE_FOUND",
    "BLE_DEVICE_LOST",

    /* System Events */
    "SYSTEM_BOOT_COMPLETE",
    "SYSTEM_SHUTDOWN",
    "CONFIG_CHANGED",
    "LIFECYCLE_CHANGED",
    "MEMORY_PRESSURE",
    "SYSTEM_MEMORY_WARNING",
    "SYSTEM_MEMORY_CRITICAL",

    /* Command Events */
    "COMMAND_RECEIVED",
    "COMMAND_RESPONSE",

    /* ESPHome Events */
    "ESPHOME_CONNECTED",
    "ESPHOME_DISCONNECTED",

    /* Power Events */
    "POWER_STATE_CHANGED",

    /* Tuya Events */
    "TUYA_DATAPOINT",

    /* OTA Events (Gateway Firmware) */
    "OTA_STARTED",
    "OTA_PROGRESS",
    "OTA_COMPLETE",
    "OTA_FAILED",

    /* Zigbee Device OTA Events */
    "ZB_OTA_PROGRESS",
    "ZB_OTA_COMPLETE",
    "ZB_OTA_IMAGES_CHANGED",

    /* Extended Zigbee Events */
    "ZB_ROUTER_STATUS",
    "ZB_TOPOLOGY_RESPONSE",
    "ZB_TOUCHLINK_RESULT",
    "ZB_ALARM_TRIGGERED",
    "ZB_REPORTING_RESPONSE",

    /* Backup/Restore Events */
    "BACKUP_RESPONSE",

    /* Demand-Response Events */
    "DRLC_LOAD_CONTROL",

    /* Battery Events */
    "DEVICE_BATTERY_CHANGED",

    /* Bridge Events */
    "BRIDGE_PUBLISH",

    /* Availability Events */
    "DEVICE_AVAILABILITY_CHANGED",

    /* Retained Topic Cleanup Events */
    "DEVICE_TOPICS_CLEAR",
};

/**
 * @brief Validate event type
 *
 * @param type Event type to validate
 * @return true if valid, false otherwise
 */
static bool event_type_is_valid(event_type_t type)
{
    return type >= 0 && type < EVT_COUNT;
}

/**
 * @brief Local copy of a subscriber entry for dispatch
 *
 * We copy subscribers to a local array to avoid holding the mutex
 * while calling handlers. This prevents priority inheritance issues
 * in ESP-IDF v6.0 where the mutex release/re-acquire pattern can
 * trigger vTaskPriorityDisinheritAfterTimeout assertions.
 */
typedef struct {
    event_handler_t handler;
    event_filter_t filter;
    void *ctx;
} dispatch_entry_t;

/**
 * @brief Dispatch an event to all subscribers
 *
 * @param event Event item to dispatch
 */
static void event_dispatch(const event_item_t *event)
{
    if (!event_type_is_valid(event->type)) {
        ESP_LOGW(TAG, "Dispatch: invalid event type %d", (int)event->type);
        return;
    }

    /* Record dispatch start time for performance metrics */
    int64_t dispatch_start_us = esp_timer_get_time();

    /* Local copy of handlers to dispatch - avoids holding mutex during callbacks */
    dispatch_entry_t handlers[MAX_SUBSCRIBERS];
    size_t handler_count = 0;

    /* Take mutex to safely copy subscriber list */
    if (xSemaphoreTake(s_subscriber_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Dispatch: failed to take mutex for %s",
                 event_type_to_str(event->type));
        return;
    }

    /* Copy active handlers to local array */
    subscriber_list_t *list = &s_subscribers[event->type];
    for (size_t i = 0; i < MAX_SUBSCRIBERS && handler_count < MAX_SUBSCRIBERS; i++) {
        subscriber_entry_t *entry = &list->entries[i];
        if (entry->active && entry->handler != NULL) {
            handlers[handler_count].handler = entry->handler;
            handlers[handler_count].filter = entry->filter;
            handlers[handler_count].ctx = entry->ctx;
            handler_count++;
        }
    }

    /* Release mutex before calling handlers - no re-acquire needed */
    xSemaphoreGive(s_subscriber_mutex);

    /* Dispatch to all copied handlers */
    for (size_t i = 0; i < handler_count; i++) {
        /* Check filter if present */
        bool should_dispatch = true;
        if (handlers[i].filter != NULL) {
            should_dispatch = handlers[i].filter(event->type, event->data, handlers[i].ctx);
        }

        if (should_dispatch) {
            ESP_LOGD(TAG, "Dispatch %s (prio=%d) to handler %p",
                     event_type_to_str(event->type), (int)event->priority, handlers[i].handler);
            handlers[i].handler(event->type, event->data, event->data_size, handlers[i].ctx);
        } else {
            ESP_LOGD(TAG, "Filtered %s for handler %p",
                     event_type_to_str(event->type), handlers[i].handler);
        }
    }

    /* Record performance metrics for event dispatch */
    int64_t dispatch_end_us = esp_timer_get_time();
    uint32_t dispatch_time_us = (uint32_t)(dispatch_end_us - dispatch_start_us);
    perf_metrics_record(PERF_METRIC_EVENT_DISPATCHED, 1);
    perf_metrics_record(PERF_METRIC_EVENT_DISPATCH_TIME_US, dispatch_time_us);
}

/**
 * @brief Process and dispatch a single event
 *
 * @param event Event item to process
 */
static void event_process(event_item_t *event)
{
    ESP_LOGD(TAG, "Processing event: %s (data_size=%zu, prio=%d)",
             event_type_to_str(event->type), event->data_size, (int)event->priority);

    /* Dispatch to all subscribers */
    event_dispatch(event);

    /* Free copied data if it was allocated (data_size > 0 indicates copied data) */
    if (event->data != NULL && event->data_size > 0) {
        mem_ng_free(event->data);
    }
}

/**
 * @brief Dispatcher task function
 *
 * Waits for events on the queues and dispatches them to subscribers.
 * High priority queue is checked first before the normal queue.
 *
 * @param arg Unused
 */
static void event_dispatcher_task(void *arg)
{
    (void)arg;
    event_item_t event;

    ESP_LOGI(TAG, "Dispatcher task started");

    while (!s_stop_dispatcher) {
        /* Always check high priority queue first */
        if (xQueueReceive(s_event_queue_high, &event, 0) == pdTRUE) {
            event_process(&event);
            continue;  /* Check high priority queue again before normal */
        }

        /* Then check normal priority queue with timeout */
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            event_process(&event);
        }
    }

    ESP_LOGI(TAG, "Dispatcher task stopped");
    psram_task_mark_deleted(&s_dispatcher_task);
    vTaskDelete(NULL);
}

esp_err_t event_bus_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Initialize subscriber arrays */
    memset(s_subscribers, 0, sizeof(s_subscribers));

    /* Create subscriber mutex */
    s_subscriber_mutex = xSemaphoreCreateMutex();
    if (s_subscriber_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create subscriber mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create normal priority event queue in PSRAM to save internal RAM */
    s_event_queue = xQueueCreateWithCaps(EVENT_QUEUE_SIZE, sizeof(event_item_t),
                                          MALLOC_CAP_SPIRAM);
    if (s_event_queue == NULL) {
        ESP_LOGW(TAG, "PSRAM queue alloc failed, falling back to internal RAM");
        s_event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(event_item_t));
    }
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create normal priority event queue");
        vSemaphoreDelete(s_subscriber_mutex);
        s_subscriber_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Create high priority event queue (small, keep in internal RAM for speed) */
    s_event_queue_high = xQueueCreate(EVENT_HIGH_PRIO_QUEUE_SIZE, sizeof(event_item_t));
    if (s_event_queue_high == NULL) {
        ESP_LOGE(TAG, "Failed to create high priority event queue");
        vQueueDeleteWithCaps(s_event_queue);
        s_event_queue = NULL;
        vSemaphoreDelete(s_subscriber_mutex);
        s_subscriber_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Create dispatcher task (stack in PSRAM to save ~4KB internal RAM) */
    s_stop_dispatcher = false;
    memset(&s_dispatcher_task, 0, sizeof(s_dispatcher_task));
    esp_err_t task_err = psram_task_create(
        event_dispatcher_task,
        "event_disp",
        DISPATCHER_STACK_SIZE,
        NULL,
        DISPATCHER_TASK_PRIO,
        &s_dispatcher_task
    );

    if (task_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create dispatcher task");
        vQueueDelete(s_event_queue_high);
        s_event_queue_high = NULL;
        vQueueDeleteWithCaps(s_event_queue);
        s_event_queue = NULL;
        vSemaphoreDelete(s_subscriber_mutex);
        s_subscriber_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Initialized (queue_size=%d, high_prio_queue_size=%d, max_subs=%d)",
             EVENT_QUEUE_SIZE, EVENT_HIGH_PRIO_QUEUE_SIZE, MAX_SUBSCRIBERS);

    return ESP_OK;
}

esp_err_t event_bus_deinit(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Signal dispatcher task to stop */
    s_stop_dispatcher = true;

    /* Wait for task to finish (with timeout), then free PSRAM stack/TCB */
    if (s_dispatcher_task.task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    psram_task_delete(&s_dispatcher_task);

    /* Drain the high priority queue and free any copied data */
    if (s_event_queue_high != NULL) {
        event_item_t event;
        while (xQueueReceive(s_event_queue_high, &event, 0) == pdTRUE) {
            if (event.data != NULL && event.data_size > 0) {
                mem_ng_free(event.data);
            }
        }
        vQueueDelete(s_event_queue_high);
        s_event_queue_high = NULL;
    }

    /* Drain the normal priority queue and free any copied data */
    if (s_event_queue != NULL) {
        event_item_t event;
        while (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
            if (event.data != NULL && event.data_size > 0) {
                mem_ng_free(event.data);
            }
        }
        vQueueDeleteWithCaps(s_event_queue);
        s_event_queue = NULL;
    }

    /* Delete mutex */
    if (s_subscriber_mutex != NULL) {
        vSemaphoreDelete(s_subscriber_mutex);
        s_subscriber_mutex = NULL;
    }

    /* Clear subscriber arrays */
    memset(s_subscribers, 0, sizeof(s_subscribers));

    s_initialized = false;

    ESP_LOGI(TAG, "Deinitialized");

    return ESP_OK;
}

esp_err_t event_subscribe(event_type_t type, event_handler_t handler, void *ctx)
{
    /* Delegate to filtered version with NULL filter for backward compatibility */
    return event_subscribe_filtered(type, handler, NULL, ctx);
}

esp_err_t event_subscribe_filtered(event_type_t type, event_handler_t handler,
                                   event_filter_t filter, void *ctx)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!event_type_is_valid(type)) {
        ESP_LOGE(TAG, "Invalid event type: %d", (int)type);
        return ESP_ERR_INVALID_ARG;
    }

    if (handler == NULL) {
        ESP_LOGE(TAG, "Handler is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    /* Take mutex */
    if (xSemaphoreTake(s_subscriber_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    subscriber_list_t *list = &s_subscribers[type];
    esp_err_t ret = ESP_ERR_NO_MEM;

    /* Find empty slot */
    for (size_t i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!list->entries[i].active) {
            list->entries[i].handler = handler;
            list->entries[i].filter = filter;
            list->entries[i].ctx = ctx;
            list->entries[i].active = true;
            list->count++;
            ret = ESP_OK;

            ESP_LOGD(TAG, "Subscribed to %s (handler=%p, filter=%p, slot=%zu, total=%zu)",
                     event_type_to_str(type), handler, filter, i, list->count);

            /* Warn when approaching subscriber limit */
            if (list->count >= (MAX_SUBSCRIBERS * 3 / 4)) {
                ESP_LOGW(TAG, "Event %s has %zu/%d subscribers (%.0f%% full)",
                         event_type_to_str(type), list->count, MAX_SUBSCRIBERS,
                         (float)list->count / MAX_SUBSCRIBERS * 100);
            }
            break;
        }
    }

    xSemaphoreGive(s_subscriber_mutex);

    if (ret == ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "Max subscribers (%d) reached for %s — increase MAX_SUBSCRIBERS or reduce subscriptions",
                 MAX_SUBSCRIBERS, event_type_to_str(type));
    }

    return ret;
}

esp_err_t event_unsubscribe(event_type_t type, event_handler_t handler)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!event_type_is_valid(type)) {
        ESP_LOGE(TAG, "Invalid event type: %d", (int)type);
        return ESP_ERR_INVALID_ARG;
    }

    if (handler == NULL) {
        ESP_LOGE(TAG, "Handler is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    /* Take mutex */
    if (xSemaphoreTake(s_subscriber_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    subscriber_list_t *list = &s_subscribers[type];
    esp_err_t ret = ESP_ERR_NOT_FOUND;

    /* Find handler and mark as inactive */
    for (size_t i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (list->entries[i].active && list->entries[i].handler == handler) {
            list->entries[i].active = false;
            list->entries[i].handler = NULL;
            list->entries[i].filter = NULL;
            list->entries[i].ctx = NULL;
            list->count--;
            ret = ESP_OK;

            ESP_LOGD(TAG, "Unsubscribed from %s (handler=%p, remaining=%zu)",
                     event_type_to_str(type), handler, list->count);
            break;
        }
    }

    xSemaphoreGive(s_subscriber_mutex);

    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Handler %p not found for %s", handler, event_type_to_str(type));
    }

    return ret;
}

esp_err_t event_publish(event_type_t type, void *data, size_t data_size)
{
    /* Default to NORMAL priority for backward compatibility */
    return event_publish_prio(type, data, data_size, EVT_PRIORITY_NORMAL);
}

esp_err_t event_publish_prio(event_type_t type, void *data, size_t data_size,
                             event_priority_t priority)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!event_type_is_valid(type)) {
        ESP_LOGE(TAG, "Invalid event type: %d", (int)type);
        return ESP_ERR_INVALID_ARG;
    }

    event_item_t event = {
        .type = type,
        .data = NULL,
        .data_size = 0,
        .priority = priority,
    };

    /* Copy data if provided */
    if (data != NULL && data_size > 0) {
        if (data_size > EVENT_DATA_MAX_SIZE) {
            ESP_LOGE(TAG, "Event data too large: %zu > %d", data_size, EVENT_DATA_MAX_SIZE);
            return ESP_ERR_INVALID_SIZE;
        }
        event.data = mem_alloc(data_size, MEM_CAP_DEFAULT);
        if (event.data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes for event data", data_size);
            return ESP_ERR_NO_MEM;
        }
        memcpy(event.data, data, data_size);
        event.data_size = data_size;
    }

    /* Select queue based on priority */
    QueueHandle_t queue = (priority >= EVT_PRIORITY_HIGH) ? s_event_queue_high : s_event_queue;
    const char *queue_name = (priority >= EVT_PRIORITY_HIGH) ? "high" : "normal";

    /* Queue the event */
    if (xQueueSend(queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Queue (%s) full, dropping event %s", queue_name, event_type_to_str(type));
        /* Free copied data if queue send failed */
        if (event.data != NULL) {
            mem_ng_free(event.data);
        }
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "Published %s (data_size=%zu, prio=%d, queue=%s)",
             event_type_to_str(type), data_size, (int)priority, queue_name);

    return ESP_OK;
}

esp_err_t event_publish_from_isr(event_type_t type, void *data, size_t data_size)
{
    /* Default to NORMAL priority for backward compatibility */
    return event_publish_prio_from_isr(type, data, data_size, EVT_PRIORITY_NORMAL);
}

esp_err_t event_publish_prio_from_isr(event_type_t type, void *data, size_t data_size,
                                      event_priority_t priority)
{
    if (!event_type_is_valid(type)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Select queue based on priority */
    QueueHandle_t queue = (priority >= EVT_PRIORITY_HIGH) ? s_event_queue_high : s_event_queue;

    if (queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* For ISR: data must be static/global, no copy performed */
    event_item_t event = {
        .type = type,
        .data = data,
        .data_size = 0,  /* 0 indicates no-copy (won't be freed) */
        .priority = priority,
    };

    (void)data_size;  /* data_size should be 0 for ISR calls */

    BaseType_t higher_priority_task_woken = pdFALSE;

    if (xQueueSendFromISR(queue, &event, &higher_priority_task_woken) != pdTRUE) {
        return ESP_FAIL;
    }

    /* Request context switch if needed */
    portYIELD_FROM_ISR(higher_priority_task_woken);

    return ESP_OK;
}

const char *event_type_to_str(event_type_t type)
{
    if (!event_type_is_valid(type)) {
        return "UNKNOWN";
    }
    return s_event_names[type];
}

size_t event_get_subscriber_count(event_type_t type)
{
    if (!event_type_is_valid(type)) {
        return 0;
    }

    if (!s_initialized) {
        return 0;
    }

    /* Take mutex */
    if (xSemaphoreTake(s_subscriber_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    size_t count = s_subscribers[type].count;

    xSemaphoreGive(s_subscriber_mutex);

    return count;
}

void event_get_queue_stats(size_t *high_prio_count, size_t *normal_prio_count)
{
    if (high_prio_count != NULL) {
        if (s_event_queue_high != NULL) {
            *high_prio_count = (size_t)uxQueueMessagesWaiting(s_event_queue_high);
        } else {
            *high_prio_count = 0;
        }
    }

    if (normal_prio_count != NULL) {
        if (s_event_queue != NULL) {
            *normal_prio_count = (size_t)uxQueueMessagesWaiting(s_event_queue);
        } else {
            *normal_prio_count = 0;
        }
    }
}
