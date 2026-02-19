/**
 * @file ha_discovery_ng.c
 * @brief Next-Generation Home Assistant MQTT Discovery Implementation
 *
 * Template-based discovery system using device capabilities.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ha_discovery_ng.h"
#include "ha_constants.h"
#include "core/device/device_registry.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/foundation_init.h"
#include "core/memory/memory_manager_ng.h"
#include "mqtt/gateway_mqtt.h"
#include "mqtt/mqtt_helpers.h"
#include "mqtt/mqtt_topics.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "zigbee/converter/zb_converter.h"
#include "zigbee/tuya/tuya_driver_registry.h"
#include "zigbee/tuya/tuya_device_driver.h"

static const char *TAG = "HA_DISC_NG";

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** @brief JSON pool buffer size (must match foundation_init.c) */
#define JSON_POOL_BUFFER_SIZE   2048

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Discovery enabled flag */
static bool s_enabled = true;

/** @brief Initialization state */
static bool s_initialized = false;

/** @brief Discovery cache array */
static discovery_cache_entry_t s_discovery_cache[DISCOVERY_CACHE_SIZE];

/** @brief Cache hit counter for statistics */
static uint32_t s_cache_hits = 0;

/** @brief Cache miss counter for statistics */
static uint32_t s_cache_misses = 0;

/* ============================================================================
 * Discovery Cache Functions (FNV-1a Hash)
 * ============================================================================ */

/** @brief FNV-1a 32-bit offset basis */
#define FNV1A_32_OFFSET_BASIS   0x811c9dc5

/** @brief FNV-1a 32-bit prime */
#define FNV1A_32_PRIME          0x01000193

/**
 * @brief Compute FNV-1a hash of a byte array
 *
 * @param[in] data Pointer to data
 * @param[in] len Length of data
 * @param[in] hash Initial hash value (use FNV1A_32_OFFSET_BASIS for new hash)
 * @return Updated hash value
 */
static uint32_t fnv1a_hash_bytes(const uint8_t *data, size_t len, uint32_t hash)
{
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= FNV1A_32_PRIME;
    }
    return hash;
}

/**
 * @brief Compute FNV-1a hash of a string
 *
 * @param[in] str Null-terminated string
 * @param[in] hash Initial hash value
 * @return Updated hash value
 */
static uint32_t fnv1a_hash_string(const char *str, uint32_t hash)
{
    if (str == NULL) {
        return hash;
    }
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= FNV1A_32_PRIME;
    }
    return hash;
}

/**
 * @brief Compute discovery hash for a device
 *
 * Combines capabilities, model, and manufacturer into a single hash
 * to detect when discovery needs to be republished.
 *
 * @param[in] dev Device pointer
 * @return 32-bit hash of discovery-relevant device properties
 */
static uint32_t compute_discovery_hash(const device_t *dev)
{
    uint32_t hash = FNV1A_32_OFFSET_BASIS;

    /* Hash capabilities bitmask */
    hash = fnv1a_hash_bytes((const uint8_t *)&dev->capabilities,
                            sizeof(dev->capabilities), hash);

    /* Hash model string */
    hash = fnv1a_hash_string(dev->model, hash);

    /* Hash manufacturer string */
    hash = fnv1a_hash_string(dev->manufacturer, hash);

    /* Hash friendly name (affects entity names) */
    hash = fnv1a_hash_string(dev->friendly_name, hash);

    /* Hash converter pointer (changes when converter is rebound) */
    if (dev->protocol == DEV_PROTOCOL_ZIGBEE) {
        uintptr_t conv_ptr = (uintptr_t)dev->proto.zigbee.converter;
        hash = fnv1a_hash_bytes((const uint8_t *)&conv_ptr, sizeof(conv_ptr), hash);

        /* Hash Tuya driver pointer (late binding adds driver-specific entities) */
        if (dev->proto.zigbee.short_addr != 0) {
            const tuya_device_driver_t *drv = tuya_driver_get(dev->proto.zigbee.short_addr);
            uintptr_t drv_ptr = (uintptr_t)drv;
            hash = fnv1a_hash_bytes((const uint8_t *)&drv_ptr, sizeof(drv_ptr), hash);
        }
    }

    return hash;
}

/**
 * @brief Get current timestamp in seconds
 *
 * @return Current epoch time in seconds
 */
