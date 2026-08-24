/**
 * @file esphome_common.h
 * @brief Common Definitions for ESPHome Native API
 *
 * Provides shared types, constants, and definitions used across
 * the ESPHome API module implementation.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_COMMON_H
#define ESPHOME_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Version Information
 * ============================================================================ */

/** @brief ESPHome API protocol version (major) */
#define ESPHOME_API_VERSION_MAJOR       1

/** @brief ESPHome API protocol version (minor) */
#define ESPHOME_API_VERSION_MINOR       10

/** @brief Implementation name for device info */
#define ESPHOME_DEVICE_NAME             "ESP32-C5 Zigbee Gateway"

/** @brief Compilation date placeholder */
#define ESPHOME_COMPILATION_TIME        __DATE__ " " __TIME__

/* ============================================================================
 * ESPHome Timeout Constants
 * ============================================================================
 * Centralized timeout values for ESPHome module operations.
 * CQ-011: Consolidated from hardcoded values in esphome_*.c files.
 * ============================================================================ */

/** @brief Minimal delay for polling loops (10ms) */
#define ESPHOME_DELAY_MINIMAL_MS            10

/** @brief Fast mutex timeout for quick operations (50ms) */
#define ESPHOME_MUTEX_TIMEOUT_FAST_MS       50

/** @brief Standard mutex timeout (100ms) */
#define ESPHOME_MUTEX_TIMEOUT_MS            100

/** @brief Medium delay for rate limiting (500ms) */
#define ESPHOME_DELAY_MEDIUM_MS             500

/** @brief Extended mutex timeout for longer operations (1000ms) */
#define ESPHOME_MUTEX_TIMEOUT_EXTENDED_MS   1000

/** @brief Restart delay before device reset (2000ms) */
#define ESPHOME_OTA_RESTART_DELAY_MS        2000

/** @brief Very long timeout for complex operations (3000ms) */
#define ESPHOME_MUTEX_TIMEOUT_LONG_MS       3000

/* FreeRTOS tick versions */
#define ESPHOME_DELAY_MINIMAL_TICKS         pdMS_TO_TICKS(ESPHOME_DELAY_MINIMAL_MS)
#define ESPHOME_MUTEX_TIMEOUT_FAST_TICKS    pdMS_TO_TICKS(ESPHOME_MUTEX_TIMEOUT_FAST_MS)
#define ESPHOME_MUTEX_TIMEOUT_TICKS         pdMS_TO_TICKS(ESPHOME_MUTEX_TIMEOUT_MS)
#define ESPHOME_DELAY_MEDIUM_TICKS          pdMS_TO_TICKS(ESPHOME_DELAY_MEDIUM_MS)
#define ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS pdMS_TO_TICKS(ESPHOME_MUTEX_TIMEOUT_EXTENDED_MS)
#define ESPHOME_OTA_RESTART_DELAY_TICKS     pdMS_TO_TICKS(ESPHOME_OTA_RESTART_DELAY_MS)
#define ESPHOME_MUTEX_TIMEOUT_LONG_TICKS    pdMS_TO_TICKS(ESPHOME_MUTEX_TIMEOUT_LONG_MS)

/* ============================================================================
 * Network Configuration
 * ============================================================================ */

/** @brief Default ESPHome API port */
#define ESPHOME_API_PORT                6053

/** @brief Maximum simultaneous client connections */
/* Follows the Kconfig knob. It used to be a hardcoded 2 while
 * ESPHOME_API_MAX_CLIENTS offered a range of 1-4, and esphome_api_init() then
 * clamped the configured value back down — so setting 4 silently gave you 2.
 * Two is genuinely tight: Home Assistant plus one connection that has stopped
 * responding fills both slots and locks everything else out. */
#ifdef CONFIG_ESPHOME_API_MAX_CLIENTS
#define ESPHOME_MAX_CLIENTS             CONFIG_ESPHOME_API_MAX_CLIENTS
#else
#define ESPHOME_MAX_CLIENTS             2
#endif

/** @brief Socket receive buffer size */
#define ESPHOME_RX_BUFFER_SIZE          1024

/** @brief Socket transmit buffer size */
#define ESPHOME_TX_BUFFER_SIZE          1024

/** @brief Socket receive timeout in milliseconds */
#define ESPHOME_SOCKET_TIMEOUT_MS       5000

/** @brief Peeks allowed while identifying a new client as Noise or plaintext
 *
 * Each attempt blocks for up to ESPHOME_SOCKET_TIMEOUT_MS, so this is the
 * patience budget for a client that has connected but not yet spoken.
 */
#define ESPHOME_PROTOCOL_PEEK_ATTEMPTS  3

/** @brief Connection keepalive interval in seconds */
#define ESPHOME_KEEPALIVE_INTERVAL      30

/** @brief How long a client may sit unauthenticated before its slot is reclaimed
 *
 * aioesphomeapi sends hello and handshake in the same packet as the connection,
 * so a real client has spoken within milliseconds. Anything still silent after
 * this is a port scan, a TCP health check or a dead peer — and every one of
 * those holds a slot that Home Assistant then cannot have.
 *
 * Fifteen seconds was the first guess and it was too generous: a monitoring
 * loop doing a bare TCP connect once a minute was enough to make the gateway
 * look like it had dropped off the network, because the probes accumulated
 * faster than they expired. Measured against real clients, five seconds is
 * still three orders of magnitude more than they need.
 */
#define ESPHOME_HANDSHAKE_IDLE_TIMEOUT_MS   5000

/** @brief How long a client may take over the ESPHome login once encrypted
 *
 * Applies after the Noise handshake, where the client has proven it holds the
 * key and is merely slow. Long enough that a busy gateway enumerating sixty
 * entities does not lose the client that asked.
 */
#define ESPHOME_LOGIN_IDLE_TIMEOUT_MS       30000

/** @brief How long a single send may block before the client is dropped
 *
 * Without this the socket blocks forever when a peer vanishes without closing,
 * the client task never reaches its cleanup, and its slot is lost for good.
 */
#define ESPHOME_SEND_TIMEOUT_MS             5000

/** @brief Seconds of silence before TCP keepalive probing starts */
#define ESPHOME_KEEPALIVE_IDLE_SEC          30

