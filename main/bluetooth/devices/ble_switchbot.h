/**
 * @file ble_switchbot.h
 * @brief SwitchBot BLE Device Parser
 *
 * Parses BLE advertisements from SwitchBot devices.
 * Supports multiple device types:
 * - SwitchBot Meter: Temperature and humidity sensor
 * - SwitchBot Bot: Switch/button actuator (status only in advertisement)
 * - SwitchBot Curtain: Motor curtain controller (position status)
 * - SwitchBot Motion: Motion sensor
 * - SwitchBot Contact: Door/window contact sensor
 *
 * Note: Full device control requires GATT connection. This parser
 * only extracts status information from advertisements.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef BLE_SWITCHBOT_H
#define BLE_SWITCHBOT_H

#include "ble_device_registry.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** @brief SwitchBot Company ID (Wonderlabs) */
#define BLE_SWITCHBOT_COMPANY_ID        0x0969

/** @brief SwitchBot service UUID for data service */
#define BLE_SWITCHBOT_SERVICE_UUID      0xFD3D

/** @brief SwitchBot device type identifiers (from service data byte 0) */
#define BLE_SWITCHBOT_TYPE_BOT          0x48    /**< 'H' - SwitchBot Bot */
#define BLE_SWITCHBOT_TYPE_CURTAIN      0x63    /**< 'c' - SwitchBot Curtain */
#define BLE_SWITCHBOT_TYPE_METER        0x54    /**< 'T' - SwitchBot Meter */
#define BLE_SWITCHBOT_TYPE_METER_PLUS   0x69    /**< 'i' - SwitchBot Meter Plus */
#define BLE_SWITCHBOT_TYPE_MOTION       0x73    /**< 's' - SwitchBot Motion */
#define BLE_SWITCHBOT_TYPE_CONTACT      0x64    /**< 'd' - SwitchBot Contact */
#define BLE_SWITCHBOT_TYPE_HUB_MINI     0x77    /**< 'w' - SwitchBot Hub Mini */
#define BLE_SWITCHBOT_TYPE_PLUG_MINI    0x6A    /**< 'j' - SwitchBot Plug Mini */

/** @brief Minimum valid temperature in Celsius */
#define BLE_SWITCHBOT_TEMP_MIN          (-20.0f)

/** @brief Maximum valid temperature in Celsius */
#define BLE_SWITCHBOT_TEMP_MAX          (80.0f)

/** @brief Minimum valid humidity percentage */
#define BLE_SWITCHBOT_HUMIDITY_MIN      (0.0f)

/** @brief Maximum valid humidity percentage */
#define BLE_SWITCHBOT_HUMIDITY_MAX      (100.0f)

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief SwitchBot device type enumeration
 */
typedef enum {
    BLE_SWITCHBOT_MODEL_UNKNOWN = 0,
    BLE_SWITCHBOT_MODEL_BOT,            /**< SwitchBot Bot (button pusher) */
    BLE_SWITCHBOT_MODEL_CURTAIN,        /**< SwitchBot Curtain */
    BLE_SWITCHBOT_MODEL_METER,          /**< SwitchBot Meter */
    BLE_SWITCHBOT_MODEL_METER_PLUS,     /**< SwitchBot Meter Plus */
    BLE_SWITCHBOT_MODEL_MOTION,         /**< SwitchBot Motion Sensor */
    BLE_SWITCHBOT_MODEL_CONTACT,        /**< SwitchBot Contact Sensor */
    BLE_SWITCHBOT_MODEL_HUB_MINI,       /**< SwitchBot Hub Mini */
    BLE_SWITCHBOT_MODEL_PLUG_MINI,      /**< SwitchBot Plug Mini */
} ble_switchbot_model_t;

/**
 * @brief SwitchBot Bot mode enumeration
 */
typedef enum {
    BLE_SWITCHBOT_BOT_MODE_PRESS = 0,   /**< Press mode (momentary) */
    BLE_SWITCHBOT_BOT_MODE_SWITCH = 1,  /**< Switch mode (toggle) */
} ble_switchbot_bot_mode_t;