static uint32_t get_current_time_sec(void)
{
    return (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
}

/**
 * @brief Find cache entry for device
 *
 * @param[in] id Device identifier
 * @return Pointer to cache entry, or NULL if not found
 */
static discovery_cache_entry_t *cache_find_entry(device_id_t id)
{
    for (size_t i = 0; i < DISCOVERY_CACHE_SIZE; i++) {
        if (s_discovery_cache[i].id == id) {
            return &s_discovery_cache[i];
        }
    }
    return NULL;
}

/**
 * @brief Find or allocate cache entry for device
 *
 * If device is not in cache, finds an empty slot or evicts
 * the oldest entry if cache is full.
 *
 * @param[in] id Device identifier
 * @return Pointer to cache entry (always succeeds)
 */
static discovery_cache_entry_t *cache_get_or_create_entry(device_id_t id)
{
    /* First, check if device already has an entry */
    discovery_cache_entry_t *entry = cache_find_entry(id);
    if (entry != NULL) {
        return entry;
    }

    /* Find empty slot (id == 0) */
    for (size_t i = 0; i < DISCOVERY_CACHE_SIZE; i++) {
        if (s_discovery_cache[i].id == 0) {
            s_discovery_cache[i].id = id;
            return &s_discovery_cache[i];
        }
    }

    /* Cache full - evict oldest entry (LRU) */
    uint32_t oldest_time = UINT32_MAX;
    size_t oldest_idx = 0;

    for (size_t i = 0; i < DISCOVERY_CACHE_SIZE; i++) {
        if (s_discovery_cache[i].last_published < oldest_time) {
            oldest_time = s_discovery_cache[i].last_published;
            oldest_idx = i;
        }
    }

    ESP_LOGD(TAG, "Cache full, evicting entry for 0x%016" PRIx64,
             s_discovery_cache[oldest_idx].id);

    s_discovery_cache[oldest_idx].id = id;
    s_discovery_cache[oldest_idx].capabilities_hash = 0;
    s_discovery_cache[oldest_idx].last_published = 0;

    return &s_discovery_cache[oldest_idx];
}

/**
 * @brief Update cache entry after successful publish
 *
 * @param[in] id Device identifier
 * @param[in] hash Capabilities hash
 */
static void cache_update_entry(device_id_t id, uint32_t hash)
{
    discovery_cache_entry_t *entry = cache_get_or_create_entry(id);
    entry->capabilities_hash = hash;
    entry->last_published = get_current_time_sec();
}

/**
 * @brief Check if device discovery is cached and still valid
 *
 * @param[in] id Device identifier
 * @param[in] hash Current capabilities hash
 * @return true if cached and valid, false if publish needed
 */
static bool cache_check_valid(device_id_t id, uint32_t hash)
{
    discovery_cache_entry_t *entry = cache_find_entry(id);
    if (entry == NULL) {
        return false;
    }

    /* Check if hash matches */
    if (entry->capabilities_hash != hash) {
        ESP_LOGD(TAG, "Cache miss for 0x%016" PRIx64 ": hash changed (0x%08" PRIx32 " -> 0x%08" PRIx32 ")",
                 id, entry->capabilities_hash, hash);
        return false;
    }

    /* Check if TTL expired */
    uint32_t now = get_current_time_sec();
    uint32_t age = now - entry->last_published;

    if (age > DISCOVERY_CACHE_TTL_SEC) {
        ESP_LOGD(TAG, "Cache miss for 0x%016" PRIx64 ": TTL expired (age: %" PRIu32 "s)",
                 id, age);
        return false;
    }

    return true;
}

/* ============================================================================
 * Entity Template Table
 * ============================================================================
 * Maps device capabilities to Home Assistant entities.
 * Order matters for actuators - more specific templates should come first.
 * ============================================================================ */

static const ha_entity_template_t s_templates[] = {
    /* ========== Sensors (Environmental) ========== */
    {
        .component = "sensor",
        .device_class = "temperature",
        .unit = HA_UNIT_CELSIUS,
        .icon = NULL,
        .value_template = "{{ value_json.temperature }}",
        .state_key = "temperature",
        .state_class = HA_STATE_CLASS_MEASUREMENT,
        .required_caps = DEV_CAP_TEMPERATURE,
        .is_actuator = false
    },
    {
        .component = "sensor",
        .device_class = "humidity",
        .unit = HA_UNIT_PERCENT,
        .icon = NULL,
        .value_template = "{{ value_json.humidity }}",
        .state_key = "humidity",
        .state_class = HA_STATE_CLASS_MEASUREMENT,
        .required_caps = DEV_CAP_HUMIDITY,
        .is_actuator = false
    },
    {
        .component = "sensor",
        .device_class = "atmospheric_pressure",
        .unit = HA_UNIT_HPA,
        .icon = "mdi:gauge",
        .value_template = "{{ value_json.pressure }}",
        .state_key = "pressure",
        .state_class = HA_STATE_CLASS_MEASUREMENT,
        .required_caps = DEV_CAP_PRESSURE,
        .is_actuator = false
    },
    {
        .component = "sensor",
        .device_class = "battery",
        .unit = HA_UNIT_PERCENT,
        .icon = NULL,
        .value_template = "{{ value_json.battery }}",
        .state_key = "battery",
        .state_class = HA_STATE_CLASS_MEASUREMENT,
        .required_caps = DEV_CAP_BATTERY,
        .is_actuator = false
    },

    /* ========== Sensors (Energy Monitoring) ========== */
    {
        .component = "sensor",
        .device_class = "power",
        .unit = HA_UNIT_WATT,
        .icon = NULL,
        .value_template = "{{ value_json.power }}",
        .state_key = "power",
        .state_class = HA_STATE_CLASS_MEASUREMENT,
        .required_caps = DEV_CAP_POWER,
        .is_actuator = false
    },
    {
        .component = "sensor",
        .device_class = "energy",
        .unit = HA_UNIT_KWH,
        .icon = NULL,
        .value_template = "{{ value_json.energy }}",
        .state_key = "energy",
        .state_class = HA_STATE_CLASS_TOTAL_INCREASING,
        .required_caps = DEV_CAP_ENERGY,
        .is_actuator = false
    },
    {
        .component = "sensor",
        .device_class = "voltage",
        .unit = HA_UNIT_VOLT,
        .icon = NULL,
        .value_template = "{{ value_json.voltage }}",
        .state_key = "voltage",
        .state_class = HA_STATE_CLASS_MEASUREMENT,
        .required_caps = DEV_CAP_VOLTAGE,
        .is_actuator = false
    },
    {
        .component = "sensor",
        .device_class = "current",
        .unit = HA_UNIT_AMPERE,
        .icon = NULL,
        .value_template = "{{ value_json.current }}",
        .state_key = "current",
        .state_class = HA_STATE_CLASS_MEASUREMENT,
        .required_caps = DEV_CAP_CURRENT,
        .is_actuator = false
    },

    /* ========== Binary Sensors ========== */
    {
        .component = "binary_sensor",
        .device_class = "motion",
        .unit = NULL,
        .icon = "mdi:motion-sensor",
        .value_template = "{{ value_json.occupancy }}",
        .state_key = "occupancy",
        .state_class = NULL,
        .required_caps = DEV_CAP_MOTION,
        .is_actuator = false
    },
    {
        .component = "binary_sensor",
        .device_class = "door",
        .unit = NULL,
        .icon = NULL,
        .value_template = "{{ value_json.contact }}",
        .state_key = "contact",
        .state_class = NULL,
        .required_caps = DEV_CAP_CONTACT,
        .is_actuator = false
    },
    {
        .component = "binary_sensor",
        .device_class = "vibration",
        .unit = NULL,
        .icon = NULL,
        .value_template = "{{ value_json.vibration }}",
        .state_key = "vibration",
        .state_class = NULL,
        .required_caps = DEV_CAP_VIBRATION,
        .is_actuator = false
    },
    {
        .component = "binary_sensor",
        .device_class = "moisture",
        .unit = NULL,
        .icon = NULL,
        .value_template = "{{ value_json.water_leak }}",
        .state_key = "water_leak",
        .state_class = NULL,
        .required_caps = DEV_CAP_WATER_LEAK,
        .is_actuator = false
    },
    {
        .component = "binary_sensor",
        .device_class = "smoke",
        .unit = NULL,
        .icon = NULL,
        .value_template = "{{ value_json.smoke }}",
        .state_key = "smoke",
        .state_class = NULL,
        .required_caps = DEV_CAP_SMOKE,
        .is_actuator = false
    },
    {
        .component = "binary_sensor",
        .device_class = "carbon_monoxide",
        .unit = NULL,
        .icon = NULL,
        .value_template = "{{ value_json.carbon_monoxide }}",
        .state_key = "carbon_monoxide",
        .state_class = NULL,
        .required_caps = DEV_CAP_CO,
        .is_actuator = false
    },

    /* ========== Actuators ========== */
    /* Note: Light with brightness takes precedence over simple on/off light */
    {
        .component = "light",
        .device_class = NULL,
        .unit = NULL,
        .icon = NULL,
        .value_template = NULL,
        .state_key = NULL,
        .state_class = NULL,
        .required_caps = DEV_CAP_ON_OFF | DEV_CAP_BRIGHTNESS,
        .is_actuator = true
    },
    /* Simple on/off light - only if no brightness */
    {
        .component = "light",
        .device_class = NULL,
        .unit = NULL,
        .icon = NULL,
        .value_template = NULL,
        .state_key = NULL,
        .state_class = NULL,
        .required_caps = DEV_CAP_ON_OFF,
        .is_actuator = true
    },
    {
        .component = "switch",
        .device_class = NULL,
        .unit = NULL,
        .icon = NULL,
        .value_template = NULL,
        .state_key = NULL,
        .state_class = NULL,
        .required_caps = DEV_CAP_ON_OFF,
        .is_actuator = true
    },
    {
        .component = "lock",
        .device_class = NULL,
        .unit = NULL,
        .icon = NULL,
        .value_template = NULL,
        .state_key = NULL,
        .state_class = NULL,
        .required_caps = DEV_CAP_LOCK,
        .is_actuator = true
    },
    {
        .component = "cover",
        .device_class = "blind",
        .unit = NULL,
        .icon = NULL,
        .value_template = NULL,
        .state_key = NULL,
        .state_class = NULL,
        .required_caps = DEV_CAP_COVER,
        .is_actuator = true
    },
    {
        .component = "fan",
        .device_class = NULL,
        .unit = NULL,
        .icon = NULL,
        .value_template = NULL,
        .state_key = NULL,
        .state_class = NULL,
        .required_caps = DEV_CAP_FAN,
        .is_actuator = true
    },
    {
        .component = "climate",
        .device_class = NULL,
        .unit = NULL,
        .icon = NULL,
        .value_template = NULL,
        .state_key = NULL,
        .state_class = NULL,
        .required_caps = DEV_CAP_CLIMATE,
        .is_actuator = true
    },
};

#define TEMPLATE_COUNT (sizeof(s_templates) / sizeof(s_templates[0]))

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * @brief Convert device_id_t to hex string
 *
 * @param[in] id Device ID
 * @param[out] buf Output buffer (min 19 bytes)
 * @param[in] buf_size Buffer size
 */
static void device_id_to_ieee_str(device_id_t id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "0x%016" PRIx64, id);
}

/**
 * @brief Build discovery topic
 *
 * @param[in] component HA component type
 * @param[in] unique_id Entity unique ID
 * @param[out] topic Output buffer
 * @param[in] topic_size Buffer size
 * @return ESP_OK on success
 */
static esp_err_t build_discovery_topic(const char *component, const char *unique_id,
                                       char *topic, size_t topic_size)
{
    return mqtt_topic_ha_discovery(component, unique_id, topic, topic_size);
}

/**
 * @brief Build unique ID for an entity
 *
 * @param[in] dev Device pointer
 * @param[in] tmpl Template pointer
 * @param[out] unique_id Output buffer
 * @param[in] buf_size Buffer size
 */
