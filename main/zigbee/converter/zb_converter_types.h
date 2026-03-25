/**
 * @file zb_converter_types.h
 * @brief Zigbee Converter Database Type Definitions
 *
 * Defines types for the converter system that maps Zigbee devices
 * to Home Assistant entities with proper state conversion.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef ZB_CONVERTER_TYPES_H
#define ZB_CONVERTER_TYPES_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration for Tuya driver integration */
struct tuya_device_driver;

/** @brief Maximum converters that can be registered */
#define ZB_CONVERTER_MAX_DEFINITIONS    128

/** @brief Maximum device bindings */
#define ZB_CONVERTER_MAX_BINDINGS       64

/** @brief Expose type enumeration - what HA entity to create */
typedef enum {
    ZB_EXPOSE_LIGHT = 0,
    ZB_EXPOSE_SWITCH,
    ZB_EXPOSE_SENSOR,
    ZB_EXPOSE_BINARY_SENSOR,
    ZB_EXPOSE_COVER,
    ZB_EXPOSE_LOCK,
    ZB_EXPOSE_CLIMATE,
    ZB_EXPOSE_FAN,
    ZB_EXPOSE_SELECT,
    ZB_EXPOSE_NUMBER,
    ZB_EXPOSE_BUTTON,
    ZB_EXPOSE_TEXT,
    ZB_EXPOSE_MAX
} zb_expose_type_t;

/** @brief Feature bitmask flags */
#define ZB_FEATURE_BRIGHTNESS       (1 << 0)
#define ZB_FEATURE_COLOR_TEMP       (1 << 1)
#define ZB_FEATURE_COLOR_XY         (1 << 2)
#define ZB_FEATURE_COLOR_HS         (1 << 3)
#define ZB_FEATURE_TRANSITION       (1 << 4)
#define ZB_FEATURE_POSITION         (1 << 5)
#define ZB_FEATURE_TILT             (1 << 6)
#define ZB_FEATURE_PRESET           (1 << 7)
#define ZB_FEATURE_SPEED            (1 << 8)
#define ZB_FEATURE_EFFECT           (1 << 9)
#define ZB_FEATURE_POWER_ON         (1 << 10)

/** @brief Quirk flags for device-specific workarounds (behavior only) */
#define ZB_QUIRK_NONE               0
#define ZB_QUIRK_NO_DEFAULT_RESPONSE (1 << 0)
#define ZB_QUIRK_INVERT_COVER       (1 << 1)
#define ZB_QUIRK_SWAP_COLOR_XY      (1 << 2)
#define ZB_QUIRK_NEEDS_CONFIGURE    (1 << 3)
#define ZB_QUIRK_TUYA_DEVICE        (1 << 4)
#define ZB_QUIRK_XIAOMI_INIT        (1 << 5)
#define ZB_QUIRK_TUYA_MCU_INIT      (1 << 6)
#define ZB_QUIRK_CUSTOM_BIND        (1 << 7)
#define ZB_QUIRK_CUSTOM_REPORTING   (1 << 8)
#define ZB_QUIRK_MULTI_ENDPOINT     (1 << 9)

/** @brief Expose access flags (from z2m) */
#define EA_STATE        1       /**< Read-only state */
#define EA_SET          2       /**< Write-only command */
#define EA_STATE_SET    3       /**< Read + Write */
#define EA_GET          4       /**< Supports explicit GET */
#define EA_ALL          7       /**< STATE + SET + GET */

/**
 * @brief Entity expose definition (tagged union)
 * Describes what a device exposes to Home Assistant.
 * Type-specific data stored in union to avoid fat struct waste.
 */
typedef struct {
    zb_expose_type_t type;      /**< HA component type */
    const char *name;           /**< Entity name suffix (NULL = primary) */
    const char *property;       /**< JSON key for state (NULL = use name) */
    uint8_t endpoint;           /**< Endpoint (0 = primary) */
    uint8_t access;             /**< Access flags: EA_STATE, EA_SET, EA_GET */
    uint32_t features;          /**< Feature bitmask */
    const char *device_class;   /**< HA device_class (NULL = default) */
    const char *unit;           /**< Unit of measurement (NULL = none) */
    const char *state_class;    /**< HA state_class (NULL = none) */
    const char *icon;           /**< MDI icon (NULL = platform default) */
    const char *description;    /**< Tooltip / description (NULL = none) */
    union {
        struct {                        /**< NUMBER, SENSOR range info */
            float min, max, step;
        } numeric;
        struct {                        /**< SELECT option list */
            const char **values;
            uint8_t count;
        } select;
        struct {                        /**< BINARY_SENSOR, SWITCH custom labels */
            const char *val_on;         /**< Custom ON label (NULL = default) */
            const char *val_off;        /**< Custom OFF label (NULL = default) */
        } binary;
    } ext;                      /**< Type-specific extension (zero-init when unused) */
} zb_expose_t;

