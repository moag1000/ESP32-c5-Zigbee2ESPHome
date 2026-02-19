/**
 * @file ble_gatt_client.c
 * @brief BLE GATT Client Implementation
 *
 * Implements BLE GATT client functionality using NimBLE stack for connecting
 * to BLE peripherals, service discovery, and read/write/notify operations.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_gatt_client.h"
#include "ble_manager.h"
#include "ble_constants.h"
#include "core/memory/memory_manager_ng.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"
#include <string.h>

#if CONFIG_BT_SCANNER_ENABLED

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

static const char *TAG = "BLE_GATT";

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

/* Event bits for synchronous operations */
#define GATT_EVENT_CONNECT_DONE     (1 << 0)
#define GATT_EVENT_DISCONNECT_DONE  (1 << 1)
#define GATT_EVENT_DISC_DONE        (1 << 2)
#define GATT_EVENT_READ_DONE        (1 << 3)
#define GATT_EVENT_WRITE_DONE       (1 << 4)
#define GATT_EVENT_MTU_DONE         (1 << 5)
#define GATT_EVENT_ERROR            (1 << 6)

/* Maximum services/characteristics to cache per connection */
#define MAX_CACHED_SERVICES         BLE_GATT_MAX_SERVICES
#define MAX_CACHED_CHARS            BLE_GATT_MAX_CHARACTERISTICS

/* ============================================================================
 * Internal Data Structures
 * ============================================================================ */

/**
 * @brief Reconnect information for async reconnect via timer
 *
 * Used to schedule reconnection attempts asynchronously to avoid deadlock
 * when auto-reconnect is triggered from the GAP event handler. The GAP event
 * handler runs in the NimBLE host task, and calling ble_gatt_connect() directly
 * would block waiting for events that need to be processed by the same task.
 */
typedef struct {
    uint8_t mac[BLE_MAC_ADDR_LEN];        /**< Device MAC address */
    ble_addr_type_t addr_type;            /**< Address type */
    bool pending;                         /**< Reconnect is pending */
} reconnect_info_t;

/**
 * @brief Cached characteristic information
 */
typedef struct {
    ble_gatt_uuid_t uuid;                     /**< Characteristic UUID */
    uint16_t def_handle;                 /**< Definition handle */
    uint16_t value_handle;               /**< Value handle */
    uint16_t cccd_handle;                /**< CCCD handle */
    uint8_t properties;                  /**< Properties */
    bool valid;                          /**< Entry is valid */
} cached_characteristic_t;

/**
 * @brief Cached service information
 */
typedef struct {
    ble_gatt_uuid_t uuid;                     /**< Service UUID */
    uint16_t start_handle;               /**< Start handle */
    uint16_t end_handle;                 /**< End handle */
    bool primary;                        /**< Is primary service */
    bool valid;                          /**< Entry is valid */
    cached_characteristic_t characteristics[MAX_CACHED_CHARS];
    uint8_t chr_count;                   /**< Characteristic count */
} cached_service_t;

/**
 * @brief Internal connection structure
 */
typedef struct {
    uint8_t mac[BLE_MAC_ADDR_LEN];       /**< Device MAC */
    ble_addr_type_t addr_type;           /**< Address type */
    uint16_t conn_handle;                /**< Connection handle */
    ble_conn_state_t state;              /**< Connection state */
    bool encrypted;                      /**< Encrypted flag */
    uint16_t mtu;                        /**< Current MTU */
    bool auto_reconnect;                 /**< Auto-reconnect flag */
    bool in_use;                         /**< Slot is in use */
    cached_service_t services[MAX_CACHED_SERVICES];
    uint8_t service_count;               /**< Service count */
    bool services_discovered;            /**< Services discovered flag */
} gatt_connection_t;

/**
 * @brief Pending operation context
 */
typedef struct {
    uint16_t conn_handle;                /**< Connection handle */
    uint16_t attr_handle;                /**< Attribute handle */
    uint8_t *buf;                        /**< Data buffer */
    uint16_t *len;                       /**< Data length pointer */
    uint16_t max_len;                    /**< Maximum buffer length */
    esp_err_t result;                    /**< Operation result */
    bool in_progress;                    /**< Operation in progress */
} pending_op_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/* GATT client state */
static ble_gatt_state_t s_gatt_state = BLE_GATT_STATE_UNINITIALIZED;
static SemaphoreHandle_t s_gatt_mutex = NULL;
static SemaphoreHandle_t s_pending_op_mutex = NULL;  /* Mutex for pending operation serialization */
static EventGroupHandle_t s_gatt_events = NULL;

/* Configuration */
static ble_gatt_client_config_t s_config = BLE_GATT_CLIENT_CONFIG_DEFAULT();

/* Connections - allocated in PSRAM to save internal RAM (~10KB) */
static gatt_connection_t *s_connections = NULL;
static uint8_t s_connection_count = 0;

/* Pending operation context */
static pending_op_t s_pending_op = {0};

/* Auto-reconnect timer and info */
static esp_timer_handle_t s_reconnect_timer = NULL;
static reconnect_info_t s_reconnect_info = {0};

/* Callbacks */
static ble_gatt_connect_cb_t s_connect_cb = NULL;
static ble_gatt_disconnect_cb_t s_disconnect_cb = NULL;
static ble_gatt_notify_cb_t s_notify_cb = NULL;
static ble_gatt_disc_cb_t s_disc_cb = NULL;
static ble_gatt_mtu_cb_t s_mtu_cb = NULL;

/* State strings */
static const char *s_state_strings[] = {
    "UNINITIALIZED",
    "INITIALIZED",
    "CONNECTING",
    "CONNECTED",
    "ERROR"
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void set_gatt_state(ble_gatt_state_t state);
static gatt_connection_t *find_connection_by_handle(uint16_t conn_handle);
static gatt_connection_t *find_connection_by_mac(const uint8_t *mac);
static gatt_connection_t *allocate_connection(void);
static void free_connection(gatt_connection_t *conn);
static void update_connection_count(void);
static int gap_event_handler(struct ble_gap_event *event, void *arg);
static int disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg);
static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg);
static int disc_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);
static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg);
static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg);
static int mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t mtu, void *arg);
static void convert_nimble_uuid(const ble_uuid_any_t *nimble_uuid, ble_gatt_uuid_t *out_uuid);
static void convert_to_nimble_uuid(const ble_gatt_uuid_t *uuid, ble_uuid_any_t *nimble_uuid);
static cached_service_t *find_service_for_char(gatt_connection_t *conn, uint16_t char_handle);
static void reconnect_timer_callback(void *arg);

/* ============================================================================
 * Initialization and Deinitialization
 * ============================================================================ */

esp_err_t ble_gatt_client_init(void)
{
    ble_gatt_client_config_t config = BLE_GATT_CLIENT_CONFIG_DEFAULT();
    return ble_gatt_client_init_with_config(&config);
}

