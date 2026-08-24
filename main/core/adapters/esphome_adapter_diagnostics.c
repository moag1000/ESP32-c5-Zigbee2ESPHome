/**
 * @file esphome_adapter_diagnostics.c
 * @brief Per-device diagnostic entities for ESPHome Native API
 *
 * Registers diagnostic entities per Zigbee device (on device's sub-device ID):
 *   - Remove (Button)        — removes device from network
 *   - Reconfigure (Button)   — re-interviews the device
 *   - Identify (Button)      — triggers ZCL Identify effect
 *   - LQI (Sensor)           — Link Quality Indicator (0-255)
 *   - RSSI (Sensor)          — Signal Strength (dBm)
 *   - Last Seen (TextSensor) — human-readable timestamp
 *   - Zigbee Info (TextSensor)— endpoint/cluster summary (disabled_by_default)
 *   - Refresh Values (Button) — Tuya devices only, asks for a datapoint dump
 *
 * Key scheme: 0x5XXXXXXX
 *   Bits 31-28: 0x5 (diagnostic prefix)
 *   Bits 27-23: diagnostic type index (0-7)
 *   Bits 22-0:  lower 23 bits of device ID hash
 */

#include "esphome_adapter_diagnostics.h"
#include "esphome/esphome_entities.h"
#include "esphome/esphome_entities_types.h"
#include "core/device/device_registry.h"
#include "zigbee/zb_interview.h"
#include "zigbee/zb_leave_helper.h"
#include "zigbee/zb_tuya.h"
#include "zigbee/converter/zb_converter_std.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "diag_ent";

/* ============================================================================
 * Diagnostic Type Indices
 * ============================================================================ */

#define DIAG_TYPE_REMOVE        0
#define DIAG_TYPE_RECONFIGURE   1
#define DIAG_TYPE_IDENTIFY      2
#define DIAG_TYPE_LQI           3
#define DIAG_TYPE_RSSI          4
#define DIAG_TYPE_LAST_SEEN     5
#define DIAG_TYPE_ZIGBEE_INFO   6
#define DIAG_TYPE_REFRESH_DP    7
#define DIAG_TYPE_COUNT         8

/* ============================================================================
 * Key Generation
 * ============================================================================ */

static uint32_t make_diag_key(device_id_t id, uint8_t diag_type)
{
    uint32_t id_hash = (uint32_t)(id ^ (id >> 32));
    uint32_t key = ESPHOME_ADAPTER_DIAG_KEY_OFFSET;
    key |= ((uint32_t)diag_type & 0x1F) << 23;
    key |= (id_hash & 0x007FFFFF);
    return key;
}

/* ============================================================================
 * Device Lookup from Diagnostic Key
 * ============================================================================ */

typedef struct {
    esphome_entity_key_t target_key;
    uint8_t diag_type;
    device_t *result;
} find_diag_device_ctx_t;

static bool find_diag_device_cb(device_t *dev, void *ctx)
{
    find_diag_device_ctx_t *fc = (find_diag_device_ctx_t *)ctx;
    uint32_t candidate = make_diag_key(dev->id, fc->diag_type);
    if (candidate == fc->target_key) {
        fc->result = dev;
        return false;
    }
    return true;
}

static device_t *find_device_for_diag_key(esphome_entity_key_t key, uint8_t diag_type)
{
    find_diag_device_ctx_t ctx = {
        .target_key = key,
        .diag_type = diag_type,
        .result = NULL,
    };
    device_registry_iterate_zigbee(find_diag_device_cb, &ctx);
    return ctx.result;
}

/* ============================================================================
 * Button Command Callbacks
 * ============================================================================ */

static esp_err_t diag_remove_press(esphome_entity_key_t key)
{
    device_t *dev = find_device_for_diag_key(key, DIAG_TYPE_REMOVE);
    if (!dev) {
        ESP_LOGW(TAG, "Remove: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Remove device 0x%016llX (0x%04X)",
             (unsigned long long)dev->id, dev->proto.zigbee.short_addr);

    zb_device_leave_cleanup(dev->id, dev->proto.zigbee.short_addr);
    return ESP_OK;
}

typedef struct {
    device_id_t ieee;
    uint16_t short_addr;
} reconfigure_req_t;

/**
 * @brief Run a re-interview away from the caller's task
 *
 * The press callback runs on the ESPHome API server task — the one holding the
 * Home Assistant connection. zb_interview_start() used to be called there
 * directly and blocked it long enough that Home Assistant dropped the
 * connection on every press.
 */
static void reconfigure_task(void *arg)
{
    reconfigure_req_t *req = (reconfigure_req_t *)arg;

    esp_err_t ret = zb_interview_start(req->ieee, req->short_addr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Re-interview of 0x%04X failed to start: %s",
                 req->short_addr, esp_err_to_name(ret));
    }

    free(req);
    vTaskDelete(NULL);
}