/** @brief Seconds between TCP keepalive probes */
#define ESPHOME_KEEPALIVE_INTERVAL_SEC      10

/** @brief Unanswered keepalive probes before the connection is dead */
#define ESPHOME_KEEPALIVE_COUNT             3

/** @brief How long interviews are collected before one re-discovery disconnect
 *
 * Sized from a measurement, not from taste: two back-to-back reconfigure calls
 * produced interview completions 20.6 s apart, because an interview holds the
 * radio for about that long. A 5 s window collapsed nothing at all. 30 s folds
 * consecutive interviews into one disconnect and costs Home Assistant at most
 * 30 s before it sees the new entities — next to an interview that already took
 * 20 s, that is not the slow part.
 */
#define ESPHOME_REDISCOVERY_DELAY_MS        30000

/** @brief Maximum connection idle time before disconnect (seconds) */
#define ESPHOME_MAX_IDLE_TIME           120

/* ============================================================================
 * Message Limits
 * ============================================================================ */

/** @brief Maximum message size in bytes */
#define ESPHOME_MAX_MESSAGE_SIZE        4096

/** @brief Maximum string length in messages */
#define ESPHOME_MAX_STRING_LEN          256

/** @brief Maximum entity name length */
#define ESPHOME_MAX_NAME_LEN            64

/** @brief Maximum entity unique ID length */
#define ESPHOME_MAX_UNIQUE_ID_LEN       64

/** @brief Maximum icon string length */
#define ESPHOME_MAX_ICON_LEN            32

/** @brief Maximum unit string length */
#define ESPHOME_MAX_UNIT_LEN            16

/* ============================================================================
 * Buffer Size Constants
 * ============================================================================ */

/** @brief Small message buffer size */
#define ESPHOME_BUFFER_SMALL            128     /**< Small buffer (128 bytes) */

/** @brief Medium message buffer size */
#define ESPHOME_BUFFER_MEDIUM           256     /**< Medium buffer (256 bytes) */

/** @brief Large message buffer size */
#define ESPHOME_BUFFER_LARGE            512     /**< Large buffer (512 bytes) */

/** @brief Extra-large message buffer size */
#define ESPHOME_BUFFER_XLARGE           1024    /**< Extra-large buffer (1024 bytes) */

/** @brief String buffer size for fixed-length strings */
#define ESPHOME_STRING_BUFFER_SHORT     32      /**< Short string buffer (32 bytes) */

/** @brief String buffer size for medium strings */
#define ESPHOME_STRING_BUFFER_MEDIUM    64      /**< Medium string buffer (64 bytes) */

/** @brief String buffer size for long strings */
#define ESPHOME_STRING_BUFFER_LONG      128     /**< Long string buffer (128 bytes) */

/** @brief String buffer size for extra-long strings */
#define ESPHOME_STRING_BUFFER_XLARGE    256     /**< Extra-long string buffer (256 bytes) */

/** @brief Fixed-size array for temporary name storage */
#define ESPHOME_TEMP_NAME_SIZE          32      /**< Temporary name buffer (32 bytes) */

/** @brief Fixed-size array for temporary effect storage */
#define ESPHOME_TEMP_EFFECT_SIZE        32      /**< Temporary effect buffer (32 bytes) */

/** @brief Fixed-size array for option storage */
#define ESPHOME_OPTION_SIZE             32      /**< Option name buffer (32 bytes) */

/** @brief Output buffer for UTF-8 string encoding */
#define ESPHOME_OUTPUT_BUFFER_SMALL     16      /**< Output buffer for small messages (16 bytes) */

/** @brief Output buffer for standard messages */
#define ESPHOME_OUTPUT_BUFFER_STANDARD  32      /**< Output buffer for standard messages (32 bytes) */

/** @brief Payload buffer for small frames */
#define ESPHOME_PAYLOAD_SMALL           16      /**< Small payload buffer (16 bytes) */

/** @brief Payload buffer for standard frames */
#define ESPHOME_PAYLOAD_MEDIUM          32      /**< Standard payload buffer (32 bytes) */

/** @brief MD5 hash size in bytes */
#define ESPHOME_MD5_SIZE                16      /**< MD5 hash size (16 bytes) */

/** @brief Task name buffer size */
#define ESPHOME_TASK_NAME_SIZE          16      /**< Task name buffer (16 bytes) */

/** @brief Regex pattern buffer size */
#define ESPHOME_PATTERN_BUFFER_SIZE     64      /**< Pattern buffer for regex validation (64 bytes) */

/* ============================================================================
 * BLE Proxy Constants (CQ-017)
 * ============================================================================ */

/** @brief Maximum iterations for BLE proxy wait loop (50 * 10ms = 500ms max) */
#define ESPHOME_BLE_PROXY_MAX_WAIT_ITERATIONS   50

/** @brief Maximum iterations for AD type parsing safety limit */
#define ESPHOME_BLE_AD_MAX_PARSE_ITERATIONS     32

/** @brief BLE proxy payload buffer size */
#define ESPHOME_BLE_PROXY_PAYLOAD_SIZE          512

/** @brief BLE data buffer size for advertisement data */
#define ESPHOME_BLE_DATA_BUFFER_SIZE            256

/* ============================================================================
 * Service Buffer Constants
 * ============================================================================ */

/** @brief Service payload buffer size */
#define ESPHOME_SERVICE_PAYLOAD_SIZE            512

/** @brief Service output buffer size */
#define ESPHOME_SERVICE_OUTPUT_SIZE             256

/* ============================================================================
 * OTA Constants
 * ============================================================================ */

/** @brief OTA nonce length in bytes */
#define ESPHOME_OTA_NONCE_LENGTH                16

/* ============================================================================
 * Testing Constants
 * ============================================================================ */

/** @brief ESPHome API alternate port for testing */
#define ESPHOME_API_TEST_PORT                   16053

/* ============================================================================
 * Entity Limits
 * ============================================================================ */

