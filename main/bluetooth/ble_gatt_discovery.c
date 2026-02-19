/**
 * @file ble_gatt_discovery.c
 * @brief Generic GATT Service Discovery Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "ble_gatt_discovery.h"
#include "ble_common.h"
#include "ble_constants.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/gateway_timeouts.h"
#include "freertos/task.h"
#include "core/memory/memory_manager_ng.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "BLE_DISCOVERY";

/* ============================================================================
 * Standard UUID Name Mappings
 * ============================================================================ */

typedef struct {
    uint16_t uuid;
    const char *name;
} uuid_name_entry_t;

/** @brief Standard GATT Service UUIDs */
static const uuid_name_entry_t s_service_names[] = {
    {0x1800, "Generic Access"},
    {0x1801, "Generic Attribute"},
    {0x1802, "Immediate Alert"},
    {0x1803, "Link Loss"},
    {0x1804, "TX Power"},
    {0x1805, "Current Time"},
    {0x1806, "Reference Time Update"},
    {0x1807, "Next DST Change"},
    {0x1808, "Glucose"},
    {0x1809, "Health Thermometer"},
    {0x180A, "Device Information"},
    {0x180D, "Heart Rate"},
    {0x180E, "Phone Alert Status"},
    {0x180F, "Battery"},
    {0x1810, "Blood Pressure"},
    {0x1811, "Alert Notification"},
    {0x1812, "Human Interface Device"},
    {0x1813, "Scan Parameters"},
    {0x1814, "Running Speed and Cadence"},
    {0x1815, "Automation IO"},
    {0x1816, "Cycling Speed and Cadence"},
    {0x1818, "Cycling Power"},
    {0x1819, "Location and Navigation"},
    {0x181A, "Environmental Sensing"},
    {0x181B, "Body Composition"},
    {0x181C, "User Data"},
    {0x181D, "Weight Scale"},
    {0x181E, "Bond Management"},
    {0x181F, "Continuous Glucose Monitoring"},
    {0x1820, "Internet Protocol Support"},
    {0x1821, "Indoor Positioning"},
    {0x1822, "Pulse Oximeter"},
    {0x1823, "HTTP Proxy"},
    {0x1824, "Transport Discovery"},
    {0x1825, "Object Transfer"},
    {0x1826, "Fitness Machine"},
    {0x1827, "Mesh Provisioning"},
    {0x1828, "Mesh Proxy"},
    {0x1829, "Reconnection Configuration"},
    {0xFE95, "Xiaomi Mi"},
    {0xFD3D, "SwitchBot"},
    {0xFEAA, "Eddystone"},
    {0xFFF0, "Inkbird"},
    {0, NULL}
};

/** @brief Standard GATT Characteristic UUIDs */
static const uuid_name_entry_t s_char_names[] = {
    {0x2A00, "Device Name"},
    {0x2A01, "Appearance"},
    {0x2A02, "Peripheral Privacy Flag"},
    {0x2A03, "Reconnection Address"},
    {0x2A04, "Peripheral Preferred Connection Parameters"},
    {0x2A05, "Service Changed"},
    {0x2A06, "Alert Level"},
    {0x2A07, "TX Power Level"},
    {0x2A19, "Battery Level"},
    {0x2A23, "System ID"},
    {0x2A24, "Model Number String"},
    {0x2A25, "Serial Number String"},
    {0x2A26, "Firmware Revision String"},
    {0x2A27, "Hardware Revision String"},
    {0x2A28, "Software Revision String"},
    {0x2A29, "Manufacturer Name String"},
    {0x2A2A, "IEEE Regulatory Certification"},
    {0x2A2B, "Current Time"},
    {0x2A37, "Heart Rate Measurement"},
    {0x2A38, "Body Sensor Location"},
    {0x2A39, "Heart Rate Control Point"},
    {0x2A4D, "Report"},
    {0x2A4E, "Protocol Mode"},
    {0x2A50, "PnP ID"},
    {0x2A6C, "Elevation"},
    {0x2A6D, "Pressure"},
    {0x2A6E, "Temperature"},
    {0x2A6F, "Humidity"},
    {0x2A70, "True Wind Speed"},
    {0x2A71, "True Wind Direction"},
    {0x2A72, "Apparent Wind Speed"},
    {0x2A73, "Apparent Wind Direction"},
    {0x2A74, "Gust Factor"},
    {0x2A75, "Pollen Concentration"},
    {0x2A76, "UV Index"},
    {0x2A77, "Irradiance"},
    {0x2A78, "Rainfall"},
    {0x2A79, "Wind Chill"},
    {0x2A7A, "Heat Index"},
    {0x2A7B, "Dew Point"},
    {0, NULL}
};

