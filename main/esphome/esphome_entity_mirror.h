/**
 * @file esphome_entity_mirror.h
 * @brief Last known state of every ESPHome entity, stored on its own
 *
 * Entity state used to live in the unified device_registry: one virtual device
 * per entity, keyed by a device_id derived from the entity key. That coupled
 * two capacities that have nothing to do with each other. Measured on a gateway
 * with two paired Zigbee devices, the moment Home Assistant connected:
 *
 *     registry = 56/64 (87%)   zigbee devices = 2
 *
 * 54 of those slots were entities. The registry could not hold the devices it
 * exists for, and CONFIG_MAX_ZIGBEE_DEVICES=50 was unreachable by construction.
 *
 * This module keeps the same information in its own table. The device registry
 * holds devices again, and entity count no longer competes with device count.
 *
 * Consumers do not get a device_t for an entity — there isn't one. State
 * changes are announced as EVT_ESPHOME_ENTITY_STATE carrying the entity key,
 * and the payload is fetched with esphome_entity_mirror_get().
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_ENTITY_MIRROR_H
#define ESPHOME_ENTITY_MIRROR_H

#include "sdkconfig.h"
#include "esphome_common.h"
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !CONFIG_ESPHOME_ENTITY_MIRROR

/*
 * Mirror compiled out (CONFIG_ESPHOME_ENTITY_MIRROR=n, the default under
 * ESPHome primary — see the Kconfig help). esphome_entity_mirror.c is not built
 * in that case, so the entity layer would not link against it.
 *
 * Rather than wrap every call site in #if, the calls stay unconditional and
 * resolve to these no-ops. is_initialized() returning false is what the call
 * sites already check, so the behaviour is the same as a mirror that failed to
 * start: entities work over the ESPHome API, they just are not mirrored.
 */

#define ESPHOME_ENTITY_MIRROR_NAME_LEN 32

static inline esp_err_t esphome_entity_mirror_init(void) { return ESP_OK; }
static inline esp_err_t esphome_entity_mirror_deinit(void) { return ESP_OK; }
static inline bool esphome_entity_mirror_is_initialized(void) { return false; }

static inline esp_err_t esphome_entity_mirror_register(esphome_entity_type_t entity_type,
                                                        esphome_entity_key_t key,
                                                        const char *name,
                                                        const char *unique_id)
{
    (void)entity_type; (void)key; (void)name; (void)unique_id;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t esphome_entity_mirror_unregister(esphome_entity_key_t key)
{
    (void)key;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t esphome_entity_mirror_sync_state(esphome_entity_type_t entity_type,
                                                          esphome_entity_key_t key,
                                                          const void *state)
{
    (void)entity_type; (void)key; (void)state;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline size_t esphome_entity_mirror_count(void) { return 0; }
static inline size_t esphome_entity_mirror_capacity(void) { return 0; }
static inline void esphome_entity_mirror_log_stats(void) { }

#else /* CONFIG_ESPHOME_ENTITY_MIRROR */

/**
 * @brief Entity name buffer size, including the terminator
 *
 * Deliberately the same 32 bytes that device_t::friendly_name gave the old
 * registry-backed mirror. MQTT state topics are built from this name, so
 * widening it would rename any topic whose entity name used to be truncated.
 */
#define ESPHOME_ENTITY_MIRROR_NAME_LEN 32

/**
 * @brief Bring up the entity mirror
 *
 * Allocates the table in PSRAM. Sized by CONFIG_ESPHOME_ENTITY_MIRROR_MAX,
 * rounded up to a power of two.
 *
 * @return ESP_OK, ESP_ERR_NO_MEM if the table or its mutex cannot be allocated
 */
esp_err_t esphome_entity_mirror_init(void);

/**
 * @brief Tear down the mirror and free every stored state object
 */
esp_err_t esphome_entity_mirror_deinit(void);

/**
 * @brief Whether init() has run and the table is usable
 */
bool esphome_entity_mirror_is_initialized(void);

/**
 * @brief Take an entity into the mirror
 *
 * Registering a key that is already present updates its name and returns
 * ESP_OK — re-registration is how the entity layer reports a renamed entity,
 * not an error.
 *
 * @param[in] entity_type Entity type, decides how state is serialized later
 * @param[in] key         ESPHome entity key
 * @param[in] name        Display name, truncated to ESPHOME_ENTITY_MIRROR_NAME_LEN
 * @param[in] unique_id   Unused, accepted for call-site compatibility; may be NULL
 * @return ESP_OK, ESP_ERR_NO_MEM when the table is full,
 *         ESP_ERR_INVALID_STATE when not initialized
 */
esp_err_t esphome_entity_mirror_register(esphome_entity_type_t entity_type,
                                          esphome_entity_key_t key,
                                          const char *name,
                                          const char *unique_id);

/**
 * @brief Drop an entity and free its stored state
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND if the key was never registered
 */
esp_err_t esphome_entity_mirror_unregister(esphome_entity_key_t key);

/**
 * @brief Record a new state for an entity and announce it
 *
 * Serializes @p state according to @p entity_type, replaces whatever the mirror
 * held for @p key, then publishes EVT_ESPHOME_ENTITY_STATE.
 *
 * @param[in] entity_type Entity type
 * @param[in] key         ESPHome entity key
 * @param[in] state       Pointer to the type's state struct
 * @return ESP_OK, ESP_ERR_NOT_FOUND if the key is not registered
 */
esp_err_t esphome_entity_mirror_sync_state(esphome_entity_type_t entity_type,
                                            esphome_entity_key_t key,
                                            const void *state);

/**
 * @brief Read an entity's name and last state
 *
 * Both outputs are copies taken under the mirror's lock, so the caller is safe
 * against a concurrent sync_state() replacing the entry.
 *
 * @param[in]  key       Entity key
 * @param[out] name_out  Name buffer, may be NULL if the name is not wanted
 * @param[in]  name_len  Size of @p name_out
 * @param[out] state_out Receives an owned cJSON object the caller must
 *                       cJSON_Delete(), or NULL if the entity has no state yet.
 *                       May be NULL if only the name is wanted.
 * @return ESP_OK, ESP_ERR_NOT_FOUND if the key is not registered
 */
esp_err_t esphome_entity_mirror_get(esphome_entity_key_t key,
                                     char *name_out, size_t name_len,
                                     cJSON **state_out);

/**
 * @brief Number of entities currently mirrored
 */
size_t esphome_entity_mirror_count(void);

/**
 * @brief Table capacity in entities
 */
size_t esphome_entity_mirror_capacity(void);

/**
 * @brief Log fill level and capacity
 */
void esphome_entity_mirror_log_stats(void);

#endif /* CONFIG_ESPHOME_ENTITY_MIRROR */

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ENTITY_MIRROR_H */
