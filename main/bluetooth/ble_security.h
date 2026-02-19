/**
 * @file ble_security.h
 * @brief BLE Security, Pairing, and Bonding API for ESP32-C5 Zigbee2MQTT Gateway
 *
 * This module provides comprehensive BLE security management including:
 * - Multiple pairing modes (Just Works, Passkey, Numeric Comparison, OOB)
 * - Persistent bond storage in NVS
 * - Auto-reconnect with bonded devices
 * - Security level management (No Security to LESC)
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_SECURITY_H
#define BLE_SECURITY_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "ble_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/**
 * @brief Maximum number of bonded devices that can be stored
 */
#define BLE_SECURITY_MAX_BONDS          20

/**
 * @brief Default maximum bonds (configurable via Kconfig)
 */
#ifndef CONFIG_BLE_MAX_BONDS
#define CONFIG_BLE_MAX_BONDS            BLE_SECURITY_MAX_BONDS
#endif

/**
 * @brief NVS namespace for BLE bond storage
 */
#define BLE_SECURITY_NVS_NAMESPACE      "ble_bonds"

/**
 * @brief Default passkey for Passkey Entry mode (000000-999999)
 */
#define BLE_SECURITY_DEFAULT_PASSKEY    123456

/**
 * @brief Passkey length (always 6 digits)
 */
#define BLE_SECURITY_PASSKEY_LEN        6

/**
 * @brief OOB data length (16 bytes for TK value)
 */
#define BLE_SECURITY_OOB_DATA_LEN       16

/* ============================================================================
 * Enumerations
 * ============================================================================ */

/**
 * @brief BLE security/pairing mode enumeration
 */
typedef enum {
    BLE_SEC_MODE_NONE = 0,              /**< No security (unencrypted connection) */
    BLE_SEC_MODE_JUST_WORKS,            /**< Just Works - no user interaction */
    BLE_SEC_MODE_PASSKEY,               /**< Passkey Entry - 6-digit PIN */
    BLE_SEC_MODE_NUMERIC_COMPARISON,    /**< Numeric Comparison (BLE 4.2+) */
    BLE_SEC_MODE_OOB                    /**< Out of Band pairing (prepared) */
} ble_security_mode_t;

/**
 * @brief BLE security level enumeration
 */
typedef enum {
    BLE_SEC_LEVEL_NONE = 0,             /**< No security */
    BLE_SEC_LEVEL_UNAUTH_ENC,           /**< Unauthenticated encryption (Just Works) */
    BLE_SEC_LEVEL_AUTH_ENC,             /**< Authenticated encryption (Passkey/Numeric) */
    BLE_SEC_LEVEL_LESC                  /**< LE Secure Connections (LESC) */
} ble_security_level_t;

/**
 * @brief BLE I/O capability for pairing
 */
typedef enum {
    BLE_IO_CAP_DISPLAY_ONLY = 0,        /**< Display only (can display passkey) */
    BLE_IO_CAP_DISPLAY_YESNO,           /**< Display with Yes/No buttons */
    BLE_IO_CAP_KEYBOARD_ONLY,           /**< Keyboard only (can input passkey) */
    BLE_IO_CAP_NO_IO,                   /**< No input/output (Just Works only) */
    BLE_IO_CAP_KEYBOARD_DISPLAY         /**< Keyboard and Display */
} ble_io_capability_t;

/**
 * @brief Pairing state enumeration
 */
typedef enum {
    BLE_PAIRING_STATE_IDLE = 0,         /**< No pairing in progress */
    BLE_PAIRING_STATE_INITIATED,        /**< Pairing initiated */
    BLE_PAIRING_STATE_PASSKEY_REQUESTED,/**< Waiting for passkey input */
    BLE_PAIRING_STATE_PASSKEY_DISPLAYED,/**< Passkey displayed, waiting for peer */
    BLE_PAIRING_STATE_NUMERIC_COMPARE,  /**< Numeric comparison in progress */
    BLE_PAIRING_STATE_COMPLETING,       /**< Pairing completing */
    BLE_PAIRING_STATE_FAILED,           /**< Pairing failed */
    BLE_PAIRING_STATE_COMPLETE          /**< Pairing complete */
} ble_pairing_state_t;

/* ============================================================================
 * Structures
 * ============================================================================ */

