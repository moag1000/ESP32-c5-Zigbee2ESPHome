/**
 * @file esphome_entity_mirror.c
 * @brief Entity state store, independent of the device registry
 *
 * See esphome_entity_mirror.h for why this is not the device registry.
 *
 * Storage is an open-addressed table with linear probing, keyed by the ESPHome
 * entity key. Deletion uses backward shifting rather than tombstones, so a
 * gateway that registers and drops entities over a long uptime does not
 * accumulate dead slots that lengthen every later probe.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_entity_mirror.h"
#include "esphome_entities.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/memory/memory_manager_ng.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "ESPHOME_MIRROR";

/* ============================================================================
 * Table
 * ============================================================================ */

#ifndef CONFIG_ESPHOME_ENTITY_MIRROR_MAX
#define CONFIG_ESPHOME_ENTITY_MIRROR_MAX 128
#endif

/**
 * @brief Load factor ceiling, as a fraction of capacity
 *
 * Linear probing degrades sharply as a table fills; 3/4 keeps the average probe
 * short without wasting much PSRAM.
 */
#define MIRROR_MAX_LOAD_NUM 3
#define MIRROR_MAX_LOAD_DEN 4

typedef struct {
    bool                  in_use;
    esphome_entity_key_t  key;
    esphome_entity_type_t type;
    char                  name[ESPHOME_ENTITY_MIRROR_NAME_LEN];
    cJSON                *state;   /**< Owned by this slot, may be NULL */
} mirror_slot_t;

static bool              s_initialized = false;
static mirror_slot_t    *s_slots       = NULL;
static size_t            s_capacity    = 0;   /**< Always a power of two */
static size_t            s_mask        = 0;   /**< s_capacity - 1 */
static size_t            s_count       = 0;
static SemaphoreHandle_t s_mutex       = NULL;

#define MIRROR_LOCK()   xSemaphoreTake(s_mutex, portMAX_DELAY)
#define MIRROR_UNLOCK() xSemaphoreGive(s_mutex)

/* ============================================================================
 * Hashing and probing
 * ============================================================================ */

/**
 * @brief Spread an entity key across the whole word
 *
 * ESPHome entity keys are themselves hashes of the object id, but the table
 * indexes with the low bits only. Keys that differ mainly in their high bits
 * would collide without this mix. (MurmurHash3 finalizer.)
 */
static inline uint32_t mix_key(esphome_entity_key_t key)
{
    uint32_t h = (uint32_t)key;
    h ^= h >> 16;
    h *= 0x85ebca6bU;
    h ^= h >> 13;
    h *= 0xc2b2ae35U;
    h ^= h >> 16;
    return h;
}

/**
 * @brief Find the slot holding @p key
 *
 * @return Index, or SIZE_MAX if the key is not present.
 * @note Caller holds the lock.
 */
static size_t find_slot(esphome_entity_key_t key)
{
    size_t idx = mix_key(key) & s_mask;
    for (size_t probe = 0; probe < s_capacity; probe++) {
        if (!s_slots[idx].in_use) {
            return SIZE_MAX;    /* Probe sequences never span an empty slot */
        }
        if (s_slots[idx].key == key) {
            return idx;
        }
        idx = (idx + 1) & s_mask;
    }
    return SIZE_MAX;
}

/**
 * @brief Find where @p key would be inserted
 *
 * @return Index of the first free slot in the key's probe sequence, or
 *         SIZE_MAX if the table is full.
 * @note Caller holds the lock, and has already established the key is absent.
 */
static size_t find_free_slot(esphome_entity_key_t key)
{
    size_t idx = mix_key(key) & s_mask;
    for (size_t probe = 0; probe < s_capacity; probe++) {
        if (!s_slots[idx].in_use) {
            return idx;
        }
        idx = (idx + 1) & s_mask;
    }
    return SIZE_MAX;
}

/**
 * @brief Remove slot @p gap, closing the probe sequence behind it
 *
 * Backward-shift deletion: walk forward from the hole and pull back any entry
 * whose home position is at or before the hole, since after clearing the slot
 * a lookup for that entry would otherwise stop at the hole and miss it.
 *
 * @note Caller holds the lock and has already freed the slot's state.
 */
