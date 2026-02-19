/**
 * @file mqtt_topics.h
 * @brief MQTT Topics Structure for Zigbee2MQTT Compatibility
 *
 * Defines MQTT topic structure compatible with Zigbee2MQTT conventions
 * and provides helper functions for topic string manipulation.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef MQTT_TOPICS_H
#define MQTT_TOPICS_H

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Base topic for all Zigbee2MQTT messages */
#define MQTT_BASE_TOPIC CONFIG_MQTT_BASE_TOPIC

/* Bridge topics (coordinator status and control) */
#define TOPIC_BRIDGE_STATE       "zigbee2mqtt/bridge/state"
#define TOPIC_BRIDGE_INFO        "zigbee2mqtt/bridge/info"
#define TOPIC_BRIDGE_DEVICES     "zigbee2mqtt/bridge/devices"
#define TOPIC_BRIDGE_GROUPS      "zigbee2mqtt/bridge/groups"
#define TOPIC_BRIDGE_LOGGING     "zigbee2mqtt/bridge/logging"
#define TOPIC_BRIDGE_CONFIG      "zigbee2mqtt/bridge/config"
#define TOPIC_BRIDGE_SYSTEM      "zigbee2mqtt/bridge/system"
#define TOPIC_BRIDGE_REQUEST     "zigbee2mqtt/bridge/request/#"

/* Device command topic patterns (for subscriptions) */
#define TOPIC_DEVICE_SET_PATTERN "zigbee2mqtt/+/set"
#define TOPIC_DEVICE_GET_PATTERN "zigbee2mqtt/+/get"

/* Bridge request subtopics */
#define TOPIC_BRIDGE_REQUEST_PERMIT_JOIN    "zigbee2mqtt/bridge/request/permit_join"
#define TOPIC_BRIDGE_REQUEST_DEVICE_REMOVE  "zigbee2mqtt/bridge/request/device/remove"
#define TOPIC_BRIDGE_REQUEST_DEVICE_RENAME  "zigbee2mqtt/bridge/request/device/rename"
#define TOPIC_BRIDGE_REQUEST_HEALTH_CHECK   "zigbee2mqtt/bridge/request/health_check"
#define TOPIC_BRIDGE_REQUEST_AVAIL_CHECK    "zigbee2mqtt/bridge/request/device/availability/check"
#define TOPIC_BRIDGE_REQUEST_DEVICE_CONFIGURE "zigbee2mqtt/bridge/request/device/configure"
#define TOPIC_BRIDGE_REQUEST_DEVICE_OPTIONS   "zigbee2mqtt/bridge/request/device/options"
#define TOPIC_BRIDGE_REQUEST_DEVICE_GET       "zigbee2mqtt/bridge/request/device/get"

/* Logging request subtopics */
#define TOPIC_BRIDGE_REQUEST_LOGGING_LEVEL      "zigbee2mqtt/bridge/request/logging/level"
#define TOPIC_BRIDGE_REQUEST_LOGGING_NAMESPACES "zigbee2mqtt/bridge/request/logging/namespaces"

/* Logging response topics */
#define TOPIC_BRIDGE_RESPONSE_LOGGING_LEVEL      "zigbee2mqtt/bridge/response/logging/level"
#define TOPIC_BRIDGE_RESPONSE_LOGGING_NAMESPACES "zigbee2mqtt/bridge/response/logging/namespaces"

/* Group request subtopics */
#define TOPIC_BRIDGE_REQUEST_GROUP_ADD           "zigbee2mqtt/bridge/request/group/add"
#define TOPIC_BRIDGE_REQUEST_GROUP_REMOVE        "zigbee2mqtt/bridge/request/group/remove"
#define TOPIC_BRIDGE_REQUEST_GROUP_MEMBERS_ADD   "zigbee2mqtt/bridge/request/group/members/add"
#define TOPIC_BRIDGE_REQUEST_GROUP_MEMBERS_REMOVE "zigbee2mqtt/bridge/request/group/members/remove"

/* Install code request subtopics */
#define TOPIC_BRIDGE_REQUEST_INSTALL_CODE_ADD    "zigbee2mqtt/bridge/request/install_code/add"
#define TOPIC_BRIDGE_REQUEST_INSTALL_CODE_REMOVE "zigbee2mqtt/bridge/request/install_code/remove"
#define TOPIC_BRIDGE_REQUEST_INSTALL_CODE_LIST   "zigbee2mqtt/bridge/request/install_code/list"

/* Bridge restart request subtopic */
#define TOPIC_BRIDGE_REQUEST_RESTART             "zigbee2mqtt/bridge/request/restart"

