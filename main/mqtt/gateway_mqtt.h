/**
 * @file gateway_mqtt.h
 * @brief MQTT Client with Zigbee2MQTT Compatibility
 *
 * Provides MQTT client functionality with thread-safe publish/subscribe
 * and automatic reconnection. Uses direct publish (no internal queue)
 * for predictable behavior and reduced memory usage.
 *
 * Note: Renamed from mqtt_client.h to avoid collision with ESP-IDF's mqtt_client.h
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef GATEWAY_MQTT_H
#define GATEWAY_MQTT_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MQTT Client Constants
 * ============================================================================ */

/** @brief Maximum length for broker URL */
#define MQTT_BROKER_URL_MAX_LEN             128

/** @brief Maximum length for username */
#define MQTT_USERNAME_MAX_LEN               64

/** @brief Maximum length for password */
#define MQTT_PASSWORD_MAX_LEN               64

/** @brief Maximum length for client ID */
#define MQTT_CLIENT_ID_MAX_LEN              64

/** @brief Mutex acquisition timeout (ms) */
#define MQTT_CLIENT_MUTEX_TIMEOUT_MS        100

/** @brief Publish mutex timeout (ms) */
#define MQTT_CLIENT_PUBLISH_MUTEX_TIMEOUT_MS 1000

/**
 * @brief MQTT connection states
 */
typedef enum {
    MQTT_STATE_DISCONNECTED,  /*!< Not connected to broker */
    MQTT_STATE_CONNECTING,    /*!< Attempting to connect */
    MQTT_STATE_CONNECTED,     /*!< Successfully connected */
    MQTT_STATE_ERROR          /*!< Error state */
} mqtt_state_t;

/**
 * @brief MQTT configuration structure
 */
typedef struct {
    char broker_url[MQTT_BROKER_URL_MAX_LEN];  /*!< Broker URL (mqtt://, mqtts://, ws://, wss://) */
    uint16_t port;                             /*!< Broker port (default: 1883 for mqtt, 8883 for mqtts) */
    char username[MQTT_USERNAME_MAX_LEN];      /*!< Username for authentication (optional) */
    char password[MQTT_PASSWORD_MAX_LEN];      /*!< Password for authentication (optional) */
    char client_id[MQTT_CLIENT_ID_MAX_LEN];    /*!< Client ID (must be unique per broker) */
    bool use_ssl;                              /*!< Use SSL/TLS encryption */
    uint16_t keepalive;                        /*!< Keep-alive interval in seconds */
    uint8_t qos;                               /*!< Default QoS level (0, 1, or 2) */
    bool clean_session;                        /*!< Clean session flag */
} mqtt_config_t;

/**
 * @brief MQTT message callback type
 *
 * @param[in] topic Topic of the received message
 * @param[in] data Message payload data
 * @param[in] data_len Length of the payload
 */
typedef void (*mqtt_message_callback_t)(const char *topic, const char *data, size_t data_len);

/**
 * @brief MQTT connection callback type
 *
 * @param[in] connected true if connected, false if disconnected
 */
typedef void (*mqtt_connection_callback_t)(bool connected);

/**
 * @brief MQTT statistics structure
 */
typedef struct {
    uint32_t connect_count;      /*!< Total successful connections */
    uint32_t disconnect_count;   /*!< Total disconnections */
    uint32_t reconnect_count;    /*!< Total reconnection attempts */
    uint32_t publish_count;      /*!< Total published messages */
    uint32_t publish_fail_count; /*!< Total publish failures */
    uint32_t receive_count;      /*!< Total received messages */
} mqtt_stats_t;

/**
 * @brief Initialize MQTT client
 *
 * Initializes MQTT client with specified configuration.
 * Must be called before any other MQTT operations.
 *
 * @param[in] config Pointer to MQTT configuration structure
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid configuration
 *     - ESP_ERR_NO_MEM: Out of memory
 */
esp_err_t mqtt_client_init(mqtt_config_t *config);

/**
 * @brief Start MQTT client
 *
 * Starts MQTT client and initiates connection to broker.
 * Requires WiFi to be connected first.
 *
 * @return
 *     - ESP_OK: Client started successfully
 *     - ESP_FAIL: Failed to start client
 */