static void remove_slot(size_t gap)
{
    s_slots[gap].in_use = false;

    size_t scan = gap;
    for (size_t probe = 0; probe < s_capacity; probe++) {
        scan = (scan + 1) & s_mask;
        if (!s_slots[scan].in_use) {
            return;
        }

        size_t home = mix_key(s_slots[scan].key) & s_mask;

        /* Is `home` cyclically outside the open interval (gap, scan]? If so the
         * entry belongs at or before the hole and has to move back into it. */
        bool must_move = (gap <= scan)
                       ? (home <= gap || home > scan)
                       : (home <= gap && home > scan);

        if (must_move) {
            s_slots[gap] = s_slots[scan];
            s_slots[scan].in_use = false;
            s_slots[scan].state  = NULL;
            gap = scan;
        }
    }
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

/** @brief Smallest power of two >= @p n, minimum 16 */
static size_t round_up_pow2(size_t n)
{
    size_t v = 16;
    while (v < n) {
        v <<= 1;
    }
    return v;
}

esp_err_t esphome_entity_mirror_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* Give the load-factor ceiling room: a table sized exactly to the
     * configured entity count would refuse the last quarter of them. */
    size_t wanted = ((size_t)CONFIG_ESPHOME_ENTITY_MIRROR_MAX * MIRROR_MAX_LOAD_DEN)
                    / MIRROR_MAX_LOAD_NUM + 1;
    s_capacity = round_up_pow2(wanted);
    s_mask     = s_capacity - 1;

    s_slots = (mirror_slot_t *)mem_ng_calloc(s_capacity, sizeof(mirror_slot_t),
                                             MEM_CAP_PSRAM);
    if (!s_slots) {
        ESP_LOGE(TAG, "Failed to allocate %zu mirror slots (%zu bytes)",
                 s_capacity, s_capacity * sizeof(mirror_slot_t));
        s_capacity = 0;
        s_mask     = 0;
        return ESP_ERR_NO_MEM;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mirror mutex");
        mem_ng_free(s_slots);
        s_slots    = NULL;
        s_capacity = 0;
        s_mask     = 0;
        return ESP_ERR_NO_MEM;
    }

    s_count       = 0;
    s_initialized = true;

    ESP_LOGI(TAG, "Entity mirror ready: %d entities max (%zu slots, %zu bytes PSRAM)",
             CONFIG_ESPHOME_ENTITY_MIRROR_MAX, s_capacity,
             s_capacity * sizeof(mirror_slot_t));
    return ESP_OK;
}

esp_err_t esphome_entity_mirror_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    MIRROR_LOCK();
    for (size_t i = 0; i < s_capacity; i++) {
        if (s_slots[i].in_use && s_slots[i].state) {
            cJSON_Delete(s_slots[i].state);
            s_slots[i].state = NULL;
        }
        s_slots[i].in_use = false;
    }
    s_count = 0;
    MIRROR_UNLOCK();

    /* Clear the flag before freeing so a caller racing us fails the
     * is_initialized() check rather than touching a freed table. */
    s_initialized = false;

    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
    mem_ng_free(s_slots);
    s_slots    = NULL;
    s_capacity = 0;
    s_mask     = 0;

    ESP_LOGI(TAG, "Entity mirror torn down");
    return ESP_OK;
}

bool esphome_entity_mirror_is_initialized(void)
{
    return s_initialized;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

esp_err_t esphome_entity_mirror_register(esphome_entity_type_t entity_type,
                                          esphome_entity_key_t key,
                                          const char *name,
                                          const char *unique_id)
{
    (void)unique_id;    /* The mirror keys on the entity key alone */

    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    MIRROR_LOCK();

    size_t idx = find_slot(key);
    if (idx != SIZE_MAX) {
        /* Already known — refresh the name, the entity layer re-registers on
         * rename. Type may legitimately change if an entity is redefined. */
        s_slots[idx].type = entity_type;
        if (name) {
            strlcpy(s_slots[idx].name, name, sizeof(s_slots[idx].name));
        }
        MIRROR_UNLOCK();
        ESP_LOGD(TAG, "Entity key=%lu already mirrored", (unsigned long)key);
        return ESP_OK;
    }

    if (s_count * MIRROR_MAX_LOAD_DEN >= s_capacity * MIRROR_MAX_LOAD_NUM) {
        MIRROR_UNLOCK();
        ESP_LOGE(TAG, "Entity mirror full (%zu entities), dropping key=%lu '%s'. "
                      "Raise CONFIG_ESPHOME_ENTITY_MIRROR_MAX.",
                 s_count, (unsigned long)key, name ? name : "");
        return ESP_ERR_NO_MEM;
    }

    idx = find_free_slot(key);
    if (idx == SIZE_MAX) {
        MIRROR_UNLOCK();
        ESP_LOGE(TAG, "No free mirror slot for key=%lu", (unsigned long)key);
        return ESP_ERR_NO_MEM;
    }

    s_slots[idx].in_use  = true;
    s_slots[idx].key     = key;
    s_slots[idx].type    = entity_type;
    s_slots[idx].state   = NULL;
    s_slots[idx].name[0] = '\0';
    if (name) {
        strlcpy(s_slots[idx].name, name, sizeof(s_slots[idx].name));
    }
    s_count++;

    size_t count = s_count;
    MIRROR_UNLOCK();

    ESP_LOGI(TAG, "Mirrored ESPHome entity: key=%lu name='%s' type=%d (%zu total)",
             (unsigned long)key, name ? name : "", entity_type, count);
    return ESP_OK;
}

esp_err_t esphome_entity_mirror_unregister(esphome_entity_key_t key)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    MIRROR_LOCK();

    size_t idx = find_slot(key);
    if (idx == SIZE_MAX) {
        MIRROR_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }

    if (s_slots[idx].state) {
        cJSON_Delete(s_slots[idx].state);
        s_slots[idx].state = NULL;
    }
    remove_slot(idx);
    s_count--;

    MIRROR_UNLOCK();
    return ESP_OK;
}

