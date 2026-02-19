/**
 * @file ble_common.h
 * @brief Common BLE definitions, types, and constants for ESP32-C5 Zigbee2MQTT Gateway
 *
 * This header provides shared types and constants used across all BLE modules
 * including the manager, scanner, and device parsers.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_COMMON_H
#define BLE_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BLE MAC address length in bytes
 */
#define BLE_MAC_ADDR_LEN            6

/**
 * @brief Maximum BLE legacy advertisement data length (BLE 4.x)
 */
#define BLE_ADV_DATA_MAX_LEN            31

/**
 * @brief Maximum BLE extended advertisement data length (BLE 5.0+)
 */
#define BLE_EXT_ADV_DATA_MAX_LEN        255

/**
 * @brief Maximum scan response data length
 */
#define BLE_SCAN_RSP_DATA_MAX_LEN       31

/**
 * @brief Maximum device name length
 */
#define BLE_DEVICE_NAME_MAX_LEN     32

/**
 * @brief Maximum number of tracked BLE devices
 *
 * Wired to CONFIG_BT_MAX_TRACKED_DEVICES from Kconfig (menuconfig).
 */
#ifdef CONFIG_BT_MAX_TRACKED_DEVICES
#define BLE_MAX_TRACKED_DEVICES     CONFIG_BT_MAX_TRACKED_DEVICES
#else
#define BLE_MAX_TRACKED_DEVICES     500
#endif

/**
 * @brief Default scan interval in milliseconds
 * Increased from 1000ms to reduce radio contention with Zigbee on single-radio ESP32-C5
 */
#define BLE_DEFAULT_SCAN_INTERVAL_MS    3000

/**
 * @brief Minimum scan interval in milliseconds
 */
#define BLE_MIN_SCAN_INTERVAL_MS        100

/**
 * @brief Maximum scan interval in milliseconds
 */
#define BLE_MAX_SCAN_INTERVAL_MS        10000

/**
 * @brief Default scan window in milliseconds (active scanning time within interval)
 */
#define BLE_DEFAULT_SCAN_WINDOW_MS      100

/**
 * @brief RSSI value indicating invalid/unknown signal strength
 */
#define BLE_RSSI_INVALID            (-128)

/**
 * @brief Time in milliseconds after which a device is considered stale
 */
#define BLE_DEVICE_STALE_TIMEOUT_MS     300000  /* 5 minutes */

/**
 * @brief BLE address type enumeration
 */
typedef enum {
    BLE_ADDR_TYPE_PUBLIC = 0,           /**< Public device address */
    BLE_ADDR_TYPE_RANDOM = 1,           /**< Random device address */
    BLE_ADDR_TYPE_PUBLIC_ID = 2,        /**< Public identity address */
    BLE_ADDR_TYPE_RANDOM_ID = 3,        /**< Random identity address */
    BLE_ADDR_TYPE_UNKNOWN = 0xFF        /**< Unknown address type */
} ble_addr_type_t;

/**
 * @brief BLE advertisement type enumeration (Legacy BLE 4.x)
 */
typedef enum {
    BLE_ADV_TYPE_IND = 0,               /**< Connectable undirected advertising */
    BLE_ADV_TYPE_DIRECT_IND_HIGH = 1,   /**< Connectable high duty directed advertising */
    BLE_ADV_TYPE_SCAN_IND = 2,          /**< Scannable undirected advertising */
    BLE_ADV_TYPE_NONCONN_IND = 3,       /**< Non-connectable undirected advertising */
    BLE_ADV_TYPE_DIRECT_IND_LOW = 4,    /**< Connectable low duty directed advertising */
    BLE_ADV_TYPE_UNKNOWN = 0xFF         /**< Unknown advertisement type */
} ble_adv_type_t;

/**
 * @brief BLE 5.0 PHY type enumeration
 */
typedef enum {
    BLE_PHY_1M = 1,                     /**< LE 1M PHY (BLE 4.x compatible) */
    BLE_PHY_2M = 2,                     /**< LE 2M PHY (BLE 5.0, higher throughput) */
    BLE_PHY_CODED = 3,                  /**< LE Coded PHY (BLE 5.0, long range) */
    BLE_PHY_UNKNOWN = 0xFF              /**< Unknown PHY type */
} ble_phy_t;

/**
 * @brief BLE 5.0 Extended Advertisement properties flags
 */
