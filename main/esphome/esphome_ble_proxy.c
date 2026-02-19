/**
 * @file esphome_ble_proxy.c
 * @brief ESPHome Bluetooth Proxy Protocol Implementation
 *
 * Implements the ESPHome Bluetooth Proxy protocol (MSG 66-78) for
 * Home Assistant integration. Bridges the ESPHome API with the
 * existing BLE scanner and GATT client modules.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_ble_proxy.h"
#include "esphome_api.h"
#include "esphome_api_internal.h"
#include "esphome_protocol.h"
#include "esphome_crypto_constants.h"
#include "../bluetooth/ble_scanner.h"
#include "../bluetooth/ble_gatt_client.h"
#include "../bluetooth/ble_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "utils/freertos_helpers.h"  /* For PSRAM task creation */
#include <string.h>

static const char *TAG = "BLE_PROXY";

/* ============================================================================
 * BF-027: BLE Advertisement Data Type Definitions
 * ============================================================================ */

/** @brief Flags (Data type value: 0x01) */
#define BLE_AD_TYPE_FLAGS                   0x01

/** @brief Incomplete List of 16-bit Service UUIDs */
#define BLE_AD_TYPE_INCOMPLETE_UUID16       0x02

/** @brief Complete List of 16-bit Service UUIDs */
#define BLE_AD_TYPE_COMPLETE_UUID16         0x03

/** @brief Incomplete List of 32-bit Service UUIDs */
#define BLE_AD_TYPE_INCOMPLETE_UUID32       0x04

/** @brief Complete List of 32-bit Service UUIDs */
#define BLE_AD_TYPE_COMPLETE_UUID32         0x05

/** @brief Incomplete List of 128-bit Service UUIDs */
#define BLE_AD_TYPE_INCOMPLETE_UUID128      0x06

/** @brief Complete List of 128-bit Service UUIDs */
#define BLE_AD_TYPE_COMPLETE_UUID128        0x07

/** @brief Shortened Local Name */
#define BLE_AD_TYPE_SHORT_NAME              0x08

/** @brief Complete Local Name */
#define BLE_AD_TYPE_COMPLETE_NAME           0x09

/** @brief TX Power Level */
#define BLE_AD_TYPE_TX_POWER_LEVEL          0x0A

/** @brief Service Data - 16-bit UUID */
#define BLE_AD_TYPE_SERVICE_DATA_16         0x16

/** @brief Service Data - 32-bit UUID */
#define BLE_AD_TYPE_SERVICE_DATA_32         0x20

/** @brief Service Data - 128-bit UUID */
#define BLE_AD_TYPE_SERVICE_DATA_128        0x21

/** @brief Manufacturer Specific Data */
#define BLE_AD_TYPE_MANUFACTURER_DATA       0xFF

/* ============================================================================
 * BF-023: Initialization Check Macro
 * ============================================================================ */

/**
 * @brief Check if BLE Proxy is initialized
 * Use at the beginning of message handlers.
 */
#define BLE_PROXY_CHECK_INITIALIZED() \
    do { \
        if (!s_proxy.initialized) { \
            ESP_LOGW(TAG, "%s: BLE Proxy not initialized", __func__); \
            return ESP_ERR_INVALID_STATE; \
        } \
    } while (0)

/* ============================================================================
 * Module State
 * ============================================================================ */

/**
 * @brief GATT connection tracking structure
 */
typedef struct {
    uint64_t address;               /**< Device address as uint64 */
    uint16_t conn_handle;           /**< NimBLE connection handle */
    uint8_t owner_client;           /**< ESPHome client that owns this connection */
    bool active;                    /**< Slot is in use */
} ble_proxy_connection_t;

/**
 * @brief Module state structure
 */
static struct {
    esphome_ble_proxy_config_t config;
    esphome_ble_proxy_stats_t stats;
    bool subscribed_clients[ESPHOME_BLE_PROXY_MAX_SUBSCRIBED_CLIENTS];
    ble_proxy_connection_t connections[ESPHOME_BLE_PROXY_MAX_CONNECTIONS];
    SemaphoreHandle_t mutex;
    QueueHandle_t adv_queue;
    psram_task_handle_t adv_psram_task;  /**< PSRAM-backed advertisement task */
    bool initialized;
    bool running;
} s_proxy = {
    .initialized = false,
    .running = false,
};

/* Forward declarations */
static void on_advertisement(const ble_adv_data_t *adv);
static void on_scanner_state_changed(ble_scanner_state_t new_state, ble_scanner_state_t old_state);
static void advertisement_task(void *pvParameters);
static esp_err_t send_advertisement_response(uint8_t client_id, const ble_adv_data_t *adv);
static esp_err_t send_device_connection_response(uint8_t client_id, uint64_t address, bool connected, int error);
static esp_err_t send_gatt_read_response(uint8_t client_id, uint64_t address, uint16_t handle,
                                          const uint8_t *data, size_t len);
static esp_err_t send_gatt_write_response(uint8_t client_id, uint64_t address, uint16_t handle, bool success);
static esp_err_t send_connections_free_response(uint8_t client_id);
static void on_gatt_connect(const uint8_t *mac, bool success, uint16_t conn_handle);
static void on_gatt_disconnect(const uint8_t *mac, uint8_t reason);
static void on_gatt_notify(uint16_t conn_handle, uint16_t attr_handle,
                           const uint8_t *data, uint16_t len);

/* ============================================================================
 * BF-024: Helper Functions for Connection Lookup
 * ============================================================================ */

/**
 * @brief Find connection slot by BLE address
 *
 * @param address Device address as uint64
 * @return Pointer to connection slot, or NULL if not found
 * @note Caller must hold s_proxy.mutex when modifying the returned slot
 */
static ble_proxy_connection_t *find_connection_by_address(uint64_t address)
{
    for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
        if (s_proxy.connections[i].active &&
            s_proxy.connections[i].address == address) {
            return &s_proxy.connections[i];
        }
    }
    return NULL;
}

/**
 * @brief Find connection slot by NimBLE connection handle
 *
 * @param conn_handle NimBLE connection handle
 * @return Pointer to connection slot, or NULL if not found
 * @note Caller must hold s_proxy.mutex when modifying the returned slot
 */
__attribute__((unused)) static ble_proxy_connection_t *find_connection_by_handle(uint16_t conn_handle)
{
    for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
        if (s_proxy.connections[i].active &&
            s_proxy.connections[i].conn_handle == conn_handle) {
            return &s_proxy.connections[i];
        }
    }
    return NULL;
}

/**
 * @brief Get connection handle for a device address
 *
 * @param address Device address as uint64
 * @return Connection handle, or BLE_GATT_INVALID_CONN_HANDLE if not connected
 */
static uint16_t __attribute__((unused)) get_conn_handle_for_address(uint64_t address)
{
    ble_proxy_connection_t *conn = find_connection_by_address(address);
    if (conn) {
        return conn->conn_handle;
    }
    return BLE_GATT_INVALID_CONN_HANDLE;
}

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================ */

