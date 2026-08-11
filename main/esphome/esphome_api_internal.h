/**
 * @file esphome_api_internal.h
 * @brief Internal declarations shared between ESPHome API modules
 *
 * This header contains internal types, structures, and function declarations
 * that are shared between esphome_api.c, esphome_api_server.c, and
 * esphome_api_handlers.c. This is not part of the public API.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_API_INTERNAL_H
#define ESPHOME_API_INTERNAL_H

#include "esphome_api.h"
#include "esphome_protocol.h"
#include "esphome_entities.h"
#include "esphome_common.h"
#include "sdkconfig.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "utils/freertos_helpers.h"  /* For PSRAM task support */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Task Configuration
 * ============================================================================ */

/** @brief Server task stack size */
#define ESPHOME_SERVER_TASK_STACK   4096

/** @brief Server task priority */
#define ESPHOME_SERVER_TASK_PRIO    4

/** @brief Client task stack size (PSRAM-backed, needs headroom for Noise crypto) */
#define ESPHOME_CLIENT_TASK_STACK   8192

/** @brief Client task priority */
#define ESPHOME_CLIENT_TASK_PRIO    4

/* ============================================================================
 * Event Bits
 * ============================================================================ */

/** @brief Event bit for stopping the server */
#define EVENT_STOP_SERVER           BIT0

/** @brief Event bit for client disconnect */
#define EVENT_CLIENT_DISCONNECT     BIT1

/* ============================================================================
 * Forward Declarations for Noise Encryption
 * ============================================================================ */

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
/* Forward declaration - actual type defined in esphome_noise.h */
struct esphome_noise_ctx;
typedef struct esphome_noise_ctx esphome_noise_ctx_t;
#endif

/* ============================================================================
 * Client Structure
 * ============================================================================ */

/**
 * @brief Client connection state structure
 *
 * Contains all state for a single connected client, including socket,
 * authentication state, subscriptions, and buffers.
 */
typedef struct {
    int socket;                                     /**< Client socket (-1 if unused) */
    /** Set by another task to ask this client's own handler to hang up.
     *
     * The socket must only ever be closed by the task that reads from it.
     * close() on a descriptor another task is blocked in recv() on corrupts
     * lwIP's internal queues — it showed up as a load access fault inside
     * xQueueGenericSend() on the tcpip thread. */
    volatile bool disconnect_requested;
    esphome_client_state_t state;                   /**< Connection state */
    bool subscribed_states;                         /**< Subscribed to state updates */
    bool subscribed_logs;                           /**< Subscribed to log messages */
    bool subscribed_ha_states;                      /**< Subscribed to HA state updates */
    esphome_log_level_t log_level;                  /**< Subscribed log level (min level) */
    uint32_t last_activity;                         /**< Last activity timestamp */
    uint32_t last_ping_sent;                        /**< Timestamp of last ping sent */
    uint32_t last_pong_received;                    /**< Timestamp of last pong received */
    bool ping_pending;                              /**< Waiting for pong response */
    char client_info[ESPHOME_STRING_BUFFER_MEDIUM]; /**< Client identification string */
    psram_task_handle_t psram_task;                 /**< PSRAM-backed client handler task */
    uint8_t rx_buffer[ESPHOME_RX_BUFFER_SIZE];      /**< Receive buffer */
    size_t rx_buffer_len;                           /**< Bytes in receive buffer */
#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
    esphome_noise_ctx_t *noise_ctx;                 /**< Noise encryption context */
    bool encryption_enabled;                        /**< True if encryption is active */
    bool require_encryption_disconnect;             /**< Disconnect after HelloResponse (plaintext client) */
#endif
} esphome_client_t;

/* ============================================================================
 * Module State Structure
 * ============================================================================ */

/**
 * @brief Global API server state
 *
 * This structure contains all state for the ESPHome API server.
 * It is defined in esphome_api.c and accessed via esphome_api_get_state().
 */