static void build_unique_id(const device_t *dev, const ha_entity_template_t *tmpl,
                            char *unique_id, size_t buf_size)
{
    char ieee_str[20];
    device_id_to_ieee_str(dev->id, ieee_str, sizeof(ieee_str));

    if (tmpl->state_key != NULL) {
        snprintf(unique_id, buf_size, "%s_%s", ieee_str, tmpl->state_key);
    } else if (tmpl->device_class != NULL) {
        snprintf(unique_id, buf_size, "%s_%s", ieee_str, tmpl->device_class);
    } else {
        snprintf(unique_id, buf_size, "%s_%s", ieee_str, tmpl->component);
    }
}

/**
 * @brief Build entity display name
 *
 * @param[in] dev Device pointer
 * @param[in] tmpl Template pointer
 * @param[out] name Output buffer
 * @param[in] buf_size Buffer size
 */
static void build_entity_name(const device_t *dev, const ha_entity_template_t *tmpl,
                              char *name, size_t buf_size)
{
    const char *suffix = NULL;

    if (tmpl->state_key != NULL) {
        suffix = tmpl->state_key;
    } else if (tmpl->device_class != NULL) {
        suffix = tmpl->device_class;
    } else {
        suffix = tmpl->component;
    }

    /* Use friendly_name, fall back to model or IEEE string.
     * Also detect when friendly_name is the default IEEE hex (e.g. "0xa4c138cd664a2051")
     * which is not user-friendly — prefer model in that case. */
    const char *dev_name = dev->friendly_name;
    char ieee_fallback[20];
    if (dev_name[0] == '\0' ||
        (dev_name[0] == '0' && dev_name[1] == 'x' && dev->model[0] != '\0')) {
        if (dev->model[0] != '\0') {
            dev_name = dev->model;
        } else {
            device_id_to_str(dev->id, ieee_fallback, sizeof(ieee_fallback));
            dev_name = ieee_fallback;
        }
    }

    /* Capitalize first letter of suffix for display */
    char cap_suffix[32];
    if (suffix != NULL && strlen(suffix) > 0) {
        strlcpy(cap_suffix, suffix, sizeof(cap_suffix));
        if (cap_suffix[0] >= 'a' && cap_suffix[0] <= 'z') {
            cap_suffix[0] = cap_suffix[0] - 'a' + 'A';
        }
        snprintf(name, buf_size, "%s %s", dev_name, cap_suffix);
    } else {
        strlcpy(name, dev_name, buf_size);
    }
}

/**
 * @brief Create device info JSON object
 *
 * @param[in] dev Device pointer
 * @return cJSON object (caller must NOT free - added to parent)
 */
static cJSON *create_device_info(const device_t *dev)
{
    cJSON *device = cJSON_CreateObject();
    if (device == NULL) {
        return NULL;
    }

    /* Identifiers array */
    char ieee_str[20];
    device_id_to_ieee_str(dev->id, ieee_str, sizeof(ieee_str));

    char identifier[64];
    snprintf(identifier, sizeof(identifier), "zigbee2mqtt_%s", ieee_str);

    cJSON *identifiers = cJSON_CreateArray();
    if (identifiers != NULL) {
        cJSON_AddItemToArray(identifiers, cJSON_CreateString(identifier));
        cJSON_AddItemToObject(device, "identifiers", identifiers);
    }

    /* Device name - fall back to model or IEEE string if friendly_name empty
     * or is the default IEEE hex string (e.g. "0xa4c138cd664a2051") */
    const char *name = dev->friendly_name;
    if (name[0] == '\0' ||
        (name[0] == '0' && name[1] == 'x' && dev->model[0] != '\0')) {
        name = (dev->model[0] != '\0') ? dev->model : ieee_str;
    }
    cJSON_AddStringToObject(device, "name", name);

    /* Manufacturer and model */
    if (strlen(dev->manufacturer) > 0) {
        cJSON_AddStringToObject(device, "manufacturer", dev->manufacturer);
    }
    if (strlen(dev->model) > 0) {
        cJSON_AddStringToObject(device, "model", dev->model);
    }

    /* Via device (bridge) */
    cJSON_AddStringToObject(device, "via_device", "zigbee2mqtt_bridge");

    return device;
}

/**
 * @brief Add sensor-specific fields to discovery config
 *
 * @param[in,out] config cJSON config object
 * @param[in] dev Device pointer
 * @param[in] tmpl Template pointer
 */
static void add_sensor_fields(cJSON *config, const device_t *dev,
                              const ha_entity_template_t *tmpl)
{
    /* State topic */
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "zigbee2mqtt/%s", dev->friendly_name);
    cJSON_AddStringToObject(config, "state_topic", state_topic);

    /* Value template */
    if (tmpl->value_template != NULL) {
        cJSON_AddStringToObject(config, "value_template", tmpl->value_template);
    }

    /* Device class */
    if (tmpl->device_class != NULL) {
        cJSON_AddStringToObject(config, "device_class", tmpl->device_class);
    }

    /* Unit of measurement */
    if (tmpl->unit != NULL) {
        cJSON_AddStringToObject(config, "unit_of_measurement", tmpl->unit);
    }

    /* Icon */
    if (tmpl->icon != NULL) {
        cJSON_AddStringToObject(config, "icon", tmpl->icon);
    }

    /* State class */
    if (tmpl->state_class != NULL) {
        cJSON_AddStringToObject(config, "state_class", tmpl->state_class);
    }
}

/**
 * @brief Add binary sensor-specific fields to discovery config
 *
 * @param[in,out] config cJSON config object
 * @param[in] dev Device pointer
 * @param[in] tmpl Template pointer
 */
static void add_binary_sensor_fields(cJSON *config, const device_t *dev,
                                     const ha_entity_template_t *tmpl)
{
    /* State topic */
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "zigbee2mqtt/%s", dev->friendly_name);
    cJSON_AddStringToObject(config, "state_topic", state_topic);

    /* Value template */
    if (tmpl->value_template != NULL) {
        cJSON_AddStringToObject(config, "value_template", tmpl->value_template);
    }

    /* Device class */
    if (tmpl->device_class != NULL) {
        cJSON_AddStringToObject(config, "device_class", tmpl->device_class);
    }

    /* Icon */
    if (tmpl->icon != NULL) {
        cJSON_AddStringToObject(config, "icon", tmpl->icon);
    }

    /* Payload on/off */
    if (strcmp(tmpl->device_class, "door") == 0) {
        /* Contact sensors: false = open (door open triggers ON) */
        cJSON_AddStringToObject(config, "payload_on", "false");
        cJSON_AddStringToObject(config, "payload_off", "true");
    } else {
        cJSON_AddStringToObject(config, "payload_on", "true");
        cJSON_AddStringToObject(config, "payload_off", "false");
    }

    /* Vibration sensor auto-off */
    if (tmpl->required_caps & DEV_CAP_VIBRATION) {
        cJSON_AddNumberToObject(config, "off_delay", 3);
    }
}

/**
 * @brief Add light-specific fields to discovery config
 *
 * @param[in,out] config cJSON config object
 * @param[in] dev Device pointer
 * @param[in] tmpl Template pointer
 */
static void add_light_fields(cJSON *config, const device_t *dev,
                             const ha_entity_template_t *tmpl)
{
    (void)tmpl;

    /* State topic */
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "zigbee2mqtt/%s", dev->friendly_name);
    cJSON_AddStringToObject(config, "state_topic", state_topic);

    /* Command topic */
    char cmd_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "zigbee2mqtt/%s/set", dev->friendly_name);
    cJSON_AddStringToObject(config, "command_topic", cmd_topic);

    /* JSON schema for lights */
    cJSON_AddStringToObject(config, "schema", "json");

    /* Brightness support based on capabilities */
    if (dev->capabilities & DEV_CAP_BRIGHTNESS) {
        cJSON_AddBoolToObject(config, "brightness", true);
        cJSON_AddNumberToObject(config, "brightness_scale", 254);
    }

    /* Color temperature support */
    if (dev->capabilities & DEV_CAP_COLOR_TEMP) {
        cJSON_AddBoolToObject(config, "color_temp", true);
    }

    /* XY color support */
    if (dev->capabilities & DEV_CAP_COLOR_XY) {
        cJSON_AddBoolToObject(config, "xy", true);
    }
}