/* ============================================================================
 * State Serialization
 * ============================================================================ */

/**
 * @brief Build the state JSON for one entity
 *
 * @param[in] entity_type Entity type
 * @param[in] state Pointer to the type's state struct
 * @return New cJSON object owned by the caller, or NULL on allocation failure
 */
static cJSON *build_state_json(esphome_entity_type_t entity_type, const void *state)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return NULL;
    }

    switch (entity_type) {
        case ESPHOME_ENTITY_SENSOR: {
            const esphome_sensor_state_t *s = (const esphome_sensor_state_t *)state;
            if (!s->missing_state) {
                cJSON_AddNumberToObject(json, "value", s->state);
            }
            cJSON_AddBoolToObject(json, "available", !s->missing_state);
            break;
        }

        case ESPHOME_ENTITY_BINARY_SENSOR: {
            const esphome_binary_sensor_state_t *s = (const esphome_binary_sensor_state_t *)state;
            if (!s->missing_state) {
                cJSON_AddBoolToObject(json, "state", s->state);
            }
            cJSON_AddBoolToObject(json, "available", !s->missing_state);
            break;
        }

        case ESPHOME_ENTITY_SWITCH: {
            const esphome_switch_state_t *s = (const esphome_switch_state_t *)state;
            cJSON_AddBoolToObject(json, "state", s->state);
            break;
        }

        case ESPHOME_ENTITY_TEXT_SENSOR: {
            const esphome_text_sensor_state_t *s = (const esphome_text_sensor_state_t *)state;
            if (!s->missing_state) {
                cJSON_AddStringToObject(json, "state", s->state);
            }
            cJSON_AddBoolToObject(json, "available", !s->missing_state);
            break;
        }

        case ESPHOME_ENTITY_NUMBER: {
            const esphome_number_state_t *s = (const esphome_number_state_t *)state;
            if (!s->missing_state) {
                cJSON_AddNumberToObject(json, "state", s->state);
            }
            cJSON_AddBoolToObject(json, "available", !s->missing_state);
            break;
        }

        case ESPHOME_ENTITY_SELECT: {
            const esphome_select_state_t *s = (const esphome_select_state_t *)state;
            if (!s->missing_state) {
                cJSON_AddStringToObject(json, "state", s->state);
            }
            cJSON_AddBoolToObject(json, "available", !s->missing_state);
            break;
        }

        case ESPHOME_ENTITY_LIGHT: {
            const esphome_light_state_t *s = (const esphome_light_state_t *)state;
            cJSON_AddBoolToObject(json, "state", s->state);
            cJSON_AddNumberToObject(json, "brightness", s->brightness);
            if (s->color_mode != 0) {
                cJSON_AddNumberToObject(json, "color_temp", s->color_temp);
            }
            break;
        }

        case ESPHOME_ENTITY_COVER: {
            const esphome_cover_state_t *s = (const esphome_cover_state_t *)state;
            cJSON_AddNumberToObject(json, "position", s->position);
            cJSON_AddNumberToObject(json, "tilt", s->tilt);
            cJSON_AddNumberToObject(json, "operation", (int)s->current_operation);
            break;
        }

        case ESPHOME_ENTITY_FAN: {
            const esphome_fan_state_t *s = (const esphome_fan_state_t *)state;
            cJSON_AddBoolToObject(json, "state", s->state);
            cJSON_AddBoolToObject(json, "oscillating", s->oscillating);
            cJSON_AddNumberToObject(json, "speed_level", s->speed_level);
            cJSON_AddNumberToObject(json, "direction", (int)s->direction);
            break;
        }

        case ESPHOME_ENTITY_CLIMATE: {
            const esphome_climate_state_t *s = (const esphome_climate_state_t *)state;
            cJSON_AddNumberToObject(json, "mode", (int)s->mode);
            cJSON_AddNumberToObject(json, "action", (int)s->action);
            cJSON_AddNumberToObject(json, "current_temperature", s->current_temperature);
            cJSON_AddNumberToObject(json, "target_temperature", s->target_temperature);
            cJSON_AddNumberToObject(json, "target_temperature_low", s->target_temperature_low);
            cJSON_AddNumberToObject(json, "target_temperature_high", s->target_temperature_high);
            cJSON_AddNumberToObject(json, "fan_mode", (int)s->fan_mode);
            break;
        }

        case ESPHOME_ENTITY_LOCK: {
            const esphome_lock_entity_state_t *s = (const esphome_lock_entity_state_t *)state;
            cJSON_AddNumberToObject(json, "state", (int)s->state);
            break;
        }

        default:
            /* Unknown entity type - just store empty object */
            break;
    }

    return json;
}