/** @brief Get the state property key for an expose (property if set, else name) */
static inline const char *zb_expose_property(const zb_expose_t *e)
{
    return e->property ? e->property : e->name;
}

/**
 * @brief fromZigbee converter: ZCL attribute -> JSON
 */
typedef struct {
    uint16_t cluster_id;        /**< ZCL cluster ID */
    uint16_t attr_id;           /**< Attribute ID (0xFFFF = any in cluster) */
    uint8_t endpoint;           /**< Endpoint filter (0 = any/primary) */
    const char *json_key;       /**< JSON key for state */
    esp_err_t (*convert)(const void *raw, size_t len, cJSON *json, const char *key);
} zb_from_zigbee_t;

/**
 * @brief toZigbee converter: JSON command -> ZCL
 */
typedef struct {
    const char *json_key;       /**< JSON key to match */
    uint8_t endpoint;           /**< Target endpoint (0 = use device primary) */
    uint16_t cluster_id;        /**< Target ZCL cluster */
    esp_err_t (*convert)(uint16_t short_addr, uint8_t endpoint, const cJSON *value);
} zb_to_zigbee_t;

/**
 * @brief Reporting configuration override
 */
typedef struct {
    uint16_t cluster_id;
    uint16_t attr_id;
    uint16_t min_interval;      /**< Minimum reporting interval (seconds) */
    uint16_t max_interval;      /**< Maximum reporting interval (seconds) */
    uint16_t reportable_change; /**< Minimum change to trigger report */
} zb_reporting_override_t;

/**
 * @brief Tuya DP entry — maps dp_id to semantic key + type info
 *
 * Used by fz_tuya_dp/tz_tuya_dp for semantic DP conversion.
 * dp_type values match TUYA_DP_TYPE_* (0=raw,1=bool,2=value,3=string,4=enum,5=bitmap).
 */
typedef struct {
    uint8_t dp_id;              /**< Tuya DataPoint ID (1-255) */
    uint8_t dp_type;            /**< TUYA_DP_TYPE_* constant */
    const char *key;            /**< Semantic key, e.g. "state", "temperature" (interned) */
    float scale;                /**< Multiplier for numeric DPs (0.0 = no scaling) */
    float min, max;             /**< Value range (0,0 = unclamped) */
    const char **enum_names;    /**< Enum display values (interned), NULL if not enum */
    const uint8_t *enum_values; /**< Corresponding raw enum values */
    uint8_t enum_count;         /**< Number of enum entries */
} tuya_dp_entry_t;

/**
 * @brief Complete converter definition for a device model
 * Stored as const in Flash (.rodata)
 */
typedef struct {
    const char *manufacturer;               /**< Zigbee manufacturer string */
    const char *model;                      /**< Zigbee model identifier */
    const char *vendor;                     /**< Display vendor name */
    const char *description;                /**< Human-readable description */
    const zb_expose_t *exposes;             /**< Entity definitions */
    uint8_t expose_count;                   /**< Number of exposes */
    const zb_from_zigbee_t *from_zigbee;    /**< Attribute->JSON converters */
    uint8_t from_zigbee_count;
    const zb_to_zigbee_t *to_zigbee;        /**< JSON->ZCL converters */
    uint8_t to_zigbee_count;
    const zb_reporting_override_t *reporting; /**< Reporting overrides */
    uint8_t reporting_count;
    uint32_t quirk_flags;                   /**< Quirk flags */
    void (*init)(uint16_t short_addr);      /**< Device init callback */
    const struct tuya_device_driver *tuya_driver; /**< For Tuya devices */
    const tuya_dp_entry_t *tuya_dp_map;     /**< Tuya DP mappings (NULL if not Tuya) */
    uint8_t tuya_dp_count;                  /**< Number of DP entries */
} zb_converter_def_t;

#ifdef __cplusplus
}
#endif

#endif /* ZB_CONVERTER_TYPES_H */
