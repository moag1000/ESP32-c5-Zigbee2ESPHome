/**
 * @file unified_device.h
 * @brief Unified Device Model for ESP32-C5 Zigbee2MQTT NG
 *
 * Provides a protocol-agnostic device abstraction that supports Zigbee,
 * BLE, and virtual devices through a common interface. This enables
 * unified device management across different transport protocols.
 *
 * Features:
 *   - Protocol-agnostic device identification (IEEE/MAC addresses)
 *   - Capability bitmask for feature detection
 *   - Protocol-specific data via union
 *   - Availability tracking
 *
 * @copyright Copyright (c) 2024-2025
 * @license Apache License 2.0
 */

#ifndef UNIFIED_DEVICE_H
#define UNIFIED_DEVICE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Device Identification
 * ============================================================================ */

/**
 * @brief Universal device identifier
 *
 * Stores IEEE 802.15.4 address for Zigbee devices (64-bit)
 * or MAC address for BLE devices (48-bit, zero-padded).
 */
typedef uint64_t device_id_t;

/* ============================================================================
 * Device Protocol Types
 * ============================================================================ */

/**
 * @brief Supported device protocols
 */
typedef enum {
    DEV_PROTOCOL_ZIGBEE,    /**< Zigbee device via IEEE 802.15.4 */
    DEV_PROTOCOL_BLE,       /**< Bluetooth Low Energy device */
    DEV_PROTOCOL_VIRTUAL,   /**< Virtual/computed/aggregated entity */
} device_protocol_t;

/* ============================================================================
 * Device Capabilities (Bitmask)
 * ============================================================================ */

/**
 * @brief Device capability flags
 *
 * Bitmask indicating supported features of a device.
 * Multiple capabilities can be combined using bitwise OR.
 */
typedef enum {
    /* Lighting capabilities */
    DEV_CAP_ON_OFF       = (1 << 0),   /**< On/Off control */
    DEV_CAP_BRIGHTNESS   = (1 << 1),   /**< Brightness/dimming control */
    DEV_CAP_COLOR_TEMP   = (1 << 2),   /**< Color temperature control */
    DEV_CAP_COLOR_XY     = (1 << 3),   /**< CIE xy color control */

    /* Environmental sensors */
    DEV_CAP_TEMPERATURE  = (1 << 4),   /**< Temperature sensing */
    DEV_CAP_HUMIDITY     = (1 << 5),   /**< Humidity sensing */
    DEV_CAP_PRESSURE     = (1 << 6),   /**< Atmospheric pressure sensing */
    DEV_CAP_BATTERY      = (1 << 7),   /**< Battery level reporting */

    /* Security/safety sensors */
    DEV_CAP_MOTION       = (1 << 8),   /**< Motion/occupancy detection */
    DEV_CAP_CONTACT      = (1 << 9),   /**< Door/window contact sensing */
    DEV_CAP_VIBRATION    = (1 << 10),  /**< Vibration detection */
    DEV_CAP_WATER_LEAK   = (1 << 11),  /**< Water leak detection */
    DEV_CAP_SMOKE        = (1 << 12),  /**< Smoke detection */
    DEV_CAP_CO           = (1 << 13),  /**< Carbon monoxide detection */

    /* Energy monitoring */
    DEV_CAP_POWER        = (1 << 14),  /**< Power measurement (W) */
    DEV_CAP_ENERGY       = (1 << 15),  /**< Energy measurement (kWh) */
    DEV_CAP_VOLTAGE      = (1 << 16),  /**< Voltage measurement (V) */
    DEV_CAP_CURRENT      = (1 << 17),  /**< Current measurement (A) */

    /* Actuators */
    DEV_CAP_LOCK         = (1 << 18),  /**< Lock control */
    DEV_CAP_COVER        = (1 << 19),  /**< Cover/blind control */
    DEV_CAP_FAN          = (1 << 20),  /**< Fan control */
    DEV_CAP_CLIMATE      = (1 << 21),  /**< Climate/thermostat control */
} device_capability_t;

/* ============================================================================
 * Device Availability
 * ============================================================================ */

/**
 * @brief Device availability states
 */
typedef enum {
    DEV_AVAIL_UNKNOWN,  /**< Availability not yet determined */
    DEV_AVAIL_ONLINE,   /**< Device is online and responding */
    DEV_AVAIL_OFFLINE,  /**< Device is offline or unreachable */
} device_availability_t;

/* ============================================================================
 * Protocol-Specific Data Structures
 * ============================================================================ */

/**
 * @brief Maximum clusters stored in unified device
 *
 * Maximum clusters tracked per device in the NG registry.
 */
#define DEVICE_MAX_CLUSTERS 32

/**
 * @brief Zigbee device type enumeration
 *
 * Subset of the full Zigbee device type for the unified model.
 * The full enum is in zb_device_handler_types.h.
 */