typedef enum {
    BLE_EXT_ADV_PROP_CONNECTABLE    = 0x01,  /**< Connectable advertising */
    BLE_EXT_ADV_PROP_SCANNABLE      = 0x02,  /**< Scannable advertising */
    BLE_EXT_ADV_PROP_DIRECTED       = 0x04,  /**< Directed advertising */
    BLE_EXT_ADV_PROP_HD_DIRECTED    = 0x08,  /**< High Duty Cycle Directed */
    BLE_EXT_ADV_PROP_LEGACY         = 0x10,  /**< Legacy advertising PDU */
    BLE_EXT_ADV_PROP_ANONYMOUS      = 0x20,  /**< Anonymous advertising */
    BLE_EXT_ADV_PROP_INCLUDE_TX_PWR = 0x40,  /**< Include TX Power */
} ble_ext_adv_prop_t;

/**
 * @brief BLE device type classification
 *
 * Generic device types used by the BLE scanner. For registry-specific
 * device model types, see ble_reg_device_type_t in ble_device_registry.h.
 */
typedef enum {
    BLE_DEVICE_TYPE_UNKNOWN = 0,        /**< Unknown device type */
    BLE_DEVICE_TYPE_GENERIC,            /**< Generic BLE device */
    BLE_DEVICE_TYPE_XIAOMI,             /**< Xiaomi/Mi device */
    BLE_DEVICE_TYPE_QINGPING,           /**< Qingping device */
    BLE_DEVICE_TYPE_ATCMI,              /**< ATC/PVVX custom firmware */
    BLE_DEVICE_TYPE_GOVEE,              /**< Govee device */
    BLE_DEVICE_TYPE_INKBIRD,            /**< Inkbird device */
    BLE_DEVICE_TYPE_RUUVI,              /**< Ruuvi tag */
    BLE_DEVICE_TYPE_TPMS,               /**< Tire pressure monitoring */
    BLE_DEVICE_TYPE_IBEACON,            /**< Apple iBeacon */
    BLE_DEVICE_TYPE_EDDYSTONE,          /**< Google Eddystone */
    BLE_DEVICE_TYPE_MAX
} ble_device_type_t;

/**
 * @brief BLE advertisement data structure
 *
 * Contains parsed advertisement data received from a BLE device.
 * Supports both Legacy (BLE 4.x) and Extended (BLE 5.0) advertisements.
 */
typedef struct {
    uint8_t mac[BLE_MAC_ADDR_LEN];       /**< Device MAC address */
    ble_addr_type_t addr_type;           /**< Address type */
    ble_adv_type_t adv_type;             /**< Advertisement type (legacy) */
    int8_t rssi;                         /**< Received signal strength (dBm) */
    uint8_t adv_data[BLE_ADV_DATA_MAX_LEN];     /**< Raw advertisement data (legacy) */
    uint8_t adv_data_len;                /**< Advertisement data length */
    uint8_t scan_rsp[BLE_SCAN_RSP_DATA_MAX_LEN]; /**< Scan response data */
    uint8_t scan_rsp_len;                /**< Scan response data length */
    uint32_t timestamp;                  /**< Timestamp of reception (ms since boot) */
    /* BLE 5.0 Extended Advertisement fields */
    bool is_extended;                    /**< true if BLE 5.0 extended advertisement */
    ble_phy_t primary_phy;               /**< Primary PHY (1M, Coded) */
    ble_phy_t secondary_phy;             /**< Secondary PHY (1M, 2M, Coded) */
    uint8_t ext_adv_props;               /**< Extended advertising properties flags */
    int8_t tx_power;                     /**< TX power level (dBm), 127 = not available */
    uint8_t sid;                         /**< Advertising SID (0-15) */
} ble_adv_data_t;

/**
 * @brief BLE 5.0 Extended Advertisement data structure
 *
 * Used for extended advertisements with larger payloads (up to 255 bytes)
 */
typedef struct {
    uint8_t mac[BLE_MAC_ADDR_LEN];       /**< Device MAC address */
    ble_addr_type_t addr_type;           /**< Address type */
    int8_t rssi;                         /**< Received signal strength (dBm) */
    ble_phy_t primary_phy;               /**< Primary PHY */
    ble_phy_t secondary_phy;             /**< Secondary PHY */
    uint8_t ext_adv_props;               /**< Extended advertising properties */
    int8_t tx_power;                     /**< TX power level (dBm) */
    uint8_t sid;                         /**< Advertising SID */
    uint8_t *ext_adv_data;               /**< Extended advertisement data (dynamically allocated) */
    uint16_t ext_adv_data_len;           /**< Extended advertisement data length (up to 255) */
    uint32_t timestamp;                  /**< Timestamp of reception */
} ble_ext_adv_data_t;

/**
 * @brief BLE tracked device information
 *
 * Stores information about a tracked BLE device including RSSI history
 * and BLE 5.0 capabilities.
 */