esp_err_t ble_gatt_client_init_with_config(const ble_gatt_client_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Configuration is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_gatt_state != BLE_GATT_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "GATT client already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!ble_manager_is_initialized()) {
        ESP_LOGE(TAG, "BLE manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing BLE GATT client...");

    /* Store configuration */
    s_config = *config;

    /* Create recursive mutex for thread-safe access to s_connections.
     * Recursive mutex allows nested locking which is needed because
     * find_connection_by_handle/mac take the mutex internally but may
     * be called from contexts that also hold the mutex. */
    s_gatt_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_gatt_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create recursive mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create pending operation mutex */
    s_pending_op_mutex = xSemaphoreCreateMutex();
    if (s_pending_op_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create pending operation mutex");
        vSemaphoreDelete(s_gatt_mutex);
        s_gatt_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Create event group */
    s_gatt_events = xEventGroupCreate();
    if (s_gatt_events == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(s_pending_op_mutex);
        s_pending_op_mutex = NULL;
        vSemaphoreDelete(s_gatt_mutex);
        s_gatt_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Create auto-reconnect timer */
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = reconnect_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "gatt_reconnect"
    };
    esp_err_t timer_err = esp_timer_create(&reconnect_timer_args, &s_reconnect_timer);
    if (timer_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create reconnect timer: %s", esp_err_to_name(timer_err));
        vEventGroupDelete(s_gatt_events);
        s_gatt_events = NULL;
        vSemaphoreDelete(s_pending_op_mutex);
        s_pending_op_mutex = NULL;
        vSemaphoreDelete(s_gatt_mutex);
        s_gatt_mutex = NULL;
        return timer_err;
    }

    /* Allocate connections array using memory manager (saves ~10KB internal RAM) */
    if (s_connections == NULL) {
        s_connections = mem_ng_calloc(BLE_GATT_MAX_CONNECTIONS, sizeof(gatt_connection_t), MEM_CAP_PSRAM);
        if (!s_connections) {
            ESP_LOGE(TAG, "Failed to allocate connections array");
            esp_timer_delete(s_reconnect_timer);
            s_reconnect_timer = NULL;
            vEventGroupDelete(s_gatt_events);
            s_gatt_events = NULL;
            vSemaphoreDelete(s_pending_op_mutex);
            s_pending_op_mutex = NULL;
            vSemaphoreDelete(s_gatt_mutex);
            s_gatt_mutex = NULL;
            return ESP_ERR_NO_MEM;
        }
    } else {
        memset(s_connections, 0, BLE_GATT_MAX_CONNECTIONS * sizeof(gatt_connection_t));
    }
    s_connection_count = 0;

    /* Clear pending operation and reconnect info */
    memset(&s_pending_op, 0, sizeof(s_pending_op));
    memset(&s_reconnect_info, 0, sizeof(s_reconnect_info));

    set_gatt_state(BLE_GATT_STATE_INITIALIZED);
    ESP_LOGI(TAG, "BLE GATT client initialized (max connections: %d, preferred MTU: %d, PSRAM)",
             BLE_GATT_MAX_CONNECTIONS, s_config.preferred_mtu);

    return ESP_OK;
}

esp_err_t ble_gatt_client_deinit(void)
{
    if (s_gatt_state == BLE_GATT_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "GATT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing BLE GATT client...");

    /* Disconnect all connections */
    ble_gatt_disconnect_all();

    /* Wait briefly for disconnections */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Clear and free connections */
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    if (s_connections) {
        mem_ng_free(s_connections);
        s_connections = NULL;
    }
    s_connection_count = 0;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    /* Stop and delete reconnect timer */
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }
    memset(&s_reconnect_info, 0, sizeof(s_reconnect_info));

    /* Delete event group */
    if (s_gatt_events != NULL) {
        vEventGroupDelete(s_gatt_events);
        s_gatt_events = NULL;
    }

    /* Delete pending operation mutex */
    if (s_pending_op_mutex != NULL) {
        vSemaphoreDelete(s_pending_op_mutex);
        s_pending_op_mutex = NULL;
    }

    /* Delete mutex */
    if (s_gatt_mutex != NULL) {
        vSemaphoreDelete(s_gatt_mutex);
        s_gatt_mutex = NULL;
    }

    /* Clear callbacks */
    s_connect_cb = NULL;
    s_disconnect_cb = NULL;
    s_notify_cb = NULL;
    s_disc_cb = NULL;
    s_mtu_cb = NULL;

    set_gatt_state(BLE_GATT_STATE_UNINITIALIZED);
    ESP_LOGI(TAG, "BLE GATT client deinitialized");

    return ESP_OK;
}

bool ble_gatt_client_is_initialized(void)
{
    return (s_gatt_state != BLE_GATT_STATE_UNINITIALIZED);
}

ble_gatt_state_t ble_gatt_client_get_state(void)
{
    return s_gatt_state;
}

const char *ble_gatt_client_get_state_str(void)
{
    if (s_gatt_state >= 0 &&
        s_gatt_state < sizeof(s_state_strings) / sizeof(s_state_strings[0])) {
        return s_state_strings[s_gatt_state];
    }
    return "UNKNOWN";
}

/* ============================================================================
 * Connection Management
 * ============================================================================ */

esp_err_t ble_gatt_connect(const uint8_t *mac, ble_addr_type_t addr_type, uint32_t timeout_ms)
{
    if (mac == NULL) {
        ESP_LOGE(TAG, "MAC address is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_gatt_client_is_initialized()) {
        ESP_LOGE(TAG, "GATT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);

    /* Check if already connected */
    if (ble_gatt_is_connected(mac)) {
        ESP_LOGW(TAG, "Device %s already connected", mac_str);
        return ESP_OK;
    }

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Allocate connection slot */
    gatt_connection_t *conn = allocate_connection();
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        ESP_LOGE(TAG, "Maximum connections reached (%d)", BLE_GATT_MAX_CONNECTIONS);
        return ESP_ERR_NO_MEM;
    }

    /* Initialize connection */
    memcpy(conn->mac, mac, BLE_MAC_ADDR_LEN);
    conn->addr_type = addr_type;
    conn->state = BLE_CONN_STATE_CONNECTING;
    conn->mtu = BLE_GATT_DEFAULT_MTU;
    conn->conn_handle = BLE_GATT_INVALID_CONN_HANDLE;

    xSemaphoreGiveRecursive(s_gatt_mutex);

    /* Use default timeout if not specified */
    if (timeout_ms == 0) {
        timeout_ms = s_config.conn_timeout_ms;
    }

    ESP_LOGI(TAG, "Connecting to %s (type: %d, timeout: %lu ms)",
             mac_str, addr_type, (unsigned long)timeout_ms);

    set_gatt_state(BLE_GATT_STATE_CONNECTING);

    /* Prepare NimBLE address */
    ble_addr_t peer_addr;
    peer_addr.type = addr_type;
    memcpy(peer_addr.val, mac, BLE_MAC_ADDR_LEN);

    /* Clear event bits */
    xEventGroupClearBits(s_gatt_events,
                          GATT_EVENT_CONNECT_DONE | GATT_EVENT_ERROR);

    /* Initiate connection */
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer_addr,
                              timeout_ms * 1000 / BLE_HCI_CONN_ITVL,  /* Convert to BLE units */
                              NULL,  /* Use default connection params */
                              gap_event_handler, NULL);

    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initiate connection: %d (%s)", rc, ble_hs_err_to_str(rc));
        xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
        free_connection(conn);
        xSemaphoreGiveRecursive(s_gatt_mutex);
        update_connection_count();
        return ESP_FAIL;
    }

    /* Wait for connection result */
    EventBits_t bits = xEventGroupWaitBits(s_gatt_events,
                                            GATT_EVENT_CONNECT_DONE | GATT_EVENT_ERROR,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));

    if (bits & GATT_EVENT_ERROR) {
        ESP_LOGE(TAG, "Connection to %s failed", mac_str);
        xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
        free_connection(conn);
        xSemaphoreGiveRecursive(s_gatt_mutex);
        update_connection_count();
        return ESP_FAIL;
    }

    if (!(bits & GATT_EVENT_CONNECT_DONE)) {
        ESP_LOGE(TAG, "Connection to %s timed out", mac_str);
        ble_gap_conn_cancel();
        xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
        free_connection(conn);
        xSemaphoreGiveRecursive(s_gatt_mutex);
        update_connection_count();
        return ESP_ERR_TIMEOUT;
    }

    update_connection_count();
    ESP_LOGI(TAG, "Connected to %s (handle: 0x%04X)", mac_str, conn->conn_handle);

    return ESP_OK;
}

esp_err_t ble_gatt_disconnect(uint16_t conn_handle)
{
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        ESP_LOGE(TAG, "Connection handle 0x%04X not found", conn_handle);
        return ESP_ERR_NOT_FOUND;
    }

    char mac_str[18];
    ble_mac_to_str(conn->mac, mac_str);
    ESP_LOGI(TAG, "Disconnecting from %s (handle: 0x%04X)", mac_str, conn_handle);

    conn->state = BLE_CONN_STATE_DISCONNECTING;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    /* Initiate disconnect (NimBLE API, no mutex needed) */
    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "Failed to terminate connection: %d (%s)", rc, ble_hs_err_to_str(rc));
    }

    return ESP_OK;
}

esp_err_t ble_gatt_disconnect_by_mac(const uint8_t *mac)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    gatt_connection_t *conn = find_connection_by_mac(mac);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t handle = conn->conn_handle;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    return ble_gatt_disconnect(handle);
}

