/**
 * @file zb_tuya.c
 * @brief Tuya Private Cluster (0xEF00) Implementation
 *
 * Generic Tuya infrastructure: DP parsing, sending, time sync, dispatch.
 * Device-specific logic is handled by drivers registered in the Tuya
 * driver registry (see tuya/tuya_driver_registry.h).
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "zb_tuya.h"
#include "tuya/tuya_driver_registry.h"
#include "tuya/tuya_device_driver.h"
#include "zb_device_handler_types.h"
#include "core/device/device_registry.h"
#include "zb_coordinator.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/discovery/ha_discovery_ng.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "ZB_TUYA";

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Module initialization flag */
static bool s_initialized = false;

/** @brief Mutex for thread-safe access */
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Sequence number for outgoing DP commands */
static uint16_t s_sequence_number = 0;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static esp_err_t build_dp_payload(const tuya_dp_t *dp, uint8_t *buffer,
                                   size_t buffer_len, size_t *payload_len);

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * @brief Update last_seen timestamp for device via NG registry
 *
 * @param short_addr Zigbee short address
 */
static void update_device_last_seen(uint16_t short_addr)
{
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev) {
        device_registry_update_last_seen(dev->id);
    }
}

/* ============================================================================
 * Module Initialization
 * ============================================================================ */

esp_err_t zb_tuya_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Tuya module initialized");
    return ESP_OK;
}

esp_err_t zb_tuya_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_initialized = false;

    ESP_LOGI(TAG, "Tuya module deinitialized");
    return ESP_OK;
}

/* ============================================================================
 * DP Parsing
 * ============================================================================ */

/**
 * @brief Parse Tuya DP from data with specified header offset
 *
 * @param data Raw DP data
 * @param len Data length
 * @param dp Output DP structure
 * @param header_size Header size (6 for cmd 0x05, 7 for others)
 * @return ESP_OK on success
 */
