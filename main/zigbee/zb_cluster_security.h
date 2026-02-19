/**
 * @file zb_cluster_security.h
 * @brief IAS Zone (0x0500) and Door Lock (0x0101) Cluster APIs
 *
 * Provides support for security sensors and door lock devices.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_CLUSTER_SECURITY_H
#define ZB_CLUSTER_SECURITY_H

#include "esp_err.h"
#include "esp_zigbee_core.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types moved from zb_device_handler_types.h
 * ============================================================================ */

/** @brief Maximum door lock devices tracked */
#define ZB_STATE_MAX_DOOR_LOCK              16

/** @brief Maximum IAS Zone (alarm) devices tracked */
#define ZB_STATE_MAX_IAS_ZONE               32

/* ============================================================================
 * Door Lock Cluster (0x0101) Types
 * ============================================================================ */

/**
 * @brief Door Lock Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_DOOR_LOCK                 0x0101

/**
 * @brief Door Lock Cluster Attribute IDs
 */
#define ZB_ZCL_ATTR_DOOR_LOCK_LOCK_STATE_ID         0x0000
#define ZB_ZCL_ATTR_DOOR_LOCK_LOCK_TYPE_ID          0x0001
#define ZB_ZCL_ATTR_DOOR_LOCK_ACTUATOR_ENABLED_ID   0x0002

/**
 * @brief Door Lock Cluster Command IDs
 */
#define ZB_ZCL_CMD_DOOR_LOCK_LOCK_DOOR_ID           0x00
#define ZB_ZCL_CMD_DOOR_LOCK_UNLOCK_DOOR_ID         0x01

/**
 * @brief Door Lock State values (enum8)
 */
typedef enum {
    ZB_DOOR_LOCK_STATE_NOT_FULLY_LOCKED = 0,  /**< Not fully locked */
    ZB_DOOR_LOCK_STATE_LOCKED = 1,            /**< Locked */
    ZB_DOOR_LOCK_STATE_UNLOCKED = 2           /**< Unlocked */
} zb_door_lock_state_t;

/**
 * @brief Door Lock state structure
 */
typedef struct {
    zb_door_lock_state_t lock_state;    /**< Current lock state */
    uint8_t lock_type;                   /**< Lock type */
    bool actuator_enabled;               /**< Actuator enabled status */
} zb_door_lock_state_struct_t;

/**
 * @brief Door Lock state change callback type
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current door lock state
 */
typedef void (*zb_door_lock_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                         const zb_door_lock_state_struct_t *state);

/* ============================================================================
 * IAS Zone Cluster (0x0500) Types
 * ============================================================================ */

/**
 * @brief IAS Zone Cluster ID
 */
#define ZB_ZCL_CLUSTER_ID_IAS_ZONE                  0x0500

/**
 * @brief IAS Zone Cluster Attribute IDs
 */
#define ZB_ZCL_ATTR_IAS_ZONE_STATE_ID               0x0000  /**< ZoneState (enum8) */
#define ZB_ZCL_ATTR_IAS_ZONE_TYPE_ID                0x0001  /**< ZoneType (enum16) */
#define ZB_ZCL_ATTR_IAS_ZONE_STATUS_ID              0x0002  /**< ZoneStatus (bitmap16) */
#define ZB_ZCL_ATTR_IAS_ZONE_CIE_ADDRESS_ID         0x0010  /**< IAS_CIE_Address (IEEE) */
#define ZB_ZCL_ATTR_IAS_ZONE_ID_ID                  0x0011  /**< ZoneID (uint8) */

/**
 * @brief IAS Zone Cluster Command IDs (Server to Client)
 */
#define ZB_ZCL_CMD_IAS_ZONE_STATUS_CHANGE_NOTIFICATION_ID   0x00  /**< Zone Status Change Notification */
#define ZB_ZCL_CMD_IAS_ZONE_ENROLL_REQUEST_ID               0x01  /**< Zone Enroll Request */

/**
 * @brief IAS Zone Cluster Command IDs (Client to Server)
 */
#define ZB_ZCL_CMD_IAS_ZONE_ENROLL_RESPONSE_ID              0x00  /**< Zone Enroll Response */
#define ZB_ZCL_CMD_IAS_ZONE_INIT_NORMAL_OP_MODE_ID          0x01  /**< Initiate Normal Operation Mode */
#define ZB_ZCL_CMD_IAS_ZONE_INIT_TEST_MODE_ID               0x02  /**< Initiate Test Mode */

