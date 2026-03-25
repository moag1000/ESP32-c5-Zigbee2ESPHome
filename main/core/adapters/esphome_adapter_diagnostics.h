/**
 * @file esphome_adapter_diagnostics.h
 * @brief Per-device diagnostic entities for ESPHome Native API
 *
 * Registers diagnostic entities for each Zigbee device:
 *   - Remove (Button)
 *   - Reconfigure (Button)
 *   - Identify (Button)
 *   - LQI (Sensor, diagnostic)
 *   - RSSI (Sensor, diagnostic)
 *   - Last Seen (TextSensor, diagnostic)
 *   - Zigbee Info (TextSensor, diagnostic, disabled_by_default)
 */

#ifndef ESPHOME_ADAPTER_DIAGNOSTICS_H
#define ESPHOME_ADAPTER_DIAGNOSTICS_H

#include "esp_err.h"
#include "core/device/unified_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Entity key offset for per-device diagnostic entities */
#define ESPHOME_ADAPTER_DIAG_KEY_OFFSET  0x50000000

/**
 * @brief Register diagnostic entities for a Zigbee device
 *
 * Creates: Remove, Reconfigure, Identify buttons and LQI, RSSI sensors,
 * Last Seen, Zigbee Info text sensors.
 *
 * @param dev Pointer to device (must be Zigbee protocol)
 * @return ESP_OK on success
 */
esp_err_t esphome_adapter_diagnostics_register(const device_t *dev);

/**
 * @brief Update diagnostic entity states for a device
 *
 * Pushes current LQI, RSSI, last_seen values to ESPHome entities.
 *
 * @param dev Pointer to device
 */
void esphome_adapter_diagnostics_update(const device_t *dev);

/**
 * @brief Deinitialize diagnostics module (no-op, stateless)
 */
void esphome_adapter_diagnostics_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ADAPTER_DIAGNOSTICS_H */