static esp_err_t parse_dp_with_offset(const uint8_t *data, size_t len, tuya_dp_t *dp, size_t header_size)
{
    if (len < header_size) {
        ESP_LOGW(TAG, "DP data too short: %zu bytes (need %zu)", len, header_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* For header_size=6 (cmd 0x05): [seq:2][dp_id:1][type:1][len:2][data...]
     * For header_size=7 (cmd 0x01/02): [status:1][seq:2][dp_id:1][type:1][len:2][data...] */
    size_t dp_offset = header_size - 6;  /* 0 for cmd 0x05, 1 for others */

    dp->dp_id = data[dp_offset + 2];
    dp->type = (tuya_dp_type_t)data[dp_offset + 3];
    dp->length = ((uint16_t)data[dp_offset + 4] << 8) | data[dp_offset + 5];

    /* Validate payload length */
    if (len < header_size + dp->length) {
        ESP_LOGW(TAG, "DP payload truncated: expected %d, got %zu",
                 dp->length, len - header_size);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *payload = &data[header_size];

    /* Parse based on type */
    memset(&dp->value, 0, sizeof(dp->value));

    switch (dp->type) {
        case TUYA_DP_TYPE_BOOL:
            if (dp->length >= 1) {
                dp->value.bool_value = (payload[0] != 0);
            }
            break;

        case TUYA_DP_TYPE_VALUE:
            /* 32-bit big-endian signed integer */
            if (dp->length >= 4) {
                dp->value.int_value = ((int32_t)payload[0] << 24) |
                                      ((int32_t)payload[1] << 16) |
                                      ((int32_t)payload[2] << 8) |
                                       (int32_t)payload[3];
            }
            break;

        case TUYA_DP_TYPE_STRING:
            if (dp->length > 0 && dp->length < ZB_TUYA_DP_MAX_RAW_SIZE) {
                memcpy(dp->value.raw, payload, dp->length);
            }
            break;

        case TUYA_DP_TYPE_ENUM:
            if (dp->length >= 1) {
                dp->value.enum_value = payload[0];
            }
            break;

        case TUYA_DP_TYPE_BITMAP:
            /* 1, 2, or 4 bytes big-endian */
            if (dp->length == 1) {
                dp->value.bitmap_value = payload[0];
            } else if (dp->length == 2) {
                dp->value.bitmap_value = ((uint32_t)payload[0] << 8) | payload[1];
            } else if (dp->length >= 4) {
                dp->value.bitmap_value = ((uint32_t)payload[0] << 24) |
                                         ((uint32_t)payload[1] << 16) |
                                         ((uint32_t)payload[2] << 8) |
                                          payload[3];
            }
            break;

        default:
            /* Raw data */
            if (dp->length > 0 && dp->length <= ZB_TUYA_DP_MAX_RAW_SIZE) {
                memcpy(dp->value.raw, payload, dp->length);
            }
            break;
    }

    ESP_LOGD(TAG, "Parsed DP: id=%d, type=%d, len=%d", dp->dp_id, dp->type, dp->length);
    return ESP_OK;
}

esp_err_t zb_tuya_parse_dp(const uint8_t *data, size_t len, tuya_dp_t *dp)
{
    if (data == NULL || dp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Standard format with 7-byte header */
    return parse_dp_with_offset(data, len, dp, ZB_TUYA_DP_HEADER_SIZE);
}

/**
 * @brief Parse Tuya DP with 6-byte header (for cmd 0x05 status sync)
 *
 * cmd 0x05 uses a shorter header format without the status byte:
 * [seq:2][dp_id:1][type:1][len:2][data...]
 */
static esp_err_t zb_tuya_parse_dp_short(const uint8_t *data, size_t len, tuya_dp_t *dp)
{
    if (data == NULL || dp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Short format with 6-byte header (no status byte) */
    return parse_dp_with_offset(data, len, dp, 6);
}

/* ============================================================================
 * Time Sync
 * ============================================================================ */

/**
 * @brief Send Tuya time sync response (cmd 0x24)
 *
 * Tuya mcuSyncTime payload (zigbee-herdsman format):
 *   [payloadSize:2 BE][UTC_unix:4 BE][Local_unix:4 BE]
 *   payloadSize = 0x0008 (always 8)
 *   UTC_unix = seconds since Unix epoch (1970-01-01)
 *   Local_unix = UTC + timezone offset
 *
 * Note: Some older Tuya devices use epoch 2000 (subtract 946684800),
 * but _TZ3210 Fingerbot Plus uses Unix epoch 1970.
 */
static esp_err_t zb_tuya_send_time_sync(uint16_t short_addr, uint8_t endpoint)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    if (tv.tv_sec < 946684800) {  /* Before 2000-01-01 = time not set */
        ESP_LOGW(TAG, "System time not set, cannot sync time to 0x%04X", short_addr);
        return ESP_ERR_INVALID_STATE;
    }

    /* UTC as Unix timestamp */
    uint32_t utc_time = (uint32_t)tv.tv_sec;

    /* Local time = UTC + timezone offset */
    time_t now = tv.tv_sec;
    struct tm utc_tm, local_tm;
    gmtime_r(&now, &utc_tm);
    localtime_r(&now, &local_tm);

    /* Calculate timezone offset in seconds */
    int32_t tz_offset = (int32_t)mktime(&local_tm) - (int32_t)mktime(&utc_tm);
    uint32_t local_time = utc_time + (uint32_t)tz_offset;

    /* Build payload: [payloadSize:2][UTC:4][Local:4] = 10 bytes */
    uint8_t payload[10];
    /* Payload size = 8 (2 x uint32) */
    payload[0] = 0x00;
    payload[1] = 0x08;
    /* UTC timestamp (big-endian) */
    payload[2] = (utc_time >> 24) & 0xFF;
    payload[3] = (utc_time >> 16) & 0xFF;
    payload[4] = (utc_time >> 8) & 0xFF;
    payload[5] = utc_time & 0xFF;
    /* Local timestamp (big-endian) */
    payload[6] = (local_time >> 24) & 0xFF;
    payload[7] = (local_time >> 16) & 0xFF;
    payload[8] = (local_time >> 8) & 0xFF;
    payload[9] = local_time & 0xFF;

    ESP_LOGI(TAG, "Sending time sync to 0x%04X: UTC=%lu, Local=%lu (tz_offset=%ld)",
             short_addr, (unsigned long)utc_time, (unsigned long)local_time,
             (long)tz_offset);

    esp_zb_zcl_custom_cluster_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = ZB_TUYA_CLUSTER_ID,
        .custom_cmd_id = 0x24,  /* Tuya mcuSyncTime */
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
            .size = sizeof(payload),
            .value = payload,
        },
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd);
    esp_zb_lock_release();

    ESP_LOGI(TAG, "Time sync command queued to 0x%04X (TSN=%d)", short_addr, tsn);
    return ESP_OK;
}

/* ============================================================================
 * DP Name Helpers
 * ============================================================================ */

static const char *tuya_dp_type_name(tuya_dp_type_t type)
{
    switch (type) {
        case TUYA_DP_TYPE_RAW:    return "raw";
        case TUYA_DP_TYPE_BOOL:   return "bool";
        case TUYA_DP_TYPE_VALUE:  return "value";
        case TUYA_DP_TYPE_STRING: return "string";
        case TUYA_DP_TYPE_ENUM:   return "enum";
        case TUYA_DP_TYPE_BITMAP: return "bitmap";
        default:                  return "unknown";
    }
}

static const char *tuya_fingerbot_dp_name(uint8_t dp_id)
{
    switch (dp_id) {
        case TUYA_DP_FINGERBOT_SWITCH:        return "switch";
        case TUYA_DP_FINGERBOT_MODE:          return "mode";
        case TUYA_DP_FINGERBOT_DOWN_MOVEMENT: return "down_movement";
        case TUYA_DP_FINGERBOT_SUSTAIN_TIME:  return "sustain_time";
        case TUYA_DP_FINGERBOT_REVERSE:       return "reverse";
        case TUYA_DP_FINGERBOT_BATTERY:       return "battery";
        case TUYA_DP_FINGERBOT_UP_MOVEMENT:   return "up_movement";
        case TUYA_DP_FINGERBOT_TOUCH_CONTROL: return "touch_control";
        case TUYA_DP_FINGERBOT_CLICK_CONTROL:  return "click_control";
        case TUYA_DP_FINGERBOT_PROGRAM:        return "program";
        case TUYA_DP_FINGERBOT_COUNT:          return "count";
        case TUYA_DP_FINGERBOT_REPEAT_FOREVER: return "repeat_forever";
        case TUYA_DP_FINGERBOT_PROGRAM_ENABLE: return "program_enable";
        default:                               return NULL;
    }
}

/**
 * @brief Log a parsed DP with raw hex and human-readable translation
 */
static void tuya_log_dp(uint16_t short_addr, const tuya_dp_t *dp,
                         const uint8_t *raw_data, size_t raw_len)
{
    const char *dp_name = tuya_fingerbot_dp_name(dp->dp_id);
    const char *type_name = tuya_dp_type_name(dp->type);

    /* Value as string */
    char val_str[48];
    switch (dp->type) {
        case TUYA_DP_TYPE_BOOL:
            snprintf(val_str, sizeof(val_str), "%s", dp->value.bool_value ? "true" : "false");
            break;
        case TUYA_DP_TYPE_VALUE:
            snprintf(val_str, sizeof(val_str), "%ld", (long)dp->value.int_value);
            break;
        case TUYA_DP_TYPE_ENUM:
            snprintf(val_str, sizeof(val_str), "%d", dp->value.enum_value);
            break;
        case TUYA_DP_TYPE_BITMAP:
            snprintf(val_str, sizeof(val_str), "0x%08lx", (unsigned long)dp->value.bitmap_value);
            break;
        default:
            snprintf(val_str, sizeof(val_str), "(%d bytes)", dp->length);
            break;
    }

    if (dp_name != NULL) {
        ESP_LOGI(TAG, "  DP %d [%s] = %s (%s)", dp->dp_id, dp_name, val_str, type_name);
    } else {
        ESP_LOGI(TAG, "  DP %d [unknown] = %s (%s)", dp->dp_id, val_str, type_name);
    }

    /* Raw hex dump */
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, raw_data, raw_len, ESP_LOG_INFO);
}

/* ============================================================================
 * Lazy Driver Binding
 * ============================================================================ */

/**
 * @brief Try to bind a device to a driver if not already bound
 *
 * Called on first DP reception. Attempts to match device manufacturer/model
 * against registered drivers.
 */
static void try_lazy_bind(uint16_t short_addr)
{
    /* Already bound? */
    if (tuya_driver_get(short_addr) != NULL) {
        return;
    }

    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return;
    }

    const tuya_device_driver_t *drv = tuya_driver_find(dev->manufacturer, dev->model);
    if (drv != NULL) {
        tuya_driver_bind(short_addr, drv);
        if (drv->init_device) {
            drv->init_device(short_addr);
        }
        ESP_LOGI(TAG, "Device 0x%04X: lazy-bound to Tuya driver '%s'",
                 short_addr, drv->name);

        /* Re-publish HA discovery now that the driver is bound.
         * Initial discovery ran before binding (manufacturer/model
         * were not yet known), so the driver-specific entities
         * (buttons, selects, numbers, switches) were missing. */
        if (drv->publish_discovery != NULL) {
            /* publish_discovery uses unified device_t */
            device_t *dev = device_registry_get_by_short_addr(short_addr);
            if (dev != NULL) {
                esp_err_t disc_ret = drv->publish_discovery(dev);
                if (disc_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to publish driver discovery for 0x%04X: %s",
                             short_addr, esp_err_to_name(disc_ret));
                }
            }
        }
    }
}