esp_err_t esphome_ble_proxy_init(const esphome_ble_proxy_config_t *config)
{
    if (s_proxy.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Use default config if none provided */
    if (config) {
        s_proxy.config = *config;
    } else {
        esphome_ble_proxy_config_t default_config = ESPHOME_BLE_PROXY_CONFIG_DEFAULT();
        s_proxy.config = default_config;
    }

    /* Create mutex */
    s_proxy.mutex = xSemaphoreCreateMutex();
    if (!s_proxy.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create advertisement queue */
    s_proxy.adv_queue = xQueueCreate(ESPHOME_BLE_PROXY_ADV_QUEUE_SIZE, sizeof(ble_adv_data_t));
    if (!s_proxy.adv_queue) {
        ESP_LOGE(TAG, "Failed to create advertisement queue");
        vSemaphoreDelete(s_proxy.mutex);
        s_proxy.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Initialize connection tracking */
    memset(s_proxy.connections, 0, sizeof(s_proxy.connections));
    for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
        s_proxy.connections[i].conn_handle = BLE_GATT_INVALID_CONN_HANDLE;
    }

    /* Clear subscriptions */
    memset(s_proxy.subscribed_clients, 0, sizeof(s_proxy.subscribed_clients));

    /* Reset statistics */
    memset(&s_proxy.stats, 0, sizeof(s_proxy.stats));

    s_proxy.initialized = true;
    ESP_LOGI(TAG, "BLE Proxy initialized (RSSI filter: %d dBm)", s_proxy.config.min_rssi);

    return ESP_OK;
}

esp_err_t esphome_ble_proxy_deinit(void)
{
    if (!s_proxy.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop if running */
    if (s_proxy.running) {
        esphome_ble_proxy_stop();
    }

    /* Delete advertisement queue */
    if (s_proxy.adv_queue) {
        vQueueDelete(s_proxy.adv_queue);
        s_proxy.adv_queue = NULL;
    }

    /* Delete mutex */
    if (s_proxy.mutex) {
        vSemaphoreDelete(s_proxy.mutex);
        s_proxy.mutex = NULL;
    }

    s_proxy.initialized = false;
    ESP_LOGI(TAG, "BLE Proxy deinitialized");

    return ESP_OK;
}

esp_err_t esphome_ble_proxy_start(void)
{
    if (!s_proxy.initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_proxy.running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_ERR_INVALID_STATE;
    }

    /* Register advertisement callback with BLE scanner */
    esp_err_t ret = ble_scanner_register_callback(on_advertisement);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register scanner callback: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register scanner state change callback for HA updates */
    ble_scanner_register_state_callback(on_scanner_state_changed);

    /* Register GATT callbacks */
    ble_gatt_set_connect_cb(on_gatt_connect);
    ble_gatt_set_disconnect_cb(on_gatt_disconnect);
    ble_gatt_set_notify_cb(on_gatt_notify);

    /* Create advertisement forwarding task with PSRAM stack (saves ~4KB internal RAM) */
    esp_err_t task_ret = psram_task_create(
        advertisement_task,
        "ble_adv_fwd",
        ESPHOME_BLE_PROXY_TASK_STACK,
        NULL,
        ESPHOME_BLE_PROXY_TASK_PRIO,
        &s_proxy.adv_psram_task
    );

    if (task_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create advertisement task: %s", esp_err_to_name(task_ret));
        ble_scanner_unregister_callback(on_advertisement);
        return task_ret;
    }

    s_proxy.running = true;
    ESP_LOGI(TAG, "BLE Proxy started");

    return ESP_OK;
}

esp_err_t esphome_ble_proxy_stop(void)
{
    if (!s_proxy.running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Signal task to stop */
    s_proxy.running = false;

    /* Unregister scanner callback FIRST to stop new advertisements entering queue */
    ble_scanner_unregister_callback(on_advertisement);

    /* Wake up the task if it's blocked on the queue */
    if (s_proxy.adv_queue != NULL) {
        xQueueReset(s_proxy.adv_queue);
    }

    /* Wait for task to self-delete — but NEVER force-delete a potentially blocked task.
     * The task may be stuck inside send() on a closed socket. Force-deleting a task
     * in that state causes a Guru Meditation crash (Load access fault in uxListRemove). */
    if (psram_task_is_valid(&s_proxy.adv_psram_task)) {
        uint32_t wait_count = 0;
        const uint32_t max_wait = 100;  /* 100 × 10ms = 1 second */

        while (psram_task_is_valid(&s_proxy.adv_psram_task) && wait_count < max_wait) {
            vTaskDelay(ESPHOME_DELAY_MINIMAL_TICKS);
            wait_count++;
        }

        if (psram_task_is_valid(&s_proxy.adv_psram_task)) {
            /* Task is stuck (likely in blocking send on closed socket).
             * Do NOT force-delete — let it exit naturally when send() fails.
             * The PSRAM stack will leak (~4KB) but that's better than crashing. */
            ESP_LOGW(TAG, "Advertisement task still running after %lums, abandoning "
                     "(will self-cleanup when send completes)",
                     (unsigned long)(max_wait * portTICK_PERIOD_MS));
            /* Invalidate handle so we don't try to delete it later */
            memset(&s_proxy.adv_psram_task, 0, sizeof(s_proxy.adv_psram_task));
        } else {
            /* Task exited cleanly — free PSRAM resources */
            psram_task_delete(&s_proxy.adv_psram_task);
        }
    }

    /* Clear GATT callbacks */
    ble_gatt_set_connect_cb(NULL);
    ble_gatt_set_disconnect_cb(NULL);
    ble_gatt_set_notify_cb(NULL);

    /* Disconnect all connections */
    for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
        if (s_proxy.connections[i].active) {
            ble_gatt_disconnect(s_proxy.connections[i].conn_handle);
            s_proxy.connections[i].active = false;
        }
    }

    ESP_LOGI(TAG, "BLE Proxy stopped");
    return ESP_OK;
}

bool esphome_ble_proxy_is_running(void)
{
    return s_proxy.running;
}

esp_err_t esphome_ble_proxy_get_stats(esphome_ble_proxy_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_proxy.mutex) {
        memset(stats, 0, sizeof(*stats));
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        *stats = s_proxy.stats;
        xSemaphoreGive(s_proxy.mutex);
    }

    return ESP_OK;
}

void esphome_ble_proxy_reset_stats(void)
{
    if (!s_proxy.mutex) {
        return;
    }
    if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        memset(&s_proxy.stats, 0, sizeof(s_proxy.stats));
        xSemaphoreGive(s_proxy.mutex);
    }
}

/* ============================================================================
 * Advertisement Handling
 * ============================================================================ */

/**
 * @brief BLE scanner callback - queues advertisement for forwarding
 */
static void on_advertisement(const ble_adv_data_t *adv)
{
    if (!s_proxy.running || !adv) {
        return;
    }

    /* Update statistics */
    if (xSemaphoreTake(s_proxy.mutex, 0) == pdTRUE) {
        s_proxy.stats.advertisements_received++;
        xSemaphoreGive(s_proxy.mutex);
    }

    /* Apply RSSI filter */
    if (adv->rssi < s_proxy.config.min_rssi) {
        if (xSemaphoreTake(s_proxy.mutex, 0) == pdTRUE) {
            s_proxy.stats.advertisements_filtered++;
            xSemaphoreGive(s_proxy.mutex);
        }
        return;
    }

    /* Queue for forwarding (don't block scanner) */
    if (xQueueSend(s_proxy.adv_queue, adv, 0) != pdTRUE) {
        /* BF-026: Track dropped advertisements */
        if (xSemaphoreTake(s_proxy.mutex, 0) == pdTRUE) {
            s_proxy.stats.advertisements_dropped++;
            xSemaphoreGive(s_proxy.mutex);
        }
        ESP_LOGD(TAG, "Advertisement queue full, dropping");
    }
}

/**
 * @brief Task that forwards queued advertisements to subscribed clients
 */
static void advertisement_task(void *pvParameters)
{
    ble_adv_data_t adv;

    ESP_LOGI(TAG, "Advertisement forwarding task started");

    while (s_proxy.running) {
        if (xQueueReceive(s_proxy.adv_queue, &adv, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;  /* Timeout — re-check running flag */
        }

        /* Send to all subscribed clients, bail out immediately if stopping */
        for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_SUBSCRIBED_CLIENTS; i++) {
            if (!s_proxy.running) {
                break;  /* Stop signaled — don't attempt any more sends */
            }
            if (s_proxy.subscribed_clients[i]) {
                esp_err_t ret = send_advertisement_response(i, &adv);
                if (ret == ESP_OK) {
                    if (xSemaphoreTake(s_proxy.mutex, 0) == pdTRUE) {
                        s_proxy.stats.advertisements_forwarded++;
                        xSemaphoreGive(s_proxy.mutex);
                    }
                }
            }
        }
    }

    ESP_LOGI(TAG, "Advertisement forwarding task exiting");
    /* Mark task as self-deleted - resources freed by stop function */
    psram_task_mark_deleted(&s_proxy.adv_psram_task);
    vTaskDelete(NULL);
}

/**
 * @brief Encode and send BLE advertisement response (MSG 67)
 */
static esp_err_t send_advertisement_response(uint8_t client_id, const ble_adv_data_t *adv)
{
    uint8_t payload[ESPHOME_BLE_PROXY_PAYLOAD_SIZE];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: address (fixed64) - MAC as little-endian uint64 */
    uint64_t address = esphome_mac_to_uint64(adv->mac);
    esphome_encode_fixed64(&buf, 1, address);

    /* Field 2: rssi (sint32) */
    esphome_encode_sint32(&buf, 2, adv->rssi);

    /* Field 3: address_type (uint32) */
    esphome_encode_uint32(&buf, 3, (uint32_t)adv->addr_type);

    /* Field 4: name (string) - parse from advertisement data if available */
    /* Note: Name parsing would need to look for AD type 0x08/0x09 in adv_data */
    /* For now, we skip the name if not parsed by scanner */

    /* BF-022/BF-027: Parse advertisement data with overflow protection
     * Using named AD type constants for clarity */
    const uint8_t *ptr = adv->adv_data;
    size_t remaining = adv->adv_data_len;

    /* BF-025: Track if we found a name (scan response may have better name) */
    bool has_name = false;

    /* Safety limit on iterations to prevent infinite loops */
    int max_iterations = ESPHOME_BLE_AD_MAX_PARSE_ITERATIONS;

    while (remaining >= 2 && max_iterations-- > 0) {
        uint8_t ad_len = ptr[0];

        /* BF-022: Additional overflow checks */
        if (ad_len == 0) {
            ESP_LOGD(TAG, "Zero-length AD structure, stopping parse");
            break;
        }

        /* Ensure we have enough data for length + type + data */
        if ((size_t)(ad_len + 1) > remaining) {
            ESP_LOGD(TAG, "AD length %u exceeds remaining %zu bytes", ad_len, remaining);
            break;
        }

        uint8_t ad_type = ptr[1];

        /* BF-027: Use named constants for AD types */
        switch (ad_type) {
            case BLE_AD_TYPE_COMPLETE_NAME:
            case BLE_AD_TYPE_SHORT_NAME:
                /* Complete name takes precedence over short name */
                if (ad_len > 1 && (!has_name || ad_type == BLE_AD_TYPE_COMPLETE_NAME)) {
                    char name[32] = {0};
                    size_t name_len = (size_t)(ad_len - 1);
                    if (name_len > sizeof(name) - 1) {
                        name_len = sizeof(name) - 1;
                    }
                    memcpy(name, &ptr[2], name_len);
                    name[name_len] = '\0';
                    esphome_encode_string(&buf, 4, name);
                    has_name = true;
                }
                break;

            case BLE_AD_TYPE_MANUFACTURER_DATA:
                if (ad_len > 1) {
                    esphome_encode_bytes(&buf, 7, &ptr[2], ad_len - 1);
                }
                break;

            case BLE_AD_TYPE_INCOMPLETE_UUID16:
            case BLE_AD_TYPE_COMPLETE_UUID16:
                if (ad_len > 1) {
                    esphome_encode_bytes(&buf, 5, &ptr[2], ad_len - 1);
                }
                break;

            case BLE_AD_TYPE_SERVICE_DATA_16:
            case BLE_AD_TYPE_SERVICE_DATA_32:
            case BLE_AD_TYPE_SERVICE_DATA_128:
                if (ad_len > 1) {
                    esphome_encode_bytes(&buf, 6, &ptr[2], ad_len - 1);
                }
                break;

            default:
                /* Other AD types not forwarded to ESPHome */
                break;
        }

        ptr += ad_len + 1;
        remaining -= (size_t)(ad_len + 1);
    }

    /* BF-025: Process scan response data if available */
    if (adv->scan_rsp_len > 0) {
        ptr = adv->scan_rsp;
        remaining = adv->scan_rsp_len;
        max_iterations = ESPHOME_BLE_AD_MAX_PARSE_ITERATIONS;

        while (remaining >= 2 && max_iterations-- > 0) {
            uint8_t ad_len = ptr[0];

            if (ad_len == 0) {
                break;
            }

            if ((size_t)(ad_len + 1) > remaining) {
                break;
            }

            uint8_t ad_type = ptr[1];

            switch (ad_type) {
                case BLE_AD_TYPE_COMPLETE_NAME:
                    /* Scan response complete name overrides adv name */
                    if (ad_len > 1) {
                        char name[32] = {0};
                        size_t name_len = (size_t)(ad_len - 1);
                        if (name_len > sizeof(name) - 1) {
                            name_len = sizeof(name) - 1;
                        }
                        memcpy(name, &ptr[2], name_len);
                        name[name_len] = '\0';
                        esphome_encode_string(&buf, 4, name);
                    }
                    break;

                case BLE_AD_TYPE_MANUFACTURER_DATA:
                    if (ad_len > 1) {
                        esphome_encode_bytes(&buf, 7, &ptr[2], ad_len - 1);
                    }
                    break;

                case BLE_AD_TYPE_SERVICE_DATA_16:
                case BLE_AD_TYPE_SERVICE_DATA_32:
                case BLE_AD_TYPE_SERVICE_DATA_128:
                    if (ad_len > 1) {
                        esphome_encode_bytes(&buf, 6, &ptr[2], ad_len - 1);
                    }
                    break;

                default:
                    break;
            }

            ptr += ad_len + 1;
            remaining -= (size_t)(ad_len + 1);
        }
    }

    /* Build and send message */
    uint8_t output[600];
    size_t output_len;
    esp_err_t ret = esphome_build_message(ESPHOME_MSG_BLE_ADVERTISEMENT_RESPONSE,
                                           payload, buf.position,
                                           output, sizeof(output), &output_len);
    if (ret != ESP_OK) {
        return ret;
    }

    return esphome_api_send_to_client(client_id, output, output_len);
}

/* ============================================================================
 * Message Handlers
 * ============================================================================ */

esp_err_t esphome_ble_proxy_handle_subscribe_advertisements(
    uint8_t client_id, const uint8_t *payload, size_t len)
{
    BLE_PROXY_CHECK_INITIALIZED();

    if (client_id >= ESPHOME_BLE_PROXY_MAX_SUBSCRIBED_CLIENTS) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse flags from payload (field 1: uint32) */
    uint32_t flags = 0;
    if (len > 0 && payload) {
        esphome_buffer_t buf;
        esphome_buffer_init(&buf, (uint8_t *)payload, len);

        while (buf.position < buf.size) {
            uint32_t field_num;
            protobuf_wire_type_t wire_type;
            esphome_decode_tag(&buf, &field_num, &wire_type);

            if (field_num == 1 && wire_type == PROTOBUF_WIRE_VARINT) {
                esphome_decode_uint32(&buf, &flags);
            } else {
                esphome_skip_field(&buf, wire_type);
            }
        }
    }

    /* Subscribe or unsubscribe based on flags */
    bool subscribe = (flags != 0) || (len == 0);  /* Default to subscribe if empty message */

    if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        s_proxy.subscribed_clients[client_id] = subscribe;

        /* Update subscribed client count */
        s_proxy.stats.subscribed_clients = 0;
        for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_SUBSCRIBED_CLIENTS; i++) {
            if (s_proxy.subscribed_clients[i]) {
                s_proxy.stats.subscribed_clients++;
            }
        }

        xSemaphoreGive(s_proxy.mutex);
    }

    ESP_LOGI(TAG, "Client %d %s BLE advertisements", client_id,
             subscribe ? "subscribed to" : "unsubscribed from");

    return ESP_OK;
}

