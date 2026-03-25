/**
 * @file bridge_request_handler.c
 * @brief Bridge Request Handler Implementation
 *
 * Handles bridge control requests received via MQTT using consistent
 * Zigbee2MQTT-compatible response formatting.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "bridge_request_handler.h"
#include "bridge_response.h"
#include "bridge_events.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/device/device_registry.h"
#include "core/device/device_persistence.h"
#include "config_manager.h"
#include "system_monitor.h"
#include "mqtt_logger.h"
#include "gateway_defaults.h"
#include "zigbee/zb_coordinator.h"
#include "zigbee/zb_device_handler_types.h"
#include "zigbee/zb_leave_helper.h"
#include "zigbee/zb_availability.h"
#include "zigbee/zb_reporting.h"
#include "zigbee/zb_install_codes.h"
#include "zigbee/zb_backup.h"
#include "zigbee/zb_network.h"
#include "zigbee/zb_poll_control.h"
#include "zigbee/zb_time_server.h"
#include "gateway_mqtt.h"
#include "mqtt/mqtt_topics.h"
#include "utils/json_utils.h"
#include "ota/ota_handler.h"
#include "memory/memory_manager_ng.h"
#include "memory/buffer_pool_helpers.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include "mbedtls/base64.h"
#include "zigbee/converter/zb_converter.h"
#include "zigbee/converter/zb_converter_loader.h"
#include "zigbee/converter/zb_custom_quirk.h"
#include "zigbee/tuya/tuya_driver_registry.h"
#include "core/littlefs_mount.h"

#if CONFIG_STATE_PERSISTENCE_ENABLE
#include "state_persistence.h"
#endif

#if CONFIG_BT_SCANNER_ENABLED
#include "bluetooth/ble_scanner.h"
#include "ha_bridge_discovery.h"
#include "core/gateway_timeouts.h"
#endif

static const char *TAG = "BRIDGE_REQ";

/* Response topic definitions */
#define RESPONSE_TOPIC_PERMIT_JOIN      "zigbee2mqtt/bridge/response/permit_join"
#define RESPONSE_TOPIC_DEVICE_REMOVE    "zigbee2mqtt/bridge/response/device/remove"
#define RESPONSE_TOPIC_DEVICE_RENAME    "zigbee2mqtt/bridge/response/device/rename"
#define RESPONSE_TOPIC_HEALTH_CHECK     "zigbee2mqtt/bridge/response/health_check"
#define RESPONSE_TOPIC_RESTART          "zigbee2mqtt/bridge/response/restart"
#define RESPONSE_TOPIC_FACTORY_RESET    "zigbee2mqtt/bridge/response/factory_reset"
#define RESPONSE_TOPIC_RESET_NETWORK   "zigbee2mqtt/bridge/response/reset/network"
#define RESPONSE_TOPIC_RESET_CONFIG    "zigbee2mqtt/bridge/response/reset/config"
#define RESPONSE_TOPIC_CONFIG_GET       "zigbee2mqtt/bridge/response/config/get"
#define RESPONSE_TOPIC_CONFIG_SET       "zigbee2mqtt/bridge/response/config/set"
#define RESPONSE_TOPIC_OTA_CHECK        "zigbee2mqtt/bridge/response/ota_update/check"
#define RESPONSE_TOPIC_OTA_INSTALL      "zigbee2mqtt/bridge/response/ota_update/install"
#define RESPONSE_TOPIC_AVAIL_CHECK      "zigbee2mqtt/bridge/response/device/availability/check"
#define RESPONSE_TOPIC_DEVICE_CONFIGURE "zigbee2mqtt/bridge/response/device/configure"
#define RESPONSE_TOPIC_DEVICE_OPTIONS   "zigbee2mqtt/bridge/response/device/options"
#define RESPONSE_TOPIC_DEVICE_GET       "zigbee2mqtt/bridge/response/device/get"
#define RESPONSE_TOPIC_INSTALL_CODE_ADD    "zigbee2mqtt/bridge/response/install_code/add"
#define RESPONSE_TOPIC_INSTALL_CODE_REMOVE "zigbee2mqtt/bridge/response/install_code/remove"
#define RESPONSE_TOPIC_INSTALL_CODE_LIST   "zigbee2mqtt/bridge/response/install_code/list"

/* Coordinator response topics (ZG-012) */
#define RESPONSE_TOPIC_COORDINATOR_CHECK   "zigbee2mqtt/bridge/response/coordinator/check"
#define RESPONSE_TOPIC_COORDINATOR_VERSION "zigbee2mqtt/bridge/response/coordinator/version"
#define RESPONSE_TOPIC_COORDINATOR_SETTINGS "zigbee2mqtt/bridge/response/coordinator/settings"

/* Network channel change response topics (ZG-016) */
#define RESPONSE_TOPIC_OPTIONS             "zigbee2mqtt/bridge/response/options"
#define RESPONSE_TOPIC_NETWORK_CHANNEL     "zigbee2mqtt/bridge/response/network/channel"

/* Extended PAN ID management response topic (API-004) */
#define RESPONSE_TOPIC_NETWORK_EXT_PAN_ID  "zigbee2mqtt/bridge/response/network/extended_pan_id"

/* Network key rotation response topic (API-006) */
#define RESPONSE_TOPIC_NETWORK_KEY_ROTATE  "zigbee2mqtt/bridge/response/network/key_rotate"

/* Poll Control response topics */
#define RESPONSE_TOPIC_POLL_CONTROL_GET    "zigbee2mqtt/bridge/response/poll_control/get"
#define RESPONSE_TOPIC_POLL_CONTROL_SET    "zigbee2mqtt/bridge/response/poll_control/set"

/* Time Server response topics */
#define RESPONSE_TOPIC_TIME_GET            "zigbee2mqtt/bridge/response/time"
#define RESPONSE_TOPIC_TIME_SET            "zigbee2mqtt/bridge/response/time/set"
#define RESPONSE_TOPIC_TIME_CONFIG         "zigbee2mqtt/bridge/response/time/config"

/* BLE scanner response topic */
#define RESPONSE_TOPIC_BLE_SCANNER         "zigbee2mqtt/bridge/response/ble_scanner"

/* Converter DB update response topic */
#define RESPONSE_TOPIC_CONVERTER_DB_UPDATE "zigbee2mqtt/bridge/response/converter_db/update"

/* Custom quirk response topics */
#define RESPONSE_TOPIC_CUSTOM_QUIRK_ADD    "zigbee2mqtt/bridge/response/custom/add"
#define RESPONSE_TOPIC_CUSTOM_QUIRK_REMOVE "zigbee2mqtt/bridge/response/custom/remove"
#define RESPONSE_TOPIC_CUSTOM_QUIRK_LIST   "zigbee2mqtt/bridge/response/custom/list"

/* NVS namespace for device options */
#define DEVICE_OPTIONS_NVS_NAMESPACE    "dev_opts"

/* Cluster name to ID mapping */
typedef struct {
    const char *name;
    uint16_t cluster_id;
} cluster_name_map_t;

static const cluster_name_map_t s_cluster_map[] = {
    {"genBasic", 0x0000},
    {"genPowerCfg", 0x0001},
    {"genDeviceTempCfg", 0x0002},
    {"genIdentify", 0x0003},
    {"genGroups", 0x0004},
    {"genScenes", 0x0005},
    {"genOnOff", 0x0006},
    {"genOnOffSwitchCfg", 0x0007},
    {"genLevelCtrl", 0x0008},
    {"genAlarms", 0x0009},
    {"genTime", 0x000A},
    {"genAnalogInput", 0x000C},
    {"genAnalogOutput", 0x000D},
    {"genAnalogValue", 0x000E},
    {"genBinaryInput", 0x000F},
    {"genBinaryOutput", 0x0010},
    {"genBinaryValue", 0x0011},
    {"genMultistateInput", 0x0012},
    {"genMultistateOutput", 0x0013},
    {"genMultistateValue", 0x0014},
    {"closuresDoorLock", 0x0101},
    {"closuresWindowCovering", 0x0102},
    {"hvacPumpCfgCtrl", 0x0200},
    {"hvacThermostat", 0x0201},
    {"hvacFanCtrl", 0x0202},
    {"hvacDehumidificationCtrl", 0x0203},
    {"hvacUserInterfaceCfg", 0x0204},
    {"lightingColorCtrl", 0x0300},
    {"lightingBallastCfg", 0x0301},
    {"msIlluminanceMeasurement", 0x0400},
    {"msIlluminanceLevelSensing", 0x0401},
    {"msTemperatureMeasurement", 0x0402},
    {"msPressureMeasurement", 0x0403},
    {"msFlowMeasurement", 0x0404},
    {"msRelativeHumidity", 0x0405},
    {"msOccupancySensing", 0x0406},
    {"ssIasZone", 0x0500},
    {"ssIasAce", 0x0501},
    {"ssIasWd", 0x0502},
    {"haElectricalMeasurement", 0x0B04},
    {"seMetering", 0x0702},
    {"haDiagnostic", 0x0B05},
    {"touchlink", 0x1000},
    {NULL, 0xFFFF}
};

/* Common attribute name to ID mapping */
typedef struct {
    uint16_t cluster_id;
    const char *name;
    uint16_t attr_id;
} attr_name_map_t;

static const attr_name_map_t s_attr_map[] = {
    /* genOnOff */
    {0x0006, "onOff", 0x0000},
    {0x0006, "globalSceneCtrl", 0x4000},
    {0x0006, "onTime", 0x4001},
    {0x0006, "offWaitTime", 0x4002},
    /* genLevelCtrl */
    {0x0008, "currentLevel", 0x0000},
    {0x0008, "remainingTime", 0x0001},
    {0x0008, "onOffTransitionTime", 0x0010},
    {0x0008, "onLevel", 0x0011},
    {0x0008, "onTransitionTime", 0x0012},
    {0x0008, "offTransitionTime", 0x0013},
    /* lightingColorCtrl */
    {0x0300, "currentHue", 0x0000},
    {0x0300, "currentSaturation", 0x0001},
    {0x0300, "remainingTime", 0x0002},
    {0x0300, "currentX", 0x0003},
    {0x0300, "currentY", 0x0004},
    {0x0300, "colorTemperatureMireds", 0x0007},
    {0x0300, "colorMode", 0x0008},
    /* msTemperatureMeasurement */
    {0x0402, "measuredValue", 0x0000},
    {0x0402, "minMeasuredValue", 0x0001},
    {0x0402, "maxMeasuredValue", 0x0002},
    {0x0402, "tolerance", 0x0003},
    /* msRelativeHumidity */
    {0x0405, "measuredValue", 0x0000},
    {0x0405, "minMeasuredValue", 0x0001},
    {0x0405, "maxMeasuredValue", 0x0002},
    {0x0405, "tolerance", 0x0003},
    /* msPressureMeasurement */
    {0x0403, "measuredValue", 0x0000},
    {0x0403, "minMeasuredValue", 0x0001},
    {0x0403, "maxMeasuredValue", 0x0002},
    /* msIlluminanceMeasurement */
    {0x0400, "measuredValue", 0x0000},
    {0x0400, "minMeasuredValue", 0x0001},
    {0x0400, "maxMeasuredValue", 0x0002},
    /* msOccupancySensing */
    {0x0406, "occupancy", 0x0000},
    {0x0406, "occupancySensorType", 0x0001},
    /* ssIasZone */
    {0x0500, "zoneState", 0x0000},
    {0x0500, "zoneType", 0x0001},
    {0x0500, "zoneStatus", 0x0002},
    /* genBasic */
    {0x0000, "zclVersion", 0x0000},
    {0x0000, "appVersion", 0x0001},
    {0x0000, "stackVersion", 0x0002},
    {0x0000, "hwVersion", 0x0003},
    {0x0000, "manufacturerName", 0x0004},
    {0x0000, "modelId", 0x0005},
    {0x0000, "dateCode", 0x0006},
    {0x0000, "powerSource", 0x0007},
    /* genPowerCfg */
    {0x0001, "batteryVoltage", 0x0020},
    {0x0001, "batteryPercentageRemaining", 0x0021},
    {0, NULL, 0}
};

/* Thread-local storage for transaction ID during request processing */
static char s_current_transaction[128] = {0};

/* Forward declarations for functions defined later in this file */
esp_err_t bridge_request_network_extended_pan_id(const char *payload);
esp_err_t bridge_request_network_key_rotate(const char *payload);
esp_err_t bridge_request_poll_control_get(const char *topic, const char *payload);
esp_err_t bridge_request_poll_control_set(const char *topic, const char *payload);
esp_err_t bridge_request_time_get(void);
esp_err_t bridge_request_time_set(const char *payload);
esp_err_t bridge_request_time_config(const char *payload);

/* ============================================================================
 * Request Dispatcher Types and Table
 * ============================================================================ */

/**
 * @brief Request handler function signature
 *
 * All handlers receive the full topic, payload, and payload length.
 * This allows handlers to extract any needed information from the topic.
 */
typedef esp_err_t (*request_handler_fn)(const char *topic, const char *payload, size_t payload_len);

/**
 * @brief Request handler dispatch table entry
 */
typedef struct {
    const char *topic_suffix;   /**< Topic suffix after "zigbee2mqtt/bridge/request/" */
    request_handler_fn handler; /**< Handler function */
    bool use_strstr;            /**< If true, use strstr() for matching instead of exact match */
} request_handler_entry_t;

/* Forward declarations for wrapper handlers */
static esp_err_t handle_permit_join(const char *topic, const char *payload, size_t len);
static esp_err_t handle_device_remove(const char *topic, const char *payload, size_t len);
static esp_err_t handle_device_rename(const char *topic, const char *payload, size_t len);
static esp_err_t handle_health_check(const char *topic, const char *payload, size_t len);
static esp_err_t handle_config_get(const char *topic, const char *payload, size_t len);
static esp_err_t handle_config_set(const char *topic, const char *payload, size_t len);
static esp_err_t handle_ota_check(const char *topic, const char *payload, size_t len);
static esp_err_t handle_ota_install(const char *topic, const char *payload, size_t len);
static esp_err_t handle_restart(const char *topic, const char *payload, size_t len);
static esp_err_t handle_factory_reset(const char *topic, const char *payload, size_t len);
static esp_err_t handle_network_reset(const char *topic, const char *payload, size_t len);
static esp_err_t handle_config_reset(const char *topic, const char *payload, size_t len);
static esp_err_t handle_networkmap(const char *topic, const char *payload, size_t len);
static esp_err_t handle_availability_check(const char *topic, const char *payload, size_t len);
static esp_err_t handle_device_configure(const char *topic, const char *payload, size_t len);
static esp_err_t handle_device_options(const char *topic, const char *payload, size_t len);
static esp_err_t handle_device_get(const char *topic, const char *payload, size_t len);
static esp_err_t handle_logging(const char *topic, const char *payload, size_t len);
static esp_err_t handle_install_code_add(const char *topic, const char *payload, size_t len);
static esp_err_t handle_install_code_remove(const char *topic, const char *payload, size_t len);
static esp_err_t handle_install_code_list(const char *topic, const char *payload, size_t len);
static esp_err_t handle_backup(const char *topic, const char *payload, size_t len);
static esp_err_t handle_restore(const char *topic, const char *payload, size_t len);
static esp_err_t handle_backup_list(const char *topic, const char *payload, size_t len);
static esp_err_t handle_coordinator_check(const char *topic, const char *payload, size_t len);
static esp_err_t handle_coordinator_version(const char *topic, const char *payload, size_t len);
static esp_err_t handle_coordinator_settings(const char *topic, const char *payload, size_t len);
static esp_err_t handle_network_options(const char *topic, const char *payload, size_t len);
static esp_err_t handle_network_ext_pan_id(const char *topic, const char *payload, size_t len);
static esp_err_t handle_network_key_rotate(const char *topic, const char *payload, size_t len);
static esp_err_t handle_poll_control(const char *topic, const char *payload, size_t len);
static esp_err_t handle_time_set(const char *topic, const char *payload, size_t len);
static esp_err_t handle_time_config(const char *topic, const char *payload, size_t len);
static esp_err_t handle_time_get(const char *topic, const char *payload, size_t len);
#if CONFIG_BT_SCANNER_ENABLED
static esp_err_t handle_ble_scanner(const char *topic, const char *payload, size_t len);
#endif
static esp_err_t handle_converter_db_update(const char *topic, const char *payload, size_t len);
static esp_err_t handle_custom_quirk_add(const char *topic, const char *payload, size_t len);
static esp_err_t handle_custom_quirk_remove(const char *topic, const char *payload, size_t len);
static esp_err_t handle_custom_quirk_list(const char *topic, const char *payload, size_t len);