/** @brief Maximum number of sensor entities */
/* Entity capacity.
 *
 * These were sized for a gateway with one or two Zigbee devices and were nearly
 * full at exactly that: two devices used 30 of 32 sensors, 14 of 16 text
 * sensors and 12 of 16 buttons, so a third would have been refused — a device
 * that pairs, interviews and binds a converter, and then silently has no
 * entities.
 *
 * The table is a single allocation in PSRAM, of which six megabytes sit idle,
 * so the limit was never about memory. The numbers below hold roughly fifteen
 * Zigbee devices.
 *
 * What they do cost is enumeration: Home Assistant reads every entity on each
 * connect, and that transfer is the fragile part of this link — a timeout there
 * leaves the device with no entities at all until it reconnects. More entities
 * make that window longer. esphome_entity_init() logs the table's real size so
 * the trade stays visible rather than assumed.
 */
#define ESPHOME_MAX_SENSORS             192

/** @brief Maximum number of binary sensor entities */
#define ESPHOME_MAX_BINARY_SENSORS      64

/** @brief Maximum number of switch entities */
#define ESPHOME_MAX_SWITCHES            48

/** @brief Maximum number of text sensor entities */
#define ESPHOME_MAX_TEXT_SENSORS        64

/** @brief Maximum number of number entities */
#define ESPHOME_MAX_NUMBERS             48

/** @brief Maximum number of button entities */
#define ESPHOME_MAX_BUTTONS             96

/** @brief Maximum number of select entities */
#define ESPHOME_MAX_SELECTS             32

/** @brief Maximum number of light entities */
#define ESPHOME_MAX_LIGHTS              32

/** @brief Maximum number of cover entities */
#define ESPHOME_MAX_COVERS              16

/** @brief Maximum number of fan entities */
#define ESPHOME_MAX_FANS                16

/** @brief Maximum number of climate entities */
#define ESPHOME_MAX_CLIMATES            16

/** @brief Maximum number of lock entities */
#define ESPHOME_MAX_LOCKS               16

/** @brief Maximum number of media player entities */
#define ESPHOME_MAX_MEDIA_PLAYERS       4

/** @brief Maximum number of alarm control panel entities */
#define ESPHOME_MAX_ALARM_PANELS        4

/** @brief Maximum number of text entities */
#define ESPHOME_MAX_TEXTS               16

/** @brief Maximum number of services */
#define ESPHOME_MAX_SERVICES            16

/** @brief Maximum select options per entity */
/* Options a select entity can carry.
 *
 * At 8 this truncated in silence: 777 of the 5928 selects in the converter
 * database declare more, up to 18, and a select showing eight of sixteen
 * colours looks perfectly normal while the other eight are simply unreachable.
 * The storage is options[N][32] inside the entity table, which lives in PSRAM,
 * so the whole increase costs 4 KB of memory that is otherwise idle. */
#define ESPHOME_MAX_SELECT_OPTIONS      24

/** @brief Maximum light effects per entity */
#define ESPHOME_MAX_LIGHT_EFFECTS       8

/** @brief Maximum service arguments */
#define ESPHOME_MAX_SERVICE_ARGS        8

/** @brief Total maximum entities */
#define ESPHOME_MAX_ENTITIES            (ESPHOME_MAX_SENSORS + \
                                         ESPHOME_MAX_BINARY_SENSORS + \
                                         ESPHOME_MAX_SWITCHES + \
                                         ESPHOME_MAX_TEXT_SENSORS + \
                                         ESPHOME_MAX_NUMBERS + \
                                         ESPHOME_MAX_BUTTONS + \
                                         ESPHOME_MAX_SELECTS + \
                                         ESPHOME_MAX_LIGHTS + \
                                         ESPHOME_MAX_COVERS + \
                                         ESPHOME_MAX_FANS + \
                                         ESPHOME_MAX_CLIMATES + \
                                         ESPHOME_MAX_LOCKS + \
                                         ESPHOME_MAX_MEDIA_PLAYERS + \
                                         ESPHOME_MAX_ALARM_PANELS + \
                                         ESPHOME_MAX_TEXTS)

/* ============================================================================
 * Message Types (ESPHome Native API Protocol)
 * ============================================================================ */

/**
 * @brief ESPHome API message types
 *
 * Based on ESPHome Native API protocol specification.
 * Messages are encoded with Protobuf-compatible wire format.
 */