/**
 * @brief Bond information structure
 *
 * Contains information about a bonded device including encryption keys
 * and connection history.
 */
typedef struct {
    uint8_t mac[BLE_MAC_ADDR_LEN];       /**< Device MAC address */
    ble_addr_type_t addr_type;           /**< Address type (public/random) */
    bool is_bonded;                      /**< Bond is valid and stored */
    uint32_t last_connected;             /**< Last connection timestamp (epoch) */
    uint32_t bond_time;                  /**< When bond was created (epoch) */
    ble_security_level_t sec_level;      /**< Security level of bond */
    char device_name[BLE_DEVICE_NAME_MAX_LEN]; /**< Device name (if known) */
    uint8_t ltk[16];                     /**< Long Term Key (for internal use) */
    uint8_t irk[16];                     /**< Identity Resolving Key */
    bool ltk_valid;                      /**< LTK is valid */
    bool irk_valid;                      /**< IRK is valid */
} ble_bond_info_t;

/**
 * @brief Security configuration structure
 */
typedef struct {
    ble_security_mode_t default_mode;    /**< Default pairing mode */
    ble_io_capability_t io_capability;   /**< I/O capability */
    bool require_bonding;                /**< Require bonding (not just pairing) */
    bool require_mitm;                   /**< Require MITM protection */
    bool require_sc;                     /**< Require Secure Connections (LESC) */
    uint32_t static_passkey;             /**< Static passkey (0 = use random) */
    bool auto_accept_pairing;            /**< Auto-accept pairing requests */
    uint16_t enc_key_size;               /**< Encryption key size (7-16) */
} ble_security_config_t;

/**
 * @brief Pairing request information
 */
typedef struct {
    uint8_t mac[BLE_MAC_ADDR_LEN];       /**< Device MAC address */
    ble_addr_type_t addr_type;           /**< Address type */
    uint16_t conn_handle;                /**< Connection handle */
    ble_pairing_state_t state;           /**< Current pairing state */
    uint32_t passkey;                    /**< Passkey for display/input */
    bool numeric_compare;                /**< Numeric comparison mode active */
    uint32_t initiated_time;             /**< When pairing was initiated */
} ble_pairing_info_t;

/* ============================================================================
 * Callback Types
 * ============================================================================ */

/**
 * @brief Passkey request callback
 *
 * Called when a passkey needs to be entered by the user.
 * Set *passkey to the user-entered passkey.
 *
 * @param mac Device MAC address
 * @param passkey Pointer to store the entered passkey
 */
typedef void (*ble_passkey_request_cb_t)(const uint8_t *mac, uint32_t *passkey);

/**
 * @brief Passkey display callback
 *
 * Called when a passkey should be displayed to the user.
 *
 * @param mac Device MAC address
 * @param passkey 6-digit passkey to display
 */
typedef void (*ble_passkey_display_cb_t)(const uint8_t *mac, uint32_t passkey);

/**
 * @brief Numeric comparison callback
 *
 * Called for BLE 4.2+ Numeric Comparison mode.
 * User should verify the number matches on both devices.
 *
 * @param mac Device MAC address
 * @param numeric_value 6-digit number to compare
 * @return true if user confirms match, false to reject
 */
typedef bool (*ble_numeric_compare_cb_t)(const uint8_t *mac, uint32_t numeric_value);

/**
 * @brief Pairing complete callback
 *
 * Called when pairing completes (success or failure).
 *
 * @param mac Device MAC address
 * @param success true if pairing succeeded
 * @param sec_level Security level achieved (if success)
 */
typedef void (*ble_pairing_complete_cb_t)(const uint8_t *mac, bool success,
                                          ble_security_level_t sec_level);

/**
 * @brief Pairing request callback
 *
 * Called when a remote device requests pairing.
 *
 * @param mac Device MAC address
 * @param io_cap Remote device's I/O capability
 * @return true to accept pairing, false to reject
 */
typedef bool (*ble_pairing_request_cb_t)(const uint8_t *mac, uint8_t io_cap);

/**
 * @brief Bond removed callback
 *
 * Called when a bond is removed.
 *
 * @param mac Device MAC address that was unbonded
 */
typedef void (*ble_bond_removed_cb_t)(const uint8_t *mac);

/**
 * @brief Encryption change callback
 *
 * Called when encryption state changes on a connection.
 *
 * @param mac Device MAC address
 * @param encrypted true if connection is now encrypted
 * @param sec_level New security level
 */