/**
 * @brief Request handler dispatch table
 *
 * Order matters: more specific patterns should come before general ones.
 * Entries with use_strstr=true use substring matching.
 * Entries with use_strstr=false use mqtt_topic_matches() for exact matching.
 */
static const request_handler_entry_t s_request_handlers[] = {
    /* Exact match handlers (use mqtt_topic_matches) */
    { TOPIC_BRIDGE_REQUEST_PERMIT_JOIN,           handle_permit_join,           false },
    { TOPIC_BRIDGE_REQUEST_DEVICE_REMOVE,         handle_device_remove,         false },
    { TOPIC_BRIDGE_REQUEST_DEVICE_RENAME,         handle_device_rename,         false },
    { TOPIC_BRIDGE_REQUEST_HEALTH_CHECK,          handle_health_check,          false },
    { TOPIC_BRIDGE_REQUEST_DEVICE_CONFIGURE,      handle_device_configure,      false },
    { TOPIC_BRIDGE_REQUEST_DEVICE_OPTIONS,        handle_device_options,        false },
    { TOPIC_BRIDGE_REQUEST_DEVICE_GET,            handle_device_get,            false },
    { TOPIC_BRIDGE_REQUEST_INSTALL_CODE_ADD,      handle_install_code_add,      false },
    { TOPIC_BRIDGE_REQUEST_INSTALL_CODE_REMOVE,   handle_install_code_remove,   false },
    { TOPIC_BRIDGE_REQUEST_INSTALL_CODE_LIST,     handle_install_code_list,     false },
    { TOPIC_BRIDGE_REQUEST_BACKUP,                handle_backup,                false },
    { TOPIC_BRIDGE_REQUEST_BACKUP_STATE,          handle_backup,                false },
    { TOPIC_BRIDGE_REQUEST_BACKUP_DEVICES,        handle_backup,                false },
    { TOPIC_BRIDGE_REQUEST_RESTORE,               handle_restore,               false },
    { TOPIC_BRIDGE_REQUEST_BACKUP_LIST,           handle_backup_list,           false },
    { TOPIC_BRIDGE_REQUEST_COORDINATOR_CHECK,     handle_coordinator_check,     false },
    { TOPIC_BRIDGE_REQUEST_COORDINATOR_VERSION,   handle_coordinator_version,   false },
    { TOPIC_BRIDGE_REQUEST_COORDINATOR_SETTINGS,  handle_coordinator_settings,  false },
    { TOPIC_BRIDGE_REQUEST_OPTIONS,               handle_network_options,       false },
    { TOPIC_BRIDGE_REQUEST_NETWORK_CHANNEL,       handle_network_options,       false },
    { TOPIC_BRIDGE_REQUEST_NETWORK_EXT_PAN_ID,    handle_network_ext_pan_id,    false },
    { TOPIC_BRIDGE_REQUEST_NETWORK_KEY_ROTATE,    handle_network_key_rotate,    false },
    { TOPIC_BRIDGE_REQUEST_RESTART,               handle_restart,               false },
    { TOPIC_BRIDGE_REQUEST_FACTORY_RESET,         handle_factory_reset,         false },
    { TOPIC_BRIDGE_REQUEST_RESET_NETWORK,         handle_network_reset,         false },
    { TOPIC_BRIDGE_REQUEST_RESET_CONFIG,          handle_config_reset,          false },
    { TOPIC_BRIDGE_REQUEST_NETWORKMAP,            handle_networkmap,            false },
#if CONFIG_BT_SCANNER_ENABLED
    { TOPIC_BRIDGE_REQUEST_BLE_SCANNER,          handle_ble_scanner,           false },
#endif

    /* Substring match handlers (use strstr) - order matters, more specific first */
    { "/device/availability/check",               handle_availability_check,    true },
    { "/config/get",                              handle_config_get,            true },
    { "/config/set",                              handle_config_set,            true },
    { "/ota_update/check",                        handle_ota_check,             true },
    { "/ota_update/install",                      handle_ota_install,           true },
    { "/logging/",                                handle_logging,               true },
    { "/poll_control/",                           handle_poll_control,          true },
    { "/time/set",                                handle_time_set,              true },
    { "/time/config",                             handle_time_config,           true },
    { "/request/time",                            handle_time_get,              true },
    { "/converter_db/update",                     handle_converter_db_update,   true },
    { "/custom/add",                              handle_custom_quirk_add,      true },
    { "/custom/remove",                           handle_custom_quirk_remove,   true },
    { "/custom/list",                             handle_custom_quirk_list,     true },

    /* Sentinel - must be last */
    { NULL, NULL, false }
};

/**
 * @brief Get current transaction ID
 * @return Current transaction ID or NULL if not set
 */
static const char* get_current_transaction(void)
{
    return s_current_transaction[0] != '\0' ? s_current_transaction : NULL;
}

/**
 * @brief Set current transaction ID from request payload
 * @param payload_str JSON payload string
 */
static void extract_transaction_from_payload(const char *payload_str)
{
    s_current_transaction[0] = '\0';

    if (payload_str == NULL) {
        return;
    }

    cJSON *json = cJSON_Parse(payload_str);
    if (json == NULL) {
        return;
    }

    const char *txn = bridge_request_get_transaction(json);
    if (txn != NULL) {
        strncpy(s_current_transaction, txn, sizeof(s_current_transaction) - 1);
        s_current_transaction[sizeof(s_current_transaction) - 1] = '\0';
        ESP_LOGD(TAG, "Transaction ID: %s", s_current_transaction);
    }

    cJSON_Delete(json);
}

/**
 * @brief Get cluster ID from cluster name
 * @param name Cluster name (e.g., "genOnOff")
 * @return Cluster ID or 0xFFFF if not found
 */
static uint16_t get_cluster_id_by_name(const char *name)
{
    if (name == NULL) {
        return 0xFFFF;
    }

    for (size_t i = 0; s_cluster_map[i].name != NULL; i++) {
        if (strcmp(s_cluster_map[i].name, name) == 0) {
            return s_cluster_map[i].cluster_id;
        }
    }

    /* Try parsing as hex value */
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        return (uint16_t)strtol(name, NULL, 16);
    }

    return 0xFFFF;
}

/**
 * @brief Get attribute ID from attribute name for a cluster
 * @param cluster_id Cluster ID
 * @param name Attribute name (e.g., "onOff")
 * @return Attribute ID or 0xFFFF if not found
 */
static uint16_t get_attr_id_by_name(uint16_t cluster_id, const char *name)
{
    if (name == NULL) {
        return 0xFFFF;
    }

    for (size_t i = 0; s_attr_map[i].name != NULL; i++) {
        if (s_attr_map[i].cluster_id == cluster_id &&
            strcmp(s_attr_map[i].name, name) == 0) {
            return s_attr_map[i].attr_id;
        }
    }

    /* Try parsing as hex value */
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        return (uint16_t)strtol(name, NULL, 16);
    }

    /* Try parsing as decimal value */
    char *endptr;
    long val = strtol(name, &endptr, 10);
    if (*endptr == '\0' && val >= 0 && val <= 0xFFFF) {
        return (uint16_t)val;
    }

    return 0xFFFF;
}

/**
 * @brief Find device by friendly name or IEEE address
 *
 * Uses device_registry_find_by_id() which searches by:
 *   1. friendly_name lookup
 *   2. IEEE address string parsing (format: "0x00124b001234abcd")
 *
 * @param id Device identifier (friendly name or IEEE string)
 * @return Device pointer or NULL if not found
 */
static device_t* find_device_by_id(const char *id)
{
    return device_registry_find_by_id(id);
}

/**
 * @brief Create NVS key from IEEE address
 * @param ieee_addr 64-bit IEEE address
 * @param key_buf Output buffer for key (min 17 bytes)
 */
static void create_nvs_key(uint64_t ieee_addr, char *key_buf)
{
    snprintf(key_buf, 17, "%016" PRIX64, ieee_addr);
}

esp_err_t bridge_request_handler_init(void)
{
    ESP_LOGI(TAG, "Bridge request handler initialized");
    return ESP_OK;
}

/* ============================================================================
 * Dispatcher Wrapper Functions
 *
 * These functions adapt the unified handler signature to the specific
 * function signatures used by the actual implementation functions.
 * ============================================================================ */

static esp_err_t handle_permit_join(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;

    json_permit_join_options_t pj_options;
    if (json_parse_permit_join_extended(payload, &pj_options) == ESP_OK) {
        return bridge_request_permit_join_extended(&pj_options);
    }

    bridge_response_publish_error(RESPONSE_TOPIC_PERMIT_JOIN,
                                 "Invalid permit_join payload",
                                 get_current_transaction());
    return ESP_FAIL;
}

static esp_err_t handle_device_remove(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_REMOVE,
                                     "Invalid JSON payload",
                                     get_current_transaction());
        return ESP_FAIL;
    }

    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    cJSON *force_item = cJSON_GetObjectItem(json, "force");

    if (id_item == NULL || !cJSON_IsString(id_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_REMOVE,
                                     "Invalid device/remove payload: missing 'id' field",
                                     get_current_transaction());
        return ESP_FAIL;
    }

    bool force = (force_item != NULL && cJSON_IsBool(force_item)) ?
                 cJSON_IsTrue(force_item) : false;
    esp_err_t ret = bridge_request_device_remove_force(id_item->valuestring, force);
    cJSON_Delete(json);
    return ret;
}

static esp_err_t handle_device_rename(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;

    char old_name[64], new_name[64];
    if (json_parse_device_rename(payload, old_name, new_name, sizeof(old_name)) == ESP_OK) {
        return bridge_request_device_rename(old_name, new_name);
    }

    bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_RENAME,
                                 "Invalid device/rename payload: missing 'from' or 'to' field",
                                 get_current_transaction());
    return ESP_FAIL;
}

static esp_err_t handle_health_check(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    return bridge_request_health_check();
}

static esp_err_t handle_config_get(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    ESP_LOGI(TAG, "Config get requested");
    return bridge_request_config_get();
}

static esp_err_t handle_config_set(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Config set requested");
    return bridge_request_config_set(payload);
}

static esp_err_t handle_ota_check(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    ESP_LOGI(TAG, "OTA check requested");
    return bridge_request_ota_check();
}

static esp_err_t handle_ota_install(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    ESP_LOGI(TAG, "OTA install requested");
    return bridge_request_ota_install();
}

static esp_err_t handle_restart(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    ESP_LOGI(TAG, "Restart requested");
    return bridge_request_restart();
}

static esp_err_t handle_factory_reset(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    ESP_LOGW(TAG, "Factory reset requested - all settings will be erased!");
    return bridge_request_factory_reset();
}

/**
 * @brief Remove all files inside a directory (non-recursive, flat dir only)
 *
 * Used by network reset and factory reset to clean LittleFS directories.
 * Does NOT remove the directory itself.
 *
 * @param dir_path Absolute path to directory (e.g. "/littlefs/backup")
 * @return Number of files removed, or -1 on error
 */
static int remove_dir_contents(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return 0;  /* Directory doesn't exist - nothing to clean */
    }

    int removed = 0;
    struct dirent *entry;
    char filepath[288];  /* dir_path (max ~30) + '/' + d_name (max 255) + NUL */

    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (entry->d_name[0] == '.') {
            continue;
        }
        snprintf(filepath, sizeof(filepath), "%.30s/%s", dir_path, entry->d_name);
        if (remove(filepath) == 0) {
            removed++;
        } else {
            ESP_LOGW(TAG, "Failed to remove: %s", filepath);
        }
    }

    closedir(dir);
    return removed;
}

/**
 * @brief Handle network reset request
 * Erases Zigbee network data, devices, and state but preserves WiFi/MQTT config.
 */
static esp_err_t handle_network_reset(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    ESP_LOGI(TAG, "Network reset requested");

    /* Publish response before reset */
    cJSON *data = cJSON_CreateObject();
    if (data != NULL) {
        cJSON_AddStringToObject(data, "state", "resetting_network");
        bridge_response_publish_ok(RESPONSE_TOPIC_RESET_NETWORK, data, get_current_transaction());
        cJSON_Delete(data);
    }

    /* Wait for MQTT delivery */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Clear in-memory device registry, bindings, and NG persistence */
    ESP_LOGI(TAG, "Clearing device registry and bindings...");
    device_registry_clear_all();
    device_persistence_clear();
    zb_converter_unbind_all();
    tuya_driver_unbind_all();

    /* Erase Zigbee-related NVS namespaces (preserves wifi_config + gateway_cfg) */
    static const char *zb_namespaces[] = {
        "devices",       /* NG device persistence (primary) */
        "zb_devices",    /* Legacy device persistence */
        "dev_names",     /* Legacy device names */
        "dev_opts",      /* Device options (friendly names, icons) */
        "zb_network",    /* Zigbee network config (PAN ID, channel) */
        "zb_report",     /* Zigbee attribute reporting config */
        "zb_binding",    /* Zigbee binding table */
        "zb_groups",     /* Zigbee group membership */
        "zb_scenes",     /* Zigbee scenes */
        "zb_avail",      /* Device availability tracking */
        "zb_topology",   /* Network topology map */
        "zb_diag",       /* Zigbee diagnostics counters */
        "zb_ic",         /* Zigbee install codes */
        "gp_storage",    /* Green Power proxy config */
        "zb_ota",        /* Zigbee OTA update state */
        "zb_touchlink",  /* Touchlink pairing state */
        "zb_router",     /* Router configuration */
        "zb_multi_pan",  /* Multi-PAN coordinator state */
        "zb_backup",     /* Backup metadata */
        "tuya_drv",      /* Tuya device driver config */
    };

    for (size_t i = 0; i < sizeof(zb_namespaces) / sizeof(zb_namespaces[0]); i++) {
        nvs_handle_t handle;
        esp_err_t ret = nvs_open(zb_namespaces[i], NVS_READWRITE, &handle);
        if (ret == ESP_OK) {
            nvs_erase_all(handle);
            nvs_commit(handle);
            nvs_close(handle);
            ESP_LOGI(TAG, "Erased NVS namespace: %s", zb_namespaces[i]);
        }
    }

    /* Erase Zigbee storage partitions */
    static const char *zb_partitions[] = {"zb_storage", "zb_fct"};
    for (size_t i = 0; i < sizeof(zb_partitions) / sizeof(zb_partitions[0]); i++) {
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, zb_partitions[i]);
        if (part != NULL) {
            esp_partition_erase_range(part, 0, part->size);
            ESP_LOGI(TAG, "Erased partition '%s'", zb_partitions[i]);
        }
    }

    /* Erase persisted state */
#if CONFIG_STATE_PERSISTENCE_ENABLE
    state_persistence_erase_all();
#endif

    /* Clean LittleFS: backups (contain network keys!) and OTA staging */
    int n;
    n = remove_dir_contents("/littlefs/backup");
    if (n > 0) ESP_LOGI(TAG, "Removed %d backup files", n);
    n = remove_dir_contents("/littlefs/ota_storage");
    if (n > 0) ESP_LOGI(TAG, "Removed %d OTA staging files", n);

    ESP_LOGW(TAG, "Network reset complete - restarting...");
    esp_restart();

    /* Never reached */
    return ESP_OK;
}

/**
 * @brief Handle config reset request
 * Erases WiFi and gateway config but preserves Zigbee network and devices.
 */
