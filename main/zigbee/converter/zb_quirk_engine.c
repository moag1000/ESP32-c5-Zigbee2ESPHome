/**
 * @file zb_quirk_engine.c
 * @brief Quirk Runtime Engine — Transform Pipeline + Init Sequence
 *
 * Implements runtime quirk data parsing from JSON, value transform
 * pipelines for post-processing fz converter output, and device
 * init sequences (read_attr, write_attr, send_cmd) executed after
 * interview.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_quirk_engine.h"
#include "core/memory/memory_manager_ng.h"
#include "core/gateway_timeouts.h"
#include "esp_zigbee_core.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "QUIRK_ENG";

/* ============================================================================
 * Transform Pipeline
 * ============================================================================ */

void quirk_apply_transforms(const quirk_data_t *qd, cJSON *json)
{
    if (!qd || !json) return;

    for (int i = 0; i < qd->transform_count; i++) {
        cJSON *val = cJSON_GetObjectItem(json, qd->transforms[i].key);
        if (!val) continue;

        const quirk_transform_t *t = &qd->transforms[i].t;

        if (cJSON_IsNumber(val)) {
            double v = (val->valuedouble * t->scale) + t->offset;
            if (t->has_min && v < t->min) v = t->min;
            if (t->has_max && v > t->max) v = t->max;
            cJSON_SetNumberValue(val, v);
        }

        if (t->invert && cJSON_IsBool(val)) {
            cJSON_SetBoolValue(val, !cJSON_IsTrue(val));
        }
    }
}

/* ============================================================================
 * Init Sequence Execution
 * ============================================================================ */

/**
 * @brief Execute a single read_attr init step
 */
static esp_err_t quirk_exec_read_attr(uint16_t short_addr, uint8_t endpoint,
                                       const quirk_init_step_t *step)
{
    uint16_t attr_id = step->attr_or_cmd_id;

    esp_zb_zcl_read_attr_cmd_t read_cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .src_endpoint = 1,
            .dst_endpoint = endpoint,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = step->cluster_id,
        .attr_number = 1,
        .attr_field = &attr_id,
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_read_attr_cmd_req(&read_cmd);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Init read_attr 0x%04X:0x%04X -> 0x%04X EP%d (TSN=%d)",
             step->cluster_id, attr_id, short_addr, endpoint, tsn);
    return ESP_OK;
}

/**
 * @brief Execute a single write_attr init step
 */
static esp_err_t quirk_exec_write_attr(uint16_t short_addr, uint8_t endpoint,
                                        const quirk_init_step_t *step)
{
    /* Use data from step as raw attribute value */
    esp_zb_zcl_attribute_t attr = {
        .id = step->attr_or_cmd_id,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
            .size = step->data_len,
            .value = (uint8_t *)step->data,
        },
    };

    esp_zb_zcl_write_attr_cmd_t write_cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .src_endpoint = 1,
            .dst_endpoint = endpoint,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = step->cluster_id,
        .attr_number = 1,
        .attr_field = &attr,
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_write_attr_cmd_req(&write_cmd);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Init write_attr 0x%04X:0x%04X -> 0x%04X EP%d (TSN=%d)",
             step->cluster_id, step->attr_or_cmd_id, short_addr, endpoint, tsn);
    return ESP_OK;
}

/**
 * @brief Execute a single send_cmd init step
 */
static esp_err_t quirk_exec_send_cmd(uint16_t short_addr, uint8_t endpoint,
                                      const quirk_init_step_t *step)
{
    esp_zb_zcl_custom_cluster_cmd_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = short_addr },
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = step->cluster_id,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .custom_cmd_id = (uint8_t)step->attr_or_cmd_id,
        .data = {
            .type = (step->data_len > 0) ? ESP_ZB_ZCL_ATTR_TYPE_SET : ESP_ZB_ZCL_ATTR_TYPE_NULL,
            .size = step->data_len,
            .value = (step->data_len > 0) ? (uint8_t *)step->data : NULL,
        },
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Init send_cmd 0x%04X:0x%02X -> 0x%04X EP%d (TSN=%d)",
             step->cluster_id, (uint8_t)step->attr_or_cmd_id, short_addr, endpoint, tsn);
    return ESP_OK;
}

esp_err_t quirk_execute_init_sequence(uint16_t short_addr, uint8_t endpoint,
                                       const quirk_data_t *qd)
{
    if (!qd || qd->init_step_count == 0) return ESP_OK;

    ESP_LOGD(TAG, "Executing %d init steps for 0x%04X EP%d",
             qd->init_step_count, short_addr, endpoint);

    for (int i = 0; i < qd->init_step_count; i++) {
        const quirk_init_step_t *step = &qd->init_steps[i];
        esp_err_t ret = ESP_OK;

        switch (step->op) {
        case QUIRK_OP_READ_ATTR:
            ret = quirk_exec_read_attr(short_addr, endpoint, step);
            break;
        case QUIRK_OP_WRITE_ATTR:
            ret = quirk_exec_write_attr(short_addr, endpoint, step);
            break;
        case QUIRK_OP_SEND_CMD:
            ret = quirk_exec_send_cmd(short_addr, endpoint, step);
            break;
        default:
            ESP_LOGW(TAG, "Unknown init op %d at step %d", step->op, i);
            continue;
        }

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Init step %d failed: %s", i, esp_err_to_name(ret));
        }

        /* Delay between steps to avoid flooding the Zigbee network */
        if (i < qd->init_step_count - 1) {
            vTaskDelay(GW_DELAY_SHORT_TICKS);
        }
    }

    return ESP_OK;
}

