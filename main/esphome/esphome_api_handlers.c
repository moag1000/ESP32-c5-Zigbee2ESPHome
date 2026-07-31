/**
 * @file esphome_api_handlers.c
 * @brief ESPHome API Message Handlers
 *
 * Implements message type switch/dispatch, individual message handlers,
 * and response building for the ESPHome Native API protocol.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_api_internal.h"
#include "esphome_noise.h"
#include "esphome_protocol.h"
#include "esphome_services.h"
#include "esphome_ble_proxy.h"
#include "core/memory/memory_manager_ng.h"
#include "core/events/event_bus.h"
#include "core/events/event_data.h"
#include "core/device/device_registry.h"
#include "zigbee/converter/zb_converter.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include "esp_log.h"
#include "esp_mac.h"

/* Log tag */
static const char *TAG = "ESPHOME_HDL";

/* ============================================================================
 * Entity Enumeration Callbacks
 * ============================================================================ */

/**
 * @brief Entity enumeration callback for ListEntities
 */
bool esphome_api_list_entities_callback(esphome_entity_type_t type, esphome_entity_key_t key,
                                        const void *config, void *user_data)
{
    esphome_list_entities_ctx_t *ctx = (esphome_list_entities_ctx_t *)user_data;

    ESP_LOGI(TAG, "Listing entity: type=%d, key=%" PRIu32, (int)type, key);

    /* Allocate buffer on heap to reduce stack usage in callback */
    uint8_t *buffer = mem_alloc(ESPHOME_BUFFER_LARGE, MEM_CAP_PSRAM);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate list entities buffer");
        ctx->result = ESP_ERR_NO_MEM;
        return false; /* Stop enumeration */
    }

    size_t buffer_len;
    esp_err_t ret = ESP_OK;

    switch (type) {
        case ESPHOME_ENTITY_SENSOR:
            ret = esphome_encode_sensor_list_entry(
                (const esphome_sensor_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_BINARY_SENSOR:
            ret = esphome_encode_binary_sensor_list_entry(
                (const esphome_binary_sensor_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_SWITCH:
            ret = esphome_encode_switch_list_entry(
                (const esphome_switch_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_TEXT_SENSOR:
            ret = esphome_encode_text_sensor_list_entry(
                (const esphome_text_sensor_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_NUMBER:
            ret = esphome_encode_number_list_entry(
                (const esphome_number_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_BUTTON:
            ret = esphome_encode_button_list_entry(
                (const esphome_button_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_SELECT:
            ret = esphome_encode_select_list_entry(
                (const esphome_select_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_LIGHT:
            ret = esphome_encode_light_list_entry(
                (const esphome_light_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_COVER:
            ret = esphome_encode_cover_list_entry(
                (const esphome_cover_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_FAN:
            ret = esphome_encode_fan_list_entry(
                (const esphome_fan_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_CLIMATE:
            ret = esphome_encode_climate_list_entry(
                (const esphome_climate_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_LOCK:
            ret = esphome_encode_lock_list_entry(
                (const esphome_lock_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_TEXT:
            ret = esphome_encode_text_list_entry(
                (const esphome_text_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_MEDIA_PLAYER:
            ret = esphome_encode_media_player_list_entry(
                (const esphome_media_player_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        case ESPHOME_ENTITY_ALARM_PANEL:
            ret = esphome_encode_alarm_list_entry(
                (const esphome_alarm_config_t *)config,
                buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
            break;

        default:
            mem_ng_free(buffer);
            return true; /* Continue with other entities */
    }

    if (ret == ESP_OK) {
        ret = esphome_api_send_message(ctx->client, buffer, buffer_len);
    }

    mem_ng_free(buffer);

    if (ret != ESP_OK) {
        ctx->result = ret;
        return false; /* Stop enumeration */
    }

    return true; /* Continue enumeration */
}

/**
 * @brief Entity enumeration callback for state broadcast
 */
bool esphome_api_broadcast_states_callback(esphome_entity_type_t type, esphome_entity_key_t key,
                                           const void *config, void *user_data)
{
    esphome_broadcast_states_ctx_t *ctx = (esphome_broadcast_states_ctx_t *)user_data;

    /* Allocate buffer on heap to reduce stack usage in callback */
    uint8_t *buffer = mem_alloc(ESPHOME_BUFFER_MEDIUM, MEM_CAP_PSRAM);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate broadcast states buffer");
        return true; /* Continue enumeration but skip this entity */
    }

    size_t buffer_len;
    esp_err_t ret = ESP_OK;

    switch (type) {
        case ESPHOME_ENTITY_SENSOR: {
            esphome_sensor_state_t state;
            if (esphome_entity_get_sensor(key, &state) == ESP_OK) {
                ret = esphome_encode_sensor_state(&state, buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_BINARY_SENSOR: {
            esphome_binary_sensor_state_t state;
            if (esphome_entity_get_binary_sensor(key, &state) == ESP_OK) {
                ret = esphome_encode_binary_sensor_state(&state, buffer, ESPHOME_BUFFER_MEDIUM,
                                                          &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_SWITCH: {
            esphome_switch_state_t state;
            if (esphome_entity_get_switch(key, &state) == ESP_OK) {
                ret = esphome_encode_switch_state(&state, buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_TEXT_SENSOR: {
            esphome_text_sensor_state_t state;
            if (esphome_entity_get_text_sensor(key, &state) == ESP_OK) {
                ret = esphome_encode_text_sensor_state(&state, buffer, ESPHOME_BUFFER_MEDIUM,
                                                        &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_NUMBER: {
            esphome_number_state_t state;
            if (esphome_entity_get_number(key, &state) == ESP_OK) {
                ret = esphome_encode_number_state(&state, buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_SELECT: {
            esphome_select_state_t state;
            if (esphome_entity_get_select(key, &state) == ESP_OK) {
                ret = esphome_encode_select_state(&state, buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_LIGHT: {
            esphome_light_state_t state;
            if (esphome_entity_get_light(key, &state) == ESP_OK) {
                ret = esphome_encode_light_state(&state, buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_COVER: {
            esphome_cover_state_t state;
            if (esphome_entity_get_cover(key, &state) == ESP_OK) {
                ret = esphome_encode_cover_state(&state, buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_FAN: {
            esphome_fan_state_t state;
            if (esphome_entity_get_fan(key, &state) == ESP_OK) {
                ret = esphome_encode_fan_state(&state, buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_CLIMATE: {
            esphome_climate_state_t state;
            if (esphome_entity_get_climate(key, &state) == ESP_OK) {
                ret = esphome_encode_climate_state(&state, buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_LOCK: {
            esphome_lock_entity_state_t state;
            if (esphome_entity_get_lock(key, &state) == ESP_OK) {
                ret = esphome_encode_lock_state(&state, buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_TEXT: {
            esphome_text_entity_state_t state;
            if (esphome_entity_get_text(key, &state) == ESP_OK) {
                ret = esphome_encode_text_state(&state, buffer, ESPHOME_BUFFER_MEDIUM, &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_MEDIA_PLAYER: {
            esphome_media_player_entity_state_t state;
            if (esphome_entity_get_media_player(key, &state) == ESP_OK) {
                ret = esphome_encode_media_player_state(&state, buffer, ESPHOME_BUFFER_MEDIUM,
                                                         &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_ALARM_PANEL: {
            esphome_alarm_entity_state_t state;
            if (esphome_entity_get_alarm(key, &state) == ESP_OK) {
                ret = esphome_encode_alarm_state(&state, buffer, ESPHOME_BUFFER_MEDIUM,
                                                        &buffer_len);
                if (ret == ESP_OK) {
                    esphome_api_send_message(ctx->client, buffer, buffer_len);
                }
            }
            break;
        }

        case ESPHOME_ENTITY_BUTTON:
            /* Buttons have no state to broadcast */
            break;

        default:
            break;
    }

    mem_ng_free(buffer);
    return true; /* Continue enumeration */
}

/* ============================================================================
 * Core Protocol Handlers
 * ============================================================================ */

/**
 * @brief Handle HelloRequest message
 */
static esp_err_t handle_hello_request(esphome_client_t *client, const esphome_message_t *msg)
{
    esphome_api_state_t *state = esphome_api_get_state();

    ESP_LOGD(TAG, "Received HelloRequest");

    /* Parse client info from payload if present */
    if (msg->payload && msg->payload_len > 0) {
        esphome_buffer_t buf;
        esphome_buffer_init(&buf, msg->payload, msg->payload_len);

        while (esphome_buffer_remaining(&buf) > 0) {
            uint32_t field_num;
            protobuf_wire_type_t wire_type;

            if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
                break;
            }

            if (field_num == 1 && wire_type == PROTOBUF_WIRE_LEN) {
                esphome_decode_string(&buf, client->client_info, sizeof(client->client_info));
                ESP_LOGI(TAG, "Client info: %s", client->client_info);
            } else {
                esphome_skip_field(&buf, wire_type);
            }
        }
    }

    ESP_LOGI(TAG, "┌─ ESPHome Client Connected ─────────────");
    ESP_LOGI(TAG, "│ Client: %s", client->client_info[0] ? client->client_info : "(unknown)");
#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
    ESP_LOGI(TAG, "│ Encrypted: %s", client->encryption_enabled ? "yes (Noise)" : "no");
#endif
    ESP_LOGI(TAG, "└────────────────────────────────────────");

    esp_err_t ret = ESP_OK;

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
#ifndef CONFIG_ESPHOME_ALLOW_PLAINTEXT
    /* If plaintext client connects but encryption is required, send 0x01 preamble.
     * aioesphomeapi detects preamble 0x01 as "RequiresEncryptionAPIError".
     *
     * IMPORTANT: We must send MORE than just 0x01. The plaintext frame helper
     * in aioesphomeapi reads data in chunks. We need to send enough data that
     * the client's read() returns and processes the 0x01 preamble.
     *
     * Send a minimal "Noise hello" frame:
     * - 0x01: Noise indicator
     * - 0x00 0x00: Length (0 bytes)
     * This signals encryption required without any payload.
     */
    if (!client->encryption_enabled && client->require_encryption_disconnect) {
        ESP_LOGI(TAG, "Sending encryption required signal to plaintext client");

        /* Ensure immediate sending with TCP_NODELAY */
        int nodelay = 1;
        setsockopt(client->socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        /* Send Noise indicator with minimal frame header */
        uint8_t noise_signal[] = { 0x01, 0x00, 0x00 };  /* indicator + 2-byte length (0) */
        int sent = send(client->socket, noise_signal, sizeof(noise_signal), 0);
        if (sent != sizeof(noise_signal)) {
            ESP_LOGW(TAG, "Failed to send encryption required signal");
        } else {
            ESP_LOGI(TAG, "Sent encryption required signal (0x01 0x00 0x00)");
        }

        /* Keep connection open briefly so client can read the data.
         * Don't close immediately - let the TCP stack flush the data. */
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Now return error to close connection from our side */
        return ESPHOME_ERR_NOT_CONNECTED;
    }
#endif
#endif

    /* Allocate output buffer on heap to reduce stack usage */
    uint8_t *output = mem_alloc(ESPHOME_BUFFER_MEDIUM, MEM_CAP_PSRAM);
    if (output == NULL) {
        ESP_LOGE(TAG, "Failed to allocate hello response buffer");
        return ESP_ERR_NO_MEM;
    }

    /* Build HelloResponse - payload is small enough for stack */
    uint8_t payload[ESPHOME_BUFFER_SMALL];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: api_version_major */
    esphome_encode_uint32(&buf, 1, ESPHOME_API_VERSION_MAJOR);

    /* Field 2: api_version_minor */
    esphome_encode_uint32(&buf, 2, ESPHOME_API_VERSION_MINOR);

    /* Field 3: server_info */
    esphome_encode_string(&buf, 3, "ESP32-C5 Zigbee Gateway");

    /* Field 4: name */
    esphome_encode_string(&buf, 4, state->config.device_name);

    /* Build and send message */
    size_t output_len;

    ret = esphome_build_message(ESPHOME_MSG_HELLO_RESPONSE, payload, buf.position,
                                output, ESPHOME_BUFFER_MEDIUM, &output_len);
    if (ret == ESP_OK) {
        ret = esphome_api_send_message(client, output, output_len);
        if (ret == ESP_OK) {
            client->state = ESPHOME_CLIENT_HELLO_RECEIVED;
        }
    }

    mem_ng_free(output);
    return ret;
}

/**
 * @brief Handle ConnectRequest message
 */
static esp_err_t handle_connect_request(esphome_client_t *client, const esphome_message_t *msg)
{
    esphome_api_state_t *state = esphome_api_get_state();

    ESP_LOGI(TAG, "Received ConnectRequest");

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
#ifndef CONFIG_ESPHOME_ALLOW_PLAINTEXT
    /* Reject plaintext clients trying to connect when encryption is required */
    if (!client->encryption_enabled) {
        ESP_LOGW(TAG, "Rejecting ConnectRequest from plaintext client - encryption required");
        return ESPHOME_ERR_NOT_CONNECTED;  /* Disconnect plaintext client */
    }
#endif
#endif

    bool password_valid = true;
    char provided_password[ESPHOME_STRING_BUFFER_MEDIUM] = "";

    /* Parse password from payload if present */
    if (msg->payload && msg->payload_len > 0) {
        esphome_buffer_t buf;
        esphome_buffer_init(&buf, msg->payload, msg->payload_len);

        while (esphome_buffer_remaining(&buf) > 0) {
            uint32_t field_num;
            protobuf_wire_type_t wire_type;

            if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
                break;
            }

            if (field_num == 1 && wire_type == PROTOBUF_WIRE_LEN) {
                esphome_decode_string(&buf, provided_password, sizeof(provided_password));
            } else {
                esphome_skip_field(&buf, wire_type);
            }
        }
    }

    /* With Noise encryption, the PSK handshake serves as authentication */
    if (client->noise_ctx != NULL && esphome_noise_is_ready(client->noise_ctx)) {
        password_valid = true;
    } else if (state->config.password[0] != '\0') {
        password_valid = (strcmp(provided_password, state->config.password) == 0);
        if (!password_valid) {
            ESP_LOGW(TAG, "Authentication failed");
            state->stats.authentication_failures++;
        }
    }

    /* Build ConnectResponse */
    uint8_t payload[ESPHOME_PAYLOAD_SMALL];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: invalid_password */
    esphome_encode_bool(&buf, 1, !password_valid);

    /* Build and send message */
    uint8_t output[ESPHOME_STRING_BUFFER_MEDIUM];
    size_t output_len;

    esp_err_t ret = esphome_build_message(ESPHOME_MSG_CONNECT_RESPONSE, payload, buf.position,
                                          output, sizeof(output), &output_len);
    if (ret == ESP_OK) {
        ret = esphome_api_send_message(client, output, output_len);
        if (ret == ESP_OK && password_valid) {
            client->state = ESPHOME_CLIENT_AUTHENTICATED;
            ESP_LOGI(TAG, "Client authenticated successfully");

            /* Publish connected event */
            uint8_t client_id = esphome_api_get_client_index(client);
            evt_esphome_connected_t evt = {
                .client_id = client_id,
                .client_info = client->client_info[0] ? client->client_info : NULL,
#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
                .encrypted = (client->noise_ctx != NULL &&
                              esphome_noise_is_ready(client->noise_ctx)),
#else
                .encrypted = false,
#endif
            };
            event_publish(EVT_ESPHOME_CONNECTED, &evt, sizeof(evt));
        }
    }

    return ret;
}

/**
 * @brief Handle DisconnectRequest message
 */
static esp_err_t handle_disconnect_request(esphome_client_t *client, const esphome_message_t *msg)
{
    ESP_LOGI(TAG, "Received DisconnectRequest");

    /* Send DisconnectResponse */
    uint8_t output[ESPHOME_OUTPUT_BUFFER_SMALL];
    size_t output_len;

    esp_err_t ret = esphome_build_empty_message(ESPHOME_MSG_DISCONNECT_RESPONSE,
                                                 output, sizeof(output), &output_len);
    if (ret == ESP_OK) {
        esphome_api_send_message(client, output, output_len);
    }

    /* Signal disconnect */
    return ESPHOME_ERR_NOT_CONNECTED;
}

/**
 * @brief Handle PingRequest message
 */
static esp_err_t handle_ping_request(esphome_client_t *client, const esphome_message_t *msg)
{
    ESP_LOGD(TAG, "Received PingRequest");

    uint8_t output[ESPHOME_OUTPUT_BUFFER_SMALL];
    size_t output_len;

    esp_err_t ret = esphome_build_empty_message(ESPHOME_MSG_PING_RESPONSE,
                                                 output, sizeof(output), &output_len);
    if (ret == ESP_OK) {
        ret = esphome_api_send_message(client, output, output_len);
    }

    return ret;
}

/**
 * @brief Handle ping response (pong) from client
 */
esp_err_t esphome_api_handle_ping_response(esphome_client_t *client)
{
    client->ping_pending = false;
    client->last_pong_received = esphome_api_get_timestamp_ms();
    ESP_LOGD(TAG, "Received pong from client");
    return ESP_OK;
}

/**
 * @brief Context for sub-device encoding callback
 */
typedef struct {
    esphome_buffer_t *buf;
} device_info_ctx_t;

/**
 * @brief Callback to encode each Zigbee device as a DeviceInfo sub-message
 *
 * Encodes a DeviceInfo sub-message (field 20) for each Zigbee device in the registry.
 * DeviceInfo proto (api.proto, ESPHome 2025.7.0+):
 *   message DeviceInfo {
 *     uint32 device_id = 1;
 *     string name = 2;
 *     uint32 area_id = 3;
 *     string manufacturer = 4;
 *     string model = 5;
 *     string hw_version = 6;
 *     string sw_version = 7;
 *   }
 */
static bool encode_sub_device_cb(device_t *dev, void *ctx)
{
    device_info_ctx_t *di_ctx = (device_info_ctx_t *)ctx;
    if (dev->protocol != DEV_PROTOCOL_ZIGBEE) {
        return true; /* Skip non-Zigbee devices */
    }

    uint8_t sub_msg[256];
    esphome_buffer_t sub_buf;
    esphome_buffer_init(&sub_buf, sub_msg, sizeof(sub_msg));

    /* Field 1: device_id (uint32) — lower 32 bits of IEEE address */
    uint32_t device_id = (uint32_t)(dev->id & 0xFFFFFFFF);
    esphome_encode_uint32(&sub_buf, 1, device_id);

    /* Field 2: name (string) — human-readable display name for HA.
     * Prefer converter vendor+description (e.g. "Adaprox Fingerbot Plus"),
     * fall back to model+short_addr, then IEEE hex. */
    char display_name[64];
    const zb_converter_def_t *cdef = (const zb_converter_def_t *)dev->proto.zigbee.converter;
    if (cdef && cdef->description && cdef->description[0]) {
        if (cdef->vendor && cdef->vendor[0]) {
            snprintf(display_name, sizeof(display_name), "%s %s", cdef->vendor, cdef->description);
        } else {
            snprintf(display_name, sizeof(display_name), "%s", cdef->description);
        }
    } else if (dev->model[0]) {
        snprintf(display_name, sizeof(display_name), "%s 0x%04X",
                 dev->model, dev->proto.zigbee.short_addr);
    } else {
        snprintf(display_name, sizeof(display_name), "0x%016llx", (unsigned long long)dev->id);
    }
    esphome_encode_string(&sub_buf, 2, display_name);

    /* Field 4: manufacturer (string) — shown as "von <manufacturer>" in HA.
     * Prefer converter vendor (human-readable), fall back to Zigbee manufacturer string. */
    const char *mfr = NULL;
    if (cdef && cdef->vendor && cdef->vendor[0]) {
        mfr = cdef->vendor;
    } else if (dev->manufacturer[0]) {
        mfr = dev->manufacturer;
    }
    if (mfr) {
        esphome_encode_string(&sub_buf, 4, mfr);
    }

    /* Field 5: model (string) — shown as model info in HA.
     * Prefer converter description, fall back to Zigbee model identifier. */
    const char *mdl = NULL;
    if (cdef && cdef->description && cdef->description[0]) {
        mdl = cdef->description;
    } else if (dev->model[0]) {
        mdl = dev->model;
    }
    if (mdl) {
        esphome_encode_string(&sub_buf, 5, mdl);
    }

    /* Field 6: hw_version — Zigbee model identifier (e.g. "TS0001") */
    if (dev->model[0]) {
        esphome_encode_string(&sub_buf, 6, dev->model);
    }

    if (!esphome_buffer_overflow(&sub_buf)) {
        esphome_encode_bytes(di_ctx->buf, 20, sub_msg, sub_buf.position);
    }

    return true; /* Continue iteration */
}

/**
 * @brief Handle DeviceInfoRequest message
 */
static esp_err_t handle_device_info_request(esphome_client_t *client, const esphome_message_t *msg)
{
    esphome_api_state_t *state = esphome_api_get_state();

    ESP_LOGI(TAG, "Received DeviceInfoRequest");

    esp_err_t ret = ESP_OK;

    /* Allocate buffers on heap to reduce stack usage.
     * Use LARGE payload for sub-device encoding, XLARGE output for framing. */
    uint8_t *payload = mem_alloc(ESPHOME_BUFFER_LARGE, MEM_CAP_PSRAM);
    uint8_t *output = mem_alloc(ESPHOME_BUFFER_XLARGE, MEM_CAP_PSRAM);
    if (payload == NULL || output == NULL) {
        ESP_LOGE(TAG, "Failed to allocate device info buffers");
        mem_ng_free(payload);
        mem_ng_free(output);
        return ESP_ERR_NO_MEM;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, ESPHOME_BUFFER_LARGE);

    /* Field 1: uses_password */
    esphome_encode_bool(&buf, 1, state->config.password[0] != '\0');

    /* Field 2: name */
    esphome_encode_string(&buf, 2, state->config.device_name);

    /* Field 3: mac_address */
    char mac[18];
    if (state->config.mac_address[0] != '\0') {
        strlcpy(mac, state->config.mac_address, sizeof(mac));
    } else {
        esphome_api_get_mac_address(mac, sizeof(mac));
    }
    esphome_encode_string(&buf, 3, mac);

    /* Field 4: esphome_version - must look like a real ESPHome version for HA compatibility.
     * 2025.7.0+ required for sub-device features in ESPHome protocol. */
    esphome_encode_string(&buf, 4, "2025.7.0");

    /* Field 5: compilation_time */
    esphome_encode_string(&buf, 5, ESPHOME_COMPILATION_TIME);

    /* Field 6: model */
    esphome_encode_string(&buf, 6, state->device_model[0] ? state->device_model : "ESP32-C5");

    /* Field 7: has_deep_sleep */
    esphome_encode_bool(&buf, 7, false);

    /* Field 8: project_name - MUST contain "." separator (HA splits on ".") */
    esphome_encode_string(&buf, 8, "esp32c5.zigbee-gateway");

    /* Field 9: project_version (our actual firmware version) */
    esphome_encode_string(&buf, 9, state->device_version[0] ? state->device_version : "1.0.0");

    /* Field 10: webserver_port */
    esphome_encode_uint32(&buf, 10, 0); /* No webserver */

    /* Field 11: legacy_bluetooth_proxy_version (0 = use feature flags instead) */
    esphome_encode_uint32(&buf, 11, 0);

    /* Field 12: manufacturer */
    esphome_encode_string(&buf, 12, "ESP32-C5 Project");

    /* Field 13: friendly_name */
    esphome_encode_string(&buf, 13, state->config.friendly_name);

    /* Field 15: bluetooth_proxy_feature_flags */
#if CONFIG_BT_ENABLED
    esphome_encode_uint32(&buf, 15,
        ESPHOME_BLE_PROXY_FEATURE_PASSIVE_SCAN |
        ESPHOME_BLE_PROXY_FEATURE_ACTIVE_SCAN |
        ESPHOME_BLE_PROXY_FEATURE_REMOTE_CACHING |
        ESPHOME_BLE_PROXY_FEATURE_PAIRING |
        ESPHOME_BLE_PROXY_FEATURE_CACHE_CLEARING |
        ESPHOME_BLE_PROXY_FEATURE_RAW_ADVERTISEMENTS |
        ESPHOME_BLE_PROXY_FEATURE_SCANNER_STATE);
#else
    esphome_encode_uint32(&buf, 15, 0);  /* BLE disabled on ESP32-C5 */
#endif

#if CONFIG_BT_ENABLED
    /* Field 18: bluetooth_mac_address - derive from WiFi MAC with offset */
    {
        uint8_t wifi_mac[6];
        esp_read_mac(wifi_mac, ESP_MAC_WIFI_STA);
        char ble_mac[18];
        uint8_t ble_last = (uint8_t)(wifi_mac[5] + 2);
        snprintf(ble_mac, sizeof(ble_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 wifi_mac[0], wifi_mac[1], wifi_mac[2],
                 wifi_mac[3], wifi_mac[4], ble_last);
        esphome_encode_string(&buf, 18, ble_mac);
    }
#endif

    /* Field 20: repeated DeviceInfo (Zigbee sub-devices) */
    {
        device_info_ctx_t di_ctx = { .buf = &buf };
        device_registry_iterate_zigbee(encode_sub_device_cb, &di_ctx);
    }

    /* Build and send message */
    size_t output_len;

    ret = esphome_build_message(ESPHOME_MSG_DEVICE_INFO_RESPONSE, payload, buf.position,
                                output, ESPHOME_BUFFER_XLARGE, &output_len);
    if (ret == ESP_OK) {
        ret = esphome_api_send_message(client, output, output_len);
    }

    mem_ng_free(payload);
    mem_ng_free(output);
    return ret;
}

/**
 * @brief Handle ListEntitiesRequest message
 */
static esp_err_t handle_list_entities_request(esphome_client_t *client, const esphome_message_t *msg)
{
    ESP_LOGI(TAG, "Received ListEntitiesRequest (encrypted=%s)",
             client->encryption_enabled ? "yes" : "no");

    /* Enumerate all entities and send their info */
    esphome_list_entities_ctx_t ctx = {
        .client = client,
        .result = ESP_OK,
    };

    ESP_LOGI(TAG, "Enumerating entities...");
    esphome_entities_enumerate(esphome_api_list_entities_callback, &ctx);
    ESP_LOGI(TAG, "Entity enumeration complete, result=%d", ctx.result);

    /* Send ListEntitiesDoneResponse */
    uint8_t output[ESPHOME_OUTPUT_BUFFER_SMALL];
    size_t output_len;

    esp_err_t ret = esphome_build_empty_message(ESPHOME_MSG_LIST_ENTITIES_DONE_RESPONSE,
                                                 output, sizeof(output), &output_len);
    if (ret == ESP_OK) {
        ret = esphome_api_send_message(client, output, output_len);
    }

    return (ctx.result == ESP_OK) ? ret : ctx.result;
}

/**
 * @brief Handle SubscribeStatesRequest message
 */
static esp_err_t handle_subscribe_states_request(esphome_client_t *client,
                                                   const esphome_message_t *msg)
{
    ESP_LOGI(TAG, "Received SubscribeStatesRequest");

    client->subscribed_states = true;

    /* Send current state of all entities */
    esphome_broadcast_states_ctx_t ctx = {
        .client = client,
    };

    esphome_entities_enumerate(esphome_api_broadcast_states_callback, &ctx);

    return ESP_OK;
}

/**
 * @brief Handle GetTimeRequest message
 */
static esp_err_t handle_get_time_request(esphome_client_t *client, const esphome_message_t *msg)
{
    ESP_LOGD(TAG, "Received GetTimeRequest");

    uint8_t payload[ESPHOME_PAYLOAD_SMALL];
    esphome_buffer_t buf;
    esphome_buffer_init(&buf, payload, sizeof(payload));

    /* Field 1: epoch_seconds (uint32) */
    time_t now;
    time(&now);
    esphome_encode_uint32(&buf, 1, (uint32_t)now);

    /* Build and send message */
    uint8_t output[ESPHOME_OUTPUT_BUFFER_STANDARD];
    size_t output_len;

    esp_err_t ret = esphome_build_message(ESPHOME_MSG_GET_TIME_RESPONSE, payload, buf.position,
                                          output, sizeof(output), &output_len);
    if (ret == ESP_OK) {
        ret = esphome_api_send_message(client, output, output_len);
    }

    return ret;
}

/* ============================================================================
 * Entity Command Handlers
 * ============================================================================ */

/**
 * @brief Publish ESPHome command received event
 *
 * Publishes an EVT_COMMAND_RECEIVED event for command tracking and potential
 * external handling via the event bus.
 */
static void publish_command_event(esphome_client_t *client, uint32_t key,
                                   esphome_cmd_type_t cmd_type, const char *json)
{
    uint8_t client_id = esphome_api_get_client_index(client);

    evt_esphome_command_t evt = {
        .entity_key = key,
        .cmd_type = (uint8_t)cmd_type,
        .client_id = client_id,
        .payload_json = json,
    };

    /* Best-effort publish - don't block command execution on event bus */
    event_publish(EVT_COMMAND_RECEIVED, &evt, sizeof(evt));
}

/**
 * @brief Handle SwitchCommand message
 */
static esp_err_t handle_switch_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    bool state = false;

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* state */
                esphome_decode_bool(&buf, &state);
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Switch command: key=%lu, state=%s", key, state ? "ON" : "OFF");

    /* Publish command event for tracking */
    char json_buf[32];
    snprintf(json_buf, sizeof(json_buf), "{\"state\":%s}", state ? "true" : "false");
    publish_command_event(client, key, ESPHOME_CMD_SWITCH, json_buf);

    return esphome_entity_execute_switch_command(key, state);
}

/**
 * @brief Handle NumberCommand message
 */
static esp_err_t handle_number_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    float value = 0.0f;

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* state (float) */
                esphome_decode_float(&buf, &value);
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Number command: key=%lu, value=%.2f", key, value);

    /* Publish command event for tracking */
    char json_buf[48];
    snprintf(json_buf, sizeof(json_buf), "{\"value\":%.4f}", value);
    publish_command_event(client, key, ESPHOME_CMD_NUMBER, json_buf);

    return esphome_entity_execute_number_command(key, value);
}

/**
 * @brief Handle ButtonCommand message
 */
static esp_err_t handle_button_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Button command: key=%lu", key);

    /* Publish command event for tracking */
    publish_command_event(client, key, ESPHOME_CMD_BUTTON, "{\"pressed\":true}");

    return esphome_entity_execute_button_command(key);
}

/**
 * @brief Handle SelectCommand message
 */
static esp_err_t handle_select_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    char state[ESPHOME_STRING_BUFFER_MEDIUM] = "";

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* state (string) */
                esphome_decode_string(&buf, state, sizeof(state));
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Select command: key=%lu, state=%s", key, state);

    /* Publish command event for tracking */
    char json_buf[128];
    snprintf(json_buf, sizeof(json_buf), "{\"option\":\"%s\"}", state);
    publish_command_event(client, key, ESPHOME_CMD_SELECT, json_buf);

    return esphome_entity_execute_select_command(key, state);
}

/**
 * @brief Handle LightCommand message
 */
static esp_err_t handle_light_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    esphome_light_command_t cmd = {0};

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* has_state */
                esphome_decode_bool(&buf, &cmd.has_state);
                break;
            case 3: /* state */
                esphome_decode_bool(&buf, &cmd.state);
                break;
            case 4: /* has_brightness */
                esphome_decode_bool(&buf, &cmd.has_brightness);
                break;
            case 5: /* brightness */
                esphome_decode_float(&buf, &cmd.brightness);
                break;
            case 6: /* has_color_mode */
                esphome_decode_bool(&buf, &cmd.has_color_mode);
                break;
            case 7: { /* color_mode */
                uint32_t color_mode;
                esphome_decode_uint32(&buf, &color_mode);
                cmd.color_mode = (esphome_color_mode_t)color_mode;
                break;
            }
            case 8: /* has_color_temp */
                esphome_decode_bool(&buf, &cmd.has_color_temp);
                break;
            case 9: /* color_temp */
                esphome_decode_float(&buf, &cmd.color_temp);
                break;
            case 10: /* has_rgb */
                esphome_decode_bool(&buf, &cmd.has_rgb);
                break;
            case 11: /* red */
                esphome_decode_float(&buf, &cmd.red);
                break;
            case 12: /* green */
                esphome_decode_float(&buf, &cmd.green);
                break;
            case 13: /* blue */
                esphome_decode_float(&buf, &cmd.blue);
                break;
            case 14: /* has_white */
                esphome_decode_bool(&buf, &cmd.has_white);
                break;
            case 15: /* white */
                esphome_decode_float(&buf, &cmd.white);
                break;
            case 16: /* has_effect */
                esphome_decode_bool(&buf, &cmd.has_effect);
                break;
            case 17: /* effect */
                esphome_decode_string(&buf, cmd.effect, sizeof(cmd.effect));
                break;
            case 18: /* has_transition_length */
                esphome_decode_bool(&buf, &cmd.has_transition_length);
                break;
            case 19: /* transition_length */
                esphome_decode_uint32(&buf, &cmd.transition_length);
                break;
            case 20: /* has_flash_length */
                esphome_decode_bool(&buf, &cmd.has_flash_length);
                break;
            case 21: /* flash_length */
                esphome_decode_uint32(&buf, &cmd.flash_length);
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Light command: key=%lu, state=%s, brightness=%.2f",
             key, cmd.has_state ? (cmd.state ? "ON" : "OFF") : "unchanged",
             cmd.has_brightness ? cmd.brightness : -1.0f);

    /* Publish command event for tracking */
    char json_buf[128];
    snprintf(json_buf, sizeof(json_buf),
             "{\"state\":%s,\"brightness\":%.2f}",
             cmd.has_state ? (cmd.state ? "true" : "false") : "null",
             cmd.has_brightness ? cmd.brightness : 0.0f);
    publish_command_event(client, key, ESPHOME_CMD_LIGHT, json_buf);

    return esphome_entity_execute_light_command(key, &cmd);
}

/**
 * @brief Handle CoverCommand message
 */
static esp_err_t handle_cover_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    esphome_cover_command_t cmd = {0};

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* has_position */
                esphome_decode_bool(&buf, &cmd.has_position);
                break;
            case 3: /* position */
                esphome_decode_float(&buf, &cmd.position);
                break;
            case 4: /* has_tilt */
                esphome_decode_bool(&buf, &cmd.has_tilt);
                break;
            case 5: /* tilt */
                esphome_decode_float(&buf, &cmd.tilt);
                break;
            case 6: /* stop */
                esphome_decode_bool(&buf, &cmd.stop);
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Cover command: key=%lu, position=%.2f, tilt=%.2f, stop=%s",
             key, cmd.has_position ? cmd.position : -1.0f,
             cmd.has_tilt ? cmd.tilt : -1.0f, cmd.stop ? "yes" : "no");

    /* Publish command event for tracking */
    char json_buf[96];
    snprintf(json_buf, sizeof(json_buf),
             "{\"position\":%.2f,\"tilt\":%.2f,\"stop\":%s}",
             cmd.has_position ? cmd.position : -1.0f,
             cmd.has_tilt ? cmd.tilt : -1.0f,
             cmd.stop ? "true" : "false");
    publish_command_event(client, key, ESPHOME_CMD_COVER, json_buf);

    return esphome_entity_execute_cover_command(key, &cmd);
}

/**
 * @brief Handle FanCommand message
 */
static esp_err_t handle_fan_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    esphome_fan_command_t cmd = {0};

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* has_state */
                esphome_decode_bool(&buf, &cmd.has_state);
                break;
            case 3: /* state */
                esphome_decode_bool(&buf, &cmd.state);
                break;
            case 4: /* has_speed_level */
                esphome_decode_bool(&buf, &cmd.has_speed_level);
                break;
            case 5: { /* speed_level */
                uint32_t speed;
                esphome_decode_uint32(&buf, &speed);
                cmd.speed_level = (int32_t)speed;
                break;
            }
            case 6: /* has_oscillating */
                esphome_decode_bool(&buf, &cmd.has_oscillating);
                break;
            case 7: /* oscillating */
                esphome_decode_bool(&buf, &cmd.oscillating);
                break;
            case 8: /* has_direction */
                esphome_decode_bool(&buf, &cmd.has_direction);
                break;
            case 9: { /* direction */
                uint32_t dir;
                esphome_decode_uint32(&buf, &dir);
                cmd.direction = (esphome_fan_direction_t)dir;
                break;
            }
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Fan command: key=%lu, state=%s, speed_level=%ld, oscillating=%s",
             key, cmd.has_state ? (cmd.state ? "ON" : "OFF") : "unchanged",
             cmd.has_speed_level ? cmd.speed_level : -1,
             cmd.has_oscillating ? (cmd.oscillating ? "yes" : "no") : "unchanged");

    /* Publish command event for tracking */
    char json_buf[96];
    snprintf(json_buf, sizeof(json_buf),
             "{\"state\":%s,\"speed_level\":%ld,\"oscillating\":%s}",
             cmd.has_state ? (cmd.state ? "true" : "false") : "null",
             cmd.has_speed_level ? cmd.speed_level : 0,
             cmd.has_oscillating ? (cmd.oscillating ? "true" : "false") : "null");
    publish_command_event(client, key, ESPHOME_CMD_FAN, json_buf);

    return esphome_entity_execute_fan_command(key, &cmd);
}

/**
 * @brief Handle ClimateCommand message
 */
static esp_err_t handle_climate_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    esphome_climate_command_t cmd = {0};

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* has_mode */
                esphome_decode_bool(&buf, &cmd.has_mode);
                break;
            case 3: { /* mode */
                uint32_t mode;
                esphome_decode_uint32(&buf, &mode);
                cmd.mode = (esphome_climate_mode_t)mode;
                break;
            }
            case 4: /* has_target_temperature */
                esphome_decode_bool(&buf, &cmd.has_target_temperature);
                break;
            case 5: /* target_temperature */
                esphome_decode_float(&buf, &cmd.target_temperature);
                break;
            case 6: /* has_target_temperature_low */
                esphome_decode_bool(&buf, &cmd.has_target_temperature_low);
                break;
            case 7: /* target_temperature_low */
                esphome_decode_float(&buf, &cmd.target_temperature_low);
                break;
            case 8: /* has_target_temperature_high */
                esphome_decode_bool(&buf, &cmd.has_target_temperature_high);
                break;
            case 9: /* target_temperature_high */
                esphome_decode_float(&buf, &cmd.target_temperature_high);
                break;
            case 10: /* has_fan_mode */
                esphome_decode_bool(&buf, &cmd.has_fan_mode);
                break;
            case 11: { /* fan_mode */
                uint32_t fan_mode;
                esphome_decode_uint32(&buf, &fan_mode);
                cmd.fan_mode = (esphome_climate_fan_mode_t)fan_mode;
                break;
            }
            case 12: /* has_swing_mode */
                esphome_decode_bool(&buf, &cmd.has_swing_mode);
                break;
            case 13: { /* swing_mode */
                uint32_t swing_mode;
                esphome_decode_uint32(&buf, &swing_mode);
                cmd.swing_mode = (esphome_climate_swing_mode_t)swing_mode;
                break;
            }
            case 14: /* has_preset */
                esphome_decode_bool(&buf, &cmd.has_preset);
                break;
            case 15: { /* preset */
                uint32_t preset;
                esphome_decode_uint32(&buf, &preset);
                cmd.preset = (esphome_climate_preset_t)preset;
                break;
            }
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Climate command: key=%lu, mode=%d, target_temp=%.1f",
             key, cmd.has_mode ? cmd.mode : -1,
             cmd.has_target_temperature ? cmd.target_temperature : -999.0f);

    /* Publish command event for tracking */
    char json_buf[96];
    snprintf(json_buf, sizeof(json_buf),
             "{\"mode\":%d,\"target_temperature\":%.1f}",
             cmd.has_mode ? cmd.mode : -1,
             cmd.has_target_temperature ? cmd.target_temperature : 0.0f);
    publish_command_event(client, key, ESPHOME_CMD_CLIMATE, json_buf);

    return esphome_entity_execute_climate_command(key, &cmd);
}

/**
 * @brief Handle LockCommand message
 */
static esp_err_t handle_lock_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    uint32_t command_val = 0;

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* command */
                esphome_decode_uint32(&buf, &command_val);
                break;
            case 3: /* code (optional, skip for now) */
                esphome_skip_field(&buf, wire_type);
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Lock command: key=%lu, cmd=%lu", key, command_val);

    /* Publish command event for tracking */
    char json_buf[48];
    snprintf(json_buf, sizeof(json_buf), "{\"command\":%lu}", command_val);
    publish_command_event(client, key, ESPHOME_CMD_LOCK, json_buf);

    return esphome_entity_execute_lock_command(key, (esphome_lock_command_t)command_val);
}

/**
 * @brief Handle TextCommand message
 */
static esp_err_t handle_text_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    char value[ESPHOME_MAX_STRING_LEN] = {0};

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* state */
                esphome_decode_string(&buf, value, sizeof(value));
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Text command: key=%lu, value='%s'", key, value);

    /* Publish command event for tracking */
    char json_buf[ESPHOME_MAX_STRING_LEN + 32];
    snprintf(json_buf, sizeof(json_buf), "{\"value\":\"%s\"}", value);
    publish_command_event(client, key, ESPHOME_CMD_TEXT, json_buf);

    return esphome_entity_execute_text_command(key, value);
}

/**
 * @brief Handle MediaPlayerCommand message
 */
static esp_err_t handle_media_player_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    esphome_media_player_cmd_t cmd = {0};

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* has_command */
                esphome_decode_bool(&buf, &cmd.has_command);
                break;
            case 3: { /* command */
                uint32_t cmd_val;
                esphome_decode_uint32(&buf, &cmd_val);
                cmd.command = (esphome_media_player_command_t)cmd_val;
                break;
            }
            case 4: /* has_volume */
                esphome_decode_bool(&buf, &cmd.has_volume);
                break;
            case 5: /* volume */
                esphome_decode_float(&buf, &cmd.volume);
                break;
            case 6: /* has_media_url */
                esphome_decode_bool(&buf, &cmd.has_media_url);
                break;
            case 7: /* media_url */
                esphome_decode_string(&buf, cmd.media_url, sizeof(cmd.media_url));
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Media player command: key=%lu, cmd=%d, vol=%.2f", key, cmd.command, cmd.volume);

    /* Publish command event for tracking */
    char json_buf[96];
    snprintf(json_buf, sizeof(json_buf),
             "{\"command\":%d,\"volume\":%.2f}", cmd.command, cmd.volume);
    publish_command_event(client, key, ESPHOME_CMD_MEDIA_PLAYER, json_buf);

    return esphome_entity_execute_media_player_command(key, &cmd);
}

/**
 * @brief Handle AlarmControlPanelCommand message
 */
static esp_err_t handle_alarm_command(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    uint32_t command_val = 0;
    char code[ESPHOME_STRING_BUFFER_SHORT] = {0};

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* command */
                esphome_decode_uint32(&buf, &command_val);
                break;
            case 3: /* code */
                esphome_decode_string(&buf, code, sizeof(code));
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Alarm command: key=%lu, cmd=%lu, has_code=%s",
             key, command_val, code[0] ? "yes" : "no");

    /* Publish command event for tracking */
    char json_buf[64];
    snprintf(json_buf, sizeof(json_buf),
             "{\"command\":%lu,\"has_code\":%s}", command_val, code[0] ? "true" : "false");
    publish_command_event(client, key, ESPHOME_CMD_ALARM, json_buf);

    return esphome_entity_execute_alarm_command(key, (esphome_alarm_command_t)command_val,
                                                 code[0] ? code : NULL);
}

/* ============================================================================
 * Service Handlers
 * ============================================================================ */

/**
 * @brief Service enumeration callback context
 */
typedef struct {
    esphome_client_t *client;
    esp_err_t result;
} list_services_ctx_t;

/**
 * @brief Callback for service enumeration in list services
 */
static bool list_services_enum_callback(const esphome_service_t *service, void *user_data)
{
    list_services_ctx_t *ctx = (list_services_ctx_t *)user_data;

    /* Allocate buffer on heap to reduce stack usage in callback */
    uint8_t *buffer = mem_alloc(ESPHOME_BUFFER_LARGE, MEM_CAP_PSRAM);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate service list buffer");
        ctx->result = ESP_ERR_NO_MEM;
        return false; /* Stop enumeration */
    }

    size_t buffer_len;

    esp_err_t ret = esphome_encode_service_list_entry(service, buffer, ESPHOME_BUFFER_LARGE, &buffer_len);
    if (ret == ESP_OK) {
        ret = esphome_api_send_message(ctx->client, buffer, buffer_len);
    }

    mem_ng_free(buffer);

    if (ret != ESP_OK) {
        ctx->result = ret;
        return false; /* Stop enumeration */
    }

    return true; /* Continue enumeration */
}

/**
 * @brief Handle ListServicesRequest message
 */
static esp_err_t handle_list_services_request(esphome_client_t *client, const esphome_message_t *msg)
{
    ESP_LOGI(TAG, "Received ListServicesRequest");

    /* Enumerate all services and send their info */
    list_services_ctx_t ctx = {
        .client = client,
        .result = ESP_OK,
    };

    esphome_services_enumerate(list_services_enum_callback, &ctx);

    return ctx.result;
}

/**
 * @brief Handle ExecuteService message
 */
static esp_err_t handle_execute_service(esphome_client_t *client, const esphome_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_buffer_t buf;
    esphome_buffer_init(&buf, msg->payload, msg->payload_len);

    uint32_t key = 0;
    esphome_service_arg_value_t args[ESPHOME_MAX_SERVICE_ARGS] = {0};
    size_t arg_count = 0;

    while (esphome_buffer_remaining(&buf) > 0) {
        uint32_t field_num;
        protobuf_wire_type_t wire_type;

        if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
            break;
        }

        switch (field_num) {
            case 1: /* key */
                esphome_decode_fixed32(&buf, &key);
                break;
            case 2: /* bool_arg */
                if (arg_count < ESPHOME_MAX_SERVICE_ARGS) {
                    args[arg_count].type = ESPHOME_SERVICE_ARG_BOOL;
                    esphome_decode_bool(&buf, &args[arg_count].bool_value);
                    arg_count++;
                }
                break;
            case 3: /* int_arg */
                if (arg_count < ESPHOME_MAX_SERVICE_ARGS) {
                    args[arg_count].type = ESPHOME_SERVICE_ARG_INT;
                    uint32_t int_val;
                    esphome_decode_uint32(&buf, &int_val);
                    args[arg_count].int_value = (int32_t)int_val;
                    arg_count++;
                }
                break;
            case 4: /* float_arg */
                if (arg_count < ESPHOME_MAX_SERVICE_ARGS) {
                    args[arg_count].type = ESPHOME_SERVICE_ARG_FLOAT;
                    esphome_decode_float(&buf, &args[arg_count].float_value);
                    arg_count++;
                }
                break;
            case 5: /* string_arg */
                if (arg_count < ESPHOME_MAX_SERVICE_ARGS) {
                    args[arg_count].type = ESPHOME_SERVICE_ARG_STRING;
                    esphome_decode_string(&buf, args[arg_count].string_value,
                                          sizeof(args[arg_count].string_value));
                    arg_count++;
                }
                break;
            default:
                esphome_skip_field(&buf, wire_type);
                break;
        }
    }

    ESP_LOGI(TAG, "Execute service: key=%lu, arg_count=%zu", key, arg_count);

    /* Publish command event for tracking */
    char json_buf[48];
    snprintf(json_buf, sizeof(json_buf), "{\"service_key\":%lu,\"arg_count\":%zu}", key, arg_count);
    publish_command_event(client, key, ESPHOME_CMD_SERVICE, json_buf);

    return esphome_service_execute(key, args, arg_count);
}

/* ============================================================================
 * Log and Home Assistant Handlers
 * ============================================================================ */

/**
 * @brief Handle SubscribeLogsRequest message
 */
static esp_err_t handle_subscribe_logs_request(esphome_client_t *client, const esphome_message_t *msg)
{
    ESP_LOGI(TAG, "Received SubscribeLogsRequest");

    esphome_log_level_t level = ESPHOME_LOG_LEVEL_DEBUG;

    /* Parse log level from payload if present */
    if (msg->payload && msg->payload_len > 0) {
        esphome_buffer_t buf;
        esphome_buffer_init(&buf, msg->payload, msg->payload_len);

        while (esphome_buffer_remaining(&buf) > 0) {
            uint32_t field_num;
            protobuf_wire_type_t wire_type;

            if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
                break;
            }

            if (field_num == 1 && wire_type == PROTOBUF_WIRE_VARINT) {
                uint32_t log_level;
                esphome_decode_uint32(&buf, &log_level);
                level = (esphome_log_level_t)log_level;
                ESP_LOGI(TAG, "Client subscribed to logs at level %d", level);
            } else {
                esphome_skip_field(&buf, wire_type);
            }
        }
    }

    client->subscribed_logs = true;
    client->log_level = level;

    ESP_LOGI(TAG, "Client subscribed to logs (level=%d)", level);
    return ESP_OK;
}

/**
 * @brief Handle SubscribeHomeAssistantStates message
 */
static esp_err_t handle_subscribe_ha_states(esphome_client_t *client, const esphome_message_t *msg)
{
    (void)msg;  /* Message has no relevant payload */
    ESP_LOGI(TAG, "Client subscribed to Home Assistant states");

    client->subscribed_ha_states = true;

    return ESP_OK;
}

/**
 * @brief Handle HomeAssistantStateResponse message
 */
static esp_err_t handle_ha_state_response(esphome_client_t *client, const esphome_message_t *msg)
{
    esphome_api_state_t *state = esphome_api_get_state();

    char entity_id[ESPHOME_STRING_BUFFER_MEDIUM] = {0};
    char entity_state[ESPHOME_STRING_BUFFER_LONG] = {0};
    char attribute[ESPHOME_STRING_BUFFER_MEDIUM] = {0};

    /* Parse message fields */
    if (msg->payload && msg->payload_len > 0) {
        esphome_buffer_t buf;
        esphome_buffer_init(&buf, msg->payload, msg->payload_len);

        while (esphome_buffer_remaining(&buf) > 0) {
            uint32_t field_num;
            protobuf_wire_type_t wire_type;

            if (esphome_decode_tag(&buf, &field_num, &wire_type) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to decode field header in HA state response");
                break;
            }

            switch (field_num) {
                case 1:  /* entity_id (string) */
                    if (wire_type == PROTOBUF_WIRE_LEN) {
                        esphome_decode_string(&buf, entity_id, sizeof(entity_id));
                    } else {
                        esphome_skip_field(&buf, wire_type);
                    }
                    break;

                case 2:  /* state (string) */
                    if (wire_type == PROTOBUF_WIRE_LEN) {
                        esphome_decode_string(&buf, entity_state, sizeof(entity_state));
                    } else {
                        esphome_skip_field(&buf, wire_type);
                    }
                    break;

                case 3:  /* attribute (string) */
                    if (wire_type == PROTOBUF_WIRE_LEN) {
                        esphome_decode_string(&buf, attribute, sizeof(attribute));
                    } else {
                        esphome_skip_field(&buf, wire_type);
                    }
                    break;

                default:
                    esphome_skip_field(&buf, wire_type);
                    break;
            }
        }
    }

    ESP_LOGD(TAG, "HA state update: entity=%s state=%s attr=%s",
             entity_id, entity_state, strlen(attribute) > 0 ? attribute : "(none)");

    /* Invoke callback if registered and we have a valid entity_id */
    if (state->ha_state_callback && strlen(entity_id) > 0) {
        state->ha_state_callback(entity_id, entity_state,
                                 strlen(attribute) > 0 ? attribute : NULL);
    }

    return ESP_OK;
}

/* ============================================================================
 * Main Message Dispatch
 * ============================================================================ */

/**
 * @brief Handle incoming client message
 */
esp_err_t esphome_api_handle_client_message(esphome_client_t *client, const esphome_message_t *msg)
{
    esphome_api_state_t *state = esphome_api_get_state();

    ESP_LOGD(TAG, "Handling message type: %d (%s)", msg->type, esphome_msg_type_name(msg->type));

    state->stats.messages_received++;
    client->last_activity = esphome_api_get_timestamp_ms();

    /* Check authentication for protected messages.
     * With Noise encryption, the PSK handshake serves as authentication -
     * skip password-based auth check entirely when Noise is established. */
    bool noise_authenticated = (client->noise_ctx != NULL &&
                                esphome_noise_is_ready(client->noise_ctx));
    bool requires_auth = (msg->type != ESPHOME_MSG_HELLO_REQUEST &&
                         msg->type != ESPHOME_MSG_CONNECT_REQUEST);

    if (requires_auth && !noise_authenticated &&
        client->state != ESPHOME_CLIENT_AUTHENTICATED) {
        ESP_LOGW(TAG, "Message type %d requires authentication", msg->type);
        return ESPHOME_ERR_AUTHENTICATION;
    }

    switch (msg->type) {
        case ESPHOME_MSG_HELLO_REQUEST:
            return handle_hello_request(client, msg);

        case ESPHOME_MSG_CONNECT_REQUEST:
            return handle_connect_request(client, msg);

        case ESPHOME_MSG_DISCONNECT_REQUEST:
            return handle_disconnect_request(client, msg);

        case ESPHOME_MSG_PING_REQUEST:
            return handle_ping_request(client, msg);

        case ESPHOME_MSG_PING_RESPONSE:
            return esphome_api_handle_ping_response(client);

        case ESPHOME_MSG_DEVICE_INFO_REQUEST:
            return handle_device_info_request(client, msg);

        case ESPHOME_MSG_LIST_ENTITIES_REQUEST:
            return handle_list_entities_request(client, msg);

        case ESPHOME_MSG_SUBSCRIBE_STATES_REQUEST:
            return handle_subscribe_states_request(client, msg);

        case ESPHOME_MSG_SWITCH_COMMAND:
            return handle_switch_command(client, msg);

        case ESPHOME_MSG_GET_TIME_REQUEST:
            return handle_get_time_request(client, msg);

        case ESPHOME_MSG_SUBSCRIBE_LOGS_REQUEST:
            return handle_subscribe_logs_request(client, msg);

        /* Number entity command */
        case ESPHOME_MSG_NUMBER_COMMAND:
            return handle_number_command(client, msg);

        /* Button entity command */
        case ESPHOME_MSG_BUTTON_COMMAND:
            return handle_button_command(client, msg);

        /* Select entity command */
        case ESPHOME_MSG_SELECT_COMMAND:
            return handle_select_command(client, msg);

        /* Light entity command */
        case ESPHOME_MSG_LIGHT_COMMAND:
            return handle_light_command(client, msg);

        /* Cover entity command */
        case ESPHOME_MSG_COVER_COMMAND:
            return handle_cover_command(client, msg);

        /* Fan entity command */
        case ESPHOME_MSG_FAN_COMMAND:
            return handle_fan_command(client, msg);

        /* Climate entity command */
        case ESPHOME_MSG_CLIMATE_COMMAND:
            return handle_climate_command(client, msg);

        /* Lock entity command */
        case ESPHOME_MSG_LOCK_COMMAND:
            return handle_lock_command(client, msg);

        /* Text entity command */
        case ESPHOME_MSG_TEXT_COMMAND:
            return handle_text_command(client, msg);

        /* Media player entity command */
        case ESPHOME_MSG_MEDIA_PLAYER_COMMAND:
            return handle_media_player_command(client, msg);

        /* Alarm control panel entity command */
        case ESPHOME_MSG_ALARM_PANEL_COMMAND:
            return handle_alarm_command(client, msg);

        /* Service calls */
        case ESPHOME_MSG_LIST_SERVICES_REQUEST:
            return handle_list_services_request(client, msg);

        case ESPHOME_MSG_EXECUTE_SERVICE:
            return handle_execute_service(client, msg);

        /* Home Assistant integration */
        case ESPHOME_MSG_SUBSCRIBE_HA_SERVICES:
            /* Client wants to know about HA service calls — we don't make any */
            ESP_LOGD(TAG, "Received SubscribeHomeassistantServicesRequest (no-op)");
            return ESP_OK;

        case ESPHOME_MSG_SUBSCRIBE_HOME_ASSISTANT_STATES:
            return handle_subscribe_ha_states(client, msg);

        case ESPHOME_MSG_HOME_ASSISTANT_STATE_RESPONSE:
            return handle_ha_state_response(client, msg);

        /* BLE Proxy messages (ES-007) */
        case ESPHOME_MSG_SUBSCRIBE_BLE_ADVERTISEMENTS: {
            uint8_t cid = esphome_api_get_client_index(client);
            esp_err_t ble_ret = esphome_ble_proxy_handle_subscribe_advertisements(
                cid, msg->payload, msg->payload_len);
            /* Send scanner state response after subscription */
            if (ble_ret == ESP_OK) {
                esphome_ble_proxy_send_scanner_state(cid);
            }
            return ble_ret;
        }

        case ESPHOME_MSG_UNSUBSCRIBE_BLE_ADVERTISEMENTS:
            return esphome_ble_proxy_handle_unsubscribe_advertisements(
                esphome_api_get_client_index(client));

        case ESPHOME_MSG_BLE_DEVICE_REQUEST:
            return esphome_ble_proxy_handle_device_request(
                esphome_api_get_client_index(client), msg->payload, msg->payload_len);

        case ESPHOME_MSG_SUBSCRIBE_BLE_CONNECTIONS_FREE:
            return esphome_ble_proxy_handle_connection_free(
                esphome_api_get_client_index(client), msg->payload, msg->payload_len);

        case ESPHOME_MSG_BLE_GATT_READ_REQUEST:
            return esphome_ble_proxy_handle_gatt_read(
                esphome_api_get_client_index(client), msg->payload, msg->payload_len);

        case ESPHOME_MSG_BLE_GATT_WRITE_REQUEST:
            return esphome_ble_proxy_handle_gatt_write(
                esphome_api_get_client_index(client), msg->payload, msg->payload_len);

        case ESPHOME_MSG_BLE_GATT_NOTIFY_REQUEST:
            return esphome_ble_proxy_handle_gatt_notify_request(
                esphome_api_get_client_index(client), msg->payload, msg->payload_len);

        case ESPHOME_MSG_BLE_SCANNER_SET_MODE_REQUEST:
            return esphome_ble_proxy_handle_scanner_set_mode(
                esphome_api_get_client_index(client), msg->payload, msg->payload_len);

        default:
            ESP_LOGW(TAG, "Unhandled message type: %d", msg->type);
            return ESP_OK;
    }
}
