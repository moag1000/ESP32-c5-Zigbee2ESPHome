/**
 * @file module_manager.c
 * @brief Dynamic Module Loading/Unloading Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "module_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "MODULE_MGR";

/* ============================================================================
 * Private Types
 * ============================================================================ */

/**
 * @brief Internal module entry with registration state
 */
typedef struct {
    module_def_t def;           /**< Module definition (copy) */
    bool registered;            /**< Entry is in use */
} module_entry_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Module registry */
static module_entry_t s_modules[MODULE_MAX_COUNT];

/** @brief Number of registered modules */
static size_t s_module_count = 0;

/** @brief Mutex for thread safety */
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Memory threshold for auto-unload */
static size_t s_memory_threshold = MODULE_DEFAULT_MEM_THRESHOLD;

/** @brief Initialization flag */
static bool s_initialized = false;

/* ============================================================================
 * Private Functions
 * ============================================================================ */

/**
 * @brief Find module entry by name (internal, no lock)
 * @param name Module name
 * @return Pointer to module entry, or NULL if not found
 */
static module_entry_t *find_module_by_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < MODULE_MAX_COUNT; i++) {
        if (s_modules[i].registered &&
            s_modules[i].def.name != NULL &&
            strcmp(s_modules[i].def.name, name) == 0) {
            return &s_modules[i];
        }
    }

    return NULL;
}

/**
 * @brief Find empty slot in registry
 * @return Pointer to empty entry, or NULL if full
 */
static module_entry_t *find_empty_slot(void)
{
    for (size_t i = 0; i < MODULE_MAX_COUNT; i++) {
        if (!s_modules[i].registered) {
            return &s_modules[i];
        }
    }
    return NULL;
}

/**
 * @brief Get current free internal heap
 * @return Free internal heap in bytes
 */