typedef enum {
    /* Core handshake messages (1-8) */
    ESPHOME_MSG_HELLO_REQUEST               = 1,   /**< Client hello request */
    ESPHOME_MSG_HELLO_RESPONSE              = 2,   /**< Server hello response */
    ESPHOME_MSG_CONNECT_REQUEST             = 3,   /**< Client connect request */
    ESPHOME_MSG_CONNECT_RESPONSE            = 4,   /**< Server connect response */
    ESPHOME_MSG_DISCONNECT_REQUEST          = 5,   /**< Client disconnect request */
    ESPHOME_MSG_DISCONNECT_RESPONSE         = 6,   /**< Server disconnect response */
    ESPHOME_MSG_PING_REQUEST                = 7,   /**< Client ping request */
    ESPHOME_MSG_PING_RESPONSE               = 8,   /**< Server ping response */

    /* Device info messages (9-10) */
    ESPHOME_MSG_DEVICE_INFO_REQUEST         = 9,   /**< Request device info */
    ESPHOME_MSG_DEVICE_INFO_RESPONSE        = 10,  /**< Device info response */

    /* Entity list messages */
    ESPHOME_MSG_LIST_ENTITIES_REQUEST       = 11,  /**< Request entity list */
    ESPHOME_MSG_LIST_ENTITIES_DONE_RESPONSE = 19,  /**< Entity list complete */

    /* Entity responses for list entities (different message types per entity) */
    ESPHOME_MSG_LIST_ENTITIES_BINARY_SENSOR = 12,  /**< Binary sensor entity info */
    ESPHOME_MSG_LIST_ENTITIES_COVER         = 13,  /**< Cover entity info */
    ESPHOME_MSG_LIST_ENTITIES_FAN           = 14,  /**< Fan entity info */
    ESPHOME_MSG_LIST_ENTITIES_LIGHT         = 15,  /**< Light entity info */
    ESPHOME_MSG_LIST_ENTITIES_SENSOR        = 16,  /**< Sensor entity info */
    ESPHOME_MSG_LIST_ENTITIES_SWITCH        = 17,  /**< Switch entity info */
    ESPHOME_MSG_LIST_ENTITIES_TEXT_SENSOR   = 18,  /**< Text sensor entity info */

    /* State subscription messages (20-27) */
    ESPHOME_MSG_SUBSCRIBE_STATES_REQUEST    = 20,  /**< Subscribe to state updates */
    ESPHOME_MSG_BINARY_SENSOR_STATE         = 21,  /**< Binary sensor state update */
    ESPHOME_MSG_COVER_STATE                 = 22,  /**< Cover state update */
    ESPHOME_MSG_FAN_STATE                   = 23,  /**< Fan state update */
    ESPHOME_MSG_LIGHT_STATE                 = 24,  /**< Light state update */
    ESPHOME_MSG_SENSOR_STATE                = 25,  /**< Sensor state update */
    ESPHOME_MSG_SWITCH_STATE                = 26,  /**< Switch state update */
    ESPHOME_MSG_TEXT_SENSOR_STATE           = 27,  /**< Text sensor state update */

    /* Logs subscription (28-29) */
    ESPHOME_MSG_SUBSCRIBE_LOGS_REQUEST      = 28,  /**< Subscribe to logs */
    ESPHOME_MSG_SUBSCRIBE_LOGS_RESPONSE     = 29,  /**< Log message */

    /* Command messages (30-33) */
    ESPHOME_MSG_COVER_COMMAND               = 30,  /**< Cover command */
    ESPHOME_MSG_FAN_COMMAND                 = 31,  /**< Fan command */
    ESPHOME_MSG_LIGHT_COMMAND               = 32,  /**< Light command */
    ESPHOME_MSG_SWITCH_COMMAND              = 33,  /**< Switch command */

    /* Home Assistant services (34-35) */
    ESPHOME_MSG_SUBSCRIBE_HA_SERVICES       = 34,  /**< Subscribe to HA services */
    ESPHOME_MSG_HA_SERVICE_RESPONSE         = 35,  /**< HA service response */

    /* Get time (36-37) */
    ESPHOME_MSG_GET_TIME_REQUEST            = 36,  /**< Request current time */
    ESPHOME_MSG_GET_TIME_RESPONSE           = 37,  /**< Time response */

    /* Home Assistant state subscription (38-39) */
    ESPHOME_MSG_SUBSCRIBE_HOME_ASSISTANT_STATES = 38,  /**< Subscribe to HA states */
    ESPHOME_MSG_HOME_ASSISTANT_STATE_RESPONSE   = 39,  /**< HA state response */

    /* Services (40-42) */
    ESPHOME_MSG_LIST_SERVICES_REQUEST       = 40,  /**< List available services */
    ESPHOME_MSG_LIST_SERVICES_RESPONSE      = 41,  /**< Service definition response */
    ESPHOME_MSG_EXECUTE_SERVICE             = 42,  /**< Execute a service */

    /* Camera (43-44) */
    ESPHOME_MSG_SUBSCRIBE_CAMERA_REQUEST    = 43,  /**< Subscribe to camera */
    ESPHOME_MSG_CAMERA_IMAGE_RESPONSE       = 44,  /**< Camera image response */

    /* Climate entity (46-48) */
    ESPHOME_MSG_LIST_ENTITIES_CLIMATE       = 46,  /**< Climate entity info */
    ESPHOME_MSG_CLIMATE_STATE               = 47,  /**< Climate state update */
    ESPHOME_MSG_CLIMATE_COMMAND             = 48,  /**< Climate command */

    /* Number entity (49-51) */
    ESPHOME_MSG_LIST_ENTITIES_NUMBER        = 49,  /**< Number entity info */
    ESPHOME_MSG_NUMBER_STATE                = 50,  /**< Number state update */
    ESPHOME_MSG_NUMBER_COMMAND              = 51,  /**< Number command */

    /* Select entity (52-54) */
    ESPHOME_MSG_LIST_ENTITIES_SELECT        = 52,  /**< Select entity info */
    ESPHOME_MSG_SELECT_STATE                = 53,  /**< Select state update */
    ESPHOME_MSG_SELECT_COMMAND              = 54,  /**< Select command */

    /* Lock entity (58-60 per api.proto; 55-57 are Siren) */
    ESPHOME_MSG_LIST_ENTITIES_LOCK          = 58,  /**< Lock entity info */
    ESPHOME_MSG_LOCK_STATE                  = 59,  /**< Lock state update */
    ESPHOME_MSG_LOCK_COMMAND                = 60,  /**< Lock command */

    /* Button entity (61-62) */
    ESPHOME_MSG_LIST_ENTITIES_BUTTON        = 61,  /**< Button entity info */
    ESPHOME_MSG_BUTTON_COMMAND              = 62,  /**< Button press command */

    /* Media player entity (63-65) - reserved */
    ESPHOME_MSG_LIST_ENTITIES_MEDIA_PLAYER  = 63,  /**< Media player entity info */
    ESPHOME_MSG_MEDIA_PLAYER_STATE          = 64,  /**< Media player state update */
    ESPHOME_MSG_MEDIA_PLAYER_COMMAND        = 65,  /**< Media player command */

    /* Bluetooth proxy (66-93, 126-127) - ESPHome API spec per api.proto */
    ESPHOME_MSG_SUBSCRIBE_BLE_ADVERTISEMENTS   = 66,  /**< Subscribe to BLE advertisements */
    ESPHOME_MSG_BLE_ADVERTISEMENT_RESPONSE     = 67,  /**< BLE advertisement response */
    ESPHOME_MSG_BLE_DEVICE_REQUEST             = 68,  /**< BLE device connect/disconnect/pair */
    ESPHOME_MSG_BLE_DEVICE_CONNECTION_RESP     = 69,  /**< BLE device connection response */
    ESPHOME_MSG_BLE_GATT_GET_SERVICES_REQ      = 70,  /**< BLE GATT get services request */
    ESPHOME_MSG_BLE_GATT_GET_SERVICES_RESP     = 71,  /**< BLE GATT get services response */
    ESPHOME_MSG_BLE_GATT_SERVICES_DONE         = 72,  /**< BLE GATT services enumeration done */
    ESPHOME_MSG_BLE_GATT_READ_REQUEST          = 73,  /**< BLE GATT read request */
    ESPHOME_MSG_BLE_GATT_READ_RESPONSE         = 74,  /**< BLE GATT read response */
    ESPHOME_MSG_BLE_GATT_WRITE_REQUEST         = 75,  /**< BLE GATT write request */
    ESPHOME_MSG_BLE_GATT_READ_DESCRIPTOR_REQ   = 76,  /**< BLE GATT read descriptor request */
    ESPHOME_MSG_BLE_GATT_WRITE_DESCRIPTOR_REQ  = 77,  /**< BLE GATT write descriptor request */
    ESPHOME_MSG_BLE_GATT_NOTIFY_REQUEST        = 78,  /**< BLE GATT notify subscribe request */
    ESPHOME_MSG_BLE_GATT_NOTIFY_DATA_RESP      = 79,  /**< BLE GATT notify data response */
    ESPHOME_MSG_SUBSCRIBE_BLE_CONNECTIONS_FREE = 80,   /**< Subscribe to BLE connections free */
    ESPHOME_MSG_BLE_CONNECTIONS_FREE_RESP      = 81,   /**< BLE connections free response */
    ESPHOME_MSG_BLE_GATT_ERROR_RESPONSE        = 82,   /**< BLE GATT error response */
    ESPHOME_MSG_BLE_GATT_WRITE_RESPONSE        = 83,   /**< BLE GATT write response */
    ESPHOME_MSG_BLE_GATT_NOTIFY_RESPONSE       = 84,   /**< BLE GATT notify response */
    ESPHOME_MSG_BLE_DEVICE_PAIRING_RESPONSE    = 85,   /**< BLE device pairing response */
    ESPHOME_MSG_BLE_DEVICE_UNPAIRING_RESPONSE  = 86,   /**< BLE device unpairing response */
    ESPHOME_MSG_UNSUBSCRIBE_BLE_ADVERTISEMENTS = 87,   /**< Unsubscribe BLE advertisements */
    ESPHOME_MSG_BLE_DEVICE_CLEAR_CACHE_RESP    = 88,   /**< BLE device clear cache response */
    ESPHOME_MSG_BLE_RAW_ADVERTISEMENTS_RESP    = 93,   /**< BLE raw advertisements response */

    /* Alarm control panel entity (94-96 per api.proto) */
    ESPHOME_MSG_LIST_ENTITIES_ALARM_PANEL      = 94,   /**< Alarm panel entity info */
    ESPHOME_MSG_ALARM_PANEL_STATE              = 95,   /**< Alarm panel state update */
    ESPHOME_MSG_ALARM_PANEL_COMMAND            = 96,   /**< Alarm panel command */

    /* Text entity (97-99 per api.proto) */
    ESPHOME_MSG_LIST_ENTITIES_TEXT              = 97,   /**< Text entity info */
    ESPHOME_MSG_TEXT_STATE                      = 98,   /**< Text state update */
    ESPHOME_MSG_TEXT_COMMAND                    = 99,   /**< Text command */

    /* BLE Scanner state (126-127) */
    ESPHOME_MSG_BLE_SCANNER_STATE_RESPONSE     = 126,  /**< BLE scanner state response */
    ESPHOME_MSG_BLE_SCANNER_SET_MODE_REQUEST   = 127,  /**< BLE scanner set mode request */
} esphome_msg_type_t;