/**
 * @brief IAS Zone State values (enum8)
 */
typedef enum {
    ZB_IAS_ZONE_STATE_NOT_ENROLLED = 0x00,  /**< Not enrolled */
    ZB_IAS_ZONE_STATE_ENROLLED = 0x01       /**< Enrolled */
} zb_ias_zone_state_t;

/**
 * @brief IAS Zone Type values (enum16)
 */
typedef enum {
    ZB_IAS_ZONE_TYPE_STANDARD_CIE       = 0x0000,  /**< Standard CIE */
    ZB_IAS_ZONE_TYPE_MOTION_SENSOR      = 0x000D,  /**< Motion Sensor */
    ZB_IAS_ZONE_TYPE_CONTACT_SWITCH     = 0x0015,  /**< Contact Switch (Door/Window) */
    ZB_IAS_ZONE_TYPE_FIRE_SENSOR        = 0x0028,  /**< Fire Sensor */
    ZB_IAS_ZONE_TYPE_WATER_SENSOR       = 0x002A,  /**< Water Sensor */
    ZB_IAS_ZONE_TYPE_GAS_SENSOR         = 0x002B,  /**< Gas Sensor */
    ZB_IAS_ZONE_TYPE_PERSONAL_EMERGENCY = 0x002C,  /**< Personal Emergency Device */
    ZB_IAS_ZONE_TYPE_VIBRATION_SENSOR   = 0x002D,  /**< Vibration/Movement Sensor */
    ZB_IAS_ZONE_TYPE_REMOTE_CONTROL     = 0x010F,  /**< Remote Control */
    ZB_IAS_ZONE_TYPE_KEY_FOB            = 0x0115,  /**< Key Fob */
    ZB_IAS_ZONE_TYPE_KEYPAD             = 0x021D,  /**< Keypad */
    ZB_IAS_ZONE_TYPE_STANDARD_WARNING   = 0x0225,  /**< Standard Warning Device */
    ZB_IAS_ZONE_TYPE_GLASS_BREAK_SENSOR = 0x0226,  /**< Glass Break Sensor */
    ZB_IAS_ZONE_TYPE_CARBON_MONOXIDE    = 0x0227,  /**< Carbon Monoxide Sensor */
    ZB_IAS_ZONE_TYPE_SECURITY_REPEATER  = 0x0229,  /**< Security Repeater */
    ZB_IAS_ZONE_TYPE_INVALID            = 0xFFFF   /**< Invalid Zone Type */
} zb_ias_zone_type_t;

/**
 * @brief IAS Zone Status bits (bitmap16)
 */
#define ZB_IAS_ZONE_STATUS_ALARM1           (1 << 0)   /**< Bit 0: Alarm1 (primary alarm) */
#define ZB_IAS_ZONE_STATUS_ALARM2           (1 << 1)   /**< Bit 1: Alarm2 (secondary alarm) */
#define ZB_IAS_ZONE_STATUS_TAMPER           (1 << 2)   /**< Bit 2: Tamper */
#define ZB_IAS_ZONE_STATUS_BATTERY_LOW      (1 << 3)   /**< Bit 3: Battery Low */
#define ZB_IAS_ZONE_STATUS_SUPERVISION      (1 << 4)   /**< Bit 4: Supervision Reports */
#define ZB_IAS_ZONE_STATUS_RESTORE_REPORTS  (1 << 5)   /**< Bit 5: Restore Reports */
#define ZB_IAS_ZONE_STATUS_TROUBLE          (1 << 6)   /**< Bit 6: Trouble */
#define ZB_IAS_ZONE_STATUS_AC_MAINS_FAULT   (1 << 7)   /**< Bit 7: AC Mains Fault */
#define ZB_IAS_ZONE_STATUS_TEST             (1 << 8)   /**< Bit 8: Test */
#define ZB_IAS_ZONE_STATUS_BATTERY_DEFECT   (1 << 9)   /**< Bit 9: Battery Defect */

/**
 * @brief IAS Zone Enroll Response codes
 */