/* ============================================================================
 * JSON Parsing
 * ============================================================================ */

/**
 * @brief Map operation string to enum
 */
static quirk_op_t parse_op_string(const char *op_str)
{
    if (!op_str) return QUIRK_OP_READ_ATTR;
    if (strcmp(op_str, "read_attr") == 0) return QUIRK_OP_READ_ATTR;
    if (strcmp(op_str, "write_attr") == 0) return QUIRK_OP_WRITE_ATTR;
    if (strcmp(op_str, "send_cmd") == 0) return QUIRK_OP_SEND_CMD;
    ESP_LOGW(TAG, "Unknown op string '%s', defaulting to read_attr", op_str);
    return QUIRK_OP_READ_ATTR;
}

/**
 * @brief Parse "bind" array -> bind_clusters[]
 */
static void parse_bind_clusters(const cJSON *bind_json, quirk_data_t *qd)
{
    if (!cJSON_IsArray(bind_json)) return;

    int count = cJSON_GetArraySize(bind_json);
    if (count > 8) count = 8;

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(bind_json, i);
        if (cJSON_IsNumber(item)) {
            qd->bind_clusters[qd->bind_cluster_count++] = (uint16_t)item->valuedouble;
        }
    }
}

/**
 * @brief Parse "rpt" array -> rpt[] (zb_reporting_override_t)
 * Format: [{"c":1, "a":33, "min":3600, "max":62000, "chg":1}]
 */
static void parse_reporting_overrides(const cJSON *rpt_json, quirk_data_t *qd)
{
    if (!cJSON_IsArray(rpt_json)) return;

    int count = cJSON_GetArraySize(rpt_json);
    if (count > 8) count = 8;

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(rpt_json, i);
        if (!cJSON_IsObject(item)) continue;
        if (qd->rpt_count >= 8) break;

        cJSON *c   = cJSON_GetObjectItem(item, "c");
        cJSON *a   = cJSON_GetObjectItem(item, "a");
        cJSON *min = cJSON_GetObjectItem(item, "min");
        cJSON *max = cJSON_GetObjectItem(item, "max");
        cJSON *chg = cJSON_GetObjectItem(item, "chg");

        if (!cJSON_IsNumber(c) || !cJSON_IsNumber(a)) continue;

        zb_reporting_override_t *rpt = &qd->rpt[qd->rpt_count++];
        rpt->cluster_id = (uint16_t)c->valuedouble;
        rpt->attr_id = (uint16_t)a->valuedouble;
        rpt->min_interval = cJSON_IsNumber(min) ? (uint16_t)min->valuedouble : 60;
        rpt->max_interval = cJSON_IsNumber(max) ? (uint16_t)max->valuedouble : 3600;
        rpt->reportable_change = cJSON_IsNumber(chg) ? (uint16_t)chg->valuedouble : 1;
    }
}

/**
 * @brief Parse "init" array -> init_steps[]
 * Format: [{"op":"read_attr", "c":0, "a":[4,5,7]}, {"op":"write_attr", "c":1, "a":0, "d":[1]}]
 * When "a" is an array, multiple steps are created (one per attribute).
 */
