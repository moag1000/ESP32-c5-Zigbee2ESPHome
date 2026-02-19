/**
 * @file zb_device_handler_types.h
 * @brief Zigbee Device Handler Type Definitions
 *
 * This header contains core type definitions (structs, enums, typedefs, and
 * type-related constants) for the Zigbee device handler module.
 *
 * Cluster-specific types have been moved to their respective cluster headers:
 *   - zb_cluster_hvac.h        (thermostat, fan control)
 *   - zb_cluster_electrical.h  (electrical measurement, metering)
 *   - zb_cluster_security.h    (door lock, IAS zone)
 *   - zb_cluster_closures.h    (window covering)
 *   - zb_cluster_measurement.h (illuminance, pressure, PM2.5)
 *   - zb_cluster_multistate.h  (multistate input/output/value)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_DEVICE_HANDLER_TYPES_H
#define ZB_DEVICE_HANDLER_TYPES_H

#include <stdint.h>
#include "esp_zigbee_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Device Limits
 * ============================================================================ */

/**
 * @brief Maximum number of devices supported
 */
#ifndef CONFIG_MAX_ZIGBEE_DEVICES
#define ZB_MAX_DEVICES 50
#else
#define ZB_MAX_DEVICES CONFIG_MAX_ZIGBEE_DEVICES
#endif

/* ============================================================================
 * String Length Constants
 * ============================================================================ */

/**
 * @brief Maximum friendly name length
 */
#define ZB_DEVICE_FRIENDLY_NAME_LEN 32

/**
 * @brief Maximum model name length
 */
#define ZB_DEVICE_MODEL_LEN 32

/**
 * @brief Maximum manufacturer name length
 */
#define ZB_DEVICE_MANUFACTURER_LEN 32

/* ============================================================================
 * Device Types
 * ============================================================================ */

/**
 * @brief Zigbee device type enumeration
 */
typedef enum {
    ZB_DEVICE_TYPE_UNKNOWN = 0,
    ZB_DEVICE_TYPE_ON_OFF_LIGHT,       /**< On/Off Light */
    ZB_DEVICE_TYPE_DIMMABLE_LIGHT,     /**< Dimmable Light */
    ZB_DEVICE_TYPE_COLOR_LIGHT,        /**< Color Light */
    ZB_DEVICE_TYPE_ON_OFF_SWITCH,      /**< On/Off Switch */
    ZB_DEVICE_TYPE_TEMP_SENSOR,        /**< Temperature Sensor */
    ZB_DEVICE_TYPE_HUMIDITY_SENSOR,    /**< Humidity Sensor */
    ZB_DEVICE_TYPE_MOTION_SENSOR,      /**< Motion Sensor */
    ZB_DEVICE_TYPE_DOOR_SENSOR,        /**< Door/Window Sensor */
    ZB_DEVICE_TYPE_PLUG,               /**< Smart Plug */
    ZB_DEVICE_TYPE_WINDOW_COVERING,    /**< Window Covering (Blinds/Shades) */
    ZB_DEVICE_TYPE_DOOR_LOCK,          /**< Door Lock */
    ZB_DEVICE_TYPE_THERMOSTAT,         /**< Thermostat/HVAC */
    ZB_DEVICE_TYPE_FAN,                /**< Fan Control */
    ZB_DEVICE_TYPE_DEHUMIDIFIER,       /**< Dehumidifier/HVAC */
    ZB_DEVICE_TYPE_ILLUMINANCE_SENSOR, /**< Illuminance Sensor */
    ZB_DEVICE_TYPE_PRESSURE_SENSOR,    /**< Pressure Sensor */
    ZB_DEVICE_TYPE_AIR_QUALITY_SENSOR, /**< Air Quality Sensor (PM2.5) */
    ZB_DEVICE_TYPE_ENERGY_METER,       /**< Energy Meter (kWh) */
    ZB_DEVICE_TYPE_GAS_METER,          /**< Gas Meter (m3) */
    ZB_DEVICE_TYPE_WATER_METER,        /**< Water Meter (L/gal) */
    ZB_DEVICE_TYPE_POWER_MONITOR,      /**< Power Monitor (Electrical Measurement) */
    ZB_DEVICE_TYPE_IAS_ZONE,           /**< IAS Zone (Generic Alarm Sensor) */
    ZB_DEVICE_TYPE_FIRE_SENSOR,        /**< Fire/Smoke Sensor (IAS Zone) */
    ZB_DEVICE_TYPE_WATER_LEAK_SENSOR,  /**< Water Leak Sensor (IAS Zone) */
    ZB_DEVICE_TYPE_GAS_SENSOR,         /**< Gas Sensor (IAS Zone) */
    ZB_DEVICE_TYPE_VIBRATION_SENSOR,   /**< Vibration/Movement Sensor (IAS Zone) */
    ZB_DEVICE_TYPE_OTHER               /**< Other Device Type */
} zb_device_type_t;

/* ============================================================================
 * Power Information
 * ============================================================================ */

/**
 * @brief Power source information structure
 */
typedef struct {
    uint8_t current_power_mode;         /**< Current power mode (0=sync, 1=periodic, 2=stimulated) */
    uint8_t available_power_sources;    /**< Available power sources bitmask */
    uint8_t current_power_source;       /**< Current power source bitmask */
    uint8_t current_power_source_level; /**< Power level (0=critical, 4=33%, 8=66%, 12=100%) */
    bool power_info_valid;              /**< True if power descriptor was successfully read */
} zb_power_info_t;