/* ============================================================================
 * Private Types
 * ============================================================================ */

/**
 * @brief Pending discovery context
 */
typedef struct {
    uint8_t mac[6];
    uint16_t conn_handle;
    ble_discovery_complete_cb_t callback;
    ble_discovery_result_t result;
    bool active;
    bool complete;
} discovery_context_t;

/* ============================================================================
 * Static Variables
 * ============================================================================ */

/** @brief Module configuration */
static ble_discovery_config_t s_config = BLE_DISCOVERY_CONFIG_DEFAULT();

/** @brief Cached discovery results - allocated in PSRAM to save internal RAM */
static ble_discovery_result_t *s_cache = NULL;

/** @brief Current discovery context */
static discovery_context_t s_current_discovery;

/** @brief Global completion callback */
static ble_discovery_complete_cb_t s_global_callback = NULL;

/** @brief Module mutex */
static SemaphoreHandle_t s_mutex = NULL;

/** @brief Initialization flag */
static bool s_initialized = false;

/* ============================================================================
 * Private Function Declarations
 * ============================================================================ */

static const char *lookup_uuid16_name(uint16_t uuid, const uuid_name_entry_t *table);
static ble_discovery_result_t *find_cached(const uint8_t *mac);
static ble_discovery_result_t *find_free_cache_slot(void);
static esp_err_t perform_discovery(discovery_context_t *ctx);
static void copy_uuid_name(ble_gatt_uuid_t *uuid, char *name, size_t name_len, bool is_service);

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t ble_gatt_discovery_init(void)
{
    ble_discovery_config_t config = BLE_DISCOVERY_CONFIG_DEFAULT();
    return ble_gatt_discovery_init_with_config(&config);
}

esp_err_t ble_gatt_discovery_init_with_config(const ble_discovery_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate cache in PSRAM (saves ~1KB internal RAM) */
    if (s_cache == NULL) {
        s_cache = mem_ng_calloc(BLE_DISCOVERY_MAX_CACHED_DEVICES,
                                 sizeof(ble_discovery_result_t),
                                 MEM_CAP_PSRAM);
        if (!s_cache) {
            ESP_LOGW(TAG, "PSRAM alloc failed, falling back to internal RAM");
            s_cache = mem_ng_calloc(BLE_DISCOVERY_MAX_CACHED_DEVICES,
                                     sizeof(ble_discovery_result_t),
                                     MEM_CAP_DEFAULT);
        }
        if (!s_cache) {
            ESP_LOGE(TAG, "Failed to allocate cache");
            vSemaphoreDelete(s_mutex);
            s_mutex = NULL;
            return ESP_ERR_NO_MEM;
        }
    } else {
        memset(s_cache, 0, BLE_DISCOVERY_MAX_CACHED_DEVICES * sizeof(ble_discovery_result_t));
    }
    memset(&s_current_discovery, 0, sizeof(s_current_discovery));
    memcpy(&s_config, config, sizeof(ble_discovery_config_t));
    s_global_callback = NULL;

    s_initialized = true;

    ESP_LOGI(TAG, "Initialized (cache: %s, mqtt: %s, PSRAM)",
             s_config.cache_results ? "yes" : "no",
             s_config.publish_to_mqtt ? "yes" : "no");

    return ESP_OK;
}

esp_err_t ble_gatt_discovery_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    memset(s_cache, 0, BLE_DISCOVERY_MAX_CACHED_DEVICES * sizeof(ble_discovery_result_t));
    memset(&s_current_discovery, 0, sizeof(s_current_discovery));
    s_global_callback = NULL;
    xSemaphoreGive(s_mutex);

    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
    s_initialized = false;

    ESP_LOGI(TAG, "Deinitialized");
    return ESP_OK;
}

bool ble_gatt_discovery_is_initialized(void)
{
    return s_initialized;
}

esp_err_t ble_gatt_discovery_start(const uint8_t *mac, ble_discovery_complete_cb_t callback)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t conn_handle = ble_gatt_get_conn_handle(mac);
    if (conn_handle == BLE_GATT_INVALID_CONN_HANDLE) {
        ESP_LOGW(TAG, "Device not connected");
        return ESP_ERR_NOT_FOUND;
    }

    return ble_gatt_discovery_start_by_handle(conn_handle, callback);
}

