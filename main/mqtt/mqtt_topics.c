/**
 * @file mqtt_topics.c
 * @brief MQTT Topics Helper Implementation
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "mqtt_topics.h"
#include <string.h>
#include <ctype.h>
#include "esp_log.h"

/* Log tag */
static const char *TAG = "MQTT_TOPIC";

/**
 * @brief Build device state topic
 */
esp_err_t mqtt_topic_device_state(const char *friendly_name, char *topic_buf, size_t buf_len)
{
    if (!friendly_name || !topic_buf || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = snprintf(topic_buf, buf_len, "%s/%s", MQTT_BASE_TOPIC, friendly_name);
    if (ret < 0 || ret >= buf_len) {
        ESP_LOGE(TAG, "Buffer too small for device state topic");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Build device set topic
 */
esp_err_t mqtt_topic_device_set(const char *friendly_name, char *topic_buf, size_t buf_len)
{
    if (!friendly_name || !topic_buf || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = snprintf(topic_buf, buf_len, "%s/%s/set", MQTT_BASE_TOPIC, friendly_name);
    if (ret < 0 || ret >= buf_len) {
        ESP_LOGE(TAG, "Buffer too small for device set topic");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Build device get topic
 */
esp_err_t mqtt_topic_device_get(const char *friendly_name, char *topic_buf, size_t buf_len)
{
    if (!friendly_name || !topic_buf || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = snprintf(topic_buf, buf_len, "%s/%s/get", MQTT_BASE_TOPIC, friendly_name);
    if (ret < 0 || ret >= buf_len) {
        ESP_LOGE(TAG, "Buffer too small for device get topic");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Build device availability topic
 */
esp_err_t mqtt_topic_device_availability(const char *friendly_name, char *topic_buf, size_t buf_len)
{
    if (!friendly_name || !topic_buf || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = snprintf(topic_buf, buf_len, "%s/%s/availability",
                      MQTT_BASE_TOPIC, friendly_name);
    if (ret < 0 || ret >= buf_len) {
        ESP_LOGE(TAG, "Buffer too small for device availability topic");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Build Home Assistant discovery topic
 */
esp_err_t mqtt_topic_ha_discovery(const char *component, const char *device_id,
                                  char *topic_buf, size_t buf_len)
{
    if (!component || !device_id || !topic_buf || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = snprintf(topic_buf, buf_len, "%s/%s/%s/config",
                      TOPIC_HA_DISCOVERY_PREFIX, component, device_id);
    if (ret < 0 || ret >= buf_len) {
        ESP_LOGE(TAG, "Buffer too small for HA discovery topic");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Sanitize friendly name
 */
esp_err_t mqtt_topic_sanitize_name(const char *name, char *sanitized, size_t buf_len)
{
    if (!name || !sanitized || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t name_len = strlen(name);
    if (name_len >= buf_len) {
        ESP_LOGE(TAG, "Buffer too small for sanitized name");
        return ESP_ERR_NO_MEM;
    }

    /* Copy and sanitize character by character */
    for (size_t i = 0; i < name_len && i < buf_len - 1; i++) {
        char c = name[i];

        /* Replace spaces, slashes, and special characters with underscore */
        if (isspace(c) || c == '/' || c == '+' || c == '#' ||
            c == '$' || c == '\\' || c == '*' || c == '?') {
            sanitized[i] = '_';
        } else if (isalnum(c) || c == '_' || c == '-' || c == '.') {
            /* Allow alphanumeric, underscore, hyphen, and dot */
            sanitized[i] = c;
        } else {
            /* Replace any other character with underscore */
            sanitized[i] = '_';
        }
    }

    sanitized[name_len] = '\0';
    return ESP_OK;
}

/**
 * @brief Extract friendly name from topic
 */
esp_err_t mqtt_topic_extract_friendly_name(const char *topic, char *friendly_name, size_t buf_len)
{
    if (!topic || !friendly_name || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check if topic starts with base topic */
    size_t base_len = strlen(MQTT_BASE_TOPIC);
    if (strncmp(topic, MQTT_BASE_TOPIC, base_len) != 0) {
        ESP_LOGE(TAG, "Topic does not start with base topic");
        return ESP_ERR_INVALID_ARG;
    }

    /* Skip base topic and leading slash */
    const char *name_start = topic + base_len;
    if (*name_start == '/') {
        name_start++;
    }

    /* Find end of friendly name (next slash or end of string) */
    const char *name_end = strchr(name_start, '/');
    size_t name_len;

    if (name_end) {
        name_len = name_end - name_start;
    } else {
        name_len = strlen(name_start);
    }

    /* Check buffer size */
    if (name_len >= buf_len) {
        ESP_LOGE(TAG, "Buffer too small for friendly name");
        return ESP_ERR_NO_MEM;
    }

    /* Copy friendly name */
    memcpy(friendly_name, name_start, name_len);
    friendly_name[name_len] = '\0';

    return ESP_OK;
}

/**
 * @brief Check if topic matches pattern with wildcards
 */
bool mqtt_topic_matches(const char *topic, const char *pattern)
{
    if (!topic || !pattern) {
        return false;
    }

    /* Simple wildcard matching */
    while (*pattern && *topic) {
        if (*pattern == '#') {
            /* Multi-level wildcard - matches everything from here */
            return true;
        } else if (*pattern == '+') {
            /* Single-level wildcard - match until next slash */
            while (*topic && *topic != '/') {
                topic++;
            }
            pattern++;
        } else if (*pattern == *topic) {
            /* Exact character match */
            pattern++;
            topic++;
        } else {
            /* Mismatch */
            return false;
        }
    }

    /* Check if both reached end (allowing trailing # in pattern) */
    if (*pattern == '#') {
        return true;
    }

    return (*pattern == '\0' && *topic == '\0');
}

/**
 * @brief Test MQTT topics
 */
esp_err_t mqtt_topic_test(void)
{
    ESP_LOGI(TAG, "=== MQTT Topics Test ===");

    char topic[MQTT_TOPIC_MAX_LEN];
    char name[64];
    esp_err_t ret;

    /* Test device state topic */
    ret = mqtt_topic_device_state("living_room_light", topic, sizeof(topic));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test FAILED: device_state");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Device state topic: %s", topic);

    /* Test device set topic */
    ret = mqtt_topic_device_set("bedroom_switch", topic, sizeof(topic));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test FAILED: device_set");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Device set topic: %s", topic);

    /* Test device get topic */
    ret = mqtt_topic_device_get("temperature_sensor", topic, sizeof(topic));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test FAILED: device_get");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Device get topic: %s", topic);

    /* Test device availability topic */
    ret = mqtt_topic_device_availability("motion_sensor", topic, sizeof(topic));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test FAILED: device_availability");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Device availability topic: %s", topic);

    /* Test HA discovery topic */
    ret = mqtt_topic_ha_discovery("light", "0x00124b001234abcd", topic, sizeof(topic));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test FAILED: ha_discovery");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "HA discovery topic: %s", topic);

    /* Test name sanitization */
    ret = mqtt_topic_sanitize_name("Living Room / Light #1", name, sizeof(name));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test FAILED: sanitize_name");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Sanitized name: %s", name);

    /* Test name extraction */
    ret = mqtt_topic_extract_friendly_name("zigbee2mqtt/bedroom_light/set", name, sizeof(name));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Test FAILED: extract_friendly_name");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Extracted name: %s", name);

    /* Test topic matching */
    bool match1 = mqtt_topic_matches("zigbee2mqtt/device1/set", "zigbee2mqtt/+/set");
    bool match2 = mqtt_topic_matches("zigbee2mqtt/device1/state", "zigbee2mqtt/#");
    bool match3 = mqtt_topic_matches("zigbee2mqtt/device1/set", "zigbee2mqtt/device2/set");

    ESP_LOGI(TAG, "Topic match tests: %d %d %d (expected: 1 1 0)",
            match1, match2, match3);

    if (!match1 || !match2 || match3) {
        ESP_LOGE(TAG, "Test FAILED: topic_matches");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Test PASSED");
    return ESP_OK;
}
