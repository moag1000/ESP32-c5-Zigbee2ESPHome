/**
 * @file zb_converter_fn_registry.h
 * @brief Function Name Registry for Runtime Converter Loader
 *
 * Maps string function names (from JSON converter DB) to C function pointers
 * using binary search for O(log n) lookup.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef ZB_CONVERTER_FN_REGISTRY_H
#define ZB_CONVERTER_FN_REGISTRY_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief fromZigbee converter function type */
typedef esp_err_t (*fz_convert_fn_t)(const void *raw, size_t len, cJSON *json, const char *key);

/** @brief toZigbee converter function type */
typedef esp_err_t (*tz_convert_fn_t)(uint16_t short_addr, uint8_t endpoint, const cJSON *value);

/**
 * @brief Look up a fromZigbee converter function by name
 * @param name Function name string (e.g. "fz_on_off")
 * @return Function pointer, or NULL if not found
 */
fz_convert_fn_t zb_fn_registry_find_fz(const char *name);

/**
 * @brief Look up a toZigbee converter function by name
 * @param name Function name string (e.g. "tz_on_off")
 * @return Function pointer, or NULL if not found
 */
tz_convert_fn_t zb_fn_registry_find_tz(const char *name);

/**
 * @brief Get number of registered fz functions
 */
size_t zb_fn_registry_fz_count(void);

/**
 * @brief Get number of registered tz functions
 */
size_t zb_fn_registry_tz_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CONVERTER_FN_REGISTRY_H */