/* ============================================================================
 * Report Handling
 * ============================================================================ */

esp_err_t zb_tuya_handle_command(uint16_t short_addr, uint8_t endpoint,
                                  uint8_t cmd_id, const uint8_t *data, size_t len)
{
    if (!s_initialized || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Tuya cmd 0x%02X from 0x%04X EP%d, len=%zu", cmd_id, short_addr, endpoint, len);

    /* Handle different Tuya command types */
    tuya_dp_t dp;
    esp_err_t ret;

    switch (cmd_id) {
        case 0x00:  /* DP Report (gateway -> device response) */
        case 0x01:  /* DP Set (device -> gateway, report after set) */
        case 0x02:  /* DP Query Response */
            /* 7-byte header format: [status:1][seq:2][dp_id:1][type:1][len:2][data...] */
            ret = zb_tuya_parse_dp(data, len, &dp);
            break;

        case 0x05:  /* DP Status Sync (unsolicited reports) */
            if (len < 6) {
                /* Short response: ACK/status frame (e.g. 5 bytes: [status:1][seq:2][result:2])
                 * These are Tuya command acknowledgements, not DP data. */
                ESP_LOGI(TAG, "Tuya ACK from 0x%04X: %zu bytes", short_addr, len);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_INFO);
                if (len >= 5) {
                    uint16_t seq = ((uint16_t)data[1] << 8) | data[2];
                    uint16_t result = ((uint16_t)data[3] << 8) | data[4];
                    ESP_LOGI(TAG, "  status=0x%02X seq=0x%04X result=0x%04X%s",
                             data[0], seq, result,
                             result == 0xFFFF ? " (UNSUPPORTED)" :
                             result == 0x0000 ? " (OK)" : "");
                }
                update_device_last_seen(short_addr);
                return ESP_OK;
            }
            /* 6-byte header format: [seq:2][dp_id:1][type:1][len:2][data...] */
            ret = zb_tuya_parse_dp_short(data, len, &dp);
            break;

        case 0x06:  /* Time Sync Request from device */
            ESP_LOGI(TAG, "Device 0x%04X requested time sync", short_addr);
            zb_tuya_send_time_sync(short_addr, endpoint);
            update_device_last_seen(short_addr);
            return ESP_OK;

        case 0x24:  /* Time Sync Response */
            ESP_LOGD(TAG, "Time sync response from 0x%04X", short_addr);
            return ESP_OK;

        default:
            ESP_LOGW(TAG, "Unknown Tuya cmd 0x%02X from 0x%04X, len=%zu", cmd_id, short_addr, len);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_DEBUG);
            update_device_last_seen(short_addr);
            return ESP_OK;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to parse Tuya DP (cmd 0x%02X): %s", cmd_id, esp_err_to_name(ret));
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_INFO);
        return ret;
    }

    /* Log every parsed DP with raw data and human-readable translation */
    tuya_log_dp(short_addr, &dp, data, len);

    /* Lazy-bind device to driver on first DP */
    try_lazy_bind(short_addr);

    /* Dispatch to driver */
    const tuya_device_driver_t *drv = tuya_driver_get(short_addr);
    if (drv != NULL && drv->process_dp != NULL) {
        ret = drv->process_dp(short_addr, &dp);
        if (ret == ESP_OK) {
            /* Publish updated state via driver */
            device_state_publish_tuya(short_addr);
        }
    } else {
        ESP_LOGD(TAG, "No driver for Tuya device 0x%04X, DP %d ignored",
                 short_addr, dp.dp_id);
    }

    /* Update last seen */
    update_device_last_seen(short_addr);

    return ESP_OK;
}