/* ============================================================================
 * BLE Proxy Feature Flags (DeviceInfoResponse field 15)
 * ============================================================================ */

/** @brief BLE proxy supports passive scanning */
#define ESPHOME_BLE_PROXY_FEATURE_PASSIVE_SCAN       (1 << 0)
/** @brief BLE proxy supports active scanning */
#define ESPHOME_BLE_PROXY_FEATURE_ACTIVE_SCAN        (1 << 1)
/** @brief BLE proxy supports remote GATT caching */
#define ESPHOME_BLE_PROXY_FEATURE_REMOTE_CACHING     (1 << 2)
/** @brief BLE proxy supports pairing */
#define ESPHOME_BLE_PROXY_FEATURE_PAIRING            (1 << 3)
/** @brief BLE proxy supports GATT cache clearing */
#define ESPHOME_BLE_PROXY_FEATURE_CACHE_CLEARING     (1 << 4)
/** @brief BLE proxy supports raw advertisements */
#define ESPHOME_BLE_PROXY_FEATURE_RAW_ADVERTISEMENTS (1 << 5)
/** @brief BLE proxy supports scanner state reporting */
#define ESPHOME_BLE_PROXY_FEATURE_SCANNER_STATE      (1 << 6)

/* ============================================================================
 * Entity Types
 * ============================================================================ */

/**
 * @brief Entity type enumeration
 */
typedef enum {
    ESPHOME_ENTITY_SENSOR,           /**< Numeric sensor */
    ESPHOME_ENTITY_BINARY_SENSOR,    /**< Binary (on/off) sensor */
    ESPHOME_ENTITY_SWITCH,           /**< Controllable switch */
    ESPHOME_ENTITY_LIGHT,            /**< Light entity */
    ESPHOME_ENTITY_COVER,            /**< Cover entity (blinds, etc.) */
    ESPHOME_ENTITY_FAN,              /**< Fan entity */
    ESPHOME_ENTITY_TEXT_SENSOR,      /**< Text sensor */
    ESPHOME_ENTITY_NUMBER,           /**< Number entity (configurable numeric) */
    ESPHOME_ENTITY_BUTTON,           /**< Button entity (press action) */
    ESPHOME_ENTITY_SELECT,           /**< Select entity (dropdown) */
    ESPHOME_ENTITY_CLIMATE,          /**< Climate/thermostat entity */
    ESPHOME_ENTITY_LOCK,             /**< Lock entity (door locks) */
    ESPHOME_ENTITY_MEDIA_PLAYER,     /**< Media player entity */
    ESPHOME_ENTITY_ALARM_PANEL,      /**< Alarm control panel entity */
    ESPHOME_ENTITY_TEXT,             /**< Text input entity */
    ESPHOME_ENTITY_MAX               /**< Maximum entity type value */
} esphome_entity_type_t;

/* ============================================================================
 * Number Entity Mode
 * ============================================================================ */

/**
 * @brief Number entity display mode
 */
