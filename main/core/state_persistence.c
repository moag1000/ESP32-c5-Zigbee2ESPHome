/**
 * @file state_persistence.c
 * @brief Device State Persistence Implementation
 *
 * Persists device runtime state to LittleFS for recovery after reboot.
 * Uses periodic timer-based saving with dirty flag to minimize flash writes.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "state_persistence.h"
#include "littlefs_mount.h"
#include "device_state_publisher.h"
#include "zigbee/zb_device_handler_types.h"
#include "core/device/device_registry.h"
#include "core/memory/memory_manager_ng.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "mqtt/mqtt_topics.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "utils/freertos_helpers.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "STATE_PERSIST";

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Module initialized flag */
static volatile bool s_initialized = false;

/** @brief Flag requesting the save task to exit its loop */
static volatile bool s_task_exit_requested = false;

/**
 * @brief Dirty flag - state needs saving
 *
 * Safe to use volatile (without atomics) on ESP32-C5 single-core RISC-V where
 * aligned bool read/write is naturally atomic. On multi-core targets this would
 * require atomic operations or a mutex to prevent torn reads/writes.
 */
static volatile bool s_dirty = false;

/** @brief Periodic save timer handle */
static TimerHandle_t s_save_timer = NULL;

/** @brief Dedicated save task handle (avoids heavy I/O in timer daemon) */
static psram_task_handle_t s_save_psram_task = {0};

/** @brief Completion semaphore for save task shutdown */
static SemaphoreHandle_t s_save_done = NULL;

/** @brief Mutex to prevent concurrent save operations (task + shutdown handler) */
static SemaphoreHandle_t s_save_mutex = NULL;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void save_task_fn(void *arg);
static void save_timer_callback(TimerHandle_t timer);
static void shutdown_handler(void);
static esp_err_t ensure_state_dir(void);
static void format_ieee_addr(const uint8_t *ieee_addr, char *buf, size_t buf_len);

/* ============================================================================
 * Device Iterator Helpers (NG)
 * ============================================================================ */

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t state_persistence_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing state persistence...");

    /* Mount LittleFS (reference counted) */
    esp_err_t ret = littlefs_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create state directory */
    ret = ensure_state_dir();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create state directory: %s", esp_err_to_name(ret));
        littlefs_unmount();
        return ret;
    }

    /* Create completion semaphore for save task shutdown */
    s_save_done = xSemaphoreCreateBinary();
    if (s_save_done == NULL) {
        ESP_LOGE(TAG, "Failed to create save done semaphore");
        littlefs_unmount();
        return ESP_ERR_NO_MEM;
    }

    /* Create mutex to protect concurrent save operations */
    s_save_mutex = xSemaphoreCreateMutex();
    if (s_save_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create save mutex");
        vSemaphoreDelete(s_save_done);
        s_save_done = NULL;
        littlefs_unmount();
        return ESP_ERR_NO_MEM;
    }

    /* Create dedicated save task to avoid heavy I/O in timer daemon.
     * Stack in PSRAM to save ~6KB internal RAM.
     * Guard against duplicate task creation (e.g. if init called twice
     * after a partial failure). */
    if (s_save_psram_task.task_handle != NULL) {
        ESP_LOGW(TAG, "Save task already exists, skipping creation");
    } else {
        memset(&s_save_psram_task, 0, sizeof(s_save_psram_task));
        esp_err_t task_ret = psram_task_create(save_task_fn,
                                                "state_save",
                                                6144,  /* Increased for debug builds */
                                                NULL,
                                                2,  /* Below publisher_task (4) */
                                                &s_save_psram_task);
        if (task_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create save task");
            vSemaphoreDelete(s_save_mutex);
            s_save_mutex = NULL;
            vSemaphoreDelete(s_save_done);
            s_save_done = NULL;
            littlefs_unmount();
            return ESP_ERR_NO_MEM;
        }
    }

    /* Create periodic save timer */
    s_save_timer = xTimerCreate("state_save",
                                 pdMS_TO_TICKS((uint32_t)CONFIG_STATE_PERSISTENCE_INTERVAL_SEC * 1000),
                                 pdTRUE,  /* Auto-reload */
                                 NULL,
                                 save_timer_callback);
    if (s_save_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create save timer");
        psram_task_delete(&s_save_psram_task);
        vSemaphoreDelete(s_save_mutex);
        s_save_mutex = NULL;
        vSemaphoreDelete(s_save_done);
        s_save_done = NULL;
        littlefs_unmount();
        return ESP_ERR_NO_MEM;
    }

    /* Start the timer */
    if (xTimerStart(s_save_timer, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start save timer");
        xTimerDelete(s_save_timer, GW_TIMEOUT_MEDIUM_TICKS);
        s_save_timer = NULL;
        psram_task_delete(&s_save_psram_task);
        vSemaphoreDelete(s_save_mutex);
        s_save_mutex = NULL;
        vSemaphoreDelete(s_save_done);
        s_save_done = NULL;
        littlefs_unmount();
        return ESP_FAIL;
    }

    /* Register shutdown handler for clean save on reboot */
    ret = esp_register_shutdown_handler(shutdown_handler);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register shutdown handler: %s (state will not auto-save on reboot)",
                 esp_err_to_name(ret));
    }

    s_initialized = true;
    s_dirty = false;

    ESP_LOGI(TAG, "State persistence initialized (interval=%ds)",
             CONFIG_STATE_PERSISTENCE_INTERVAL_SEC);
    return ESP_OK;
}

