/**
 * @file esphome_ota.h
 * @brief ESPHome OTA Update Support
 *
 * Provides OTA update functionality compatible with ESPHome.
 * Uses standard ESPHome OTA protocol on port 3232.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#ifndef ESPHOME_OTA_H
#define ESPHOME_OTA_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OTA configuration */
typedef struct {
    uint16_t port;           /* Default: 3232 */
    char password[64];       /* OTA password (empty for no auth) */
    bool safe_mode;          /* Enable safe mode after failed update */
    uint32_t timeout_ms;     /* Connection timeout */
} esphome_ota_config_t;

#define ESPHOME_OTA_CONFIG_DEFAULT() { \
    .port = 3232, \
    .password = "", \
    .safe_mode = true, \
    .timeout_ms = 60000, \
}

/* OTA status callback */
typedef void (*esphome_ota_status_cb_t)(int progress_percent, const char *status);

/* API functions */
esp_err_t esphome_ota_init(const esphome_ota_config_t *config);
esp_err_t esphome_ota_deinit(void);
esp_err_t esphome_ota_start(void);
esp_err_t esphome_ota_stop(void);
bool esphome_ota_is_running(void);
bool esphome_ota_in_progress(void);
void esphome_ota_set_status_callback(esphome_ota_status_cb_t callback);

#ifdef __cplusplus
}
#endif

#endif /* ESPHOME_OTA_H */