esp_err_t mqtt_client_start(void);

/**
 * @brief Stop MQTT client
 *
 * Stops MQTT client and disconnects from broker.
 *
 * @return
 *     - ESP_OK: Client stopped successfully
 *     - ESP_FAIL: Failed to stop client
 */
esp_err_t mqtt_client_stop(void);

/**
 * @brief Pause MQTT client for pairing phase
 *
 * Stops the MQTT client but marks it as paused for WiFi, so it will
 * automatically restart when WiFi reconnects and gets an IP address.
 * Use this when entering PAIRING phase to cleanly close the MQTT connection
 * before WiFi is disconnected.
 *
 * @return
 *     - ESP_OK: Client paused successfully
 *     - ESP_FAIL: Failed to pause client
 */
esp_err_t mqtt_client_pause_for_pairing(void);

/**
 * @brief Publish MQTT message
 *
 * Publishes message directly to the MQTT broker. Thread-safe through
 * mutex protection. Synchronous operation - returns when publish is
 * queued in ESP-IDF's internal outbox.
 *
 * @param[in] topic Topic to publish to (null-terminated string)
 * @param[in] data Message payload data
 * @param[in] len Length of the payload (0 for strlen(data))
 * @param[in] qos QoS level (0, 1, or 2)
 * @param[in] retain Retain flag
 *
 * @return
 *     - ESP_OK: Message published successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_TIMEOUT: Failed to acquire mutex
 *     - ESP_ERR_INVALID_STATE: Client not connected
 *     - ESP_FAIL: Publish failed
 */
esp_err_t mqtt_client_publish(const char *topic, const char *data,
                              size_t len, int qos, bool retain);

/**
 * @brief Subscribe to MQTT topic
 *
 * Subscribes to specified topic with given QoS level.
 *
 * @param[in] topic Topic to subscribe to (null-terminated string)
 * @param[in] qos QoS level (0, 1, or 2)
 *
 * @return
 *     - ESP_OK: Subscribe request sent
 *     - ESP_ERR_INVALID_ARG: Invalid topic
 *     - ESP_ERR_INVALID_STATE: Client not connected
 */
esp_err_t mqtt_client_subscribe(const char *topic, int qos);

/**
 * @brief Unsubscribe from MQTT topic
 *
 * Unsubscribes from specified topic.
 *
 * @param[in] topic Topic to unsubscribe from (null-terminated string)
 *
 * @return
 *     - ESP_OK: Unsubscribe request sent
 *     - ESP_ERR_INVALID_ARG: Invalid topic
 *     - ESP_ERR_INVALID_STATE: Client not connected
 */
esp_err_t mqtt_client_unsubscribe(const char *topic);

/**
 * @brief Register message callback
 *
 * Registers callback function to be called when messages are received.
 * Only one callback can be registered at a time.
 *
 * @param[in] callback Callback function pointer
 *
 * @return
 *     - ESP_OK: Callback registered successfully
 *     - ESP_ERR_INVALID_ARG: Invalid callback
 */
esp_err_t mqtt_client_register_callback(mqtt_message_callback_t callback);

/**
 * @brief Register connection callback
 *
 * Registers callback function to be called on connection state changes.
 *
 * @param[in] callback Callback function pointer
 *
 * @return
 *     - ESP_OK: Callback registered successfully
 *     - ESP_ERR_INVALID_ARG: Invalid callback
 */
esp_err_t mqtt_client_register_connection_callback(mqtt_connection_callback_t callback);

/**
 * @brief Get MQTT client state
 *
 * Thread-safe retrieval of current MQTT state.
 *
 * @return Current MQTT state
 */
mqtt_state_t mqtt_client_get_state(void);

/**
 * @brief Check if MQTT is connected
 *
 * Convenience function to check if MQTT is in CONNECTED state.
 *
 * @return
 *     - true: MQTT is connected
 *     - false: MQTT is not connected
 */
bool mqtt_client_is_connected(void);

/**
 * @brief Get MQTT statistics
 *
 * Retrieves connection and message statistics.
 *
 * @param[out] stats Pointer to statistics structure to fill
 *
 * @return
 *     - ESP_OK: Statistics retrieved successfully
 *     - ESP_ERR_INVALID_ARG: Invalid stats pointer
 */
