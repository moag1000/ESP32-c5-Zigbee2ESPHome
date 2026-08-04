/**
 * @file event_data.h
 * @brief Event payload structures for ESP32-C5 Zigbee2MQTT NG
 *
 * This header defines payload structures for each event type. These are the
 * `data` parameter passed to `event_publish()`.
 *
 * ## Ownership Semantics
 *
 * Each struct follows one of two ownership patterns:
 *
 * **BORROW** (default, most events):
 *   - `event_publish()` copies the struct via `memcpy` into the event queue.
 *   - Pointed-to data (strings, buffers) is NOT deep-copied.
 *   - The handler runs synchronously within the `event_dispatcher_task` context.
 *   - Caller retains ownership of all pointed-to data and must keep it valid
 *     until `event_publish()` returns (stack/static/pool buffers are fine).
 *   - Handler must NOT free any pointed-to data.
 *
 * **TRANSFER** (explicitly marked events):
 *   - `event_publish()` copies the struct via `memcpy` (same as BORROW).
 *   - Pointed-to strings MUST be heap-allocated by the caller (e.g., `strdup()`,
 *     `cJSON_PrintUnformatted()`).
 *   - The handler takes ownership and MUST `free()` all heap-allocated pointers
 *     on every exit path (including error paths, MQTT disconnected, etc.).
 *   - On `event_publish()` failure: the handler will NOT run, so the caller
 *     MUST `free()` the heap-allocated pointers itself.
 *
 * @note Structs are kept small as they get copied on publish
 */

#ifndef EVENT_DATA_H
#define EVENT_DATA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Device Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_DEVICE_JOINED payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Network short address */
    uint8_t endpoint;           /**< Primary endpoint */
    const char *manufacturer;   /**< Manufacturer string (may be NULL) */
    const char *model;          /**< Model string (may be NULL) */
} evt_device_joined_t;

/** @ownership BORROW */
/** @brief EVT_DEVICE_LEFT payload */
typedef struct {
    uint64_t ieee_addr;
    uint16_t short_addr;
    bool rejoin_expected;       /**< True if device may rejoin */
} evt_device_left_t;

/** @ownership BORROW */
/** @brief EVT_DEVICE_STATE_CHANGED payload */
typedef struct {
    uint64_t ieee_addr;
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    const char *json_state;     /**< JSON string of changed state */
} evt_device_state_t;

/** @ownership NONE */
/**
 * @brief EVT_ESPHOME_ENTITY_STATE payload
 *
 * Carries the key only. ESPHome entities have no device_t and their state is
 * not in the device registry — read it with esphome_entity_mirror_get(), which
 * hands back a copy taken under the mirror's lock. Putting the cJSON pointer in
 * the event instead would race the next sync_state() freeing it.
 */
typedef struct {
    uint32_t key;               /**< ESPHome entity key */
    uint8_t  entity_type;       /**< esphome_entity_type_t of the entity */
} evt_esphome_entity_state_t;

/** @ownership BORROW */
/** @brief EVT_DEVICE_INTERVIEWED payload */
typedef struct {
    uint64_t ieee_addr;
    uint16_t short_addr;
    bool success;
    uint8_t endpoint_count;
} evt_device_interviewed_t;

/* ============================================================================
 * Network Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_NETWORK_READY payload */
typedef struct {
    uint8_t channel;            /**< Zigbee channel (11-26) */
    uint16_t pan_id;            /**< PAN ID */
    uint64_t ext_pan_id;        /**< Extended PAN ID */
    uint8_t network_key[16];    /**< Network key (for backup) */
} evt_network_ready_t;

/** @ownership BORROW */
/** @brief EVT_ZB_PERMIT_JOIN payload */
typedef struct {
    bool enabled;               /**< True if permit join is active */
    uint8_t duration;           /**< Remaining duration in seconds (0-254, 255=forever) */
} evt_permit_join_t;

