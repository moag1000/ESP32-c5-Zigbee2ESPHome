/**
 * @file esphome_adapter_gateway.h
 * @brief Gateway management entities for ESPHome Native API
 */

#ifndef ESPHOME_ADAPTER_GATEWAY_H
#define ESPHOME_ADAPTER_GATEWAY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Entity key offset for gateway management entities */
#define ESPHOME_ADAPTER_GATEWAY_KEY_OFFSET  0x40000000

/**
 * @brief Register gateway management entities (device_id=0)
 *
 * Registers: Permit Join switch, Duration number, Network Heal button,
 * Channel sensor, Device Count sensor, PAN ID text, Coordinator State text.
 *
 * Safe to call multiple times (idempotent).
 */
esp_err_t esphome_adapter_gateway_register(void);

/**
 * @brief Push current gateway state to all entities
 *
 * Updates channel, PAN ID, device count, coordinator state, permit join.
 * Called on client connect and periodically.
 */
void esphome_adapter_gateway_update_state(void);

/**
 * @brief Deinitialize gateway entities
 *
 * Unsubscribes from event bus.
 */
void esphome_adapter_gateway_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_ADAPTER_GATEWAY_H */