esp_err_t esphome_ble_proxy_handle_device_request(
    uint8_t client_id, const uint8_t *payload, size_t len)
{
    BLE_PROXY_CHECK_INITIALIZED();

    if (!payload || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse request */
    uint64_t address = 0;
    uint32_t request_type = 0;
    uint32_t address_type = 0;

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, (uint8_t *)payload, len);

    while (buf.position < buf.size) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;
        esphome_decode_tag(&buf, &field_num, &wire_type);

        switch (field_num) {
            case 1:  /* address (fixed64) */
                if (wire_type == PROTOBUF_WIRE_64BIT) {
                    esphome_decode_fixed64(&buf, &address);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            case 2:  /* request_type (uint32) */
                if (wire_type == PROTOBUF_WIRE_VARINT) {
                    esphome_decode_uint32(&buf, &request_type);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            case 4:  /* address_type (uint32) */
                if (wire_type == PROTOBUF_WIRE_VARINT) {
                    esphome_decode_uint32(&buf, &address_type);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    /* Convert address to MAC */
    uint8_t mac[6];
    esphome_uint64_to_mac(address, mac);

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);
    ESP_LOGI(TAG, "Device request: type=%lu, address=%s", request_type, mac_str);

    esp_err_t ret = ESP_OK;

    switch (request_type) {
        case ESPHOME_BLE_REQUEST_CONNECT: {
            /* BF-014: Atomically find and reserve connection slot with mutex */
            int slot = -1;

            if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) != pdTRUE) {
                ESP_LOGW(TAG, "Failed to acquire mutex for connection");
                return send_device_connection_response(client_id, address, false,
                                                       ESPHOME_BLE_GATT_ERROR_FAILED);
            }

            /* Find and atomically reserve slot */
            for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
                if (!s_proxy.connections[i].active) {
                    slot = i;
                    /* Reserve slot immediately while holding mutex */
                    s_proxy.connections[i].active = true;
                    s_proxy.connections[i].address = address;
                    s_proxy.connections[i].owner_client = client_id;
                    s_proxy.connections[i].conn_handle = BLE_GATT_INVALID_CONN_HANDLE;
                    s_proxy.stats.connections++;
                    break;
                }
            }

            xSemaphoreGive(s_proxy.mutex);

            if (slot < 0) {
                ESP_LOGW(TAG, "No free connection slots");
                return send_device_connection_response(client_id, address, false,
                                                       ESPHOME_BLE_GATT_ERROR_FAILED);
            }

            /* Initiate connection (slot already reserved) */
            ret = ble_gatt_connect(mac, (ble_addr_type_t)address_type,
                                   BLE_GATT_DEFAULT_CONN_TIMEOUT_MS);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initiate connection: %s", esp_err_to_name(ret));
                /* Release the reserved slot */
                if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
                    s_proxy.connections[slot].active = false;
                    if (s_proxy.stats.connections > 0) {
                        s_proxy.stats.connections--;
                    }
                    xSemaphoreGive(s_proxy.mutex);
                }
                return send_device_connection_response(client_id, address, false,
                                                       ESPHOME_BLE_GATT_ERROR_FAILED);
            }
            break;
        }

        case ESPHOME_BLE_REQUEST_DISCONNECT: {
            /* Find connection slot */
            for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
                if (s_proxy.connections[i].active &&
                    s_proxy.connections[i].address == address) {
                    ret = ble_gatt_disconnect(s_proxy.connections[i].conn_handle);
                    /* Slot will be cleaned up in disconnect callback */
                    break;
                }
            }
            break;
        }

        case ESPHOME_BLE_REQUEST_PAIR:
            /* Pairing is handled automatically by BLE security module */
            ESP_LOGI(TAG, "Pair request - security handled automatically");
            break;

        default:
            ESP_LOGW(TAG, "Unknown request type: %lu", request_type);
            break;
    }

    return ret;
}