static esp_err_t handle_config_reset(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    ESP_LOGI(TAG, "Config reset requested");

    /* Publish response before reset */
    cJSON *data = cJSON_CreateObject();
    if (data != NULL) {
        cJSON_AddStringToObject(data, "state", "resetting_config");
        bridge_response_publish_ok(RESPONSE_TOPIC_RESET_CONFIG, data, get_current_transaction());
        cJSON_Delete(data);
    }

    /* Wait for MQTT delivery */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Erase WiFi and gateway config namespaces */
    static const char *config_namespaces[] = {
        "wifi_config", "gateway_cfg"
    };

    for (size_t i = 0; i < sizeof(config_namespaces) / sizeof(config_namespaces[0]); i++) {
        nvs_handle_t handle;
        esp_err_t ret = nvs_open(config_namespaces[i], NVS_READWRITE, &handle);
        if (ret == ESP_OK) {
            nvs_erase_all(handle);
            nvs_commit(handle);
            nvs_close(handle);
            ESP_LOGI(TAG, "Erased NVS namespace: %s", config_namespaces[i]);
        }
    }

    ESP_LOGW(TAG, "Config reset complete - restarting...");
    esp_restart();

    /* Never reached */
    return ESP_OK;
}

static esp_err_t handle_networkmap(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    ESP_LOGI(TAG, "Network map requested");
    return bridge_request_networkmap();
}

static esp_err_t handle_availability_check(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Device availability check requested");
    return bridge_request_availability_check(payload);
}

static esp_err_t handle_device_configure(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Device configure requested");
    return bridge_request_device_configure(payload);
}

static esp_err_t handle_device_options(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Device options requested");
    return bridge_request_device_options(payload);
}

static esp_err_t handle_device_get(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Device get requested");
    return bridge_request_device_get(payload);
}

static esp_err_t handle_logging(const char *topic, const char *payload, size_t len)
{
    (void)len;
    ESP_LOGI(TAG, "Logging configuration requested");
    return mqtt_logger_process_request(topic, payload, strlen(payload));
}

static esp_err_t handle_install_code_add(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Install code add requested");
    return bridge_request_install_code_add(payload);
}

static esp_err_t handle_install_code_remove(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Install code remove requested");
    return bridge_request_install_code_remove(payload);
}

static esp_err_t handle_install_code_list(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Install code list requested");
    return bridge_request_install_code_list(payload);
}

static esp_err_t handle_backup(const char *topic, const char *payload, size_t len)
{
    (void)len;
    ESP_LOGI(TAG, "Backup requested: %s", topic);
    return zb_backup_process_mqtt_backup(topic, payload, strlen(payload));
}

static esp_err_t handle_restore(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Restore requested");
    return zb_backup_process_mqtt_restore(payload, strlen(payload));
}

static esp_err_t handle_backup_list(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Backup list requested");
    return zb_backup_process_mqtt_list(payload, strlen(payload));
}

static esp_err_t handle_coordinator_check(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    ESP_LOGI(TAG, "Coordinator health check requested");
    return bridge_request_coordinator_check();
}

static esp_err_t handle_coordinator_version(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;
    ESP_LOGI(TAG, "Coordinator version requested");
    return bridge_request_coordinator_version();
}

static esp_err_t handle_coordinator_settings(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Coordinator settings requested");
    return bridge_request_coordinator_settings(payload);
}

static esp_err_t handle_network_options(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Network options/channel change requested");
    return bridge_request_network_options(payload);
}

static esp_err_t handle_network_ext_pan_id(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Network extended PAN ID request");
    return bridge_request_network_extended_pan_id(payload);
}

static esp_err_t handle_network_key_rotate(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Network key rotation requested");
    return bridge_request_network_key_rotate(payload);
}

static esp_err_t handle_poll_control(const char *topic, const char *payload, size_t len)
{
    (void)len;

    /* Poll Control topic routing based on /set suffix */
    if (strstr(topic, "/set") != NULL) {
        ESP_LOGI(TAG, "Poll control set requested");
        return bridge_request_poll_control_set(topic, payload);
    }

    ESP_LOGI(TAG, "Poll control get requested");
    return bridge_request_poll_control_get(topic, payload);
}

static esp_err_t handle_time_set(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Time set requested");
    return bridge_request_time_set(payload);
}

static esp_err_t handle_time_config(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;
    ESP_LOGI(TAG, "Time config requested");
    return bridge_request_time_config(payload);
}