/* Factory reset request subtopic (erases all NVS and restarts) */
#define TOPIC_BRIDGE_REQUEST_FACTORY_RESET       "zigbee2mqtt/bridge/request/factory_reset"

/* Network reset request subtopic (erases Zigbee network/devices, preserves WiFi/MQTT config) */
#define TOPIC_BRIDGE_REQUEST_RESET_NETWORK       "zigbee2mqtt/bridge/request/reset/network"

/* Config reset request subtopic (erases WiFi/gateway config, preserves Zigbee network) */
#define TOPIC_BRIDGE_REQUEST_RESET_CONFIG        "zigbee2mqtt/bridge/request/reset/config"

/* Backup/restore request subtopics */
#define TOPIC_BRIDGE_REQUEST_BACKUP              "zigbee2mqtt/bridge/request/backup"
#define TOPIC_BRIDGE_REQUEST_BACKUP_STATE        "zigbee2mqtt/bridge/request/backup/state"
#define TOPIC_BRIDGE_REQUEST_BACKUP_DEVICES      "zigbee2mqtt/bridge/request/backup/devices"
#define TOPIC_BRIDGE_REQUEST_RESTORE             "zigbee2mqtt/bridge/request/restore"
#define TOPIC_BRIDGE_REQUEST_BACKUP_LIST         "zigbee2mqtt/bridge/request/backup/list"

/* Coordinator request subtopics (ZG-012) */
#define TOPIC_BRIDGE_REQUEST_COORDINATOR_CHECK   "zigbee2mqtt/bridge/request/coordinator/check"
#define TOPIC_BRIDGE_REQUEST_COORDINATOR_VERSION "zigbee2mqtt/bridge/request/coordinator/version"
#define TOPIC_BRIDGE_REQUEST_COORDINATOR_SETTINGS "zigbee2mqtt/bridge/request/coordinator/settings"

/* Network channel change subtopics (ZG-016) */
#define TOPIC_BRIDGE_REQUEST_OPTIONS             "zigbee2mqtt/bridge/request/options"
#define TOPIC_BRIDGE_REQUEST_NETWORK_CHANNEL     "zigbee2mqtt/bridge/request/network/channel"

/* Extended PAN ID management subtopic (API-004) */
#define TOPIC_BRIDGE_REQUEST_NETWORK_EXT_PAN_ID  "zigbee2mqtt/bridge/request/network/extended_pan_id"

/* Network key rotation subtopic (API-006) */
#define TOPIC_BRIDGE_REQUEST_NETWORK_KEY_ROTATE  "zigbee2mqtt/bridge/request/network/key_rotate"

/* Network map request subtopic */
#define TOPIC_BRIDGE_REQUEST_NETWORKMAP          "zigbee2mqtt/bridge/request/networkmap"

/** @brief BLE scanner control topic */
#define TOPIC_BRIDGE_REQUEST_BLE_SCANNER         "zigbee2mqtt/bridge/request/ble_scanner"

/* Group command topics */
#define TOPIC_GROUP_SET_PREFIX "zigbee2mqtt/group/"
#define TOPIC_GROUP_SET_SUFFIX "/set"

/* Home Assistant discovery prefix */
#define TOPIC_HA_DISCOVERY_PREFIX CONFIG_HOMEASSISTANT_DISCOVERY_PREFIX

/* ============================================================================
 * MQTT Buffer Size Constants
 * ============================================================================ */

/** @brief Maximum topic length in bytes */
#define MQTT_TOPIC_MAX_LEN          256

/** @brief Small payload buffer size (typical for single values) */
#define MQTT_PAYLOAD_SMALL          256

/** @brief Medium payload buffer size (typical for JSON objects) */
#define MQTT_PAYLOAD_MEDIUM         512

/** @brief Large payload buffer size (complex JSON structures) */
#define MQTT_PAYLOAD_LARGE          1024

/** @brief Maximum payload buffer size (safety limit) */
#define MQTT_PAYLOAD_MAX            4096

/** @brief Maximum MQTT client ID length */
#define MQTT_CLIENT_ID_MAX_LEN      64

/** @brief String buffer for IEEE address (format: "0x0123456789abcdef") */
#define MQTT_IEEE_ADDR_STR_LEN      19

/** @brief String buffer for unique IDs in discovery */
#define MQTT_UNIQUE_ID_MAX_LEN      80

/** @brief Generic short string buffer (tags, names, etc.) */
#define MQTT_SHORT_STR_MAX_LEN      32

/** @brief Generic medium string buffer (device names, states) */
#define MQTT_MEDIUM_STR_MAX_LEN     64