esp_err_t mqtt_client_get_stats(mqtt_stats_t *stats);

/**
 * @brief Publish a null-terminated string (auto-calculates length)
 *
 * Convenience wrapper around mqtt_client_publish() that automatically
 * calculates the string length. Reduces code duplication where strlen()
 * is called manually.
 *
 * @param[in] topic Topic to publish to (null-terminated string)
 * @param[in] payload Message payload as null-terminated string
 * @param[in] qos QoS level (0, 1, or 2)
 * @param[in] retain Retain flag
 *
 * @return
 *     - ESP_OK: Message queued successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NO_MEM: Queue full
 *     - ESP_ERR_INVALID_STATE: Client not connected
 */
esp_err_t mqtt_client_publish_string(const char *topic, const char *payload, int qos, bool retain);

/**
 * @brief Publish a cJSON object (serializes and frees the JSON)
 *
 * Convenience wrapper that serializes a cJSON object to a string,
 * publishes it, and frees the JSON memory. Reduces boilerplate code
 * for the common pattern of:
 *   char *json_str = cJSON_PrintUnformatted(json);
 *   mqtt_client_publish(topic, json_str, strlen(json_str), qos, retain);
 *   cJSON_Delete(json);
 *
 * @param[in] topic Topic to publish to (null-terminated string)
 * @param[in] json cJSON object to serialize and publish (will be freed)
 * @param[in] qos QoS level (0, 1, or 2)
 * @param[in] retain Retain flag
 *
 * @return
 *     - ESP_OK: Message queued successfully
 *     - ESP_ERR_INVALID_ARG: Invalid arguments (NULL topic or json)
 *     - ESP_ERR_NO_MEM: Queue full or JSON serialization failed
 *     - ESP_ERR_INVALID_STATE: Client not connected
 */
esp_err_t mqtt_client_publish_json(const char *topic, cJSON *json, int qos, bool retain);

/**
 * @brief Test MQTT client functionality
 *
 * Performs basic MQTT test and reports results.
 * For debugging and verification purposes.
 *
 * @return
 *     - ESP_OK: Test passed
 *     - ESP_FAIL: Test failed
 */
esp_err_t mqtt_client_test(void);

/* ============================================================================
 * MQTT Latency Measurement
 * ============================================================================ */

/** @brief Ping topic for latency measurement */
#define MQTT_PING_TOPIC                 "zigbee2mqtt/bridge/ping"

/** @brief Pong topic for latency response */
#define MQTT_PONG_TOPIC                 "zigbee2mqtt/bridge/pong"

/** @brief Latency measurement timeout (ms) */
#define MQTT_LATENCY_TIMEOUT_MS         5000

/** @brief Invalid/unknown latency value */
#define MQTT_LATENCY_UNKNOWN            0xFFFFFFFF

/**
 * @brief Start MQTT latency measurement
 *
 * Initiates a ping/pong round-trip measurement by publishing to the ping topic
 * and subscribing to the pong topic. Call mqtt_latency_get_ms() to retrieve
 * the measured latency after a short delay.
 *
 * The measurement works by:
 * 1. Recording the current timestamp
 * 2. Publishing a message to zigbee2mqtt/bridge/ping
 * 3. Waiting for the broker to echo it back (or using PUBACK timing for QoS 1)
 * 4. Calculating the round-trip time
 *
 * @return
 *     - ESP_OK: Measurement started successfully
 *     - ESP_ERR_INVALID_STATE: MQTT not connected
 */
esp_err_t mqtt_latency_measure_start(void);

/**
 * @brief Get the last measured MQTT latency
 *
 * Returns the last measured round-trip latency to the MQTT broker in milliseconds.
 *
 * @return Latency in milliseconds, or MQTT_LATENCY_UNKNOWN if not yet measured
 */
uint32_t mqtt_latency_get_ms(void);

/**
 * @brief Check if a latency measurement is in progress
 *
 * @return true if measurement is pending, false otherwise
 */
bool mqtt_latency_is_measuring(void);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_MQTT_H */