typedef enum {
    DEVICE_ZB_TYPE_UNKNOWN = 0,
    DEVICE_ZB_TYPE_LIGHT,           /**< Any light (on/off, dimmable, color) */
    DEVICE_ZB_TYPE_SWITCH,          /**< On/Off Switch */
    DEVICE_ZB_TYPE_SENSOR,          /**< Any sensor (temp, humidity, motion, etc.) */
    DEVICE_ZB_TYPE_PLUG,            /**< Smart Plug */
    DEVICE_ZB_TYPE_COVER,           /**< Window Covering */
    DEVICE_ZB_TYPE_LOCK,            /**< Door Lock */
    DEVICE_ZB_TYPE_CLIMATE,         /**< Thermostat/HVAC */
    DEVICE_ZB_TYPE_FAN,             /**< Fan Control */
    DEVICE_ZB_TYPE_METER,           /**< Energy/Gas/Water Meter */
    DEVICE_ZB_TYPE_IAS_ZONE,        /**< IAS Zone (alarm sensors) */
    DEVICE_ZB_TYPE_OTHER,           /**< Other device type */
} device_zigbee_type_t;

/**
 * @brief Power source information for Zigbee devices
 *
 * Power descriptor fields from Zigbee Node Power Descriptor.
 */
typedef struct {
    uint8_t current_power_mode;         /**< Power mode (0=sync, 1=periodic, 2=stimulated) */
    uint8_t available_power_sources;    /**< Available power sources bitmask */
    uint8_t current_power_source;       /**< Current power source bitmask */
    uint8_t current_power_source_level; /**< Power level (0=critical, 4=33%, 8=66%, 12=100%) */
    bool power_info_valid;              /**< True if power descriptor was successfully read */
} device_power_info_t;

/**
 * @brief Availability tracking metadata for Zigbee devices
 *
 * Used by zb_availability.c for active/passive online checks.
 * Volatile state -- not persisted to NVS, reset on boot.
 * All fields default to 0/false after device_init() memset.
 */
typedef struct {
    uint8_t power_type;            /**< 0=unknown, 1=mains, 2=battery (zb_availability_power_type_t) */
    uint8_t consecutive_failures;  /**< Ping failure counter (0-3) */
    uint8_t backoff_multiplier;    /**< Exponential backoff (1-8, 0 treated as 1) */
    bool pending_response;         /**< Waiting for ZCL ping response */
    uint32_t custom_timeout;       /**< Per-device timeout override (0=default) */
    uint32_t last_check;           /**< Last active ping timestamp (epoch) */
    uint32_t next_check;           /**< Next scheduled check timestamp (epoch) */
} device_avail_meta_t;

/**
 * @brief Zigbee-specific device data
 *
 * Contains all Zigbee-specific information needed to operate the device.
 */
typedef struct {
    uint16_t short_addr;                    /**< Zigbee network short address */
    uint8_t endpoint;                       /**< Zigbee endpoint number */
    uint8_t lqi;                            /**< Link Quality Indicator (0-255) */
    int8_t rssi;                            /**< Received Signal Strength (dBm, typically -100 to 0) */
    device_zigbee_type_t device_type;       /**< Classified device type */
    uint16_t cluster_count;                 /**< Number of supported clusters */
    uint16_t clusters[DEVICE_MAX_CLUSTERS]; /**< Supported cluster IDs */
    device_power_info_t power_info;         /**< Power source information */
    const void *converter;                  /**< Pointer to zb_converter_def_t (NULL if not matched) */
    device_avail_meta_t avail_meta;         /**< Availability tracking metadata (volatile, not persisted) */
} device_zigbee_data_t;

/**
 * @brief BLE-specific device data
 */
typedef struct {
    uint8_t mac_addr[6];    /**< BLE MAC address (6 bytes) */
    uint8_t addr_type;      /**< BLE address type (public/random) */
    int8_t rssi;            /**< Received Signal Strength Indicator (dBm) */
    uint16_t service_uuid;  /**< Primary service UUID if known */
} device_ble_data_t;

/* ============================================================================
 * Main Device Structure
 * ============================================================================ */

/**
 * @brief Unified device structure
 *
 * Protocol-agnostic device representation. Dynamic state values
 * (sensor readings, switch states, etc.) are stored separately
 * via the device_registry module.
 */
typedef struct {
    device_id_t id;             /**< Unique device identifier */
    char friendly_name[32];     /**< User-assigned device name */
    char model[32];             /**< Device model identifier */
    char manufacturer[32];      /**< Manufacturer name */
    device_protocol_t protocol; /**< Device protocol type */
    uint32_t capabilities;      /**< DEV_CAP_* bitmask */
    device_availability_t availability; /**< Current availability */
    uint32_t last_seen;         /**< Epoch time when last seen */
    bool interviewed;           /**< True if device interview complete */
    bool in_use;                /**< True if slot is occupied (registry internal) */

    /** Protocol-specific data */
    union {
        device_zigbee_data_t zigbee;    /**< Zigbee-specific fields */
        device_ble_data_t ble;          /**< BLE-specific fields */
    } proto;
} device_t;