static size_t get_free_heap(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/**
 * @brief Lock the module manager mutex
 * @return true if lock acquired, false on timeout
 */
static bool lock_acquire(void)
{
    if (s_mutex == NULL) {
        return false;
    }
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

/**
 * @brief Release the module manager mutex
 */
static void lock_release(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

/**
 * @brief Validate module definition
 * @param def Module definition
 * @return true if valid, false otherwise
 */
static bool validate_module_def(const module_def_t *def)
{
    if (def == NULL) {
        ESP_LOGE(TAG, "Module definition is NULL");
        return false;
    }

    if (def->name == NULL || strlen(def->name) == 0) {
        ESP_LOGE(TAG, "Module name is required");
        return false;
    }

    if (strlen(def->name) >= MODULE_NAME_MAX_LEN) {
        ESP_LOGE(TAG, "Module name too long: %s", def->name);
        return false;
    }

    if (def->load == NULL) {
        ESP_LOGE(TAG, "Module %s: load callback is required", def->name);
        return false;
    }

    if (def->unload == NULL && !def->essential) {
        ESP_LOGE(TAG, "Module %s: unload callback required for non-essential modules",
                 def->name);
        return false;
    }

    return true;
}

/**
 * @brief Compare modules by priority for sorting (lower priority first)
 * @note Currently unused - reserved for future qsort-based sorting
 */
__attribute__((unused))
static int compare_by_priority(const void *a, const void *b)
{
    const module_entry_t *ma = *(const module_entry_t **)a;
    const module_entry_t *mb = *(const module_entry_t **)b;
    return (int)ma->def.priority - (int)mb->def.priority;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t module_manager_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* Clear registry */
    memset(s_modules, 0, sizeof(s_modules));
    s_module_count = 0;
    s_memory_threshold = MODULE_DEFAULT_MEM_THRESHOLD;

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized (max modules: %d, mem threshold: %zu bytes)",
             MODULE_MAX_COUNT, s_memory_threshold);

    return ESP_OK;
}

void module_manager_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    if (!lock_acquire()) {
        ESP_LOGE(TAG, "Failed to acquire lock for deinit");
        return;
    }

    /* Unload all loaded modules */
    for (size_t i = 0; i < MODULE_MAX_COUNT; i++) {
        if (s_modules[i].registered &&
            s_modules[i].def.state == MODULE_LOADED) {

            /* Skip essential modules during deinit - they handle their own cleanup */
            if (s_modules[i].def.essential) {
                ESP_LOGW(TAG, "Skipping essential module: %s", s_modules[i].def.name);
                continue;
            }

            if (s_modules[i].def.unload != NULL) {
                ESP_LOGI(TAG, "Unloading module: %s", s_modules[i].def.name);
                s_modules[i].def.unload();
            }
            s_modules[i].def.state = MODULE_UNLOADED;
        }
    }

    /* Clear registry */
    memset(s_modules, 0, sizeof(s_modules));
    s_module_count = 0;

    lock_release();

    /* Delete mutex */
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
}

esp_err_t module_register(const module_def_t *def)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!validate_module_def(def)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lock_acquire()) {
        ESP_LOGE(TAG, "Failed to acquire lock");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Check for duplicate */
    if (find_module_by_name(def->name) != NULL) {
        ESP_LOGE(TAG, "Module already registered: %s", def->name);
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    /* Find empty slot */
    module_entry_t *entry = find_empty_slot();
    if (entry == NULL) {
        ESP_LOGE(TAG, "Registry full (max %d modules)", MODULE_MAX_COUNT);
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }

    /* Copy module definition */
    memcpy(&entry->def, def, sizeof(module_def_t));
    entry->def.state = MODULE_UNLOADED;
    entry->registered = true;
    s_module_count++;

    ESP_LOGI(TAG, "Registered module: %s (heap: %zu, priority: %u, essential: %s)",
             def->name, def->heap_required, def->priority,
             def->essential ? "yes" : "no");

exit:
    lock_release();
    return ret;
}

esp_err_t module_unregister(const char *name)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lock_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    module_entry_t *entry = find_module_by_name(name);
    if (entry == NULL) {
        ESP_LOGE(TAG, "Module not found: %s", name);
        ret = ESP_ERR_NOT_FOUND;
        goto exit;
    }

    if (entry->def.state == MODULE_LOADED ||
        entry->def.state == MODULE_LOADING) {
        ESP_LOGE(TAG, "Cannot unregister loaded module: %s", name);
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    entry->registered = false;
    memset(&entry->def, 0, sizeof(module_def_t));
    s_module_count--;

    ESP_LOGI(TAG, "Unregistered module: %s", name);

exit:
    lock_release();
    return ret;
}

esp_err_t module_load(const char *name)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lock_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    module_entry_t *entry = find_module_by_name(name);
    if (entry == NULL) {
        ESP_LOGE(TAG, "Module not found: %s", name);
        ret = ESP_ERR_NOT_FOUND;
        goto exit;
    }

    /* Check current state */
    if (entry->def.state == MODULE_LOADED) {
        ESP_LOGD(TAG, "Module already loaded: %s", name);
        ret = ESP_OK;
        goto exit;
    }

    if (entry->def.state == MODULE_LOADING ||
        entry->def.state == MODULE_UNLOADING) {
        ESP_LOGE(TAG, "Module in transition: %s (state: %d)",
                 name, entry->def.state);
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    /* Check heap availability */
    size_t free_heap = get_free_heap();
    if (entry->def.heap_required > 0 && free_heap < entry->def.heap_required) {
        ESP_LOGE(TAG, "Insufficient heap for %s (need: %zu, free: %zu)",
                 name, entry->def.heap_required, free_heap);
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }

    /* Start loading */
    entry->def.state = MODULE_LOADING;
    ESP_LOGI(TAG, "Loading module: %s (heap before: %zu)", name, free_heap);

    /* Release lock during load callback (it may take time) */
    lock_release();

    /* Call load callback */
    ret = entry->def.load();

    /* Re-acquire lock to update state */
    if (!lock_acquire()) {
        ESP_LOGE(TAG, "Failed to re-acquire lock after load");
        /* Module state may be inconsistent */
        return ESP_ERR_TIMEOUT;
    }

    if (ret == ESP_OK) {
        entry->def.state = MODULE_LOADED;
        size_t heap_after = get_free_heap();
        entry->def.heap_usage = (free_heap > heap_after) ?
                                 (free_heap - heap_after) : 0;
        ESP_LOGI(TAG, "Module loaded: %s (heap used: %zu, free: %zu)",
                 name, entry->def.heap_usage, heap_after);
    } else {
        entry->def.state = MODULE_ERROR;
        ESP_LOGE(TAG, "Module load failed: %s (error: 0x%x)", name, ret);
    }

exit:
    lock_release();
    return ret;
}

esp_err_t module_unload(const char *name)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lock_acquire()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    module_entry_t *entry = find_module_by_name(name);
    if (entry == NULL) {
        ESP_LOGE(TAG, "Module not found: %s", name);
        ret = ESP_ERR_NOT_FOUND;
        goto exit;
    }

    /* Check if essential */
    if (entry->def.essential) {
        ESP_LOGE(TAG, "Cannot unload essential module: %s", name);
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    /* Check current state */
    if (entry->def.state == MODULE_UNLOADED) {
        ESP_LOGD(TAG, "Module already unloaded: %s", name);
        ret = ESP_OK;
        goto exit;
    }

    if (entry->def.state != MODULE_LOADED) {
        ESP_LOGE(TAG, "Module not in loaded state: %s (state: %d)",
                 name, entry->def.state);
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    /* Check can_unload callback */
    if (entry->def.can_unload != NULL && !entry->def.can_unload()) {
        ESP_LOGW(TAG, "Module cannot be unloaded now: %s", name);
        ret = ESP_ERR_NOT_ALLOWED;
        goto exit;
    }

    /* Start unloading */
    entry->def.state = MODULE_UNLOADING;
    size_t heap_before = get_free_heap();
    ESP_LOGI(TAG, "Unloading module: %s (heap before: %zu)", name, heap_before);

    /* Release lock during unload callback */
    lock_release();

    /* Call unload callback */
    if (entry->def.unload != NULL) {
        ret = entry->def.unload();
    }

    /* Re-acquire lock to update state */
    if (!lock_acquire()) {
        ESP_LOGE(TAG, "Failed to re-acquire lock after unload");
        return ESP_ERR_TIMEOUT;
    }

    if (ret == ESP_OK) {
        entry->def.state = MODULE_UNLOADED;
        entry->def.heap_usage = 0;
        size_t heap_after = get_free_heap();
        ESP_LOGI(TAG, "Module unloaded: %s (heap freed: %zu, free: %zu)",
                 name,
                 (heap_after > heap_before) ? (heap_after - heap_before) : 0,
                 heap_after);
    } else {
        entry->def.state = MODULE_ERROR;
        ESP_LOGE(TAG, "Module unload failed: %s (error: 0x%x)", name, ret);
    }

exit:
    lock_release();
    return ret;
}

esp_err_t module_ensure_loaded(const char *name)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* First check state without heavy lock operations */
    module_state_t state = module_get_state(name);

    if (state == MODULE_LOADED) {
        return ESP_OK;
    }

    if (state == MODULE_ERROR) {
        /* Find module to check if it exists */
        if (!lock_acquire()) {
            return ESP_ERR_TIMEOUT;
        }
        module_entry_t *entry = find_module_by_name(name);
        lock_release();

        if (entry == NULL) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* Load the module */
    return module_load(name);
}

module_state_t module_get_state(const char *name)
{
    if (!s_initialized || name == NULL) {
        return MODULE_ERROR;
    }

    if (!lock_acquire()) {
        return MODULE_ERROR;
    }

    module_state_t state = MODULE_ERROR;
    module_entry_t *entry = find_module_by_name(name);
    if (entry != NULL) {
        state = entry->def.state;
    }

    lock_release();
    return state;
}

void module_manager_set_memory_threshold(size_t min_free_heap)
{
    if (!s_initialized) {
        return;
    }

    if (!lock_acquire()) {
        return;
    }

    s_memory_threshold = min_free_heap;
    ESP_LOGI(TAG, "Memory threshold set to %zu bytes", s_memory_threshold);

    lock_release();
}

size_t module_manager_get_memory_threshold(void)
{
    return s_memory_threshold;
}

size_t module_get_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    if (!lock_acquire()) {
        return 0;
    }

    size_t count = s_module_count;
    lock_release();
    return count;
}

size_t module_get_loaded_count(void)
{
    if (!s_initialized) {
        return 0;
    }

    if (!lock_acquire()) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < MODULE_MAX_COUNT; i++) {
        if (s_modules[i].registered &&
            s_modules[i].def.state == MODULE_LOADED) {
            count++;
        }
    }

    lock_release();
    return count;
}