/** @ownership BORROW */
/** @brief EVT_ZB_ATTRIBUTE_REPORT payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    uint8_t endpoint;           /**< Endpoint that reported */
    uint16_t cluster_id;        /**< Cluster ID */
    uint16_t attr_id;           /**< Attribute ID */
    uint8_t attr_type;          /**< Attribute data type */
    const void *value;          /**< Pointer to attribute value */
    size_t value_len;           /**< Length of attribute value */
} evt_zb_attribute_report_t;

/** @ownership BORROW */
/** @brief EVT_ZB_COMMAND_RECEIVED payload */
typedef struct {
    uint64_t ieee_addr;         /**< Source device IEEE address */
    uint16_t short_addr;        /**< Source device short address */
    uint8_t endpoint;           /**< Source endpoint */
    uint16_t cluster_id;        /**< Cluster ID */
    uint8_t command_id;         /**< Command ID */
    const uint8_t *payload;     /**< Command payload data */
    size_t payload_len;         /**< Payload length */
} evt_zb_command_received_t;

/** @ownership BORROW */
/** @brief EVT_ZB_BIND_SUCCESS/FAILED payload */
typedef struct {
    uint64_t src_ieee;          /**< Source device IEEE address */
    uint64_t dst_ieee;          /**< Destination device IEEE address (or 0 for group) */
    uint16_t dst_group;         /**< Destination group ID (if dst_ieee is 0) */
    uint8_t src_endpoint;       /**< Source endpoint */
    uint8_t dst_endpoint;       /**< Destination endpoint */
    uint16_t cluster_id;        /**< Cluster ID */
    uint8_t error_code;         /**< Error code (0 on success) */
} evt_zb_bind_t;

/** @ownership BORROW */
/** @brief EVT_ZB_BINDINGS_LIST payload */
typedef struct {
    const char *json_bindings;  /**< JSON array string of all bindings */
    size_t binding_count;       /**< Number of bindings in the list */
} evt_zb_bindings_list_t;

/** @ownership BORROW */
/** @brief EVT_ZB_GROUP_ADDED payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    uint8_t endpoint;           /**< Endpoint */
    uint16_t group_id;          /**< Group ID */
    const char *group_name;     /**< Group name (may be NULL) */
} evt_zb_group_t;

/** @ownership BORROW */
/** @brief EVT_ZB_SCENE_ACTIVATED payload */
typedef struct {
    uint16_t group_id;          /**< Group ID */
    uint8_t scene_id;           /**< Scene ID */
    const char *scene_name;     /**< Scene name (may be NULL) */
} evt_zb_scene_t;

/* ============================================================================
 * MQTT Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_MQTT_CONNECTED payload */
typedef struct {
    const char *broker_uri;     /**< Broker URI */
    bool session_present;       /**< Clean session or resumed */
} evt_mqtt_connected_t;

/** @ownership BORROW */
/** @brief EVT_MQTT_DISCONNECTED payload */
typedef struct {
    int error_code;             /**< MQTT error code */
    bool reconnecting;          /**< Auto-reconnect in progress */
} evt_mqtt_disconnected_t;

/** @ownership BORROW */
/** @brief EVT_MQTT_SUBSCRIBE_OK payload */
typedef struct {
    const char *topic;          /**< Subscribed topic */
    int qos;                    /**< Granted QoS level */
} evt_mqtt_subscribe_t;

/** @ownership BORROW */
/** @brief EVT_MQTT_PUBLISH_OK payload */
typedef struct {
    int msg_id;                 /**< Message ID */
    const char *topic;          /**< Published topic */
} evt_mqtt_publish_t;

/** @ownership BORROW */
/** @brief EVT_MQTT_ERROR payload */
typedef struct {
    int error_code;             /**< Error code */
    const char *error_msg;      /**< Error message */
    const char *topic;          /**< Related topic (may be NULL) */
} evt_mqtt_error_t;

/* ============================================================================
 * BLE Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_BLE_DEVICE_FOUND payload */
