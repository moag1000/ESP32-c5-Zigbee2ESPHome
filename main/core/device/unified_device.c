/**
 * @file unified_device.c
 * @brief Unified Device Model Implementation for ESP32-C5 Zigbee2MQTT NG
 *
 * Implements helper functions for the unified device model including
 * string conversions, capability management, and device ID handling.
 *
 * @copyright Copyright (c) 2024-2025
 * @license Apache License 2.0
 */

#include "unified_device.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "unified_device";

/* ============================================================================
 * Static String Tables
 * ============================================================================ */

/** @brief Protocol name strings for logging/serialization */
static const char *s_protocol_names[] = {
    "zigbee",
    "ble",
    "virtual",
};

/** @brief Availability name strings for logging/serialization */
static const char *s_availability_names[] = {
    "unknown",
    "online",
    "offline",
};

/* ============================================================================
 * String Conversion Functions
 * ============================================================================ */

const char *device_protocol_to_str(device_protocol_t proto)
{
    if (proto > DEV_PROTOCOL_VIRTUAL) {
        return "unknown";
    }
    return s_protocol_names[proto];
}

const char *device_availability_to_str(device_availability_t avail)
{
    if (avail > DEV_AVAIL_OFFLINE) {
        return "unknown";
    }
    return s_availability_names[avail];
}

/* ============================================================================
 * Capability Management Functions
 * ============================================================================ */

bool device_has_capability(const device_t *dev, device_capability_t cap)
{
    if (dev == NULL) {
        return false;
    }
    return (dev->capabilities & cap) != 0;
}

void device_set_capability(device_t *dev, device_capability_t cap)
{
    if (dev == NULL) {
        return;
    }
    dev->capabilities |= cap;
}

void device_clear_capability(device_t *dev, device_capability_t cap)
{
    if (dev == NULL) {
        return;
    }
    dev->capabilities &= ~cap;
}

/* ============================================================================
 * Device ID Functions
 * ============================================================================ */

device_id_t device_id_from_ieee(const uint8_t ieee[8])
{
    if (ieee == NULL) {
        ESP_LOGW(TAG, "NULL IEEE address provided");
        return 0;
    }

    /* Convert 8-byte big-endian IEEE address to uint64_t */
    device_id_t id = 0;
    for (int i = 0; i < 8; i++) {
        id = (id << 8) | ieee[i];
    }
    return id;
}

device_id_t device_id_from_mac(const uint8_t mac[6])
{
    if (mac == NULL) {
        ESP_LOGW(TAG, "NULL MAC address provided");
        return 0;
    }

    /* Convert 6-byte MAC address to uint64_t (upper 2 bytes are zero) */
    device_id_t id = 0;
    for (int i = 0; i < 6; i++) {
        id = (id << 8) | mac[i];
    }
    return id;
}

void device_id_to_str(device_id_t id, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return;
    }

    /* Format as "0x" followed by 16 hex digits */
    /* Minimum buffer size: 2 (0x) + 16 (hex) + 1 (null) = 19 bytes */
    if (buf_size < 19) {
        ESP_LOGW(TAG, "Buffer too small for device ID string (need 19, got %zu)", buf_size);
        buf[0] = '\0';
        return;
    }

    snprintf(buf, buf_size, "0x%016llx", (unsigned long long)id);
}

/* ============================================================================
 * Device Initialization
 * ============================================================================ */

void device_init(device_t *dev, device_id_t id, device_protocol_t proto)
{
    if (dev == NULL) {
        ESP_LOGE(TAG, "NULL device pointer");
        return;
    }

    /* Clear the entire structure */
    memset(dev, 0, sizeof(device_t));

    /* Set provided values */
    dev->id = id;
    dev->protocol = proto;

    /* Set defaults */
    dev->availability = DEV_AVAIL_UNKNOWN;
    dev->interviewed = false;
    dev->capabilities = 0;
    dev->last_seen = 0;

    /* Default friendly_name to IEEE address (like zigbee2mqtt).
     * Users can rename later; discovery and MQTT topics need a valid name. */
    device_id_to_str(id, dev->friendly_name, sizeof(dev->friendly_name));

    ESP_LOGD(TAG, "Initialized device 0x%016llx, protocol: %s",
             (unsigned long long)id, device_protocol_to_str(proto));
}

