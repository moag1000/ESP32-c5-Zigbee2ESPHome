/**
 * @file esphome_gateway_services.h
 * @brief Gateway operations exposed to Home Assistant as ESPHome services
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_GATEWAY_SERVICES_H
#define ESPHOME_GATEWAY_SERVICES_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the service manager and register the gateway's services
 *
 * Registers, all taking a single argument:
 *   - permit_join(duration: int)         open the network for new devices,
 *                                        0 closes it again
 *   - remove_device(device: string)      friendly name or IEEE address
 *   - reconfigure_device(device: string) re-run the interview
 *
 * Call once, after the ESPHome API is up. Home Assistant picks the definitions
 * up during entity listing, so they appear as esphome.<device>_<service>.
 *
 * @return ESP_OK if every service registered
 * @return the first error otherwise; registration continues for the rest
 */
esp_err_t esphome_gateway_services_register(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_GATEWAY_SERVICES_H */