static esp_err_t handle_time_get(const char *topic, const char *payload, size_t len)
{
    (void)payload;
    (void)len;

    /* Time Server get topic - make sure this is the exact /request/time topic,
     * not a /time/set or /time/config topic */
    if (strstr(topic, "/time/") != NULL) {
        /* This is /time/set or /time/config, not the base /time endpoint */
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Time get requested");
    return bridge_request_time_get();
}

/**
 * @brief Handle converter DB file upload
 *
 * Accepts JSON payload with "file" (filename) and "data" (base64-encoded content).
 * Writes the decoded file to /littlefs/converters/<filename>.
 * If the file is "index.json", reinitializes the converter loader.
 */
static esp_err_t handle_converter_db_update(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;

    ESP_LOGI(TAG, "Converter DB update requested");

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_CONVERTER_DB_UPDATE,
                                      "Invalid JSON payload",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *file_item = cJSON_GetObjectItem(json, "file");
    const cJSON *data_item = cJSON_GetObjectItem(json, "data");

    if (!cJSON_IsString(file_item) || !cJSON_IsString(data_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_CONVERTER_DB_UPDATE,
                                      "Missing 'file' or 'data' field",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    const char *filename = file_item->valuestring;
    const char *b64_data = data_item->valuestring;

    /* Validate filename (no path traversal) */
    if (filename == NULL || filename[0] == '\0' || strchr(filename, '/') != NULL ||
        strchr(filename, '\\') != NULL || strcmp(filename, "..") == 0) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_CONVERTER_DB_UPDATE,
                                      "Invalid filename",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Base64 decode: first pass to get decoded length */
    size_t b64_len = strlen(b64_data);
    size_t decoded_len = 0;
    int mb_ret = mbedtls_base64_decode(NULL, 0, &decoded_len,
                                        (const unsigned char *)b64_data, b64_len);
    if (mb_ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && mb_ret != 0) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_CONVERTER_DB_UPDATE,
                                      "Base64 decode failed",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *decoded = (uint8_t *)mem_alloc(decoded_len + 1, MEM_CAP_DEFAULT);
    if (decoded == NULL) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_CONVERTER_DB_UPDATE,
                                      "Out of memory for decode buffer",
                                      get_current_transaction());
        return ESP_ERR_NO_MEM;
    }

    mb_ret = mbedtls_base64_decode(decoded, decoded_len + 1, &decoded_len,
                                    (const unsigned char *)b64_data, b64_len);
    if (mb_ret != 0) {
        mem_ng_free(decoded);
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_CONVERTER_DB_UPDATE,
                                      "Base64 decode failed (pass 2)",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Create directory if needed (may fail if exists, that's OK) */
    mkdir("/littlefs/converters", 0755);

    /* Write decoded data to file */
    char path[80];
    snprintf(path, sizeof(path), "/littlefs/converters/%s", filename);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        mem_ng_free(decoded);
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_CONVERTER_DB_UPDATE,
                                      "Failed to write file to LittleFS",
                                      get_current_transaction());
        return ESP_FAIL;
    }

    size_t written = fwrite(decoded, 1, decoded_len, f);
    fclose(f);
    mem_ng_free(decoded);

    if (written != decoded_len) {
        ESP_LOGE(TAG, "Short write to %s: %zu/%zu", path, written, decoded_len);
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_CONVERTER_DB_UPDATE,
                                      "Incomplete write to LittleFS",
                                      get_current_transaction());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wrote %zu bytes to %s", decoded_len, path);

    /* If this was index.json, reinitialize the loader */
    bool reinitialized = false;
    if (strcmp(filename, "index.json") == 0) {
        esp_err_t loader_ret = zb_converter_loader_reload_index();
        if (loader_ret == ESP_OK) {
            reinitialized = true;
            ESP_LOGI(TAG, "Converter loader reinitialized after index.json update");
        } else {
            ESP_LOGW(TAG, "Converter loader reinit failed: %s", esp_err_to_name(loader_ret));
        }
    }

    /* Build success response */
    cJSON *resp_data = cJSON_CreateObject();
    if (resp_data != NULL) {
        cJSON_AddStringToObject(resp_data, "file", filename);
        cJSON_AddNumberToObject(resp_data, "size", (double)decoded_len);
        if (reinitialized) {
            cJSON_AddTrueToObject(resp_data, "loader_reinitialized");
        }
    }

    bridge_response_publish_ok(RESPONSE_TOPIC_CONVERTER_DB_UPDATE,
                               resp_data,
                               get_current_transaction());

    cJSON_Delete(resp_data);
    cJSON_Delete(json);

    return ESP_OK;
}

/* ============================================================================
 * Custom Quirk Handlers
 * ============================================================================ */

/**
 * @brief Add a custom community quirk
 *
 * Payload: {"name": "my_quirk", "definition": { ... device JSON ... }}
 */
static esp_err_t handle_custom_quirk_add(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;

    cJSON *json = cJSON_Parse(payload);
    if (!json) {
        bridge_response_publish_error(RESPONSE_TOPIC_CUSTOM_QUIRK_ADD,
                                      "Invalid JSON payload",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *name_item = cJSON_GetObjectItem(json, "name");
    const cJSON *def_item = cJSON_GetObjectItem(json, "definition");

    if (!cJSON_IsString(name_item) || !cJSON_IsObject(def_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_CUSTOM_QUIRK_ADD,
                                      "Missing 'name' (string) or 'definition' (object)",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Serialize definition back to string for the quirk API */
    char *def_str = cJSON_PrintUnformatted(def_item);
    if (!def_str) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_CUSTOM_QUIRK_ADD,
                                      "Failed to serialize definition",
                                      get_current_transaction());
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = zb_custom_quirk_add(name_item->valuestring, def_str);
    cJSON_free(def_str);
    cJSON_Delete(json);

    if (ret != ESP_OK) {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "Failed to add quirk: %s", esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_CUSTOM_QUIRK_ADD,
                                      err_msg,
                                      get_current_transaction());
        return ret;
    }

    cJSON *resp_data = cJSON_CreateObject();
    if (resp_data) {
        cJSON_AddStringToObject(resp_data, "name", name_item->valuestring);
        cJSON_AddNumberToObject(resp_data, "total", (double)zb_custom_quirk_count());
    }

    bridge_response_publish_ok(RESPONSE_TOPIC_CUSTOM_QUIRK_ADD,
                               resp_data,
                               get_current_transaction());
    cJSON_Delete(resp_data);

    return ESP_OK;
}

/**
 * @brief Remove a custom community quirk
 *
 * Payload: {"name": "my_quirk"}
 */
static esp_err_t handle_custom_quirk_remove(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;

    cJSON *json = cJSON_Parse(payload);
    if (!json) {
        bridge_response_publish_error(RESPONSE_TOPIC_CUSTOM_QUIRK_REMOVE,
                                      "Invalid JSON payload",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *name_item = cJSON_GetObjectItem(json, "name");
    if (!cJSON_IsString(name_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_CUSTOM_QUIRK_REMOVE,
                                      "Missing 'name' field",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = zb_custom_quirk_remove(name_item->valuestring);
    cJSON_Delete(json);

    if (ret != ESP_OK) {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "Failed to remove quirk: %s", esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_CUSTOM_QUIRK_REMOVE,
                                      err_msg,
                                      get_current_transaction());
        return ret;
    }

    bridge_response_publish_ok(RESPONSE_TOPIC_CUSTOM_QUIRK_REMOVE,
                               NULL,
                               get_current_transaction());

    return ESP_OK;
}

/**
 * @brief List all custom community quirks
 *
 * Payload: (empty or {})
 */
static esp_err_t handle_custom_quirk_list(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)payload;
    (void)len;

    zb_custom_quirk_info_t infos[ZB_CUSTOM_QUIRK_MAX];
    size_t count = zb_custom_quirk_list(infos, ZB_CUSTOM_QUIRK_MAX);

    cJSON *resp_data = cJSON_CreateObject();
    if (!resp_data) {
        bridge_response_publish_error(RESPONSE_TOPIC_CUSTOM_QUIRK_LIST,
                                      "Out of memory",
                                      get_current_transaction());
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(resp_data, "count", (double)count);

    cJSON *arr = cJSON_AddArrayToObject(resp_data, "quirks");
    if (arr) {
        for (size_t i = 0; i < count; i++) {
            cJSON *item = cJSON_CreateObject();
            if (!item) break;
            cJSON_AddStringToObject(item, "name", infos[i].name ? infos[i].name : "");
            if (infos[i].manufacturer) {
                cJSON_AddStringToObject(item, "manufacturer", infos[i].manufacturer);
            }
            if (infos[i].model) {
                cJSON_AddStringToObject(item, "model", infos[i].model);
            }
            if (infos[i].description) {
                cJSON_AddStringToObject(item, "description", infos[i].description);
            }
            cJSON_AddItemToArray(arr, item);
        }
    }

    bridge_response_publish_ok(RESPONSE_TOPIC_CUSTOM_QUIRK_LIST,
                               resp_data,
                               get_current_transaction());
    cJSON_Delete(resp_data);

    return ESP_OK;
}

/**
 * @brief Find and execute the appropriate handler for a request topic
 *
 * Searches the dispatch table for a matching handler and executes it.
 *
 * @param topic MQTT topic
 * @param payload Null-terminated payload string
 * @param len Payload length
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no handler found
 */
static esp_err_t dispatch_request(const char *topic, const char *payload, size_t len)
{
    for (size_t i = 0; s_request_handlers[i].topic_suffix != NULL; i++) {
        const request_handler_entry_t *entry = &s_request_handlers[i];
        bool matched = false;

        if (entry->use_strstr) {
            /* Substring matching */
            matched = (strstr(topic, entry->topic_suffix) != NULL);
        } else {
            /* Exact topic matching using mqtt_topic_matches */
            matched = mqtt_topic_matches(topic, entry->topic_suffix);
        }

        if (matched) {
            ESP_LOGD(TAG, "Dispatching to handler for: %s", entry->topic_suffix);
            return entry->handler(topic, payload, len);
        }
    }

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bridge_request_process(const char *topic, const char *payload, size_t len)
{
    if (topic == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Processing bridge request: %s", topic);
    ESP_LOGD(TAG, "Payload: %.*s", (int)len, payload);

    /* Null-terminate payload */
    char *payload_str = (char *)mem_alloc(len + 1, MEM_CAP_DEFAULT);
    if (payload_str == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(payload_str, payload, len);
    payload_str[len] = '\0';

    /* Extract transaction ID before processing */
    extract_transaction_from_payload(payload_str);

    /* Dispatch to appropriate handler */
    esp_err_t ret = dispatch_request(topic, payload_str, len);

    /* Handle unknown request type */
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Unknown bridge request: %s", topic);

        /* Try to build response topic dynamically */
        char response_topic[MQTT_TOPIC_MAX_LEN];
        esp_err_t topic_ret = bridge_response_topic_from_request(
            topic, response_topic, sizeof(response_topic));
        if (topic_ret == ESP_OK) {
            bridge_response_publish_error(response_topic,
                                         "Unknown request type",
                                         get_current_transaction());
        }
    }

    /* Clear transaction ID */
    s_current_transaction[0] = '\0';

    mem_ng_free(payload_str);
    return ret;
}

esp_err_t bridge_request_permit_join(uint8_t duration)
{
    /* Delegate to extended version */
    json_permit_join_options_t options = {
        .value = (duration > 0),
        .time = duration,
        .has_device = false
    };
    memset(options.device_ieee, 0, sizeof(options.device_ieee));

    return bridge_request_permit_join_extended(&options);
}

esp_err_t bridge_request_permit_join_extended(const json_permit_join_options_t *options)
{
    if (options == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t duration = options->value ? options->time : 0;

    if (options->has_device) {
        ESP_LOGI(TAG, "Permit join request: duration=%d, target device specified", duration);
    } else {
        ESP_LOGI(TAG, "Permit join request: duration=%d", duration);
    }

    /* IMPORTANT: Publish response BEFORE calling coordinator permit_join.
     * The coordinator permit_join function enters PAIRING phase which disconnects
     * WiFi to give Zigbee 100% radio time. If we wait until after the call,
     * WiFi will already be down and the response won't be delivered. */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for permit join response");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(data, "value", duration > 0);
    cJSON_AddNumberToObject(data, "time", duration);

    if (options->has_device) {
        /* Format IEEE address for response */
        char ieee_str[19];
        snprintf(ieee_str, sizeof(ieee_str), "0x%02X%02X%02X%02X%02X%02X%02X%02X",
                 options->device_ieee[7], options->device_ieee[6],
                 options->device_ieee[5], options->device_ieee[4],
                 options->device_ieee[3], options->device_ieee[2],
                 options->device_ieee[1], options->device_ieee[0]);
        cJSON_AddStringToObject(data, "device", ieee_str);
    }

    /* Publish early response before WiFi goes down */
    bridge_response_publish_ok(RESPONSE_TOPIC_PERMIT_JOIN, data, get_current_transaction());
    cJSON_Delete(data);

    /* Brief delay to allow TCP to flush the response packet.
     * The coordinator permit_join will enter PAIRING phase which disconnects WiFi.
     * Without this delay, the TCP packet may still be in the buffer when WiFi goes down. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Build coordinator permit join options */
    zb_permit_join_options_t coord_options = {
        .duration = duration,
        .has_target_device = options->has_device
    };

    if (options->has_device) {
        memcpy(coord_options.target_ieee_addr, options->device_ieee, 8);
    } else {
        memset(coord_options.target_ieee_addr, 0, 8);
    }

    /* Now call coordinator - this will enter PAIRING phase and disconnect WiFi */
    esp_err_t ret = zb_coordinator_permit_join_with_options(&coord_options);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set permit join: %s", esp_err_to_name(ret));
        /* Note: Response already sent, but operation failed.
         * The permit_join event or subsequent state will indicate the actual status. */
        return ret;
    }

    ESP_LOGI(TAG, "Permit join set: duration=%d", duration);
    return ESP_OK;
}

esp_err_t bridge_request_device_rename(const char *old_name, const char *new_name)
{
    if (old_name == NULL || new_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Device rename request: %s -> %s", old_name, new_name);

    /* Find device using NG registry */
    device_t *target = find_device_by_id(old_name);
    if (target == NULL) {
        ESP_LOGW(TAG, "Device not found: %s", old_name);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Device not found: %s", old_name);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_RENAME,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_NOT_FOUND;
    }

    /* device_t.id is the IEEE address as uint64_t */
    uint64_t ieee64 = target->id;

    /* Rename device in NG registry */
    esp_err_t ret = device_registry_set_friendly_name(target->id, new_name);

    if (ret == ESP_OK) {
        /* Save device to NVS for persistence across reboots (NG persistence) */
        esp_err_t nvs_ret = device_persistence_save(target);
        if (nvs_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to persist device to NVS: %s",
                     esp_err_to_name(nvs_ret));
            /* Continue anyway - the rename was successful in memory */
        } else {
            ESP_LOGI(TAG, "Device persisted to NVS");
        }

        /* Publish bridge event: device_renamed */
        bridge_event_device_renamed(ieee64, old_name, new_name);

        /* Build success response data */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object for device rename response");
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(data, "from", old_name);
        cJSON_AddStringToObject(data, "to", new_name);
        cJSON_AddBoolToObject(data, "homeassistant_rename", false);

        bridge_response_publish_ok(RESPONSE_TOPIC_DEVICE_RENAME, data, get_current_transaction());
        cJSON_Delete(data);

        ESP_LOGI(TAG, "Device renamed: %s -> %s", old_name, new_name);
    } else {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Failed to rename device: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_RENAME,
                                     error_msg,
                                     get_current_transaction());
    }

    return ret;
}

esp_err_t bridge_request_health_check(void)
{
    ESP_LOGI(TAG, "Health check requested");

    /* Create health status data */
    cJSON *data = json_create_bridge_info();
    if (data == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_HEALTH_CHECK,
                                     "Failed to create health info",
                                     get_current_transaction());
        return ESP_FAIL;
    }

    /* Add health status */
    cJSON_AddStringToObject(data, "health", "healthy");

    bridge_response_publish_ok(RESPONSE_TOPIC_HEALTH_CHECK, data, get_current_transaction());
    cJSON_Delete(data);

    return ESP_OK;
}

esp_err_t bridge_request_restart(void)
{
    ESP_LOGW(TAG, "Restarting ESP32 in 2 seconds...");

    /* Build response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for restart response");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(data, "state", "restarting");
    cJSON_AddNumberToObject(data, "delay_ms", GW_TIMEOUT_LONG_MS);

    bridge_response_publish_ok(RESPONSE_TOPIC_RESTART, data, get_current_transaction());
    cJSON_Delete(data);

    /* Wait for message to be sent */
    vTaskDelay(pdMS_TO_TICKS(GW_TIMEOUT_LONG_MS));

    /* Restart */
    esp_restart();

    /* Never reached */
    return ESP_OK;
}

esp_err_t bridge_request_factory_reset(void)
{
    ESP_LOGW(TAG, "FACTORY RESET: Erasing ALL NVS data in 2 seconds...");

    /* Build response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for factory reset response");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(data, "state", "factory_resetting");
    cJSON_AddNumberToObject(data, "delay_ms", GW_TIMEOUT_LONG_MS);

    bridge_response_publish_ok(RESPONSE_TOPIC_FACTORY_RESET, data, get_current_transaction());
    cJSON_Delete(data);

    /* Wait for message to be sent */
    vTaskDelay(pdMS_TO_TICKS(GW_TIMEOUT_LONG_MS));

    /* Clear in-memory registries and bindings (prevents stale saves between erase and restart) */
    ESP_LOGI(TAG, "Clearing device registry, bindings, and persistence...");
    device_registry_clear_all();
    device_persistence_clear();
    zb_converter_unbind_all();
    tuya_driver_unbind_all();

    /* Erase ALL NVS data */
    ESP_LOGW(TAG, "Erasing NVS flash...");
    esp_err_t ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "NVS erased successfully");
    }

    /* Erase Zigbee storage partitions (zb_storage + zb_fct) */
    static const char *zb_partitions[] = {"zb_storage", "zb_fct"};
    for (size_t i = 0; i < sizeof(zb_partitions) / sizeof(zb_partitions[0]); i++) {
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, zb_partitions[i]);
        if (part != NULL) {
            ret = esp_partition_erase_range(part, 0, part->size);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Erased partition '%s' (%lu bytes)", zb_partitions[i],
                         (unsigned long)part->size);
            } else {
                ESP_LOGE(TAG, "Failed to erase partition '%s': %s", zb_partitions[i],
                         esp_err_to_name(ret));
            }
        } else {
            ESP_LOGW(TAG, "Partition '%s' not found", zb_partitions[i]);
        }
    }

    /* Erase LittleFS user data but PRESERVE converter DB.
     * Converter DB (/littlefs/converters/) is firmware-like data that was
     * uploaded separately and should survive factory reset. */
    {
        int n;
        n = remove_dir_contents("/littlefs/state");
        if (n > 0) ESP_LOGI(TAG, "Removed %d state files", n);
        n = remove_dir_contents("/littlefs/backup");
        if (n > 0) ESP_LOGI(TAG, "Removed %d backup files (network keys)", n);
        n = remove_dir_contents("/littlefs/ota_storage");
        if (n > 0) ESP_LOGI(TAG, "Removed %d OTA staging files", n);
    }

    /* Restart to apply changes */
    ESP_LOGW(TAG, "Restarting...");
    esp_restart();

    /* Never reached */
    return ESP_OK;
}

/**
 * @brief Iterator callback for network map device enumeration
 *
 * Adds each available device to the JSON array.
 *
 * @param dev Device pointer
 * @param ctx Context (cJSON array pointer)
 * @return true to continue iteration
 */
static bool networkmap_device_iterator(device_t *dev, void *ctx)
{
    cJSON *devices = (cJSON *)ctx;

    if (dev == NULL || (dev->availability != DEV_AVAIL_ONLINE)) {
        return true;  /* Continue iteration */
    }

    cJSON *dev_obj = cJSON_CreateObject();
    if (dev_obj == NULL) {
        return true;  /* Continue iteration even on allocation failure */
    }

    /* Format IEEE address from uint64_t id */
    char ieee_str[24];
    device_id_to_str(dev->id, ieee_str, sizeof(ieee_str));
    cJSON_AddStringToObject(dev_obj, "ieee_address", ieee_str);
    cJSON_AddNumberToObject(dev_obj, "nwk_address", dev->proto.zigbee.short_addr);
    cJSON_AddStringToObject(dev_obj, "friendly_name", dev->friendly_name);

    /* Derive type from device capabilities */
    const char *type_str = "Unknown";
    if (dev->capabilities & (DEV_CAP_BRIGHTNESS | DEV_CAP_COLOR_TEMP | DEV_CAP_COLOR_XY)) {
        type_str = "Light";
    } else if (dev->capabilities & DEV_CAP_ON_OFF) {
        /* Could be light or switch - check for power monitoring */
        if (dev->capabilities & DEV_CAP_POWER) {
            type_str = "Plug";
        } else {
            type_str = "Switch";
        }
    } else if (dev->capabilities & (DEV_CAP_TEMPERATURE | DEV_CAP_HUMIDITY |
                                    DEV_CAP_PRESSURE | DEV_CAP_MOTION |
                                    DEV_CAP_CONTACT)) {
        type_str = "Sensor";
    } else if (dev->capabilities & DEV_CAP_LOCK) {
        type_str = "Lock";
    } else if (dev->capabilities & (DEV_CAP_CLIMATE | DEV_CAP_FAN)) {
        type_str = "Climate";
    } else if (dev->capabilities & DEV_CAP_COVER) {
        type_str = "Cover";
    } else if (dev->capabilities & (DEV_CAP_SMOKE | DEV_CAP_CO | DEV_CAP_WATER_LEAK |
                                    DEV_CAP_VIBRATION)) {
        type_str = "Alarm";
    }
    cJSON_AddStringToObject(dev_obj, "type", type_str);
    cJSON_AddBoolToObject(dev_obj, "online", (dev->availability == DEV_AVAIL_ONLINE));
    cJSON_AddNumberToObject(dev_obj, "lqi", dev->proto.zigbee.lqi);

    cJSON_AddItemToArray(devices, dev_obj);
    return true;  /* Continue iteration */
}

esp_err_t bridge_request_networkmap(void)
{
    ESP_LOGI(TAG, "Network map generation requested");

    /* Get network information */
    zb_coordinator_settings_t settings;
    esp_err_t ret = zb_coordinator_get_settings(&settings);
    if (ret != ESP_OK) {
        bridge_response_publish_error("zigbee2mqtt/bridge/response/networkmap",
                                     "Failed to get coordinator settings",
                                     get_current_transaction());
        return ret;
    }

    /* Get coordinator version info (contains IEEE address) */
    zb_coordinator_version_t version;
    ret = zb_coordinator_get_version(&version);
    if (ret != ESP_OK) {
        bridge_response_publish_error("zigbee2mqtt/bridge/response/networkmap",
                                     "Failed to get coordinator version",
                                     get_current_transaction());
        return ret;
    }

    /* Build network map response */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        bridge_response_publish_error("zigbee2mqtt/bridge/response/networkmap",
                                     "Failed to create JSON object",
                                     get_current_transaction());
        return ESP_ERR_NO_MEM;
    }

    /* Add coordinator info */
    cJSON *coordinator = cJSON_CreateObject();
    if (coordinator == NULL) {
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(coordinator, "ieee_address", version.ieee_address);
    cJSON_AddNumberToObject(coordinator, "nwk_address", 0x0000);
    cJSON_AddStringToObject(coordinator, "type", "Coordinator");
    cJSON_AddItemToObject(data, "coordinator", coordinator);

    /* Add network info */
    cJSON_AddNumberToObject(data, "channel", settings.channel);

    char pan_id_str[8];
    snprintf(pan_id_str, sizeof(pan_id_str), "0x%04X", settings.pan_id);
    cJSON_AddStringToObject(data, "pan_id", pan_id_str);

    /* Add device list */
    cJSON *devices = cJSON_CreateArray();
    if (devices == NULL) {
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    /* Iterate through all Zigbee devices using NG registry iterator */
    size_t device_count = device_registry_count();
    device_registry_iterate_zigbee(networkmap_device_iterator, devices);
    cJSON_AddItemToObject(data, "devices", devices);
    cJSON_AddNumberToObject(data, "device_count", device_count);

    bridge_response_publish_ok("zigbee2mqtt/bridge/response/networkmap", data, get_current_transaction());
    cJSON_Delete(data);

    ESP_LOGI(TAG, "Network map published with %zu devices", device_count);
    return ESP_OK;
}

esp_err_t bridge_request_config_get(void)
{
    ESP_LOGI(TAG, "Configuration get requested");

    /* Get configuration as JSON */
    char config_json[1024];
    esp_err_t ret = config_manager_export_json(config_json, sizeof(config_json));

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to export configuration: %s", esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_CONFIG_GET,
                                     "Failed to export configuration",
                                     get_current_transaction());
        return ret;
    }

    /* Parse config JSON to include as data */
    cJSON *data = cJSON_Parse(config_json);
    if (data == NULL) {
        /* If parsing fails, wrap raw string in data object */
        data = cJSON_CreateObject();
        if (data == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object for config get response");
            bridge_response_publish_error(RESPONSE_TOPIC_CONFIG_GET,
                                         "Failed to create JSON response",
                                         get_current_transaction());
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(data, "raw_config", config_json);
    }

    bridge_response_publish_ok(RESPONSE_TOPIC_CONFIG_GET, data, get_current_transaction());
    cJSON_Delete(data);

    ESP_LOGI(TAG, "Configuration published");
    return ESP_OK;
}

esp_err_t bridge_request_config_set(const char *payload)
{
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Configuration set requested");

    /*
     * NOTE: Full JSON parsing would require implementation of
     * config_manager_import_json() using cJSON library.
     * This is a placeholder for the response.
     */

    bridge_response_publish_error(RESPONSE_TOPIC_CONFIG_SET,
                                 "Configuration import not fully implemented. "
                                 "Use individual config parameters or restart required.",
                                 get_current_transaction());

    ESP_LOGW(TAG, "Configuration set not fully implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bridge_request_ota_check(void)
{
    ESP_LOGI(TAG, "OTA update check requested");

    /* Trigger OTA check */
    esp_err_t ret = ota_handler_check_for_update();

    /* Get OTA info */
    char ota_json[512];
    ota_handler_get_json(ota_json, sizeof(ota_json));

    /* Publish OTA status via event bus with ownership transfer.
     * Both topic (string literal) and payload (stack buffer) must be strdup'd
     * because the handler frees them after use. */
    char *topic_copy = strdup("zigbee2mqtt/bridge/ota/status");
    char *json_copy = strdup(ota_json);
    if (topic_copy && json_copy) {
        evt_ha_discovery_publish_t ota_evt = {
            .topic = topic_copy,
            .payload_json = json_copy,
            .qos = 0,
            .retain = false
        };
        if (event_publish(EVT_HA_DISCOVERY_PUBLISH, &ota_evt, sizeof(ota_evt)) != ESP_OK) {
            free(topic_copy);
            free(json_copy);
        }
    } else {
        free(topic_copy);
        free(json_copy);
    }

    if (ret != ESP_OK) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "OTA check failed: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_OTA_CHECK,
                                     error_msg,
                                     get_current_transaction());
        return ret;
    }

    /* Build success response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for OTA check response");
        bridge_response_publish_error(RESPONSE_TOPIC_OTA_CHECK,
                                     "Failed to create JSON response",
                                     get_current_transaction());
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(data, "checked", true);
    cJSON_AddStringToObject(data, "message", "Check completed");

    /* Parse OTA info and add to response */
    cJSON *ota_info = cJSON_Parse(ota_json);
    if (ota_info != NULL) {
        cJSON_AddItemToObject(data, "ota_info", ota_info);
    }

    bridge_response_publish_ok(RESPONSE_TOPIC_OTA_CHECK, data, get_current_transaction());
    cJSON_Delete(data);

    ESP_LOGI(TAG, "OTA check completed");
    return ESP_OK;
}

esp_err_t bridge_request_ota_install(void)
{
    ESP_LOGI(TAG, "OTA update install requested");

    /* Build starting response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for OTA install response");
        bridge_response_publish_error(RESPONSE_TOPIC_OTA_INSTALL,
                                     "Failed to create JSON response",
                                     get_current_transaction());
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(data, "state", "starting");
    cJSON_AddStringToObject(data, "message", "OTA update starting...");

    bridge_response_publish_ok(RESPONSE_TOPIC_OTA_INSTALL, data, get_current_transaction());
    cJSON_Delete(data);

    /* Wait for message to be sent */
    vTaskDelay(pdMS_TO_TICKS(GW_WIFI_STABILIZE_DELAY_MS));

    /* Start OTA update (blocking - will reboot on success) */
    esp_err_t ret = ota_handler_start_update();

    /* If we reach here, OTA failed */
    char error_msg[128];
    snprintf(error_msg, sizeof(error_msg), "OTA update failed: %s",
             esp_err_to_name(ret));

    bridge_response_publish_error(RESPONSE_TOPIC_OTA_INSTALL,
                                 error_msg,
                                 get_current_transaction());

    /* Also publish to OTA status topic for monitoring via event bus */
    cJSON *error_status = cJSON_CreateObject();
    if (error_status != NULL) {
        cJSON_AddStringToObject(error_status, "status", "error");
        cJSON_AddStringToObject(error_status, "error", esp_err_to_name(ret));
        bool from_pool;
        char *json_str = pool_json_print_unformatted(error_status, &from_pool);
        if (json_str != NULL) {
            /* Publish via event bus with ownership transfer.
             * topic (string literal) must be strdup'd.
             * json_str from pool must be strdup'd, then pool original freed. */
            char *topic_copy = strdup("zigbee2mqtt/bridge/ota/status");
            char *json_copy = strdup(json_str);
            pool_json_free(json_str, from_pool);
            if (topic_copy && json_copy) {
                evt_ha_discovery_publish_t ota_err_evt = {
                    .topic = topic_copy,
                    .payload_json = json_copy,
                    .qos = 0,
                    .retain = false
                };
                if (event_publish(EVT_HA_DISCOVERY_PUBLISH, &ota_err_evt, sizeof(ota_err_evt)) != ESP_OK) {
                    free(topic_copy);
                    free(json_copy);
                }
            } else {
                free(topic_copy);
                free(json_copy);
            }
        }
        cJSON_Delete(error_status);
    }

    ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t bridge_request_availability_check(const char *payload)
{
    ESP_LOGI(TAG, "Device availability check requested");

    /* Parse payload to get device ID (friendly_name or IEEE address) */
    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_AVAIL_CHECK,
                                     "Invalid JSON payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get device identifier */
    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (id_item == NULL || !cJSON_IsString(id_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_AVAIL_CHECK,
                                     "Missing 'id' field in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    const char *device_id = id_item->valuestring;

    /* Find device using NG registry */
    device_t *target = find_device_by_id(device_id);
    if (target == NULL) {
        cJSON_Delete(json);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Device not found: %s", device_id);
        bridge_response_publish_error(RESPONSE_TOPIC_AVAIL_CHECK,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t short_addr = target->proto.zigbee.short_addr;

    /* Trigger availability check */
    esp_err_t ret = zb_availability_check_device(short_addr);

    if (ret == ESP_OK) {
        /* Build success response data */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object for availability check response");
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(data, "id", device_id);
        cJSON_AddStringToObject(data, "status", "check_initiated");

        /* Get current availability state */
        zb_availability_state_t state = zb_availability_get_state(short_addr);
        cJSON_AddStringToObject(data, "current_state",
                               zb_availability_state_to_str(state));

        bridge_response_publish_ok(RESPONSE_TOPIC_AVAIL_CHECK, data, get_current_transaction());
        cJSON_Delete(data);

        ESP_LOGI(TAG, "Availability check initiated for: %s", device_id);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        /* Check already pending */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object for availability check response");
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(data, "id", device_id);
        cJSON_AddStringToObject(data, "status", "check_already_pending");

        bridge_response_publish_ok(RESPONSE_TOPIC_AVAIL_CHECK, data, get_current_transaction());
        cJSON_Delete(data);
    } else {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Failed to initiate availability check: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_AVAIL_CHECK,
                                     error_msg,
                                     get_current_transaction());
    }

    cJSON_Delete(json);
    return ret;
}

/* ============================================================================
 * Device Options NVS Storage
 * ============================================================================ */

esp_err_t device_options_get(uint64_t ieee_addr, device_options_t *options)
{
    if (options == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Initialize with defaults */
    device_options_t defaults = DEVICE_OPTIONS_DEFAULT;
    memcpy(options, &defaults, sizeof(device_options_t));

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(DEVICE_OPTIONS_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[17];
    create_nvs_key(ieee_addr, key);

    size_t required_size = sizeof(device_options_t);
    ret = nvs_get_blob(nvs_handle, key, options, &required_size);

    nvs_close(nvs_handle);
    return ret;
}

esp_err_t device_options_set(uint64_t ieee_addr, const device_options_t *options)
{
    if (options == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(DEVICE_OPTIONS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    char key[17];
    create_nvs_key(ieee_addr, key);

    ret = nvs_set_blob(nvs_handle, key, options, sizeof(device_options_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set NVS blob: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Saved device options for %016" PRIX64, ieee_addr);
    return ret;
}

esp_err_t device_options_remove(uint64_t ieee_addr)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(DEVICE_OPTIONS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[17];
    create_nvs_key(ieee_addr, key);

    ret = nvs_erase_key(nvs_handle, key);
    if (ret == ESP_OK) {
        nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return ret;
}

/* ============================================================================
 * Device Configure Command
 * ============================================================================ */

esp_err_t bridge_request_device_configure(const char *payload)
{
    ESP_LOGI(TAG, "Device configure request");

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_CONFIGURE,
                                     "Invalid JSON payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get device identifier */
    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (id_item == NULL || !cJSON_IsString(id_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_CONFIGURE,
                                     "Missing 'id' field in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    const char *device_id = id_item->valuestring;

    /* Find device using NG registry */
    device_t *target = find_device_by_id(device_id);
    if (target == NULL) {
        cJSON_Delete(json);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Device not found: %s", device_id);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_CONFIGURE,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_NOT_FOUND;
    }

    /* Trigger reporting configuration */
    esp_err_t ret = zb_reporting_set_defaults(target->proto.zigbee.short_addr);

    if (ret == ESP_OK) {
        /* Build success response data */
        char ieee_str[24];
        device_id_to_str(target->id, ieee_str, sizeof(ieee_str));

        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "id", device_id);
        cJSON_AddStringToObject(data, "ieee_address", ieee_str);
        cJSON_AddBoolToObject(data, "configured", true);

        bridge_response_publish_ok(
            RESPONSE_TOPIC_DEVICE_CONFIGURE, data, get_current_transaction());
        cJSON_Delete(data);

        ESP_LOGI(TAG, "Device configured: %s", device_id);
    } else {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Failed to configure device: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_CONFIGURE,
                                     error_msg,
                                     get_current_transaction());
    }

    cJSON_Delete(json);
    return ret;
}

/* ============================================================================
 * Device Options Command
 * ============================================================================ */

esp_err_t bridge_request_device_options(const char *payload)
{
    ESP_LOGI(TAG, "Device options request");

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_OPTIONS,
                                     "Invalid JSON payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get device identifier */
    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (id_item == NULL || !cJSON_IsString(id_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_OPTIONS,
                                     "Missing 'id' field in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    const char *device_id = id_item->valuestring;

    /* Find device using NG registry */
    device_t *target = find_device_by_id(device_id);
    if (target == NULL) {
        cJSON_Delete(json);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Device not found: %s", device_id);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_OPTIONS,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_NOT_FOUND;
    }

    /* device_t.id is already the IEEE address as uint64_t */
    uint64_t ieee64 = target->id;

    /* Get current options */
    device_options_t old_options;
    device_options_get(ieee64, &old_options);  /* Ignores error, uses defaults */

    /* Parse new options */
    cJSON *options_item = cJSON_GetObjectItem(json, "options");
    if (options_item == NULL || !cJSON_IsObject(options_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_OPTIONS,
                                     "Missing 'options' object in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Update options with new values */
    device_options_t new_options = old_options;

    cJSON *retain = cJSON_GetObjectItem(options_item, "retain");
    if (retain != NULL && cJSON_IsBool(retain)) {
        new_options.retain = cJSON_IsTrue(retain);
    }

    cJSON *qos = cJSON_GetObjectItem(options_item, "qos");
    if (qos != NULL && cJSON_IsNumber(qos)) {
        new_options.qos = (uint8_t)(qos->valueint & 0x03);  /* Clamp to 0-2 */
    }

    cJSON *debounce = cJSON_GetObjectItem(options_item, "debounce");
    if (debounce != NULL && cJSON_IsNumber(debounce)) {
        new_options.debounce = (uint16_t)debounce->valueint;
    }

    cJSON *debounce_ignore = cJSON_GetObjectItem(options_item, "debounce_ignore");
    if (debounce_ignore != NULL && cJSON_IsNumber(debounce_ignore)) {
        new_options.debounce_ignore = (uint16_t)debounce_ignore->valueint;
    }

    cJSON *optimistic = cJSON_GetObjectItem(options_item, "optimistic");
    if (optimistic != NULL && cJSON_IsBool(optimistic)) {
        new_options.optimistic = cJSON_IsTrue(optimistic);
    }

    cJSON *force_state = cJSON_GetObjectItem(options_item, "force_enable_state");
    if (force_state != NULL && cJSON_IsBool(force_state)) {
        new_options.force_enable_state = cJSON_IsTrue(force_state);
    }

    /* Handle filtered_attributes array */
    cJSON *filtered = cJSON_GetObjectItem(options_item, "filtered_attributes");
    if (filtered != NULL && cJSON_IsArray(filtered)) {
        new_options.filtered_attributes[0] = '\0';
        size_t offset = 0;
        cJSON *attr;
        cJSON_ArrayForEach(attr, filtered) {
            if (cJSON_IsString(attr)) {
                size_t len = strlen(attr->valuestring);
                if (offset + len + 2 < sizeof(new_options.filtered_attributes)) {
                    if (offset > 0) {
                        new_options.filtered_attributes[offset++] = ',';
                    }
                    /* Use memcpy instead of strcpy - length already validated */
                    memcpy(new_options.filtered_attributes + offset, attr->valuestring, len);
                    offset += len;
                    new_options.filtered_attributes[offset] = '\0';
                }
            }
        }
    }

    /* Save to NVS */
    esp_err_t ret = device_options_set(ieee64, &new_options);

    /* Build response */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for device options response");
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }

    /* Create "from" object with old values */
    cJSON *from = cJSON_CreateObject();
    if (from == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for 'from' in device options response");
        cJSON_Delete(data);
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(from, "retain", old_options.retain);
    cJSON_AddNumberToObject(from, "qos", old_options.qos);
    cJSON_AddNumberToObject(from, "debounce", old_options.debounce);
    cJSON_AddNumberToObject(from, "debounce_ignore", old_options.debounce_ignore);
    cJSON_AddBoolToObject(from, "optimistic", old_options.optimistic);
    cJSON_AddBoolToObject(from, "force_enable_state", old_options.force_enable_state);
    if (old_options.filtered_attributes[0] != '\0') {
        cJSON_AddStringToObject(from, "filtered_attributes", old_options.filtered_attributes);
    }
    cJSON_AddItemToObject(data, "from", from);

    /* Create "to" object with new values */
    cJSON *to = cJSON_CreateObject();
    if (to == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for 'to' in device options response");
        cJSON_Delete(data);
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(to, "retain", new_options.retain);
    cJSON_AddNumberToObject(to, "qos", new_options.qos);
    cJSON_AddNumberToObject(to, "debounce", new_options.debounce);
    cJSON_AddNumberToObject(to, "debounce_ignore", new_options.debounce_ignore);
    cJSON_AddBoolToObject(to, "optimistic", new_options.optimistic);
    cJSON_AddBoolToObject(to, "force_enable_state", new_options.force_enable_state);
    if (new_options.filtered_attributes[0] != '\0') {
        cJSON_AddStringToObject(to, "filtered_attributes", new_options.filtered_attributes);
    }
    cJSON_AddItemToObject(data, "to", to);

    if (ret == ESP_OK) {
        bridge_response_publish_ok(RESPONSE_TOPIC_DEVICE_OPTIONS, data, get_current_transaction());
        ESP_LOGI(TAG, "Device options updated: %s", device_id);
    } else {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Failed to save device options: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_OPTIONS,
                                     error_msg,
                                     get_current_transaction());
    }

    cJSON_Delete(data);
    cJSON_Delete(json);
    return ret;
}

/* ============================================================================
 * Device Get Command (Read Attributes)
 * ============================================================================ */

esp_err_t bridge_request_device_get(const char *payload)
{
    ESP_LOGI(TAG, "Device get request");

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_GET,
                                     "Invalid JSON payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get device identifier */
    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (id_item == NULL || !cJSON_IsString(id_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_GET,
                                     "Missing 'id' field in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    const char *device_id = id_item->valuestring;

    /* Find device using NG registry */
    device_t *target = find_device_by_id(device_id);
    if (target == NULL) {
        cJSON_Delete(json);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Device not found: %s", device_id);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_GET,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_NOT_FOUND;
    }

    /* Get cluster */
    cJSON *cluster_item = cJSON_GetObjectItem(json, "cluster");
    if (cluster_item == NULL || !cJSON_IsString(cluster_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_GET,
                                     "Missing 'cluster' field in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t cluster_id = get_cluster_id_by_name(cluster_item->valuestring);
    if (cluster_id == 0xFFFF) {
        cJSON_Delete(json);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Unknown cluster: %s", cluster_item->valuestring);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_GET,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get attributes array */
    cJSON *attrs_item = cJSON_GetObjectItem(json, "attributes");
    if (attrs_item == NULL || !cJSON_IsArray(attrs_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_GET,
                                     "Missing 'attributes' array in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    int attr_count = cJSON_GetArraySize(attrs_item);
    if (attr_count == 0 || attr_count > 16) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_GET,
                                     "Invalid attributes count (1-16 allowed)",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse attribute IDs */
    uint16_t attr_ids[16];
    int valid_count = 0;

    cJSON *attr_item;
    cJSON_ArrayForEach(attr_item, attrs_item) {
        if (!cJSON_IsString(attr_item)) {
            continue;
        }

        uint16_t attr_id = get_attr_id_by_name(cluster_id, attr_item->valuestring);
        if (attr_id != 0xFFFF) {
            attr_ids[valid_count++] = attr_id;
        } else {
            ESP_LOGW(TAG, "Unknown attribute: %s for cluster 0x%04X",
                     attr_item->valuestring, cluster_id);
        }
    }

    if (valid_count == 0) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_GET,
                                     "No valid attributes found",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get optional endpoint - default to device's primary endpoint */
    uint8_t endpoint = target->proto.zigbee.endpoint;
    cJSON *ep_item = cJSON_GetObjectItem(json, "endpoint");
    if (ep_item != NULL && cJSON_IsNumber(ep_item)) {
        endpoint = (uint8_t)ep_item->valueint;
    }

    /* Send ZCL Read Attributes request */
    esp_zb_zcl_read_attr_cmd_t read_cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = target->proto.zigbee.short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = 1
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
        .attr_number = (uint16_t)valid_count,
        .attr_field = attr_ids
    };

    /* Thread-safety: Acquire Zigbee lock before API call */
    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_read_attr_cmd_req(&read_cmd);
    esp_zb_lock_release();

    /* Note: Response will come asynchronously via ZCL callback */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for device get response");
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(data, "id", device_id);
    cJSON_AddStringToObject(data, "cluster", cluster_item->valuestring);
    cJSON_AddStringToObject(data, "status", "request_sent");
    cJSON_AddNumberToObject(data, "attribute_count", valid_count);

    bridge_response_publish_ok(RESPONSE_TOPIC_DEVICE_GET, data, get_current_transaction());
    cJSON_Delete(data);

    ESP_LOGI(TAG, "Read attribute request sent: %s, cluster 0x%04X, %d attrs, TSN=0x%02X",
             device_id, cluster_id, valid_count, tsn);

    cJSON_Delete(json);
    return ESP_OK;
}

/* ============================================================================
 * Install Code Management Commands
 * ============================================================================ */

esp_err_t bridge_request_install_code_add(const char *payload)
{
    ESP_LOGI(TAG, "Install code add request");

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_ADD,
                                     "Invalid JSON payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get IEEE address */
    cJSON *ieee_item = cJSON_GetObjectItem(json, "ieee_address");
    if (ieee_item == NULL || !cJSON_IsString(ieee_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_ADD,
                                     "Missing 'ieee_address' field in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get install code */
    cJSON *code_item = cJSON_GetObjectItem(json, "install_code");
    if (code_item == NULL || !cJSON_IsString(code_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_ADD,
                                     "Missing 'install_code' field in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse IEEE address */
    uint64_t ieee_addr;
    esp_err_t ret = zb_install_codes_parse_ieee(ieee_item->valuestring, &ieee_addr);
    if (ret != ESP_OK) {
        cJSON_Delete(json);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Invalid IEEE address format: %s", ieee_item->valuestring);
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_ADD,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse install code */
    uint8_t install_code[ZB_INSTALL_CODE_MAX_LEN];
    size_t code_len;
    ret = zb_install_codes_parse_code(code_item->valuestring, install_code,
                                       sizeof(install_code), &code_len);
    if (ret != ESP_OK) {
        cJSON_Delete(json);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Invalid install code format: %s", code_item->valuestring);
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_ADD,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Add install code */
    ret = zb_install_codes_add(ieee_addr, install_code, code_len);

    if (ret == ESP_OK) {
        /* Get derived key for response */
        uint8_t derived_key[ZB_INSTALL_CODE_KEY_LEN];
        zb_install_codes_get_key(ieee_addr, derived_key);

        /* Format derived key as hex for response */
        char key_hex[ZB_INSTALL_CODE_KEY_LEN * 2 + 1];
        for (int i = 0; i < ZB_INSTALL_CODE_KEY_LEN; i++) {
            snprintf(&key_hex[i * 2], 3, "%02X", derived_key[i]);
        }
        key_hex[ZB_INSTALL_CODE_KEY_LEN * 2] = '\0';  /* Ensure null termination */

        /* Build success response data */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object for install code add response");
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(data, "ieee_address", ieee_item->valuestring);
        cJSON_AddStringToObject(data, "install_code", code_item->valuestring);
        cJSON_AddStringToObject(data, "derived_key", key_hex);
        cJSON_AddNumberToObject(data, "total_entries", (double)zb_install_codes_count());

        bridge_response_publish_ok(
            RESPONSE_TOPIC_INSTALL_CODE_ADD, data, get_current_transaction());
        cJSON_Delete(data);

        ESP_LOGI(TAG, "Install code added for 0x%016" PRIX64, ieee_addr);
    } else if (ret == ESP_ERR_INVALID_CRC) {
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_ADD,
                                     "Install code CRC validation failed",
                                     get_current_transaction());
    } else if (ret == ESP_ERR_NO_MEM) {
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_ADD,
                                     "Install code storage full (max 50)",
                                     get_current_transaction());
    } else {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Failed to add install code: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_ADD,
                                     error_msg,
                                     get_current_transaction());
    }

    cJSON_Delete(json);
    return ret;
}

esp_err_t bridge_request_install_code_remove(const char *payload)
{
    ESP_LOGI(TAG, "Install code remove request");

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_REMOVE,
                                     "Invalid JSON payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get IEEE address */
    cJSON *ieee_item = cJSON_GetObjectItem(json, "ieee_address");
    if (ieee_item == NULL || !cJSON_IsString(ieee_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_REMOVE,
                                     "Missing 'ieee_address' field in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse IEEE address */
    uint64_t ieee_addr;
    esp_err_t ret = zb_install_codes_parse_ieee(ieee_item->valuestring, &ieee_addr);
    if (ret != ESP_OK) {
        cJSON_Delete(json);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Invalid IEEE address format: %s", ieee_item->valuestring);
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_REMOVE,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Remove install code */
    ret = zb_install_codes_remove(ieee_addr);

    if (ret == ESP_OK) {
        /* Build success response data */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object for install code remove response");
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(data, "ieee_address", ieee_item->valuestring);
        cJSON_AddBoolToObject(data, "removed", true);
        cJSON_AddNumberToObject(data, "total_entries", (double)zb_install_codes_count());

        bridge_response_publish_ok(
            RESPONSE_TOPIC_INSTALL_CODE_REMOVE, data, get_current_transaction());
        cJSON_Delete(data);

        ESP_LOGI(TAG, "Install code removed for 0x%016" PRIX64, ieee_addr);
    } else if (ret == ESP_ERR_NOT_FOUND) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "No install code found for: %s", ieee_item->valuestring);
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_REMOVE,
                                     error_msg,
                                     get_current_transaction());
    } else {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Failed to remove install code: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_INSTALL_CODE_REMOVE,
                                     error_msg,
                                     get_current_transaction());
    }

    cJSON_Delete(json);
    return ret;
}

esp_err_t bridge_request_install_code_list(const char *payload)
{
    ESP_LOGI(TAG, "Install code list request");
    (void)payload;  /* Payload is ignored for list request */

    /* Get all install codes */
    zb_install_code_entry_t entries[ZB_INSTALL_CODE_MAX_ENTRIES];
    size_t count = zb_install_codes_list(entries, ZB_INSTALL_CODE_MAX_ENTRIES);

    /* Build response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for install code list response");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(data, "count", (double)count);

    cJSON *codes_array = cJSON_CreateArray();
    if (codes_array == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON array for install codes");
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;  /* Skip this entry on allocation failure */
        }

        /* Format IEEE address */
        char ieee_str[24];
        snprintf(ieee_str, sizeof(ieee_str), "0x%016" PRIX64, entries[i].ieee_addr);
        cJSON_AddStringToObject(entry, "ieee_address", ieee_str);

        /* Format install code */
        char code_hex[ZB_INSTALL_CODE_MAX_LEN * 2 + 1];
        zb_install_codes_format_code(entries[i].install_code,
                                      entries[i].install_code_len,
                                      code_hex, sizeof(code_hex));
        cJSON_AddStringToObject(entry, "install_code", code_hex);

        /* Format derived key (only first/last 4 bytes for security) */
        char key_preview[24];
        snprintf(key_preview, sizeof(key_preview),
                 "%02X%02X%02X%02X...%02X%02X%02X%02X",
                 entries[i].derived_key[0], entries[i].derived_key[1],
                 entries[i].derived_key[2], entries[i].derived_key[3],
                 entries[i].derived_key[12], entries[i].derived_key[13],
                 entries[i].derived_key[14], entries[i].derived_key[15]);
        cJSON_AddStringToObject(entry, "derived_key_preview", key_preview);

        cJSON_AddBoolToObject(entry, "active", entries[i].is_active);
        cJSON_AddNumberToObject(entry, "added_timestamp", (double)entries[i].added_timestamp);

        cJSON_AddItemToArray(codes_array, entry);
    }

    cJSON_AddItemToObject(data, "install_codes", codes_array);

    bridge_response_publish_ok(RESPONSE_TOPIC_INSTALL_CODE_LIST, data, get_current_transaction());
    cJSON_Delete(data);

    ESP_LOGI(TAG, "Listed %d install codes", count);
    return ESP_OK;
}

/* ============================================================================
 * Device Remove with Force Option
 * ============================================================================ */

esp_err_t bridge_request_device_remove_force(const char *friendly_name, bool force)
{
    if (friendly_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Device remove request: %s (force=%d)", friendly_name, force);

    /* Find device using NG registry */
    device_t *target = find_device_by_id(friendly_name);
    if (target == NULL) {
        ESP_LOGW(TAG, "Device not found: %s", friendly_name);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Device not found: %s", friendly_name);
        bridge_response_publish_error(RESPONSE_TOPIC_DEVICE_REMOVE,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_NOT_FOUND;
    }

    /* Store device info before removal for response */
    char ieee_str[24];
    device_id_to_str(target->id, ieee_str, sizeof(ieee_str));
    uint64_t ieee64 = target->id;
    uint16_t short_addr = target->proto.zigbee.short_addr;

    if (!force) {
        /* Normal remove: send ZDO mgmt_leave_req first (fire-and-forget) */
        ESP_LOGI(TAG, "Sending mgmt_leave_req to 0x%04X", short_addr);
        esp_zb_zdo_mgmt_leave_req_param_t leave_req = {
            .dst_nwk_addr = short_addr,
            .remove_children = false,
            .rejoin = false
        };
        memcpy(leave_req.device_address, &ieee64, sizeof(leave_req.device_address));
        esp_zb_lock_acquire(portMAX_DELAY);
        esp_zb_zdo_device_leave_req(&leave_req, NULL, NULL);
        esp_zb_lock_release();
    } else {
        ESP_LOGW(TAG, "Force removing device %s without Leave request", friendly_name);
    }

    /* Full cleanup: event, MQTT clear, converter, availability, registry, NVS */
    zb_device_leave_cleanup(ieee64, short_addr);

    /* Build success response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for device remove response");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(data, "id", friendly_name);
    cJSON_AddStringToObject(data, "ieee_address", ieee_str);
    cJSON_AddBoolToObject(data, "force", force);

    bridge_response_publish_ok(RESPONSE_TOPIC_DEVICE_REMOVE, data, get_current_transaction());
    cJSON_Delete(data);

    ESP_LOGI(TAG, "Device removed: %s (force=%d)", friendly_name, force);
    return ESP_OK;
}

/* Maintain backwards compatibility with existing function */
esp_err_t bridge_request_device_remove(const char *friendly_name)
{
    return bridge_request_device_remove_force(friendly_name, false);
}

/* ============================================================================
 * Coordinator Settings and Info Commands (ZG-012)
 * ============================================================================ */

esp_err_t bridge_request_coordinator_check(void)
{
    ESP_LOGI(TAG, "Coordinator health check requested");

    zb_coordinator_health_t health;
    esp_err_t ret = zb_coordinator_health_check(&health);

    /* Build response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for coordinator check response");
        bridge_response_publish_error(RESPONSE_TOPIC_COORDINATOR_CHECK,
                                     "Failed to create JSON response",
                                     get_current_transaction());
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(data, "health", health.healthy ? "healthy" : "unhealthy");
    cJSON_AddBoolToObject(data, "network_up", health.network_up);
    cJSON_AddBoolToObject(data, "zigbee_stack_running", health.zigbee_stack_running);
    cJSON_AddNumberToObject(data, "uptime_seconds", health.uptime_seconds);
    cJSON_AddNumberToObject(data, "joined_devices", health.joined_devices);
    cJSON_AddNumberToObject(data, "message_count_tx", health.message_count_tx);
    cJSON_AddNumberToObject(data, "message_count_rx", health.message_count_rx);
    cJSON_AddNumberToObject(data, "last_rssi", health.last_rssi);
    cJSON_AddNumberToObject(data, "last_lqi", health.last_lqi);

    /* Always publish OK even if unhealthy - the data shows the status */
    bridge_response_publish_ok(RESPONSE_TOPIC_COORDINATOR_CHECK, data, get_current_transaction());
    cJSON_Delete(data);

    return ret;
}

esp_err_t bridge_request_coordinator_version(void)
{
    ESP_LOGI(TAG, "Coordinator version requested");

    zb_coordinator_version_t version;
    esp_err_t ret = zb_coordinator_get_version(&version);

    if (ret != ESP_OK) {
        bridge_response_publish_error(RESPONSE_TOPIC_COORDINATOR_VERSION,
                                     "Failed to get coordinator version",
                                     get_current_transaction());
        return ret;
    }

    /* Build response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for coordinator version response");
        bridge_response_publish_error(RESPONSE_TOPIC_COORDINATOR_VERSION,
                                     "Failed to create JSON response",
                                     get_current_transaction());
        return ESP_ERR_NO_MEM;
    }

    /* Coordinator info */
    cJSON *coordinator = cJSON_CreateObject();
    if (coordinator == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for coordinator info");
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(coordinator, "type", version.type);

    cJSON *meta = cJSON_CreateObject();
    if (meta == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for meta info");
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(meta, "version", version.firmware_version);
    cJSON_AddStringToObject(meta, "revision", version.firmware_build);
    cJSON_AddStringToObject(meta, "zigbee_version", version.zigbee_version);
    cJSON_AddItemToObject(coordinator, "meta", meta);

    cJSON_AddStringToObject(coordinator, "ieee_address", version.ieee_address);
    cJSON_AddNumberToObject(coordinator, "short_address", version.short_address);

    cJSON_AddItemToObject(data, "coordinator", coordinator);

    bridge_response_publish_ok(RESPONSE_TOPIC_COORDINATOR_VERSION, data, get_current_transaction());
    cJSON_Delete(data);

    return ESP_OK;
}

esp_err_t bridge_request_coordinator_settings(const char *payload)
{
    ESP_LOGI(TAG, "Coordinator settings requested");
    (void)payload;  /* Payload currently unused for settings GET */

    zb_coordinator_settings_t settings;
    esp_err_t ret = zb_coordinator_get_settings(&settings);

    if (ret != ESP_OK) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Failed to get coordinator settings: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_COORDINATOR_SETTINGS,
                                     error_msg,
                                     get_current_transaction());
        return ret;
    }

    /* Build response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for coordinator settings response");
        bridge_response_publish_error(RESPONSE_TOPIC_COORDINATOR_SETTINGS,
                                     "Failed to create JSON response",
                                     get_current_transaction());
        return ESP_ERR_NO_MEM;
    }

    /* Network info */
    cJSON *network = cJSON_CreateObject();
    if (network == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for network info");
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(network, "pan_id", settings.pan_id);
    cJSON_AddNumberToObject(network, "channel", settings.channel);

    /* Format extended PAN ID */
    char ext_pan_id_str[24];
    snprintf(ext_pan_id_str, sizeof(ext_pan_id_str), "0x%02X%02X%02X%02X%02X%02X%02X%02X",
             settings.extended_pan_id[7], settings.extended_pan_id[6],
             settings.extended_pan_id[5], settings.extended_pan_id[4],
             settings.extended_pan_id[3], settings.extended_pan_id[2],
             settings.extended_pan_id[1], settings.extended_pan_id[0]);
    cJSON_AddStringToObject(network, "extended_pan_id", ext_pan_id_str);

    cJSON_AddBoolToObject(network, "network_formed", settings.network_formed);
    cJSON_AddItemToObject(data, "network", network);

    /* Settings */
    cJSON *config = cJSON_CreateObject();
    if (config == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for config info");
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(config, "transmit_power", settings.transmit_power);
    cJSON_AddBoolToObject(config, "permit_join_default", settings.permit_join_default);
    cJSON_AddNumberToObject(config, "max_children", settings.max_children);
    cJSON_AddItemToObject(data, "config", config);

    bridge_response_publish_ok(
        RESPONSE_TOPIC_COORDINATOR_SETTINGS, data, get_current_transaction());
    cJSON_Delete(data);

    return ESP_OK;
}

/* ============================================================================
 * Network Options / Channel Change (ZG-016)
 * ============================================================================ */

/**
 * @brief Channel change completion callback
 */
static void channel_change_callback(esp_err_t result, uint8_t old_channel,
                                    uint8_t new_channel, void *user_data)
{
    (void)user_data;

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Channel change completed: %d -> %d", old_channel, new_channel);

        /* Publish success event to bridge/logging */
        cJSON *event = cJSON_CreateObject();
        if (event != NULL) {
            cJSON_AddStringToObject(event, "type", "channel_changed");
            cJSON_AddNumberToObject(event, "old_channel", old_channel);
            cJSON_AddNumberToObject(event, "new_channel", new_channel);
            bool from_pool;
            char *json_str = pool_json_print_unformatted(event, &from_pool);
            if (json_str != NULL) {
                /* Publish via event bus with ownership transfer.
                 * topic (string literal) must be strdup'd.
                 * json_str from pool must be strdup'd, then pool original freed. */
                char *topic_copy = strdup("zigbee2mqtt/bridge/logging");
                char *json_copy = strdup(json_str);
                pool_json_free(json_str, from_pool);
                if (topic_copy && json_copy) {
                    evt_ha_discovery_publish_t log_evt = {
                        .topic = topic_copy,
                        .payload_json = json_copy,
                        .qos = 0,
                        .retain = false
                    };
                    if (event_publish(EVT_HA_DISCOVERY_PUBLISH, &log_evt, sizeof(log_evt)) != ESP_OK) {
                        free(topic_copy);
                        free(json_copy);
                    }
                } else {
                    free(topic_copy);
                    free(json_copy);
                }
            }
            cJSON_Delete(event);
        }
    } else {
        ESP_LOGE(TAG, "Channel change failed: %s", esp_err_to_name(result));
    }
}

esp_err_t bridge_request_network_options(const char *payload)
{
    ESP_LOGI(TAG, "Network options request");

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_OPTIONS,
                                     "Invalid JSON payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get current network info */
    zb_network_info_t net_info;
    esp_err_t ret = zb_network_get_info(&net_info);
    if (ret != ESP_OK) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_OPTIONS,
                                     "Failed to get network info",
                                     get_current_transaction());
        return ret;
    }

    /* Check for channel change request */
    cJSON *channel_item = cJSON_GetObjectItem(json, "channel");
    if (channel_item != NULL && cJSON_IsNumber(channel_item)) {
        uint8_t new_channel = (uint8_t)channel_item->valueint;
        uint8_t old_channel = net_info.channel;

        ESP_LOGI(TAG, "Channel change requested: %d -> %d", old_channel, new_channel);

        /* Validate channel */
        if (!zb_network_validate_channel(new_channel)) {
            cJSON_Delete(json);

            char error_msg[128];
            snprintf(error_msg, sizeof(error_msg),
                     "Invalid channel: %d (must be 11-26)", new_channel);
            bridge_response_publish_error(RESPONSE_TOPIC_OPTIONS,
                                         error_msg,
                                         get_current_transaction());
            return ESP_ERR_INVALID_ARG;
        }

        /* Check if already on this channel */
        if (old_channel == new_channel) {
            cJSON *data = cJSON_CreateObject();
            if (data == NULL) {
                cJSON_Delete(json);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(data, "status", "already_on_channel");
            cJSON_AddNumberToObject(data, "channel", new_channel);

            bridge_response_publish_ok(RESPONSE_TOPIC_OPTIONS, data, get_current_transaction());
            cJSON_Delete(data);
            cJSON_Delete(json);
            return ESP_OK;
        }

        /* Check if change already in progress */
        if (zb_network_is_channel_change_pending()) {
            cJSON_Delete(json);
            bridge_response_publish_error(RESPONSE_TOPIC_OPTIONS,
                                         "Channel change already in progress",
                                         get_current_transaction());
            return ESP_ERR_INVALID_STATE;
        }

        /* Initiate channel change */
        ret = zb_network_change_channel(new_channel, channel_change_callback, NULL);

        if (ret == ESP_OK) {
            /* Build response data */
            cJSON *data = cJSON_CreateObject();
            if (data == NULL) {
                ESP_LOGE(TAG, "Failed to create JSON object for channel change response");
                cJSON_Delete(json);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(data, "status", "channel_change_initiated");
            cJSON_AddNumberToObject(data, "old_channel", old_channel);
            cJSON_AddNumberToObject(data, "new_channel", new_channel);
            cJSON_AddStringToObject(data, "warning",
                "Channel change may cause temporary network disruption. "
                "Devices will need to follow the coordinator to the new channel.");

            bridge_response_publish_ok(RESPONSE_TOPIC_OPTIONS, data, get_current_transaction());
            cJSON_Delete(data);

            ESP_LOGI(TAG, "Channel change initiated: %d -> %d", old_channel, new_channel);
        } else {
            char error_msg[128];
            snprintf(error_msg, sizeof(error_msg),
                     "Failed to initiate channel change: %s",
                     esp_err_to_name(ret));
            bridge_response_publish_error(RESPONSE_TOPIC_OPTIONS,
                                         error_msg,
                                         get_current_transaction());
        }

        cJSON_Delete(json);
        return ret;
    }

    /* No channel change - return current network options */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for network options response");
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(data, "channel", net_info.channel);
    cJSON_AddNumberToObject(data, "pan_id", net_info.pan_id);
    cJSON_AddBoolToObject(data, "permit_join", net_info.permit_join);
    cJSON_AddNumberToObject(data, "device_count", net_info.device_count);

    /* Add channel change state if relevant */
    zb_channel_change_state_t ch_state = zb_network_get_channel_change_state();
    if (ch_state != ZB_CHANNEL_CHANGE_IDLE) {
        const char *state_str = "unknown";
        switch (ch_state) {
            case ZB_CHANNEL_CHANGE_PENDING:   state_str = "pending"; break;
            case ZB_CHANNEL_CHANGE_NOTIFYING: state_str = "notifying_devices"; break;
            case ZB_CHANNEL_CHANGE_SWITCHING: state_str = "switching"; break;
            case ZB_CHANNEL_CHANGE_COMPLETE:  state_str = "complete"; break;
            case ZB_CHANNEL_CHANGE_FAILED:    state_str = "failed"; break;
            default: break;
        }
        cJSON_AddStringToObject(data, "channel_change_state", state_str);
    }

    bridge_response_publish_ok(RESPONSE_TOPIC_OPTIONS, data, get_current_transaction());
    cJSON_Delete(data);
    cJSON_Delete(json);

    return ESP_OK;
}

/* ============================================================================
 * Time Server Commands
 * ============================================================================ */

esp_err_t bridge_request_time_get(void)
{
    ESP_LOGI(TAG, "Time get requested");

    /* Check if time server is initialized */
    if (!zb_time_server_is_initialized()) {
        bridge_response_publish_error(RESPONSE_TOPIC_TIME_GET,
                                     "Time server not initialized",
                                     get_current_transaction());
        return ESP_ERR_INVALID_STATE;
    }

    /* Get current time */
    time_t unix_time = zb_time_server_get_time_unix();
    uint32_t zigbee_time = zb_time_server_get_time();
    bool synchronized = zb_time_server_is_synchronized();

    /* Build response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for time get response");
        bridge_response_publish_error(RESPONSE_TOPIC_TIME_GET,
                                     "Failed to create JSON response",
                                     get_current_transaction());
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(data, "time", (double)unix_time);
    cJSON_AddNumberToObject(data, "zigbee_time", zigbee_time);
    cJSON_AddBoolToObject(data, "synchronized", synchronized);

    /* Add ISO 8601 formatted time string */
    if (unix_time > 0) {
        struct tm timeinfo;
        gmtime_r(&unix_time, &timeinfo);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
        cJSON_AddStringToObject(data, "utc", time_str);
    }

    /* Add timezone info */
    int32_t tz_offset = zb_time_server_get_timezone();
    cJSON_AddNumberToObject(data, "timezone_offset", tz_offset);
    cJSON_AddBoolToObject(data, "dst_active", zb_time_server_is_dst_active());

    bridge_response_publish_ok(RESPONSE_TOPIC_TIME_GET, data, get_current_transaction());
    cJSON_Delete(data);

    return ESP_OK;
}

esp_err_t bridge_request_time_set(const char *payload)
{
    ESP_LOGI(TAG, "Time set requested");

    /* Check if time server is initialized */
    if (!zb_time_server_is_initialized()) {
        bridge_response_publish_error(RESPONSE_TOPIC_TIME_SET,
                                     "Time server not initialized",
                                     get_current_transaction());
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_TIME_SET,
                                     "Invalid JSON payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get time value from payload */
    cJSON *time_item = cJSON_GetObjectItem(json, "time");
    if (time_item == NULL || !cJSON_IsNumber(time_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_TIME_SET,
                                     "Missing 'time' field (unix timestamp) in payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    time_t unix_time = (time_t)time_item->valuedouble;

    /* Validate time value */
    if (unix_time < ZB_TIME_EPOCH_OFFSET_SECONDS) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_TIME_SET,
                                     "Invalid time: must be after 2000-01-01 00:00:00 UTC",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Set the time */
    esp_err_t ret = zb_time_server_set_time_unix(unix_time);

    if (ret == ESP_OK) {
        /* Build success response data */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object for time set response");
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(data, "time", (double)unix_time);
        cJSON_AddBoolToObject(data, "set", true);

        /* Add ISO 8601 formatted time string */
        struct tm timeinfo;
        gmtime_r(&unix_time, &timeinfo);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
        cJSON_AddStringToObject(data, "utc", time_str);

        bridge_response_publish_ok(RESPONSE_TOPIC_TIME_SET, data, get_current_transaction());
        cJSON_Delete(data);

        ESP_LOGI(TAG, "Time set to: %s", time_str);
    } else {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Failed to set time: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_TIME_SET,
                                     error_msg,
                                     get_current_transaction());
    }

    cJSON_Delete(json);
    return ret;
}

esp_err_t bridge_request_time_config(const char *payload)
{
    ESP_LOGI(TAG, "Time config requested");

    /* Check if time server is initialized */
    if (!zb_time_server_is_initialized()) {
        bridge_response_publish_error(RESPONSE_TOPIC_TIME_CONFIG,
                                     "Time server not initialized",
                                     get_current_transaction());
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *json = cJSON_Parse(payload);

    /* If no payload or empty payload, return current config (GET) */
    if (json == NULL || cJSON_GetArraySize(json) == 0) {
        if (json != NULL) {
            cJSON_Delete(json);
        }

        /* Get current config */
        zb_time_server_config_t config;
        esp_err_t ret = zb_time_server_get_config(&config);

        if (ret != ESP_OK) {
            bridge_response_publish_error(RESPONSE_TOPIC_TIME_CONFIG,
                                         "Failed to get time config",
                                         get_current_transaction());
            return ret;
        }

        /* Build response data */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object for time config response");
            bridge_response_publish_error(RESPONSE_TOPIC_TIME_CONFIG,
                                         "Failed to create JSON response",
                                         get_current_transaction());
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(data, "timezone_offset", config.timezone_offset);
        cJSON_AddNumberToObject(data, "dst_shift", config.dst_shift);
        cJSON_AddNumberToObject(data, "dst_start", config.dst_start);
        cJSON_AddNumberToObject(data, "dst_end", config.dst_end);
        cJSON_AddBoolToObject(data, "is_master", config.is_master);
        cJSON_AddBoolToObject(data, "is_synchronized", config.is_synchronized);

        bridge_response_publish_ok(RESPONSE_TOPIC_TIME_CONFIG, data, get_current_transaction());
        cJSON_Delete(data);

        return ESP_OK;
    }

    /* Set config (SET) */
    zb_time_server_config_t config;
    esp_err_t ret = zb_time_server_get_config(&config);
    if (ret != ESP_OK) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_TIME_CONFIG,
                                     "Failed to get current config",
                                     get_current_transaction());
        return ret;
    }

    /* Update config with new values */
    cJSON *tz_item = cJSON_GetObjectItem(json, "timezone_offset");
    if (tz_item != NULL && cJSON_IsNumber(tz_item)) {
        config.timezone_offset = (int32_t)tz_item->valueint;
    }

    cJSON *dst_shift_item = cJSON_GetObjectItem(json, "dst_shift");
    if (dst_shift_item != NULL && cJSON_IsNumber(dst_shift_item)) {
        config.dst_shift = (int32_t)dst_shift_item->valueint;
    }

    cJSON *dst_start_item = cJSON_GetObjectItem(json, "dst_start");
    if (dst_start_item != NULL && cJSON_IsNumber(dst_start_item)) {
        config.dst_start = (uint32_t)dst_start_item->valuedouble;
    }

    cJSON *dst_end_item = cJSON_GetObjectItem(json, "dst_end");
    if (dst_end_item != NULL && cJSON_IsNumber(dst_end_item)) {
        config.dst_end = (uint32_t)dst_end_item->valuedouble;
    }

    cJSON *master_item = cJSON_GetObjectItem(json, "is_master");
    if (master_item != NULL && cJSON_IsBool(master_item)) {
        config.is_master = cJSON_IsTrue(master_item);
    }

    cJSON *sync_item = cJSON_GetObjectItem(json, "is_synchronized");
    if (sync_item != NULL && cJSON_IsBool(sync_item)) {
        config.is_synchronized = cJSON_IsTrue(sync_item);
    }

    /* Apply new config */
    ret = zb_time_server_set_config(&config);

    if (ret == ESP_OK) {
        /* Build success response data */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object for time config set response");
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(data, "timezone_offset", config.timezone_offset);
        cJSON_AddNumberToObject(data, "dst_shift", config.dst_shift);
        cJSON_AddNumberToObject(data, "dst_start", config.dst_start);
        cJSON_AddNumberToObject(data, "dst_end", config.dst_end);
        cJSON_AddBoolToObject(data, "is_master", config.is_master);
        cJSON_AddBoolToObject(data, "is_synchronized", config.is_synchronized);
        cJSON_AddBoolToObject(data, "updated", true);

        bridge_response_publish_ok(RESPONSE_TOPIC_TIME_CONFIG, data, get_current_transaction());
        cJSON_Delete(data);

        ESP_LOGI(TAG, "Time config updated: tz_offset=%d, dst_shift=%d",
                 config.timezone_offset, config.dst_shift);
    } else {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Failed to set time config: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_TIME_CONFIG,
                                     error_msg,
                                     get_current_transaction());
    }

    cJSON_Delete(json);
    return ret;
}

/* ============================================================================
 * Extended PAN ID Management (API-004)
 * ============================================================================ */

esp_err_t bridge_request_network_extended_pan_id(const char *payload)
{
    ESP_LOGI(TAG, "Network extended PAN ID request");

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        /* Empty payload - return current Extended PAN ID */
        json = cJSON_CreateObject();
        if (json == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Get current Extended PAN ID */
    uint8_t current_ext_pan_id[ZB_EXTENDED_PAN_ID_LEN];
    esp_err_t ret = zb_network_get_extended_pan_id(current_ext_pan_id);
    if (ret != ESP_OK) {
        cJSON_Delete(json);
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Failed to get Extended PAN ID: %s", esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_NETWORK_EXT_PAN_ID,
                                     error_msg,
                                     get_current_transaction());
        return ret;
    }

    /* Format current Extended PAN ID */
    char current_ext_pan_id_str[24];
    zb_network_format_extended_pan_id(current_ext_pan_id,
                                       current_ext_pan_id_str,
                                       sizeof(current_ext_pan_id_str));

    /* Check if a new Extended PAN ID is being set */
    cJSON *value_item = cJSON_GetObjectItem(json, "value");
    if (value_item != NULL && cJSON_IsString(value_item)) {
        const char *new_ext_pan_id_str = value_item->valuestring;

        /* Parse new Extended PAN ID */
        uint8_t new_ext_pan_id[ZB_EXTENDED_PAN_ID_LEN];
        ret = zb_network_parse_extended_pan_id(new_ext_pan_id_str, new_ext_pan_id);
        if (ret != ESP_OK) {
            cJSON_Delete(json);
            char error_msg[128];
            snprintf(error_msg, sizeof(error_msg),
                     "Invalid Extended PAN ID format: %s (expected 16 hex chars)",
                     new_ext_pan_id_str);
            bridge_response_publish_error(RESPONSE_TOPIC_NETWORK_EXT_PAN_ID,
                                         error_msg,
                                         get_current_transaction());
            return ESP_ERR_INVALID_ARG;
        }

        /* Get network info to check if network is formed */
        zb_network_info_t net_info;
        ret = zb_network_get_info(&net_info);
        if (ret == ESP_OK && net_info.network_formed) {
            ESP_LOGW(TAG, "WARNING: Changing Extended PAN ID on an active network "
                         "will cause all devices to lose connectivity!");
        }

        ESP_LOGI(TAG, "Setting Extended PAN ID: %s -> %s",
                 current_ext_pan_id_str, new_ext_pan_id_str);

        /* Set new Extended PAN ID */
        ret = zb_network_set_extended_pan_id(new_ext_pan_id);
        if (ret != ESP_OK) {
            cJSON_Delete(json);
            char error_msg[128];
            snprintf(error_msg, sizeof(error_msg),
                     "Failed to set Extended PAN ID: %s", esp_err_to_name(ret));
            bridge_response_publish_error(RESPONSE_TOPIC_NETWORK_EXT_PAN_ID,
                                         error_msg,
                                         get_current_transaction());
            return ret;
        }

        /* Build success response */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(data, "old_extended_pan_id", current_ext_pan_id_str);
        cJSON_AddStringToObject(data, "extended_pan_id", new_ext_pan_id_str);
        cJSON_AddStringToObject(data, "status", "set");

        if (net_info.network_formed) {
            cJSON_AddStringToObject(data, "warning",
                "Extended PAN ID changed on active network. Devices may need to rejoin.");
        }

        bridge_response_publish_ok(
            RESPONSE_TOPIC_NETWORK_EXT_PAN_ID, data, get_current_transaction());
        cJSON_Delete(data);
        cJSON_Delete(json);

        ESP_LOGI(TAG, "Extended PAN ID set successfully");
        return ESP_OK;
    }

    /* Check for "use_mac" flag to set from MAC address */
    cJSON *use_mac_item = cJSON_GetObjectItem(json, "use_mac");
    if (use_mac_item != NULL && cJSON_IsBool(use_mac_item) && cJSON_IsTrue(use_mac_item)) {
        ESP_LOGI(TAG, "Setting Extended PAN ID from IEEE MAC address");

        ret = zb_network_set_extended_pan_id_from_mac();
        if (ret != ESP_OK) {
            cJSON_Delete(json);
            char error_msg[128];
            snprintf(error_msg, sizeof(error_msg),
                     "Failed to set Extended PAN ID from MAC: %s", esp_err_to_name(ret));
            bridge_response_publish_error(RESPONSE_TOPIC_NETWORK_EXT_PAN_ID,
                                         error_msg,
                                         get_current_transaction());
            return ret;
        }

        /* Get the new Extended PAN ID */
        uint8_t new_ext_pan_id[ZB_EXTENDED_PAN_ID_LEN];
        zb_network_get_extended_pan_id(new_ext_pan_id);
        char new_ext_pan_id_str[24];
        zb_network_format_extended_pan_id(new_ext_pan_id,
                                           new_ext_pan_id_str,
                                           sizeof(new_ext_pan_id_str));

        /* Build success response */
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(data, "old_extended_pan_id", current_ext_pan_id_str);
        cJSON_AddStringToObject(data, "extended_pan_id", new_ext_pan_id_str);
        cJSON_AddStringToObject(data, "status", "set_from_mac");

        bridge_response_publish_ok(
            RESPONSE_TOPIC_NETWORK_EXT_PAN_ID, data, get_current_transaction());
        cJSON_Delete(data);
        cJSON_Delete(json);

        ESP_LOGI(TAG, "Extended PAN ID set from MAC successfully");
        return ESP_OK;
    }

    /* No value or use_mac - return current Extended PAN ID (GET request) */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(data, "extended_pan_id", current_ext_pan_id_str);
    cJSON_AddBoolToObject(data, "configured", zb_network_has_extended_pan_id());

    /* Add network status */
    zb_network_info_t net_info;
    if (zb_network_get_info(&net_info) == ESP_OK) {
        cJSON_AddNumberToObject(data, "pan_id", net_info.pan_id);
        cJSON_AddNumberToObject(data, "channel", net_info.channel);
        cJSON_AddBoolToObject(data, "network_formed", net_info.network_formed);
    }

    bridge_response_publish_ok(RESPONSE_TOPIC_NETWORK_EXT_PAN_ID, data, get_current_transaction());
    cJSON_Delete(data);
    cJSON_Delete(json);

    return ESP_OK;
}

/* ============================================================================
 * Network Key Rotation (API-006)
 * ============================================================================ */

esp_err_t bridge_request_network_key_rotate(const char *payload)
{
    ESP_LOGI(TAG, "Network key rotation request");

    /* Payload is optional - we ignore it but may contain transaction ID */
    (void)payload;

    /* Initiate key rotation */
    uint32_t sequence = 0;
    esp_err_t ret = zb_network_rotate_key(&sequence);

    if (ret != ESP_OK) {
        char error_msg[128];
        if (ret == ESP_ERR_INVALID_STATE) {
            snprintf(error_msg, sizeof(error_msg),
                     "Cannot rotate key: network not formed or not initialized");
        } else {
            snprintf(error_msg, sizeof(error_msg),
                     "Key rotation failed: %s", esp_err_to_name(ret));
        }
        bridge_response_publish_error(RESPONSE_TOPIC_NETWORK_KEY_ROTATE,
                                     error_msg,
                                     get_current_transaction());
        return ret;
    }

    /* Build success response */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_NETWORK_KEY_ROTATE,
                                     "Memory allocation failed",
                                     get_current_transaction());
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(data, "key_rotation", "initiated");
    cJSON_AddNumberToObject(data, "sequence", (double)sequence);

    /* Add network info for context */
    zb_network_info_t net_info;
    if (zb_network_get_info(&net_info) == ESP_OK) {
        cJSON_AddNumberToObject(data, "device_count", net_info.device_count);
        cJSON_AddNumberToObject(data, "channel", net_info.channel);
    }

    bridge_response_publish_ok(RESPONSE_TOPIC_NETWORK_KEY_ROTATE, data, get_current_transaction());
    cJSON_Delete(data);

    ESP_LOGI(TAG, "Network key rotation initiated, sequence: %" PRIu32, sequence);
    return ESP_OK;
}

/* ============================================================================
 * Poll Control Commands
 * ============================================================================ */

/**
 * @brief Extract short address from poll control topic
 *
 * Topic format: zigbee2mqtt/bridge/request/poll_control/{short_addr}[/set]
 *
 * @param topic MQTT topic string
 * @param short_addr Output for parsed short address
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parse fails
 */
static esp_err_t parse_poll_control_short_addr(const char *topic, uint16_t *short_addr)
{
    if (topic == NULL || short_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find "/poll_control/" in topic */
    const char *start = strstr(topic, "/poll_control/");
    if (start == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Skip "/poll_control/" */
    start += strlen("/poll_control/");

    /* Parse hex address (0x prefix optional) */
    char *endptr;
    unsigned long addr;

    if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
        addr = strtoul(start, &endptr, 16);
    } else {
        addr = strtoul(start, &endptr, 16);
    }

    /* Check that we parsed something and it's a valid short address */
    if (endptr == start || addr > 0xFFFF) {
        return ESP_ERR_INVALID_ARG;
    }

    /* endptr should point to '/' or end of string */
    if (*endptr != '\0' && *endptr != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    *short_addr = (uint16_t)addr;
    return ESP_OK;
}

esp_err_t bridge_request_poll_control_get(const char *topic, const char *payload)
{
    ESP_LOGI(TAG, "Poll control get request");
    (void)payload;  /* Payload is optional for GET */

    /* Parse short address from topic */
    uint16_t short_addr;
    esp_err_t ret = parse_poll_control_short_addr(topic, &short_addr);
    if (ret != ESP_OK) {
        bridge_response_publish_error(RESPONSE_TOPIC_POLL_CONTROL_GET,
                                     "Invalid topic format: expected /poll_control/{short_addr}",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Getting poll control config for device 0x%04X", short_addr);

    /* Get poll control configuration */
    zb_poll_control_config_t config;
    ret = zb_poll_control_get_config(short_addr, &config);

    if (ret == ESP_ERR_NOT_FOUND) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Device 0x%04X not tracked by poll control", short_addr);
        bridge_response_publish_error(RESPONSE_TOPIC_POLL_CONTROL_GET,
                                     error_msg,
                                     get_current_transaction());
        return ret;
    }

    if (ret != ESP_OK) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Failed to get poll control config: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_POLL_CONTROL_GET,
                                     error_msg,
                                     get_current_transaction());
        return ret;
    }

    /* Build response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for poll control get response");
        bridge_response_publish_error(RESPONSE_TOPIC_POLL_CONTROL_GET,
                                     "Failed to create JSON response",
                                     get_current_transaction());
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(data, "short_addr", short_addr);

    /* Convert quarter-seconds to seconds for user-friendly output */
    cJSON_AddNumberToObject(data, "check_in_interval",
                           zb_poll_qs_to_seconds(config.check_in_interval));
    cJSON_AddNumberToObject(data, "check_in_interval_qs", config.check_in_interval);
    cJSON_AddNumberToObject(data, "long_poll_interval",
                           zb_poll_qs_to_seconds(config.long_poll_interval));
    cJSON_AddNumberToObject(data, "long_poll_interval_qs", config.long_poll_interval);
    cJSON_AddNumberToObject(data, "short_poll_interval",
                           zb_poll_qs_to_seconds(config.short_poll_interval));
    cJSON_AddNumberToObject(data, "short_poll_interval_qs", config.short_poll_interval);
    cJSON_AddNumberToObject(data, "fast_poll_timeout",
                           zb_poll_qs_to_seconds(config.fast_poll_timeout));
    cJSON_AddNumberToObject(data, "fast_poll_timeout_qs", config.fast_poll_timeout);
    cJSON_AddBoolToObject(data, "start_fast_polling", config.start_fast_polling);

    bridge_response_publish_ok(RESPONSE_TOPIC_POLL_CONTROL_GET, data, get_current_transaction());
    cJSON_Delete(data);

    ESP_LOGI(TAG, "Poll control config retrieved for device 0x%04X", short_addr);
    return ESP_OK;
}

esp_err_t bridge_request_poll_control_set(const char *topic, const char *payload)
{
    ESP_LOGI(TAG, "Poll control set request");

    /* Parse short address from topic */
    uint16_t short_addr;
    esp_err_t ret = parse_poll_control_short_addr(topic, &short_addr);
    if (ret != ESP_OK) {
        bridge_response_publish_error(
            RESPONSE_TOPIC_POLL_CONTROL_SET,
            "Invalid topic format: expected /poll_control/{short_addr}/set",
            get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Setting poll control config for device 0x%04X", short_addr);

    /* Parse payload */
    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_POLL_CONTROL_SET,
                                     "Invalid JSON payload",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Get check_in_interval from payload (in seconds) */
    cJSON *interval_item = cJSON_GetObjectItem(json, "check_in_interval");
    if (interval_item == NULL || !cJSON_IsNumber(interval_item)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_POLL_CONTROL_SET,
                                     "Missing 'check_in_interval' field in payload (seconds)",
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t interval_sec = (uint32_t)interval_item->valueint;

    /* Validate interval range (convert to quarter-seconds for validation) */
    uint32_t interval_qs = zb_poll_seconds_to_qs(interval_sec);
    if (interval_qs < ZB_POLL_MIN_CHECK_IN_INTERVAL ||
        interval_qs > ZB_POLL_MAX_CHECK_IN_INTERVAL) {
        cJSON_Delete(json);

        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Invalid check_in_interval: %lu seconds (must be %lu-%lu seconds)",
                 (unsigned long)interval_sec,
                 (unsigned long)zb_poll_qs_to_seconds(ZB_POLL_MIN_CHECK_IN_INTERVAL),
                 (unsigned long)zb_poll_qs_to_seconds(ZB_POLL_MAX_CHECK_IN_INTERVAL));
        bridge_response_publish_error(RESPONSE_TOPIC_POLL_CONTROL_SET,
                                     error_msg,
                                     get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    /* Set the interval */
    ret = zb_poll_control_set_interval_seconds(short_addr, interval_sec);

    cJSON_Delete(json);

    if (ret == ESP_ERR_NOT_FOUND) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Device 0x%04X not tracked by poll control", short_addr);
        bridge_response_publish_error(RESPONSE_TOPIC_POLL_CONTROL_SET,
                                     error_msg,
                                     get_current_transaction());
        return ret;
    }

    if (ret != ESP_OK) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "Failed to set poll control interval: %s",
                 esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_POLL_CONTROL_SET,
                                     error_msg,
                                     get_current_transaction());
        return ret;
    }

    /* Build success response data */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for poll control set response");
        bridge_response_publish_error(RESPONSE_TOPIC_POLL_CONTROL_SET,
                                     "Failed to create JSON response",
                                     get_current_transaction());
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(data, "short_addr", short_addr);
    cJSON_AddNumberToObject(data, "check_in_interval", interval_sec);
    cJSON_AddNumberToObject(data, "check_in_interval_qs", interval_qs);
    cJSON_AddStringToObject(data, "status", "interval_updated");

    bridge_response_publish_ok(RESPONSE_TOPIC_POLL_CONTROL_SET, data, get_current_transaction());
    cJSON_Delete(data);

    ESP_LOGI(TAG, "Poll control interval set to %lu seconds for device 0x%04X",
             (unsigned long)interval_sec, short_addr);
    return ESP_OK;
}

/* ============================================================================
 * BLE Scanner Toggle Handler
 * ============================================================================ */

#if CONFIG_BT_SCANNER_ENABLED
static esp_err_t handle_ble_scanner(const char *topic, const char *payload, size_t len)
{
    (void)topic;
    (void)len;

    ESP_LOGI(TAG, "Processing BLE scanner request");

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        bridge_response_publish_error(RESPONSE_TOPIC_BLE_SCANNER,
                                      "Invalid JSON payload",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *value = cJSON_GetObjectItem(json, "value");
    if (value == NULL || !cJSON_IsBool(value)) {
        cJSON_Delete(json);
        bridge_response_publish_error(RESPONSE_TOPIC_BLE_SCANNER,
                                      "Missing or invalid 'value' field",
                                      get_current_transaction());
        return ESP_ERR_INVALID_ARG;
    }

    bool enable = cJSON_IsTrue(value);
    cJSON_Delete(json);

    esp_err_t ret;
    if (enable) {
        ret = ble_scanner_start();
    } else {
        ret = ble_scanner_stop();
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "BLE scanner %s", enable ? "started" : "stopped");

        /* Publish updated bridge state for immediate HA feedback */
        ha_bridge_publish_state();

        cJSON *data = cJSON_CreateObject();
        if (data != NULL) {
            cJSON_AddBoolToObject(data, "value", enable);
            bridge_response_publish_ok(RESPONSE_TOPIC_BLE_SCANNER,
                                       data, get_current_transaction());
            cJSON_Delete(data);
        }
    } else {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "Failed to %s BLE scanner: %s",
                 enable ? "start" : "stop", esp_err_to_name(ret));
        bridge_response_publish_error(RESPONSE_TOPIC_BLE_SCANNER,
                                      err_msg, get_current_transaction());
    }

    return ret;
}
#endif /* CONFIG_BT_SCANNER_ENABLED */