typedef struct {
    uint8_t mac[BLE_MAC_ADDR_LEN];       /**< Device MAC address */
    ble_addr_type_t addr_type;           /**< Address type */
    ble_device_type_t device_type;           /**< Detected device type */
    char name[BLE_DEVICE_NAME_MAX_LEN];  /**< Device name (if available) */
    int8_t rssi;                         /**< Last RSSI value */
    int8_t rssi_avg;                     /**< Average RSSI (exponential moving average) */
    uint32_t first_seen;                 /**< First seen timestamp (ms since boot) */
    uint32_t last_seen;                  /**< Last seen timestamp (ms since boot) */
    uint32_t adv_count;                  /**< Number of advertisements received */
    bool active;                         /**< Device is active (slot in use) */
    /* BLE 5.0 fields */
    bool supports_ble5;                  /**< Device uses BLE 5.0 extended advertising */
    ble_phy_t preferred_phy;             /**< Preferred/detected PHY */
    int8_t tx_power;                     /**< Advertised TX power (127 = unknown) */
} ble_tracked_device_t;

/**
 * @brief BLE scanner statistics
 */
typedef struct {
    uint32_t total_advertisements;       /**< Total advertisements received */
    uint32_t unique_devices;             /**< Number of unique devices seen */
    uint32_t scan_cycles;                /**< Number of complete scan cycles */
    uint32_t last_scan_time_ms;          /**< Duration of last scan cycle */
    uint32_t start_time;                 /**< Scanner start timestamp */
    /* BLE 5.0 statistics */
    uint32_t legacy_adv_count;           /**< Legacy (BLE 4.x) advertisements */
    uint32_t extended_adv_count;         /**< Extended (BLE 5.0) advertisements */
    uint32_t phy_1m_count;               /**< Advertisements on 1M PHY */
    uint32_t phy_2m_count;               /**< Advertisements on 2M PHY */
    uint32_t phy_coded_count;            /**< Advertisements on Coded PHY */
} ble_scanner_stats_t;

/**
 * @brief Convert MAC address to string
 *
 * @param mac MAC address bytes (6 bytes)
 * @param str Output string buffer (must be at least 18 bytes)
 * @return Pointer to str
 */