typedef struct {
    uint8_t mac[6];             /**< BLE MAC address */
    uint8_t addr_type;          /**< Address type (public/random) */
    int8_t rssi;                /**< Signal strength */
    const char *name;           /**< Device name (may be NULL) */
    uint16_t service_uuid;      /**< Primary service UUID (0 if unknown) */
    const uint8_t *adv_data;    /**< Raw advertisement data */
    size_t adv_data_len;        /**< Advertisement data length */
} evt_ble_device_found_t;

/** @ownership BORROW */
/** @brief EVT_BLE_DEVICE_LOST payload */
typedef struct {
    uint8_t mac[6];
    uint32_t last_seen_ms;      /**< Last seen timestamp */
} evt_ble_device_lost_t;

/* ============================================================================
 * System Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_CONFIG_CHANGED payload */
typedef struct {
    const char *key;            /**< Config key that changed */
    const char *old_value;      /**< Previous value (may be NULL) */
    const char *new_value;      /**< New value */
} evt_config_changed_t;

/** @ownership BORROW */
/** @brief EVT_LIFECYCLE_CHANGED payload (include lifecycle.h for state enum) */
typedef struct {
    int old_state;              /**< Previous lifecycle_state_t */
    int new_state;              /**< New lifecycle_state_t */
} evt_lifecycle_changed_t;

/** @ownership BORROW */
/** @brief EVT_MEMORY_PRESSURE payload (include memory_pressure.h for level enum) */
typedef struct {
    int level;                  /**< memory_pressure_t level */
    size_t free_heap;           /**< Current free internal heap */
} evt_memory_pressure_t;

/** @ownership BORROW */
/** @brief EVT_SYSTEM_BOOT_COMPLETE payload */
typedef struct {
    uint32_t boot_time_ms;      /**< Time taken to boot in milliseconds */
    uint8_t reset_reason;       /**< ESP reset reason code */
} evt_system_boot_t;

/** @ownership BORROW */
/** @brief EVT_SYSTEM_SHUTDOWN payload */
typedef struct {
    uint8_t reason;             /**< Shutdown reason (0=user, 1=OTA, 2=error) */
    const char *message;        /**< Optional shutdown message */
} evt_system_shutdown_t;

/** @ownership BORROW */
/** @brief EVT_WIFI_SCAN_COMPLETE payload */
typedef struct {
    uint16_t ap_count;          /**< Number of access points found */
    bool success;               /**< True if scan completed successfully */
} evt_wifi_scan_t;

/** @ownership BORROW */
/** @brief EVT_WIFI_AP_STARTED payload */
typedef struct {
    const char *ssid;           /**< AP SSID */
    uint8_t channel;            /**< WiFi channel */
} evt_wifi_ap_started_t;

/** @ownership BORROW */
/** @brief EVT_WIFI_AP_CLIENT_CONNECTED payload */
typedef struct {
    uint8_t mac[6];             /**< Client MAC address */
    uint8_t aid;                /**< Association ID */
} evt_wifi_ap_client_t;

/* ============================================================================
 * Command Events
 * ============================================================================ */

/** @brief Command source identifiers */
typedef enum {
    CMD_SOURCE_MQTT = 0,        /**< Command from MQTT */
    CMD_SOURCE_ESPHOME = 1,     /**< Command from ESPHome API */
    CMD_SOURCE_INTERNAL = 2,    /**< Internal command (e.g., automation) */
} cmd_source_t;

/* Note: device_id_t is defined in unified_device.h as uint64_t */
#ifndef UNIFIED_DEVICE_H
typedef uint64_t device_id_t;  /**< Device ID (IEEE address or 0 for bridge) */
#endif

/** @ownership BORROW */
/** @brief EVT_COMMAND_RECEIVED payload */
typedef struct {
    device_id_t target_device;  /**< Target device ID (0 for bridge commands) */
    const char *command;        /**< Command name ("set", "get", "configure", etc.) */
    const char *payload_json;   /**< JSON payload string */
    uint8_t source;             /**< Command source (cmd_source_t) */
} evt_command_received_t;