/* ============================================================================
 * DP Command Sending
 * ============================================================================ */

/**
 * @brief Build DP payload for sending
 */
static esp_err_t build_dp_payload(const tuya_dp_t *dp, uint8_t *buffer,
                                   size_t buffer_len, size_t *payload_len)
{
    if (dp == NULL || buffer == NULL || payload_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Calculate required size */
    uint16_t data_len;
    switch (dp->type) {
        case TUYA_DP_TYPE_BOOL:
            data_len = 1;
            break;
        case TUYA_DP_TYPE_VALUE:
            data_len = 4;
            break;
        case TUYA_DP_TYPE_ENUM:
            data_len = 1;
            break;
        case TUYA_DP_TYPE_BITMAP:
            data_len = 4;  /* Assume 4 bytes */
            break;
        default:
            data_len = dp->length;
            break;
    }

    size_t total_len = ZB_TUYA_DP_HEADER_SIZE_NO_STATUS + data_len;
    if (buffer_len < total_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Build header — no status byte for outgoing cmd 0x04 (sendData).
     * Format: [seq:2][dp_id:1][type:1][len:2][data:N] */
    uint16_t seq = s_sequence_number++;
    buffer[0] = (seq >> 8) & 0xFF;       /* Sequence high byte */
    buffer[1] = seq & 0xFF;              /* Sequence low byte */
    buffer[2] = dp->dp_id;
    buffer[3] = (uint8_t)dp->type;
    buffer[4] = (data_len >> 8) & 0xFF;  /* Length high byte */
    buffer[5] = data_len & 0xFF;         /* Length low byte */

    /* Build payload */
    uint8_t *payload = &buffer[6];
    switch (dp->type) {
        case TUYA_DP_TYPE_BOOL:
            payload[0] = dp->value.bool_value ? 1 : 0;
            break;

        case TUYA_DP_TYPE_VALUE:
            /* 32-bit big-endian */
            payload[0] = (dp->value.int_value >> 24) & 0xFF;
            payload[1] = (dp->value.int_value >> 16) & 0xFF;
            payload[2] = (dp->value.int_value >> 8) & 0xFF;
            payload[3] = dp->value.int_value & 0xFF;
            break;

        case TUYA_DP_TYPE_ENUM:
            payload[0] = dp->value.enum_value;
            break;

        case TUYA_DP_TYPE_BITMAP:
            /* 32-bit big-endian */
            payload[0] = (dp->value.bitmap_value >> 24) & 0xFF;
            payload[1] = (dp->value.bitmap_value >> 16) & 0xFF;
            payload[2] = (dp->value.bitmap_value >> 8) & 0xFF;
            payload[3] = dp->value.bitmap_value & 0xFF;
            break;

        default:
            memcpy(payload, dp->value.raw, data_len);
            break;
    }

    *payload_len = total_len;
    return ESP_OK;
}

esp_err_t zb_tuya_send_dp_with_cmd(uint16_t short_addr, uint8_t endpoint,
                                    const tuya_dp_t *dp, uint8_t tuya_cmd_id)
{
    if (!s_initialized || dp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Build payload */
    uint8_t payload[ZB_TUYA_DP_HEADER_SIZE_NO_STATUS + ZB_TUYA_DP_MAX_RAW_SIZE];
    size_t payload_len;

    esp_err_t ret = build_dp_payload(dp, payload, sizeof(payload), &payload_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build DP payload: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Sending Tuya DP to 0x%04X EP%d: dp_id=%d type=%d cmd=0x%02X",
             short_addr, endpoint, dp->dp_id, dp->type, tuya_cmd_id);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, payload_len, ESP_LOG_DEBUG);

    /* Send via custom cluster command */
    esp_zb_zcl_custom_cluster_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = ZB_TUYA_CLUSTER_ID,
        .custom_cmd_id = tuya_cmd_id,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_SET,  /* Use SET type for raw data payload */
            .size = payload_len,
            .value = payload,
        },
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd);
    esp_zb_lock_release();

    ESP_LOGI(TAG, "Tuya DP to 0x%04X EP%d queued (dp_id=%d cmd=0x%02X TSN=%d)",
             short_addr, endpoint, dp->dp_id, tuya_cmd_id, tsn);
    ret = ESP_OK;

    zb_coordinator_update_tx_count();
    return ret;
}

