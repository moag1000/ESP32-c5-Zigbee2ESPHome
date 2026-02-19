/**
 * @file ble_constants.h
 * @brief BLE protocol and timing constants
 *
 * This header defines constants for BLE initialization, GATT client operations,
 * security settings, and device-specific sensor configurations to eliminate
 * magic numbers throughout the Bluetooth modules.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_CONSTANTS_H
#define BLE_CONSTANTS_H

#include "ble_common.h"  /* Shared types and validation macros */

/* ============================================================================
 * Initialization and Manager Constants
 * ============================================================================ */

/** @brief BLE host sync timeout in milliseconds */
#define BLE_INIT_TIMEOUT_MS                 5000

/** @brief BLE initialization poll delay in milliseconds */
#define BLE_INIT_POLL_DELAY_MS              100

/** @brief Low heap warning threshold in bytes */
#define BLE_MANAGER_LOW_HEAP_THRESHOLD      20000

/** @brief BLE task stack warning threshold in bytes */
#define BLE_STACK_WARNING_THRESHOLD         512

/* ============================================================================
 * GATT Client Constants
 * ============================================================================ */

/** @brief GATT operation poll delay in milliseconds */
#define BLE_GATT_POLL_DELAY_MS              100

/** @brief GATT reconnect delay in microseconds */
#define BLE_GATT_RECONNECT_DELAY_US         100000

/** @brief Maximum supported MTU size */
#define BLE_MTU_MAX                         517

/** @brief BLE connection interval unit in microseconds (0.625ms) */
#define BLE_INTERVAL_UNIT_US                625

/** @brief Deinit wait delay in milliseconds */
#define BLE_DEINIT_WAIT_DELAY_MS            100

/* ============================================================================
 * Security Constants
 * ============================================================================ */

/** @brief Minimum encryption key size in bytes */
#define BLE_ENC_KEY_SIZE_MIN                7

/** @brief Maximum encryption key size in bytes */
#define BLE_ENC_KEY_SIZE_MAX                16

/** @brief Maximum valid passkey value */
#define BLE_PASSKEY_MAX                     999999

/** @brief Random passkey modulo value */
#define BLE_PASSKEY_RANDOM_MAX              1000000

/* ============================================================================
 * Sensor Data Scale Factors
 * ============================================================================ */

/** @brief Common temperature/humidity scale (divide by 100 for degrees/percent) */
#define BLE_SENSOR_TEMP_HUM_SCALE           100.0f

/** @brief Sensor encoding divisor for Govee format */
#define BLE_SENSOR_ENCODING_DIVISOR         1000

/* ============================================================================
 * Ruuvi Tag Constants
 * ============================================================================ */

/** @brief Ruuvi battery voltage for 100% (CR2477 typical, in mV) */
#define BLE_RUUVI_BATTERY_FULL_MV           3000

/** @brief Ruuvi battery voltage for 0% (cutoff, in mV) */
#define BLE_RUUVI_BATTERY_EMPTY_MV          2000

/** @brief Ruuvi pressure offset in Pa (subtracted before encoding) */
#define BLE_RUUVI_PRESSURE_OFFSET_PA        50000

/** @brief Ruuvi pressure scale factor (Pa to hPa conversion) */
#define BLE_RUUVI_PRESSURE_SCALE            100.0f

/** @brief Ruuvi RAWv2 temperature resolution (0.005 C) */
#define BLE_RUUVI_TEMP_RESOLUTION_V2        0.005f

/** @brief Ruuvi RAWv2 humidity resolution (0.0025%) */
#define BLE_RUUVI_HUM_RESOLUTION_V2         0.0025f

/** @brief Ruuvi RAWv1 humidity resolution (0.5%) */
#define BLE_RUUVI_HUM_RESOLUTION_V1         0.5f

/** @brief Ruuvi RAWv1 temperature fraction scale */
#define BLE_RUUVI_TEMP_FRAC_SCALE_V1        100.0f

/** @brief Ruuvi battery offset for RAWv2 in mV */
#define BLE_RUUVI_BATTERY_OFFSET_MV_V2      1600