esp_err_t esphome_ble_proxy_handle_connection_free(
    uint8_t client_id, const uint8_t *payload, size_t len)
{
    BLE_PROXY_CHECK_INITIALIZED();

    (void)payload;
    (void)len;

    return send_connections_free_response(client_id);
}

esp_err_t esphome_ble_proxy_handle_gatt_read(
    uint8_t client_id, const uint8_t *payload, size_t len)
{
    BLE_PROXY_CHECK_INITIALIZED();

    if (!payload || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse request */
    uint64_t address = 0;
    uint32_t handle = 0;

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, (uint8_t *)payload, len);

    while (buf.position < buf.size) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;
        esphome_decode_tag(&buf, &field_num, &wire_type);

        switch (field_num) {
            case 1:  /* address (fixed64) */
                if (wire_type == PROTOBUF_WIRE_64BIT) {
                    esphome_decode_fixed64(&buf, &address);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            case 2:  /* handle (uint32) */
                if (wire_type == PROTOBUF_WIRE_VARINT) {
                    esphome_decode_uint32(&buf, &handle);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    /* Find connection */
    uint16_t conn_handle = BLE_GATT_INVALID_CONN_HANDLE;
    for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
        if (s_proxy.connections[i].active &&
            s_proxy.connections[i].address == address) {
            conn_handle = s_proxy.connections[i].conn_handle;
            break;
        }
    }

    if (conn_handle == BLE_GATT_INVALID_CONN_HANDLE) {
        ESP_LOGW(TAG, "Device not connected for GATT read");
        /* BF-017: Send error response with empty data */
        return send_gatt_read_response(client_id, address, (uint16_t)handle, NULL, 0);
    }

    /* Perform read */
    uint8_t data[ESPHOME_BLE_DATA_BUFFER_SIZE];
    uint16_t data_len = sizeof(data);
    esp_err_t ret = ble_gatt_read_characteristic(conn_handle, (uint16_t)handle,
                                                  data, &data_len,
                                                  BLE_GATT_DEFAULT_OP_TIMEOUT_MS);

    if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        if (ret == ESP_OK) {
            s_proxy.stats.gatt_reads++;
        } else {
            s_proxy.stats.gatt_errors++;
        }
        xSemaphoreGive(s_proxy.mutex);
    }

    /* Send response */
    return send_gatt_read_response(client_id, address, (uint16_t)handle,
                                   ret == ESP_OK ? data : NULL,
                                   ret == ESP_OK ? data_len : 0);
}

esp_err_t esphome_ble_proxy_handle_gatt_write(
    uint8_t client_id, const uint8_t *payload, size_t len)
{
    BLE_PROXY_CHECK_INITIALIZED();

    if (!payload || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse request */
    uint64_t address = 0;
    uint32_t handle = 0;
    uint8_t data[ESPHOME_BLE_DATA_BUFFER_SIZE];
    size_t data_len = 0;
    bool response = true;

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, (uint8_t *)payload, len);

    while (buf.position < buf.size) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;
        esphome_decode_tag(&buf, &field_num, &wire_type);

        switch (field_num) {
            case 1:  /* address (fixed64) */
                if (wire_type == PROTOBUF_WIRE_64BIT) {
                    esphome_decode_fixed64(&buf, &address);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            case 2:  /* handle (uint32) */
                if (wire_type == PROTOBUF_WIRE_VARINT) {
                    esphome_decode_uint32(&buf, &handle);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            case 3:  /* data (bytes) */
                if (wire_type == PROTOBUF_WIRE_LEN) {
                    esphome_decode_bytes(&buf, data, sizeof(data), &data_len);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            case 4:  /* response (bool) */
                if (wire_type == PROTOBUF_WIRE_VARINT) {
                    bool val;
                    esphome_decode_bool(&buf, &val);
                    response = val;
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    /* Find connection */
    uint16_t conn_handle = BLE_GATT_INVALID_CONN_HANDLE;
    for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
        if (s_proxy.connections[i].active &&
            s_proxy.connections[i].address == address) {
            conn_handle = s_proxy.connections[i].conn_handle;
            break;
        }
    }

    if (conn_handle == BLE_GATT_INVALID_CONN_HANDLE) {
        ESP_LOGW(TAG, "Device not connected for GATT write");
        return send_gatt_write_response(client_id, address, (uint16_t)handle, false);
    }

    /* Perform write */
    esp_err_t ret;
    if (response) {
        ret = ble_gatt_write_characteristic(conn_handle, (uint16_t)handle,
                                            data, (uint16_t)data_len,
                                            BLE_GATT_DEFAULT_OP_TIMEOUT_MS);
    } else {
        ret = ble_gatt_write_no_response(conn_handle, (uint16_t)handle,
                                          data, (uint16_t)data_len);
    }

    if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        if (ret == ESP_OK) {
            s_proxy.stats.gatt_writes++;
        } else {
            s_proxy.stats.gatt_errors++;
        }
        xSemaphoreGive(s_proxy.mutex);
    }

    /* Send response if requested */
    if (response) {
        return send_gatt_write_response(client_id, address, (uint16_t)handle, ret == ESP_OK);
    }

    return ret;
}

esp_err_t esphome_ble_proxy_handle_gatt_notify_request(
    uint8_t client_id, const uint8_t *payload, size_t len)
{
    BLE_PROXY_CHECK_INITIALIZED();

    if (!payload || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse request */
    uint64_t address = 0;
    uint32_t handle = 0;
    bool enable = true;

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, (uint8_t *)payload, len);

    while (buf.position < buf.size) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;
        esphome_decode_tag(&buf, &field_num, &wire_type);

        switch (field_num) {
            case 1:  /* address (fixed64) */
                if (wire_type == PROTOBUF_WIRE_64BIT) {
                    esphome_decode_fixed64(&buf, &address);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            case 2:  /* handle (uint32) */
                if (wire_type == PROTOBUF_WIRE_VARINT) {
                    esphome_decode_uint32(&buf, &handle);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            case 3:  /* enable (bool) */
                if (wire_type == PROTOBUF_WIRE_VARINT) {
                    esphome_decode_bool(&buf, &enable);
                } else {
                    esphome_skip_field(&buf, wire_type);
                }
                break;

            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    /* Find connection */
    uint16_t conn_handle = BLE_GATT_INVALID_CONN_HANDLE;
    for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
        if (s_proxy.connections[i].active &&
            s_proxy.connections[i].address == address) {
            conn_handle = s_proxy.connections[i].conn_handle;
            break;
        }
    }

    if (conn_handle == BLE_GATT_INVALID_CONN_HANDLE) {
        ESP_LOGW(TAG, "Device not connected for notify subscription");
        return ESP_ERR_NOT_FOUND;
    }

    /* Subscribe/unsubscribe to notifications */
    return ble_gatt_subscribe_notify(conn_handle, (uint16_t)handle, enable);
}

/* ============================================================================
 * Response Senders
 * ============================================================================ */

/**
 * @brief Send device connection response (MSG 69)
 */
static esp_err_t send_device_connection_response(uint8_t client_id, uint64_t address,
                                                  bool connected, int error)
{
    uint8_t payload[64];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: address (fixed64) */
    esphome_encode_fixed64(&buf, 1, address);

    /* Field 2: free - number of free connections */
    esphome_encode_uint32(&buf, 2, esphome_ble_proxy_get_free_connections());

    /* Field 3: error (int32) */
    if (error != 0) {
        esphome_encode_int32(&buf, 3, error);
    }

    uint8_t output[128];
    size_t output_len;
    esp_err_t ret = esphome_build_message(ESPHOME_MSG_BLE_DEVICE_CONNECTION_RESP,
                                           payload, buf.position,
                                           output, sizeof(output), &output_len);
    if (ret != ESP_OK) {
        return ret;
    }

    return esphome_api_send_to_client(client_id, output, output_len);
}

/**
 * @brief Send connections free response (MSG 81)
 */
static esp_err_t send_connections_free_response(uint8_t client_id)
{
    uint8_t payload[ESPHOME_PAYLOAD_SMALL];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: free (uint32) - number of free connection slots */
    esphome_encode_uint32(&buf, 1, esphome_ble_proxy_get_free_connections());

    /* Field 2: limit (uint32) - maximum connections */
    esphome_encode_uint32(&buf, 2, ESPHOME_BLE_PROXY_MAX_CONNECTIONS);

    uint8_t output[64];
    size_t output_len;
    esp_err_t ret = esphome_build_message(ESPHOME_MSG_BLE_CONNECTIONS_FREE_RESP,
                                           payload, buf.position,
                                           output, sizeof(output), &output_len);
    if (ret != ESP_OK) {
        return ret;
    }

    return esphome_api_send_to_client(client_id, output, output_len);
}

/**
 * @brief Send GATT read response (MSG 74)
 */
static esp_err_t send_gatt_read_response(uint8_t client_id, uint64_t address,
                                          uint16_t handle, const uint8_t *data, size_t len)
{
    uint8_t payload[300];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: address (fixed64) */
    esphome_encode_fixed64(&buf, 1, address);

    /* Field 2: handle (uint32) */
    esphome_encode_uint32(&buf, 2, handle);

    /* Field 3: data (bytes) */
    if (data && len > 0) {
        esphome_encode_bytes(&buf, 3, data, len);
    }

    uint8_t output[400];
    size_t output_len;
    esp_err_t ret = esphome_build_message(ESPHOME_MSG_BLE_GATT_READ_RESPONSE,
                                           payload, buf.position,
                                           output, sizeof(output), &output_len);
    if (ret != ESP_OK) {
        return ret;
    }

    return esphome_api_send_to_client(client_id, output, output_len);
}

/**
 * @brief Send GATT write response (MSG 76)
 */
static esp_err_t send_gatt_write_response(uint8_t client_id, uint64_t address,
                                           uint16_t handle, bool success)
{
    uint8_t payload[32];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: address (fixed64) */
    esphome_encode_fixed64(&buf, 1, address);

    /* Field 2: handle (uint32) */
    esphome_encode_uint32(&buf, 2, handle);

    /* Field 3: error (int32) - 0 for success, non-zero for error */
    if (!success) {
        esphome_encode_int32(&buf, 3, ESPHOME_BLE_GATT_ERROR_FAILED);
    }

    uint8_t output[64];
    size_t output_len;
    esp_err_t ret = esphome_build_message(ESPHOME_MSG_BLE_GATT_WRITE_RESPONSE,
                                           payload, buf.position,
                                           output, sizeof(output), &output_len);
    if (ret != ESP_OK) {
        return ret;
    }

    return esphome_api_send_to_client(client_id, output, output_len);
}

/* ============================================================================
 * GATT Callbacks
 * ============================================================================ */

/**
 * @brief GATT connection callback
 * BF-016: Thread-safe with mutex protection
 */
static void on_gatt_connect(const uint8_t *mac, bool success, uint16_t conn_handle)
{
    uint64_t address = esphome_mac_to_uint64(mac);
    uint8_t owner_client = 0;
    bool found = false;

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);
    ESP_LOGI(TAG, "GATT connect callback: %s, success=%d, handle=%u",
             mac_str, success, conn_handle);

    /* BF-016: Protect connection array access with mutex */
    if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        /* Find and update connection slot */
        for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
            if (s_proxy.connections[i].active &&
                s_proxy.connections[i].address == address) {
                if (success) {
                    s_proxy.connections[i].conn_handle = conn_handle;
                    s_proxy.stats.active_connections++;
                } else {
                    /* Connection failed - clear slot */
                    s_proxy.connections[i].active = false;
                }
                owner_client = s_proxy.connections[i].owner_client;
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_proxy.mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex in connect callback");
        return;
    }

    /* Notify client outside of mutex */
    if (found) {
        send_device_connection_response(owner_client, address, success,
                                        success ? 0 : ESPHOME_BLE_GATT_ERROR_FAILED);
    }
}

/**
 * @brief GATT disconnection callback
 * BF-016: Thread-safe with mutex protection
 */
static void on_gatt_disconnect(const uint8_t *mac, uint8_t reason)
{
    uint64_t address = esphome_mac_to_uint64(mac);
    uint8_t owner = 0;
    bool found = false;

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);
    ESP_LOGI(TAG, "GATT disconnect callback: %s, reason=%u", mac_str, reason);

    /* BF-016: Protect connection array access with mutex */
    if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        /* Find and clear connection slot */
        for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
            if (s_proxy.connections[i].active &&
                s_proxy.connections[i].address == address) {

                owner = s_proxy.connections[i].owner_client;
                s_proxy.connections[i].active = false;
                s_proxy.connections[i].conn_handle = BLE_GATT_INVALID_CONN_HANDLE;

                if (s_proxy.stats.active_connections > 0) {
                    s_proxy.stats.active_connections--;
                }
                s_proxy.stats.disconnections++;
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_proxy.mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex in disconnect callback");
        return;
    }

    /* Notify client outside of mutex */
    if (found) {
        send_device_connection_response(owner, address, false, 0);
    }
}

/**
 * @brief GATT notification callback - forward to ESPHome clients
 * BF-016: Thread-safe with mutex protection
 */
static void on_gatt_notify(uint16_t conn_handle, uint16_t attr_handle,
                           const uint8_t *data, uint16_t len)
{
    /* Find connection owner */
    uint64_t address = 0;
    uint8_t owner = 0;

    /* BF-016: Protect connection array access with mutex */
    if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_FAST_TICKS) == pdTRUE) {
        for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
            if (s_proxy.connections[i].active &&
                s_proxy.connections[i].conn_handle == conn_handle) {
                address = s_proxy.connections[i].address;
                owner = s_proxy.connections[i].owner_client;
                break;
            }
        }
        xSemaphoreGive(s_proxy.mutex);
    }

    if (address == 0) {
        ESP_LOGW(TAG, "Notification from unknown connection handle %u", conn_handle);
        return;
    }

    /* Build and send MSG 74: BLE GATT Notify Data Response */
    uint8_t payload[300];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: address (fixed64) */
    esphome_encode_fixed64(&buf, 1, address);

    /* Field 2: handle (uint32) */
    esphome_encode_uint32(&buf, 2, attr_handle);

    /* Field 3: data (bytes) */
    if (data && len > 0) {
        esphome_encode_bytes(&buf, 3, data, len);
    }

    uint8_t output[400];
    size_t output_len;
    esp_err_t ret = esphome_build_message(ESPHOME_MSG_BLE_GATT_NOTIFY_DATA_RESP,
                                           payload, buf.position,
                                           output, sizeof(output), &output_len);
    if (ret == ESP_OK) {
        esphome_api_send_to_client(owner, output, output_len);
    }
}

/* ============================================================================
 * Client Cleanup
 * ============================================================================ */

void esphome_ble_proxy_client_disconnected(uint8_t client_id)
{
    if (client_id >= ESPHOME_BLE_PROXY_MAX_SUBSCRIBED_CLIENTS) {
        return;
    }

    /* Check if BLE proxy is initialized */
    if (!s_proxy.initialized || !s_proxy.mutex) {
        return;
    }

    /* Unsubscribe from advertisements */
    if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        s_proxy.subscribed_clients[client_id] = false;

        /* Update subscribed count */
        s_proxy.stats.subscribed_clients = 0;
        for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_SUBSCRIBED_CLIENTS; i++) {
            if (s_proxy.subscribed_clients[i]) {
                s_proxy.stats.subscribed_clients++;
            }
        }
        xSemaphoreGive(s_proxy.mutex);
    }

    /* Disconnect any GATT connections owned by this client */
    for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
        if (s_proxy.connections[i].active &&
            s_proxy.connections[i].owner_client == client_id) {
            ESP_LOGI(TAG, "Disconnecting GATT connection owned by client %u", client_id);
            ble_gatt_disconnect(s_proxy.connections[i].conn_handle);
            /* Slot will be cleaned up in disconnect callback */
        }
    }

    ESP_LOGD(TAG, "Client %u cleanup complete", client_id);
}