/* ============================================================================
 * State Synchronization
 * ============================================================================ */

esp_err_t esphome_entity_mirror_sync_state(esphome_entity_type_t entity_type,
                                            esphome_entity_key_t key,
                                            const void *state)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build outside the lock: serialization allocates, and the mirror lock is
     * also taken by the MQTT event handler reading states out. */
    cJSON *json = build_state_json(entity_type, state);
    if (!json) {
        ESP_LOGE(TAG, "Failed to build state JSON for key=%lu", (unsigned long)key);
        return ESP_ERR_NO_MEM;
    }

    MIRROR_LOCK();

    size_t idx = find_slot(key);
    if (idx == SIZE_MAX) {
        MIRROR_UNLOCK();
        cJSON_Delete(json);
        ESP_LOGD(TAG, "Entity key=%lu not mirrored, skipping sync", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_slots[idx].state) {
        cJSON_Delete(s_slots[idx].state);
    }
    s_slots[idx].state = json;      /* Slot owns it now */

    MIRROR_UNLOCK();

    /* Announce the change. The payload carries the key only — a subscriber runs
     * on the dispatcher task and reads the state back with _get(), which copies
     * under the lock. Handing out the pointer instead would race this function
     * replacing it on the next update. */
    evt_esphome_entity_state_t evt = {
        .key         = key,
        .entity_type = (uint8_t)entity_type,
    };

    esp_err_t ret = event_publish(EVT_ESPHOME_ENTITY_STATE, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        /* State is stored; only the notification was lost. */
        ESP_LOGD(TAG, "Failed to publish state event for key=%lu: %s",
                 (unsigned long)key, esp_err_to_name(ret));
    }

    ESP_LOGD(TAG, "Synced state for ESPHome entity key=%lu type=%d",
             (unsigned long)key, entity_type);
    return ESP_OK;
}

esp_err_t esphome_entity_mirror_get(esphome_entity_key_t key,
                                     char *name_out, size_t name_len,
                                     cJSON **state_out)
{
    if (state_out) {
        *state_out = NULL;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    MIRROR_LOCK();

    size_t idx = find_slot(key);
    if (idx == SIZE_MAX) {
        MIRROR_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }

    if (name_out && name_len > 0) {
        strlcpy(name_out, s_slots[idx].name, name_len);
    }
    if (state_out && s_slots[idx].state) {
        *state_out = cJSON_Duplicate(s_slots[idx].state, true);
    }

    MIRROR_UNLOCK();
    return ESP_OK;
}

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

size_t esphome_entity_mirror_count(void)
{
    if (!s_initialized) {
        return 0;
    }
    MIRROR_LOCK();
    size_t count = s_count;
    MIRROR_UNLOCK();
    return count;
}

size_t esphome_entity_mirror_capacity(void)
{
    return s_initialized ? (size_t)CONFIG_ESPHOME_ENTITY_MIRROR_MAX : 0;
}

void esphome_entity_mirror_log_stats(void)
{
    if (!s_initialized) {
        ESP_LOGI(TAG, "Entity mirror: not initialized");
        return;
    }

    size_t count = esphome_entity_mirror_count();
    ESP_LOGI(TAG, "Entity mirror: %zu/%d entities (%zu slots)",
             count, CONFIG_ESPHOME_ENTITY_MIRROR_MAX, s_capacity);
}
