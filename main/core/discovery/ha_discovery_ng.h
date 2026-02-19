/**
 * @file ha_discovery_ng.h
 * @brief Next-Generation Home Assistant MQTT Discovery
 *
 * Template-based discovery system that uses device capabilities (DEV_CAP_*)
 * to automatically generate appropriate HA discovery messages. This approach
 * eliminates hardcoded entity types and enables automatic discovery based on
 * the unified device model.
 *
 * Features:
 *   - Capability-driven entity generation
 *   - Template table for sensors, binary sensors, and actuators
 *   - Event-driven discovery (subscribes to device events)
 *   - Zigbee2MQTT compatible discovery format
 *   - Supports unified device model (device_t)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef HA_DISCOVERY_NG_H
#define HA_DISCOVERY_NG_H

#include <stdint.h>
#include "esp_err.h"
#include "core/device/unified_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief Number of devices to publish per batch before delay */
#define HA_DISCOVERY_NG_BATCH_SIZE          3

/** @brief Delay between batches to let TCP drain (WiFi/Zigbee coexistence) */
#define HA_DISCOVERY_NG_BATCH_DELAY_MS      200

/** @brief Maximum number of devices in discovery cache */
#define DISCOVERY_CACHE_SIZE                64

/** @brief Cache entry validity period in seconds (1 hour) */
#define DISCOVERY_CACHE_TTL_SEC             3600

/* ============================================================================
 * Discovery Cache Structure
 * ============================================================================ */

/**
 * @brief Discovery cache entry
 *
 * Tracks which devices have had discovery published and their
 * capabilities hash to prevent redundant MQTT publishes.
 */
typedef struct {
    device_id_t id;             /**< Device identifier (0 = empty slot) */
    uint32_t capabilities_hash; /**< FNV-1a hash of published capabilities */
    uint32_t last_published;    /**< Epoch timestamp of last publish */
} discovery_cache_entry_t;

/* ============================================================================
 * Entity Template Structure
 * ============================================================================ */

/**
 * @brief Home Assistant entity template
 *
 * Defines how a device capability maps to a Home Assistant entity.
 * Used to auto-generate discovery messages based on device capabilities.
 */
typedef struct {
    const char *component;      /**< HA component type: "sensor", "binary_sensor", "light", etc. */
    const char *device_class;   /**< HA device class: "temperature", "humidity", "motion", etc. */
    const char *unit;           /**< Unit of measurement: "°C", "%", "W", etc. (NULL = none) */
    const char *icon;           /**< MDI icon: "mdi:thermometer", etc. (NULL = use default) */
    const char *value_template; /**< Jinja2 template for value extraction */
    const char *state_key;      /**< JSON key in state message (e.g., "temperature") */
    const char *state_class;    /**< HA state class: "measurement", "total_increasing", etc. */
    uint32_t required_caps;     /**< DEV_CAP_* bitmask required for this entity */
    bool is_actuator;           /**< True if this is an actuator (light, switch, etc.) */
} ha_entity_template_t;

/* ============================================================================
 * Initialization / Deinitialization
 * ============================================================================ */

/**
 * @brief Initialize the NG Home Assistant discovery system
 *
 * Subscribes to device events (EVT_DEVICE_JOINED, EVT_DEVICE_INTERVIEWED)
 * for automatic discovery publication.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized
 *      - ESP_FAIL on subscription failure
 */
esp_err_t ha_discovery_ng_init(void);

/**
 * @brief Deinitialize the NG Home Assistant discovery system
 *
 * Unsubscribes from device events and cleans up resources.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ha_discovery_ng_deinit(void);

/* ============================================================================
 * Discovery Publication
 * ============================================================================ */

/**
 * @brief Publish discovery for a device based on its capabilities
 *
 * Iterates through entity templates and publishes discovery messages
 * for each template where the device has the required capabilities.
 *
 * @param[in] id Device identifier (device_id_t)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_INVALID_STATE if MQTT not connected
 *      - ESP_FAIL on publish failure
 */
esp_err_t ha_discovery_ng_publish_device(device_id_t id);

/**
 * @brief Publish discovery for all devices in the registry
 *
 * Iterates through all devices and publishes discovery messages.
 * Useful during startup or after Home Assistant restart.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t ha_discovery_ng_publish_all(void);

/**
 * @brief Remove discovery for a device
 *
 * Publishes empty payloads to remove all discovery entries for a device.
 *
 * @param[in] id Device identifier
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if device not in registry
 *      - ESP_ERR_INVALID_STATE if MQTT not connected
 */
esp_err_t ha_discovery_ng_remove_device(device_id_t id);

/* ============================================================================
 * Event Handler
 * ============================================================================ */

/**
 * @brief Handle device ready event
 *
 * Called when a device joins or completes interview. Publishes
 * discovery messages for the device based on its capabilities.
 *
 * @param[in] id Device identifier
 *
 * @note This function is automatically subscribed to EVT_DEVICE_JOINED
 *       and EVT_DEVICE_INTERVIEWED during init.
 */
void ha_discovery_ng_on_device_ready(device_id_t id);

/* ============================================================================
 * Configuration
 * ============================================================================ */

/**
 * @brief Enable or disable NG discovery
 *
 * @param[in] enable true to enable, false to disable
 * @return ESP_OK always
 */
esp_err_t ha_discovery_ng_set_enabled(bool enable);

/**
 * @brief Check if NG discovery is enabled
 *
 * @return true if enabled, false otherwise
 */
bool ha_discovery_ng_is_enabled(void);

/* ============================================================================
 * Discovery Cache Management
 * ============================================================================ */

/**
 * @brief Invalidate entire discovery cache
 *
 * Forces republishing of discovery for all devices on next request.
 * Useful after MQTT reconnection or Home Assistant restart.
 *
 * @return ESP_OK always
 */
esp_err_t ha_discovery_invalidate_cache(void);

/**
 * @brief Invalidate cache entry for a specific device
 *
 * Forces republishing of discovery for this device on next request.
 *
 * @param[in] id Device identifier
 * @return
 *      - ESP_OK if device found and invalidated
 *      - ESP_ERR_NOT_FOUND if device not in cache
 */
esp_err_t ha_discovery_invalidate_device(device_id_t id);

/**
 * @brief Check if device discovery is cached and valid
 *
 * Returns true if the device has been published recently with the
 * same capabilities hash. Used internally to skip redundant publishes.
 *
 * @param[in] id Device identifier
 * @param[in] capabilities_hash Hash of current capabilities
 * @return true if cached and valid, false if needs (re)publish
 */
bool ha_discovery_is_cached(device_id_t id, uint32_t capabilities_hash);

/**
 * @brief Get discovery cache statistics
 *
 * @param[out] entries_used Number of cache entries in use
 * @param[out] cache_hits Total cache hits since init
 * @param[out] cache_misses Total cache misses since init
 */
void ha_discovery_get_cache_stats(size_t *entries_used, uint32_t *cache_hits, uint32_t *cache_misses);

/* ============================================================================
 * Template Access (for testing/debugging)
 * ============================================================================ */

/**
 * @brief Get the number of entity templates
 *
 * @return Number of templates in the static template table
 */
size_t ha_discovery_ng_get_template_count(void);

/**
 * @brief Get a template by index
 *
 * @param[in] index Template index (0 to count-1)
 * @return Pointer to template, or NULL if index out of range
 */
const ha_entity_template_t *ha_discovery_ng_get_template(size_t index);

#ifdef __cplusplus
}
#endif

#endif /* HA_DISCOVERY_NG_H */