esp_err_t ble_gatt_disconnect_all(void)
{
    ESP_LOGI(TAG, "Disconnecting all connections...");

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    for (int i = 0; i < BLE_GATT_MAX_CONNECTIONS; i++) {
        if (s_connections[i].in_use &&
            s_connections[i].state != BLE_CONN_STATE_DISCONNECTED) {
            s_connections[i].state = BLE_CONN_STATE_DISCONNECTING;
            ble_gap_terminate(s_connections[i].conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }

    xSemaphoreGiveRecursive(s_gatt_mutex);

    return ESP_OK;
}

bool ble_gatt_is_connected(const uint8_t *mac)
{
    if (mac == NULL) {
        return false;
    }

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    gatt_connection_t *conn = find_connection_by_mac(mac);
    bool connected = (conn != NULL) &&
                     (conn->state == BLE_CONN_STATE_CONNECTED ||
                      conn->state == BLE_CONN_STATE_DISCOVERING ||
                      conn->state == BLE_CONN_STATE_READY);
    xSemaphoreGiveRecursive(s_gatt_mutex);

    return connected;
}

bool ble_gatt_is_handle_connected(uint16_t conn_handle)
{
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    bool connected = (conn != NULL) &&
                     (conn->state == BLE_CONN_STATE_CONNECTED ||
                      conn->state == BLE_CONN_STATE_DISCOVERING ||
                      conn->state == BLE_CONN_STATE_READY);
    xSemaphoreGiveRecursive(s_gatt_mutex);

    return connected;
}

uint16_t ble_gatt_get_conn_handle(const uint8_t *mac)
{
    if (mac == NULL) {
        return BLE_GATT_INVALID_CONN_HANDLE;
    }

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    gatt_connection_t *conn = find_connection_by_mac(mac);
    uint16_t handle = (conn != NULL) ? conn->conn_handle : BLE_GATT_INVALID_CONN_HANDLE;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    return handle;
}

uint8_t ble_gatt_get_connection_count(void)
{
    return s_connection_count;
}

esp_err_t ble_gatt_get_connection_info(uint16_t conn_handle, ble_gatt_connection_t *conn_info)
{
    if (conn_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(conn_info->mac, conn->mac, BLE_MAC_ADDR_LEN);
    conn_info->addr_type = conn->addr_type;
    conn_info->conn_handle = conn->conn_handle;
    conn_info->state = conn->state;
    conn_info->encrypted = conn->encrypted;
    conn_info->mtu = conn->mtu;
    conn_info->service_count = conn->service_count;
    conn_info->auto_reconnect = conn->auto_reconnect;
    conn_info->rssi = BLE_RSSI_INVALID;  /* Updated below */

    xSemaphoreGiveRecursive(s_gatt_mutex);

    /* Get current RSSI */
    int8_t rssi;
    if (ble_gatt_get_rssi(conn_handle, &rssi) == ESP_OK) {
        conn_info->rssi = rssi;
    }

    return ESP_OK;
}

esp_err_t ble_gatt_set_auto_reconnect(uint16_t conn_handle, bool enable)
{
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    conn->auto_reconnect = enable;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    ESP_LOGI(TAG, "Auto-reconnect %s for handle 0x%04X",
             enable ? "enabled" : "disabled", conn_handle);

    return ESP_OK;
}

/* ============================================================================
 * Service Discovery
 * ============================================================================ */

esp_err_t ble_gatt_discover_all_services(uint16_t conn_handle)
{
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (conn->state != BLE_CONN_STATE_CONNECTED &&
        conn->state != BLE_CONN_STATE_READY) {
        ESP_LOGE(TAG, "Invalid state for discovery: %d", conn->state);
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Discovering all services (handle: 0x%04X)...", conn_handle);
    conn->state = BLE_CONN_STATE_DISCOVERING;
    conn->service_count = 0;
    memset(conn->services, 0, sizeof(conn->services));
    xSemaphoreGiveRecursive(s_gatt_mutex);

    /* Clear event bits */
    xEventGroupClearBits(s_gatt_events, GATT_EVENT_DISC_DONE | GATT_EVENT_ERROR);

    /* Start service discovery */
    int rc = ble_gattc_disc_all_svcs(conn_handle, disc_svc_cb, conn);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start service discovery: %d (%s)", rc, ble_hs_err_to_str(rc));
        xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
        conn->state = BLE_CONN_STATE_CONNECTED;
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_FAIL;
    }

    /* Wait for discovery to complete */
    EventBits_t bits = xEventGroupWaitBits(s_gatt_events,
                                            GATT_EVENT_DISC_DONE | GATT_EVENT_ERROR,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(s_config.op_timeout_ms));

    if (bits & GATT_EVENT_ERROR) {
        ESP_LOGE(TAG, "Service discovery failed");
        return ESP_FAIL;
    }

    if (!(bits & GATT_EVENT_DISC_DONE)) {
        ESP_LOGE(TAG, "Service discovery timed out");
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    conn->state = BLE_CONN_STATE_READY;
    conn->services_discovered = true;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    ESP_LOGI(TAG, "Service discovery complete: %d services found", conn->service_count);

    return ESP_OK;
}

esp_err_t ble_gatt_discover_service_by_uuid(uint16_t conn_handle, const ble_gatt_uuid_t *uuid)
{
    if (uuid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (conn->state != BLE_CONN_STATE_CONNECTED &&
        conn->state != BLE_CONN_STATE_READY) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    char uuid_str[37];
    ble_gatt_uuid_to_str(uuid, uuid_str);
    ESP_LOGI(TAG, "Discovering service %s (handle: 0x%04X)...", uuid_str, conn_handle);

    conn->state = BLE_CONN_STATE_DISCOVERING;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    /* Convert UUID to NimBLE format */
    ble_uuid_any_t nimble_uuid;
    convert_to_nimble_uuid(uuid, &nimble_uuid);

    /* Clear event bits */
    xEventGroupClearBits(s_gatt_events, GATT_EVENT_DISC_DONE | GATT_EVENT_ERROR);

    /* Start discovery */
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &nimble_uuid.u, disc_svc_cb, conn);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start service discovery: %d (%s)", rc, ble_hs_err_to_str(rc));
        xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
        conn->state = BLE_CONN_STATE_CONNECTED;
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_FAIL;
    }

    /* Wait for discovery */
    EventBits_t bits = xEventGroupWaitBits(s_gatt_events,
                                            GATT_EVENT_DISC_DONE | GATT_EVENT_ERROR,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(s_config.op_timeout_ms));

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    conn->state = BLE_CONN_STATE_READY;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    if (bits & GATT_EVENT_ERROR) {
        return ESP_FAIL;
    }

    if (!(bits & GATT_EVENT_DISC_DONE)) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t ble_gatt_discover_characteristics(uint16_t conn_handle,
                                             uint16_t start_handle,
                                             uint16_t end_handle)
{
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    bool connected = (conn->state == BLE_CONN_STATE_CONNECTED ||
                      conn->state == BLE_CONN_STATE_DISCOVERING ||
                      conn->state == BLE_CONN_STATE_READY);
    xSemaphoreGiveRecursive(s_gatt_mutex);

    if (!connected) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Discovering characteristics (0x%04X-0x%04X)...", start_handle, end_handle);

    /* Clear event bits */
    xEventGroupClearBits(s_gatt_events, GATT_EVENT_DISC_DONE | GATT_EVENT_ERROR);

    /* Start characteristic discovery */
    int rc = ble_gattc_disc_all_chrs(conn_handle, start_handle, end_handle, disc_chr_cb, conn);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start characteristic discovery: %d (%s)", rc, ble_hs_err_to_str(rc));
        return ESP_FAIL;
    }

    /* Wait for discovery */
    EventBits_t bits = xEventGroupWaitBits(s_gatt_events,
                                            GATT_EVENT_DISC_DONE | GATT_EVENT_ERROR,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(s_config.op_timeout_ms));

    if (bits & GATT_EVENT_ERROR) {
        return ESP_FAIL;
    }

    if (!(bits & GATT_EVENT_DISC_DONE)) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t ble_gatt_discover_descriptors(uint16_t conn_handle,
                                         uint16_t start_handle,
                                         uint16_t end_handle)
{
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    bool connected = (conn->state == BLE_CONN_STATE_CONNECTED ||
                      conn->state == BLE_CONN_STATE_DISCOVERING ||
                      conn->state == BLE_CONN_STATE_READY);
    xSemaphoreGiveRecursive(s_gatt_mutex);

    if (!connected) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Discovering descriptors (0x%04X-0x%04X)...", start_handle, end_handle);

    /* Clear event bits */
    xEventGroupClearBits(s_gatt_events, GATT_EVENT_DISC_DONE | GATT_EVENT_ERROR);

    /* Start descriptor discovery */
    int rc = ble_gattc_disc_all_dscs(conn_handle, start_handle, end_handle, disc_dsc_cb, conn);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start descriptor discovery: %d (%s)", rc, ble_hs_err_to_str(rc));
        return ESP_FAIL;
    }

    /* Wait for discovery */
    EventBits_t bits = xEventGroupWaitBits(s_gatt_events,
                                            GATT_EVENT_DISC_DONE | GATT_EVENT_ERROR,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(s_config.op_timeout_ms));

    if (bits & GATT_EVENT_ERROR) {
        return ESP_FAIL;
    }

    if (!(bits & GATT_EVENT_DISC_DONE)) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t ble_gatt_get_services(uint16_t conn_handle,
                                 ble_gatt_service_t *services,
                                 uint8_t max_services,
                                 uint8_t *count)
{
    if (services == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t copy_count = conn->service_count;
    if (copy_count > max_services) {
        copy_count = max_services;
    }

    for (uint8_t i = 0; i < copy_count; i++) {
        if (conn->services[i].valid) {
            services[i].uuid = conn->services[i].uuid;
            services[i].start_handle = conn->services[i].start_handle;
            services[i].end_handle = conn->services[i].end_handle;
            services[i].primary = conn->services[i].primary;
        }
    }

    *count = copy_count;

    xSemaphoreGiveRecursive(s_gatt_mutex);

    return ESP_OK;
}

esp_err_t ble_gatt_get_characteristics(uint16_t conn_handle,
                                        uint16_t service_handle,
                                        ble_gatt_characteristic_t *characteristics,
                                        uint8_t max_chars,
                                        uint8_t *count)
{
    if (characteristics == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Find the service */
    cached_service_t *svc = NULL;
    for (uint8_t i = 0; i < conn->service_count; i++) {
        if (conn->services[i].valid && conn->services[i].start_handle == service_handle) {
            svc = &conn->services[i];
            break;
        }
    }

    if (svc == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t copy_count = svc->chr_count;
    if (copy_count > max_chars) {
        copy_count = max_chars;
    }

    for (uint8_t i = 0; i < copy_count; i++) {
        if (svc->characteristics[i].valid) {
            characteristics[i].uuid = svc->characteristics[i].uuid;
            characteristics[i].def_handle = svc->characteristics[i].def_handle;
            characteristics[i].value_handle = svc->characteristics[i].value_handle;
            characteristics[i].cccd_handle = svc->characteristics[i].cccd_handle;
            characteristics[i].properties = svc->characteristics[i].properties;
        }
    }

    *count = copy_count;

    xSemaphoreGiveRecursive(s_gatt_mutex);

    return ESP_OK;
}

/* ============================================================================
 * Read Operations
 * ============================================================================ */

esp_err_t ble_gatt_read_characteristic(uint16_t conn_handle,
                                        uint16_t attr_handle,
                                        uint8_t *buf,
                                        uint16_t *len,
                                        uint32_t timeout_ms)
{
    if (buf == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_gatt_is_handle_connected(conn_handle)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timeout_ms == 0) {
        timeout_ms = s_config.op_timeout_ms;
    }

    /* Take pending op mutex to serialize read/write operations */
    xSemaphoreTake(s_pending_op_mutex, GW_TIMEOUT_LONG_TICKS);

    ESP_LOGD(TAG, "Reading characteristic (handle: 0x%04X, attr: 0x%04X)...",
             conn_handle, attr_handle);

    /* Setup pending operation */
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    s_pending_op.conn_handle = conn_handle;
    s_pending_op.attr_handle = attr_handle;
    s_pending_op.buf = buf;
    s_pending_op.len = len;
    s_pending_op.max_len = *len;
    s_pending_op.result = ESP_OK;
    s_pending_op.in_progress = true;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    /* Clear event bits */
    xEventGroupClearBits(s_gatt_events, GATT_EVENT_READ_DONE | GATT_EVENT_ERROR);

    /* Start read operation */
    int rc = ble_gattc_read(conn_handle, attr_handle, read_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start read: %d (%s)", rc, ble_hs_err_to_str(rc));
        s_pending_op.in_progress = false;
        xSemaphoreGive(s_pending_op_mutex);
        return ESP_FAIL;
    }

    /* Wait for read to complete */
    EventBits_t bits = xEventGroupWaitBits(s_gatt_events,
                                            GATT_EVENT_READ_DONE | GATT_EVENT_ERROR,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));

    s_pending_op.in_progress = false;
    esp_err_t result = ESP_OK;

    if (bits & GATT_EVENT_ERROR) {
        ESP_LOGE(TAG, "Read failed");
        result = s_pending_op.result;
    } else if (!(bits & GATT_EVENT_READ_DONE)) {
        ESP_LOGE(TAG, "Read timed out");
        result = ESP_ERR_TIMEOUT;
    } else {
        ESP_LOGD(TAG, "Read complete: %d bytes", *len);
    }

    xSemaphoreGive(s_pending_op_mutex);
    return result;
}

esp_err_t ble_gatt_read_by_uuid(uint16_t conn_handle,
                                 uint16_t start_handle,
                                 uint16_t end_handle,
                                 const ble_gatt_uuid_t *uuid,
                                 uint8_t *buf,
                                 uint16_t *len,
                                 uint32_t timeout_ms)
{
    if (uuid == NULL || buf == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_gatt_is_handle_connected(conn_handle)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timeout_ms == 0) {
        timeout_ms = s_config.op_timeout_ms;
    }

    /* Take pending op mutex to serialize read/write operations */
    xSemaphoreTake(s_pending_op_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Setup pending operation */
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    s_pending_op.conn_handle = conn_handle;
    s_pending_op.buf = buf;
    s_pending_op.len = len;
    s_pending_op.max_len = *len;
    s_pending_op.result = ESP_OK;
    s_pending_op.in_progress = true;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    /* Convert UUID */
    ble_uuid_any_t nimble_uuid;
    convert_to_nimble_uuid(uuid, &nimble_uuid);

    /* Clear event bits */
    xEventGroupClearBits(s_gatt_events, GATT_EVENT_READ_DONE | GATT_EVENT_ERROR);

    /* Start read by UUID */
    int rc = ble_gattc_read_by_uuid(conn_handle, start_handle, end_handle,
                                     &nimble_uuid.u, read_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start read by UUID: %d (%s)", rc, ble_hs_err_to_str(rc));
        s_pending_op.in_progress = false;
        xSemaphoreGive(s_pending_op_mutex);
        return ESP_FAIL;
    }

    /* Wait for read */
    EventBits_t bits = xEventGroupWaitBits(s_gatt_events,
                                            GATT_EVENT_READ_DONE | GATT_EVENT_ERROR,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));

    s_pending_op.in_progress = false;
    esp_err_t result = ESP_OK;

    if (bits & GATT_EVENT_ERROR) {
        result = s_pending_op.result;
    } else if (!(bits & GATT_EVENT_READ_DONE)) {
        result = ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(s_pending_op_mutex);
    return result;
}

esp_err_t ble_gatt_read_descriptor(uint16_t conn_handle,
                                    uint16_t desc_handle,
                                    uint8_t *buf,
                                    uint16_t *len,
                                    uint32_t timeout_ms)
{
    /* Descriptors are read the same way as characteristics */
    return ble_gatt_read_characteristic(conn_handle, desc_handle, buf, len, timeout_ms);
}

/* ============================================================================
 * Write Operations
 * ============================================================================ */

esp_err_t ble_gatt_write_characteristic(uint16_t conn_handle,
                                         uint16_t attr_handle,
                                         const uint8_t *data,
                                         uint16_t len,
                                         uint32_t timeout_ms)
{
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_gatt_is_handle_connected(conn_handle)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timeout_ms == 0) {
        timeout_ms = s_config.op_timeout_ms;
    }

    /* Take pending op mutex to serialize read/write operations */
    xSemaphoreTake(s_pending_op_mutex, GW_TIMEOUT_LONG_TICKS);

    ESP_LOGD(TAG, "Writing characteristic (handle: 0x%04X, attr: 0x%04X, len: %d)...",
             conn_handle, attr_handle, len);

    /* Setup pending operation */
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    s_pending_op.conn_handle = conn_handle;
    s_pending_op.attr_handle = attr_handle;
    s_pending_op.result = ESP_OK;
    s_pending_op.in_progress = true;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    /* Clear event bits */
    xEventGroupClearBits(s_gatt_events, GATT_EVENT_WRITE_DONE | GATT_EVENT_ERROR);

    /* Start write with response */
    int rc = ble_gattc_write_flat(conn_handle, attr_handle, data, len, write_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start write: %d (%s)", rc, ble_hs_err_to_str(rc));
        s_pending_op.in_progress = false;
        xSemaphoreGive(s_pending_op_mutex);
        return ESP_FAIL;
    }

    /* Wait for write to complete */
    EventBits_t bits = xEventGroupWaitBits(s_gatt_events,
                                            GATT_EVENT_WRITE_DONE | GATT_EVENT_ERROR,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));

    s_pending_op.in_progress = false;
    esp_err_t result = ESP_OK;

    if (bits & GATT_EVENT_ERROR) {
        ESP_LOGE(TAG, "Write failed");
        result = s_pending_op.result;
    } else if (!(bits & GATT_EVENT_WRITE_DONE)) {
        ESP_LOGE(TAG, "Write timed out");
        result = ESP_ERR_TIMEOUT;
    } else {
        ESP_LOGD(TAG, "Write complete");
    }

    xSemaphoreGive(s_pending_op_mutex);
    return result;
}

esp_err_t ble_gatt_write_no_response(uint16_t conn_handle,
                                      uint16_t attr_handle,
                                      const uint8_t *data,
                                      uint16_t len)
{
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_gatt_is_handle_connected(conn_handle)) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Writing without response (handle: 0x%04X, attr: 0x%04X, len: %d)",
             conn_handle, attr_handle, len);

    /* Write without response */
    int rc = ble_gattc_write_no_rsp_flat(conn_handle, attr_handle, data, len);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to write (no response): %d (%s)", rc, ble_hs_err_to_str(rc));
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ble_gatt_write_descriptor(uint16_t conn_handle,
                                     uint16_t desc_handle,
                                     const uint8_t *data,
                                     uint16_t len,
                                     uint32_t timeout_ms)
{
    /* Descriptors are written the same way as characteristics */
    return ble_gatt_write_characteristic(conn_handle, desc_handle, data, len, timeout_ms);
}

/* ============================================================================
 * Notification/Indication Subscription
 * ============================================================================ */

esp_err_t ble_gatt_subscribe_notify(uint16_t conn_handle,
                                     uint16_t attr_handle,
                                     bool enable)
{
    uint16_t cccd_handle = ble_gatt_get_cccd_handle(conn_handle, attr_handle);
    if (cccd_handle == BLE_GATT_INVALID_ATTR_HANDLE) {
        ESP_LOGE(TAG, "CCCD not found for handle 0x%04X", attr_handle);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t value = enable ? BLE_GATT_CCCD_NOTIFY : 0;
    uint8_t data[2] = {value & 0xFF, (value >> 8) & 0xFF};

    ESP_LOGI(TAG, "%s notifications for handle 0x%04X (CCCD: 0x%04X)",
             enable ? "Enabling" : "Disabling", attr_handle, cccd_handle);

    return ble_gatt_write_descriptor(conn_handle, cccd_handle, data, sizeof(data),
                                      s_config.op_timeout_ms);
}

esp_err_t ble_gatt_subscribe_indicate(uint16_t conn_handle,
                                       uint16_t attr_handle,
                                       bool enable)
{
    uint16_t cccd_handle = ble_gatt_get_cccd_handle(conn_handle, attr_handle);
    if (cccd_handle == BLE_GATT_INVALID_ATTR_HANDLE) {
        ESP_LOGE(TAG, "CCCD not found for handle 0x%04X", attr_handle);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t value = enable ? BLE_GATT_CCCD_INDICATE : 0;
    uint8_t data[2] = {value & 0xFF, (value >> 8) & 0xFF};

    ESP_LOGI(TAG, "%s indications for handle 0x%04X (CCCD: 0x%04X)",
             enable ? "Enabling" : "Disabling", attr_handle, cccd_handle);

    return ble_gatt_write_descriptor(conn_handle, cccd_handle, data, sizeof(data),
                                      s_config.op_timeout_ms);
}

uint16_t ble_gatt_get_cccd_handle(uint16_t conn_handle, uint16_t chr_value_handle)
{
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn == NULL) {
        xSemaphoreGiveRecursive(s_gatt_mutex);
        return BLE_GATT_INVALID_ATTR_HANDLE;
    }

    /* Search through services and characteristics */
    for (uint8_t i = 0; i < conn->service_count; i++) {
        if (!conn->services[i].valid) continue;

        for (uint8_t j = 0; j < conn->services[i].chr_count; j++) {
            if (conn->services[i].characteristics[j].valid &&
                conn->services[i].characteristics[j].value_handle == chr_value_handle) {
                uint16_t cccd = conn->services[i].characteristics[j].cccd_handle;
                xSemaphoreGiveRecursive(s_gatt_mutex);
                return cccd;
            }
        }
    }

    xSemaphoreGiveRecursive(s_gatt_mutex);

    /* CCCD not found in cache - caller should run service discovery first */
    ESP_LOGW(TAG, "CCCD handle not found in cache for char 0x%04X, run service discovery first",
             chr_value_handle);
    return BLE_GATT_INVALID_ATTR_HANDLE;
}

/* ============================================================================
 * MTU Operations
 * ============================================================================ */

esp_err_t ble_gatt_exchange_mtu(uint16_t conn_handle, uint16_t mtu)
{
    if (mtu < BLE_GATT_DEFAULT_MTU || mtu > BLE_MTU_MAX) {
        ESP_LOGE(TAG, "Invalid MTU: %d (range: 23-%d)", mtu, BLE_MTU_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_gatt_is_handle_connected(conn_handle)) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Exchanging MTU (handle: 0x%04X, requested: %d)...", conn_handle, mtu);

    /* Clear event bits */
    xEventGroupClearBits(s_gatt_events, GATT_EVENT_MTU_DONE | GATT_EVENT_ERROR);

    /* Start MTU exchange */
    int rc = ble_gattc_exchange_mtu(conn_handle, mtu_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start MTU exchange: %d (%s)", rc, ble_hs_err_to_str(rc));
        return ESP_FAIL;
    }

    /* Wait for MTU exchange */
    EventBits_t bits = xEventGroupWaitBits(s_gatt_events,
                                            GATT_EVENT_MTU_DONE | GATT_EVENT_ERROR,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(s_config.op_timeout_ms));

    if (bits & GATT_EVENT_ERROR) {
        return ESP_FAIL;
    }

    if (!(bits & GATT_EVENT_MTU_DONE)) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

uint16_t ble_gatt_get_mtu(uint16_t conn_handle)
{
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    uint16_t mtu = (conn != NULL) ? conn->mtu : 0;
    xSemaphoreGiveRecursive(s_gatt_mutex);

    return mtu;
}

/* ============================================================================
 * Callback Registration
 * ============================================================================ */

void ble_gatt_set_connect_cb(ble_gatt_connect_cb_t cb)
{
    s_connect_cb = cb;
}

void ble_gatt_set_disconnect_cb(ble_gatt_disconnect_cb_t cb)
{
    s_disconnect_cb = cb;
}

void ble_gatt_set_notify_cb(ble_gatt_notify_cb_t cb)
{
    s_notify_cb = cb;
}

void ble_gatt_set_discovery_cb(ble_gatt_disc_cb_t cb)
{
    s_disc_cb = cb;
}

void ble_gatt_set_mtu_cb(ble_gatt_mtu_cb_t cb)
{
    s_mtu_cb = cb;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

ble_gatt_uuid_t ble_gatt_uuid16(uint16_t uuid16)
{
    ble_gatt_uuid_t uuid = {
        .type = BLE_GATT_UUID_TYPE_16,
        .value.uuid16 = uuid16
    };
    return uuid;
}

ble_gatt_uuid_t ble_gatt_uuid128(const uint8_t *uuid128)
{
    ble_gatt_uuid_t uuid = {
        .type = BLE_GATT_UUID_TYPE_128
    };
    if (uuid128 != NULL) {
        memcpy(uuid.value.uuid128, uuid128, 16);
    }
    return uuid;
}

bool ble_gatt_uuid_equal(const ble_gatt_uuid_t *uuid1, const ble_gatt_uuid_t *uuid2)
{
    if (uuid1 == NULL || uuid2 == NULL) {
        return false;
    }

    if (uuid1->type != uuid2->type) {
        return false;
    }

    if (uuid1->type == BLE_GATT_UUID_TYPE_16) {
        return uuid1->value.uuid16 == uuid2->value.uuid16;
    } else {
        return memcmp(uuid1->value.uuid128, uuid2->value.uuid128, 16) == 0;
    }
}

char *ble_gatt_uuid_to_str(const ble_gatt_uuid_t *uuid, char *str)
{
    if (uuid == NULL || str == NULL) {
        if (str) str[0] = '\0';
        return str;
    }

    if (uuid->type == BLE_GATT_UUID_TYPE_16) {
        snprintf(str, 7, "0x%04X", uuid->value.uuid16);
    } else {
        /* Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
        const uint8_t *u = uuid->value.uuid128;
        snprintf(str, 37,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8],
                 u[7], u[6], u[5], u[4], u[3], u[2], u[1], u[0]);
    }

    return str;
}

esp_err_t ble_gatt_get_rssi(uint16_t conn_handle, int8_t *rssi)
{
    if (rssi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_gatt_is_handle_connected(conn_handle)) {
        return ESP_ERR_INVALID_STATE;
    }

    int rc = ble_gap_conn_rssi(conn_handle, rssi);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to get RSSI: %d (%s)", rc, ble_hs_err_to_str(rc));
        *rssi = BLE_RSSI_INVALID;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ble_gatt_client_self_test(void)
{
    ESP_LOGI(TAG, "Running BLE GATT client self-test...");

    /* Check initialization */
    if (s_gatt_state == BLE_GATT_STATE_UNINITIALIZED) {
        ESP_LOGE(TAG, "GATT client not initialized");
        return ESP_FAIL;
    }

    /* Check mutex */
    if (s_gatt_mutex == NULL) {
        ESP_LOGE(TAG, "GATT mutex not created");
        return ESP_FAIL;
    }

    /* Check event group */
    if (s_gatt_events == NULL) {
        ESP_LOGE(TAG, "GATT event group not created");
        return ESP_FAIL;
    }

    /* Check BLE manager */
    if (!ble_manager_is_initialized()) {
        ESP_LOGE(TAG, "BLE manager not initialized");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE GATT client self-test PASSED (connections: %d/%d)",
             s_connection_count, BLE_GATT_MAX_CONNECTIONS);
    return ESP_OK;
}

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

static void set_gatt_state(ble_gatt_state_t state)
{
    s_gatt_state = state;
    ESP_LOGD(TAG, "State: %s", ble_gatt_client_get_state_str());
}

/**
 * @brief Find connection by handle (requires s_gatt_mutex held or acquires it)
 * @note Uses recursive mutex — safe to call when mutex already held
 */
static gatt_connection_t *find_connection_by_handle(uint16_t conn_handle)
{
    gatt_connection_t *result = NULL;

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    for (int i = 0; i < BLE_GATT_MAX_CONNECTIONS; i++) {
        if (s_connections[i].in_use && s_connections[i].conn_handle == conn_handle) {
            result = &s_connections[i];
            break;
        }
    }
    xSemaphoreGiveRecursive(s_gatt_mutex);
    return result;
}

/**
 * @brief Find connection by MAC (requires s_gatt_mutex held or acquires it)
 * @note Uses recursive mutex — safe to call when mutex already held
 */
static gatt_connection_t *find_connection_by_mac(const uint8_t *mac)
{
    gatt_connection_t *result = NULL;

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    for (int i = 0; i < BLE_GATT_MAX_CONNECTIONS; i++) {
        if (s_connections[i].in_use && ble_mac_equal(s_connections[i].mac, mac)) {
            result = &s_connections[i];
            break;
        }
    }
    xSemaphoreGiveRecursive(s_gatt_mutex);
    return result;
}

static gatt_connection_t *allocate_connection(void)
{
    for (int i = 0; i < BLE_GATT_MAX_CONNECTIONS; i++) {
        if (!s_connections[i].in_use) {
            memset(&s_connections[i], 0, sizeof(gatt_connection_t));
            s_connections[i].in_use = true;
            s_connections[i].conn_handle = BLE_GATT_INVALID_CONN_HANDLE;
            return &s_connections[i];
        }
    }
    return NULL;
}

static void free_connection(gatt_connection_t *conn)
{
    if (conn != NULL) {
        memset(conn, 0, sizeof(gatt_connection_t));
        conn->in_use = false;
    }
}

static void update_connection_count(void)
{
    uint8_t count = 0;
    for (int i = 0; i < BLE_GATT_MAX_CONNECTIONS; i++) {
        if (s_connections[i].in_use &&
            s_connections[i].state != BLE_CONN_STATE_DISCONNECTED) {
            count++;
        }
    }
    s_connection_count = count;

    /* Update GATT state */
    if (count > 0) {
        set_gatt_state(BLE_GATT_STATE_CONNECTED);
    } else if (s_gatt_state == BLE_GATT_STATE_CONNECTED) {
        set_gatt_state(BLE_GATT_STATE_INITIALIZED);
    }
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    gatt_connection_t *conn;
    char mac_str[18];

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGD(TAG, "GAP connect event: status=%d", event->connect.status);

            /* Protect s_connections access with mutex to prevent race conditions */
            xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

            conn = NULL;

            if (event->connect.status == 0) {
                /* Connection successful - get peer address from connection descriptor
                 * to correctly identify which connection this event belongs to */
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                    /* Try to find connection by OTA address first */
                    conn = find_connection_by_mac(desc.peer_ota_addr.val);
                    if (conn == NULL) {
                        /* Fallback to ID address */
                        conn = find_connection_by_mac(desc.peer_id_addr.val);
                    }
                }
            }

            /* Fallback: find first CONNECTING connection (for failures or if above didn't work) */
            if (conn == NULL) {
                int connecting_count = 0;
                for (int i = 0; i < BLE_GATT_MAX_CONNECTIONS; i++) {
                    if (s_connections[i].in_use &&
                        s_connections[i].state == BLE_CONN_STATE_CONNECTING) {
                        if (conn == NULL) {
                            conn = &s_connections[i];
                        }
                        connecting_count++;
                    }
                }
                if (connecting_count > 1) {
                    ESP_LOGW(TAG, "Multiple connections in CONNECTING state (%d), "
                             "assignment may be incorrect for failed connects", connecting_count);
                }
            }

            if (conn == NULL) {
                xSemaphoreGiveRecursive(s_gatt_mutex);
                ESP_LOGW(TAG, "Connection event for unknown device");
                xEventGroupSetBits(s_gatt_events, GATT_EVENT_ERROR);
                break;
            }

            if (event->connect.status == 0) {
                /* Connection successful - mutex already held */
                conn->conn_handle = event->connect.conn_handle;
                conn->state = BLE_CONN_STATE_CONNECTED;
                xSemaphoreGiveRecursive(s_gatt_mutex);

                ble_mac_to_str(conn->mac, mac_str);
                ESP_LOGI(TAG, "Connected to %s (handle: 0x%04X)",
                         mac_str, conn->conn_handle);

                /* Auto MTU exchange if configured */
                if (s_config.auto_mtu_exchange) {
                    ble_gattc_exchange_mtu(conn->conn_handle, mtu_cb, NULL);
                }

                /* Auto service discovery if configured */
                if (s_config.auto_discover_services) {
                    ble_gattc_disc_all_svcs(conn->conn_handle, disc_svc_cb, conn);
                }

                xEventGroupSetBits(s_gatt_events, GATT_EVENT_CONNECT_DONE);

                /* Invoke callback */
                if (s_connect_cb != NULL) {
                    s_connect_cb(conn->mac, true, conn->conn_handle);
                }
            } else {
                /* Connection failed - mutex already held from search */
                /* Copy MAC before freeing connection for callback use */
                uint8_t mac_copy[BLE_MAC_ADDR_LEN];
                memcpy(mac_copy, conn->mac, BLE_MAC_ADDR_LEN);

                ble_mac_to_str(conn->mac, mac_str);
                ESP_LOGE(TAG, "Connection to %s failed: %d", mac_str, event->connect.status);

                free_connection(conn);
                xSemaphoreGiveRecursive(s_gatt_mutex);

                xEventGroupSetBits(s_gatt_events, GATT_EVENT_ERROR);

                /* Invoke callback with copied MAC since conn is now freed */
                if (s_connect_cb != NULL) {
                    s_connect_cb(mac_copy, false, 0);
                }
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "GAP disconnect event: reason=%d", event->disconnect.reason);

            /* Protect s_connections access with mutex to prevent race conditions */
            xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
            conn = find_connection_by_handle(event->disconnect.conn.conn_handle);
            if (conn != NULL) {
                ble_mac_to_str(conn->mac, mac_str);
                ESP_LOGI(TAG, "Disconnected from %s (reason: 0x%02X)",
                         mac_str, event->disconnect.reason);

                /* Copy data before freeing connection */
                uint8_t mac[BLE_MAC_ADDR_LEN];
                memcpy(mac, conn->mac, BLE_MAC_ADDR_LEN);
                bool auto_reconnect = conn->auto_reconnect;
                ble_addr_type_t addr_type = conn->addr_type;

                conn->state = BLE_CONN_STATE_DISCONNECTED;
                free_connection(conn);
                xSemaphoreGiveRecursive(s_gatt_mutex);

                update_connection_count();

                xEventGroupSetBits(s_gatt_events, GATT_EVENT_DISCONNECT_DONE);

                /* Invoke callback */
                if (s_disconnect_cb != NULL) {
                    s_disconnect_cb(mac, event->disconnect.reason);
                }

                /* Auto-reconnect if enabled - use timer to avoid deadlock */
                if (auto_reconnect && s_reconnect_timer != NULL) {
                    ESP_LOGI(TAG, "Scheduling auto-reconnect to %s...", mac_str);
                    memcpy(s_reconnect_info.mac, mac, BLE_MAC_ADDR_LEN);
                    s_reconnect_info.addr_type = addr_type;
                    s_reconnect_info.pending = true;
                    /* Start one-shot timer with 100ms delay */
                    esp_timer_start_once(s_reconnect_timer, BLE_GATT_RECONNECT_DELAY_US);  /* 100ms in microseconds */
                }
            } else {
                xSemaphoreGiveRecursive(s_gatt_mutex);
                ESP_LOGD(TAG, "Disconnect event for unknown connection handle 0x%04X",
                         event->disconnect.conn.conn_handle);
            }
            break;

        case BLE_GAP_EVENT_NOTIFY_RX:
            ESP_LOGD(TAG, "Notification received (handle: 0x%04X, attr: 0x%04X, len: %d)",
                     event->notify_rx.conn_handle, event->notify_rx.attr_handle,
                     OS_MBUF_PKTLEN(event->notify_rx.om));

            if (s_notify_cb != NULL) {
                /* Extract data from mbuf */
                uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
                uint8_t *data = mem_alloc(len, MEM_CAP_DEFAULT);
                if (data != NULL) {
                    os_mbuf_copydata(event->notify_rx.om, 0, len, data);
                    s_notify_cb(event->notify_rx.conn_handle,
                                event->notify_rx.attr_handle, data, len);
                    mem_ng_free(data);
                } else {
                    ESP_LOGW(TAG, "Failed to allocate %d bytes for notification data", len);
                }
            }
            break;

        case BLE_GAP_EVENT_MTU:
            xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
            conn = find_connection_by_handle(event->mtu.conn_handle);
            if (conn != NULL) {
                conn->mtu = event->mtu.value;
            }
            xSemaphoreGiveRecursive(s_gatt_mutex);

            if (conn != NULL) {
                ESP_LOGI(TAG, "MTU updated: %d (handle: 0x%04X)",
                         event->mtu.value, event->mtu.conn_handle);
                xEventGroupSetBits(s_gatt_events, GATT_EVENT_MTU_DONE);
                if (s_mtu_cb != NULL) {
                    s_mtu_cb(event->mtu.conn_handle, event->mtu.value);
                }
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE: {
            xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
            conn = find_connection_by_handle(event->enc_change.conn_handle);
            bool encrypted = false;
            if (conn != NULL) {
                conn->encrypted = (event->enc_change.status == 0);
                encrypted = conn->encrypted;
            }
            xSemaphoreGiveRecursive(s_gatt_mutex);

            if (conn != NULL) {
                ESP_LOGI(TAG, "Encryption %s (handle: 0x%04X)",
                         encrypted ? "enabled" : "failed",
                         event->enc_change.conn_handle);
            }
            break;
        }

        default:
            ESP_LOGD(TAG, "Unhandled GAP event: %d", event->type);
            break;
    }

    return 0;
}

static int disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    gatt_connection_t *conn = (gatt_connection_t *)arg;

    if (error->status == BLE_HS_EDONE) {
        /* Discovery complete */
        ESP_LOGD(TAG, "Service discovery complete");
        xEventGroupSetBits(s_gatt_events, GATT_EVENT_DISC_DONE);
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "Service discovery error: %d", error->status);
        xEventGroupSetBits(s_gatt_events, GATT_EVENT_ERROR);
        return 0;
    }

    if (service == NULL || conn == NULL) {
        return 0;
    }

    /* Store discovered service */
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    if (conn->service_count < MAX_CACHED_SERVICES) {
        cached_service_t *svc = &conn->services[conn->service_count];
        convert_nimble_uuid(&service->uuid, &svc->uuid);
        svc->start_handle = service->start_handle;
        svc->end_handle = service->end_handle;
        svc->primary = true;
        svc->valid = true;
        svc->chr_count = 0;
        conn->service_count++;

        char uuid_str[37];
        ble_gatt_uuid_to_str(&svc->uuid, uuid_str);
        ESP_LOGD(TAG, "Found service: %s (0x%04X-0x%04X)",
                 uuid_str, svc->start_handle, svc->end_handle);

        /* Automatically discover characteristics for this service */
        ble_gattc_disc_all_chrs(conn_handle, svc->start_handle, svc->end_handle,
                                 disc_chr_cb, conn);
    }

    xSemaphoreGiveRecursive(s_gatt_mutex);

    return 0;
}

static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    gatt_connection_t *conn = (gatt_connection_t *)arg;

    if (error->status == BLE_HS_EDONE) {
        ESP_LOGD(TAG, "Characteristic discovery complete");
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "Characteristic discovery error: %d", error->status);
        return 0;
    }

    if (chr == NULL || conn == NULL) {
        return 0;
    }

    /* Find the service this characteristic belongs to */
    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

    cached_service_t *svc = find_service_for_char(conn, chr->def_handle);
    if (svc != NULL && svc->chr_count < MAX_CACHED_CHARS) {
        cached_characteristic_t *cached_chr = &svc->characteristics[svc->chr_count];
        convert_nimble_uuid(&chr->uuid, &cached_chr->uuid);
        cached_chr->def_handle = chr->def_handle;
        cached_chr->value_handle = chr->val_handle;
        cached_chr->properties = chr->properties;
        cached_chr->cccd_handle = 0;  /* Will be discovered separately */
        cached_chr->valid = true;
        svc->chr_count++;

        char uuid_str[37];
        ble_gatt_uuid_to_str(&cached_chr->uuid, uuid_str);
        ESP_LOGD(TAG, "Found characteristic: %s (val: 0x%04X, props: 0x%02X)",
                 uuid_str, cached_chr->value_handle, cached_chr->properties);

        /* If characteristic supports notify/indicate, discover descriptors */
        if (cached_chr->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) {
            uint16_t end_handle = svc->end_handle;
            /* Check if there's a next characteristic */
            if (svc->chr_count > 0) {
                for (uint8_t i = 0; i < svc->chr_count; i++) {
                    if (svc->characteristics[i].def_handle > cached_chr->value_handle &&
                        svc->characteristics[i].def_handle < end_handle) {
                        end_handle = svc->characteristics[i].def_handle - 1;
                    }
                }
            }
            ble_gattc_disc_all_dscs(conn_handle, cached_chr->value_handle, end_handle,
                                     disc_dsc_cb, conn);
        }
    }

    xSemaphoreGiveRecursive(s_gatt_mutex);

    return 0;
}

static int disc_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn_handle;
    gatt_connection_t *conn = (gatt_connection_t *)arg;

    if (error->status == BLE_HS_EDONE) {
        return 0;
    }

    if (error->status != 0) {
        return 0;
    }

    if (dsc == NULL || conn == NULL) {
        return 0;
    }

    /* Check if this is a CCCD */
    if (dsc->uuid.u.type == BLE_GATT_UUID_TYPE_16) {
        uint16_t uuid16 = ((ble_uuid16_t *)&dsc->uuid)->value;
        if (uuid16 == BLE_UUID_CCCD) {
            /* Find and update the characteristic */
            xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);

            for (uint8_t i = 0; i < conn->service_count; i++) {
                if (!conn->services[i].valid) continue;

                for (uint8_t j = 0; j < conn->services[i].chr_count; j++) {
                    if (conn->services[i].characteristics[j].valid &&
                        conn->services[i].characteristics[j].value_handle == chr_val_handle) {
                        conn->services[i].characteristics[j].cccd_handle = dsc->handle;
                        ESP_LOGD(TAG, "Found CCCD: 0x%04X for char 0x%04X",
                                 dsc->handle, chr_val_handle);
                        xSemaphoreGiveRecursive(s_gatt_mutex);
                        return 0;
                    }
                }
            }

            xSemaphoreGiveRecursive(s_gatt_mutex);
        }
    }

    return 0;
}

static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (error->status == BLE_HS_EDONE) {
        xEventGroupSetBits(s_gatt_events, GATT_EVENT_READ_DONE);
        return 0;
    }

    if (error->status != 0) {
        ESP_LOGE(TAG, "Read error: %d", error->status);
        s_pending_op.result = ESP_FAIL;
        xEventGroupSetBits(s_gatt_events, GATT_EVENT_ERROR);
        return 0;
    }

    if (attr == NULL || !s_pending_op.in_progress) {
        return 0;
    }

    /* Copy data to pending operation buffer */
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len > s_pending_op.max_len) {
        len = s_pending_op.max_len;
    }

    if (s_pending_op.buf != NULL && len > 0) {
        os_mbuf_copydata(attr->om, 0, len, s_pending_op.buf);
    }

    if (s_pending_op.len != NULL) {
        *s_pending_op.len = len;
    }

    s_pending_op.result = ESP_OK;
    xEventGroupSetBits(s_gatt_events, GATT_EVENT_READ_DONE);

    return 0;
}