/**
 * @brief Add switch-specific fields to discovery config
 *
 * @param[in,out] config cJSON config object
 * @param[in] dev Device pointer
 */
static void add_switch_fields(cJSON *config, const device_t *dev)
{
    /* State topic */
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "zigbee2mqtt/%s", dev->friendly_name);
    cJSON_AddStringToObject(config, "state_topic", state_topic);

    /* Command topic */
    char cmd_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "zigbee2mqtt/%s/set", dev->friendly_name);
    cJSON_AddStringToObject(config, "command_topic", cmd_topic);

    /* Value template and payloads */
    cJSON_AddStringToObject(config, "value_template", "{{ value_json.state }}");
    cJSON_AddStringToObject(config, "payload_on", "{\"state\":\"ON\"}");
    cJSON_AddStringToObject(config, "payload_off", "{\"state\":\"OFF\"}");
    cJSON_AddStringToObject(config, "state_on", "ON");
    cJSON_AddStringToObject(config, "state_off", "OFF");
}

/**
 * @brief Add lock-specific fields to discovery config
 *
 * @param[in,out] config cJSON config object
 * @param[in] dev Device pointer
 */
static void add_lock_fields(cJSON *config, const device_t *dev)
{
    /* State topic */
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "zigbee2mqtt/%s", dev->friendly_name);
    cJSON_AddStringToObject(config, "state_topic", state_topic);

    /* Command topic */
    char cmd_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "zigbee2mqtt/%s/set", dev->friendly_name);
    cJSON_AddStringToObject(config, "command_topic", cmd_topic);

    /* Value template */
    cJSON_AddStringToObject(config, "value_template", "{{ value_json.state }}");

    /* State values */
    cJSON_AddStringToObject(config, "state_locked", "LOCK");
    cJSON_AddStringToObject(config, "state_unlocked", "UNLOCK");

    /* Command payloads */
    cJSON_AddStringToObject(config, "payload_lock", "{\"state\": \"LOCK\"}");
    cJSON_AddStringToObject(config, "payload_unlock", "{\"state\": \"UNLOCK\"}");

    cJSON_AddBoolToObject(config, "optimistic", false);
}

/**
 * @brief Add cover-specific fields to discovery config
 *
 * @param[in,out] config cJSON config object
 * @param[in] dev Device pointer
 */
static void add_cover_fields(cJSON *config, const device_t *dev)
{
    /* State topic */
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "zigbee2mqtt/%s", dev->friendly_name);
    cJSON_AddStringToObject(config, "state_topic", state_topic);

    /* Command topic */
    char cmd_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "zigbee2mqtt/%s/set", dev->friendly_name);
    cJSON_AddStringToObject(config, "command_topic", cmd_topic);

    /* Position topics */
    cJSON_AddStringToObject(config, "position_topic", state_topic);
    cJSON_AddStringToObject(config, "set_position_topic", cmd_topic);

    /* State values */
    cJSON_AddStringToObject(config, "state_open", "OPEN");
    cJSON_AddStringToObject(config, "state_closed", "CLOSE");
    cJSON_AddStringToObject(config, "state_stopped", "STOP");

    /* Position templates */
    cJSON_AddStringToObject(config, "position_template", "{{ value_json.position }}");
    cJSON_AddStringToObject(config, "set_position_template", "{\"position\": {{ position }}}");

    /* Payload templates */
    cJSON_AddStringToObject(config, "payload_open", "{\"state\": \"OPEN\"}");
    cJSON_AddStringToObject(config, "payload_close", "{\"state\": \"CLOSE\"}");
    cJSON_AddStringToObject(config, "payload_stop", "{\"state\": \"STOP\"}");

    /* Position range */
    cJSON_AddNumberToObject(config, "position_open", HA_COVER_POSITION_OPEN);
    cJSON_AddNumberToObject(config, "position_closed", HA_COVER_POSITION_CLOSED);

    cJSON_AddBoolToObject(config, "optimistic", false);
}

/**
 * @brief Add fan-specific fields to discovery config
 *
 * @param[in,out] config cJSON config object
 * @param[in] dev Device pointer
 */
static void add_fan_fields(cJSON *config, const device_t *dev)
{
    /* State topic */
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "zigbee2mqtt/%s", dev->friendly_name);
    cJSON_AddStringToObject(config, "state_topic", state_topic);

    /* Command topic */
    char cmd_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "zigbee2mqtt/%s/set", dev->friendly_name);
    cJSON_AddStringToObject(config, "command_topic", cmd_topic);

    /* State value templates */
    cJSON_AddStringToObject(config, "state_value_template",
                            "{{ 'ON' if value_json.fan_mode != 'off' else 'OFF' }}");
    cJSON_AddStringToObject(config, "preset_mode_state_topic", state_topic);
    cJSON_AddStringToObject(config, "preset_mode_value_template",
                            "{{ value_json.fan_mode }}");

    /* Command templates */
    cJSON_AddStringToObject(config, "payload_on", "{\"fan_state\": \"ON\"}");
    cJSON_AddStringToObject(config, "payload_off", "{\"fan_mode\": \"off\"}");

    /* Preset mode */
    cJSON_AddStringToObject(config, "preset_mode_command_topic", cmd_topic);
    cJSON_AddStringToObject(config, "preset_mode_command_template",
                            "{\"fan_mode\": \"{{ value }}\"}");

    /* Preset modes array */
    cJSON *preset_modes = cJSON_CreateArray();
    if (preset_modes != NULL) {
        cJSON_AddItemToArray(preset_modes, cJSON_CreateString("off"));
        cJSON_AddItemToArray(preset_modes, cJSON_CreateString("low"));
        cJSON_AddItemToArray(preset_modes, cJSON_CreateString("medium"));
        cJSON_AddItemToArray(preset_modes, cJSON_CreateString("high"));
        cJSON_AddItemToArray(preset_modes, cJSON_CreateString("auto"));
        cJSON_AddItemToObject(config, "preset_modes", preset_modes);
    }

    cJSON_AddBoolToObject(config, "optimistic", false);
}

/**
 * @brief Add climate-specific fields to discovery config
 *
 * @param[in,out] config cJSON config object
 * @param[in] dev Device pointer
 */
static void add_climate_fields(cJSON *config, const device_t *dev)
{
    /* State topic */
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "zigbee2mqtt/%s", dev->friendly_name);

    /* Command topic */
    char cmd_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "zigbee2mqtt/%s/set", dev->friendly_name);

    /* State topics */
    cJSON_AddStringToObject(config, "current_temperature_topic", state_topic);
    cJSON_AddStringToObject(config, "mode_state_topic", state_topic);
    cJSON_AddStringToObject(config, "temperature_state_topic", state_topic);
    cJSON_AddStringToObject(config, "temperature_high_state_topic", state_topic);
    cJSON_AddStringToObject(config, "temperature_low_state_topic", state_topic);

    /* Command topics */
    cJSON_AddStringToObject(config, "mode_command_topic", cmd_topic);
    cJSON_AddStringToObject(config, "temperature_command_topic", cmd_topic);
    cJSON_AddStringToObject(config, "temperature_high_command_topic", cmd_topic);
    cJSON_AddStringToObject(config, "temperature_low_command_topic", cmd_topic);

    /* Value templates */
    cJSON_AddStringToObject(config, "current_temperature_template",
                            "{{ value_json.local_temperature }}");
    cJSON_AddStringToObject(config, "mode_state_template",
                            "{{ value_json.system_mode }}");
    cJSON_AddStringToObject(config, "temperature_state_template",
                            "{% if value_json.system_mode == 'heat' %}"
                            "{{ value_json.occupied_heating_setpoint }}"
                            "{% else %}"
                            "{{ value_json.occupied_cooling_setpoint }}"
                            "{% endif %}");
    cJSON_AddStringToObject(config, "temperature_high_state_template",
                            "{{ value_json.occupied_cooling_setpoint }}");
    cJSON_AddStringToObject(config, "temperature_low_state_template",
                            "{{ value_json.occupied_heating_setpoint }}");

    /* Command templates */
    cJSON_AddStringToObject(config, "mode_command_template",
                            "{\"system_mode\": \"{{ value }}\"}");
    cJSON_AddStringToObject(config, "temperature_command_template",
                            "{% if state_attr(entity_id, 'hvac_mode') == 'heat' %}"
                            "{\"occupied_heating_setpoint\": {{ value }}}"
                            "{% else %}"
                            "{\"occupied_cooling_setpoint\": {{ value }}}"
                            "{% endif %}");
    cJSON_AddStringToObject(config, "temperature_high_command_template",
                            "{\"occupied_cooling_setpoint\": {{ value }}}");
    cJSON_AddStringToObject(config, "temperature_low_command_template",
                            "{\"occupied_heating_setpoint\": {{ value }}}");

    /* HVAC modes */
    cJSON *modes = cJSON_CreateArray();
    if (modes != NULL) {
        cJSON_AddItemToArray(modes, cJSON_CreateString("off"));
        cJSON_AddItemToArray(modes, cJSON_CreateString("heat"));
        cJSON_AddItemToArray(modes, cJSON_CreateString("cool"));
        cJSON_AddItemToArray(modes, cJSON_CreateString("auto"));
        cJSON_AddItemToArray(modes, cJSON_CreateString("fan_only"));
        cJSON_AddItemToArray(modes, cJSON_CreateString("dry"));
        cJSON_AddItemToObject(config, "modes", modes);
    }

    /* Temperature settings */
    cJSON_AddNumberToObject(config, "min_temp", HA_CLIMATE_MIN_TEMP_DEFAULT);
    cJSON_AddNumberToObject(config, "max_temp", HA_CLIMATE_MAX_TEMP_DEFAULT);
    cJSON_AddNumberToObject(config, "temp_step", HA_CLIMATE_TEMP_STEP_DEFAULT);
    cJSON_AddStringToObject(config, "temperature_unit", "C");

    cJSON_AddBoolToObject(config, "optimistic", false);
}