const module_def_t *module_get_def(const char *name)
{
    if (!s_initialized || name == NULL) {
        return NULL;
    }

    if (!lock_acquire()) {
        return NULL;
    }

    const module_def_t *def = NULL;
    module_entry_t *entry = find_module_by_name(name);
    if (entry != NULL) {
        def = &entry->def;
    }

    lock_release();
    return def;
}

void module_iterate(module_iterator_fn callback, void *user_ctx)
{
    if (!s_initialized || callback == NULL) {
        return;
    }

    if (!lock_acquire()) {
        return;
    }

    for (size_t i = 0; i < MODULE_MAX_COUNT; i++) {
        if (s_modules[i].registered) {
            if (!callback(&s_modules[i].def, user_ctx)) {
                break;  /* Callback requested stop */
            }
        }
    }

    lock_release();
}

size_t module_find_unloadable(const char **names, size_t max_count)
{
    if (!s_initialized || names == NULL || max_count == 0) {
        return 0;
    }

    if (!lock_acquire()) {
        return 0;
    }

    /* Collect unloadable modules */
    module_entry_t *unloadable[MODULE_MAX_COUNT];
    size_t count = 0;

    for (size_t i = 0; i < MODULE_MAX_COUNT && count < MODULE_MAX_COUNT; i++) {
        if (!s_modules[i].registered) {
            continue;
        }

        module_entry_t *entry = &s_modules[i];

        /* Skip essential modules */
        if (entry->def.essential) {
            continue;
        }

        /* Skip modules not loaded */
        if (entry->def.state != MODULE_LOADED) {
            continue;
        }

        /* Check can_unload callback */
        if (entry->def.can_unload != NULL && !entry->def.can_unload()) {
            continue;
        }

        unloadable[count++] = entry;
    }

    /* Sort by priority (lowest first) */
    if (count > 1) {
        /* Simple bubble sort for small array */
        for (size_t i = 0; i < count - 1; i++) {
            for (size_t j = 0; j < count - i - 1; j++) {
                if (unloadable[j]->def.priority > unloadable[j + 1]->def.priority) {
                    module_entry_t *tmp = unloadable[j];
                    unloadable[j] = unloadable[j + 1];
                    unloadable[j + 1] = tmp;
                }
            }
        }
    }

    /* Copy names to output */
    size_t result_count = (count < max_count) ? count : max_count;
    for (size_t i = 0; i < result_count; i++) {
        names[i] = unloadable[i]->def.name;
    }

    lock_release();
    return result_count;
}