void state_persistence_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Deinitializing state persistence...");

    /* Unregister shutdown handler so it won't fire after deinit */
    esp_unregister_shutdown_handler(shutdown_handler);

    /* Stop and delete timer first so no new notifications arrive */
    if (s_save_timer != NULL) {
        xTimerStop(s_save_timer, GW_TIMEOUT_MEDIUM_TICKS);
        xTimerDelete(s_save_timer, GW_TIMEOUT_MEDIUM_TICKS);
        s_save_timer = NULL;
    }

    /* Request the save task to exit after its next save cycle.
     * Setting s_dirty ensures any pending state is flushed. */
    if (s_save_psram_task.task_handle != NULL) {
        s_task_exit_requested = true;
        s_dirty = true;
        xTaskNotifyGive(s_save_psram_task.task_handle);
        /* Wait for save task to complete and self-delete (max 5 seconds) */
        if (s_save_done != NULL) {
            BaseType_t taken = xSemaphoreTake(s_save_done, pdMS_TO_TICKS(5000));
            if (taken != pdTRUE) {
                ESP_LOGW(TAG, "Timed out waiting for save task — retrying");
                /* Send another notification in case the task missed the first */
                xTaskNotifyGive(s_save_psram_task.task_handle);
                taken = xSemaphoreTake(s_save_done, pdMS_TO_TICKS(2000));
                if (taken != pdTRUE) {
                    ESP_LOGE(TAG, "Save task did not exit in time, abandoning");
                    /* Do NOT force-delete: task may hold s_save_mutex.
                     * Force-delete corrupts FreeRTOS priority inheritance.
                     * Leak the task instead of crashing. */
                }
            }
        }
        /* Free PSRAM stack/TCB (task already self-deleted via mark_deleted) */
        psram_task_delete(&s_save_psram_task);
        s_task_exit_requested = false;
    }

    /* Delete save mutex */
    if (s_save_mutex != NULL) {
        vSemaphoreDelete(s_save_mutex);
        s_save_mutex = NULL;
    }

    /* Delete completion semaphore */
    if (s_save_done != NULL) {
        vSemaphoreDelete(s_save_done);
        s_save_done = NULL;
    }

    /* Unmount LittleFS (reference counted) */
    littlefs_unmount();

    s_initialized = false;
    ESP_LOGI(TAG, "State persistence deinitialized");
}