static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;

    if (error->status != 0) {
        ESP_LOGE(TAG, "Write error: %d", error->status);
        s_pending_op.result = ESP_FAIL;
        xEventGroupSetBits(s_gatt_events, GATT_EVENT_ERROR);
        return 0;
    }

    s_pending_op.result = ESP_OK;
    xEventGroupSetBits(s_gatt_events, GATT_EVENT_WRITE_DONE);

    return 0;
}

static int mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t mtu, void *arg)
{
    (void)arg;

    if (error->status != 0) {
        ESP_LOGW(TAG, "MTU exchange error: %d", error->status);
        xEventGroupSetBits(s_gatt_events, GATT_EVENT_ERROR);
        return 0;
    }

    xSemaphoreTakeRecursive(s_gatt_mutex, GW_TIMEOUT_LONG_TICKS);
    gatt_connection_t *conn = find_connection_by_handle(conn_handle);
    if (conn != NULL) {
        conn->mtu = mtu;
    }
    xSemaphoreGiveRecursive(s_gatt_mutex);

    ESP_LOGI(TAG, "MTU negotiated: %d", mtu);
    xEventGroupSetBits(s_gatt_events, GATT_EVENT_MTU_DONE);

    if (s_mtu_cb != NULL) {
        s_mtu_cb(conn_handle, mtu);
    }

    return 0;
}