/* ============================================================================
 * Connection Management
 * ============================================================================ */

uint8_t esphome_ble_proxy_get_free_connections(void)
{
    uint8_t free = 0;
    for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_CONNECTIONS; i++) {
        if (!s_proxy.connections[i].active) {
            free++;
        }
    }
    return free;
}

bool esphome_ble_proxy_is_device_connected(uint64_t address)
{
    /* BF-024: Use helper function for connection lookup */
    ble_proxy_connection_t *conn = find_connection_by_address(address);
    return (conn != NULL && conn->conn_handle != BLE_GATT_INVALID_CONN_HANDLE);
}

/* ============================================================================
 * BLE Scanner State and Unsubscribe Handlers
 * ============================================================================ */

esp_err_t esphome_ble_proxy_handle_unsubscribe_advertisements(uint8_t client_id)
{
    BLE_PROXY_CHECK_INITIALIZED();

    if (client_id >= ESPHOME_BLE_PROXY_MAX_SUBSCRIBED_CLIENTS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
        s_proxy.subscribed_clients[client_id] = false;

        s_proxy.stats.subscribed_clients = 0;
        for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_SUBSCRIBED_CLIENTS; i++) {
            if (s_proxy.subscribed_clients[i]) {
                s_proxy.stats.subscribed_clients++;
            }
        }

        xSemaphoreGive(s_proxy.mutex);
    }

    ESP_LOGI(TAG, "Client %d unsubscribed from BLE advertisements", client_id);
    return ESP_OK;
}