/** @ownership BORROW */
/** @brief EVT_COMMAND_RESPONSE payload */
typedef struct {
    device_id_t device;         /**< Device ID that was targeted */
    const char *command;        /**< Command that was executed */
    bool success;               /**< True if command succeeded */
    const char *error_msg;      /**< Error message (NULL if success) */
} evt_command_response_t;

/* ============================================================================
 * Power/Electrical Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_POWER_STATE_CHANGED payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    float power_w;              /**< Power in Watts */
    float voltage_v;            /**< Voltage in Volts */
    float current_ma;           /**< Current in milliAmps */
    float power_factor;         /**< Power factor (0.0-1.0) */
    float energy_kwh;           /**< Total energy in kWh */
} evt_power_state_t;

/* ============================================================================
 * Tuya Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_TUYA_DATAPOINT payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    uint8_t dp_id;              /**< Datapoint ID */
    uint8_t dp_type;            /**< Datapoint type */
    const char *json_state;     /**< JSON string of datapoint state */
} evt_tuya_dp_t;

/* ============================================================================
 * OTA Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_OTA_STARTED payload */
typedef struct {
    const char *url;            /**< OTA URL */
    size_t total_size;          /**< Total firmware size in bytes (0 if unknown) */
} evt_ota_started_t;

/** @ownership BORROW */
/** @brief EVT_OTA_PROGRESS payload */
typedef struct {
    size_t bytes_written;       /**< Bytes written so far */
    size_t total_size;          /**< Total size in bytes */
    uint8_t percent;            /**< Progress percentage (0-100) */
} evt_ota_progress_t;

/** @ownership BORROW */
/** @brief EVT_OTA_COMPLETE payload */
typedef struct {
    size_t total_size;          /**< Total firmware size */
    uint32_t duration_ms;       /**< Time taken in milliseconds */
    bool restart_pending;       /**< True if restart is pending */
} evt_ota_complete_t;

/** @ownership BORROW */
/** @brief EVT_OTA_FAILED payload */
typedef struct {
    int error_code;             /**< ESP error code */
    const char *error_msg;      /**< Error message */
    size_t bytes_written;       /**< Bytes written before failure */
} evt_ota_failed_t;

/* ============================================================================
 * Zigbee Device OTA Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_ZB_OTA_PROGRESS payload */
typedef struct {
    uint8_t ieee_addr[8];       /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    const char *state;          /**< Transfer state string */
    uint8_t percent;            /**< Progress percentage (0-100) */
    size_t remaining;           /**< Bytes remaining */
} evt_zb_ota_progress_t;

/** @ownership BORROW */
/** @brief EVT_ZB_OTA_COMPLETE payload */
typedef struct {
    uint8_t ieee_addr[8];       /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    bool success;               /**< True if OTA succeeded */
    uint32_t duration_ms;       /**< Time taken in milliseconds */
} evt_zb_ota_complete_t;

/** @ownership BORROW */
/** @brief EVT_ZB_OTA_IMAGES_CHANGED payload */
typedef struct {
    const char *json_images;    /**< JSON array of available images */
    size_t image_count;         /**< Number of available images */
} evt_zb_ota_images_t;

/* ============================================================================
 * Extended Zigbee Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_ZB_ROUTER_STATUS payload */
typedef struct {
    uint64_t ieee_addr;         /**< Router IEEE address */
    uint16_t short_addr;        /**< Router short address */
    bool online;                /**< Router online status */
    uint8_t lqi;                /**< Link quality indicator */
    int8_t rssi;                /**< RSSI value */
    const char *json_parent;    /**< JSON string of parent info (optional) */
    const char *json_neighbors; /**< JSON array of neighbors (optional) */
} evt_zb_router_status_t;

