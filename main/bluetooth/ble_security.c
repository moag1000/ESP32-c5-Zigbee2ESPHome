/**
 * @file ble_security.c
 * @brief BLE Security, Pairing, and Bonding Implementation
 *
 * Implements comprehensive BLE security management using NimBLE stack.
 * Provides pairing, bonding, and encryption services for the ESP32-C5
 * Zigbee2MQTT Gateway.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_security.h"
#include "ble_manager.h"
#include "ble_constants.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include <string.h>
#include <time.h>

#if CONFIG_BT_SCANNER_ENABLED

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"

static const char *TAG = "BLE_SECURITY";

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

/** NVS key prefix for bond entries */
#define NVS_BOND_KEY_PREFIX     "bond_"

/** Maximum NVS key length */
#define NVS_KEY_MAX_LEN         16

/** Bond database version for migration */
#define BOND_DB_VERSION         1

/** NVS key for bond count */
#define NVS_BOND_COUNT_KEY      "bond_count"

/** NVS key for database version */
#define NVS_BOND_VERSION_KEY    "bond_version"

/** NVS key for bond data CRC */
#define NVS_BOND_CRC_KEY        "bond_crc"

/** CRC-16 polynomial (CRC-16-CCITT) */
#define CRC16_POLY              0x1021

/** Maximum concurrent pairings */
#define MAX_CONCURRENT_PAIRINGS 2

/** Maximum OOB data entries */
#define MAX_OOB_ENTRIES         4

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

/**
 * @brief OOB data entry
 */
typedef struct {
    uint8_t mac[BLE_MAC_ADDR_LEN];
    uint8_t oob_data[BLE_SECURITY_OOB_DATA_LEN];
    bool valid;
} ble_oob_entry_t;

/**
 * @brief Active pairing entry
 */
typedef struct {
    ble_pairing_info_t info;
    bool active;
} ble_active_pairing_t;

/**
 * @brief Module state structure
 */
typedef struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    ble_security_config_t config;

    /* Bond storage */
    ble_bond_info_t bonds[BLE_SECURITY_MAX_BONDS];
    size_t bond_count;
    bool bonds_dirty;  /* Need to save to NVS */

    /* Active pairings */
    ble_active_pairing_t pairings[MAX_CONCURRENT_PAIRINGS];

    /* OOB data */
    ble_oob_entry_t oob_data[MAX_OOB_ENTRIES];

    /* Callbacks */
    ble_passkey_request_cb_t passkey_request_cb;
    ble_passkey_display_cb_t passkey_display_cb;
    ble_numeric_compare_cb_t numeric_compare_cb;
    ble_pairing_complete_cb_t pairing_complete_cb;
    ble_pairing_request_cb_t pairing_request_cb;
    ble_bond_removed_cb_t bond_removed_cb;
    ble_encryption_change_cb_t encryption_change_cb;
} ble_security_state_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