/** @brief Ruuvi TX power offset (value * 2 - 40 = dBm) */
#define BLE_RUUVI_TX_POWER_OFFSET           40

/* ============================================================================
 * Beacon Constants
 * ============================================================================ */

/** @brief Beacon temperature fraction divisor (8.8 fixed point) */
#define BLE_BEACON_TEMP_FRAC_DIVISOR        256.0f

/** @brief Maximum reasonable beacon distance estimate in meters */
#define BLE_BEACON_DISTANCE_MAX             100.0f

/* ============================================================================
 * Battery Service Constants
 * ============================================================================ */

/** @brief Maximum battery percentage value */
#define BLE_BATTERY_PERCENTAGE_MAX          100

/** @brief Voltage to mV conversion factor */
#define BLE_VOLTAGE_TO_MV_SCALE             1000.0f

/* ============================================================================
 * RSSI and Distance Thresholds
 * ============================================================================ */

/** @brief RSSI threshold for "immediate" presence zone (very close, <1m) */
#define BLE_RSSI_IMMEDIATE_THRESHOLD        (-50)

/** @brief RSSI threshold for "near" presence zone (close, 1-3m) */
#define BLE_RSSI_NEAR_THRESHOLD             (-70)

/** @brief RSSI threshold for "far" presence zone (>3m) */
#define BLE_RSSI_FAR_THRESHOLD              (-85)

/* ============================================================================
 * Xiaomi/ATC Firmware Constants
 * ============================================================================ */

/** @brief Xiaomi ATC format temperature scale (0.01 C per unit) */
#define BLE_XIAOMI_TEMP_SCALE               100.0f

/** @brief Xiaomi pvvx humidity scale (0.01% per unit) */
#define BLE_XIAOMI_HUM_SCALE_PVVX           100.0f

/* ============================================================================
 * SwitchBot Constants
 * ============================================================================ */

/** @brief SwitchBot meter temperature decimal step */
#define BLE_SWITCHBOT_TEMP_DECIMAL_STEP     10.0f

/** @brief SwitchBot humidity decimal step */
#define BLE_SWITCHBOT_HUM_DECIMAL_STEP      0.5f

/* ============================================================================
 * Inkbird Constants
 * ============================================================================ */

/** @brief Inkbird temperature/humidity scale (0.01 per unit) */
#define BLE_INKBIRD_TEMP_HUM_SCALE          100.0f

/* ============================================================================
 * Govee Constants
 * ============================================================================ */

/** @brief Govee encoded value humidity divisor */
#define BLE_GOVEE_HUM_DIVISOR               10.0f

/** @brief Govee encoded value temperature divisor */
#define BLE_GOVEE_TEMP_DIVISOR              10.0f

/** @brief Govee encoding modulo for humidity extraction */
#define BLE_GOVEE_ENCODING_MODULO           1000

/* ============================================================================
 * String and Buffer Size Constants
 * ============================================================================ */

/** @brief BLE UUID string length (XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX + null) */
#define BLE_UUID_STRING_LEN                 37

/** @brief Minimum JSON buffer size for GATT discovery operations */
#define BLE_DISCOVERY_JSON_MIN_SIZE         64

/** @brief MAC address string length (XX:XX:XX:XX:XX:XX + null) */
#define BLE_MAC_ADDRESS_STRING_LEN          18

/** @brief iBeacon UUID buffer size (same as BLE_UUID_STRING_LEN) */
#define BLE_BEACON_UUID_BUFFER_SIZE         37

/** @brief Eddystone namespace buffer size (20 hex chars + null) */
#define BLE_BEACON_NAMESPACE_BUFFER_SIZE    21

/* ============================================================================
 * Date/Time Validation Constants
 * ============================================================================ */

/** @brief Unix epoch threshold for date validation (2001-09-09 01:46:40 UTC) */
#define BLE_UNIX_EPOCH_THRESHOLD            1000000000UL

#endif /* BLE_CONSTANTS_H */