esp_err_t esphome_ble_proxy_handle_scanner_set_mode(
    uint8_t client_id, const uint8_t *payload, size_t len)
{
    BLE_PROXY_CHECK_INITIALIZED();

    /* Parse mode from payload (field 1: uint32 mode) */
    uint32_t mode = 0;
    if (len > 0 && payload) {
        esphome_buffer_t buf;
        esphome_buffer_init(&buf, (uint8_t *)payload, len);

        while (buf.position < buf.size) {
            uint32_t field_num;
            protobuf_wire_type_t wire_type;
            esphome_decode_tag(&buf, &field_num, &wire_type);

            if (field_num == 1 && wire_type == PROTOBUF_WIRE_VARINT) {
                esphome_decode_uint32(&buf, &mode);
            } else {
                esphome_skip_field(&buf, wire_type);
            }
        }
    }

    ESP_LOGI(TAG, "Client %d set scanner mode: %lu", client_id, mode);

    /* Respond with current scanner state */
    return esphome_ble_proxy_send_scanner_state(client_id);
}

/**
 * @brief Map internal BLE scanner state to ESPHome protocol enum
 *
 * ESPHome BluetoothScannerState:
 *   0 = IDLE, 1 = STARTING, 2 = RUNNING, 3 = FAILED, 4 = STOPPING, 5 = STOPPED
 */