typedef enum {
    ESPHOME_NUMBER_MODE_AUTO = 0,    /**< Auto-select display mode */
    ESPHOME_NUMBER_MODE_BOX = 1,     /**< Input box */
    ESPHOME_NUMBER_MODE_SLIDER = 2,  /**< Slider */
} esphome_number_mode_t;

/* ============================================================================
 * Button Device Classes
 * ============================================================================ */

/**
 * @brief Button device class enumeration
 */
typedef enum {
    ESPHOME_BUTTON_CLASS_NONE = 0,
    ESPHOME_BUTTON_CLASS_RESTART,
    ESPHOME_BUTTON_CLASS_UPDATE,
    ESPHOME_BUTTON_CLASS_IDENTIFY,
} esphome_button_device_class_t;

/* ============================================================================
 * Cover Device Classes
 * ============================================================================ */

/**
 * @brief Cover device class enumeration
 */
typedef enum {
    ESPHOME_COVER_CLASS_NONE = 0,
    ESPHOME_COVER_CLASS_AWNING,
    ESPHOME_COVER_CLASS_BLIND,
    ESPHOME_COVER_CLASS_CURTAIN,
    ESPHOME_COVER_CLASS_DAMPER,
    ESPHOME_COVER_CLASS_DOOR,
    ESPHOME_COVER_CLASS_GARAGE,
    ESPHOME_COVER_CLASS_GATE,
    ESPHOME_COVER_CLASS_SHADE,
    ESPHOME_COVER_CLASS_SHUTTER,
    ESPHOME_COVER_CLASS_WINDOW,
} esphome_cover_device_class_t;

/**
 * @brief Cover operation state
 */
typedef enum {
    ESPHOME_COVER_OPERATION_IDLE = 0,
    ESPHOME_COVER_OPERATION_OPENING = 1,
    ESPHOME_COVER_OPERATION_CLOSING = 2,
} esphome_cover_operation_t;

/* ============================================================================
 * Fan Direction and Speed
 * ============================================================================ */

/**
 * @brief Fan direction enumeration
 */
typedef enum {
    ESPHOME_FAN_DIRECTION_FORWARD = 0,
    ESPHOME_FAN_DIRECTION_REVERSE = 1,
} esphome_fan_direction_t;

/* ============================================================================
 * Light Color Modes
 * ============================================================================ */

/**
 * @brief Light color mode flags (bitmask)
 */
typedef enum {
    ESPHOME_COLOR_MODE_UNKNOWN         = 0,
    ESPHOME_COLOR_MODE_ON_OFF          = (1 << 0),
    ESPHOME_COLOR_MODE_BRIGHTNESS      = (1 << 1),
    ESPHOME_COLOR_MODE_WHITE           = (1 << 2),
    ESPHOME_COLOR_MODE_COLOR_TEMP      = (1 << 3),
    ESPHOME_COLOR_MODE_COLD_WARM_WHITE = (1 << 4),
    ESPHOME_COLOR_MODE_RGB             = (1 << 5),
    ESPHOME_COLOR_MODE_RGB_WHITE       = (1 << 6),
    ESPHOME_COLOR_MODE_RGB_CT          = (1 << 7),
    ESPHOME_COLOR_MODE_RGB_CCT         = (1 << 8),
} esphome_color_mode_t;

/* ============================================================================
 * Climate Modes
 * ============================================================================ */

/**
 * @brief Climate operating mode
 */
typedef enum {
    ESPHOME_CLIMATE_MODE_OFF = 0,
    ESPHOME_CLIMATE_MODE_HEAT_COOL = 1,
    ESPHOME_CLIMATE_MODE_COOL = 2,
    ESPHOME_CLIMATE_MODE_HEAT = 3,
    ESPHOME_CLIMATE_MODE_FAN_ONLY = 4,
    ESPHOME_CLIMATE_MODE_DRY = 5,
    ESPHOME_CLIMATE_MODE_AUTO = 6,
} esphome_climate_mode_t;

/**
 * @brief Climate action (current state)
 */
typedef enum {
    ESPHOME_CLIMATE_ACTION_OFF = 0,
    ESPHOME_CLIMATE_ACTION_COOLING = 2,
    ESPHOME_CLIMATE_ACTION_HEATING = 3,
    ESPHOME_CLIMATE_ACTION_IDLE = 4,
    ESPHOME_CLIMATE_ACTION_DRYING = 5,
    ESPHOME_CLIMATE_ACTION_FAN = 6,
} esphome_climate_action_t;

/**
 * @brief Climate fan mode
 */
typedef enum {
    ESPHOME_CLIMATE_FAN_ON = 0,
    ESPHOME_CLIMATE_FAN_OFF = 1,
    ESPHOME_CLIMATE_FAN_AUTO = 2,
    ESPHOME_CLIMATE_FAN_LOW = 3,
    ESPHOME_CLIMATE_FAN_MEDIUM = 4,
    ESPHOME_CLIMATE_FAN_HIGH = 5,
    ESPHOME_CLIMATE_FAN_MIDDLE = 6,
    ESPHOME_CLIMATE_FAN_FOCUS = 7,
    ESPHOME_CLIMATE_FAN_DIFFUSE = 8,
    ESPHOME_CLIMATE_FAN_QUIET = 9,
} esphome_climate_fan_mode_t;

/**
 * @brief Climate swing mode
 */
typedef enum {
    ESPHOME_CLIMATE_SWING_OFF = 0,
    ESPHOME_CLIMATE_SWING_BOTH = 1,
    ESPHOME_CLIMATE_SWING_VERTICAL = 2,
    ESPHOME_CLIMATE_SWING_HORIZONTAL = 3,
} esphome_climate_swing_mode_t;

/**
 * @brief Climate preset
 */
typedef enum {
    ESPHOME_CLIMATE_PRESET_NONE = 0,
    ESPHOME_CLIMATE_PRESET_HOME = 1,
    ESPHOME_CLIMATE_PRESET_AWAY = 2,
    ESPHOME_CLIMATE_PRESET_BOOST = 3,
    ESPHOME_CLIMATE_PRESET_COMFORT = 4,
    ESPHOME_CLIMATE_PRESET_ECO = 5,
    ESPHOME_CLIMATE_PRESET_SLEEP = 6,
    ESPHOME_CLIMATE_PRESET_ACTIVITY = 7,
} esphome_climate_preset_t;