/* ============================================================================
 * Helper Functions - String Conversion
 * ============================================================================ */

/**
 * @brief Convert device protocol to string
 *
 * @param proto Device protocol enum value
 * @return String representation (e.g., "zigbee", "ble", "virtual")
 */
const char *device_protocol_to_str(device_protocol_t proto);

/**
 * @brief Convert device availability to string
 *
 * @param avail Device availability enum value
 * @return String representation (e.g., "online", "offline", "unknown")
 */
const char *device_availability_to_str(device_availability_t avail);

/* ============================================================================
 * Helper Functions - Capability Management
 * ============================================================================ */

/**
 * @brief Check if device has a specific capability
 *
 * @param dev Pointer to device structure
 * @param cap Capability flag to check
 * @return true if device has the capability, false otherwise
 */
bool device_has_capability(const device_t *dev, device_capability_t cap);

/**
 * @brief Set a capability flag on a device
 *
 * @param dev Pointer to device structure
 * @param cap Capability flag to set
 */
void device_set_capability(device_t *dev, device_capability_t cap);

/**
 * @brief Clear a capability flag from a device
 *
 * @param dev Pointer to device structure
 * @param cap Capability flag to clear
 */
void device_clear_capability(device_t *dev, device_capability_t cap);

/* ============================================================================
 * Helper Functions - Device ID
 * ============================================================================ */

/**
 * @brief Create device ID from IEEE 802.15.4 address
 *
 * Converts an 8-byte IEEE address to a device_id_t.
 * Used for Zigbee devices.
 *
 * @param ieee Pointer to 8-byte IEEE address (big-endian)
 * @return device_id_t value
 */
device_id_t device_id_from_ieee(const uint8_t ieee[8]);

/**
 * @brief Create device ID from BLE MAC address
 *
 * Converts a 6-byte MAC address to a device_id_t.
 * Upper 2 bytes are zero-padded.
 *
 * @param mac Pointer to 6-byte MAC address
 * @return device_id_t value
 */
device_id_t device_id_from_mac(const uint8_t mac[6]);

/**
 * @brief Convert device ID to hex string
 *
 * Formats the device ID as "0x00158d00012345ab".
 *
 * @param id Device ID to convert
 * @param buf Output buffer (must be at least 19 bytes: "0x" + 16 hex + null)
 * @param buf_size Size of output buffer
 */
void device_id_to_str(device_id_t id, char *buf, size_t buf_size);

/* ============================================================================
 * Device Initialization
 * ============================================================================ */

/**
 * @brief Initialize a device structure
 *
 * Sets up a device with the given ID and protocol, clearing all
 * other fields to sensible defaults:
 *   - Empty strings for names
 *   - No capabilities
 *   - Unknown availability
 *   - Not interviewed
 *
 * @param dev Pointer to device structure to initialize
 * @param id Device identifier
 * @param proto Device protocol type
 */
void device_init(device_t *dev, device_id_t id, device_protocol_t proto);

/* ============================================================================
 * Zigbee-Specific Helper Functions
 * ============================================================================ */

/**
 * @brief Check if Zigbee device has a specific cluster
 *
 * @param dev Pointer to device structure (must be Zigbee protocol)
 * @param cluster_id Cluster ID to check for
 * @return true if device has the cluster, false otherwise
 */
bool device_zigbee_has_cluster(const device_t *dev, uint16_t cluster_id);

/**
 * @brief Add a cluster to Zigbee device's cluster list
 *
 * @param dev Pointer to device structure (must be Zigbee protocol)
 * @param cluster_id Cluster ID to add
 * @return true if cluster was added, false if list is full or invalid
 */
bool device_zigbee_add_cluster(device_t *dev, uint16_t cluster_id);

/**
 * @brief Determine Zigbee device type from its cluster list
 *
 * Analyzes the device's clusters to classify it as light, switch, sensor, etc.
 * Sets dev->proto.zigbee.device_type.
 *
 * @param dev Pointer to device structure (must be Zigbee protocol)
 * @return true if type was determined, false on error
 */
bool device_determine_zigbee_type(device_t *dev);

/**
 * @brief Check if device is battery powered
 *
 * @param dev Pointer to device structure (must be Zigbee protocol)
 * @return true if device is battery powered, false otherwise
 */
bool device_zigbee_is_battery_powered(const device_t *dev);

/**
 * @brief Get battery percentage from power info
 *
 * @param dev Pointer to device structure (must be Zigbee protocol)
 * @return Battery percentage (0-100), or -1 if not available
 */
int device_zigbee_get_battery_percent(const device_t *dev);

/**
 * @brief Convert power source level to percentage
 *
 * @param power_level Power level from power descriptor
 * @return Approximate percentage (0, 5, 33, 66, or 100)
 */
uint8_t device_power_level_to_percent(uint8_t power_level);

#ifdef __cplusplus
}
#endif

#endif /* UNIFIED_DEVICE_H */