static uint32_t map_scanner_state_to_esphome(ble_scanner_state_t state)
{
    switch (state) {
        case BLE_SCANNER_STATE_RUNNING:        return 2;  /* RUNNING */
        case BLE_SCANNER_STATE_STARTING:       return 1;  /* STARTING */
        case BLE_SCANNER_STATE_STOPPING:       return 4;  /* STOPPING */
        case BLE_SCANNER_STATE_ERROR:          return 3;  /* FAILED */
        case BLE_SCANNER_STATE_INITIALIZED:    return 5;  /* STOPPED (init but not scanning) */
        case BLE_SCANNER_STATE_UNINITIALIZED:
        default:                               return 0;  /* IDLE */
    }
}

esp_err_t esphome_ble_proxy_send_scanner_state(uint8_t client_id)
{
    BLE_PROXY_CHECK_INITIALIZED();

    uint8_t payload[32];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Query actual BLE scanner state */
    ble_scanner_state_t scanner_state = ble_scanner_get_state();
    uint32_t esphome_state = map_scanner_state_to_esphome(scanner_state);
    bool active = ble_scanner_is_active_enabled();
    uint32_t mode = active ? 1 : 0;  /* 0 = PASSIVE, 1 = ACTIVE */

    ESP_LOGI(TAG, "Sending scanner state to client %d: state=%lu (%s), mode=%s",
             client_id, (unsigned long)esphome_state,
             ble_scanner_get_state_str(), active ? "ACTIVE" : "PASSIVE");

    /* Field 1: state (enum BluetoothScannerState) */
    esphome_encode_uint32(&buf, 1, esphome_state);

    /* Field 2: mode (enum BluetoothScannerMode) - current mode */
    esphome_encode_uint32(&buf, 2, mode);

    /* Field 3: config_mode (enum BluetoothScannerMode) - configured mode */
    esphome_encode_uint32(&buf, 3, mode);

    uint8_t output[64];
    size_t output_len;
    esp_err_t ret = esphome_build_message(ESPHOME_MSG_BLE_SCANNER_STATE_RESPONSE,
                                           payload, buf.position,
                                           output, sizeof(output), &output_len);
    if (ret != ESP_OK) {
        return ret;
    }

    return esphome_api_send_to_client(client_id, output, output_len);
}