static ble_security_state_t s_state = {
    .initialized = false,
    .mutex = NULL,
    .bond_count = 0,
    .bonds_dirty = false
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static esp_err_t load_bonds_from_nvs(void);
static esp_err_t save_bonds_to_nvs(void);
static esp_err_t configure_nimble_security(void);
static int find_bond_index(const uint8_t *mac);
static int find_free_bond_slot(void);
static int find_active_pairing(const uint8_t *mac);
static int find_free_pairing_slot(void);
static void clear_pairing_slot(int index);
static uint32_t get_current_timestamp(void);
static void handle_passkey_action(struct ble_gap_event *event);
static void handle_enc_change(struct ble_gap_event *event);
static void handle_pairing_complete(struct ble_gap_event *event);
static void mac_to_nvs_key(const uint8_t *mac, char *key);
static int get_conn_handle_by_mac(const uint8_t *mac);
static uint16_t calculate_bonds_crc(void);

/* ============================================================================
 * String Lookup Tables
 * ============================================================================ */

static const char *s_security_mode_str[] = {
    "NONE",
    "JUST_WORKS",
    "PASSKEY",
    "NUMERIC_COMPARISON",
    "OOB"
};

static const char *s_security_level_str[] = {
    "NO_SECURITY",
    "UNAUTHENTICATED",
    "AUTHENTICATED",
    "LESC"
};

static const char *s_pairing_state_str[] = {
    "IDLE",
    "INITIATED",
    "PASSKEY_REQUESTED",
    "PASSKEY_DISPLAYED",
    "NUMERIC_COMPARE",
    "COMPLETING",
    "FAILED",
    "COMPLETE"
};

static const char *s_io_cap_str[] = {
    "DISPLAY_ONLY",
    "DISPLAY_YESNO",
    "KEYBOARD_ONLY",
    "NO_IO",
    "KEYBOARD_DISPLAY"
};

/* ============================================================================
 * Initialization and Deinitialization
 * ============================================================================ */

esp_err_t ble_security_init(void)
{
    ble_security_config_t config = BLE_SECURITY_CONFIG_DEFAULT();
    return ble_security_init_with_config(&config);
}

esp_err_t ble_security_init_with_config(const ble_security_config_t *config)
{
    if (s_state.initialized) {
        ESP_LOGW(TAG, "Security module already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing BLE security module...");

    /* Validate configuration */
    if (config == NULL) {
        ble_security_config_t default_config = BLE_SECURITY_CONFIG_DEFAULT();
        s_state.config = default_config;
    } else {
        if (config->enc_key_size < BLE_ENC_KEY_SIZE_MIN || config->enc_key_size > BLE_ENC_KEY_SIZE_MAX) {
            ESP_LOGE(TAG, "Invalid encryption key size: %d", config->enc_key_size);
            return ESP_ERR_INVALID_ARG;
        }
        s_state.config = *config;
    }

    /* Create mutex */
    s_state.mutex = xSemaphoreCreateMutex();
    if (s_state.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize bond storage */
    memset(s_state.bonds, 0, sizeof(s_state.bonds));
    s_state.bond_count = 0;
    s_state.bonds_dirty = false;

    /* Initialize pairings */
    memset(s_state.pairings, 0, sizeof(s_state.pairings));

    /* Initialize OOB data */
    memset(s_state.oob_data, 0, sizeof(s_state.oob_data));

    /* Load bonds from NVS */
    esp_err_t ret = load_bonds_from_nvs();
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load bonds from NVS: %s", esp_err_to_name(ret));
        /* Continue - will use empty bond database */
    }

    /* Configure NimBLE security parameters */
    ret = configure_nimble_security();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure NimBLE security: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
        return ret;
    }

    s_state.initialized = true;
    ESP_LOGI(TAG, "BLE security initialized (mode=%s, bonds=%zu)",
             s_security_mode_str[s_state.config.default_mode], s_state.bond_count);

    return ESP_OK;
}

esp_err_t ble_security_deinit(void)
{
    if (!s_state.initialized) {
        ESP_LOGW(TAG, "Security module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing BLE security module...");

    /* Save any pending bond changes */
    if (s_state.bonds_dirty) {
        esp_err_t ret = save_bonds_to_nvs();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save bonds on deinit: %s", esp_err_to_name(ret));
        }
    }

    /* Clear sensitive data */
    memset(s_state.bonds, 0, sizeof(s_state.bonds));
    memset(s_state.pairings, 0, sizeof(s_state.pairings));
    memset(s_state.oob_data, 0, sizeof(s_state.oob_data));

    /* Clear callbacks */
    s_state.passkey_request_cb = NULL;
    s_state.passkey_display_cb = NULL;
    s_state.numeric_compare_cb = NULL;
    s_state.pairing_complete_cb = NULL;
    s_state.pairing_request_cb = NULL;
    s_state.bond_removed_cb = NULL;
    s_state.encryption_change_cb = NULL;

    /* Delete mutex */
    if (s_state.mutex != NULL) {
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
    }

    s_state.initialized = false;
    ESP_LOGI(TAG, "BLE security deinitialized");

    return ESP_OK;
}

bool ble_security_is_initialized(void)
{
    return s_state.initialized;
}

/* ============================================================================
 * Pairing Mode and Configuration
 * ============================================================================ */

esp_err_t ble_security_set_mode(ble_security_mode_t mode)
{
    if (!s_state.initialized) {
        ESP_LOGE(TAG, "Security module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (mode > BLE_SEC_MODE_OOB) {
        ESP_LOGE(TAG, "Invalid security mode: %d", mode);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    s_state.config.default_mode = mode;
    xSemaphoreGive(s_state.mutex);

    /* Reconfigure NimBLE */
    configure_nimble_security();

    ESP_LOGI(TAG, "Security mode set to %s", s_security_mode_str[mode]);
    return ESP_OK;
}

ble_security_mode_t ble_security_get_mode(void)
{
    return s_state.config.default_mode;
}

esp_err_t ble_security_set_io_cap(ble_io_capability_t io_cap)
{
    if (!s_state.initialized) {
        ESP_LOGE(TAG, "Security module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (io_cap > BLE_IO_CAP_KEYBOARD_DISPLAY) {
        ESP_LOGE(TAG, "Invalid I/O capability: %d", io_cap);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    s_state.config.io_capability = io_cap;
    xSemaphoreGive(s_state.mutex);

    /* Reconfigure NimBLE */
    configure_nimble_security();

    ESP_LOGI(TAG, "I/O capability set to %s", s_io_cap_str[io_cap]);
    return ESP_OK;
}

ble_io_capability_t ble_security_get_io_cap(void)
{
    return s_state.config.io_capability;
}

esp_err_t ble_security_set_passkey(uint32_t passkey)
{
    if (passkey > BLE_PASSKEY_MAX) {
        ESP_LOGE(TAG, "Invalid passkey: %lu (max %d)", (unsigned long)passkey, BLE_PASSKEY_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    s_state.config.static_passkey = passkey;
    xSemaphoreGive(s_state.mutex);

    /* Update NimBLE static passkey */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    if (passkey > 0) {
        ESP_LOGI(TAG, "Static passkey set to %06lu", (unsigned long)passkey);
    } else {
        ESP_LOGI(TAG, "Using random passkey");
    }

    return ESP_OK;
}

uint32_t ble_security_get_passkey(void)
{
    return s_state.config.static_passkey;
}

esp_err_t ble_security_set_secure_connections(bool enable)
{
    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    s_state.config.require_sc = enable;
    xSemaphoreGive(s_state.mutex);

    /* Reconfigure NimBLE */
    configure_nimble_security();

    ESP_LOGI(TAG, "Secure Connections (LESC) %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

bool ble_security_is_secure_connections_enabled(void)
{
    return s_state.config.require_sc;
}

/* ============================================================================
 * Pairing Operations
 * ============================================================================ */

esp_err_t ble_security_pair(const uint8_t *mac)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Find connection handle for this MAC */
    int conn_handle = get_conn_handle_by_mac(mac);
    if (conn_handle < 0) {
        char mac_str[18];
        ESP_LOGE(TAG, "Device not connected: %s", ble_mac_to_str(mac, mac_str));
        return ESP_ERR_NOT_FOUND;
    }

    return ble_security_pair_by_handle((uint16_t)conn_handle);
}

esp_err_t ble_security_pair_by_handle(uint16_t conn_handle)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    /* Find free pairing slot */
    int slot = find_free_pairing_slot();
    if (slot < 0) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGE(TAG, "No free pairing slot");
        return ESP_ERR_NO_MEM;
    }

    /* Get connection info */
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGE(TAG, "Connection not found: handle=%d", conn_handle);
        return ESP_ERR_NOT_FOUND;
    }

    /* Initialize pairing entry */
    ble_active_pairing_t *pairing = &s_state.pairings[slot];
    memset(pairing, 0, sizeof(*pairing));
    memcpy(pairing->info.mac, desc.peer_id_addr.val, BLE_MAC_ADDR_LEN);
    pairing->info.addr_type = (ble_addr_type_t)desc.peer_id_addr.type;
    pairing->info.conn_handle = conn_handle;
    pairing->info.state = BLE_PAIRING_STATE_INITIATED;
    pairing->info.initiated_time = get_current_timestamp();
    pairing->active = true;

    xSemaphoreGive(s_state.mutex);

    /* Initiate security */
    char mac_str[18];
    ESP_LOGI(TAG, "Initiating pairing with %s (handle=%d)",
             ble_mac_to_str(pairing->info.mac, mac_str), conn_handle);

    rc = ble_gap_security_initiate(conn_handle);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initiate security: %d", rc);
        xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
        clear_pairing_slot(slot);
        xSemaphoreGive(s_state.mutex);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ble_security_cancel_pairing(const uint8_t *mac)
{
    if (!s_state.initialized || mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_active_pairing(mac);
    if (idx < 0) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t conn_handle = s_state.pairings[idx].info.conn_handle;
    clear_pairing_slot(idx);

    xSemaphoreGive(s_state.mutex);

    /* Terminate connection to cancel pairing */
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);

    char mac_str[18];
    ESP_LOGI(TAG, "Cancelled pairing with %s", ble_mac_to_str(mac, mac_str));

    return ESP_OK;
}

esp_err_t ble_security_respond_passkey(const uint8_t *mac, uint32_t passkey)
{
    if (!s_state.initialized || mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (passkey > BLE_PASSKEY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_active_pairing(mac);
    if (idx < 0 || s_state.pairings[idx].info.state != BLE_PAIRING_STATE_PASSKEY_REQUESTED) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t conn_handle = s_state.pairings[idx].info.conn_handle;
    s_state.pairings[idx].info.state = BLE_PAIRING_STATE_COMPLETING;

    xSemaphoreGive(s_state.mutex);

    /* Submit passkey */
    struct ble_sm_io pkey = {
        .action = BLE_SM_IOACT_INPUT,
        .passkey = passkey
    };

    int rc = ble_sm_inject_io(conn_handle, &pkey);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to inject passkey: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Passkey submitted for pairing");
    return ESP_OK;
}

esp_err_t ble_security_respond_numeric_compare(const uint8_t *mac, bool accept)
{
    if (!s_state.initialized || mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_active_pairing(mac);
    if (idx < 0 || s_state.pairings[idx].info.state != BLE_PAIRING_STATE_NUMERIC_COMPARE) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t conn_handle = s_state.pairings[idx].info.conn_handle;
    s_state.pairings[idx].info.state = BLE_PAIRING_STATE_COMPLETING;

    xSemaphoreGive(s_state.mutex);

    /* Submit numeric comparison result */
    struct ble_sm_io pkey = {
        .action = BLE_SM_IOACT_NUMCMP,
        .numcmp_accept = accept
    };

    int rc = ble_sm_inject_io(conn_handle, &pkey);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to inject numeric comparison: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Numeric comparison %s", accept ? "accepted" : "rejected");
    return ESP_OK;
}

esp_err_t ble_security_get_pairing_info(const uint8_t *mac, ble_pairing_info_t *info)
{
    if (!s_state.initialized || mac == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_active_pairing(mac);
    if (idx < 0) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    *info = s_state.pairings[idx].info;

    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

bool ble_security_is_pairing_active(void)
{
    if (!s_state.initialized) {
        return false;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    bool active = false;
    for (int i = 0; i < MAX_CONCURRENT_PAIRINGS; i++) {
        if (s_state.pairings[i].active) {
            active = true;
            break;
        }
    }

    xSemaphoreGive(s_state.mutex);
    return active;
}

/* ============================================================================
 * Bonding Management
 * ============================================================================ */

esp_err_t ble_security_get_bond_count(size_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    *count = s_state.bond_count;
    xSemaphoreGive(s_state.mutex);

    return ESP_OK;
}

esp_err_t ble_security_get_bond_info(size_t index, ble_bond_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    if (index >= s_state.bond_count) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    /* Find nth valid bond */
    size_t count = 0;
    for (int i = 0; i < BLE_SECURITY_MAX_BONDS; i++) {
        if (s_state.bonds[i].is_bonded) {
            if (count == index) {
                *info = s_state.bonds[i];
                /* Clear sensitive key data for external access */
                memset(info->ltk, 0, sizeof(info->ltk));
                memset(info->irk, 0, sizeof(info->irk));
                xSemaphoreGive(s_state.mutex);
                return ESP_OK;
            }
            count++;
        }
    }

    xSemaphoreGive(s_state.mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_security_get_bond_by_mac(const uint8_t *mac, ble_bond_info_t *info)
{
    if (mac == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_bond_index(mac);
    if (idx < 0) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    *info = s_state.bonds[idx];
    /* Clear sensitive key data */
    memset(info->ltk, 0, sizeof(info->ltk));
    memset(info->irk, 0, sizeof(info->irk));

    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

esp_err_t ble_security_remove_bond(const uint8_t *mac)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_bond_index(mac);
    if (idx < 0) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    char mac_str[18];
    ESP_LOGI(TAG, "Removing bond with %s", ble_mac_to_str(mac, mac_str));

    /* Save addr_type before clearing bond entry (avoid use-after-free) */
    uint8_t saved_addr_type = s_state.bonds[idx].addr_type;

    /* Clear bond entry */
    memset(&s_state.bonds[idx], 0, sizeof(ble_bond_info_t));
    s_state.bond_count--;
    s_state.bonds_dirty = true;

    /* Also remove from NimBLE store */
    ble_addr_t addr;
    addr.type = saved_addr_type;
    memcpy(addr.val, mac, BLE_MAC_ADDR_LEN);
    ble_store_util_delete_peer(&addr);

    xSemaphoreGive(s_state.mutex);

    /* Save to NVS */
    save_bonds_to_nvs();

    /* Notify callback */
    if (s_state.bond_removed_cb != NULL) {
        s_state.bond_removed_cb(mac);
    }

    return ESP_OK;
}

esp_err_t ble_security_clear_all_bonds(void)
{
    ESP_LOGI(TAG, "Clearing all bonds...");

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    /* Collect MACs for callbacks */
    uint8_t macs[BLE_SECURITY_MAX_BONDS][BLE_MAC_ADDR_LEN];
    size_t count = 0;
    for (int i = 0; i < BLE_SECURITY_MAX_BONDS; i++) {
        if (s_state.bonds[i].is_bonded) {
            memcpy(macs[count], s_state.bonds[i].mac, BLE_MAC_ADDR_LEN);
            count++;
        }
    }

    /* Clear all bonds */
    memset(s_state.bonds, 0, sizeof(s_state.bonds));
    s_state.bond_count = 0;
    s_state.bonds_dirty = true;

    /* Clear NimBLE store */
    ble_store_clear();

    xSemaphoreGive(s_state.mutex);

    /* Save empty bond list to NVS */
    esp_err_t ret = save_bonds_to_nvs();

    /* Notify callbacks */
    if (s_state.bond_removed_cb != NULL) {
        for (size_t i = 0; i < count; i++) {
            s_state.bond_removed_cb(macs[i]);
        }
    }

    ESP_LOGI(TAG, "Cleared %zu bonds", count);
    return ret;
}

bool ble_security_is_bonded(const uint8_t *mac)
{
    if (mac == NULL || !s_state.initialized) {
        return false;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    int idx = find_bond_index(mac);
    xSemaphoreGive(s_state.mutex);

    return idx >= 0;
}

esp_err_t ble_security_update_bond_timestamp(const uint8_t *mac)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_bond_index(mac);
    if (idx < 0) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    s_state.bonds[idx].last_connected = get_current_timestamp();
    s_state.bonds_dirty = true;

    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

esp_err_t ble_security_save_bonds(void)
{
    return save_bonds_to_nvs();
}

esp_err_t ble_security_load_bonds(void)
{
    return load_bonds_from_nvs();
}

/* ============================================================================
 * Encryption Management
 * ============================================================================ */

esp_err_t ble_security_encrypt(const uint8_t *mac)
{
    if (!s_state.initialized || mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int conn_handle = get_conn_handle_by_mac(mac);
    if (conn_handle < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char mac_str[18];
    ESP_LOGI(TAG, "Requesting encryption for %s", ble_mac_to_str(mac, mac_str));

    int rc = ble_gap_security_initiate((uint16_t)conn_handle);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initiate encryption: %d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool ble_security_is_encrypted(const uint8_t *mac)
{
    if (!s_state.initialized || mac == NULL) {
        return false;
    }

    int conn_handle = get_conn_handle_by_mac(mac);
    if (conn_handle < 0) {
        return false;
    }

    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find((uint16_t)conn_handle, &desc);
    if (rc != 0) {
        return false;
    }

    return desc.sec_state.encrypted;
}

esp_err_t ble_security_get_level(const uint8_t *mac, ble_security_level_t *level)
{
    if (!s_state.initialized || mac == NULL || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int conn_handle = get_conn_handle_by_mac(mac);
    if (conn_handle < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find((uint16_t)conn_handle, &desc);
    if (rc != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!desc.sec_state.encrypted) {
        *level = BLE_SEC_LEVEL_NONE;
    } else if (desc.sec_state.authenticated) {
        if (desc.sec_state.bonded) {
            *level = BLE_SEC_LEVEL_LESC;
        } else {
            *level = BLE_SEC_LEVEL_AUTH_ENC;
        }
    } else {
        *level = BLE_SEC_LEVEL_UNAUTH_ENC;
    }

    return ESP_OK;
}

/* ============================================================================
 * Callback Registration
 * ============================================================================ */

void ble_security_set_passkey_request_cb(ble_passkey_request_cb_t cb)
{
    if (s_state.mutex != NULL) {
        xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    }
    s_state.passkey_request_cb = cb;
    if (s_state.mutex != NULL) {
        xSemaphoreGive(s_state.mutex);
    }
}

void ble_security_set_passkey_display_cb(ble_passkey_display_cb_t cb)
{
    if (s_state.mutex != NULL) {
        xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    }
    s_state.passkey_display_cb = cb;
    if (s_state.mutex != NULL) {
        xSemaphoreGive(s_state.mutex);
    }
}

void ble_security_set_numeric_compare_cb(ble_numeric_compare_cb_t cb)
{
    if (s_state.mutex != NULL) {
        xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    }
    s_state.numeric_compare_cb = cb;
    if (s_state.mutex != NULL) {
        xSemaphoreGive(s_state.mutex);
    }
}

void ble_security_set_pairing_complete_cb(ble_pairing_complete_cb_t cb)
{
    if (s_state.mutex != NULL) {
        xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    }
    s_state.pairing_complete_cb = cb;
    if (s_state.mutex != NULL) {
        xSemaphoreGive(s_state.mutex);
    }
}

void ble_security_set_pairing_request_cb(ble_pairing_request_cb_t cb)
{
    if (s_state.mutex != NULL) {
        xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    }
    s_state.pairing_request_cb = cb;
    if (s_state.mutex != NULL) {
        xSemaphoreGive(s_state.mutex);
    }
}

void ble_security_set_bond_removed_cb(ble_bond_removed_cb_t cb)
{
    if (s_state.mutex != NULL) {
        xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    }
    s_state.bond_removed_cb = cb;
    if (s_state.mutex != NULL) {
        xSemaphoreGive(s_state.mutex);
    }
}

void ble_security_set_encryption_change_cb(ble_encryption_change_cb_t cb)
{
    if (s_state.mutex != NULL) {
        xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    }
    s_state.encryption_change_cb = cb;
    if (s_state.mutex != NULL) {
        xSemaphoreGive(s_state.mutex);
    }
}

/* ============================================================================
 * OOB Pairing Support
 * ============================================================================ */

esp_err_t ble_security_set_oob_data(const uint8_t *mac, const uint8_t *oob_data)
{
    if (mac == NULL || oob_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    /* Find existing entry or free slot */
    int slot = -1;
    for (int i = 0; i < MAX_OOB_ENTRIES; i++) {
        if (s_state.oob_data[i].valid &&
            ble_mac_equal(s_state.oob_data[i].mac, mac)) {
            slot = i;
            break;
        }
        if (!s_state.oob_data[i].valid && slot < 0) {
            slot = i;
        }
    }

    if (slot < 0) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGE(TAG, "No free OOB data slot");
        return ESP_ERR_NO_MEM;
    }

    memcpy(s_state.oob_data[slot].mac, mac, BLE_MAC_ADDR_LEN);
    memcpy(s_state.oob_data[slot].oob_data, oob_data, BLE_SECURITY_OOB_DATA_LEN);
    s_state.oob_data[slot].valid = true;

    xSemaphoreGive(s_state.mutex);

    char mac_str[18];
    ESP_LOGI(TAG, "OOB data set for %s", ble_mac_to_str(mac, mac_str));

    return ESP_OK;
}

esp_err_t ble_security_clear_oob_data(const uint8_t *mac)
{
    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    if (mac == NULL) {
        /* Clear all */
        memset(s_state.oob_data, 0, sizeof(s_state.oob_data));
        ESP_LOGI(TAG, "Cleared all OOB data");
    } else {
        /* Clear specific entry */
        for (int i = 0; i < MAX_OOB_ENTRIES; i++) {
            if (s_state.oob_data[i].valid &&
                ble_mac_equal(s_state.oob_data[i].mac, mac)) {
                memset(&s_state.oob_data[i], 0, sizeof(ble_oob_entry_t));
                break;
            }
        }
    }

    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

bool ble_security_has_oob_data(const uint8_t *mac)
{
    if (mac == NULL) {
        return false;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    bool found = false;
    for (int i = 0; i < MAX_OOB_ENTRIES; i++) {
        if (s_state.oob_data[i].valid &&
            ble_mac_equal(s_state.oob_data[i].mac, mac)) {
            found = true;
            break;
        }
    }

    xSemaphoreGive(s_state.mutex);
    return found;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

const char *ble_security_mode_to_str(ble_security_mode_t mode)
{
    if (mode <= BLE_SEC_MODE_OOB) {
        return s_security_mode_str[mode];
    }
    return "UNKNOWN";
}

const char *ble_security_level_to_str(ble_security_level_t level)
{
    if (level <= BLE_SEC_LEVEL_LESC) {
        return s_security_level_str[level];
    }
    return "UNKNOWN";
}

const char *ble_security_pairing_state_to_str(ble_pairing_state_t state)
{
    if (state <= BLE_PAIRING_STATE_COMPLETE) {
        return s_pairing_state_str[state];
    }
    return "UNKNOWN";
}

const char *ble_security_io_cap_to_str(ble_io_capability_t io_cap)
{
    if (io_cap <= BLE_IO_CAP_KEYBOARD_DISPLAY) {
        return s_io_cap_str[io_cap];
    }
    return "UNKNOWN";
}

esp_err_t ble_security_self_test(void)
{
    ESP_LOGI(TAG, "Running BLE security self-test...");

    if (!s_state.initialized) {
        ESP_LOGE(TAG, "Security module not initialized");
        return ESP_FAIL;
    }

    if (s_state.mutex == NULL) {
        ESP_LOGE(TAG, "Mutex not created");
        return ESP_FAIL;
    }

    /* Test NVS access */
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_SECURITY_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_OK) {
        nvs_close(nvs);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS access issue: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "BLE security self-test PASSED (mode=%s, bonds=%zu)",
             s_security_mode_str[s_state.config.default_mode], s_state.bond_count);

    return ESP_OK;
}

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * @brief Calculate CRC-16-CCITT for bond data integrity validation
 *
 * @return 16-bit CRC value over all valid bonds
 */
static uint16_t calculate_bonds_crc(void)
{
    uint16_t crc = 0xFFFF;

    for (int i = 0; i < BLE_SECURITY_MAX_BONDS; i++) {
        if (s_state.bonds[i].is_bonded) {
            /* Include MAC, addr_type, sec_level, and bond_time in CRC */
            const uint8_t *data = (const uint8_t *)&s_state.bonds[i];
            /* Only CRC over non-sensitive fields: mac(6) + addr_type(1) + is_bonded(1) +
             * last_connected(4) + bond_time(4) + sec_level(4) + name(32) = 52 bytes */
            size_t crc_len = offsetof(ble_bond_info_t, ltk);

            for (size_t j = 0; j < crc_len; j++) {
                crc ^= ((uint16_t)data[j] << 8);
                for (int k = 0; k < 8; k++) {
                    if (crc & 0x8000) {
                        crc = (crc << 1) ^ CRC16_POLY;
                    } else {
                        crc <<= 1;
                    }
                }
            }
        }
    }

    return crc;
}

/**
 * @brief Load bonds from NVS storage
 */
static esp_err_t load_bonds_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_SECURITY_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No existing bond storage found");
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Read bond count */
    uint8_t count = 0;
    ret = nvs_get_u8(nvs, NVS_BOND_COUNT_KEY, &count);
    if (ret != ESP_OK) {
        nvs_close(nvs);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    if (count > BLE_SECURITY_MAX_BONDS) {
        count = BLE_SECURITY_MAX_BONDS;
    }

    ESP_LOGI(TAG, "Loading %d bonds from NVS", count);

    /* Read stored CRC for validation */
    uint16_t stored_crc = 0;
    ret = nvs_get_u16(nvs, NVS_BOND_CRC_KEY, &stored_crc);
    bool has_crc = (ret == ESP_OK);

    /* Read each bond */
    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);
    s_state.bond_count = 0;

    for (uint8_t i = 0; i < count; i++) {
        char key[NVS_KEY_MAX_LEN];
        snprintf(key, sizeof(key), "%s%d", NVS_BOND_KEY_PREFIX, i);

        size_t size = sizeof(ble_bond_info_t);
        ret = nvs_get_blob(nvs, key, &s_state.bonds[i], &size);
        if (ret == ESP_OK && s_state.bonds[i].is_bonded) {
            s_state.bond_count++;
            char mac_str[18];
            ESP_LOGD(TAG, "Loaded bond: %s",
                     ble_mac_to_str(s_state.bonds[i].mac, mac_str));
        }
    }

    /* Validate CRC if available */
    if (has_crc && s_state.bond_count > 0) {
        uint16_t calculated_crc = calculate_bonds_crc();
        if (calculated_crc != stored_crc) {
            ESP_LOGE(TAG, "Bond CRC mismatch! stored=0x%04X calculated=0x%04X",
                     stored_crc, calculated_crc);
            ESP_LOGW(TAG, "Bond database may be corrupted, clearing bonds");
            memset(s_state.bonds, 0, sizeof(s_state.bonds));
            s_state.bond_count = 0;
            xSemaphoreGive(s_state.mutex);
            nvs_close(nvs);
            return ESP_ERR_INVALID_CRC;
        }
        ESP_LOGI(TAG, "Bond CRC validated successfully (0x%04X)", stored_crc);
    }

    xSemaphoreGive(s_state.mutex);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Loaded %zu bonds from NVS", s_state.bond_count);
    return ESP_OK;
}

/**
 * @brief Save bonds to NVS storage
 */
static esp_err_t save_bonds_to_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_SECURITY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(ret));
        return ret;
    }

    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    /* Write bond count */
    uint8_t count = (uint8_t)s_state.bond_count;
    ret = nvs_set_u8(nvs, NVS_BOND_COUNT_KEY, count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write bond count: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_state.mutex);
        nvs_close(nvs);
        return ret;
    }

    /* Write each bond */
    uint8_t written = 0;
    for (int i = 0; i < BLE_SECURITY_MAX_BONDS && written < count; i++) {
        if (s_state.bonds[i].is_bonded) {
            char key[NVS_KEY_MAX_LEN];
            snprintf(key, sizeof(key), "%s%d", NVS_BOND_KEY_PREFIX, written);

            ret = nvs_set_blob(nvs, key, &s_state.bonds[i], sizeof(ble_bond_info_t));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write bond %d: %s", written, esp_err_to_name(ret));
            } else {
                written++;
            }
        }
    }

    /* Calculate and store CRC for integrity validation */
    uint16_t crc = calculate_bonds_crc();
    ret = nvs_set_u16(nvs, NVS_BOND_CRC_KEY, crc);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to write bond CRC: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Bond CRC stored: 0x%04X", crc);
    }

    s_state.bonds_dirty = false;
    xSemaphoreGive(s_state.mutex);

    /* Commit changes */
    ret = nvs_commit(nvs);
    nvs_close(nvs);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Saved %d bonds to NVS (CRC: 0x%04X)", written, crc);
    }

    return ret;
}

/**
 * @brief Configure NimBLE security manager
 */
static esp_err_t configure_nimble_security(void)
{
    ESP_LOGI(TAG, "Configuring NimBLE security...");

    /* Set I/O capability */
    ble_hs_cfg.sm_io_cap = s_state.config.io_capability;

    /* Set bonding flags */
    ble_hs_cfg.sm_bonding = s_state.config.require_bonding ? 1 : 0;

    /* Set MITM requirement */
    ble_hs_cfg.sm_mitm = s_state.config.require_mitm ? 1 : 0;

    /* Set Secure Connections requirement */
    ble_hs_cfg.sm_sc = s_state.config.require_sc ? 1 : 0;

    /* Set key distribution */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ESP_LOGI(TAG, "NimBLE security configured: io_cap=%s, bonding=%d, mitm=%d, sc=%d",
             s_io_cap_str[s_state.config.io_capability],
             ble_hs_cfg.sm_bonding,
             ble_hs_cfg.sm_mitm,
             ble_hs_cfg.sm_sc);

    return ESP_OK;
}

/**
 * @brief Find bond index by MAC address
 */
static int find_bond_index(const uint8_t *mac)
{
    for (int i = 0; i < BLE_SECURITY_MAX_BONDS; i++) {
        if (s_state.bonds[i].is_bonded &&
            ble_mac_equal(s_state.bonds[i].mac, mac)) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Find free bond slot
 */
static int find_free_bond_slot(void)
{
    for (int i = 0; i < BLE_SECURITY_MAX_BONDS; i++) {
        if (!s_state.bonds[i].is_bonded) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Find active pairing by MAC
 */
static int find_active_pairing(const uint8_t *mac)
{
    for (int i = 0; i < MAX_CONCURRENT_PAIRINGS; i++) {
        if (s_state.pairings[i].active &&
            ble_mac_equal(s_state.pairings[i].info.mac, mac)) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Find free pairing slot
 */
static int find_free_pairing_slot(void)
{
    for (int i = 0; i < MAX_CONCURRENT_PAIRINGS; i++) {
        if (!s_state.pairings[i].active) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Clear pairing slot
 */
static void clear_pairing_slot(int index)
{
    if (index >= 0 && index < MAX_CONCURRENT_PAIRINGS) {
        memset(&s_state.pairings[index], 0, sizeof(ble_active_pairing_t));
    }
}

/**
 * @brief Get current timestamp (seconds since boot or epoch)
 */
static uint32_t get_current_timestamp(void)
{
    time_t now;
    time(&now);
    if (now < BLE_UNIX_EPOCH_THRESHOLD) {
        /* Time not set, use uptime */
        return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    }
    return (uint32_t)now;
}

/**
 * @brief Convert MAC to NVS key
 */
static void __attribute__((unused)) mac_to_nvs_key(const uint8_t *mac, char *key)
{
    snprintf(key, NVS_KEY_MAX_LEN, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * @brief Get connection handle by MAC address
 */
static int get_conn_handle_by_mac(const uint8_t *mac)
{
    /* Iterate through all connections to find matching MAC */
    struct ble_gap_conn_desc desc;
    int rc;

    for (uint16_t handle = 0; handle < MYNEWT_VAL(BLE_MAX_CONNECTIONS); handle++) {
        rc = ble_gap_conn_find(handle, &desc);
        if (rc == 0) {
            if (ble_mac_equal(desc.peer_id_addr.val, mac) ||
                ble_mac_equal(desc.peer_ota_addr.val, mac)) {
                return (int)handle;
            }
        }
    }

    return -1;
}

/**
 * @brief Handle GAP passkey action event
 */
static void __attribute__((unused)) handle_passkey_action(struct ble_gap_event *event)
{
    uint16_t conn_handle = event->passkey.conn_handle;
    struct ble_sm_io pkey = {0};

    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        ESP_LOGE(TAG, "Connection not found for passkey action");
        return;
    }

    uint8_t *mac = desc.peer_id_addr.val;
    char mac_str[18];
    ble_mac_to_str(mac, mac_str);

    /* Find or create pairing entry */
    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_active_pairing(mac);
    if (idx < 0) {
        idx = find_free_pairing_slot();
        if (idx >= 0) {
            s_state.pairings[idx].active = true;
            memcpy(s_state.pairings[idx].info.mac, mac, BLE_MAC_ADDR_LEN);
            s_state.pairings[idx].info.addr_type = (ble_addr_type_t)desc.peer_id_addr.type;
            s_state.pairings[idx].info.conn_handle = conn_handle;
        }
    }

    switch (event->passkey.params.action) {
        case BLE_SM_IOACT_DISP:
            /* Display passkey */
            ESP_LOGI(TAG, "Passkey display for %s", mac_str);

            if (s_state.config.static_passkey > 0) {
                pkey.passkey = s_state.config.static_passkey;
            } else {
                pkey.passkey = esp_random() % BLE_PASSKEY_RANDOM_MAX;
            }

            if (idx >= 0) {
                s_state.pairings[idx].info.passkey = pkey.passkey;
                s_state.pairings[idx].info.state = BLE_PAIRING_STATE_PASSKEY_DISPLAYED;
            }

            xSemaphoreGive(s_state.mutex);

            ESP_LOGI(TAG, "Displaying passkey: %06lu", (unsigned long)pkey.passkey);

            if (s_state.passkey_display_cb != NULL) {
                s_state.passkey_display_cb(mac, pkey.passkey);
            }

            pkey.action = BLE_SM_IOACT_DISP;
            ble_sm_inject_io(conn_handle, &pkey);
            return;

        case BLE_SM_IOACT_INPUT:
            /* Request passkey input */
            ESP_LOGI(TAG, "Passkey input requested for %s", mac_str);

            if (idx >= 0) {
                s_state.pairings[idx].info.state = BLE_PAIRING_STATE_PASSKEY_REQUESTED;
            }

            xSemaphoreGive(s_state.mutex);

            if (s_state.passkey_request_cb != NULL) {
                uint32_t passkey = 0;
                s_state.passkey_request_cb(mac, &passkey);

                pkey.action = BLE_SM_IOACT_INPUT;
                pkey.passkey = passkey;
                ble_sm_inject_io(conn_handle, &pkey);
            } else if (s_state.config.static_passkey > 0) {
                /* Use static passkey */
                pkey.action = BLE_SM_IOACT_INPUT;
                pkey.passkey = s_state.config.static_passkey;
                ble_sm_inject_io(conn_handle, &pkey);
            }
            return;

        case BLE_SM_IOACT_NUMCMP:
            /* Numeric comparison */
            ESP_LOGI(TAG, "Numeric comparison for %s: %06lu",
                     mac_str, (unsigned long)event->passkey.params.numcmp);

            if (idx >= 0) {
                s_state.pairings[idx].info.passkey = event->passkey.params.numcmp;
                s_state.pairings[idx].info.state = BLE_PAIRING_STATE_NUMERIC_COMPARE;
                s_state.pairings[idx].info.numeric_compare = true;
            }

            xSemaphoreGive(s_state.mutex);

            if (s_state.numeric_compare_cb != NULL) {
                bool accept = s_state.numeric_compare_cb(mac, event->passkey.params.numcmp);
                pkey.action = BLE_SM_IOACT_NUMCMP;
                pkey.numcmp_accept = accept;
                ble_sm_inject_io(conn_handle, &pkey);
            } else if (s_state.config.auto_accept_pairing) {
                /* Auto-accept */
                pkey.action = BLE_SM_IOACT_NUMCMP;
                pkey.numcmp_accept = 1;
                ble_sm_inject_io(conn_handle, &pkey);
            }
            return;

        case BLE_SM_IOACT_OOB:
            /* OOB pairing */
            ESP_LOGI(TAG, "OOB pairing requested for %s", mac_str);

            /* Search for OOB data while holding mutex to prevent race condition */
            {
                bool oob_found = false;
                uint8_t oob_copy[BLE_SECURITY_OOB_DATA_LEN];

                for (int i = 0; i < MAX_OOB_ENTRIES; i++) {
                    if (s_state.oob_data[i].valid &&
                        ble_mac_equal(s_state.oob_data[i].mac, mac)) {
                        /* Copy OOB data while holding mutex */
                        memcpy(oob_copy, s_state.oob_data[i].oob_data, BLE_SECURITY_OOB_DATA_LEN);
                        oob_found = true;
                        break;
                    }
                }

                xSemaphoreGive(s_state.mutex);

                if (oob_found) {
                    pkey.action = BLE_SM_IOACT_OOB;
                    memcpy(pkey.oob, oob_copy, 16);
                    ble_sm_inject_io(conn_handle, &pkey);
                } else {
                    ESP_LOGW(TAG, "No OOB data available for device");
                }
            }
            return;

        case BLE_SM_IOACT_NONE:
            /* Just Works */
            ESP_LOGI(TAG, "Just Works pairing for %s", mac_str);
            xSemaphoreGive(s_state.mutex);
            return;

        default:
            ESP_LOGW(TAG, "Unknown passkey action: %d", event->passkey.params.action);
            xSemaphoreGive(s_state.mutex);
            return;
    }
}

/**
 * @brief Handle encryption change event
 */
static void __attribute__((unused)) handle_enc_change(struct ble_gap_event *event)
{
    uint16_t conn_handle = event->enc_change.conn_handle;
    int status = event->enc_change.status;

    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        return;
    }

    uint8_t *mac = desc.peer_id_addr.val;
    char mac_str[18];
    ble_mac_to_str(mac, mac_str);

    if (status == 0) {
        /* Encryption succeeded */
        ble_security_level_t level;

        if (desc.sec_state.authenticated) {
            level = desc.sec_state.bonded ? BLE_SEC_LEVEL_LESC : BLE_SEC_LEVEL_AUTH_ENC;
        } else {
            level = BLE_SEC_LEVEL_UNAUTH_ENC;
        }

        ESP_LOGI(TAG, "Encryption enabled for %s (level=%s)",
                 mac_str, s_security_level_str[level]);

        /* Update bond timestamp if bonded */
        if (desc.sec_state.bonded) {
            ble_security_update_bond_timestamp(mac);
        }

        /* Notify callback */
        if (s_state.encryption_change_cb != NULL) {
            s_state.encryption_change_cb(mac, true, level);
        }
    } else {
        ESP_LOGW(TAG, "Encryption failed for %s: status=%d", mac_str, status);

        if (s_state.encryption_change_cb != NULL) {
            s_state.encryption_change_cb(mac, false, BLE_SEC_LEVEL_NONE);
        }
    }
}

/**
 * @brief Handle pairing complete (repeat pairing event)
 */
static void __attribute__((unused)) handle_pairing_complete(struct ble_gap_event *event)
{
    uint16_t conn_handle = event->repeat_pairing.conn_handle;

    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        return;
    }

    uint8_t *mac = desc.peer_id_addr.val;
    char mac_str[18];
    ble_mac_to_str(mac, mac_str);

    bool success = (event->repeat_pairing.cur_key_size > 0);
    ble_security_level_t level = BLE_SEC_LEVEL_NONE;

    if (success) {
        if (desc.sec_state.authenticated) {
            level = BLE_SEC_LEVEL_AUTH_ENC;
        } else {
            level = BLE_SEC_LEVEL_UNAUTH_ENC;
        }

        /* Store bond information */
        xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

        int idx = find_bond_index(mac);
        bool is_new_bond = (idx < 0);  /* Track if this is a new bond */
        if (is_new_bond) {
            idx = find_free_bond_slot();
        }

        if (idx >= 0) {
            memcpy(s_state.bonds[idx].mac, mac, BLE_MAC_ADDR_LEN);
            s_state.bonds[idx].addr_type = (ble_addr_type_t)desc.peer_id_addr.type;
            s_state.bonds[idx].is_bonded = true;
            s_state.bonds[idx].last_connected = get_current_timestamp();
            if (is_new_bond) {
                s_state.bonds[idx].bond_time = get_current_timestamp();
                s_state.bond_count++;
            }
            s_state.bonds[idx].sec_level = level;
            s_state.bonds_dirty = true;

            ESP_LOGI(TAG, "Bond %s for %s (level=%s)",
                     is_new_bond ? "stored" : "updated", mac_str, s_security_level_str[level]);
        } else {
            ESP_LOGW(TAG, "No free bond slot for %s", mac_str);
        }

        /* Clear active pairing */
        int pairing_idx = find_active_pairing(mac);
        if (pairing_idx >= 0) {
            clear_pairing_slot(pairing_idx);
        }

        xSemaphoreGive(s_state.mutex);

        /* Save to NVS */
        save_bonds_to_nvs();
    }

    ESP_LOGI(TAG, "Pairing %s for %s", success ? "succeeded" : "failed", mac_str);

    /* Notify callback */
    if (s_state.pairing_complete_cb != NULL) {
        s_state.pairing_complete_cb(mac, success, level);
    }
}

/* ============================================================================
 * NimBLE Store Callbacks (for bond persistence)
 * ============================================================================ */

/**
 * @brief NimBLE store read callback - called when NimBLE needs to read a key
 */
int ble_store_read_our_sec(const struct ble_store_key_sec *key_sec,
                            struct ble_store_value_sec *value_sec)
{
    /* Find bond by peer address */
    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_bond_index(key_sec->peer_addr.val);
    if (idx < 0 || !s_state.bonds[idx].ltk_valid) {
        xSemaphoreGive(s_state.mutex);
        return BLE_HS_ENOENT;
    }

    /* Populate value from stored bond */
    memcpy(&value_sec->peer_addr, &key_sec->peer_addr, sizeof(ble_addr_t));
    memcpy(value_sec->ltk, s_state.bonds[idx].ltk, 16);
    value_sec->ltk_present = 1;
    value_sec->irk_present = s_state.bonds[idx].irk_valid ? 1 : 0;
    if (value_sec->irk_present) {
        memcpy(value_sec->irk, s_state.bonds[idx].irk, 16);
    }

    xSemaphoreGive(s_state.mutex);
    return 0;
}

/**
 * @brief NimBLE store write callback - called when NimBLE wants to store a key
 */
int ble_store_write_our_sec(const struct ble_store_value_sec *value_sec)
{
    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_bond_index(value_sec->peer_addr.val);
    if (idx < 0) {
        idx = find_free_bond_slot();
        if (idx < 0) {
            xSemaphoreGive(s_state.mutex);
            ESP_LOGE(TAG, "No free bond slot");
            return BLE_HS_ENOMEM;
        }
        s_state.bond_count++;
    }

    /* Store bond information */
    memcpy(s_state.bonds[idx].mac, value_sec->peer_addr.val, BLE_MAC_ADDR_LEN);
    s_state.bonds[idx].addr_type = (ble_addr_type_t)value_sec->peer_addr.type;
    s_state.bonds[idx].is_bonded = true;

    if (value_sec->ltk_present) {
        memcpy(s_state.bonds[idx].ltk, value_sec->ltk, 16);
        s_state.bonds[idx].ltk_valid = true;
    }

    if (value_sec->irk_present) {
        memcpy(s_state.bonds[idx].irk, value_sec->irk, 16);
        s_state.bonds[idx].irk_valid = true;
    }

    s_state.bonds[idx].last_connected = get_current_timestamp();
    if (s_state.bonds[idx].bond_time == 0) {
        s_state.bonds[idx].bond_time = get_current_timestamp();
    }

    s_state.bonds_dirty = true;

    xSemaphoreGive(s_state.mutex);

    char mac_str[18];
    ESP_LOGI(TAG, "Stored security keys for %s",
             ble_mac_to_str(value_sec->peer_addr.val, mac_str));

    /* Save to NVS */
    save_bonds_to_nvs();

    return 0;
}

/**
 * @brief NimBLE store delete callback
 */
int ble_store_delete_our_sec(const struct ble_store_key_sec *key_sec)
{
    xSemaphoreTake(s_state.mutex, GW_TIMEOUT_LONG_TICKS);

    int idx = find_bond_index(key_sec->peer_addr.val);
    if (idx >= 0) {
        memset(&s_state.bonds[idx], 0, sizeof(ble_bond_info_t));
        s_state.bond_count--;
        s_state.bonds_dirty = true;
    }

    xSemaphoreGive(s_state.mutex);

    save_bonds_to_nvs();
    return 0;
}

/* Peer security callbacks (mirror of our security) */
int ble_store_read_peer_sec(const struct ble_store_key_sec *key_sec,
                             struct ble_store_value_sec *value_sec)
{
    return ble_store_read_our_sec(key_sec, value_sec);
}

int ble_store_write_peer_sec(const struct ble_store_value_sec *value_sec)
{
    return ble_store_write_our_sec(value_sec);
}

int ble_store_delete_peer_sec(const struct ble_store_key_sec *key_sec)
{
    return ble_store_delete_our_sec(key_sec);
}

/* CCCD store callbacks (not used but required) */
int ble_store_read_cccd(const struct ble_store_key_cccd *key_cccd,
                         struct ble_store_value_cccd *value_cccd)
{
    return BLE_HS_ENOENT;
}

int ble_store_write_cccd(const struct ble_store_value_cccd *value_cccd)
{
    return 0;
}

int ble_store_delete_cccd(const struct ble_store_key_cccd *key_cccd)
{
    return 0;
}

#else /* CONFIG_BT_SCANNER_ENABLED */

/* Stub implementations when BLE is disabled */
static const char *TAG = "BLE_SECURITY";

esp_err_t ble_security_init(void)
{
    ESP_LOGW(TAG, "BLE security disabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_init_with_config(const ble_security_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_security_is_initialized(void)
{
    return false;
}

esp_err_t ble_security_set_mode(ble_security_mode_t mode)
{
    (void)mode;
    return ESP_ERR_NOT_SUPPORTED;
}

ble_security_mode_t ble_security_get_mode(void)
{
    return BLE_SEC_MODE_NONE;
}

esp_err_t ble_security_set_io_cap(ble_io_capability_t io_cap)
{
    (void)io_cap;
    return ESP_ERR_NOT_SUPPORTED;
}

ble_io_capability_t ble_security_get_io_cap(void)
{
    return BLE_IO_CAP_NO_IO;
}

esp_err_t ble_security_set_passkey(uint32_t passkey)
{
    (void)passkey;
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t ble_security_get_passkey(void)
{
    return 0;
}

esp_err_t ble_security_set_secure_connections(bool enable)
{
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_security_is_secure_connections_enabled(void)
{
    return false;
}

esp_err_t ble_security_pair(const uint8_t *mac)
{
    (void)mac;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_pair_by_handle(uint16_t conn_handle)
{
    (void)conn_handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_cancel_pairing(const uint8_t *mac)
{
    (void)mac;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_respond_passkey(const uint8_t *mac, uint32_t passkey)
{
    (void)mac;
    (void)passkey;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_respond_numeric_compare(const uint8_t *mac, bool accept)
{
    (void)mac;
    (void)accept;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_get_pairing_info(const uint8_t *mac, ble_pairing_info_t *info)
{
    (void)mac;
    (void)info;
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_security_is_pairing_active(void)
{
    return false;
}

esp_err_t ble_security_get_bond_count(size_t *count)
{
    if (count) *count = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_get_bond_info(size_t index, ble_bond_info_t *info)
{
    (void)index;
    (void)info;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_get_bond_by_mac(const uint8_t *mac, ble_bond_info_t *info)
{
    (void)mac;
    (void)info;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_remove_bond(const uint8_t *mac)
{
    (void)mac;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_clear_all_bonds(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_security_is_bonded(const uint8_t *mac)
{
    (void)mac;
    return false;
}

esp_err_t ble_security_update_bond_timestamp(const uint8_t *mac)
{
    (void)mac;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_save_bonds(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_load_bonds(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_encrypt(const uint8_t *mac)
{
    (void)mac;
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_security_is_encrypted(const uint8_t *mac)
{
    (void)mac;
    return false;
}

esp_err_t ble_security_get_level(const uint8_t *mac, ble_security_level_t *level)
{
    (void)mac;
    if (level) *level = BLE_SEC_LEVEL_NONE;
    return ESP_ERR_NOT_SUPPORTED;
}

void ble_security_set_passkey_request_cb(ble_passkey_request_cb_t cb) { (void)cb; }
void ble_security_set_passkey_display_cb(ble_passkey_display_cb_t cb) { (void)cb; }
void ble_security_set_numeric_compare_cb(ble_numeric_compare_cb_t cb) { (void)cb; }
void ble_security_set_pairing_complete_cb(ble_pairing_complete_cb_t cb) { (void)cb; }
void ble_security_set_pairing_request_cb(ble_pairing_request_cb_t cb) { (void)cb; }
void ble_security_set_bond_removed_cb(ble_bond_removed_cb_t cb) { (void)cb; }
void ble_security_set_encryption_change_cb(ble_encryption_change_cb_t cb) { (void)cb; }

esp_err_t ble_security_set_oob_data(const uint8_t *mac, const uint8_t *oob_data)
{
    (void)mac;
    (void)oob_data;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_security_clear_oob_data(const uint8_t *mac)
{
    (void)mac;
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_security_has_oob_data(const uint8_t *mac)
{
    (void)mac;
    return false;
}

const char *ble_security_mode_to_str(ble_security_mode_t mode)
{
    (void)mode;
    return "DISABLED";
}

const char *ble_security_level_to_str(ble_security_level_t level)
{
    (void)level;
    return "DISABLED";
}

const char *ble_security_pairing_state_to_str(ble_pairing_state_t state)
{
    (void)state;
    return "DISABLED";
}

const char *ble_security_io_cap_to_str(ble_io_capability_t io_cap)
{
    (void)io_cap;
    return "DISABLED";
}

esp_err_t ble_security_self_test(void)
{
    ESP_LOGW(TAG, "BLE security disabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_BT_SCANNER_ENABLED */