esp_err_t zb_tuya_send_dp(uint16_t short_addr, uint8_t endpoint, const tuya_dp_t *dp)
{
    return zb_tuya_send_dp_with_cmd(short_addr, endpoint, dp, ZB_TUYA_CMD_SEND_DATA);
}

/* ============================================================================
 * Device Type Checking
 * ============================================================================ */

bool zb_device_is_tuya(uint16_t short_addr)
{
    /* Check cluster list via device registry */
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return false;
    }
    return device_zigbee_has_cluster(dev, ZB_TUYA_CLUSTER_ID);
}

bool zb_tuya_is_fingerbot(uint16_t short_addr)
{
    /* Use driver registry for fingerbot detection */
    const tuya_device_driver_t *drv = tuya_driver_get(short_addr);
    if (drv != NULL && strcmp(drv->name, "fingerbot") == 0) {
        return true;
    }

    /* Fallback: try match if not yet bound */
    device_t *device = device_registry_get_by_short_addr(short_addr);
    if (device == NULL || !zb_device_is_tuya(short_addr)) {
        return false;
    }

    drv = tuya_driver_find(device->manufacturer, device->model);
    return (drv != NULL && strcmp(drv->name, "fingerbot") == 0);
}

/* ============================================================================
 * Backward-compatible State Access
 * ============================================================================ */