size_t module_unload_by_priority(void)
{
    if (!s_initialized) {
        return 0;
    }

    size_t unloaded_count = 0;
    const char *names[MODULE_MAX_COUNT];

    size_t count = module_find_unloadable(names, MODULE_MAX_COUNT);
    if (count == 0) {
        ESP_LOGD(TAG, "No modules available for unloading");
        return 0;
    }

    ESP_LOGI(TAG, "Starting priority-based unload (threshold: %zu, free: %zu)",
             s_memory_threshold, get_free_heap());

    for (size_t i = 0; i < count; i++) {
        /* Check if we've reached threshold */
        if (get_free_heap() >= s_memory_threshold) {
            ESP_LOGI(TAG, "Memory threshold reached, stopping unload");
            break;
        }

        esp_err_t ret = module_unload(names[i]);
        if (ret == ESP_OK) {
            unloaded_count++;
            ESP_LOGI(TAG, "Unloaded module: %s (free heap: %zu)",
                     names[i], get_free_heap());
        } else {
            ESP_LOGW(TAG, "Failed to unload module: %s (error: 0x%x)",
                     names[i], ret);
        }
    }

    ESP_LOGI(TAG, "Priority-based unload complete: %zu modules unloaded",
             unloaded_count);

    return unloaded_count;
}

void module_print_status(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return;
    }

    if (!lock_acquire()) {
        return;
    }

    ESP_LOGI(TAG, "=== Module Status ===");
    ESP_LOGI(TAG, "Registered: %zu / %d", s_module_count, MODULE_MAX_COUNT);
    ESP_LOGI(TAG, "Memory threshold: %zu bytes", s_memory_threshold);
    ESP_LOGI(TAG, "Free heap: %zu bytes", get_free_heap());
    ESP_LOGI(TAG, "");

    static const char *state_names[] = {
        "UNLOADED", "LOADING", "LOADED", "UNLOADING", "ERROR"
    };

    for (size_t i = 0; i < MODULE_MAX_COUNT; i++) {
        if (!s_modules[i].registered) {
            continue;
        }

        const module_def_t *def = &s_modules[i].def;
        const char *state_str = (def->state < sizeof(state_names) / sizeof(state_names[0]))
                                ? state_names[def->state] : "UNKNOWN";

        ESP_LOGI(TAG, "  [%s] %s", state_str, def->name);
        ESP_LOGI(TAG, "    heap_required: %zu, heap_usage: %zu, priority: %u%s",
                 def->heap_required, def->heap_usage, def->priority,
                 def->essential ? " (essential)" : "");
    }

    ESP_LOGI(TAG, "=====================");

    lock_release();
}