typedef enum {
    ZB_IAS_ZONE_ENROLL_SUCCESS          = 0x00,  /**< Success */
    ZB_IAS_ZONE_ENROLL_NOT_SUPPORTED    = 0x01,  /**< Not Supported */
    ZB_IAS_ZONE_ENROLL_NO_ENROLL_PERMIT = 0x02,  /**< No Enroll Permit */
    ZB_IAS_ZONE_ENROLL_TOO_MANY_ZONES   = 0x03   /**< Too Many Zones */
} zb_ias_zone_enroll_response_code_t;

/**
 * @brief IAS Zone state structure
 */
typedef struct {
    zb_ias_zone_state_t zone_state;     /**< Zone enrollment state */
    zb_ias_zone_type_t zone_type;       /**< Zone type */
    uint16_t zone_status;               /**< Raw zone status bitmap */
    uint8_t zone_id;                    /**< Assigned Zone ID */
    esp_zb_ieee_addr_t cie_address;     /**< IAS CIE Address */
    bool alarm1;                        /**< Primary alarm active */
    bool alarm2;                        /**< Secondary alarm active */
    bool tamper;                        /**< Tamper detected */
    bool battery_low;                   /**< Battery low */
    bool supervision_reports;           /**< Supervision reports enabled */
    bool restore_reports;               /**< Restore reports enabled */
    bool trouble;                       /**< Trouble detected */
    bool ac_mains_fault;                /**< AC mains fault */
    bool test_mode;                     /**< Test mode active */
    bool battery_defect;                /**< Battery defect */
} zb_ias_zone_state_struct_t;

/**
 * @brief IAS Zone state change callback type
 *
 * @param[in] short_addr Device short address
 * @param[in] endpoint Device endpoint
 * @param[in] state Current IAS Zone state
 */
typedef void (*zb_ias_zone_state_cb_t)(uint16_t short_addr, uint8_t endpoint,
                                        const zb_ias_zone_state_struct_t *state);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

esp_err_t zb_cluster_security_init(void);
esp_err_t zb_cluster_security_deinit(void);
void zb_cluster_security_clear_all(void);
void zb_cluster_security_remove_device(uint16_t short_addr);

/* ============================================================================
 * Door Lock (0x0101) Public API
 * ============================================================================ */

esp_err_t zb_door_lock_register_callback(zb_door_lock_state_cb_t callback);
bool zb_device_has_door_lock(uint16_t short_addr);
esp_err_t zb_door_lock_read_state(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_door_lock_cmd_lock(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_door_lock_cmd_unlock(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_door_lock_handle_report(uint16_t short_addr, uint8_t endpoint,
                                      uint16_t attr_id, void *value, size_t value_len);
esp_err_t zb_door_lock_get_state(uint16_t short_addr, zb_door_lock_state_struct_t *state);

/* ============================================================================
 * IAS Zone (0x0500) Public API
 * ============================================================================ */

esp_err_t zb_ias_zone_register_callback(zb_ias_zone_state_cb_t callback);
bool zb_device_has_ias_zone(uint16_t short_addr);
esp_err_t zb_ias_zone_read_state(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_ias_zone_write_cie_address(uint16_t short_addr, uint8_t endpoint);
esp_err_t zb_ias_zone_enroll_response(uint16_t short_addr, uint8_t endpoint,
                                       zb_ias_zone_enroll_response_code_t response_code,
                                       uint8_t zone_id);
esp_err_t zb_ias_zone_handle_report(uint16_t short_addr, uint8_t endpoint,
                                     uint16_t attr_id, void *value, size_t value_len);
esp_err_t zb_ias_zone_handle_status_change(uint16_t short_addr, uint8_t endpoint,
                                            uint16_t zone_status, uint8_t extended_status,
                                            uint8_t zone_id, uint16_t delay);
esp_err_t zb_ias_zone_handle_enroll_request(uint16_t short_addr, uint8_t endpoint,
                                             uint16_t zone_type, uint16_t manufacturer_code);
esp_err_t zb_ias_zone_get_state(uint16_t short_addr, zb_ias_zone_state_struct_t *state);
const char* zb_ias_zone_get_device_class(zb_ias_zone_type_t zone_type);
void zb_ias_zone_parse_status(uint16_t zone_status, zb_ias_zone_state_struct_t *state);
esp_err_t zb_ias_zone_set_auto_enroll(bool enable);
bool zb_ias_zone_get_auto_enroll(void);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CLUSTER_SECURITY_H */