const tuya_fingerbot_state_t* zb_tuya_get_fingerbot_state(uint16_t short_addr)
{
    /* Delegate to the fingerbot driver's state accessor */
    extern const tuya_fingerbot_state_t *tuya_fingerbot_get_state(uint16_t short_addr);
    return tuya_fingerbot_get_state(short_addr);
}

/* ============================================================================
 * Fingerbot Command Helpers (public API, delegates to zb_tuya_send_dp)
 * ============================================================================ */

esp_err_t zb_tuya_fingerbot_set_switch(uint16_t short_addr, uint8_t endpoint, bool state)
{
    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_SWITCH,
        .type = TUYA_DP_TYPE_BOOL,
        .length = 1,
        .value.bool_value = state
    };
    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_fingerbot_set_mode(uint16_t short_addr, uint8_t endpoint,
                                      fingerbot_mode_t mode)
{
    if (mode > FINGERBOT_MODE_PROGRAM) {
        return ESP_ERR_INVALID_ARG;
    }

    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_MODE,
        .type = TUYA_DP_TYPE_ENUM,
        .length = 1,
        .value.enum_value = (uint8_t)mode
    };
    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_fingerbot_set_down_movement(uint16_t short_addr, uint8_t endpoint,
                                               uint8_t percent)
{
    if (percent < 51) {
        percent = 51;
    } else if (percent > 100) {
        percent = 100;
    }

    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_DOWN_MOVEMENT,
        .type = TUYA_DP_TYPE_VALUE,
        .length = 4,
        .value.int_value = percent
    };
    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_fingerbot_set_sustain_time(uint16_t short_addr, uint8_t endpoint,
                                              uint8_t seconds)
{
    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_SUSTAIN_TIME,
        .type = TUYA_DP_TYPE_VALUE,
        .length = 4,
        .value.int_value = seconds
    };
    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_fingerbot_set_reverse(uint16_t short_addr, uint8_t endpoint,
                                         bool reverse)
{
    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_REVERSE,
        .type = TUYA_DP_TYPE_ENUM,
        .length = 1,
        .value.enum_value = reverse ? 1 : 0
    };
    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_fingerbot_set_up_movement(uint16_t short_addr, uint8_t endpoint,
                                             uint8_t percent)
{
    if (percent > 50) {
        percent = 50;
    }

    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_UP_MOVEMENT,
        .type = TUYA_DP_TYPE_VALUE,
        .length = 4,
        .value.int_value = percent
    };
    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_fingerbot_set_touch_control(uint16_t short_addr, uint8_t endpoint,
                                               bool enable)
{
    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_TOUCH_CONTROL,
        .type = TUYA_DP_TYPE_BOOL,
        .length = 1,
        .value.bool_value = enable
    };
    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_fingerbot_set_click_control(uint16_t short_addr, uint8_t endpoint,
                                               bool enable)
{
    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_CLICK_CONTROL,
        .type = TUYA_DP_TYPE_BOOL,
        .length = 1,
        .value.bool_value = enable
    };
    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_fingerbot_set_program_enable(uint16_t short_addr, uint8_t endpoint,
                                                bool enable)
{
    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_PROGRAM_ENABLE,
        .type = TUYA_DP_TYPE_BOOL,
        .length = 1,
        .value.bool_value = enable
    };
    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_fingerbot_set_repeat_forever(uint16_t short_addr, uint8_t endpoint,
                                                bool repeat)
{
    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_REPEAT_FOREVER,
        .type = TUYA_DP_TYPE_BOOL,
        .length = 1,
        .value.bool_value = repeat
    };
    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_query_dp(uint16_t short_addr, uint8_t endpoint)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Tuya DP Query: ZCL cmd 0x03 (dataQuery) with payload [seq_hi, seq_lo] only.
     * Device may respond with cmd 0x01/0x02/0x05 containing current DP values.
     * Note: _TZ3210 Fingerbot does NOT respond to this with DP data. */
    uint16_t seq = s_sequence_number++;
    uint8_t payload[2] = {
        (seq >> 8) & 0xFF,
        seq & 0xFF,
    };

    ESP_LOGI(TAG, "Sending Tuya DP query to 0x%04X EP%d (seq=%u)",
             short_addr, endpoint, seq);

    esp_zb_zcl_custom_cluster_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = 1,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = ZB_TUYA_CLUSTER_ID,
        .custom_cmd_id = ZB_TUYA_CMD_DATA_QUERY,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
            .size = sizeof(payload),
            .value = payload,
        },
    };

    esp_zb_lock_acquire(GW_TIMEOUT_VERY_LONG_TICKS);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd);
    esp_zb_lock_release();

    ESP_LOGI(TAG, "Tuya DP query to 0x%04X queued (TSN=%d)", short_addr, tsn);

    zb_coordinator_update_tx_count();
    return ESP_OK;
}