esp_err_t state_persistence_save(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_dirty) {
        ESP_LOGD(TAG, "State not dirty, skipping save");
        return ESP_OK;
    }

    /* Serialize save operations — prevents concurrent saves from the
     * periodic task and the shutdown handler. */
    if (s_save_mutex != NULL) {
        xSemaphoreTake(s_save_mutex, GW_TIMEOUT_LONG_TICKS);
    }

    /* Re-check dirty after acquiring the mutex; a concurrent save may
     * have already flushed the state while we were waiting. */
    if (!s_dirty) {
        if (s_save_mutex != NULL) {
            xSemaphoreGive(s_save_mutex);
        }
        return ESP_OK;
    }

    /* Clear dirty flag before save — any new changes during save
     * will re-set it, ensuring they're caught on the next timer tick.
     * Safe on ESP32-C5 single-core: no torn writes on aligned bool. */
    s_dirty = false;

    ESP_LOGI(TAG, "Saving device states to LittleFS...");

    esp_err_t ret;

    /* Allocate JSON buffer (auto-PSRAM for large allocations) */
    char *buf = (char *)mem_alloc(STATE_PERSIST_MAX_FILE_SIZE, MEM_CAP_PSRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate state buffer");
        s_dirty = true;
        ret = ESP_ERR_NO_MEM;
        goto save_unlock;
    }

    /* Build JSON object with all device states */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create root JSON object");
        mem_ng_free(buf);
        s_dirty = true;
        ret = ESP_ERR_NO_MEM;
        goto save_unlock;
    }

    /* Collect device IDs first, then build JSON outside registry mutex
     * (AP-1.3: avoid holding registry mutex during long JSON serialization) */
    device_id_t ids[DEVICE_REGISTRY_MAX_DEVICES];
    size_t id_count;
    if (device_registry_collect_ids(ids, DEVICE_REGISTRY_MAX_DEVICES, &id_count) != ESP_OK) {
        id_count = 0;
    }

    size_t saved_count = 0;
    for (size_t i = 0; i < id_count; i++) {
        device_t *dev = device_registry_get(ids[i]);
        if (dev == NULL) continue;

        char *state_json = device_state_to_json(dev);
        if (state_json == NULL) continue;

        cJSON *state_obj = cJSON_Parse(state_json);
        free(state_json);
        if (state_obj == NULL) continue;

        char ieee_key[20];
        snprintf(ieee_key, sizeof(ieee_key), "0x%016llX", (unsigned long long)dev->id);
        cJSON_AddStringToObject(state_obj, "_friendly_name", dev->friendly_name);
        cJSON_AddItemToObject(root, ieee_key, state_obj);
        saved_count++;
    }

    /* Serialize to buffer */
    if (!cJSON_PrintPreallocated(root, buf, STATE_PERSIST_MAX_FILE_SIZE, false)) {
        ESP_LOGE(TAG, "Failed to serialize state JSON (buffer too small?)");
        cJSON_Delete(root);
        mem_ng_free(buf);
        s_dirty = true;
        ret = ESP_ERR_NO_MEM;
        goto save_unlock;
    }
    cJSON_Delete(root);

    size_t json_len = strlen(buf);

    /* Atomic write: write to temp file then rename */
    FILE *f = fopen(STATE_PERSIST_TEMP_PATH, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open temp file for writing: %s", STATE_PERSIST_TEMP_PATH);
        mem_ng_free(buf);
        s_dirty = true;
        ret = ESP_FAIL;
        goto save_unlock;
    }

    size_t written = fwrite(buf, 1, json_len, f);
    fclose(f);
    mem_ng_free(buf);

    if (written != json_len) {
        ESP_LOGE(TAG, "Incomplete write: %zu/%zu bytes", written, json_len);
        remove(STATE_PERSIST_TEMP_PATH);
        s_dirty = true;
        ret = ESP_FAIL;
        goto save_unlock;
    }

    /* Rename temp to final (atomic on LittleFS) */
    if (rename(STATE_PERSIST_TEMP_PATH, STATE_PERSIST_FILE_PATH) != 0) {
        ESP_LOGE(TAG, "Failed to rename temp file to state file");
        remove(STATE_PERSIST_TEMP_PATH);
        s_dirty = true;
        ret = ESP_FAIL;
        goto save_unlock;
    }

    ESP_LOGI(TAG, "Saved %zu device states (%zu bytes)", saved_count, json_len);

    ret = ESP_OK;

save_unlock:
    if (s_save_mutex != NULL) {
        xSemaphoreGive(s_save_mutex);
    }
    return ret;
}

