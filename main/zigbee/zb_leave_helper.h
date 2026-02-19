/**
 * @file zb_leave_helper.h
 * @brief Zigbee Device Leave Cleanup Helper
 *
 * Consolidates device leave cleanup logic (factory reset, manual remove)
 * into a single helper that ensures correct ordering:
 * 1. Publish EVT_DEVICE_LEFT (subscribers need device info from registry)
 * 2. Clear retained MQTT topics
 * 3. Unbind converter + Tuya driver
 * 4. Remove from availability tracker, registry, and NVS
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ZB_LEAVE_HELPER_H
#define ZB_LEAVE_HELPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Complete cleanup when a device leaves the network
 *
 * Safe to call from any task context. Publishes EVT_DEVICE_LEFT before
 * removing the device from registry (subscribers need device info).
 *
 * @param ieee64     Device IEEE address (64-bit)
 * @param short_addr Device short address (for availability/converter unbind)
 */
void zb_device_leave_cleanup(uint64_t ieee64, uint16_t short_addr);

#ifdef __cplusplus
}
#endif

#endif /* ZB_LEAVE_HELPER_H */