typedef void (*ble_encryption_change_cb_t)(const uint8_t *mac, bool encrypted,
                                           ble_security_level_t sec_level);

/* ============================================================================
 * Default Configuration Macros
 * ============================================================================ */

/**
 * @brief Default security configuration
 */
#define BLE_SECURITY_CONFIG_DEFAULT() { \
    .default_mode = BLE_SEC_MODE_JUST_WORKS, \
    .io_capability = BLE_IO_CAP_NO_IO, \
    .require_bonding = true, \
    .require_mitm = false, \
    .require_sc = false, \
    .static_passkey = 0, \
    .auto_accept_pairing = true, \
    .enc_key_size = 16 \
}

/**
 * @brief Configuration for high security (LESC + MITM)
 */
#define BLE_SECURITY_CONFIG_HIGH() { \
    .default_mode = BLE_SEC_MODE_NUMERIC_COMPARISON, \
    .io_capability = BLE_IO_CAP_DISPLAY_YESNO, \
    .require_bonding = true, \
    .require_mitm = true, \
    .require_sc = true, \
    .static_passkey = 0, \
    .auto_accept_pairing = false, \
    .enc_key_size = 16 \
}

/* ============================================================================
 * Initialization and Deinitialization
 * ============================================================================ */

/**
 * @brief Initialize BLE security module
 *
 * Initializes the security manager, loads stored bonds from NVS,
 * and configures the NimBLE security parameters.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if memory allocation fails
 * @return ESP_ERR_INVALID_STATE if already initialized
 * @return ESP_FAIL on NVS or configuration error
 */
esp_err_t ble_security_init(void);

/**
 * @brief Initialize BLE security with custom configuration
 *
 * @param config Security configuration (NULL for defaults)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if config is invalid
 * @return ESP_ERR_NO_MEM if memory allocation fails
 * @return ESP_FAIL on initialization error
 */
esp_err_t ble_security_init_with_config(const ble_security_config_t *config);

/**
 * @brief Deinitialize BLE security module
 *
 * Saves any pending bond changes and releases resources.
 *
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ble_security_deinit(void);

/**
 * @brief Check if security module is initialized
 *
 * @return true if initialized
 * @return false if not initialized
 */
bool ble_security_is_initialized(void);

/* ============================================================================
 * Pairing Mode and Configuration
 * ============================================================================ */

/**
 * @brief Set the security/pairing mode
 *
 * Sets the default pairing mode for new connections.
 *
 * @param mode Pairing mode to use
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if mode is invalid
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ble_security_set_mode(ble_security_mode_t mode);

/**
 * @brief Get the current security mode
 *
 * @return Current security mode
 */
ble_security_mode_t ble_security_get_mode(void);

/**
 * @brief Set I/O capability
 *
 * Sets the I/O capability which determines which pairing methods
 * are available.
 *
 * @param io_cap I/O capability
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if io_cap is invalid
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ble_security_set_io_cap(ble_io_capability_t io_cap);

/**
 * @brief Get current I/O capability
 *
 * @return Current I/O capability
 */
ble_io_capability_t ble_security_get_io_cap(void);

/**
 * @brief Set static passkey for Passkey Entry mode
 *
 * @param passkey 6-digit passkey (0-999999), 0 to use random
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if passkey > 999999
 */
esp_err_t ble_security_set_passkey(uint32_t passkey);

/**
 * @brief Get current static passkey
 *
 * @return Current passkey (0 if using random)
 */
uint32_t ble_security_get_passkey(void);

/**
 * @brief Enable/disable Secure Connections (LESC)
 *
 * @param enable true to require LESC, false to allow legacy
 * @return ESP_OK on success
 */
esp_err_t ble_security_set_secure_connections(bool enable);

/**
 * @brief Check if Secure Connections is enabled
 *
 * @return true if LESC is required
 */
bool ble_security_is_secure_connections_enabled(void);

/* ============================================================================
 * Pairing Operations
 * ============================================================================ */

/**
 * @brief Initiate pairing with a device
 *
 * Starts the pairing process with a connected device.
 *
 * @param mac Device MAC address
 * @return ESP_OK if pairing initiated
 * @return ESP_ERR_NOT_FOUND if device not connected
 * @return ESP_ERR_INVALID_STATE if pairing already in progress
 * @return ESP_FAIL on pairing error
 */