/* ============================================================================
 * Binary Output Cluster (0x0010) Types
 * ============================================================================ */

/**
 * @brief Binary Output Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_BINARY_OUTPUT             0x0010

/**
 * @brief Binary Output Cluster Attribute IDs
 */
#define ZB_ZCL_ATTR_BINARY_OUT_OF_SERVICE_ID        0x0051  /**< OutOfService (bool) */
#define ZB_ZCL_ATTR_BINARY_PRESENT_VALUE_ID         0x0055  /**< PresentValue (bool) */
#define ZB_ZCL_ATTR_BINARY_STATUS_FLAGS_ID          0x006F  /**< StatusFlags (bitmap8) */

/**
 * @brief Binary Status Flags bits (bitmap8)
 */
#define ZB_BINARY_STATUS_IN_ALARM       (1 << 0)   /**< Bit 0: In Alarm */
#define ZB_BINARY_STATUS_FAULT          (1 << 1)   /**< Bit 1: Fault */
#define ZB_BINARY_STATUS_OVERRIDDEN     (1 << 2)   /**< Bit 2: Overridden */
#define ZB_BINARY_STATUS_OUT_OF_SERVICE (1 << 3)   /**< Bit 3: Out of Service */

/**
 * @brief Binary Output state structure
 */
typedef struct {
    bool present_value;         /**< Current output state (ON/OFF) */
    bool out_of_service;        /**< Device out of service */
    uint8_t status_flags;       /**< Raw status flags bitmap */
    bool in_alarm;              /**< In alarm state */
    bool fault;                 /**< Fault detected */
    bool overridden;            /**< Overridden by local control */
} zb_binary_output_state_t;

/* ============================================================================
 * Binary Value Cluster (0x0011) Types
 * ============================================================================ */

/**
 * @brief Binary Value Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_BINARY_VALUE              0x0011

/**
 * @brief Binary Value state structure
 *
 * Uses same attribute IDs as Binary Output (0x0051, 0x0055, 0x006F)
 */
typedef struct {
    bool present_value;         /**< Current value (ON/OFF) */
    bool out_of_service;        /**< Value out of service */
    uint8_t status_flags;       /**< Raw status flags bitmap */
    bool in_alarm;              /**< In alarm state */
    bool fault;                 /**< Fault detected */
    bool overridden;            /**< Overridden by local control */
} zb_binary_value_state_t;

/* ============================================================================
 * Address Constants
 * ============================================================================ */

/**
 * @brief Marker for pending/unknown short address
 *
 * Used when device is loaded from NVS but hasn't communicated yet
 */
#define ZB_SHORT_ADDR_PENDING           0xFFFF

/* ============================================================================
 * Power Descriptor Types (API-007)
 * ============================================================================ */

/**
 * @brief Power Source Bitmask values
 *
 * These aliases map to the ESP-Zigbee-SDK constants for convenience.
 * From Zigbee Specification: Table 2.37 Power Source Field
 */
#define ZB_POWER_SOURCE_MAINS                   ESP_ZB_AF_NODE_POWER_SOURCE_CONSTANT_POWER       /**< Constant (mains) power */
#define ZB_POWER_SOURCE_RECHARGEABLE_BATTERY    ESP_ZB_AF_NODE_POWER_SOURCE_RECHARGEABLE_BATTERY /**< Rechargeable battery */
#define ZB_POWER_SOURCE_DISPOSABLE_BATTERY      ESP_ZB_AF_NODE_POWER_SOURCE_DISPOSABLE_BATTERY   /**< Disposable battery */

/**
 * @brief Power Level values
 *
 * These aliases map to the ESP-Zigbee-SDK constants for convenience.
 * From Zigbee Specification: Table 2.38 Current Power Source Level Field
 */
#define ZB_POWER_LEVEL_CRITICAL     ESP_ZB_AF_NODE_POWER_SOURCE_LEVEL_CRITICAL     /**< Critical (< 5%) */
#define ZB_POWER_LEVEL_33_PERCENT   ESP_ZB_AF_NODE_POWER_SOURCE_LEVEL_33_PERCENT   /**< 33% remaining */
#define ZB_POWER_LEVEL_66_PERCENT   ESP_ZB_AF_NODE_POWER_SOURCE_LEVEL_66_PERCENT   /**< 66% remaining */
#define ZB_POWER_LEVEL_100_PERCENT  ESP_ZB_AF_NODE_POWER_SOURCE_LEVEL_100_PERCENT  /**< 100% (full) */

/**
 * @brief Power Mode values
 *
 * From Zigbee Specification: Table 2.36 Current Power Mode Field
 */
#define ZB_POWER_MODE_SYNC_ON_WHEN_IDLE         0x00    /**< Receiver synchronized when idle */
#define ZB_POWER_MODE_PERIODIC_ON_WHEN_IDLE     0x01    /**< Receiver periodically on when idle */
#define ZB_POWER_MODE_STIMULATED_ON_WHEN_IDLE   0x02    /**< Receiver stimulated on when idle */

#ifdef __cplusplus
}
#endif

#endif /* ZB_DEVICE_HANDLER_TYPES_H */
