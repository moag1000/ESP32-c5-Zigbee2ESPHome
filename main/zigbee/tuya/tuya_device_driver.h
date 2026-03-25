/**
 * @file tuya_device_driver.h
 * @brief Tuya Device Driver Vtable Interface
 *
 * Defines the interface (vtable) for Tuya device-specific drivers.
 * Each Tuya device type (Fingerbot, Curtain, Valve, etc.) implements
 * this interface to handle its DPs, commands, state, and discovery.
 *
 * Tuya devices may use BOTH the Tuya Private Cluster (0xEF00) AND
 * standard ZCL clusters (e.g. On/Off 0x0006). The driver decides
 * which protocol to use for each command.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef TUYA_DEVICE_DRIVER_H
#define TUYA_DEVICE_DRIVER_H

#include "esp_err.h"
#include "zigbee/zb_tuya.h"
#include "core/device/unified_device.h"
#include "cJSON.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tuya entity type for ESPHome auto-registration
 *
 * Controls how a JSON field from build_state_json() maps to an ESPHome entity.
 * AUTO means the adapter infers the type from the cJSON value type.
 */
typedef enum {
    TUYA_ENTITY_AUTO = 0,       /**< Auto-detect from JSON value type */
    TUYA_ENTITY_SWITCH,         /**< Boolean → ESPHome Switch (writable) */
    TUYA_ENTITY_SENSOR,         /**< Number → ESPHome Sensor (read-only) */
    TUYA_ENTITY_NUMBER,         /**< Number → ESPHome Number (writable, min/max/step) */
    TUYA_ENTITY_SELECT,         /**< String → ESPHome Select (writable, options) */
    TUYA_ENTITY_TEXT_SENSOR,    /**< String → ESPHome TextSensor (read-only) */
    TUYA_ENTITY_TEXT,           /**< String → ESPHome Text (writable) */
    TUYA_ENTITY_BINARY_SENSOR,  /**< Boolean → ESPHome BinarySensor (read-only) */
    TUYA_ENTITY_SKIP,           /**< Do not create an entity for this field */
} tuya_entity_type_t;

/**
 * @brief Per-field metadata for ESPHome entity registration
 *
 * Optional: if a driver provides entity_meta, the adapter uses it to
 * upgrade auto-detected types (e.g., sensor → number with min/max).
 */
typedef struct {
    const char *field_name;         /**< JSON key from build_state_json() */
    tuya_entity_type_t entity_type; /**< Override entity type (or AUTO) */
    const char *device_class;       /**< ESPHome device_class string (NULL = none) */
    const char *unit;               /**< Unit of measurement (NULL = none) */
    const char *icon;               /**< MDI icon (NULL = default) */
    float min;                      /**< Number min (only for NUMBER) */
    float max;                      /**< Number max (only for NUMBER) */
    float step;                     /**< Number step (only for NUMBER) */
    const char *const *options;     /**< Select options array (only for SELECT) */
    uint8_t option_count;           /**< Number of select options */
} tuya_entity_meta_t;

/**
 * @brief Tuya device driver interface (vtable)
 *
 * Each Tuya device type provides a static instance of this struct
 * with function pointers for device-specific behavior.
 */
typedef struct tuya_device_driver {
    /** @brief Driver name (e.g. "fingerbot", "curtain") */
    const char *name;

    /**
     * @brief Check if this driver handles the given device
     *
     * @param manufacturer Manufacturer string (e.g. "_TZ3210")
     * @param model Model string (e.g. "TS004F")
     * @return true if this driver handles the device
     */
    bool (*match)(const char *manufacturer, const char *model);

    /**
     * @brief Process incoming Tuya DP (cmd 0x01/0x02/0x05)
     *
     * Called when a DP report is received from the device.
     *
     * @param short_addr Device short address
     * @param dp Parsed DP structure
     * @return ESP_OK on success
     */
    esp_err_t (*process_dp)(uint16_t short_addr, const tuya_dp_t *dp);

    /**
     * @brief Handle MQTT command JSON
     *
     * Can send both Tuya DPs (via zb_tuya_send_dp) and standard ZCL
     * commands (via command_send_on_off etc.).
     *
     * @param short_addr Device short address
     * @param endpoint Device endpoint
     * @param json Parsed JSON command object
     * @return ESP_OK if at least one field was handled
     * @return ESP_ERR_NOT_FOUND if no known field in JSON
     */
    esp_err_t (*handle_command)(uint16_t short_addr, uint8_t endpoint,
                                const cJSON *json);

    /**
     * @brief Build device-specific state JSON for MQTT publish
     *
     * @param short_addr Device short address
     * @return cJSON object (caller must delete), or NULL on error
     */
    cJSON *(*build_state_json)(uint16_t short_addr);

    /**
     * @brief Publish HA discovery entities
     *
     * @param device Unified device (device_t)
     * @return ESP_OK on success
     */
    esp_err_t (*publish_discovery)(const device_t *device);

    /**
     * @brief Initialize device state (optional, may be NULL)
     *
     * Called when driver is bound to a device.
     *
     * @param short_addr Device short address
     * @return ESP_OK on success
     */
    esp_err_t (*init_device)(uint16_t short_addr);

    /**
     * @brief Remove device state (optional, may be NULL)
     *
     * Called when device is removed from the network.
     *
     * @param short_addr Device short address
     */
    void (*remove_device)(uint16_t short_addr);

    /** @brief Optional entity metadata array for ESPHome registration */
    const tuya_entity_meta_t *entity_meta;

    /** @brief Number of entries in entity_meta array */
    uint8_t entity_meta_count;

} tuya_device_driver_t;

#ifdef __cplusplus
}
#endif

#endif /* TUYA_DEVICE_DRIVER_H */
