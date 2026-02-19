/**
 * @file bridge_response.c
 * @brief Consistent Bridge Response Format Implementation
 *
 * Implements standardized response formatting for Zigbee2MQTT compatibility.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "bridge_response.h"
#include "gateway_defaults.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "BRIDGE_RSP";

/** Maximum length for stored strings (error message, transaction ID) */
#define MAX_STRING_LEN GW_BUFFER_SIZE_LARGE

/** Response status strings */
static const char *STATUS_OK = "ok";
static const char *STATUS_ERROR = "error";

/* Internal storage for copied strings */
static char s_error_buffer[MAX_STRING_LEN];
static char s_transaction_buffer[MAX_STRING_LEN];

esp_err_t bridge_response_init(bridge_response_t *resp)
{
    if (resp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    resp->status = BRIDGE_RESPONSE_OK;
    resp->error_message = NULL;
    resp->transaction_id = NULL;
    resp->data = NULL;

    return ESP_OK;
}

esp_err_t bridge_response_set_ok(bridge_response_t *resp, cJSON *data)
{
    if (resp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    resp->status = BRIDGE_RESPONSE_OK;
    resp->error_message = NULL;
    resp->data = data;

    return ESP_OK;
}

esp_err_t bridge_response_set_error(bridge_response_t *resp, const char *error)
{
    if (resp == NULL || error == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    resp->status = BRIDGE_RESPONSE_ERROR;

    /* Copy error message to internal buffer */
    strncpy(s_error_buffer, error, MAX_STRING_LEN - 1);
    s_error_buffer[MAX_STRING_LEN - 1] = '\0';
    resp->error_message = s_error_buffer;

    /* Clear any existing data for error responses */
    if (resp->data != NULL) {
        cJSON_Delete(resp->data);
        resp->data = NULL;
    }

    return ESP_OK;
}

esp_err_t bridge_response_set_transaction(bridge_response_t *resp, const char *txn_id)
{
    if (resp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (txn_id == NULL) {
        resp->transaction_id = NULL;
        return ESP_OK;
    }

    /* Copy transaction ID to internal buffer */
    strncpy(s_transaction_buffer, txn_id, MAX_STRING_LEN - 1);
    s_transaction_buffer[MAX_STRING_LEN - 1] = '\0';
    resp->transaction_id = s_transaction_buffer;

    return ESP_OK;
}

char* bridge_response_to_json(const bridge_response_t *resp)
{
    if (resp == NULL) {
        ESP_LOGE(TAG, "NULL response pointer");
        return NULL;
    }

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return NULL;
    }

    /* Add data field - use empty object if NULL */
    if (resp->data != NULL) {
        /* Create a duplicate to avoid ownership issues */
        cJSON *data_copy = cJSON_Duplicate(resp->data, true);
        if (data_copy != NULL) {
            cJSON_AddItemToObject(json, "data", data_copy);
        } else {
            cJSON *empty_data = cJSON_CreateObject();
            if (empty_data == NULL) {
                ESP_LOGE(TAG, "Failed to create empty data JSON object");
                cJSON_Delete(json);
                return NULL;
            }
            cJSON_AddItemToObject(json, "data", empty_data);
        }
    } else {
        cJSON *empty_data = cJSON_CreateObject();
        if (empty_data == NULL) {
            ESP_LOGE(TAG, "Failed to create empty data JSON object");
            cJSON_Delete(json);
            return NULL;
        }
        cJSON_AddItemToObject(json, "data", empty_data);
    }

    /* Add status field */
    const char *status_str = (resp->status == BRIDGE_RESPONSE_OK) ? STATUS_OK : STATUS_ERROR;
    cJSON_AddStringToObject(json, "status", status_str);

    /* Add error field only for error responses */
    if (resp->status == BRIDGE_RESPONSE_ERROR && resp->error_message != NULL) {
        cJSON_AddStringToObject(json, "error", resp->error_message);
    }

    /* Add transaction field if present */
    if (resp->transaction_id != NULL) {
        cJSON_AddStringToObject(json, "transaction", resp->transaction_id);
    }

    /* Convert to string */
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON response");
    }

    return json_str;
}

esp_err_t bridge_response_publish(const char *response_topic, const bridge_response_t *resp)
{
    if (response_topic == NULL || resp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json_str = bridge_response_to_json(resp);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to build response JSON");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Publishing response to %s: %s", response_topic, json_str);

    /* Ownership transfer via event bus:
     * - topic: strdup'd because response_topic is a const param (stack/static/constant)
     * - payload_json: json_str from cJSON_PrintUnformatted (heap) - ownership transfers directly
     * The handler (mqtt_event_handler.c) frees both via free() after use. */
    char *topic_dup = strdup(response_topic);
    if (topic_dup == NULL) {
        ESP_LOGE(TAG, "Failed to strdup response topic");
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    evt_ha_discovery_publish_t evt = {
        .topic = topic_dup,
        .payload_json = json_str,
        .qos = 0,
        .retain = false
    };

    esp_err_t ret = event_publish(EVT_HA_DISCOVERY_PUBLISH, &evt, sizeof(evt));

    if (ret != ESP_OK) {
        /* event_publish failed - handler will not run, so we must free both */
        ESP_LOGE(TAG, "Failed to publish response event: %s", esp_err_to_name(ret));
        free(topic_dup);
        free(json_str);
    }
    /* On success: do NOT free topic_dup or json_str - ownership transferred to handler */

    return ret;
}

void bridge_response_free(bridge_response_t *resp)
{
    if (resp == NULL) {
        return;
    }

    /* Free data cJSON object if present */
    if (resp->data != NULL) {
        cJSON_Delete(resp->data);
        resp->data = NULL;
    }

    /* Clear pointers (strings are in static buffers, not heap allocated) */
    resp->error_message = NULL;
    resp->transaction_id = NULL;
    resp->status = BRIDGE_RESPONSE_OK;
}

const char* bridge_request_get_transaction(const cJSON *request)
{
    if (request == NULL) {
        return NULL;
    }

    cJSON *txn = cJSON_GetObjectItem(request, "transaction");
    if (txn == NULL || !cJSON_IsString(txn)) {
        return NULL;
    }

    return txn->valuestring;
}

esp_err_t bridge_response_topic_from_request(const char *request_topic,
                                             char *response_topic,
                                             size_t buf_len)
{
    if (request_topic == NULL || response_topic == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Look for "/request/" in the topic and replace with "/response/" */
    const char *request_marker = strstr(request_topic, "/request/");
    if (request_marker == NULL) {
        /* No /request/ found, just copy as-is */
        strncpy(response_topic, request_topic, buf_len - 1);
        response_topic[buf_len - 1] = '\0';
        return ESP_OK;
    }

    /* Calculate positions */
    size_t prefix_len = request_marker - request_topic;
    const char *suffix = request_marker + strlen("/request/");

    /* Build response topic: prefix + "/response/" + suffix */
    size_t required_len = prefix_len + strlen("/response/") + strlen(suffix) + 1;
    if (required_len > buf_len) {
        ESP_LOGE(TAG, "Response topic buffer too small: need %zu, have %zu",
                 required_len, buf_len);
        return ESP_ERR_NO_MEM;
    }

    /* Copy prefix */
    memcpy(response_topic, request_topic, prefix_len);

    /* Add "/response/" */
    memcpy(response_topic + prefix_len, "/response/", strlen("/response/"));

    /* Add suffix and null terminator - use memcpy with explicit length */
    size_t suffix_len = strlen(suffix);
    memcpy(response_topic + prefix_len + strlen("/response/"), suffix, suffix_len + 1);

    return ESP_OK;
}

esp_err_t bridge_response_publish_ok(const char *response_topic,
                                     cJSON *data,
                                     const char *transaction_id)
{
    if (response_topic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bridge_response_t resp;
    bridge_response_init(&resp);

    /* Duplicate data if provided (we don't take ownership) */
    if (data != NULL) {
        resp.data = cJSON_Duplicate(data, true);
    }

    bridge_response_set_transaction(&resp, transaction_id);

    esp_err_t ret = bridge_response_publish(response_topic, &resp);

    bridge_response_free(&resp);

    return ret;
}

esp_err_t bridge_response_publish_error(const char *response_topic,
                                        const char *error_message,
                                        const char *transaction_id)
{
    if (response_topic == NULL || error_message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bridge_response_t resp;
    bridge_response_init(&resp);
    bridge_response_set_error(&resp, error_message);
    bridge_response_set_transaction(&resp, transaction_id);

    esp_err_t ret = bridge_response_publish(response_topic, &resp);

    bridge_response_free(&resp);

    return ret;
}