static void parse_init_steps(const cJSON *init_json, quirk_data_t *qd)
{
    if (!cJSON_IsArray(init_json)) return;

    int arr_count = cJSON_GetArraySize(init_json);

    for (int i = 0; i < arr_count && qd->init_step_count < 8; i++) {
        cJSON *item = cJSON_GetArrayItem(init_json, i);
        if (!cJSON_IsObject(item)) continue;

        cJSON *op_json = cJSON_GetObjectItem(item, "op");
        cJSON *c_json  = cJSON_GetObjectItem(item, "c");
        cJSON *a_json  = cJSON_GetObjectItem(item, "a");
        cJSON *d_json  = cJSON_GetObjectItem(item, "d");

        if (!cJSON_IsNumber(c_json)) continue;

        quirk_op_t op = parse_op_string(cJSON_IsString(op_json) ? op_json->valuestring : NULL);
        uint16_t cluster_id = (uint16_t)c_json->valuedouble;

        /* Parse optional data payload from "d" array */
        uint8_t data[8] = {0};
        uint8_t data_len = 0;
        if (cJSON_IsArray(d_json)) {
            int d_count = cJSON_GetArraySize(d_json);
            if (d_count > 8) d_count = 8;
            for (int d = 0; d < d_count; d++) {
                cJSON *db = cJSON_GetArrayItem(d_json, d);
                if (cJSON_IsNumber(db)) {
                    data[data_len++] = (uint8_t)db->valuedouble;
                }
            }
        }

        /* "a" can be a single number or an array of attribute IDs */
        if (cJSON_IsArray(a_json)) {
            /* Multiple attributes -> one step per attribute */
            int a_count = cJSON_GetArraySize(a_json);
            for (int j = 0; j < a_count && qd->init_step_count < 8; j++) {
                cJSON *attr_item = cJSON_GetArrayItem(a_json, j);
                if (!cJSON_IsNumber(attr_item)) continue;

                quirk_init_step_t *step = &qd->init_steps[qd->init_step_count++];
                step->op = (uint8_t)op;
                step->cluster_id = cluster_id;
                step->attr_or_cmd_id = (uint16_t)attr_item->valuedouble;
                memcpy(step->data, data, data_len);
                step->data_len = data_len;
            }
        } else if (cJSON_IsNumber(a_json)) {
            /* Single attribute/command ID */
            quirk_init_step_t *step = &qd->init_steps[qd->init_step_count++];
            step->op = (uint8_t)op;
            step->cluster_id = cluster_id;
            step->attr_or_cmd_id = (uint16_t)a_json->valuedouble;
            memcpy(step->data, data, data_len);
            step->data_len = data_len;
        } else {
            ESP_LOGW(TAG, "Init step %d: missing 'a' field", i);
        }
    }
}

/**
 * @brief Parse "transforms" object -> transforms[]
 * Format: {"battery": {"scale":0.5, "min":0, "max":100}, "temperature": {"offset":-40}}
 */
static void parse_transforms(const cJSON *transforms_json, quirk_data_t *qd)
{
    if (!cJSON_IsObject(transforms_json)) return;

    cJSON *child = transforms_json->child;
    while (child && qd->transform_count < 8) {
        if (!cJSON_IsObject(child)) {
            child = child->next;
            continue;
        }

        /* Key is the JSON property name to transform */
        size_t key_len = strlen(child->string);
        if (key_len >= 16) {
            ESP_LOGW(TAG, "Transform key '%s' too long (max 15), skipping", child->string);
            child = child->next;
            continue;
        }

        int idx = qd->transform_count++;
        strncpy(qd->transforms[idx].key, child->string, 15);
        qd->transforms[idx].key[15] = '\0';

        quirk_transform_t *t = &qd->transforms[idx].t;

        /* Defaults */
        t->scale = 1.0f;
        t->offset = 0.0f;
        t->has_min = false;
        t->has_max = false;
        t->invert = false;

        cJSON *scale_j  = cJSON_GetObjectItem(child, "scale");
        cJSON *offset_j = cJSON_GetObjectItem(child, "offset");
        cJSON *min_j    = cJSON_GetObjectItem(child, "min");
        cJSON *max_j    = cJSON_GetObjectItem(child, "max");
        cJSON *invert_j = cJSON_GetObjectItem(child, "invert");

        if (cJSON_IsNumber(scale_j))  t->scale  = (float)scale_j->valuedouble;
        if (cJSON_IsNumber(offset_j)) t->offset = (float)offset_j->valuedouble;
        if (cJSON_IsNumber(min_j))    { t->min = (float)min_j->valuedouble; t->has_min = true; }
        if (cJSON_IsNumber(max_j))    { t->max = (float)max_j->valuedouble; t->has_max = true; }
        if (cJSON_IsBool(invert_j))   t->invert = cJSON_IsTrue(invert_j);

        child = child->next;
    }
}

/* ============================================================================
 * Public API: Parse + Free
 * ============================================================================ */

quirk_data_t *quirk_data_parse(const cJSON *quirks_json)
{
    if (!quirks_json || !cJSON_IsObject(quirks_json)) return NULL;

    quirk_data_t *qd = mem_ng_calloc(1, sizeof(quirk_data_t), MEM_CAP_DEFAULT);
    if (!qd) {
        ESP_LOGW(TAG, "Failed to allocate quirk_data_t (%zu bytes)", sizeof(quirk_data_t));
        return NULL;
    }

    /* Parse each section */
    parse_bind_clusters(cJSON_GetObjectItem(quirks_json, "bind"), qd);
    parse_reporting_overrides(cJSON_GetObjectItem(quirks_json, "rpt"), qd);
    parse_init_steps(cJSON_GetObjectItem(quirks_json, "init"), qd);
    parse_transforms(cJSON_GetObjectItem(quirks_json, "transforms"), qd);

    ESP_LOGD(TAG, "Parsed quirk: %d binds, %d rpt, %d init steps, %d transforms",
             qd->bind_cluster_count, qd->rpt_count,
             qd->init_step_count, qd->transform_count);

    return qd;
}

void quirk_data_free(quirk_data_t *qd)
{
    mem_ng_free(qd);
}