/**
 * @brief Extended SwitchBot data structure
 *
 * Contains data for all supported SwitchBot device types.
 * Only relevant fields are populated based on device model.
 */
typedef struct {
    ble_switchbot_model_t model;    /**< Detected device model */
    uint8_t battery;                /**< Battery level 0-100% (most devices) */
    bool valid;                     /**< Data validity flag */

    /* Meter/Meter Plus specific */
    float temperature;              /**< Temperature in Celsius */
    float humidity;                 /**< Relative humidity in percent */
    bool has_temperature;           /**< Temperature data present */

    /* Bot specific */
    bool bot_state;                 /**< Bot on/off state */
    ble_switchbot_bot_mode_t bot_mode;  /**< Bot mode (press/switch) */
    bool has_bot_state;             /**< Bot state present */

    /* Curtain specific */
    uint8_t curtain_position;       /**< Curtain position 0-100% (0=open) */
    bool curtain_moving;            /**< Curtain is currently moving */
    bool curtain_calibrated;        /**< Curtain is calibrated */
    bool has_curtain_position;      /**< Curtain position present */

    /* Motion sensor specific */
    bool motion_detected;           /**< Motion detected flag */
    uint8_t motion_brightness;      /**< Ambient brightness level */
    bool has_motion;                /**< Motion data present */

    /* Contact sensor specific */
    bool contact_open;              /**< Contact is open */
    bool contact_timeout;           /**< No activity timeout */
    bool has_contact;               /**< Contact data present */
} ble_switchbot_data_t;

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

/**
 * @brief Detect if advertisement is from SwitchBot device
 *
 * Checks for SwitchBot company ID (0x0969) or service UUID (0xFD3D)
 * in advertisement data.
 *
 * @param[in] adv Advertisement data to analyze
 * @return
 *      - BLE_REG_DEVICE_TYPE_SWITCHBOT_METER if Meter detected
 *      - BLE_REG_DEVICE_TYPE_SWITCHBOT_BOT if Bot detected
 *      - BLE_REG_DEVICE_TYPE_SWITCHBOT_CURTAIN if Curtain detected
 *      - BLE_REG_DEVICE_TYPE_UNKNOWN otherwise
 */
ble_reg_device_type_t ble_switchbot_detect(const ble_registry_adv_data_t *adv);

/**
 * @brief Parse SwitchBot advertisement data
 *
 * Extracts temperature and humidity from SwitchBot Meter.
 * For other device types, returns minimal sensor data.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Parsed sensor data (uses sensor union member)
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if format not recognized
 *      - ESP_ERR_INVALID_RESPONSE if data validation fails
 */
esp_err_t ble_switchbot_parse(const ble_registry_adv_data_t *adv, ble_parsed_data_t *out_data);

/**
 * @brief Parse SwitchBot advertisement with extended data
 *
 * Extracts all available data fields for all device types.
 *
 * @param[in] adv Advertisement data to parse
 * @param[out] out_data Extended SwitchBot data structure
 * @return
 *      - ESP_OK on successful parse
 *      - ESP_ERR_INVALID_ARG if adv or out_data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if format not recognized
 */
esp_err_t ble_switchbot_parse_extended(const ble_registry_adv_data_t *adv, ble_switchbot_data_t *out_data);

/**
 * @brief Check if SwitchBot sensor data is valid
 *
 * Validates temperature and humidity are within expected ranges.
 *
 * @param[in] data Sensor data to validate
 * @return true if data is valid, false otherwise
 */
bool ble_switchbot_validate_data(const ble_sensor_data_t *data);

/**
 * @brief Get model name from SwitchBot model enumeration
 *
 * @param[in] model Device model enumeration
 * @return Model name string ("Meter", "Bot", etc.)
 */
const char *ble_switchbot_get_model_name(ble_switchbot_model_t model);

/**
 * @brief Get detected model from advertisement
 *
 * @param[in] adv Advertisement data
 * @return Detected model enumeration
 */
ble_switchbot_model_t ble_switchbot_get_model(const ble_registry_adv_data_t *adv);

#ifdef __cplusplus
}
#endif

#endif /* BLE_SWITCHBOT_H */
