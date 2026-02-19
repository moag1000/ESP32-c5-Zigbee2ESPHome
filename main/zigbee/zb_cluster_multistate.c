/**
 * @file zb_cluster_multistate.c
 * @brief Multistate Input/Output/Value Cluster (0x0012, 0x0013, 0x0014) Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_cluster_multistate.h"
#include "zb_cluster_internal.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ZB_MULTISTATE";

/* Module state */
static bool s_multistate_initialized = false;
static zb_multistate_state_t s_multistate_states[ZB_STATE_MAX_MULTISTATE];
static uint16_t s_multistate_addrs[ZB_STATE_MAX_MULTISTATE];
static size_t s_multistate_count = 0;
static zb_multistate_state_cb_t s_multistate_callback = NULL;

/* ============================================================================
 * Module Init/Deinit
 * ============================================================================ */

esp_err_t zb_cluster_multistate_init(void)
{
    s_multistate_count = 0;
    s_multistate_callback = NULL;
    memset(s_multistate_states, 0, sizeof(s_multistate_states));
    memset(s_multistate_addrs, 0, sizeof(s_multistate_addrs));
    s_multistate_initialized = true;
    ESP_LOGI(TAG, "Multistate cluster module initialized");
    return ESP_OK;
}

void zb_cluster_multistate_deinit(void)
{
    s_multistate_count = 0;
    s_multistate_callback = NULL;
    s_multistate_initialized = false;
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static int find_multistate_state_index(uint16_t short_addr, uint8_t endpoint)
{
    for (size_t i = 0; i < s_multistate_count; i++) {
        if (s_multistate_addrs[i] == short_addr) {
            if (endpoint == 0 || s_multistate_states[i].endpoint == endpoint) {
                return (int)i;
            }
        }
    }
    return -1;
}

static zb_multistate_state_t* get_or_create_multistate_state(uint16_t short_addr,
                                                               uint8_t endpoint,
                                                               zb_multistate_type_t type)
{
    int idx = find_multistate_state_index(short_addr, endpoint);
    if (idx >= 0) {
        return &s_multistate_states[idx];
    }

    if (s_multistate_count >= ZB_STATE_MAX_MULTISTATE) {
        ESP_LOGW(TAG, "Multistate state storage full");
        return NULL;
    }

    idx = s_multistate_count++;
    s_multistate_addrs[idx] = short_addr;
    memset(&s_multistate_states[idx], 0, sizeof(zb_multistate_state_t));
    s_multistate_states[idx].type = type;
    s_multistate_states[idx].endpoint = endpoint;

    ESP_LOGI(TAG, "Created multistate state for device 0x%04X EP%d type=%s",
             short_addr, endpoint, zb_multistate_type_to_string(type));
    return &s_multistate_states[idx];
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t zb_multistate_register_callback(zb_multistate_state_cb_t callback)
{
    s_multistate_callback = callback;
    ESP_LOGI(TAG, "Multistate state callback registered");
    return ESP_OK;
}

bool zb_device_has_multistate_input(uint16_t short_addr)
{
    return zb_device_has_cluster(short_addr, ZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT);
}

bool zb_device_has_multistate_output(uint16_t short_addr)
{
    return zb_device_has_cluster(short_addr, ZB_ZCL_CLUSTER_ID_MULTISTATE_OUTPUT);
}

bool zb_device_has_multistate_value(uint16_t short_addr)
{
    return zb_device_has_cluster(short_addr, ZB_ZCL_CLUSTER_ID_MULTISTATE_VALUE);
}

bool zb_device_has_multistate(uint16_t short_addr)
{
    return zb_device_has_multistate_input(short_addr) ||
           zb_device_has_multistate_output(short_addr) ||
           zb_device_has_multistate_value(short_addr);
}

int zb_multistate_get_type_from_cluster(uint16_t cluster_id)
{
    switch (cluster_id) {
        case ZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT:
            return ZB_MULTISTATE_TYPE_INPUT;
        case ZB_ZCL_CLUSTER_ID_MULTISTATE_OUTPUT:
            return ZB_MULTISTATE_TYPE_OUTPUT;
        case ZB_ZCL_CLUSTER_ID_MULTISTATE_VALUE:
            return ZB_MULTISTATE_TYPE_VALUE;
        default:
            return -1;
    }
}

const char* zb_multistate_type_to_string(zb_multistate_type_t type)
{
    switch (type) {
        case ZB_MULTISTATE_TYPE_INPUT:
            return "input";
        case ZB_MULTISTATE_TYPE_OUTPUT:
            return "output";
        case ZB_MULTISTATE_TYPE_VALUE:
            return "value";
        default:
            return "unknown";
    }
}

bool zb_multistate_is_in_alarm(const zb_multistate_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    return (state->status_flags & ZB_MULTISTATE_STATUS_IN_ALARM) != 0;
}

bool zb_multistate_has_fault(const zb_multistate_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    return (state->status_flags & ZB_MULTISTATE_STATUS_FAULT) != 0;
}

esp_err_t zb_multistate_read_state(uint16_t short_addr, uint8_t endpoint,
                                    uint16_t cluster_id)
{
    if (!s_multistate_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int type = zb_multistate_get_type_from_cluster(cluster_id);
    if (type < 0) {
        ESP_LOGE(TAG, "Invalid multistate cluster ID: 0x%04X", cluster_id);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Reading multistate state from 0x%04X EP%d cluster=0x%04X",
             short_addr, endpoint, cluster_id);

    esp_zb_zcl_read_attr_cmd_t cmd_req = {
        ZCL_BASIC_CMD_INIT(short_addr, endpoint),
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
    };

    uint16_t attr_ids[] = {
        ZB_ZCL_ATTR_MULTISTATE_NUMBER_OF_STATES,
        ZB_ZCL_ATTR_MULTISTATE_PRESENT_VALUE,
        ZB_ZCL_ATTR_MULTISTATE_OUT_OF_SERVICE,
        ZB_ZCL_ATTR_MULTISTATE_STATUS_FLAGS
    };
    cmd_req.attr_number = sizeof(attr_ids) / sizeof(attr_ids[0]);
    cmd_req.attr_field = attr_ids;

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_read_attr_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Multistate read request queued, TSN=0x%02X", tsn);
    return ESP_OK;
}

esp_err_t zb_multistate_set_value(uint16_t short_addr, uint8_t endpoint,
                                   uint16_t cluster_id, uint16_t value)
{
    if (!s_multistate_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int type = zb_multistate_get_type_from_cluster(cluster_id);
    if (type < 0) {
        ESP_LOGE(TAG, "Invalid multistate cluster ID: 0x%04X", cluster_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (type == ZB_MULTISTATE_TYPE_INPUT) {
        ESP_LOGE(TAG, "Cannot write to Multistate Input cluster (read-only)");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Setting multistate value on 0x%04X EP%d to %u",
             short_addr, endpoint, value);

    esp_zb_zcl_write_attr_cmd_t cmd_req = {
        ZCL_BASIC_CMD_INIT(short_addr, endpoint),
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
    };

    esp_zb_zcl_attribute_t attr = {
        .id = ZB_ZCL_ATTR_MULTISTATE_PRESENT_VALUE,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_U16,
            .size = sizeof(uint16_t),
            .value = (void *)&value,
        },
    };
    cmd_req.attr_number = 1;
    cmd_req.attr_field = &attr;

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_write_attr_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGD(TAG, "Multistate write request queued, TSN=0x%02X", tsn);
    return ESP_OK;
}

esp_err_t zb_multistate_handle_report(uint16_t short_addr, uint8_t endpoint,
                                       uint16_t cluster_id, uint16_t attr_id,
                                       void *value, size_t value_len)
{
    if (value == NULL || value_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int type = zb_multistate_get_type_from_cluster(cluster_id);
    if (type < 0) {
        ESP_LOGW(TAG, "Unknown multistate cluster ID: 0x%04X", cluster_id);
        return ESP_ERR_INVALID_ARG;
    }

    zb_multistate_state_t *state = get_or_create_multistate_state(short_addr, endpoint,
                                                                    (zb_multistate_type_t)type);
    if (state == NULL) {
        return ESP_ERR_NO_MEM;
    }

    switch (attr_id) {
        case ZB_ZCL_ATTR_MULTISTATE_NUMBER_OF_STATES:
            if (value_len >= 2) {
                state->number_of_states = *(uint16_t *)value;
                ESP_LOGI(TAG, "Multistate 0x%04X EP%d: NumberOfStates=%u",
                         short_addr, endpoint, state->number_of_states);
            }
            break;

        case ZB_ZCL_ATTR_MULTISTATE_PRESENT_VALUE:
            if (value_len >= 2) {
                uint16_t new_value = *(uint16_t *)value;
                if (new_value != state->present_value) {
                    ESP_LOGI(TAG, "Multistate 0x%04X EP%d: PresentValue changed %u -> %u",
                             short_addr, endpoint, state->present_value, new_value);
                }
                state->present_value = new_value;
            }
            break;

        case ZB_ZCL_ATTR_MULTISTATE_OUT_OF_SERVICE:
            if (value_len >= 1) {
                state->out_of_service = (*(uint8_t *)value != 0);
                ESP_LOGD(TAG, "Multistate 0x%04X EP%d: OutOfService=%s",
                         short_addr, endpoint, state->out_of_service ? "true" : "false");
            }
            break;

        case ZB_ZCL_ATTR_MULTISTATE_STATUS_FLAGS:
            if (value_len >= 1) {
                state->status_flags = *(uint8_t *)value;
                ESP_LOGD(TAG, "Multistate 0x%04X EP%d: StatusFlags=0x%02X (alarm=%d fault=%d)",
                         short_addr, endpoint, state->status_flags,
                         zb_multistate_is_in_alarm(state),
                         zb_multistate_has_fault(state));
            }
            break;

        default:
            ESP_LOGD(TAG, "Multistate 0x%04X EP%d: unhandled attr 0x%04X",
                     short_addr, endpoint, attr_id);
            return ESP_OK;
    }

    if (s_multistate_callback != NULL) {
        s_multistate_callback(short_addr, endpoint, state);
    }

    return ESP_OK;
}

esp_err_t zb_multistate_get_state(uint16_t short_addr, uint8_t endpoint,
                                   zb_multistate_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int idx = find_multistate_state_index(short_addr, endpoint);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(state, &s_multistate_states[idx], sizeof(zb_multistate_state_t));
    return ESP_OK;
}