esp_err_t state_persistence_load_and_publish(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Loading persisted device states...");

    /* Check if state file exists */
    struct stat st;
    if (stat(STATE_PERSIST_FILE_PATH, &st) != 0) {
        ESP_LOGI(TAG, "No persisted state file found");
        return ESP_OK;  /* Not an error, just no saved state */
    }

    if (st.st_size == 0 || st.st_size > STATE_PERSIST_MAX_FILE_SIZE) {
        ESP_LOGW(TAG, "Invalid state file size: %ld", st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Allocate read buffer (auto-PSRAM for large allocations) */
    char *buf = (char *)mem_alloc(st.st_size + 1, MEM_CAP_PSRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate read buffer");
        return ESP_ERR_NO_MEM;
    }

    /* Read state file */
    FILE *f = fopen(STATE_PERSIST_FILE_PATH, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open state file");
        mem_ng_free(buf);
        return ESP_FAIL;
    }

    size_t read_size = fread(buf, 1, st.st_size, f);
    fclose(f);
    buf[read_size] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(buf);
    mem_ng_free(buf);

    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse state JSON");
        return ESP_ERR_INVALID_ARG;
    }

    /* Build a lookup map: iterate all registered devices using the safe
     * copy-by-index pattern, convert each IEEE address to a hex key, and
     * check if a matching entry exists in the loaded JSON.  This avoids
     * the thread-unsafe zb_device_get_by_ieee() which returns a raw pointer
     * that can become invalid after the device mutex is released. */
    size_t published_count = 0;
    size_t device_count = device_registry_count();

    for (size_t i = 0; i < device_count; i++) {
        device_t *dev = device_registry_get_by_index(i);
        if (dev == NULL) {
            continue;
        }

        /* Convert this device's IEEE address to a hex key */
        char ieee_key[20];
        device_id_to_str(dev->id, ieee_key, sizeof(ieee_key));

        /* Look up this device's persisted state in the JSON */
        cJSON *item = cJSON_GetObjectItem(root, ieee_key);
        if (item == NULL || !cJSON_IsObject(item)) {
            continue;
        }

        /* Determine friendly name: prefer live device registry, fall back to stored */
        const char *friendly_name = NULL;
        if (dev->friendly_name[0] != '\0') {
            friendly_name = dev->friendly_name;
        } else {
            cJSON *name_item = cJSON_GetObjectItem(item, "_friendly_name");
            if (name_item != NULL && cJSON_IsString(name_item)) {
                friendly_name = name_item->valuestring;
            }
        }

        if (friendly_name == NULL || strlen(friendly_name) == 0) {
            ESP_LOGW(TAG, "No friendly name for device %s, skipping", ieee_key);
            continue;
        }

        /* Remove the internal _friendly_name field before publishing */
        cJSON_DeleteItemFromObject(item, "_friendly_name");

        /* Merge cached state back into device registry so ESPHome adapter
         * (and other consumers of device_registry_get_state()) can access
         * it immediately after boot without waiting for a Zigbee report. */
        {
            cJSON *state_copy = cJSON_Duplicate(item, true);
            if (state_copy != NULL) {
                esp_err_t merge_ret = device_registry_merge_state(dev->id, state_copy);
                cJSON_Delete(state_copy);
                if (merge_ret == ESP_OK) {
                    ESP_LOGD(TAG, "Restored cached state into registry for %s", friendly_name);

                    /* Notify ESPHome adapter (and other subscribers) so entity
                     * states are updated even if HA already connected and synced
                     * before this load ran.  json_state=NULL tells the adapter
                     * to read the state from the registry. */
                    evt_device_state_t state_evt = {
                        .ieee_addr = dev->id,
                        .short_addr = dev->proto.zigbee.short_addr,
                        .endpoint = dev->proto.zigbee.endpoint,
                        .cluster_id = 0,
                        .attr_id = 0,
                        .json_state = NULL,
                    };
                    event_publish(EVT_DEVICE_STATE_CHANGED, &state_evt, sizeof(state_evt));
                }
            }
        }

        /* Serialize the state object */
        char *state_str = cJSON_PrintUnformatted(item);
        if (state_str == NULL) {
            continue;
        }

        /* Build MQTT topic */
        char topic[MQTT_TOPIC_MAX_LEN];
        esp_err_t pub_ret = mqtt_topic_device_state(friendly_name, topic, sizeof(topic));
        if (pub_ret != ESP_OK) {
            free(state_str);
            continue;
        }

        /* Ownership transfer to event handler:
         * - topic is a stack buffer -> must strdup()
         * - state_str is from cJSON_PrintUnformatted (heap) -> transfer directly
         * The handler frees both via free() after publishing. */
        char *topic_heap = strdup(topic);
        if (topic_heap == NULL) {
            free(state_str);
            continue;
        }

        evt_ha_discovery_publish_t evt = {
            .topic = topic_heap,
            .payload_json = state_str,  /* heap from cJSON_PrintUnformatted, ownership transfers */
            .qos = 0,
            .retain = false
        };
        pub_ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

        if (pub_ret == ESP_OK) {
            /* Ownership transferred - do NOT free topic_heap or state_str */
            published_count++;
            ESP_LOGD(TAG, "Published cached state for %s", friendly_name);
        } else {
            /* event_publish failed - ownership NOT transferred, we must free */
            free(topic_heap);
            free(state_str);
            ESP_LOGW(TAG, "Failed to publish cached state for %s: %s",
                     friendly_name, esp_err_to_name(pub_ret));
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Published cached state for %zu devices", published_count);
    return ESP_OK;
}

void state_persistence_mark_dirty(void)
{
    s_dirty = true;
}

esp_err_t state_persistence_erase_all(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Erasing all persisted state");

    if (remove(STATE_PERSIST_FILE_PATH) != 0) {
        ESP_LOGD(TAG, "State file did not exist or could not be removed");
    }

    /* Also remove temp file if it exists */
    remove(STATE_PERSIST_TEMP_PATH);

    s_dirty = false;
    return ESP_OK;
}

bool state_persistence_is_initialized(void)
{
    return s_initialized;
}

esp_err_t state_persistence_save_immediate(void)
{
    /* This function works even when state_persistence is deinitialized.
     * It's designed for emergency saves during PAIRING phase when devices
     * join but the normal persistence is stopped. */

    ESP_LOGI(TAG, "Emergency save: persisting device states during PAIRING...");

    /* Mount LittleFS (reference counted, so safe even if already mounted) */
    esp_err_t ret = littlefs_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Emergency save: failed to mount LittleFS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Ensure state directory exists */
    ret = ensure_state_dir();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Emergency save: failed to create state directory");
        littlefs_unmount();
        return ret;
    }

    /* Allocate JSON buffer (auto-PSRAM for large allocations) */
    char *buf = (char *)mem_alloc(STATE_PERSIST_MAX_FILE_SIZE, MEM_CAP_PSRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Emergency save: failed to allocate buffer");
        littlefs_unmount();
        return ESP_ERR_NO_MEM;
    }

    /* Build JSON object with all device states */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Emergency save: failed to create JSON object");
        mem_ng_free(buf);
        littlefs_unmount();
        return ESP_ERR_NO_MEM;
    }

    /* Collect device IDs, then build JSON outside registry mutex (AP-1.3) */
    device_id_t ids[DEVICE_REGISTRY_MAX_DEVICES];
    size_t id_count;
    if (device_registry_collect_ids(ids, DEVICE_REGISTRY_MAX_DEVICES, &id_count) != ESP_OK) {
        id_count = 0;
    }

    size_t saved_count = 0;
    for (size_t i = 0; i < id_count; i++) {
        device_t *dev = device_registry_get(ids[i]);
        if (dev == NULL) continue;

        char *state_json = device_state_to_json(dev);
        if (state_json == NULL) continue;

        cJSON *state_obj = cJSON_Parse(state_json);
        free(state_json);
        if (state_obj == NULL) continue;

        char ieee_key[20];
        snprintf(ieee_key, sizeof(ieee_key), "0x%016llX", (unsigned long long)dev->id);
        cJSON_AddStringToObject(state_obj, "_friendly_name", dev->friendly_name);
        cJSON_AddItemToObject(root, ieee_key, state_obj);
        saved_count++;
    }

    /* Serialize to buffer */
    if (!cJSON_PrintPreallocated(root, buf, STATE_PERSIST_MAX_FILE_SIZE, false)) {
        ESP_LOGE(TAG, "Emergency save: JSON serialization failed");
        cJSON_Delete(root);
        mem_ng_free(buf);
        littlefs_unmount();
        return ESP_ERR_NO_MEM;
    }
    cJSON_Delete(root);

    size_t json_len = strlen(buf);

    /* Atomic write: write to temp file then rename */
    FILE *f = fopen(STATE_PERSIST_TEMP_PATH, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Emergency save: failed to open temp file");
        mem_ng_free(buf);
        littlefs_unmount();
        return ESP_FAIL;
    }

    size_t written = fwrite(buf, 1, json_len, f);
    fclose(f);
    mem_ng_free(buf);

    if (written != json_len) {
        ESP_LOGE(TAG, "Emergency save: incomplete write");
        remove(STATE_PERSIST_TEMP_PATH);
        littlefs_unmount();
        return ESP_FAIL;
    }

    /* Rename temp to final (atomic on LittleFS) */
    if (rename(STATE_PERSIST_TEMP_PATH, STATE_PERSIST_FILE_PATH) != 0) {
        ESP_LOGE(TAG, "Emergency save: failed to rename temp file");
        remove(STATE_PERSIST_TEMP_PATH);
        littlefs_unmount();
        return ESP_FAIL;
    }

    /* Unmount LittleFS (reference counted) */
    littlefs_unmount();

    ESP_LOGI(TAG, "Emergency save: persisted %zu device states (%zu bytes)", saved_count, json_len);

    /* Clear dirty flag if initialized - we just saved everything */
    if (s_initialized) {
        s_dirty = false;
    }

    return ESP_OK;
}

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

/**
 * @brief Dedicated save task function
 *
 * Blocks on task notification and performs state persistence save when notified.
 * This avoids running heavy I/O (PSRAM allocation, JSON building, file writes)
 * in the FreeRTOS timer daemon task which has a limited stack.
 */
static void save_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        /* Block until notified by timer callback or shutdown/deinit
         * v6.0 compatible: xTaskNotifyWait replaces deprecated ulTaskNotifyTake */
        uint32_t notification_value;
        xTaskNotifyWait(0, ULONG_MAX, &notification_value, portMAX_DELAY);
        if (s_dirty) {
            state_persistence_save();
        }
        /* Exit loop if deinit requested — after completing any pending save */
        if (s_task_exit_requested) {
            break;
        }
    }
    /* Signal completion ONLY when exiting - this was incorrectly done
     * on every loop iteration before, causing a race condition where
     * deinit would think the task had exited while it was still running. */
    if (s_save_done != NULL) {
        xSemaphoreGive(s_save_done);
    }
    /* Mark self-deleted so psram_task_delete() only frees PSRAM memory */
    psram_task_mark_deleted(&s_save_psram_task);
    vTaskDelete(NULL);
}