static void convert_nimble_uuid(const ble_uuid_any_t *nimble_uuid, ble_gatt_uuid_t *out_uuid)
{
    if (nimble_uuid == NULL || out_uuid == NULL) {
        return;
    }

    /* Convert from NimBLE's ble_uuid_any_t to our ble_gatt_uuid_t */
    if (nimble_uuid->u.type == BLE_UUID_TYPE_16) {
        out_uuid->type = BLE_GATT_UUID_TYPE_16;
        out_uuid->value.uuid16 = nimble_uuid->u16.value;
    } else if (nimble_uuid->u.type == BLE_UUID_TYPE_128) {
        out_uuid->type = BLE_GATT_UUID_TYPE_128;
        memcpy(out_uuid->value.uuid128, nimble_uuid->u128.value, 16);
    } else {
        /* Default to 16-bit */
        out_uuid->type = BLE_GATT_UUID_TYPE_16;
        out_uuid->value.uuid16 = 0;
    }
}

static void convert_to_nimble_uuid(const ble_gatt_uuid_t *uuid, ble_uuid_any_t *nimble_uuid)
{
    if (uuid == NULL || nimble_uuid == NULL) {
        return;
    }

    if (uuid->type == BLE_GATT_UUID_TYPE_16) {
        nimble_uuid->u16.u.type = BLE_UUID_TYPE_16;
        nimble_uuid->u16.value = uuid->value.uuid16;
    } else {
        nimble_uuid->u128.u.type = BLE_UUID_TYPE_128;
        memcpy(nimble_uuid->u128.value, uuid->value.uuid128, 16);
    }
}