void esphome_ble_proxy_broadcast_scanner_state(void)
{
    if (!s_proxy.initialized || !s_proxy.running) {
        return;
    }

    for (int i = 0; i < ESPHOME_BLE_PROXY_MAX_SUBSCRIBED_CLIENTS; i++) {
        if (s_proxy.subscribed_clients[i]) {
            esphome_ble_proxy_send_scanner_state((uint8_t)i);
        }
    }
}

/**
 * @brief BLE scanner state change callback
 *
 * Automatically pushes scanner state updates to all subscribed HA clients
 * when the BLE scanner state changes (e.g., after delayed start).
 */
static void on_scanner_state_changed(ble_scanner_state_t new_state,
                                      ble_scanner_state_t old_state)
{
    ESP_LOGI(TAG, "Scanner state changed: %d → %d", (int)old_state, (int)new_state);
    esphome_ble_proxy_broadcast_scanner_state();
}

/* ============================================================================
 * Testing
 * ============================================================================ */

esp_err_t esphome_ble_proxy_test(void)
{
    ESP_LOGI(TAG, "=== BLE Proxy Self-Test ===");

    /* Test 1: Address conversion */
    ESP_LOGI(TAG, "Test 1: Address conversion");
    uint8_t test_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint64_t addr = esphome_mac_to_uint64(test_mac);

    uint8_t result_mac[6];
    esphome_uint64_to_mac(addr, result_mac);

    for (int i = 0; i < 6; i++) {
        if (test_mac[i] != result_mac[i]) {
            ESP_LOGE(TAG, "Address conversion failed at byte %d", i);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "  PASS: Address conversion");

    /* Test 2: Init/Deinit */
    ESP_LOGI(TAG, "Test 2: Init/Deinit");
    if (!s_proxy.initialized) {
        esphome_ble_proxy_config_t config = ESPHOME_BLE_PROXY_CONFIG_DEFAULT();
        esp_err_t ret = esphome_ble_proxy_init(&config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        if (!s_proxy.initialized) {
            ESP_LOGE(TAG, "Init flag not set");
            return ESP_FAIL;
        }

        ret = esphome_ble_proxy_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Deinit failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    ESP_LOGI(TAG, "  PASS: Init/Deinit");

    /* Test 3: Free connections */
    ESP_LOGI(TAG, "Test 3: Free connections");
    uint8_t free = esphome_ble_proxy_get_free_connections();
    if (free != ESPHOME_BLE_PROXY_MAX_CONNECTIONS) {
        ESP_LOGE(TAG, "Expected %d free connections, got %d",
                 ESPHOME_BLE_PROXY_MAX_CONNECTIONS, free);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "  PASS: Free connections = %d", free);

    /* BF-028: Extended Unit Tests */

    /* Test 4: Helper function - find_connection_by_address (empty) */
    ESP_LOGI(TAG, "Test 4: find_connection_by_address (empty)");
    {
        ble_proxy_connection_t *conn = find_connection_by_address(0x112233445566ULL);
        if (conn != NULL) {
            ESP_LOGE(TAG, "Expected NULL for non-existent connection");
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "  PASS: find_connection_by_address returns NULL for non-existent");

    /* Test 5: is_device_connected for non-connected device */
    ESP_LOGI(TAG, "Test 5: is_device_connected (not connected)");
    {
        bool connected = esphome_ble_proxy_is_device_connected(0x112233445566ULL);
        if (connected) {
            ESP_LOGE(TAG, "Expected false for non-connected device");
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "  PASS: is_device_connected returns false for non-connected");

    /* Test 6: Statistics reset */
    ESP_LOGI(TAG, "Test 6: Statistics reset");
    {
        /* Initialize for stats test */
        esphome_ble_proxy_config_t config = ESPHOME_BLE_PROXY_CONFIG_DEFAULT();
        esp_err_t ret = esphome_ble_proxy_init(&config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Init for stats test failed");
            return ret;
        }

        /* Manually increment a stat */
        if (xSemaphoreTake(s_proxy.mutex, ESPHOME_MUTEX_TIMEOUT_TICKS) == pdTRUE) {
            s_proxy.stats.advertisements_received = ESPHOME_PROXY_TEST_ADV_COUNT;
            s_proxy.stats.advertisements_dropped = 10;
            xSemaphoreGive(s_proxy.mutex);
        }

        /* Reset stats */
        esphome_ble_proxy_reset_stats();

        /* Verify reset */
        esphome_ble_proxy_stats_t stats;
        esphome_ble_proxy_get_stats(&stats);
        if (stats.advertisements_received != 0 || stats.advertisements_dropped != 0) {
            ESP_LOGE(TAG, "Stats not reset properly");
            esphome_ble_proxy_deinit();
            return ESP_FAIL;
        }

        esphome_ble_proxy_deinit();
    }
    ESP_LOGI(TAG, "  PASS: Statistics reset works correctly");

    /* Test 7: AD Type constants defined */
    ESP_LOGI(TAG, "Test 7: AD Type constants");
    {
        /* Verify constants have expected values */
        if (BLE_AD_TYPE_FLAGS != 0x01 ||
            BLE_AD_TYPE_COMPLETE_NAME != 0x09 ||
            BLE_AD_TYPE_SHORT_NAME != 0x08 ||
            BLE_AD_TYPE_MANUFACTURER_DATA != 0xFF ||
            BLE_AD_TYPE_SERVICE_DATA_16 != 0x16) {
            ESP_LOGE(TAG, "AD Type constants have incorrect values");
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "  PASS: AD Type constants correctly defined");

    /* Test 8: Double init protection */
    ESP_LOGI(TAG, "Test 8: Double init protection");
    {
        esphome_ble_proxy_config_t config = ESPHOME_BLE_PROXY_CONFIG_DEFAULT();
        esp_err_t ret = esphome_ble_proxy_init(&config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "First init failed");
            return ret;
        }

        /* Second init should fail */
        ret = esphome_ble_proxy_init(&config);
        if (ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Double init should return ESP_ERR_INVALID_STATE");
            esphome_ble_proxy_deinit();
            return ESP_FAIL;
        }

        esphome_ble_proxy_deinit();
    }
    ESP_LOGI(TAG, "  PASS: Double init protection works");

    /* Test 9: Initialization check macro */
    ESP_LOGI(TAG, "Test 9: Handler rejects calls when not initialized");
    {
        /* Ensure not initialized */
        if (s_proxy.initialized) {
            esphome_ble_proxy_deinit();
        }

        /* Handler should fail */
        esp_err_t ret = esphome_ble_proxy_handle_connection_free(0, NULL, 0);
        if (ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Handler should return ESP_ERR_INVALID_STATE when not initialized");
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "  PASS: Handlers reject calls when not initialized");

    ESP_LOGI(TAG, "=== All BLE Proxy Tests PASSED (9 tests) ===");
    return ESP_OK;
}