typedef struct {
    esphome_api_config_t config;                    /**< Server configuration */
    esphome_api_stats_t stats;                      /**< Server statistics */
    /** Client connection array, allocated at init in PSRAM
     *
     * Roughly 1.2 KB per slot, almost all of it the receive buffer, and four
     * slots put it well past 4 KB of internal RAM that never needed to be
     * internal — nothing here is touched from an ISR. Kept as a pointer rather
     * than moving the whole s_api struct: esphome_api_get_state() hands out
     * &s_api to ten call sites that dereference it without a null check, and a
     * struct that can be null before init is exactly the crash this project
     * already had once with the entity table. Indexing is unchanged. */
    esphome_client_t *clients;
    int server_socket;                              /**< Server listening socket */
    TaskHandle_t server_task;                       /**< Server accept task handle (for API compat) */
    psram_task_handle_t server_psram_task;          /**< PSRAM-backed server task resources */
    SemaphoreHandle_t mutex;                        /**< Mutex for thread safety */
    EventGroupHandle_t event_group;                 /**< Event group for signaling */
    esp_timer_handle_t uptime_timer;                /**< Uptime tracking timer (1s) */
    esp_timer_handle_t stats_timer;                 /**< System stats update timer (60s) */
    esphome_api_connection_cb_t connection_callback;/**< Connection state callback */
    esphome_ha_state_cb_t ha_state_callback;        /**< HA state update callback */
    char device_version[ESPHOME_STRING_BUFFER_SHORT]; /**< Device firmware version */
    char device_model[ESPHOME_STRING_BUFFER_SHORT]; /**< Device model string */
    bool initialized;                               /**< True if initialized */
    bool running;                                   /**< True if server is running */
} esphome_api_state_t;

/* ============================================================================
 * State Access Functions
 * ============================================================================ */

/**
 * @brief Get pointer to the global API state
 *
 * @return Pointer to the global esphome_api_state_t structure
 */
esphome_api_state_t *esphome_api_get_state(void);

/* ============================================================================
 * Internal Helper Functions (esphome_api.c)
 * ============================================================================ */

/**
 * @brief Get current timestamp in milliseconds
 *
 * @return Timestamp in milliseconds since boot
 */
uint32_t esphome_api_get_timestamp_ms(void);

/**
 * @brief Get MAC address as a string
 *
 * @param[out] mac_str Buffer to store MAC string (at least 18 bytes)
 * @param[in] len Length of mac_str buffer
 */
void esphome_api_get_mac_address(char *mac_str, size_t len);

/**
 * @brief Count active (connected) clients
 *
 * @return Number of currently connected clients
 */
uint8_t esphome_api_count_active_clients(void);

/**
 * @brief Find a free client slot
 *
 * @return Index of free slot, or -1 if all slots are in use
 */
int esphome_api_find_free_client_slot(void);

/**
 * @brief Get client ID from client pointer
 *
 * @param[in] client Pointer to client structure
 * @return Client ID (0-based index)
 */
uint8_t esphome_api_get_client_index(esphome_client_t *client);

/**
 * @brief Close a client connection
 *
 * Closes the socket, clears state, and notifies callbacks.
 *
 * @param[in] client Pointer to client structure
 * @param[in] client_id Client identifier
 */
void esphome_api_close_client(esphome_client_t *client, uint8_t client_id);

/* ============================================================================
 * Message Sending Functions (esphome_api_server.c)
 * ============================================================================ */

/**
 * @brief Send raw bytes to a client
 *
 * Low-level send with retry handling for partial sends and EAGAIN.
 *
 * @param[in] client Client to send to
 * @param[in] data Data buffer
 * @param[in] len Data length
 * @return ESP_OK on success
 */
esp_err_t esphome_api_send_raw_bytes(esphome_client_t *client, const uint8_t *data, size_t len);

/**
 * @brief Send a message to a client
 *
 * Handles encryption if enabled, updates statistics.
 *
 * @param[in] client Client to send to
 * @param[in] data Message data (complete ESPHome frame)
 * @param[in] len Message length
 * @return ESP_OK on success
 */
esp_err_t esphome_api_send_message(esphome_client_t *client, const uint8_t *data, size_t len);

/**
 * @brief Send a ping request to client
 *
 * @param[in] client Client to ping
 * @return ESP_OK on success
 */