static cached_service_t *find_service_for_char(gatt_connection_t *conn, uint16_t char_handle)
{
    if (conn == NULL) {
        return NULL;
    }

    for (uint8_t i = 0; i < conn->service_count; i++) {
        if (conn->services[i].valid &&
            char_handle >= conn->services[i].start_handle &&
            char_handle <= conn->services[i].end_handle) {
            return &conn->services[i];
        }
    }

    return NULL;
}

/**
 * @brief Timer callback for auto-reconnect
 *
 * Called asynchronously after disconnect to avoid blocking the NimBLE host task.
 * This ensures ble_gatt_connect() can safely wait for events.
 */
static void reconnect_timer_callback(void *arg)
{
    (void)arg;

    if (!s_reconnect_info.pending) {
        return;
    }

    s_reconnect_info.pending = false;

    char mac_str[18];
    ble_mac_to_str(s_reconnect_info.mac, mac_str);
    ESP_LOGI(TAG, "Auto-reconnecting to %s...", mac_str);

    esp_err_t ret = ble_gatt_connect(s_reconnect_info.mac, s_reconnect_info.addr_type, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Auto-reconnect to %s failed: %s", mac_str, esp_err_to_name(ret));
    }
}

#else /* CONFIG_BT_SCANNER_ENABLED */