/* ============================================================================
 * Zigbee-Specific Helper Functions
 * ============================================================================ */

/**
 * @brief Power source bitmask constants (from Zigbee specification)
 *
 * These are the same values as used in zb_device_handler_types.h
 */
#define POWER_SOURCE_MAINS               (1 << 0)  /**< Constant (mains) power */
#define POWER_SOURCE_RECHARGEABLE_BATTERY (1 << 1) /**< Rechargeable battery */
#define POWER_SOURCE_DISPOSABLE_BATTERY  (1 << 2) /**< Disposable battery */

/**
 * @brief Power level constants (from Zigbee specification)
 */
#define POWER_LEVEL_CRITICAL    0   /**< Critical (< 5%) */
#define POWER_LEVEL_33_PERCENT  4   /**< 33% remaining */
#define POWER_LEVEL_66_PERCENT  8   /**< 66% remaining */
#define POWER_LEVEL_100_PERCENT 12  /**< 100% (full) */

bool device_zigbee_has_cluster(const device_t *dev, uint16_t cluster_id)
{
    if (dev == NULL || dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        return false;
    }

    const device_zigbee_data_t *zb = &dev->proto.zigbee;

    for (uint16_t i = 0; i < zb->cluster_count && i < DEVICE_MAX_CLUSTERS; i++) {
        if (zb->clusters[i] == cluster_id) {
            return true;
        }
    }

    return false;
}

bool device_zigbee_add_cluster(device_t *dev, uint16_t cluster_id)
{
    if (dev == NULL || dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        return false;
    }

    device_zigbee_data_t *zb = &dev->proto.zigbee;

    /* Check if cluster list is full */
    if (zb->cluster_count >= DEVICE_MAX_CLUSTERS) {
        ESP_LOGW(TAG, "Cluster list full, cannot add 0x%04X", cluster_id);
        return false;
    }

    /* Check if cluster already exists */
    if (device_zigbee_has_cluster(dev, cluster_id)) {
        return true; /* Already present */
    }

    /* Add cluster */
    zb->clusters[zb->cluster_count++] = cluster_id;
    return true;
}