esp_err_t ble_security_pair(const uint8_t *mac);

/**
 * @brief Initiate pairing by connection handle
 *
 * @param conn_handle Connection handle
 * @return ESP_OK if pairing initiated
 * @return ESP_ERR_INVALID_ARG if handle is invalid
 * @return ESP_FAIL on pairing error
 */
esp_err_t ble_security_pair_by_handle(uint16_t conn_handle);

/**
 * @brief Cancel ongoing pairing
 *
 * @param mac Device MAC address
 * @return ESP_OK if pairing cancelled
 * @return ESP_ERR_NOT_FOUND if no pairing in progress with device
 */
esp_err_t ble_security_cancel_pairing(const uint8_t *mac);

/**
 * @brief Respond to passkey request
 *
 * Call this when a passkey has been entered by the user.
 *
 * @param mac Device MAC address
 * @param passkey Entered passkey (0-999999)
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if no passkey request pending
 * @return ESP_ERR_INVALID_ARG if passkey > 999999
 */
esp_err_t ble_security_respond_passkey(const uint8_t *mac, uint32_t passkey);

/**
 * @brief Respond to numeric comparison
 *
 * Call this with user's confirmation of numeric comparison.
 *
 * @param mac Device MAC address
 * @param accept true if user confirms numbers match
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if no comparison pending
 */
esp_err_t ble_security_respond_numeric_compare(const uint8_t *mac, bool accept);

/**
 * @brief Get current pairing state for a device
 *
 * @param mac Device MAC address
 * @param info Output pairing information
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if no pairing with device
 * @return ESP_ERR_INVALID_ARG if info is NULL
 */
esp_err_t ble_security_get_pairing_info(const uint8_t *mac, ble_pairing_info_t *info);

/**
 * @brief Check if pairing is in progress
 *
 * @return true if any pairing is active
 */
bool ble_security_is_pairing_active(void);

/* ============================================================================
 * Bonding Management
 * ============================================================================ */

/**
 * @brief Get number of stored bonds
 *
 * @param count Output for bond count
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if count is NULL
 */
esp_err_t ble_security_get_bond_count(size_t *count);

/**
 * @brief Get bond information by index
 *
 * @param index Bond index (0 to count-1)
 * @param info Output bond information
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if info is NULL
 * @return ESP_ERR_NOT_FOUND if index out of range
 */
esp_err_t ble_security_get_bond_info(size_t index, ble_bond_info_t *info);

/**
 * @brief Get bond information by MAC address
 *
 * @param mac Device MAC address
 * @param info Output bond information
 * @return ESP_OK if bonded
 * @return ESP_ERR_NOT_FOUND if not bonded
 * @return ESP_ERR_INVALID_ARG if mac or info is NULL
 */
esp_err_t ble_security_get_bond_by_mac(const uint8_t *mac, ble_bond_info_t *info);

/**
 * @brief Remove bond with a device
 *
 * @param mac Device MAC address
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if not bonded
 * @return ESP_ERR_INVALID_ARG if mac is NULL
 * @return ESP_FAIL on NVS error
 */
esp_err_t ble_security_remove_bond(const uint8_t *mac);

/**
 * @brief Clear all stored bonds
 *
 * @return ESP_OK on success
 * @return ESP_FAIL on NVS error
 */
esp_err_t ble_security_clear_all_bonds(void);

/**
 * @brief Check if device is bonded
 *
 * @param mac Device MAC address
 * @return true if device is bonded
 * @return false if not bonded or mac is NULL
 */
bool ble_security_is_bonded(const uint8_t *mac);

/**
 * @brief Update bond timestamp
 *
 * Updates the last_connected timestamp for a bonded device.
 *
 * @param mac Device MAC address
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if not bonded
 */
esp_err_t ble_security_update_bond_timestamp(const uint8_t *mac);

/**
 * @brief Save all bonds to NVS
 *
 * Forces immediate save of bond database to NVS.
 *
 * @return ESP_OK on success
 * @return ESP_FAIL on NVS error
 */
esp_err_t ble_security_save_bonds(void);

/**
 * @brief Load bonds from NVS
 *
 * Reloads bond database from NVS (useful after NVS corruption recovery).
 *
 * @return ESP_OK on success
 * @return ESP_FAIL on NVS error
 */