/* Stub implementations when BLE is disabled */
static const char *TAG = "BLE_GATT";

esp_err_t ble_gatt_client_init(void)
{
    ESP_LOGW(TAG, "BLE disabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_client_init_with_config(const ble_gatt_client_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_client_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_gatt_client_is_initialized(void)
{
    return false;
}

ble_gatt_state_t ble_gatt_client_get_state(void)
{
    return BLE_GATT_STATE_UNINITIALIZED;
}

const char *ble_gatt_client_get_state_str(void)
{
    return "DISABLED";
}

esp_err_t ble_gatt_connect(const uint8_t *mac, ble_addr_type_t addr_type, uint32_t timeout_ms)
{
    (void)mac; (void)addr_type; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_disconnect(uint16_t conn_handle)
{
    (void)conn_handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_disconnect_by_mac(const uint8_t *mac)
{
    (void)mac;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_disconnect_all(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_gatt_is_connected(const uint8_t *mac)
{
    (void)mac;
    return false;
}

bool ble_gatt_is_handle_connected(uint16_t conn_handle)
{
    (void)conn_handle;
    return false;
}

uint16_t ble_gatt_get_conn_handle(const uint8_t *mac)
{
    (void)mac;
    return BLE_GATT_INVALID_CONN_HANDLE;
}

uint8_t ble_gatt_get_connection_count(void)
{
    return 0;
}

esp_err_t ble_gatt_get_connection_info(uint16_t conn_handle, ble_gatt_connection_t *conn_info)
{
    (void)conn_handle; (void)conn_info;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_set_auto_reconnect(uint16_t conn_handle, bool enable)
{
    (void)conn_handle; (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_discover_all_services(uint16_t conn_handle)
{
    (void)conn_handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_discover_service_by_uuid(uint16_t conn_handle, const ble_gatt_uuid_t *uuid)
{
    (void)conn_handle; (void)uuid;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_discover_characteristics(uint16_t conn_handle,
                                             uint16_t start_handle, uint16_t end_handle)
{
    (void)conn_handle; (void)start_handle; (void)end_handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_discover_descriptors(uint16_t conn_handle,
                                         uint16_t start_handle, uint16_t end_handle)
{
    (void)conn_handle; (void)start_handle; (void)end_handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_get_services(uint16_t conn_handle, ble_gatt_service_t *services,
                                 uint8_t max_services, uint8_t *count)
{
    (void)conn_handle; (void)services; (void)max_services; (void)count;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_get_characteristics(uint16_t conn_handle, uint16_t service_handle,
                                        ble_gatt_characteristic_t *characteristics,
                                        uint8_t max_chars, uint8_t *count)
{
    (void)conn_handle; (void)service_handle; (void)characteristics;
    (void)max_chars; (void)count;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_read_characteristic(uint16_t conn_handle, uint16_t attr_handle,
                                        uint8_t *buf, uint16_t *len, uint32_t timeout_ms)
{
    (void)conn_handle; (void)attr_handle; (void)buf; (void)len; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_read_by_uuid(uint16_t conn_handle, uint16_t start_handle,
                                 uint16_t end_handle, const ble_gatt_uuid_t *uuid,
                                 uint8_t *buf, uint16_t *len, uint32_t timeout_ms)
{
    (void)conn_handle; (void)start_handle; (void)end_handle; (void)uuid;
    (void)buf; (void)len; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_read_descriptor(uint16_t conn_handle, uint16_t desc_handle,
                                    uint8_t *buf, uint16_t *len, uint32_t timeout_ms)
{
    (void)conn_handle; (void)desc_handle; (void)buf; (void)len; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_write_characteristic(uint16_t conn_handle, uint16_t attr_handle,
                                         const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    (void)conn_handle; (void)attr_handle; (void)data; (void)len; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_write_no_response(uint16_t conn_handle, uint16_t attr_handle,
                                      const uint8_t *data, uint16_t len)
{
    (void)conn_handle; (void)attr_handle; (void)data; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_write_descriptor(uint16_t conn_handle, uint16_t desc_handle,
                                     const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    (void)conn_handle; (void)desc_handle; (void)data; (void)len; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_subscribe_notify(uint16_t conn_handle, uint16_t attr_handle, bool enable)
{
    (void)conn_handle; (void)attr_handle; (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_subscribe_indicate(uint16_t conn_handle, uint16_t attr_handle, bool enable)
{
    (void)conn_handle; (void)attr_handle; (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

uint16_t ble_gatt_get_cccd_handle(uint16_t conn_handle, uint16_t chr_value_handle)
{
    (void)conn_handle; (void)chr_value_handle;
    return BLE_GATT_INVALID_ATTR_HANDLE;
}

esp_err_t ble_gatt_exchange_mtu(uint16_t conn_handle, uint16_t mtu)
{
    (void)conn_handle; (void)mtu;
    return ESP_ERR_NOT_SUPPORTED;
}

uint16_t ble_gatt_get_mtu(uint16_t conn_handle)
{
    (void)conn_handle;
    return 0;
}

void ble_gatt_set_connect_cb(ble_gatt_connect_cb_t cb) { (void)cb; }
void ble_gatt_set_disconnect_cb(ble_gatt_disconnect_cb_t cb) { (void)cb; }
void ble_gatt_set_notify_cb(ble_gatt_notify_cb_t cb) { (void)cb; }
void ble_gatt_set_discovery_cb(ble_gatt_disc_cb_t cb) { (void)cb; }
void ble_gatt_set_mtu_cb(ble_gatt_mtu_cb_t cb) { (void)cb; }

ble_gatt_uuid_t ble_gatt_uuid16(uint16_t uuid16)
{
    ble_gatt_uuid_t uuid = { .type = BLE_GATT_UUID_TYPE_16, .value.uuid16 = uuid16 };
    return uuid;
}

ble_gatt_uuid_t ble_gatt_uuid128(const uint8_t *uuid128)
{
    ble_gatt_uuid_t uuid = { .type = BLE_GATT_UUID_TYPE_128 };
    if (uuid128) memcpy(uuid.value.uuid128, uuid128, 16);
    return uuid;
}

bool ble_gatt_uuid_equal(const ble_gatt_uuid_t *uuid1, const ble_gatt_uuid_t *uuid2)
{
    (void)uuid1; (void)uuid2;
    return false;
}

char *ble_gatt_uuid_to_str(const ble_gatt_uuid_t *uuid, char *str)
{
    if (str) str[0] = '\0';
    (void)uuid;
    return str;
}

esp_err_t ble_gatt_get_rssi(uint16_t conn_handle, int8_t *rssi)
{
    (void)conn_handle;
    if (rssi) *rssi = BLE_RSSI_INVALID;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_gatt_client_self_test(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_BT_SCANNER_ENABLED */