/**
 * @brief Create discovery config JSON for a template
 *
 * @param[in] dev Device pointer
 * @param[in] tmpl Template pointer
 * @return cJSON object (caller must free), or NULL on error
 */
static cJSON *create_discovery_config(const device_t *dev, const ha_entity_template_t *tmpl)
{
    cJSON *config = cJSON_CreateObject();
    if (config == NULL) {
        return NULL;
    }

    /* Build unique_id and name */
    char unique_id[80];
    char name[96];
    build_unique_id(dev, tmpl, unique_id, sizeof(unique_id));
    build_entity_name(dev, tmpl, name, sizeof(name));

    /* Common fields */
    cJSON_AddStringToObject(config, "name", name);
    cJSON_AddStringToObject(config, "unique_id", unique_id);

    /* Device info */
    cJSON *device_info = create_device_info(dev);
    if (device_info != NULL) {
        cJSON_AddItemToObject(config, "device", device_info);
    }

    /* Availability topic */
    char avail_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "zigbee2mqtt/%s/availability", dev->friendly_name);
    cJSON_AddStringToObject(config, "availability_topic", avail_topic);
    cJSON_AddStringToObject(config, "availability_template", "{{ value_json.state }}");

    /* Component-specific fields */
    if (strcmp(tmpl->component, "sensor") == 0) {
        add_sensor_fields(config, dev, tmpl);
    } else if (strcmp(tmpl->component, "binary_sensor") == 0) {
        add_binary_sensor_fields(config, dev, tmpl);
    } else if (strcmp(tmpl->component, "light") == 0) {
        add_light_fields(config, dev, tmpl);
    } else if (strcmp(tmpl->component, "switch") == 0) {
        add_switch_fields(config, dev);
    } else if (strcmp(tmpl->component, "lock") == 0) {
        add_lock_fields(config, dev);
    } else if (strcmp(tmpl->component, "cover") == 0) {
        add_cover_fields(config, dev);
    } else if (strcmp(tmpl->component, "fan") == 0) {
        add_fan_fields(config, dev);
    } else if (strcmp(tmpl->component, "climate") == 0) {
        add_climate_fields(config, dev);
    }

    return config;
}

/**
 * @brief Publish discovery for a single entity template
 *
 * Uses the JSON buffer pool when available for serialization,
 * falling back to dynamic allocation for larger discovery messages.
 *
 * @param[in] dev Device pointer
 * @param[in] tmpl Template pointer
 * @return ESP_OK on success
 */