esp_err_t ble_security_load_bonds(void);

/* ============================================================================
 * Encryption Management
 * ============================================================================ */

/**
 * @brief Request encryption on a connection
 *
 * For bonded devices, uses stored keys. For unbonded, initiates pairing.
 *
 * @param mac Device MAC address
 * @return ESP_OK if encryption started
 * @return ESP_ERR_NOT_FOUND if device not connected
 * @return ESP_FAIL on encryption error
 */
esp_err_t ble_security_encrypt(const uint8_t *mac);

/**
 * @brief Check if connection is encrypted
 *
 * @param mac Device MAC address
 * @return true if connection is encrypted
 * @return false if not encrypted or not connected
 */
bool ble_security_is_encrypted(const uint8_t *mac);

/**
 * @brief Get security level of a connection
 *
 * @param mac Device MAC address
 * @param level Output security level
 * @return ESP_OK on success
 * @return ESP_ERR_NOT_FOUND if not connected
 */
esp_err_t ble_security_get_level(const uint8_t *mac, ble_security_level_t *level);

/* ============================================================================
 * Callback Registration
 * ============================================================================ */

/**
 * @brief Set passkey request callback
 *
 * @param cb Callback function (NULL to disable)
 */
void ble_security_set_passkey_request_cb(ble_passkey_request_cb_t cb);

/**
 * @brief Set passkey display callback
 *
 * @param cb Callback function (NULL to disable)
 */
void ble_security_set_passkey_display_cb(ble_passkey_display_cb_t cb);

/**
 * @brief Set numeric comparison callback
 *
 * @param cb Callback function (NULL to disable)
 */
void ble_security_set_numeric_compare_cb(ble_numeric_compare_cb_t cb);

/**
 * @brief Set pairing complete callback
 *
 * @param cb Callback function (NULL to disable)
 */
void ble_security_set_pairing_complete_cb(ble_pairing_complete_cb_t cb);

/**
 * @brief Set pairing request callback
 *
 * @param cb Callback function (NULL to disable)
 */
void ble_security_set_pairing_request_cb(ble_pairing_request_cb_t cb);

/**
 * @brief Set bond removed callback
 *
 * @param cb Callback function (NULL to disable)
 */
void ble_security_set_bond_removed_cb(ble_bond_removed_cb_t cb);

/**
 * @brief Set encryption change callback
 *
 * @param cb Callback function (NULL to disable)
 */
void ble_security_set_encryption_change_cb(ble_encryption_change_cb_t cb);

/* ============================================================================
 * OOB Pairing Support (Prepared)
 * ============================================================================ */

/**
 * @brief Set OOB data for pairing
 *
 * Sets Out-of-Band data received through alternate channel (NFC, QR code, etc.)
 *
 * @param mac Device MAC address
 * @param oob_data OOB data (16 bytes TK value)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if parameters invalid
 * @return ESP_ERR_NO_MEM if no space for OOB data
 */
esp_err_t ble_security_set_oob_data(const uint8_t *mac, const uint8_t *oob_data);

/**
 * @brief Clear OOB data
 *
 * @param mac Device MAC address (NULL to clear all)
 * @return ESP_OK on success
 */
esp_err_t ble_security_clear_oob_data(const uint8_t *mac);

/**
 * @brief Check if OOB data is available for device
 *
 * @param mac Device MAC address
 * @return true if OOB data set for device
 */
bool ble_security_has_oob_data(const uint8_t *mac);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * @brief Get security mode as string
 *
 * @param mode Security mode
 * @return String representation
 */
const char *ble_security_mode_to_str(ble_security_mode_t mode);

/**
 * @brief Get security level as string
 *
 * @param level Security level
 * @return String representation
 */
const char *ble_security_level_to_str(ble_security_level_t level);

/**
 * @brief Get pairing state as string
 *
 * @param state Pairing state
 * @return String representation
 */
const char *ble_security_pairing_state_to_str(ble_pairing_state_t state);

/**
 * @brief Get I/O capability as string
 *
 * @param io_cap I/O capability
 * @return String representation
 */
const char *ble_security_io_cap_to_str(ble_io_capability_t io_cap);

/**
 * @brief Self-test function for security module
 *
 * @return ESP_OK if all checks pass
 * @return ESP_FAIL if any check fails
 */
esp_err_t ble_security_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_SECURITY_H */
