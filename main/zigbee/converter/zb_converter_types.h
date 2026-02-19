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

/** @brief Quirk flags for device-specific workarounds */
#define ZB_QUIRK_NONE               0
#define ZB_QUIRK_NO_DEFAULT_RESPONSE (1 << 0)
#define ZB_QUIRK_INVERT_COVER       (1 << 1)
#define ZB_QUIRK_SWAP_COLOR_XY      (1 << 2)
#define ZB_QUIRK_NEEDS_CONFIGURE    (1 << 3)
#define ZB_QUIRK_TUYA_DEVICE        (1 << 4)

/**
 * @brief Entity expose definition
 * Describes what a device exposes to Home Assistant
 */
typedef struct {
    zb_expose_type_t type;      /**< HA component type */
    const char *name;           /**< Entity name suffix (NULL = primary) */
    uint8_t endpoint;           /**< Endpoint (0 = primary) */
    uint32_t features;          /**< Feature bitmask */
    const char *device_class;   /**< HA device_class (NULL = default) */
    const char *unit;           /**< Unit of measurement (NULL = none) */
    const char *state_class;    /**< HA state_class (NULL = none) */
} zb_expose_t;

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
} zb_converter_def_t;

#ifdef __cplusplus
}
#endif

#endif /* ZB_CONVERTER_TYPES_H */