bool device_determine_zigbee_type(device_t *dev)
{
    if (dev == NULL || dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        return false;
    }

    /* Zigbee standard cluster IDs (from ZCL spec) */
    #define CL_ON_OFF           0x0006
    #define CL_LEVEL_CONTROL    0x0008
    #define CL_COLOR_CONTROL    0x0300
    #define CL_TEMP_MEASUREMENT 0x0402
    #define CL_HUMIDITY         0x0405
    #define CL_OCCUPANCY        0x0406
    #define CL_WINDOW_COVERING  0x0102
    #define CL_DOOR_LOCK        0x0101
    #define CL_ILLUMINANCE      0x0400
    #define CL_PRESSURE         0x0403
    #define CL_PM25             0x042A
    #define CL_THERMOSTAT       0x0201
    #define CL_FAN_CONTROL      0x0202
    #define CL_DEHUMIDIFICATION 0x0203
    #define CL_ELECTRICAL       0x0B04
    #define CL_IAS_ZONE         0x0500
    #define CL_METERING         0x0702
    #define CL_TUYA_PRIVATE     0xEF00

    /* Classify based on clusters — priority: HVAC > Lock > Cover > Light > Switch > Meter > Sensor */
    device_zigbee_type_t type = DEVICE_ZB_TYPE_UNKNOWN;

    if (device_zigbee_has_cluster(dev, CL_THERMOSTAT) ||
        device_zigbee_has_cluster(dev, CL_DEHUMIDIFICATION)) {
        type = DEVICE_ZB_TYPE_CLIMATE;
    } else if (device_zigbee_has_cluster(dev, CL_FAN_CONTROL)) {
        type = DEVICE_ZB_TYPE_FAN;
    } else if (device_zigbee_has_cluster(dev, CL_DOOR_LOCK)) {
        type = DEVICE_ZB_TYPE_LOCK;
    } else if (device_zigbee_has_cluster(dev, CL_WINDOW_COVERING)) {
        type = DEVICE_ZB_TYPE_COVER;
    } else if (device_zigbee_has_cluster(dev, CL_COLOR_CONTROL) ||
               (device_zigbee_has_cluster(dev, CL_LEVEL_CONTROL) &&
                device_zigbee_has_cluster(dev, CL_ON_OFF))) {
        type = DEVICE_ZB_TYPE_LIGHT;
    } else if (device_zigbee_has_cluster(dev, CL_ON_OFF) &&
               !device_zigbee_has_cluster(dev, CL_LEVEL_CONTROL)) {
        type = DEVICE_ZB_TYPE_SWITCH;
    } else if (device_zigbee_has_cluster(dev, CL_METERING) ||
               device_zigbee_has_cluster(dev, CL_ELECTRICAL)) {
        type = DEVICE_ZB_TYPE_METER;
    } else if (device_zigbee_has_cluster(dev, CL_IAS_ZONE)) {
        type = DEVICE_ZB_TYPE_IAS_ZONE;
    } else if (device_zigbee_has_cluster(dev, CL_TEMP_MEASUREMENT) ||
               device_zigbee_has_cluster(dev, CL_HUMIDITY) ||
               device_zigbee_has_cluster(dev, CL_OCCUPANCY) ||
               device_zigbee_has_cluster(dev, CL_ILLUMINANCE) ||
               device_zigbee_has_cluster(dev, CL_PRESSURE) ||
               device_zigbee_has_cluster(dev, CL_PM25)) {
        type = DEVICE_ZB_TYPE_SENSOR;
    } else if (device_zigbee_has_cluster(dev, CL_TUYA_PRIVATE)) {
        type = DEVICE_ZB_TYPE_OTHER; /* Tuya type determined by driver */
    } else {
        type = DEVICE_ZB_TYPE_OTHER;
    }

    #undef CL_ON_OFF
    #undef CL_LEVEL_CONTROL
    #undef CL_COLOR_CONTROL
    #undef CL_TEMP_MEASUREMENT
    #undef CL_HUMIDITY
    #undef CL_OCCUPANCY
    #undef CL_WINDOW_COVERING
    #undef CL_DOOR_LOCK
    #undef CL_ILLUMINANCE
    #undef CL_PRESSURE
    #undef CL_PM25
    #undef CL_THERMOSTAT
    #undef CL_FAN_CONTROL
    #undef CL_DEHUMIDIFICATION
    #undef CL_ELECTRICAL
    #undef CL_IAS_ZONE
    #undef CL_METERING
    #undef CL_TUYA_PRIVATE

    dev->proto.zigbee.device_type = type;
    ESP_LOGD(TAG, "Device type determined: %d", type);
    return true;
}

bool device_zigbee_is_battery_powered(const device_t *dev)
{
    if (dev == NULL || dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        return false;
    }

    const device_power_info_t *power = &dev->proto.zigbee.power_info;

    if (!power->power_info_valid) {
        return false;
    }

    /* Check if current power source is battery (disposable or rechargeable) */
    uint8_t battery_mask = POWER_SOURCE_RECHARGEABLE_BATTERY | POWER_SOURCE_DISPOSABLE_BATTERY;
    return (power->current_power_source & battery_mask) != 0;
}

int device_zigbee_get_battery_percent(const device_t *dev)
{
    if (dev == NULL || dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        return -1;
    }

    const device_power_info_t *power = &dev->proto.zigbee.power_info;

    if (!power->power_info_valid) {
        return -1;
    }

    /* Only return battery percentage if battery powered */
    if (!device_zigbee_is_battery_powered(dev)) {
        return -1;
    }

    return device_power_level_to_percent(power->current_power_source_level);
}

uint8_t device_power_level_to_percent(uint8_t power_level)
{
    /* Power level encoding from Zigbee specification */
    if (power_level >= POWER_LEVEL_100_PERCENT) {
        return 100;
    } else if (power_level >= POWER_LEVEL_66_PERCENT) {
        return 66;
    } else if (power_level >= POWER_LEVEL_33_PERCENT) {
        return 33;
    } else if (power_level > POWER_LEVEL_CRITICAL) {
        return 5;  /* Low but not critical */
    } else {
        return 0;  /* Critical */
    }
}