/** @ownership BORROW */
/** @brief EVT_ZB_TOPOLOGY_RESPONSE payload */
typedef struct {
    uint64_t coordinator_ieee;  /**< Coordinator IEEE address */
    uint16_t device_count;      /**< Number of devices in network */
    const char *json_topology;  /**< JSON representation of topology */
} evt_zb_topology_response_t;

/** @ownership BORROW */
/** @brief EVT_ZB_TOUCHLINK_RESULT payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    bool success;               /**< Touchlink success */
    uint8_t error_code;         /**< Error code (0 on success) */
} evt_zb_touchlink_result_t;

/** @ownership BORROW */
/** @brief EVT_ZB_ALARM_TRIGGERED payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    uint8_t endpoint;           /**< Endpoint */
    uint16_t cluster_id;        /**< Cluster ID */
    uint8_t alarm_code;         /**< Alarm code */
    const char *description;    /**< Alarm description (may be NULL) */
} evt_zb_alarm_t;

/** @ownership BORROW */
/** @brief EVT_ZB_REPORTING_RESPONSE payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    uint16_t cluster_id;        /**< Cluster ID */
    uint16_t attribute_id;      /**< Attribute ID */
    uint16_t min_interval;      /**< Minimum reporting interval (seconds) */
    uint16_t max_interval;      /**< Maximum reporting interval (seconds) */
    bool success;               /**< Configuration success */
    uint8_t error_code;         /**< ZCL status/error code (0 on success) */
} evt_zb_reporting_response_t;

/* ============================================================================
 * Backup/Restore Events
 * ============================================================================ */

/** @brief Backup operation types */
typedef enum {
    BACKUP_OP_SAVE = 0,         /**< Backup save operation */
    BACKUP_OP_RESTORE = 1,      /**< Backup restore operation */
    BACKUP_OP_DELETE = 2,       /**< Backup delete operation */
    BACKUP_OP_LIST = 3,         /**< Backup list operation */
} backup_operation_t;

/** @ownership BORROW */
/** @brief EVT_BACKUP_RESPONSE payload */
typedef struct {
    uint8_t operation;          /**< backup_operation_t */
    bool success;               /**< Operation success */
    const char *filename;       /**< Backup filename (may be NULL) */
    const char *error_msg;      /**< Error message (NULL on success) */
    size_t file_size;           /**< File size in bytes (for save/restore) */
    const char *json_payload;   /**< Full JSON response (for list/restore) */
} evt_backup_response_t;

/* ============================================================================
 * Demand-Response Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_DRLC_LOAD_CONTROL payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    uint32_t event_id;          /**< DRLC event ID */
    uint8_t criticality_level;  /**< Event criticality (0-15) */
    uint16_t duration_min;      /**< Event duration in minutes */
    bool opt_out_available;     /**< Can device opt out */
    const char *json_payload;   /**< Full DRLC payload as JSON */
} evt_drlc_load_control_t;

/* ============================================================================
 * Battery Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_DEVICE_BATTERY_CHANGED payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
    uint8_t percentage;         /**< Battery percentage (0-100, 255=unknown) */
    uint16_t voltage_mv;        /**< Battery voltage in millivolts */
    bool low_battery;           /**< Low battery warning */
} evt_device_battery_t;

/* ============================================================================
 * Availability Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_DEVICE_AVAILABILITY_CHANGED payload */
typedef struct {
    uint64_t device_id;         /**< Device IEEE address */
    bool online;                /**< true = online, false = offline */
} evt_device_availability_t;

/* ============================================================================
 * Retained Topic Cleanup Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_DEVICE_TOPICS_CLEAR payload */
typedef struct {
    uint64_t ieee_addr;         /**< Device IEEE address */
    uint16_t short_addr;        /**< Device short address */
} evt_device_topics_clear_t;

/* ============================================================================
 * Bridge Events (Layer-clean MQTT publishing from core)
 * ============================================================================ */