static esp_err_t publish_entity_discovery(const device_t *dev, const ha_entity_template_t *tmpl)
{
    /* Create discovery config */
    cJSON *config = create_discovery_config(dev, tmpl);
    if (config == NULL) {
        ESP_LOGE(TAG, "Failed to create discovery config for %s", tmpl->component);
        return ESP_FAIL;
    }

    /* Build unique_id for topic */
    char unique_id[80];
    build_unique_id(dev, tmpl, unique_id, sizeof(unique_id));

    /* Build discovery topic */
    char topic[MQTT_TOPIC_MAX_LEN];
    esp_err_t ret = build_discovery_topic(tmpl->component, unique_id, topic, sizeof(topic));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build discovery topic");
        cJSON_Delete(config);
        return ret;
    }

    /* Try to use JSON pool buffer for serialization */
    buffer_pool_t json_pool = foundation_get_json_pool();
    char *pool_buffer = NULL;
    char *json_str = NULL;
    bool used_pool = false;

    if (json_pool != NULL) {
        pool_buffer = (char *)mem_pool_acquire(json_pool);
        if (pool_buffer != NULL) {
            /* Try to print JSON directly into pool buffer */
            if (cJSON_PrintPreallocated(config, pool_buffer,
                                         JSON_POOL_BUFFER_SIZE, false)) {
                json_str = pool_buffer;
                used_pool = true;
                ESP_LOGD(TAG, "Using pool buffer for discovery publish");
            } else {
                /* Buffer too small, release and fall back to malloc */
                mem_pool_release(json_pool, pool_buffer);
                pool_buffer = NULL;
                ESP_LOGD(TAG, "Pool buffer too small for discovery, using malloc");
            }
        }
    }

    /* Fall back to regular allocation if pool not available or too small */
    if (!used_pool) {
        json_str = cJSON_PrintUnformatted(config);
    }

    cJSON_Delete(config);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize discovery JSON");
        return ESP_FAIL;
    }

    /* Prepare heap-allocated copies for ownership transfer via event bus.
     * The handler (publish_ha_discovery in mqtt_event_handler.c) will free()
     * both topic and payload_json after use. */

    /* Topic is stack-based char[] - must strdup for ownership transfer */
    char *topic_heap = strdup(topic);
    if (topic_heap == NULL) {
        ESP_LOGE(TAG, "Failed to strdup topic for discovery publish");
        if (used_pool) {
            mem_pool_release(json_pool, pool_buffer);
        } else {
            free(json_str);
        }
        return ESP_ERR_NO_MEM;
    }

    /* json_str: if from pool, must strdup then release pool buffer.
     * If from cJSON_PrintUnformatted, already heap-allocated - transfer directly. */
    char *payload_heap = NULL;
    if (used_pool) {
        payload_heap = strdup(json_str);
        mem_pool_release(json_pool, pool_buffer);
        if (payload_heap == NULL) {
            ESP_LOGE(TAG, "Failed to strdup pool JSON for discovery publish");
            free(topic_heap);
            return ESP_ERR_NO_MEM;
        }
    } else {
        /* cJSON_PrintUnformatted result - already heap-allocated, transfer ownership */
        payload_heap = json_str;
    }

    /* Publish via event bus - ownership of topic_heap and payload_heap transfers */
    evt_ha_discovery_publish_t evt = {
        .topic = topic_heap,
        .payload_json = payload_heap,
        .qos = 1,
        .retain = true
    };
    ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Published %s discovery: %s", tmpl->component, dev->friendly_name);
    } else {
        /* event_publish failed - ownership was NOT transferred, we must free */
        free(topic_heap);
        free(payload_heap);
        ESP_LOGE(TAG, "Failed to publish %s discovery: %s",
                 tmpl->component, esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================================
 * Converter Expose-Based Discovery
 * ============================================================================
 * Publishes discovery for exposes not covered by the capability-based templates.
 * This handles generic sensors (action, angle, etc.) and select entities
 * that don't map to a DEV_CAP_* flag.
 * ============================================================================ */

/**
 * @brief Check if an expose is already covered by a capability-based template
 *
 * Capability templates already handle: battery, temperature, humidity, pressure,
 * vibration, contact, motion, water_leak, smoke, CO, power, energy, voltage, current,
 * on_off, light, lock, cover, fan, climate.
 */
static bool expose_covered_by_capability(const zb_expose_t *expose, uint32_t dev_caps)
{
    /* All binary sensors with device_class are covered by capability templates */
    if (expose->type == ZB_EXPOSE_BINARY_SENSOR && expose->device_class != NULL) {
        return true;
    }

    /* Sensors with recognized device_class are covered */
    if (expose->type == ZB_EXPOSE_SENSOR && expose->device_class != NULL) {
        const char *dc = expose->device_class;
        if (strcmp(dc, "battery") == 0 || strcmp(dc, "temperature") == 0 ||
            strcmp(dc, "humidity") == 0 || strcmp(dc, "pressure") == 0 ||
            strcmp(dc, "power") == 0 || strcmp(dc, "energy") == 0 ||
            strcmp(dc, "voltage") == 0 || strcmp(dc, "current") == 0) {
            return true;
        }
    }

    /* Actuator types covered by capabilities */
    if (expose->type == ZB_EXPOSE_LIGHT || expose->type == ZB_EXPOSE_SWITCH ||
        expose->type == ZB_EXPOSE_LOCK || expose->type == ZB_EXPOSE_COVER ||
        expose->type == ZB_EXPOSE_FAN || expose->type == ZB_EXPOSE_CLIMATE) {
        return true;
    }

    return false;
}

/**
 * @brief Map expose type to HA component string
 */
static const char *expose_type_to_component(zb_expose_type_t type)
{
    switch (type) {
        case ZB_EXPOSE_SENSOR:        return "sensor";
        case ZB_EXPOSE_BINARY_SENSOR: return "binary_sensor";
        case ZB_EXPOSE_SELECT:        return "select";
        case ZB_EXPOSE_NUMBER:        return "number";
        case ZB_EXPOSE_BUTTON:        return "button";
        case ZB_EXPOSE_TEXT:          return "text";
        default:                      return NULL;
    }
}

/**
 * @brief Publish a single discovery entity from a converter expose
 */
static esp_err_t publish_expose_entity(const device_t *dev, const zb_expose_t *expose)
{
    const char *component = expose_type_to_component(expose->type);
    if (component == NULL) {
        return ESP_OK;  /* Skip unsupported types */
    }

    const char *key = expose->name;
    if (key == NULL) {
        return ESP_OK;  /* Skip unnamed exposes */
    }

    char ieee_str[20];
    device_id_to_ieee_str(dev->id, ieee_str, sizeof(ieee_str));

    /* Unique ID */
    char unique_id[80];
    snprintf(unique_id, sizeof(unique_id), "%s_%s", ieee_str, key);

    /* Discovery topic */
    char topic[MQTT_TOPIC_MAX_LEN];
    esp_err_t ret = build_discovery_topic(component, unique_id, topic, sizeof(topic));
    if (ret != ESP_OK) {
        return ret;
    }

    /* Build config JSON */
    cJSON *config = cJSON_CreateObject();
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Entity name */
    char name[64];
    const char *dev_name = dev->friendly_name;
    if (dev_name[0] == '\0' ||
        (dev_name[0] == '0' && dev_name[1] == 'x' && dev->model[0] != '\0')) {
        dev_name = dev->model;
    }
    char cap_key[32];
    strlcpy(cap_key, key, sizeof(cap_key));
    if (cap_key[0] >= 'a' && cap_key[0] <= 'z') {
        cap_key[0] = cap_key[0] - 'a' + 'A';
    }
    /* Replace underscores with spaces for display */
    for (char *p = cap_key; *p; p++) {
        if (*p == '_') *p = ' ';
    }
    snprintf(name, sizeof(name), "%s %s", dev_name, cap_key);

    cJSON_AddStringToObject(config, "name", name);
    cJSON_AddStringToObject(config, "unique_id", unique_id);

    /* State topic */
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "zigbee2mqtt/%s", dev->friendly_name);
    cJSON_AddStringToObject(config, "state_topic", state_topic);

    /* Value template */
    char value_tmpl[80];
    snprintf(value_tmpl, sizeof(value_tmpl), "{{ value_json.%s }}", key);
    cJSON_AddStringToObject(config, "value_template", value_tmpl);

    /* Device class */
    if (expose->device_class != NULL) {
        cJSON_AddStringToObject(config, "device_class", expose->device_class);
    }

    /* Unit of measurement */
    if (expose->unit != NULL) {
        cJSON_AddStringToObject(config, "unit_of_measurement", expose->unit);
    }

    /* State class */
    if (expose->state_class != NULL) {
        cJSON_AddStringToObject(config, "state_class", expose->state_class);
    }

    /* Select-specific: add command_topic and options placeholder */
    if (expose->type == ZB_EXPOSE_SELECT) {
        char cmd_topic[128];
        snprintf(cmd_topic, sizeof(cmd_topic), "zigbee2mqtt/%s/set", dev->friendly_name);
        cJSON_AddStringToObject(config, "command_topic", cmd_topic);

        char cmd_tmpl[80];
        snprintf(cmd_tmpl, sizeof(cmd_tmpl), "{\"%s\": \"{{ value }}\"}", key);
        cJSON_AddStringToObject(config, "command_template", cmd_tmpl);

        /* Sensitivity options for Aqara vibration sensor */
        if (strcmp(key, "sensitivity") == 0) {
            cJSON *options = cJSON_CreateArray();
            cJSON_AddItemToArray(options, cJSON_CreateString("low"));
            cJSON_AddItemToArray(options, cJSON_CreateString("medium"));
            cJSON_AddItemToArray(options, cJSON_CreateString("high"));
            cJSON_AddItemToObject(config, "options", options);
        }
    }

    /* Device info */
    cJSON *dev_info = create_device_info(dev);
    if (dev_info != NULL) {
        cJSON_AddItemToObject(config, "device", dev_info);
    }

    /* Availability topic + template (matches capability-based discovery) */
    char avail_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "zigbee2mqtt/%s/availability", dev->friendly_name);
    cJSON_AddStringToObject(config, "availability_topic", avail_topic);
    cJSON_AddStringToObject(config, "availability_template", "{{ value_json.state }}");

    /* Serialize and publish via event bus */
    char *json_str = cJSON_PrintUnformatted(config);
    cJSON_Delete(config);
    if (json_str == NULL) {
        return ESP_FAIL;
    }

    char *topic_heap = strdup(topic);
    if (topic_heap == NULL) {
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_heap,
        .payload_json = json_str,
        .qos = 1,
        .retain = true
    };
    ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));
    if (ret != ESP_OK) {
        free(topic_heap);
        free(json_str);
    }

    return ret;
}

/**
 * @brief Publish discovery for converter exposes not covered by capability templates
 *
 * Iterates the converter's expose list and publishes discovery for entities
 * that don't have a corresponding DEV_CAP_* flag (e.g. action, angle sensors).
 */
static esp_err_t publish_converter_expose_discovery(const device_t *dev)
{
    if (dev->protocol != DEV_PROTOCOL_ZIGBEE || dev->proto.zigbee.converter == NULL) {
        return ESP_OK;
    }

    const zb_converter_def_t *conv = (const zb_converter_def_t *)dev->proto.zigbee.converter;
    if (conv->exposes == NULL || conv->expose_count == 0) {
        return ESP_OK;
    }

    esp_err_t first_error = ESP_OK;

    for (uint8_t i = 0; i < conv->expose_count; i++) {
        const zb_expose_t *expose = &conv->exposes[i];

        /* Skip exposes already handled by capability templates */
        if (expose_covered_by_capability(expose, dev->capabilities)) {
            continue;
        }

        esp_err_t ret = publish_expose_entity(dev, expose);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }

    if (first_error == ESP_OK && conv->expose_count > 0) {
        ESP_LOGD(TAG, "Published converter expose discovery for %s", dev->friendly_name);
    }

    return first_error;
}

/**
 * @brief Check if device capabilities match template requirements
 *
 * @param[in] dev_caps Device capability bitmask
 * @param[in] tmpl Template to check
 * @return true if device has all required capabilities
 */