/** @brief Generic long string buffer (full topics with paths) */
#define MQTT_LONG_STR_MAX_LEN       128

/**
 * @brief Build device state topic
 *
 * Constructs topic for device state updates: "zigbee2mqtt/[friendly_name]"
 *
 * @param[in] friendly_name Device friendly name
 * @param[out] topic_buf Buffer to store resulting topic
 * @param[in] buf_len Length of the buffer
 *
 * @return
 *     - ESP_OK: Topic built successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NO_MEM: Buffer too small
 */
esp_err_t mqtt_topic_device_state(const char *friendly_name, char *topic_buf, size_t buf_len);

/**
 * @brief Build device set topic
 *
 * Constructs topic for device control: "zigbee2mqtt/[friendly_name]/set"
 *
 * @param[in] friendly_name Device friendly name
 * @param[out] topic_buf Buffer to store resulting topic
 * @param[in] buf_len Length of the buffer
 *
 * @return
 *     - ESP_OK: Topic built successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NO_MEM: Buffer too small
 */
esp_err_t mqtt_topic_device_set(const char *friendly_name, char *topic_buf, size_t buf_len);

/**
 * @brief Build device get topic
 *
 * Constructs topic for device state query: "zigbee2mqtt/[friendly_name]/get"
 *
 * @param[in] friendly_name Device friendly name
 * @param[out] topic_buf Buffer to store resulting topic
 * @param[in] buf_len Length of the buffer
 *
 * @return
 *     - ESP_OK: Topic built successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NO_MEM: Buffer too small
 */
esp_err_t mqtt_topic_device_get(const char *friendly_name, char *topic_buf, size_t buf_len);

/**
 * @brief Build device availability topic
 *
 * Constructs topic for device availability: "zigbee2mqtt/[friendly_name]/availability"
 *
 * @param[in] friendly_name Device friendly name
 * @param[out] topic_buf Buffer to store resulting topic
 * @param[in] buf_len Length of the buffer
 *
 * @return
 *     - ESP_OK: Topic built successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NO_MEM: Buffer too small
 */
esp_err_t mqtt_topic_device_availability(const char *friendly_name, char *topic_buf, size_t buf_len);

/**
 * @brief Build Home Assistant discovery topic
 *
 * Constructs topic for HA device discovery:
 * "homeassistant/[component]/[device_id]/config"
 *
 * @param[in] component HA component type (e.g., "light", "sensor", "switch")
 * @param[in] device_id Unique device identifier
 * @param[out] topic_buf Buffer to store resulting topic
 * @param[in] buf_len Length of the buffer
 *
 * @return
 *     - ESP_OK: Topic built successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NO_MEM: Buffer too small
 */
esp_err_t mqtt_topic_ha_discovery(const char *component, const char *device_id,
                                  char *topic_buf, size_t buf_len);

/**
 * @brief Sanitize friendly name for MQTT topics
 *
 * Replaces spaces and special characters with underscores to create
 * MQTT-compatible topic names.
 *
 * @param[in] name Original friendly name
 * @param[out] sanitized Buffer to store sanitized name
 * @param[in] buf_len Length of the buffer
 *
 * @return
 *     - ESP_OK: Name sanitized successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NO_MEM: Buffer too small
 */
esp_err_t mqtt_topic_sanitize_name(const char *name, char *sanitized, size_t buf_len);

/**
 * @brief Extract friendly name from device topic
 *
 * Extracts device friendly name from a topic like "zigbee2mqtt/[name]/set"
 *
 * @param[in] topic Full MQTT topic
 * @param[out] friendly_name Buffer to store extracted name
 * @param[in] buf_len Length of the buffer
 *
 * @return
 *     - ESP_OK: Name extracted successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments or topic format
 *     - ESP_ERR_NO_MEM: Buffer too small
 */
esp_err_t mqtt_topic_extract_friendly_name(const char *topic, char *friendly_name, size_t buf_len);

/**
 * @brief Check if topic matches pattern
 *
 * Checks if topic matches pattern with wildcard support (+ and #)
 *
 * @param[in] topic Topic to check
 * @param[in] pattern Pattern with wildcards
 *
 * @return
 *     - true: Topic matches pattern
 *     - false: Topic does not match pattern
 */
bool mqtt_topic_matches(const char *topic, const char *pattern);

/**
 * @brief Test MQTT topics functionality
 *
 * Performs basic topic manipulation test and reports results.
 * For debugging and verification purposes.
 *
 * @return
 *     - ESP_OK: Test passed
 *     - ESP_FAIL: Test failed
 */
esp_err_t mqtt_topic_test(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_TOPICS_H */