/* ============================================================================
 * Lock Entity Types
 * ============================================================================ */

/**
 * @brief Lock state enumeration
 */
typedef enum {
    ESPHOME_LOCK_STATE_NONE = 0,
    ESPHOME_LOCK_STATE_LOCKED = 1,
    ESPHOME_LOCK_STATE_UNLOCKED = 2,
    ESPHOME_LOCK_STATE_JAMMED = 3,
    ESPHOME_LOCK_STATE_LOCKING = 4,
    ESPHOME_LOCK_STATE_UNLOCKING = 5,
} esphome_lock_state_t;

/**
 * @brief Lock command enumeration
 */
typedef enum {
    ESPHOME_LOCK_COMMAND_UNLOCK = 0,
    ESPHOME_LOCK_COMMAND_LOCK = 1,
    ESPHOME_LOCK_COMMAND_OPEN = 2,
} esphome_lock_command_t;

/* ============================================================================
 * Media Player Entity Types
 * ============================================================================ */

/**
 * @brief Media player state enumeration
 */
typedef enum {
    ESPHOME_MEDIA_PLAYER_STATE_NONE = 0,
    ESPHOME_MEDIA_PLAYER_STATE_IDLE = 1,
    ESPHOME_MEDIA_PLAYER_STATE_PLAYING = 2,
    ESPHOME_MEDIA_PLAYER_STATE_PAUSED = 3,
} esphome_media_player_state_t;

/**
 * @brief Media player command enumeration
 */
typedef enum {
    ESPHOME_MEDIA_PLAYER_CMD_PLAY = 0,
    ESPHOME_MEDIA_PLAYER_CMD_PAUSE = 1,
    ESPHOME_MEDIA_PLAYER_CMD_STOP = 2,
    ESPHOME_MEDIA_PLAYER_CMD_MUTE = 3,
    ESPHOME_MEDIA_PLAYER_CMD_UNMUTE = 4,
} esphome_media_player_command_t;

/* ============================================================================
 * Alarm Control Panel Entity Types
 * ============================================================================ */

/**
 * @brief Alarm control panel state enumeration
 */
typedef enum {
    ESPHOME_ALARM_STATE_DISARMED = 0,
    ESPHOME_ALARM_STATE_ARMED_HOME = 1,
    ESPHOME_ALARM_STATE_ARMED_AWAY = 2,
    ESPHOME_ALARM_STATE_ARMED_NIGHT = 3,
    ESPHOME_ALARM_STATE_ARMED_VACATION = 4,
    ESPHOME_ALARM_STATE_ARMED_CUSTOM_BYPASS = 5,
    ESPHOME_ALARM_STATE_PENDING = 6,
    ESPHOME_ALARM_STATE_ARMING = 7,
    ESPHOME_ALARM_STATE_DISARMING = 8,
    ESPHOME_ALARM_STATE_TRIGGERED = 9,
} esphome_alarm_state_t;

/**
 * @brief Alarm control panel command enumeration
 */
typedef enum {
    ESPHOME_ALARM_CMD_DISARM = 0,
    ESPHOME_ALARM_CMD_ARM_AWAY = 1,
    ESPHOME_ALARM_CMD_ARM_HOME = 2,
    ESPHOME_ALARM_CMD_ARM_NIGHT = 3,
    ESPHOME_ALARM_CMD_ARM_VACATION = 4,
    ESPHOME_ALARM_CMD_ARM_CUSTOM_BYPASS = 5,
    ESPHOME_ALARM_CMD_TRIGGER = 6,
} esphome_alarm_command_t;

/* ============================================================================
 * Text Entity Types
 * ============================================================================ */

/**
 * @brief Text entity display mode
 */
typedef enum {
    ESPHOME_TEXT_MODE_TEXT = 0,
    ESPHOME_TEXT_MODE_PASSWORD = 1,
} esphome_text_mode_t;

/* ============================================================================
 * Log Levels
 * ============================================================================ */

/**
 * @brief ESPHome log level enumeration
 */
typedef enum {
    ESPHOME_LOG_LEVEL_NONE = 0,
    ESPHOME_LOG_LEVEL_ERROR = 1,
    ESPHOME_LOG_LEVEL_WARN = 2,
    ESPHOME_LOG_LEVEL_INFO = 3,
    ESPHOME_LOG_LEVEL_CONFIG = 4,
    ESPHOME_LOG_LEVEL_DEBUG = 5,
    ESPHOME_LOG_LEVEL_VERBOSE = 6,
    ESPHOME_LOG_LEVEL_VERY_VERBOSE = 7,
} esphome_log_level_t;

/* ============================================================================
 * Service Argument Types
 * ============================================================================ */

/**
 * @brief Service argument type enumeration
 */
typedef enum {
    ESPHOME_SERVICE_ARG_BOOL = 0,
    ESPHOME_SERVICE_ARG_INT = 1,
    ESPHOME_SERVICE_ARG_FLOAT = 2,
    ESPHOME_SERVICE_ARG_STRING = 3,
    ESPHOME_SERVICE_ARG_BOOL_ARRAY = 4,
    ESPHOME_SERVICE_ARG_INT_ARRAY = 5,
    ESPHOME_SERVICE_ARG_FLOAT_ARRAY = 6,
    ESPHOME_SERVICE_ARG_STRING_ARRAY = 7,
} esphome_service_arg_type_t;

/* ============================================================================
 * Sensor Device Classes (for Home Assistant compatibility)
 * ============================================================================ */

/**
 * @brief Sensor device class enumeration
 */
typedef enum {
    ESPHOME_SENSOR_CLASS_NONE = 0,
    ESPHOME_SENSOR_CLASS_TEMPERATURE,
    ESPHOME_SENSOR_CLASS_HUMIDITY,
    ESPHOME_SENSOR_CLASS_PRESSURE,
    ESPHOME_SENSOR_CLASS_POWER,
    ESPHOME_SENSOR_CLASS_ENERGY,
    ESPHOME_SENSOR_CLASS_VOLTAGE,
    ESPHOME_SENSOR_CLASS_CURRENT,
    ESPHOME_SENSOR_CLASS_BATTERY,
    ESPHOME_SENSOR_CLASS_ILLUMINANCE,
    ESPHOME_SENSOR_CLASS_SIGNAL_STRENGTH,
    ESPHOME_SENSOR_CLASS_TIMESTAMP,
    ESPHOME_SENSOR_CLASS_CARBON_DIOXIDE,
    ESPHOME_SENSOR_CLASS_VOLATILE_ORGANIC_COMPOUNDS,
    ESPHOME_SENSOR_CLASS_PM25,
    ESPHOME_SENSOR_CLASS_MOISTURE,
    ESPHOME_SENSOR_CLASS_DISTANCE,
} esphome_sensor_device_class_t;