static bool device_matches_template(uint32_t dev_caps, const ha_entity_template_t *tmpl)
{
    return (dev_caps & tmpl->required_caps) == tmpl->required_caps;
}

/* ============================================================================
 * Event Handler
 * ============================================================================ */

/**
 * @brief Event bus handler for device events
 *
 * @param[in] type Event type
 * @param[in] data Event data (device_id_t pointer)
 * @param[in] data_size Size of event data
 * @param[in] user_ctx User context (unused)
 */
static void device_event_handler(event_type_t type, void *data, size_t data_size, void *user_ctx)
{
    (void)user_ctx;

    if (!s_enabled) {
        return;
    }

    device_id_t id = 0;

    /* Handle different event types with their specific payload structures */
    switch (type) {
        case EVT_DEVICE_JOINED:
            if (data != NULL && data_size >= sizeof(evt_device_joined_t)) {
                evt_device_joined_t *evt = (evt_device_joined_t *)data;
                id = evt->ieee_addr;
            }
            break;

        case EVT_DEVICE_INTERVIEWED:
            if (data != NULL && data_size >= sizeof(evt_device_interviewed_t)) {
                evt_device_interviewed_t *evt = (evt_device_interviewed_t *)data;
                id = evt->ieee_addr;
                if (!evt->success) {
                    ESP_LOGD(TAG, "Interview failed for 0x%016" PRIx64 ", skipping discovery", id);
                    return;
                }
            }
            break;

        case EVT_DEVICE_LEFT:
            if (data != NULL && data_size >= sizeof(evt_device_left_t)) {
                evt_device_left_t *evt = (evt_device_left_t *)data;
                id = evt->ieee_addr;
                ESP_LOGI(TAG, "Device left: removing discovery for 0x%016" PRIx64, id);
                ha_discovery_ng_remove_device(id);
                return;  /* No further processing needed for left devices */
            }
            break;

        default:
            ESP_LOGW(TAG, "Unhandled event type: %s", event_type_to_str(type));
            return;
    }

    if (id == 0) {
        ESP_LOGW(TAG, "Invalid event data for %s", event_type_to_str(type));
        return;
    }

    ESP_LOGI(TAG, "Device event %s for 0x%016" PRIx64,
             event_type_to_str(type), id);

    /* Small delay to allow device state to stabilize */
    vTaskDelay(pdMS_TO_TICKS(50));

    ha_discovery_ng_on_device_ready(id);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t ha_discovery_ng_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing NG discovery (templates: %zu, cache: %d entries)",
             TEMPLATE_COUNT, DISCOVERY_CACHE_SIZE);

    /* Initialize discovery cache */
    memset(s_discovery_cache, 0, sizeof(s_discovery_cache));
    s_cache_hits = 0;
    s_cache_misses = 0;

    /* Subscribe to device events */
    esp_err_t ret = event_subscribe(EVT_DEVICE_JOINED, device_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_DEVICE_JOINED: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = event_subscribe(EVT_DEVICE_INTERVIEWED, device_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_DEVICE_INTERVIEWED: %s", esp_err_to_name(ret));
        event_unsubscribe(EVT_DEVICE_JOINED, device_event_handler);
        return ret;
    }

    ret = event_subscribe(EVT_DEVICE_LEFT, device_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to EVT_DEVICE_LEFT: %s", esp_err_to_name(ret));
        event_unsubscribe(EVT_DEVICE_JOINED, device_event_handler);
        event_unsubscribe(EVT_DEVICE_INTERVIEWED, device_event_handler);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "NG discovery initialized (enabled: %s)", s_enabled ? "yes" : "no");

    return ESP_OK;
}

esp_err_t ha_discovery_ng_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Unsubscribe from events */
    event_unsubscribe(EVT_DEVICE_JOINED, device_event_handler);
    event_unsubscribe(EVT_DEVICE_INTERVIEWED, device_event_handler);
    event_unsubscribe(EVT_DEVICE_LEFT, device_event_handler);

    /* Clear discovery cache */
    memset(s_discovery_cache, 0, sizeof(s_discovery_cache));

    /* Log final cache statistics */
    ESP_LOGI(TAG, "NG discovery deinitialized (cache stats: hits=%" PRIu32 ", misses=%" PRIu32 ")",
             s_cache_hits, s_cache_misses);

    s_initialized = false;

    return ESP_OK;
}