static esp_err_t diag_reconfigure_press(esphome_entity_key_t key)
{
    device_t *dev = find_device_for_diag_key(key, DIAG_TYPE_RECONFIGURE);
    if (!dev) {
        ESP_LOGW(TAG, "Reconfigure: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Re-interview device 0x%016llX (0x%04X)",
             (unsigned long long)dev->id, dev->proto.zigbee.short_addr);

    reconfigure_req_t *req = malloc(sizeof(*req));
    if (!req) {
        return ESP_ERR_NO_MEM;
    }
    req->ieee = dev->id;
    req->short_addr = dev->proto.zigbee.short_addr;

    if (xTaskCreate(reconfigure_task, "zb_reconf", 4096, req, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start re-interview task");
        free(req);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t diag_identify_press(esphome_entity_key_t key)
{
    device_t *dev = find_device_for_diag_key(key, DIAG_TYPE_IDENTIFY);
    if (!dev) {
        ESP_LOGW(TAG, "Identify: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Identify device 0x%016llX (0x%04X)",
             (unsigned long long)dev->id, dev->proto.zigbee.short_addr);

    /* Send 5-second identify */
    cJSON *val = cJSON_CreateNumber(5);
    if (!val) return ESP_ERR_NO_MEM;

    esp_err_t ret = tz_identify(dev->proto.zigbee.short_addr,
                                dev->proto.zigbee.endpoint, val);
    cJSON_Delete(val);
    return ret;
}

static esp_err_t diag_refresh_dp_press(esphome_entity_key_t key)
{
    device_t *dev = find_device_for_diag_key(key, DIAG_TYPE_REFRESH_DP);
    if (!dev) {
        ESP_LOGW(TAG, "Refresh: device not found for key 0x%08lX", (unsigned long)key);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Refreshing Tuya datapoints of 0x%04X", dev->proto.zigbee.short_addr);

    esp_err_t ret = zb_tuya_refresh_datapoints(dev->proto.zigbee.short_addr,
                                               dev->proto.zigbee.endpoint);
    if (ret == ESP_ERR_NOT_FOUND) {
        /* The query went out; there was just no safe way to force a dump. Not
         * an error the user needs to see as a failure. */
        return ESP_OK;
    }
    return ret;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

esp_err_t esphome_adapter_diagnostics_register(const device_t *dev)
{
    if (!dev || !dev->in_use || dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);
    /* Only for the log line below. Entity names carry no device prefix: Home
     * Assistant prepends the device name itself, so a prefix rendered it twice
     * — the four buttons above were fixed for that and these four sensors were
     * not. Worse, the prefix was dev->friendly_name, which embeds the short
     * address, so after a rejoin the device read "TS0601 0x3C92" while its own
     * entities still read "TS0601 0xA0A3 LQI". */
    const char *name = dev->friendly_name[0] ? dev->friendly_name : "Device";

    ESP_LOGD(TAG, "Registering diagnostics for %s (0x%04X)",
             name, dev->proto.zigbee.short_addr);

    /* --- Remove Button (CONFIG category) --- */
    {
        esphome_button_config_t cfg = {0};
        cfg.key = make_diag_key(dev->id, DIAG_TYPE_REMOVE);
        cfg.device_id = device_id;
        /* No device prefix: HA prepends the device name itself, so "%s Remove"
         * rendered as the device name twice. */
        strlcpy(cfg.name, "Remove", sizeof(cfg.name));
        snprintf(cfg.unique_id, sizeof(cfg.unique_id),
                 "0x%016llX_remove", (unsigned long long)dev->id);
        strncpy(cfg.icon, "mdi:delete", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BUTTON_CLASS_NONE;
        cfg.entity_category = 1;  /* CONFIG */
        cfg.disabled_by_default = false;
        cfg.press_callback = diag_remove_press;
        esphome_entity_register_button(&cfg);
    }

    /* --- Reconfigure Button (CONFIG category) --- */
    {
        esphome_button_config_t cfg = {0};
        cfg.key = make_diag_key(dev->id, DIAG_TYPE_RECONFIGURE);
        cfg.device_id = device_id;
        strlcpy(cfg.name, "Reconfigure", sizeof(cfg.name));
        snprintf(cfg.unique_id, sizeof(cfg.unique_id),
                 "0x%016llX_reconfigure", (unsigned long long)dev->id);
        strncpy(cfg.icon, "mdi:cog-refresh", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BUTTON_CLASS_NONE;
        cfg.entity_category = 1;  /* CONFIG */
        cfg.disabled_by_default = false;
        cfg.press_callback = diag_reconfigure_press;
        esphome_entity_register_button(&cfg);
    }

    /* --- Refresh Values Button (CONFIG category, Tuya devices only) ---
     *
     * Tuya devices carry their data in datapoints rather than ZCL attributes,
     * and some only send them when something changes. The Fingerbot Plus is the
     * extreme case: after a reboot its battery, sustain time and movement
     * limits read as unknown and never fill in on their own. This asks for
     * them. Only registered where there is a Tuya cluster to ask. */
    if (device_zigbee_has_cluster(dev, ZB_TUYA_CLUSTER_ID)) {
        esphome_button_config_t cfg = {0};
        cfg.key = make_diag_key(dev->id, DIAG_TYPE_REFRESH_DP);
        cfg.device_id = device_id;
        strlcpy(cfg.name, "Refresh Values", sizeof(cfg.name));
        snprintf(cfg.unique_id, sizeof(cfg.unique_id),
                 "0x%016llX_refresh_dp", (unsigned long long)dev->id);
        strncpy(cfg.icon, "mdi:database-refresh", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BUTTON_CLASS_NONE;
        cfg.entity_category = 1;  /* CONFIG */
        cfg.disabled_by_default = false;
        cfg.press_callback = diag_refresh_dp_press;
        esphome_entity_register_button(&cfg);
    }

    /* --- Identify Button (CONFIG category) --- */
    {
        esphome_button_config_t cfg = {0};
        cfg.key = make_diag_key(dev->id, DIAG_TYPE_IDENTIFY);
        cfg.device_id = device_id;
        strlcpy(cfg.name, "Identify", sizeof(cfg.name));
        snprintf(cfg.unique_id, sizeof(cfg.unique_id),
                 "0x%016llX_identify", (unsigned long long)dev->id);
        strncpy(cfg.icon, "mdi:flash-alert", sizeof(cfg.icon) - 1);
        cfg.device_class = ESPHOME_BUTTON_CLASS_IDENTIFY;
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        cfg.press_callback = diag_identify_press;
        esphome_entity_register_button(&cfg);
    }

    /* --- LQI Sensor (DIAGNOSTIC) --- */
    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = make_diag_key(dev->id, DIAG_TYPE_LQI);
        cfg.device_id = device_id;
        strlcpy(cfg.name, "LQI", sizeof(cfg.name));
        snprintf(cfg.unique_id, sizeof(cfg.unique_id),
                 "0x%016llX_lqi", (unsigned long long)dev->id);
        strncpy(cfg.icon, "mdi:signal", sizeof(cfg.icon) - 1);
        cfg.accuracy_decimals = 0;
        cfg.device_class = ESPHOME_SENSOR_CLASS_NONE;
        cfg.state_class = ESPHOME_STATE_CLASS_MEASUREMENT;
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }

    /* --- RSSI Sensor (DIAGNOSTIC) --- */
    {
        esphome_sensor_config_t cfg = {0};
        cfg.key = make_diag_key(dev->id, DIAG_TYPE_RSSI);
        cfg.device_id = device_id;
        strlcpy(cfg.name, "RSSI", sizeof(cfg.name));
        snprintf(cfg.unique_id, sizeof(cfg.unique_id),
                 "0x%016llX_rssi", (unsigned long long)dev->id);
        strncpy(cfg.icon, "mdi:signal-variant", sizeof(cfg.icon) - 1);
        strncpy(cfg.unit_of_measurement, "dBm", sizeof(cfg.unit_of_measurement) - 1);
        cfg.accuracy_decimals = 0;
        cfg.device_class = ESPHOME_SENSOR_CLASS_SIGNAL_STRENGTH;
        cfg.state_class = ESPHOME_STATE_CLASS_MEASUREMENT;
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        esphome_entity_register_sensor(&cfg);
    }

    /* --- Last Seen TextSensor (DIAGNOSTIC) --- */
    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = make_diag_key(dev->id, DIAG_TYPE_LAST_SEEN);
        cfg.device_id = device_id;
        strlcpy(cfg.name, "Last Seen", sizeof(cfg.name));
        snprintf(cfg.unique_id, sizeof(cfg.unique_id),
                 "0x%016llX_last_seen", (unsigned long long)dev->id);
        strncpy(cfg.icon, "mdi:clock-outline", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = false;
        esphome_entity_register_text_sensor(&cfg);
    }

    /* --- Zigbee Info TextSensor (DIAGNOSTIC, disabled by default) --- */
    {
        esphome_text_sensor_config_t cfg = {0};
        cfg.key = make_diag_key(dev->id, DIAG_TYPE_ZIGBEE_INFO);
        cfg.device_id = device_id;
        strlcpy(cfg.name, "Zigbee Info", sizeof(cfg.name));
        snprintf(cfg.unique_id, sizeof(cfg.unique_id),
                 "0x%016llX_zigbee_info", (unsigned long long)dev->id);
        strncpy(cfg.icon, "mdi:information-outline", sizeof(cfg.icon) - 1);
        cfg.entity_category = 2;  /* DIAGNOSTIC */
        cfg.disabled_by_default = true;
        esphome_entity_register_text_sensor(&cfg);
    }

    /* Push initial state values */
    esphome_adapter_diagnostics_update(dev);

    return ESP_OK;
}

/* ============================================================================
 * State Updates
 * ============================================================================ */

void esphome_adapter_diagnostics_update(const device_t *dev)
{
    if (!dev || dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        return;
    }

    /* LQI. Zero is the "never measured" state, not a reading — the field is
     * zeroed when a device is loaded from NVS and only the NWK neighbour table
     * fills it. A real link quality of 0 would mean an unusable link, which is
     * a different claim from "we have not heard this device yet". */
    esphome_entity_update_sensor(
        make_diag_key(dev->id, DIAG_TYPE_LQI),
        dev->proto.zigbee.lqi != 0 ? (float)dev->proto.zigbee.lqi : NAN);

    /* RSSI.
     *
     * 0 dBm cannot come off a received Zigbee frame, so it is the "nothing
     * measured yet" value rather than a reading — the field is zeroed on load
     * and only the NWK neighbour table ever fills it. Publishing NaN makes Home
     * Assistant show 'unknown', which is what it is. It used to publish a
     * number derived from LQI, so a device that had never been heard from
     * reported a confident -100 dBm. */
    esphome_entity_update_sensor(
        make_diag_key(dev->id, DIAG_TYPE_RSSI),
        dev->proto.zigbee.rssi != 0 ? (float)dev->proto.zigbee.rssi : NAN);

    /* Last Seen */
    if (dev->last_seen > 0) {
        time_t ts = (time_t)dev->last_seen;
        struct tm timeinfo;
        localtime_r(&ts, &timeinfo);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
        esphome_entity_update_text_sensor(
            make_diag_key(dev->id, DIAG_TYPE_LAST_SEEN), time_str);
    }

    /* Zigbee Info (compact summary) */
    {
        char info[128];
        int n = snprintf(info, sizeof(info),
                         "EP:%u Addr:0x%04X Type:%u Clusters:%u",
                         dev->proto.zigbee.endpoint,
                         dev->proto.zigbee.short_addr,
                         (unsigned)dev->proto.zigbee.device_type,
                         dev->proto.zigbee.cluster_count);

        /* Append power type if known */
        if (dev->proto.zigbee.avail_meta.power_type != 0) {
            const char *pwr = (dev->proto.zigbee.avail_meta.power_type == 1)
                              ? "mains" : "battery";
            snprintf(info + n, sizeof(info) - n, " Pwr:%s", pwr);
        }

        esphome_entity_update_text_sensor(
            make_diag_key(dev->id, DIAG_TYPE_ZIGBEE_INFO), info);
    }
}

/* ============================================================================
 * Deinit (stateless — no cleanup needed)
 * ============================================================================ */

void esphome_adapter_diagnostics_deinit(void)
{
    /* No event subscriptions or state to clean up.
     * Entity lifetime is managed by the entity core. */
}