/**
 * @brief Binary sensor device class enumeration
 */
typedef enum {
    ESPHOME_BINARY_CLASS_NONE = 0,
    ESPHOME_BINARY_CLASS_BATTERY,
    ESPHOME_BINARY_CLASS_COLD,
    ESPHOME_BINARY_CLASS_CONNECTIVITY,
    ESPHOME_BINARY_CLASS_DOOR,
    ESPHOME_BINARY_CLASS_GARAGE_DOOR,
    ESPHOME_BINARY_CLASS_GAS,
    ESPHOME_BINARY_CLASS_HEAT,
    ESPHOME_BINARY_CLASS_LIGHT,
    ESPHOME_BINARY_CLASS_LOCK,
    ESPHOME_BINARY_CLASS_MOISTURE,
    ESPHOME_BINARY_CLASS_MOTION,
    ESPHOME_BINARY_CLASS_MOVING,
    ESPHOME_BINARY_CLASS_OCCUPANCY,
    ESPHOME_BINARY_CLASS_OPENING,
    ESPHOME_BINARY_CLASS_PLUG,
    ESPHOME_BINARY_CLASS_POWER,
    ESPHOME_BINARY_CLASS_PRESENCE,
    ESPHOME_BINARY_CLASS_PROBLEM,
    ESPHOME_BINARY_CLASS_SAFETY,
    ESPHOME_BINARY_CLASS_SMOKE,
    ESPHOME_BINARY_CLASS_SOUND,
    ESPHOME_BINARY_CLASS_VIBRATION,
    ESPHOME_BINARY_CLASS_WINDOW,
    ESPHOME_BINARY_CLASS_TAMPER,
    ESPHOME_BINARY_CLASS_BATTERY_LOW,
} esphome_binary_device_class_t;

/* ============================================================================
 * State Accuracy Classes
 * ============================================================================ */

/**
 * @brief Sensor state accuracy class
 */
typedef enum {
    ESPHOME_STATE_CLASS_NONE = 0,
    ESPHOME_STATE_CLASS_MEASUREMENT,      /**< Current measurement */
    ESPHOME_STATE_CLASS_TOTAL,            /**< Total (e.g., energy) */
    ESPHOME_STATE_CLASS_TOTAL_INCREASING, /**< Monotonically increasing total */
} esphome_state_class_t;

/* ============================================================================
 * Connection State
 * ============================================================================ */

/**
 * @brief Client connection state
 */
typedef enum {
    ESPHOME_CLIENT_DISCONNECTED,    /**< Not connected */
    ESPHOME_CLIENT_CONNECTED,       /**< TCP connected, not authenticated */
    ESPHOME_CLIENT_HELLO_RECEIVED,  /**< Hello received, waiting for connect */
    ESPHOME_CLIENT_AUTHENTICATED,   /**< Fully authenticated */
} esphome_client_state_t;

/* ============================================================================
 * Common Structures
 * ============================================================================ */

/**
 * @brief Entity key type (unique identifier)
 */
typedef uint32_t esphome_entity_key_t;

/**
 * @brief Entity base information
 */
typedef struct {
    esphome_entity_key_t key;                           /**< Unique entity key */
    char name[ESPHOME_MAX_NAME_LEN];                    /**< Display name */
    char unique_id[ESPHOME_MAX_UNIQUE_ID_LEN];          /**< Unique ID for HA */
    char icon[ESPHOME_MAX_ICON_LEN];                    /**< MDI icon (e.g., "mdi:lightbulb") */
    esphome_entity_type_t type;                         /**< Entity type */
    bool disabled_by_default;                           /**< Hidden by default */
} esphome_entity_base_t;

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define ESPHOME_ERR_BASE                0x10000
#define ESPHOME_ERR_INVALID_MESSAGE     (ESPHOME_ERR_BASE + 1)
#define ESPHOME_ERR_BUFFER_OVERFLOW     (ESPHOME_ERR_BASE + 2)
#define ESPHOME_ERR_AUTHENTICATION      (ESPHOME_ERR_BASE + 3)
#define ESPHOME_ERR_NOT_CONNECTED       (ESPHOME_ERR_BASE + 4)
#define ESPHOME_ERR_MAX_CLIENTS         (ESPHOME_ERR_BASE + 5)
#define ESPHOME_ERR_ENTITY_NOT_FOUND    (ESPHOME_ERR_BASE + 6)
#define ESPHOME_ERR_MAX_ENTITIES        (ESPHOME_ERR_BASE + 7)
#define ESPHOME_ERR_INVALID_TYPE        (ESPHOME_ERR_BASE + 8)
#define ESPHOME_ERR_ENCRYPTION          (ESPHOME_ERR_BASE + 9)  /**< Encryption error */
#define ESPHOME_ERR_HANDSHAKE           (ESPHOME_ERR_BASE + 10) /**< Handshake failed */

/* ============================================================================
 * Noise Encryption Frame Types
 * ============================================================================ */

/** @brief Frame indicator: plaintext (varint length encoding) */
#define ESPHOME_FRAME_PLAINTEXT         0x00

/** @brief Frame indicator: Noise protocol (Hello, handshake, encrypted data; 2-byte BE length) */
#define ESPHOME_FRAME_NOISE             0x01

/** @brief Noise frame header size (1-byte indicator + 2-byte BE length) */
#define ESPHOME_NOISE_FRAME_HEADER_SIZE 3

/** @brief Noise encryption overhead (1-byte indicator + 2-byte length + 16-byte MAC) */
#define ESPHOME_NOISE_FRAME_OVERHEAD    (ESPHOME_NOISE_FRAME_HEADER_SIZE + 16)

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_COMMON_H */
