/**
 * @file zb_quirk_engine.h
 * @brief Quirk Runtime Engine — Transform Pipeline + Init Sequence
 *
 * Provides runtime quirk data parsing, value transforms, and
 * device init sequences for device-specific workarounds.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#ifndef ZB_QUIRK_ENGINE_H
#define ZB_QUIRK_ENGINE_H

#include "esp_err.h"
#include "cJSON.h"
#include "zb_converter_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Transform operations for value post-processing */
typedef struct {
    float scale;            /**< Multiplier (default 1.0) */
    float offset;           /**< Addend (default 0.0) */
    float min, max;         /**< Clamp range */
    bool has_min, has_max;
    bool invert;            /**< Invert boolean values */
} quirk_transform_t;

/** Init sequence operation types */
typedef enum {
    QUIRK_OP_READ_ATTR = 0,
    QUIRK_OP_WRITE_ATTR = 1,
    QUIRK_OP_SEND_CMD = 2,
} quirk_op_t;

/** Single init step */
typedef struct {
    uint8_t op;             /**< quirk_op_t */
    uint16_t cluster_id;
    uint16_t attr_or_cmd_id;
    uint8_t data[8];
    uint8_t data_len;
} quirk_init_step_t;

/** Complete quirk data for a device (PSRAM allocated) */
typedef struct quirk_data {
    struct { char key[16]; quirk_transform_t t; } transforms[8];
    uint8_t transform_count;

    uint16_t bind_clusters[8];
    uint8_t bind_cluster_count;

    zb_reporting_override_t rpt[8];
    uint8_t rpt_count;

    quirk_init_step_t init_steps[8];
    uint8_t init_step_count;
} quirk_data_t;

/**
 * @brief Apply transforms to JSON state after fz conversion
 * Called by converter registry after each fz dispatch.
 */
void quirk_apply_transforms(const quirk_data_t *qd, cJSON *json);

/**
 * @brief Execute init sequence after device interview
 * Runs read_attr, write_attr, send_cmd operations.
 */
esp_err_t quirk_execute_init_sequence(uint16_t short_addr, uint8_t endpoint,
                                       const quirk_data_t *qd);

/**
 * @brief Parse quirk data from JSON "quirks" object
 * Returns heap-allocated (PSRAM) quirk_data_t or NULL if no quirks.
 */
quirk_data_t *quirk_data_parse(const cJSON *quirks_json);

/**
 * @brief Free quirk data
 */
void quirk_data_free(quirk_data_t *qd);

#ifdef __cplusplus
}
#endif
#endif /* ZB_QUIRK_ENGINE_H */
