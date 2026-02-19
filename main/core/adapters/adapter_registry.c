/**
 * @file adapter_registry.c
 * @brief Adapter registry implementation
 *
 * Static registry for protocol adapters. Manages lifecycle (init/start/stop/deinit)
 * in registration order (start) and reverse order (stop/deinit).
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "adapter_interface.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ADAPTER_REG";

static adapter_desc_t s_adapters[ADAPTER_MAX_COUNT];
static size_t s_count = 0;

esp_err_t adapter_registry_register(const char *name, const adapter_ops_t *ops)
{
    if (name == NULL || ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_count >= ADAPTER_MAX_COUNT) {
        ESP_LOGE(TAG, "Registry full (%d/%d), cannot register '%s'",
                 (int)s_count, ADAPTER_MAX_COUNT, name);
        return ESP_ERR_NO_MEM;
    }

    s_adapters[s_count] = (adapter_desc_t){
        .name = name,
        .ops = ops,
        .initialized = false,
        .started = false,
    };
    s_count++;

    ESP_LOGD(TAG, "Registered adapter '%s' (slot %zu)", name, s_count - 1);
    return ESP_OK;
}

esp_err_t adapter_registry_init_all(void)
{
    esp_err_t first_error = ESP_OK;

    for (size_t i = 0; i < s_count; i++) {
        adapter_desc_t *ad = &s_adapters[i];
        if (ad->ops->init == NULL) {
            ESP_LOGD(TAG, "Adapter '%s' has no init — skipping", ad->name);
            continue;
        }

        ESP_LOGI(TAG, "Initializing adapter '%s'...", ad->name);
        esp_err_t err = ad->ops->init();
        if (err == ESP_OK) {
            ad->initialized = true;
            ESP_LOGI(TAG, "Adapter '%s' initialized", ad->name);
        } else {
            ESP_LOGW(TAG, "Adapter '%s' init failed: %s", ad->name, esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }

    return first_error;
}

esp_err_t adapter_registry_start_all(void)
{
    esp_err_t first_error = ESP_OK;

    for (size_t i = 0; i < s_count; i++) {
        adapter_desc_t *ad = &s_adapters[i];
        if (!ad->initialized || ad->ops->start == NULL) {
            continue;
        }

        ESP_LOGI(TAG, "Starting adapter '%s'...", ad->name);
        esp_err_t err = ad->ops->start();
        if (err == ESP_OK) {
            ad->started = true;
            ESP_LOGI(TAG, "Adapter '%s' started", ad->name);
        } else {
            ESP_LOGW(TAG, "Adapter '%s' start failed: %s", ad->name, esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }

    return first_error;
}

esp_err_t adapter_registry_stop_all(void)
{
    esp_err_t first_error = ESP_OK;

    /* Stop in reverse order */
    for (size_t i = s_count; i > 0; i--) {
        adapter_desc_t *ad = &s_adapters[i - 1];
        if (!ad->started || ad->ops->stop == NULL) {
            continue;
        }

        ESP_LOGI(TAG, "Stopping adapter '%s'...", ad->name);
        esp_err_t err = ad->ops->stop();
        if (err == ESP_OK) {
            ad->started = false;
            ESP_LOGI(TAG, "Adapter '%s' stopped", ad->name);
        } else {
            ESP_LOGW(TAG, "Adapter '%s' stop failed: %s", ad->name, esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }

    return first_error;
}

esp_err_t adapter_registry_deinit_all(void)
{
    esp_err_t first_error = ESP_OK;

    /* Deinit in reverse order */
    for (size_t i = s_count; i > 0; i--) {
        adapter_desc_t *ad = &s_adapters[i - 1];
        if (!ad->initialized || ad->ops->deinit == NULL) {
            continue;
        }

        ESP_LOGI(TAG, "Deinitializing adapter '%s'...", ad->name);
        esp_err_t err = ad->ops->deinit();
        if (err == ESP_OK) {
            ad->initialized = false;
            ESP_LOGI(TAG, "Adapter '%s' deinitialized", ad->name);
        } else {
            ESP_LOGW(TAG, "Adapter '%s' deinit failed: %s", ad->name, esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }

    return first_error;
}

size_t adapter_registry_count(void)
{
    return s_count;
}

const adapter_desc_t *adapter_registry_get(size_t index)
{
    if (index >= s_count) {
        return NULL;
    }
    return &s_adapters[index];
}

void adapter_registry_log_status(void)
{
    ESP_LOGI(TAG, "Adapter Registry Status (%zu registered):", s_count);
    for (size_t i = 0; i < s_count; i++) {
        const adapter_desc_t *ad = &s_adapters[i];
        bool running = (ad->ops->is_running != NULL) ? ad->ops->is_running() : false;
        ESP_LOGI(TAG, "  [%zu] %-20s init=%c start=%c running=%c",
                 i, ad->name,
                 ad->initialized ? 'Y' : 'N',
                 ad->started ? 'Y' : 'N',
                 running ? 'Y' : 'N');
    }
}