esp_err_t ble_gatt_discovery_start_by_handle(uint16_t conn_handle,
                                              ble_discovery_complete_cb_t callback)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!ble_gatt_is_handle_connected(conn_handle)) {
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    /* Check if discovery already in progress */
    if (s_current_discovery.active) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Discovery already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    /* Initialize discovery context */
    memset(&s_current_discovery, 0, sizeof(discovery_context_t));
    s_current_discovery.conn_handle = conn_handle;
    s_current_discovery.callback = callback;
    s_current_discovery.active = true;

    /* Get MAC from connection */
    ble_gatt_connection_t conn_info;
    if (ble_gatt_get_connection_info(conn_handle, &conn_info) == ESP_OK) {
        memcpy(s_current_discovery.mac, conn_info.mac, 6);
        memcpy(s_current_discovery.result.mac, conn_info.mac, 6);
    }

    s_current_discovery.result.conn_handle = conn_handle;

    xSemaphoreGive(s_mutex);

    /* Perform discovery */
    esp_err_t ret = perform_discovery(&s_current_discovery);

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    s_current_discovery.complete = (ret == ESP_OK);
    s_current_discovery.result.complete = (ret == ESP_OK);
    s_current_discovery.result.discovery_time = (uint32_t)time(NULL);

    /* Cache result if configured */
    if (ret == ESP_OK && s_config.cache_results) {
        ble_discovery_result_t *cached = find_cached(s_current_discovery.mac);
        if (cached == NULL) {
            cached = find_free_cache_slot();
        }
        if (cached != NULL) {
            memcpy(cached, &s_current_discovery.result, sizeof(ble_discovery_result_t));
        }
    }

    /* Invoke callbacks */
    ble_discovery_complete_cb_t cb = s_current_discovery.callback;
    const ble_discovery_result_t *result = &s_current_discovery.result;
    uint8_t mac[6];
    memcpy(mac, s_current_discovery.mac, 6);

    s_current_discovery.active = false;

    xSemaphoreGive(s_mutex);

    /* Invoke per-discovery callback */
    if (cb != NULL) {
        cb(mac, ret == ESP_OK ? result : NULL, ret == ESP_OK);
    }

    /* Invoke global callback */
    if (s_global_callback != NULL) {
        s_global_callback(mac, ret == ESP_OK ? result : NULL, ret == ESP_OK);
    }

    /* Publish to MQTT if configured */
    if (ret == ESP_OK && s_config.publish_to_mqtt) {
        ble_gatt_discovery_publish_mqtt(result);
    }

    char mac_str[18];
    ble_mac_to_str(mac, mac_str);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Discovery complete: %s (%d services)",
                 mac_str, result->service_count);
    } else {
        ESP_LOGW(TAG, "Discovery failed: %s (%s)", mac_str, esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t ble_gatt_discovery_cancel(const uint8_t *mac)
{
    if (!s_initialized || mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    if (s_current_discovery.active && ble_mac_equal(s_current_discovery.mac, mac)) {
        s_current_discovery.active = false;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Discovery cancelled");
        return ESP_OK;
    }

    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

bool ble_gatt_discovery_in_progress(const uint8_t *mac)
{
    if (!s_initialized || mac == NULL) {
        return false;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    bool in_progress = s_current_discovery.active &&
                       ble_mac_equal(s_current_discovery.mac, mac);
    xSemaphoreGive(s_mutex);

    return in_progress;
}

esp_err_t ble_gatt_discovery_get_cached(const uint8_t *mac, ble_discovery_result_t *result)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mac == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    ble_discovery_result_t *cached = find_cached(mac);
    if (cached == NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(result, cached, sizeof(ble_discovery_result_t));
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

bool ble_gatt_discovery_has_cached(const uint8_t *mac)
{
    if (!s_initialized || mac == NULL) {
        return false;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    bool has_cached = (find_cached(mac) != NULL);
    xSemaphoreGive(s_mutex);

    return has_cached;
}

esp_err_t ble_gatt_discovery_clear_cached(const uint8_t *mac)
{
    if (!s_initialized || mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);

    ble_discovery_result_t *cached = find_cached(mac);
    if (cached != NULL) {
        memset(cached, 0, sizeof(ble_discovery_result_t));
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t ble_gatt_discovery_clear_all_cached(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
    memset(s_cache, 0, BLE_DISCOVERY_MAX_CACHED_DEVICES * sizeof(ble_discovery_result_t));
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Cleared all cached discoveries");
    return ESP_OK;
}

const char *ble_gatt_discovery_uuid16_name(uint16_t uuid16)
{
    /* Check services first */
    const char *name = lookup_uuid16_name(uuid16, s_service_names);
    if (name != NULL) {
        return name;
    }

    /* Then characteristics */
    name = lookup_uuid16_name(uuid16, s_char_names);
    if (name != NULL) {
        return name;
    }

    return "Unknown";
}

char *ble_gatt_discovery_uuid_to_str(const ble_gatt_uuid_t *uuid, char *buf, size_t buf_len)
{
    if (uuid == NULL || buf == NULL || buf_len < 5) {
        return NULL;
    }

    if (uuid->type == BLE_GATT_UUID_TYPE_16) {
        snprintf(buf, buf_len, "0x%04X", uuid->value.uuid16);
    } else {
        /* 128-bit UUID: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
        if (buf_len >= BLE_UUID_STRING_LEN) {
            const uint8_t *u = uuid->value.uuid128;
            snprintf(buf, buf_len,
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8],
                     u[7], u[6], u[5], u[4], u[3], u[2], u[1], u[0]);
        } else {
            snprintf(buf, buf_len, "128-bit");
        }
    }

    return buf;
}

char *ble_gatt_discovery_props_to_str(uint8_t properties, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 2) {
        return NULL;
    }

    buf[0] = '\0';
    size_t pos = 0;

    if (properties & BLE_GATT_CHR_PROP_BROADCAST) {
        pos += snprintf(buf + pos, buf_len - pos, "broadcast,");
    }
    if (properties & BLE_GATT_CHR_PROP_READ) {
        pos += snprintf(buf + pos, buf_len - pos, "read,");
    }
    if (properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) {
        pos += snprintf(buf + pos, buf_len - pos, "write-no-rsp,");
    }
    if (properties & BLE_GATT_CHR_PROP_WRITE) {
        pos += snprintf(buf + pos, buf_len - pos, "write,");
    }
    if (properties & BLE_GATT_CHR_PROP_NOTIFY) {
        pos += snprintf(buf + pos, buf_len - pos, "notify,");
    }
    if (properties & BLE_GATT_CHR_PROP_INDICATE) {
        pos += snprintf(buf + pos, buf_len - pos, "indicate,");
    }

    /* Remove trailing comma */
    if (pos > 0 && buf[pos - 1] == ',') {
        buf[pos - 1] = '\0';
    }

    return buf;
}

esp_err_t ble_gatt_discovery_publish_mqtt(const ble_discovery_result_t *result)
{
    if (!s_initialized || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* TODO: Implement MQTT publishing */
    /* Topic: zigbee2mqtt/ble/<mac>/services */

    char mac_str[18];
    ble_mac_to_str(result->mac, mac_str);
    ESP_LOGD(TAG, "MQTT publish: %s (%d services)", mac_str, result->service_count);

    return ESP_OK;
}

int ble_gatt_discovery_to_json(const ble_discovery_result_t *result,
                                char *json_buf, size_t buf_len)
{
    if (result == NULL || json_buf == NULL || buf_len < BLE_DISCOVERY_JSON_MIN_SIZE) {
        return -1;
    }

    char mac_str[18];
    ble_mac_to_str(result->mac, mac_str);

    int pos = snprintf(json_buf, buf_len,
                       "{\"mac\":\"%s\",\"services\":[", mac_str);

    for (uint8_t i = 0; i < result->service_count && pos < (int)buf_len - 10; i++) {
        const ble_discovery_service_t *svc = &result->services[i];
        char uuid_str[40];
        ble_gatt_discovery_uuid_to_str(&svc->uuid, uuid_str, sizeof(uuid_str));

        if (i > 0) {
            pos += snprintf(json_buf + pos, buf_len - pos, ",");
        }

        pos += snprintf(json_buf + pos, buf_len - pos,
                        "{\"uuid\":\"%s\",\"name\":\"%s\",\"chars\":%d}",
                        uuid_str, svc->name, svc->char_count);
    }

    pos += snprintf(json_buf + pos, buf_len - pos, "]}");

    return pos;
}

void ble_gatt_discovery_set_callback(ble_discovery_complete_cb_t callback)
{
    if (s_initialized) {
        xSemaphoreTake(s_mutex, GW_TIMEOUT_LONG_TICKS);
        s_global_callback = callback;
        xSemaphoreGive(s_mutex);
    }
}

/* ============================================================================
 * Private Function Implementation
 * ============================================================================ */

/**
 * @brief Lookup UUID name in table
 */
static const char *lookup_uuid16_name(uint16_t uuid, const uuid_name_entry_t *table)
{
    for (int i = 0; table[i].name != NULL; i++) {
        if (table[i].uuid == uuid) {
            return table[i].name;
        }
    }
    return NULL;
}

/**
 * @brief Find cached discovery result by MAC
 */
static ble_discovery_result_t *find_cached(const uint8_t *mac)
{
    for (int i = 0; i < BLE_DISCOVERY_MAX_CACHED_DEVICES; i++) {
        if (s_cache[i].complete && ble_mac_equal(s_cache[i].mac, mac)) {
            return &s_cache[i];
        }
    }
    return NULL;
}

/**
 * @brief Find free cache slot
 */
static ble_discovery_result_t *find_free_cache_slot(void)
{
    /* First pass: find empty slot */
    for (int i = 0; i < BLE_DISCOVERY_MAX_CACHED_DEVICES; i++) {
        if (!s_cache[i].complete) {
            return &s_cache[i];
        }
    }

    /* Cache full: find oldest entry */
    int oldest_idx = 0;
    uint32_t oldest_time = UINT32_MAX;
    for (int i = 0; i < BLE_DISCOVERY_MAX_CACHED_DEVICES; i++) {
        if (s_cache[i].discovery_time < oldest_time) {
            oldest_time = s_cache[i].discovery_time;
            oldest_idx = i;
        }
    }

    memset(&s_cache[oldest_idx], 0, sizeof(ble_discovery_result_t));
    return &s_cache[oldest_idx];
}

/**
 * @brief Copy UUID name to buffer
 */
static void copy_uuid_name(ble_gatt_uuid_t *uuid, char *name, size_t name_len, bool is_service)
{
    if (uuid->type == BLE_GATT_UUID_TYPE_16) {
        const char *known_name = is_service ?
            lookup_uuid16_name(uuid->value.uuid16, s_service_names) :
            lookup_uuid16_name(uuid->value.uuid16, s_char_names);

        if (known_name != NULL) {
            strncpy(name, known_name, name_len - 1);
            name[name_len - 1] = '\0';
            return;
        }
    }

    /* Unknown UUID - format as hex */
    ble_gatt_discovery_uuid_to_str(uuid, name, name_len);
}

/**
 * @brief Perform actual discovery
 */
static esp_err_t perform_discovery(discovery_context_t *ctx)
{
    uint16_t conn_handle = ctx->conn_handle;
    ble_discovery_result_t *result = &ctx->result;

    /* Get discovered services from GATT client */
    ble_gatt_service_t services[BLE_GATT_MAX_SERVICES];
    uint8_t svc_count = 0;

    esp_err_t ret = ble_gatt_get_services(conn_handle, services, BLE_GATT_MAX_SERVICES, &svc_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get services: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Found %d services", svc_count);

    /* Copy service information */
    result->service_count = 0;
    for (uint8_t i = 0; i < svc_count && result->service_count < BLE_DISCOVERY_MAX_SERVICES; i++) {
        ble_discovery_service_t *dst = &result->services[result->service_count];

        memcpy(&dst->uuid, &services[i].uuid, sizeof(ble_gatt_uuid_t));
        dst->start_handle = services[i].start_handle;
        dst->end_handle = services[i].end_handle;
        dst->primary = services[i].primary;

        /* Lookup service name */
        copy_uuid_name(&dst->uuid, dst->name, sizeof(dst->name), true);

        /* Get characteristics for this service */
        ble_gatt_characteristic_t chars[BLE_GATT_MAX_CHARACTERISTICS];
        uint8_t char_count = 0;

        ret = ble_gatt_get_characteristics(conn_handle, services[i].start_handle,
                                            chars, BLE_GATT_MAX_CHARACTERISTICS, &char_count);

        dst->char_count = 0;
        if (ret == ESP_OK) {
            for (uint8_t j = 0; j < char_count && dst->char_count < BLE_DISCOVERY_MAX_CHARS_PER_SVC; j++) {
                ble_discovery_characteristic_t *chr = &dst->characteristics[dst->char_count];

                memcpy(&chr->uuid, &chars[j].uuid, sizeof(ble_gatt_uuid_t));
                chr->value_handle = chars[j].value_handle;
                chr->cccd_handle = chars[j].cccd_handle;
                chr->properties = chars[j].properties;

                /* Lookup characteristic name */
                copy_uuid_name(&chr->uuid, chr->name, sizeof(chr->name), false);

                dst->char_count++;
            }
        }

        char uuid_str[40];
        ble_gatt_discovery_uuid_to_str(&dst->uuid, uuid_str, sizeof(uuid_str));
        ESP_LOGD(TAG, "  Service %s (%s): %d characteristics",
                 uuid_str, dst->name, dst->char_count);

        result->service_count++;
    }

    return ESP_OK;
}