esp_err_t ha_discovery_ng_publish_device(device_id_t id)
{
    if (!s_enabled) {
        ESP_LOGD(TAG, "Discovery disabled, skipping device 0x%016" PRIx64, id);
        return ESP_OK;
    }

    REQUIRE_MQTT_CONNECTED(ESP_ERR_INVALID_STATE);

    /* Get device from registry */
    device_t *dev = device_registry_get(id);
    if (dev == NULL) {
        ESP_LOGW(TAG, "Device 0x%016" PRIx64 " not found in registry", id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Skip virtual/ESPHome devices - they are handled by ha_bridge_discovery */
    if (dev->protocol == DEV_PROTOCOL_VIRTUAL) {
        ESP_LOGD(TAG, "Skipping virtual device 0x%016" PRIx64, id);
        return ESP_OK;
    }

    /* Lazy converter rebind: after reboot, converters aren't persisted.
     * Try to find and bind converter so that converter-derived capabilities
     * (e.g. DEV_CAP_VIBRATION) are set before we check templates. */
    if (dev->protocol == DEV_PROTOCOL_ZIGBEE &&
        dev->proto.zigbee.converter == NULL) {
        const zb_converter_def_t *conv = NULL;

        /* Strategy 1: Find converter by manufacturer/model (works if persisted) */
        if (dev->manufacturer[0] != '\0') {
            conv = zb_converter_find(dev->manufacturer, dev->model);
        }

        /* Strategy 2: Look up from converter registry binding table.
         * The converter may already be bound via attribute reports even if
         * manufacturer/model aren't in the device_t (e.g. after NVS clear). */
        if (conv == NULL && dev->proto.zigbee.short_addr != 0) {
            conv = zb_converter_get(dev->proto.zigbee.short_addr);
        }

        if (conv != NULL) {
            dev->proto.zigbee.converter = (void *)conv;
            uint32_t conv_caps = zb_converter_get_capabilities(conv);
            if (conv_caps != 0 && (dev->capabilities & conv_caps) != conv_caps) {
                dev->capabilities |= conv_caps;
                ESP_LOGI(TAG, "Converter rebind for %s: caps now 0x%08" PRIx32,
                         dev->friendly_name, dev->capabilities);
            }
        }
    }

    /* Compute discovery hash and check cache */
    uint32_t hash = compute_discovery_hash(dev);

    if (cache_check_valid(id, hash)) {
        s_cache_hits++;
        ESP_LOGD(TAG, "Discovery cache hit for %s (hash: 0x%08" PRIx32 ")",
                 dev->friendly_name, hash);
        return ESP_OK;
    }

    s_cache_misses++;
    ESP_LOGI(TAG, "Publishing discovery for %s (caps: 0x%08" PRIx32 ", hash: 0x%08" PRIx32 ")",
             dev->friendly_name, dev->capabilities, hash);

    uint32_t caps = dev->capabilities;
    esp_err_t ret = ESP_OK;
    esp_err_t first_error = ESP_OK;

    /* Track which actuator component we've published to avoid duplicates */
    bool published_light = false;
    bool published_switch = false;

    /* Iterate through templates */
    for (size_t i = 0; i < TEMPLATE_COUNT; i++) {
        const ha_entity_template_t *tmpl = &s_templates[i];

        if (!device_matches_template(caps, tmpl)) {
            continue;
        }

        /* For actuators, prevent publishing both light and switch for same on/off capability */
        if (tmpl->is_actuator) {
            if (strcmp(tmpl->component, "light") == 0) {
                if (published_light) {
                    continue;  /* Skip duplicate light */
                }
                /* If device has brightness, prefer light over switch */
                if (caps & DEV_CAP_BRIGHTNESS) {
                    published_light = true;
                    published_switch = true;  /* Prevent switch from being published */
                } else {
                    /* Simple on/off - check if we should publish as switch instead */
                    /* Skip light if no brightness (switch will be published) */
                    continue;
                }
            } else if (strcmp(tmpl->component, "switch") == 0) {
                if (published_switch || published_light) {
                    continue;  /* Skip if already published light or switch */
                }
                /* Only publish switch if no brightness capability */
                if (caps & DEV_CAP_BRIGHTNESS) {
                    continue;  /* Light was or will be published instead */
                }
                published_switch = true;
            }
        }

        ret = publish_entity_discovery(dev, tmpl);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }

    /* Publish additional entities from converter exposes not covered by
     * capability templates (e.g. action, angle, sensitivity for vibration sensor) */
    ret = publish_converter_expose_discovery(dev);
    if (ret != ESP_OK && first_error == ESP_OK) {
        first_error = ret;
    }

    /* Publish Tuya driver-specific discovery entities (e.g. Fingerbot mode,
     * buttons, numbers, battery) if a Tuya driver is bound to this device. */
    if (dev->protocol == DEV_PROTOCOL_ZIGBEE && dev->proto.zigbee.short_addr != 0) {
        const tuya_device_driver_t *drv = tuya_driver_get(dev->proto.zigbee.short_addr);
        if (drv != NULL && drv->publish_discovery != NULL) {
            ret = drv->publish_discovery(dev);
            if (ret != ESP_OK && first_error == ESP_OK) {
                first_error = ret;
            }
        }
    }

    if (first_error == ESP_OK) {
        /* Update cache on successful publish */
        cache_update_entry(id, hash);
        ESP_LOGI(TAG, "Published discovery for %s (cached)", dev->friendly_name);
    } else {
        ESP_LOGW(TAG, "Some discovery publishes failed for %s", dev->friendly_name);
    }

    return first_error;
}

/**
 * @brief Iterator context for collecting device IDs
 */
typedef struct {
    device_id_t *ids;
    size_t count;
    size_t max;
} collect_ids_ctx_t;

/**
 * @brief Iterator callback that collects device IDs without calling back
 *        into the registry (avoids non-recursive mutex deadlock).
 */
static bool collect_ids_iterator(device_t *dev, void *ctx)
{
    collect_ids_ctx_t *collect = (collect_ids_ctx_t *)ctx;
    if (collect->count < collect->max) {
        collect->ids[collect->count++] = dev->id;
    }
    return true;
}

esp_err_t ha_discovery_ng_publish_all(void)
{
    if (!s_enabled) {
        ESP_LOGI(TAG, "Discovery disabled, skipping publish all");
        return ESP_OK;
    }

    REQUIRE_MQTT_CONNECTED(ESP_ERR_INVALID_STATE);

    size_t count = device_registry_count();
    ESP_LOGI(TAG, "Publishing discovery for %zu devices (batch size: %d)",
             count, HA_DISCOVERY_NG_BATCH_SIZE);

    /* Collect device IDs first, then publish outside the registry mutex.
     * device_registry_iterate() holds the mutex, and publish_device() also
     * needs it — calling publish from inside iterate deadlocks. */
    device_id_t ids[DEVICE_REGISTRY_MAX_DEVICES];
    collect_ids_ctx_t ctx = { .ids = ids, .count = 0, .max = DEVICE_REGISTRY_MAX_DEVICES };
    device_registry_iterate(collect_ids_iterator, &ctx);

    uint8_t batch_counter = 0;
    for (size_t i = 0; i < ctx.count; i++) {
        ha_discovery_ng_publish_device(ids[i]);
        batch_counter++;
        if (batch_counter >= HA_DISCOVERY_NG_BATCH_SIZE) {
            vTaskDelay(pdMS_TO_TICKS(HA_DISCOVERY_NG_BATCH_DELAY_MS));
            batch_counter = 0;
        }
    }

    return ESP_OK;
}

esp_err_t ha_discovery_ng_remove_device(device_id_t id)
{
    REQUIRE_MQTT_CONNECTED(ESP_ERR_INVALID_STATE);

    device_t *dev = device_registry_get(id);
    if (dev == NULL) {
        ESP_LOGW(TAG, "Device 0x%016" PRIx64 " not found for removal", id);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Removing discovery for %s", dev->friendly_name);

    uint32_t caps = dev->capabilities;
    char unique_id[80];
    char topic[MQTT_TOPIC_MAX_LEN];

    /* Iterate through templates and remove matching entities */
    for (size_t i = 0; i < TEMPLATE_COUNT; i++) {
        const ha_entity_template_t *tmpl = &s_templates[i];

        if (!device_matches_template(caps, tmpl)) {
            continue;
        }

        build_unique_id(dev, tmpl, unique_id, sizeof(unique_id));

        if (build_discovery_topic(tmpl->component, unique_id, topic, sizeof(topic)) == ESP_OK) {
            /* Ownership transfer: heap-allocate both topic and empty payload.
             * The handler (publish_ha_discovery) will free() both after use. */
            char *topic_heap = strdup(topic);
            char *payload_heap = strdup("");
            if (topic_heap == NULL || payload_heap == NULL) {
                free(topic_heap);
                free(payload_heap);
                ESP_LOGE(TAG, "Failed to alloc for discovery removal");
                continue;
            }

            evt_ha_discovery_publish_t evt = {
                .topic = topic_heap,
                .payload_json = payload_heap,
                .qos = 1,
                .retain = true
            };
            esp_err_t pub_ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));
            if (pub_ret != ESP_OK) {
                /* event_publish failed - ownership NOT transferred, free here */
                free(topic_heap);
                free(payload_heap);
            }
        }
    }

    /* Remove from discovery cache */
    ha_discovery_invalidate_device(id);

    return ESP_OK;
}

void ha_discovery_ng_on_device_ready(device_id_t id)
{
    if (!s_enabled) {
        return;
    }

    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected, skipping discovery for 0x%016" PRIx64, id);
        return;
    }

    esp_err_t ret = ha_discovery_ng_publish_device(id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish discovery for 0x%016" PRIx64 ": %s",
                 id, esp_err_to_name(ret));
    }
}

esp_err_t ha_discovery_ng_set_enabled(bool enable)
{
    s_enabled = enable;
    ESP_LOGI(TAG, "NG discovery %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

bool ha_discovery_ng_is_enabled(void)
{
    return s_enabled;
}

size_t ha_discovery_ng_get_template_count(void)
{
    return TEMPLATE_COUNT;
}

const ha_entity_template_t *ha_discovery_ng_get_template(size_t index)
{
    if (index >= TEMPLATE_COUNT) {
        return NULL;
    }
    return &s_templates[index];
}

/* ============================================================================
 * Discovery Cache Public API
 * ============================================================================ */

esp_err_t ha_discovery_invalidate_cache(void)
{
    ESP_LOGI(TAG, "Invalidating entire discovery cache");

    memset(s_discovery_cache, 0, sizeof(s_discovery_cache));

    /* Reset statistics */
    s_cache_hits = 0;
    s_cache_misses = 0;

    return ESP_OK;
}

esp_err_t ha_discovery_invalidate_device(device_id_t id)
{
    discovery_cache_entry_t *entry = cache_find_entry(id);
    if (entry == NULL) {
        ESP_LOGD(TAG, "Device 0x%016" PRIx64 " not in cache", id);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGD(TAG, "Invalidating cache for device 0x%016" PRIx64, id);

    /* Clear the entry by setting id to 0 */
    entry->id = 0;
    entry->capabilities_hash = 0;
    entry->last_published = 0;

    return ESP_OK;
}

bool ha_discovery_is_cached(device_id_t id, uint32_t capabilities_hash)
{
    return cache_check_valid(id, capabilities_hash);
}

void ha_discovery_get_cache_stats(size_t *entries_used, uint32_t *cache_hits, uint32_t *cache_misses)
{
    if (entries_used != NULL) {
        size_t count = 0;
        for (size_t i = 0; i < DISCOVERY_CACHE_SIZE; i++) {
            if (s_discovery_cache[i].id != 0) {
                count++;
            }
        }
        *entries_used = count;
    }

    if (cache_hits != NULL) {
        *cache_hits = s_cache_hits;
    }

    if (cache_misses != NULL) {
        *cache_misses = s_cache_misses;
    }
}
