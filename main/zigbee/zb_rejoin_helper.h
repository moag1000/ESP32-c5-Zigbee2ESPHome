/**
 * @file zb_rejoin_helper.h
 * @brief Zigbee Rejoin Helper — atomic short_addr update during device rejoin
 *
 * Consolidates the duplicated rejoin logic from zb_callbacks.c into a single
 * helper that atomically updates: short_addr, availability tracker, converter
 * binding, Tuya driver, and NVS persistence.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_REJOIN_HELPER_H
#define ZB_REJOIN_HELPER_H

#include "core/device/unified_device.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Atomically update a device's short address during rejoin
 *
 * Handles all side effects of a short_addr change:
 * 1. Updates dev->proto.zigbee.short_addr
 * 2. Swaps availability tracker entry (old → new addr)
 * 3. Rebinds converter (unbind old + pending, bind new)
 * 4. Restores Tuya driver binding (unbind old, restore from NVS)
 * 5. Persists updated device to NVS
 *
 * If old_addr == new_addr, only updates last_seen (no-op for bindings).
 *
 * @param dev       NG device pointer (must not be NULL)
 * @param old_addr  Previous short address (from NVS or last session)
 * @param new_addr  New short address assigned by the network
 */
void zb_rejoin_update_short_addr(device_t *dev, uint16_t old_addr, uint16_t new_addr);

#ifdef __cplusplus
}
#endif

#endif /* ZB_REJOIN_HELPER_H */