/** @ownership TRANSFER - handler frees topic and payload_json */
/** @brief EVT_BRIDGE_PUBLISH payload */
typedef struct {
    const char *topic;          /**< MQTT topic (heap-allocated, handler frees) */
    const char *payload_json;   /**< JSON payload (heap-allocated, handler frees) */
    int qos;                    /**< MQTT QoS level (0, 1, or 2) */
    bool retain;                /**< Retain flag */
} evt_bridge_publish_t;

/* ============================================================================
 * HA Discovery Events
 * ============================================================================ */

/**
 * @ownership TRANSFER
 *
 * Both `topic` and `payload_json` MUST be heap-allocated by the caller:
 *   - topic: use strdup() for constant/stack strings
 *   - payload_json: use cJSON_PrintUnformatted() (returns heap) or strdup()
 *                   for pool buffers (pool_json_free() the original after strdup)
 *
 * Handler: publish_ha_discovery() in mqtt_event_handler.c takes ownership and
 * calls free() on both `topic` and `payload_json` on ALL exit paths, including:
 *   - Successful MQTT publish
 *   - MQTT disconnected / client NULL
 *   - Any error condition (goto cleanup)
 *
 * On event_publish() failure: the handler will NOT run, so the CALLER must
 * free() both pointers itself to avoid leaks.
 */
/** @brief EVT_HA_DISCOVERY_PUBLISH payload */
typedef struct {
    const char *topic;          /**< Discovery topic (heap-allocated, handler frees) */
    const char *payload_json;   /**< JSON payload (heap-allocated, handler frees) */
    int qos;                    /**< MQTT QoS level (0, 1, or 2) */
    bool retain;                /**< Retain flag */
} evt_ha_discovery_publish_t;

/* ============================================================================
 * ESPHome Events
 * ============================================================================ */

/** @ownership BORROW */
/** @brief EVT_ESPHOME_CONNECTED payload */
typedef struct {
    uint8_t client_id;          /**< Client slot ID */
    const char *client_info;    /**< Client info string (may be NULL) */
    bool encrypted;             /**< True if connection is encrypted (Noise) */
} evt_esphome_connected_t;

/** @ownership BORROW */
/** @brief EVT_ESPHOME_DISCONNECTED payload */
typedef struct {
    uint8_t client_id;          /**< Client slot ID */
    const char *client_info;    /**< Client info string (may be NULL) */
} evt_esphome_disconnected_t;

/** @brief ESPHome command types */
typedef enum {
    ESPHOME_CMD_SWITCH = 0,     /**< Switch command */
    ESPHOME_CMD_LIGHT,          /**< Light command */
    ESPHOME_CMD_NUMBER,         /**< Number command */
    ESPHOME_CMD_SELECT,         /**< Select command */
    ESPHOME_CMD_BUTTON,         /**< Button command */
    ESPHOME_CMD_COVER,          /**< Cover command */
    ESPHOME_CMD_FAN,            /**< Fan command */
    ESPHOME_CMD_CLIMATE,        /**< Climate command */
    ESPHOME_CMD_LOCK,           /**< Lock command */
    ESPHOME_CMD_TEXT,           /**< Text command */
    ESPHOME_CMD_MEDIA_PLAYER,   /**< Media player command */
    ESPHOME_CMD_ALARM,          /**< Alarm control panel command */
    ESPHOME_CMD_SERVICE,        /**< Service call command */
} esphome_cmd_type_t;

/** @ownership BORROW */
/** @brief ESPHome command received payload (extends evt_command_received_t) */
typedef struct {
    uint32_t entity_key;        /**< Target entity key */
    uint8_t cmd_type;           /**< esphome_cmd_type_t */
    uint8_t client_id;          /**< Source client ID */
    const char *payload_json;   /**< JSON payload string */
} evt_esphome_command_t;

#ifdef __cplusplus
}
#endif

#endif /* EVENT_DATA_H */
