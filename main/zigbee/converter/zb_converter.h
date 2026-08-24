/**
 * @file zb_converter.h
 * @brief Zigbee Converter Database Public API
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef ZB_CONVERTER_H
#define ZB_CONVERTER_H

#include "zb_converter_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize converter registry
 * @return ESP_OK on success
 */
esp_err_t zb_converter_registry_init(void);

/**
 * @brief Register all built-in converters
 * @return ESP_OK on success
 */
esp_err_t zb_converter_register_all(void);

/**
 * @brief Register a single converter definition
 * @param def Pointer to const converter definition (must be in .rodata)
 * @return ESP_OK on success
 */
esp_err_t zb_converter_register(const zb_converter_def_t *def);

/**
 * @brief Find converter by manufacturer and model strings
 * @param manufacturer Manufacturer string (from Basic cluster)
 * @param model Model identifier string (from Basic cluster)
 * @return Pointer to converter definition, or NULL if not found
 */
const zb_converter_def_t *zb_converter_find(const char *manufacturer, const char *model);

/**
 * @brief Bind a converter to a device (by short address)
 * @param short_addr Device short address
 * @param def Converter definition to bind
 * @return ESP_OK on success
 */
esp_err_t zb_converter_bind(uint16_t short_addr, const zb_converter_def_t *def);

/**
 * @brief Unbind converter from a device
 * @param short_addr Device short address
 * @return ESP_OK on success
 */
esp_err_t zb_converter_unbind(uint16_t short_addr);

/**
 * @brief Unbind all converter bindings
 *
 * Clears the entire binding table. Used during network reset/factory reset
 * and new network formation to prevent stale bindings.
 */
void zb_converter_unbind_all(void);

/**
 * @brief Get the converter bound to a device
 * @param short_addr Device short address
 * @return Pointer to converter definition, or NULL if none bound
 */
const zb_converter_def_t *zb_converter_get(uint16_t short_addr);

/**
 * @brief Handle an incoming attribute report via converter
 *
 * Looks up converter for the device, finds matching from_zigbee entry,
 * converts attribute to JSON, and publishes to MQTT.
 *
 * @param short_addr Device short address
 * @param endpoint Source endpoint
 * @param cluster_id ZCL cluster ID
 * @param attr_id Attribute ID
 * @param raw Raw attribute data
 * @param len Length of raw data
 * @param zcl_attr_type ZCL attribute data type (0x10=bool, 0x20=uint8, etc.)
 * @return ESP_OK if handled, ESP_ERR_NOT_FOUND if no converter
 */
esp_err_t zb_converter_handle_report(uint16_t short_addr, uint8_t endpoint,
                                      uint16_t cluster_id, uint16_t attr_id,
                                      const void *raw, size_t len,
                                      uint8_t zcl_attr_type);

/**
 * @brief Dispatch context for converter functions
 *
 * Set before calling fz/tz converters so generic converters can access
 * metadata (converter definition, ZCL data type) without signature changes.
 */
typedef struct {
    const zb_converter_def_t *conv;     /**< Active converter definition */
    uint8_t zcl_attr_type;              /**< ZCL attribute data type */
    uint16_t cluster_id;                /**< Matched cluster ID */
    uint16_t attr_id;                   /**< Matched attribute ID */
    /** @brief JSON key of the matched to_zigbee entry, NULL when dispatching a report.
     *
     * tz_tuya_dp() needs to know which datapoint it was called for, and used to
     * recover that by scanning to_zigbee for the first Tuya entry — which is
     * the right answer only when there is exactly one. A device with several
     * writable datapoints would have written all of them to whichever came
     * first in the array. */
    const char *json_key;
} zb_dispatch_ctx_t;

/**
 * @brief Set the dispatch context (call before fz/tz dispatch)
 */
void zb_converter_set_dispatch_ctx(const zb_dispatch_ctx_t *ctx);

/**
 * @brief Get the dispatch context (call from fz/tz converter functions)
 * @return Current dispatch context, or NULL if not set
 */
const zb_dispatch_ctx_t *zb_converter_get_dispatch_ctx(void);

/**
 * @brief Handle an MQTT command via converter
 *
 * Looks up converter for the device, parses JSON payload, finds matching
 * to_zigbee entry, and sends ZCL command.
 *
 * @param short_addr Device short address
 * @param endpoint Device endpoint
 * @param json Parsed JSON command payload
 * @return ESP_OK if handled, ESP_ERR_NOT_FOUND if no converter
 */
esp_err_t zb_converter_handle_command(uint16_t short_addr, uint8_t endpoint,
                                       const cJSON *json);

/**
 * @brief Get converter count
 * @return Number of registered converters
 */
size_t zb_converter_get_count(void);

/**
 * @brief Get device capabilities from converter exposes
 *
 * Maps the converter's expose definitions to device capability flags.
 * This enables automatic capability detection when a converter is
 * matched to a device.
 *
 * Mapping rules:
 *   - ZB_EXPOSE_LIGHT          -> DEV_CAP_ON_OFF | DEV_CAP_BRIGHTNESS
 *                                 (+ DEV_CAP_COLOR_TEMP if ZB_FEATURE_COLOR_TEMP)
 *                                 (+ DEV_CAP_COLOR_XY if ZB_FEATURE_COLOR_XY)
 *   - ZB_EXPOSE_SWITCH         -> DEV_CAP_ON_OFF
 *   - ZB_EXPOSE_SENSOR         -> device_class-dependent:
 *                                 "temperature" -> DEV_CAP_TEMPERATURE
 *                                 "humidity"    -> DEV_CAP_HUMIDITY
 *                                 "pressure"    -> DEV_CAP_PRESSURE
 *                                 "battery"     -> DEV_CAP_BATTERY
 *                                 "power"       -> DEV_CAP_POWER
 *                                 "energy"      -> DEV_CAP_ENERGY
 *                                 "voltage"     -> DEV_CAP_VOLTAGE
 *                                 "current"     -> DEV_CAP_CURRENT
 *   - ZB_EXPOSE_BINARY_SENSOR  -> device_class-dependent:
 *                                 "motion"      -> DEV_CAP_MOTION
 *                                 "occupancy"   -> DEV_CAP_MOTION
 *                                 "contact"     -> DEV_CAP_CONTACT
 *                                 "door"        -> DEV_CAP_CONTACT
 *                                 "window"      -> DEV_CAP_CONTACT
 *                                 "vibration"   -> DEV_CAP_VIBRATION
 *                                 "water_leak"  -> DEV_CAP_WATER_LEAK
 *                                 "moisture"    -> DEV_CAP_WATER_LEAK
 *                                 "smoke"       -> DEV_CAP_SMOKE
 *                                 "co"          -> DEV_CAP_CO
 *   - ZB_EXPOSE_LOCK           -> DEV_CAP_LOCK
 *   - ZB_EXPOSE_COVER          -> DEV_CAP_COVER
 *   - ZB_EXPOSE_FAN            -> DEV_CAP_FAN
 *   - ZB_EXPOSE_CLIMATE        -> DEV_CAP_CLIMATE
 *
 * @param def Pointer to converter definition
 * @return Bitmask of device_capability_t flags, or 0 if def is NULL
 */
uint32_t zb_converter_get_capabilities(const zb_converter_def_t *def);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CONVERTER_H */