static inline char *ble_mac_to_str(const uint8_t *mac, char *str)
{
    snprintf(str, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return str;
}

/**
 * @brief Compare two MAC addresses
 *
 * @param mac1 First MAC address
 * @param mac2 Second MAC address
 * @return true if addresses match, false otherwise
 */
static inline bool ble_mac_equal(const uint8_t *mac1, const uint8_t *mac2)
{
    for (int i = 0; i < BLE_MAC_ADDR_LEN; i++) {
        if (mac1[i] != mac2[i]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Check if MAC address is all zeros
 *
 * @param mac MAC address to check
 * @return true if all zeros, false otherwise
 */
static inline bool ble_mac_is_zero(const uint8_t *mac)
{
    for (int i = 0; i < BLE_MAC_ADDR_LEN; i++) {
        if (mac[i] != 0) {
            return false;
        }
    }
    return true;
}

/* ============================================================================
 * BLE Advertisement Data Validation Helpers (CQ-060)
 *
 * These helpers reduce code duplication in BLE device parser detect functions.
 * For use with ble_registry_adv_data_t from ble_device_registry.h.
 * ============================================================================ */

/**
 * @brief Validate BLE advertisement data pointer (generic version)
 *
 * Checks that advertisement data structure and its data pointer are valid.
 * Works with any struct that has adv_data (uint8_t*), adv_len (size_t) members.
 *
 * @param adv_data Pointer to advertisement data buffer
 * @param adv_len Length of advertisement data
 * @return true if valid, false otherwise
 */
static inline bool ble_validate_adv_data_raw(const uint8_t *adv_data, size_t adv_len)
{
    return (adv_data != NULL && adv_len > 0);
}

/**
 * @brief Macro to validate BLE advertisement data and return UNKNOWN if invalid
 *
 * For use in detect functions that take ble_registry_adv_data_t* parameter.
 * Returns BLE_REG_DEVICE_TYPE_UNKNOWN if the advertisement data is invalid.
 *
 * @param adv Pointer to ble_registry_adv_data_t structure
 */
#define BLE_VALIDATE_ADV_OR_RETURN_UNKNOWN(adv) \
    do { \
        if ((adv) == NULL || (adv)->adv_data == NULL || (adv)->adv_len == 0) { \
            return BLE_REG_DEVICE_TYPE_UNKNOWN; \
        } \
    } while(0)

/**
 * @brief NimBLE host error codes (from host/ble_hs.h)
 *
 * Maps to human-readable strings for debugging.
 */
#define BLE_HS_EAGAIN           1   /**< Temporary failure; try again */
#define BLE_HS_EALREADY         2   /**< Operation already in progress */
#define BLE_HS_EINVAL           3   /**< Invalid arguments */
#define BLE_HS_EMSGSIZE         4   /**< Message too large */
#define BLE_HS_ENOENT           5   /**< No such entry */
#define BLE_HS_ENOMEM           6   /**< Out of memory */
#define BLE_HS_ENOTCONN         7   /**< Not connected */
#define BLE_HS_ENOTSUP          8   /**< Operation not supported */
#define BLE_HS_EAPP             9   /**< Application callback error */
#define BLE_HS_EBADDATA         10  /**< Invalid data */
#define BLE_HS_EOS              11  /**< OS error */
#define BLE_HS_ECONTROLLER      12  /**< Controller error */
#define BLE_HS_ETIMEOUT         13  /**< Timeout */
#define BLE_HS_EDONE            14  /**< Operation completed */
#define BLE_HS_EBUSY            15  /**< Resource busy */
#define BLE_HS_EREJECT          16  /**< Rejected */
#define BLE_HS_EUNKNOWN         17  /**< Unknown error */
#define BLE_HS_EROLE            18  /**< Role error */
#define BLE_HS_ETIMEOUT_HCI     19  /**< HCI timeout */
#define BLE_HS_ENOMEM_EVT       20  /**< Out of event memory */
#define BLE_HS_ENOADDR          21  /**< No address */
#define BLE_HS_ENOTSYNCED       22  /**< Not synced */
#define BLE_HS_EAUTHEN          23  /**< Authentication failure */
#define BLE_HS_EAUTHOR          24  /**< Authorization failure */
#define BLE_HS_EENCRYPT         25  /**< Encryption failure */
#define BLE_HS_EENCRYPT_KEY_SZ  26  /**< Encryption key size */
#define BLE_HS_ESTORE_CAP       27  /**< Store capacity exceeded */
#define BLE_HS_ESTORE_FAIL      28  /**< Store failure */

/**
 * @brief Convert NimBLE host error code to string
 *
 * @param rc NimBLE return code
 * @return Human-readable error string
 */
static inline const char *ble_hs_err_to_str(int rc)
{
    switch (rc) {
        case 0:                      return "OK";
        case BLE_HS_EAGAIN:          return "EAGAIN (try again)";
        case BLE_HS_EALREADY:        return "EALREADY (already in progress)";
        case BLE_HS_EINVAL:          return "EINVAL (invalid argument)";
        case BLE_HS_EMSGSIZE:        return "EMSGSIZE (message too large)";
        case BLE_HS_ENOENT:          return "ENOENT (no such entry)";
        case BLE_HS_ENOMEM:          return "ENOMEM (out of memory)";
        case BLE_HS_ENOTCONN:        return "ENOTCONN (not connected)";
        case BLE_HS_ENOTSUP:         return "ENOTSUP (not supported)";
        case BLE_HS_EAPP:            return "EAPP (application error)";
        case BLE_HS_EBADDATA:        return "EBADDATA (invalid data)";
        case BLE_HS_EOS:             return "EOS (OS error)";
        case BLE_HS_ECONTROLLER:     return "ECONTROLLER (controller error)";
        case BLE_HS_ETIMEOUT:        return "ETIMEOUT (timeout)";
        case BLE_HS_EDONE:           return "EDONE (completed)";
        case BLE_HS_EBUSY:           return "EBUSY (busy)";
        case BLE_HS_EREJECT:         return "EREJECT (rejected)";
        case BLE_HS_EUNKNOWN:        return "EUNKNOWN (unknown)";
        case BLE_HS_EROLE:           return "EROLE (role error)";
        case BLE_HS_ETIMEOUT_HCI:    return "ETIMEOUT_HCI (HCI timeout)";
        case BLE_HS_ENOMEM_EVT:      return "ENOMEM_EVT (event memory)";
        case BLE_HS_ENOADDR:         return "ENOADDR (no address)";
        case BLE_HS_ENOTSYNCED:      return "ENOTSYNCED (not synced)";
        case BLE_HS_EAUTHEN:         return "EAUTHEN (authentication failure)";
        case BLE_HS_EAUTHOR:         return "EAUTHOR (authorization failure)";
        case BLE_HS_EENCRYPT:        return "EENCRYPT (encryption failure)";
        case BLE_HS_EENCRYPT_KEY_SZ: return "EENCRYPT_KEY_SZ (key size)";
        case BLE_HS_ESTORE_CAP:      return "ESTORE_CAP (store capacity)";
        case BLE_HS_ESTORE_FAIL:     return "ESTORE_FAIL (store failure)";
        default:                     return "UNKNOWN";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* BLE_COMMON_H */