/**
 * @brief Timer callback for periodic state saving
 *
 * Only sends a task notification to the dedicated save task instead of
 * performing I/O directly in the timer daemon context.
 */
static void save_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (s_dirty && s_save_psram_task.task_handle != NULL) {
        xTaskNotifyGive(s_save_psram_task.task_handle);
    }
}

/**
 * @brief Shutdown handler for clean save on reboot
 *
 * ESP-IDF shutdown handlers registered via esp_register_shutdown_handler()
 * execute in the context of esp_restart() *before* the FreeRTOS scheduler
 * is stopped. This means heap allocation, mutexes, and file I/O are all
 * still available. Calling state_persistence_save() here is therefore safe.
 */
static void shutdown_handler(void)
{
    if (s_initialized && s_dirty) {
        ESP_LOGI(TAG, "Saving state on shutdown (best-effort)...");
        /* Safe: ESP-IDF shutdown handlers run before scheduler shutdown,
         * so heap allocation and file I/O are still available. */
        state_persistence_save();
    }
}

/**
 * @brief Ensure the state directory exists
 */
static esp_err_t ensure_state_dir(void)
{
    struct stat st;
    if (stat(STATE_PERSIST_DIR, &st) != 0) {
        ESP_LOGI(TAG, "Creating state directory: %s", STATE_PERSIST_DIR);
        if (mkdir(STATE_PERSIST_DIR, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create state directory");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/**
 * @brief Format IEEE address as hex string "0x0011223344556677"
 *
 * Bytes are formatted MSB first (byte[7]..byte[0]).
 * @note Currently unused
 */
__attribute__((unused))
static void format_ieee_addr(const uint8_t *ieee_addr, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "0x%02x%02x%02x%02x%02x%02x%02x%02x",
             ieee_addr[7], ieee_addr[6], ieee_addr[5], ieee_addr[4],
             ieee_addr[3], ieee_addr[2], ieee_addr[1], ieee_addr[0]);
}