esp_err_t zb_tuya_refresh_datapoints(uint16_t short_addr, uint8_t endpoint)
{
    /* Ask nicely first. Plenty of Tuya devices answer cmd 0x03 with their whole
     * datapoint set, and it costs one frame. */
    zb_tuya_query_dp(short_addr, endpoint);

    /* The Fingerbot Plus does not. Hardware testing (see the notes at the top of
     * zb_tuya.h) found it answers dataQuery with a Default Response and no
     * datapoints, and dumps its values only after pairing or a mode change.
     *
     * So rewrite the mode the device itself last reported, taken from the
     * persisted state. Writing a guessed mode would silently reconfigure
     * someone's device to obtain a reading, which is not a trade worth making.
     *
     * Measured limit, 2026-08-06: rewriting the mode the device already holds
     * makes it answer with that one datapoint and nothing else — only a real
     * change produces the full dump. This therefore refreshes the mode
     * reliably and the rest only when the device feels like it. Getting more
     * would mean changing the user's mode and changing it back, and a failed
     * write-back leaves their Fingerbot in the wrong mode. Not worth it for a
     * battery reading. */
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *state = device_registry_state_dup(dev->id);
    if (state == NULL) {
        ESP_LOGW(TAG, "0x%04X: no known mode to rewrite — datapoint query sent, "
                 "but this device only dumps after a mode change", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *mode_item = cJSON_GetObjectItem(state, "mode");
    fingerbot_mode_t mode;
    esp_err_t ret = ESP_ERR_NOT_FOUND;

    if (cJSON_IsString(mode_item) &&
        zb_tuya_fingerbot_mode_from_string(mode_item->valuestring, &mode) == ESP_OK) {
        ESP_LOGI(TAG, "0x%04X: rewriting last reported mode '%s' — refreshes the "
                 "mode, and whatever else the device chooses to send",
                 short_addr, mode_item->valuestring);
        ret = zb_tuya_fingerbot_set_mode(short_addr, endpoint, mode);
    } else {
        ESP_LOGW(TAG, "0x%04X: no mode in the stored state — datapoint query "
                 "sent, nothing else is safe to do", short_addr);
    }

    cJSON_Delete(state);
    return ret;
}

esp_err_t zb_tuya_fingerbot_send_program(uint16_t short_addr, uint8_t endpoint,
                                          const char *program_str)
{
    if (program_str == NULL || strlen(program_str) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t str_len = strlen(program_str);
    if (str_len > ZB_TUYA_DP_MAX_RAW_SIZE) {
        ESP_LOGE(TAG, "Program string too long: %zu (max %d)", str_len, ZB_TUYA_DP_MAX_RAW_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_PROGRAM,
        .type = TUYA_DP_TYPE_RAW,
        .length = (uint16_t)str_len,
    };
    memcpy(dp.value.raw, program_str, str_len);

    ESP_LOGI(TAG, "Sending program to 0x%04X: \"%s\" (%zu bytes)",
             short_addr, program_str, str_len);

    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

esp_err_t zb_tuya_fingerbot_send_program_raw(uint16_t short_addr, uint8_t endpoint,
                                              const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > ZB_TUYA_DP_MAX_RAW_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    tuya_dp_t dp = {
        .dp_id = TUYA_DP_FINGERBOT_PROGRAM,
        .type = TUYA_DP_TYPE_RAW,
        .length = (uint16_t)len,
    };
    memcpy(dp.value.raw, data, len);

    ESP_LOGI(TAG, "Sending program raw to 0x%04X (%zu bytes)", short_addr, len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_INFO);

    return zb_tuya_send_dp(short_addr, endpoint, &dp);
}

const char* zb_tuya_fingerbot_mode_to_string(fingerbot_mode_t mode)
{
    switch (mode) {
        case FINGERBOT_MODE_PUSH:    return "push";
        case FINGERBOT_MODE_SWITCH:  return "switch";
        case FINGERBOT_MODE_PROGRAM: return "program";
        default:                     return "unknown";
    }
}

esp_err_t zb_tuya_fingerbot_mode_from_string(const char *str, fingerbot_mode_t *mode)
{
    if (str == NULL || mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcasecmp(str, "push") == 0) {
        *mode = FINGERBOT_MODE_PUSH;
        return ESP_OK;
    } else if (strcasecmp(str, "switch") == 0) {
        *mode = FINGERBOT_MODE_SWITCH;
        return ESP_OK;
    } else if (strcasecmp(str, "program") == 0) {
        *mode = FINGERBOT_MODE_PROGRAM;
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

/* ============================================================================
 * Generic Tuya State Publishing
 * ============================================================================ */

esp_err_t device_state_publish_tuya(uint16_t short_addr)
{
    const tuya_device_driver_t *drv = tuya_driver_get(short_addr);
    if (drv == NULL || drv->build_state_json == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *json = drv->build_state_json(short_addr);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Look up device in NG registry */
    device_t *dev = device_registry_get_by_short_addr(short_addr);
    if (dev == NULL) {
        cJSON_Delete(json);
        return ESP_ERR_NOT_FOUND;
    }

    /* Add common fields */
    cJSON_AddNumberToObject(json, "linkquality", dev->proto.zigbee.lqi);

    /*
     * Merge state into device_registry BEFORE publishing the event.
     * The async event handler reads from device_registry_get_state()
     * instead of the event's json_state pointer (which would be a
     * use-after-free since the event bus copies the struct but not
     * the pointed-to string).
     */
    esp_err_t merge_ret = device_registry_merge_state(dev->id, json);
    cJSON_Delete(json);
    if (merge_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to merge Tuya state for 0x%04X: %s",
                 short_addr, esp_err_to_name(merge_ret));
        return merge_ret;
    }

    /* Publish via EVT_DEVICE_STATE_CHANGED - same path as converter.
     * Handler uses device_registry_get_state(), not json_state pointer. */
    evt_device_state_t evt = {
        .ieee_addr = dev->id,
        .short_addr = short_addr,
        .endpoint = 1,
        .cluster_id = 0xEF00,   /* Tuya cluster */
        .attr_id = 0,
        .json_state = NULL,     /* Not used - handler reads from registry */
    };

    return event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));
}