esp_err_t esphome_api_send_ping_request(esphome_client_t *client);

/**
 * @brief Send data to client by ID
 *
 * Used by BLE Proxy to send responses to specific clients.
 *
 * @param[in] client_id Client ID (0 to max_clients-1)
 * @param[in] data Data to send
 * @param[in] len Data length
 * @return ESP_OK on success
 */
esp_err_t esphome_api_send_to_client(uint8_t client_id, const uint8_t *data, size_t len);

/**
 * @brief Check and handle client keepalive
 *
 * @param[in] client Client to check
 * @return true if client should be disconnected due to timeout
 */
bool esphome_api_check_client_keepalive(esphome_client_t *client);

/* ============================================================================
 * Server Task Functions (esphome_api_server.c)
 * ============================================================================ */

/**
 * @brief Server accept task
 *
 * Main task that accepts incoming connections and spawns client handlers.
 *
 * @param[in] pvParameters Task parameters (unused)
 */
void esphome_api_server_task(void *pvParameters);

/**
 * @brief Client handler task
 *
 * Task that handles a single client connection.
 *
 * @param[in] pvParameters Client ID (as uintptr_t)
 */
void esphome_api_client_task(void *pvParameters);

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
/**
 * @brief Handle Noise protocol handshake
 *
 * @param[in] client Client performing handshake
 * @param[in] client_id Client identifier
 * @return ESP_OK on success
 */
esp_err_t esphome_api_handle_noise_handshake(esphome_client_t *client, uint8_t client_id);
#endif

/* ============================================================================
 * Message Handler Functions (esphome_api_handlers.c)
 * ============================================================================ */

/**
 * @brief Handle incoming client message
 *
 * Main message dispatch function that routes to specific handlers.
 *
 * @param[in] client Client that sent the message
 * @param[in] msg Decoded message
 * @return ESP_OK on success, or error code
 */
esp_err_t esphome_api_handle_client_message(esphome_client_t *client, const esphome_message_t *msg);

/**
 * @brief Handle ping response from client
 *
 * @param[in] client Client that sent the pong
 * @return ESP_OK on success
 */
esp_err_t esphome_api_handle_ping_response(esphome_client_t *client);

/* ============================================================================
 * Entity Enumeration Callbacks (esphome_api_handlers.c)
 * ============================================================================ */

/**
 * @brief Context for ListEntities callback
 */
typedef struct {
    esphome_client_t *client;   /**< Client to send entities to */
    esp_err_t result;           /**< Result of enumeration */
} esphome_list_entities_ctx_t;

/**
 * @brief Context for state broadcast callback
 */
typedef struct {
    esphome_client_t *client;   /**< Client to send states to */
} esphome_broadcast_states_ctx_t;

/**
 * @brief Entity enumeration callback for ListEntities
 *
 * @param[in] type Entity type
 * @param[in] key Entity key
 * @param[in] config Entity configuration
 * @param[in,out] user_data Callback context
 * @return true to continue enumeration, false to stop
 */
bool esphome_api_list_entities_callback(esphome_entity_type_t type, esphome_entity_key_t key,
                                        const void *config, void *user_data);

/**
 * @brief Entity enumeration callback for state broadcast
 *
 * @param[in] type Entity type
 * @param[in] key Entity key
 * @param[in] config Entity configuration
 * @param[in,out] user_data Callback context
 * @return true to continue enumeration
 */
bool esphome_api_broadcast_states_callback(esphome_entity_type_t type, esphome_entity_key_t key,
                                           const void *config, void *user_data);

/* ============================================================================
 * Log Streaming Functions (esphome_api.c)
 * ============================================================================ */

/**
 * @brief Encode and send a log message to a client
 *
 * @param[in] client Client to send to
 * @param[in] level Log level
 * @param[in] tag Log tag
 * @param[in] message Log message
 * @return ESP_OK on success
 */
esp_err_t esphome_api_encode_and_send_log(esphome_client_t *client, esphome_log_level_t level,
                                          const char *tag, const char *message);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_API_INTERNAL_H */
