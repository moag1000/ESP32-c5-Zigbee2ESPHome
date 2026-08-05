/**
 * @file esphome_api_server.c
 * @brief ESPHome API TCP Server and Client Management
 *
 * Handles TCP server setup/teardown, socket accept loop, client connection
 * management, and client task handling. This module is responsible for all
 * network-level operations of the ESPHome API.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_api_internal.h"
#include "esphome_protocol.h"
#include "esphome_ble_proxy.h"
#include "core/memory/memory_manager_ng.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_mac.h"

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
#include "esphome_noise.h"
#endif

/* Log tag */
static const char *TAG = "ESPHOME_SRV";

/* ============================================================================
 * Send Functions
 * ============================================================================ */

/**
 * @brief Send raw bytes to client (internal helper)
 */
esp_err_t esphome_api_send_raw_bytes(esphome_client_t *client, const uint8_t *data, size_t len)
{
    size_t total_sent = 0;
    int retry_count = 0;
    const int max_retries = 5;

    while (total_sent < len && retry_count < max_retries) {
        ssize_t sent = send(client->socket, data + total_sent, len - total_sent, 0);

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                retry_count++;
                ESP_LOGD(TAG, "Send would block, retry %d/%d", retry_count, max_retries);
                vTaskDelay(ESPHOME_DELAY_MINIMAL_TICKS);
                continue;
            }
            ESP_LOGE(TAG, "Send failed: errno=%d", errno);
            client->state = ESPHOME_CLIENT_DISCONNECTED;
            return ESP_FAIL;
        }

        if (sent == 0) {
            ESP_LOGW(TAG, "Connection closed during send");
            client->state = ESPHOME_CLIENT_DISCONNECTED;
            return ESP_FAIL;
        }

        total_sent += sent;
        retry_count = 0;
    }

    if (total_sent != len) {
        ESP_LOGE(TAG, "Failed to send: %zu/%zu bytes", total_sent, len);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Send message to client with retry handling for partial sends
 *
 * Handles EAGAIN/EWOULDBLOCK by retrying, and properly handles partial sends
 * by continuing until all data is sent or max retries exceeded.
 *
 * When Noise encryption is enabled and active, this function encrypts the
 * message before sending. The input data should be a complete ESPHome frame
 * (0x00 + length varint + type varint + payload).
 */
esp_err_t esphome_api_send_message(esphome_client_t *client, const uint8_t *data, size_t len)
{
    if (!client || client->socket < 0 || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esphome_api_state_t *state = esphome_api_get_state();

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
    /* If encryption is enabled, encrypt the message */
    if (client->encryption_enabled && client->noise_ctx &&
        esphome_noise_is_ready(client->noise_ctx)) {

        /* Parse the plaintext ESPHome frame to extract msg_type and payload */
        esphome_message_t msg;
        size_t consumed;
        esp_err_t ret = esphome_decode_frame(data, len, &msg, &consumed);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to parse frame for encryption");
            return ret;
        }

        /* Encrypt and send - use heap to avoid large stack allocation */
        size_t encrypted_buf_size = ESPHOME_MAX_MESSAGE_SIZE + ESPHOME_NOISE_FRAME_OVERHEAD;
        uint8_t *encrypted = mem_alloc(encrypted_buf_size, MEM_CAP_PSRAM);
        if (!encrypted) {
            ESP_LOGE(TAG, "Failed to allocate encryption buffer");
            return ESP_ERR_NO_MEM;
        }
        size_t encrypted_len;

        ret = esphome_noise_encrypt(client->noise_ctx, msg.type,
                                     msg.payload, msg.payload_len,
                                     encrypted, encrypted_buf_size, &encrypted_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Encryption failed: %s", esp_err_to_name(ret));
            mem_ng_free(encrypted);
            return ret;
        }

        ret = esphome_api_send_raw_bytes(client, encrypted, encrypted_len);
        if (ret == ESP_OK) {
            /* Use trylock (timeout=0) for stats — this function is called from
             * esphome_api_broadcast_state() which already holds state->mutex.
             * A non-zero timeout on the same non-recursive mutex triggers
             * vTaskPriorityDisinheritAfterTimeout assert on single-core. */
            if (xSemaphoreTake(state->mutex, 0) == pdTRUE) {
                state->stats.messages_sent++;
                state->stats.bytes_sent += encrypted_len;
                xSemaphoreGive(state->mutex);
            }
        }
        mem_ng_free(encrypted);
        return ret;
    }
#endif

    /* Send plaintext */
    esp_err_t ret = esphome_api_send_raw_bytes(client, data, len);

    if (ret == ESP_OK) {
        /* Use trylock (timeout=0) — see encrypted path comment above */
        if (xSemaphoreTake(state->mutex, 0) == pdTRUE) {
            state->stats.messages_sent++;
            state->stats.bytes_sent += len;
            xSemaphoreGive(state->mutex);
        }
    }

    return ret;
}

/**
 * @brief Send ping request to client
 */
esp_err_t esphome_api_send_ping_request(esphome_client_t *client)
{
    uint8_t output[ESPHOME_OUTPUT_BUFFER_SMALL];
    size_t output_len;

    esp_err_t ret = esphome_build_empty_message(ESPHOME_MSG_PING_REQUEST,
                                                 output, sizeof(output), &output_len);
    if (ret != ESP_OK) {
        return ret;
    }

    return esphome_api_send_message(client, output, output_len);
}

/**
 * @brief Check and handle client keepalive
 *
 * Returns true if client should be disconnected due to keepalive timeout.
 */
bool esphome_api_check_client_keepalive(esphome_client_t *client)
{
    esphome_api_state_t *state = esphome_api_get_state();
    uint32_t now = esphome_api_get_timestamp_ms();
    uint32_t keepalive_ms = state->config.keepalive_ms;

    /* Skip if keepalive disabled or client not authenticated */
    if (keepalive_ms == 0 || client->state != ESPHOME_CLIENT_AUTHENTICATED) {
        return false;
    }

    /* Check for ping timeout (waiting for pong) */
    if (client->ping_pending) {
        uint32_t elapsed = now - client->last_ping_sent;
        if (elapsed > keepalive_ms * 2) {
            ESP_LOGW(TAG, "Client keepalive timeout - no pong received");
            return true;  /* Disconnect */
        }
        return false;  /* Still waiting */
    }

    /* Send ping if idle too long */
    uint32_t idle_time = now - client->last_activity;
    if (idle_time > keepalive_ms) {
        ESP_LOGD(TAG, "Sending keepalive ping");
        if (esphome_api_send_ping_request(client) == ESP_OK) {
            client->last_ping_sent = now;
            client->ping_pending = true;
        }
    }

    return false;
}

/* ============================================================================
 * Noise Encryption Handshake
 * ============================================================================ */

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
/**
 * @brief Handle Noise protocol handshake for a client
 *
 * @return ESP_OK if handshake successful, error otherwise
 */
esp_err_t esphome_api_handle_noise_handshake(esphome_client_t *client, uint8_t client_id)
{
    esphome_api_state_t *state = esphome_api_get_state();

    char mac[13];
    uint8_t mac_bytes[6];
    esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
    snprintf(mac, sizeof(mac), "%02X%02X%02X%02X%02X%02X",
             mac_bytes[0], mac_bytes[1], mac_bytes[2],
             mac_bytes[3], mac_bytes[4], mac_bytes[5]);

    /* Create Noise context */
#ifdef CONFIG_ESPHOME_NOISE_PSK
    const char *psk = CONFIG_ESPHOME_NOISE_PSK;
#else
    const char *psk = NULL;
#endif

#ifdef CONFIG_ESPHOME_NOISE_REQUIRE_PSK
    /* Check that PSK is configured when required */
    if (psk == NULL || strlen(psk) == 0) {
        ESP_LOGE(TAG, "PSK required but not configured");
        return ESP_ERR_INVALID_STATE;
    }
#endif

    client->noise_ctx = esphome_noise_create(psk, state->config.device_name, mac);
    if (!client->noise_ctx) {
        ESP_LOGE(TAG, "Failed to create Noise context");
        return ESP_ERR_NO_MEM;
    }

    /* === Phase 1: Read and process Noise Hello ===
     * All Noise frames use: 0x01 + 2-byte BE length + payload
     * Client sends: HelloRequest (payload may be empty)
     * We respond:   ServerHello (0x01 proto + name\0 + mac\0) */

    /* Read Hello frame header */
    client->rx_buffer_len = 0;
    while (client->rx_buffer_len < ESPHOME_NOISE_FRAME_HEADER_SIZE) {
        ssize_t received = recv(client->socket,
                                &client->rx_buffer[client->rx_buffer_len],
                                sizeof(client->rx_buffer) - client->rx_buffer_len, 0);
        if (received <= 0) {
            ESP_LOGE(TAG, "Failed to receive Hello header");
            return ESP_FAIL;
        }
        client->rx_buffer_len += received;
    }

    /* Verify Noise frame indicator */
    if (client->rx_buffer[0] != ESPHOME_FRAME_NOISE) {
        ESP_LOGW(TAG, "Bad Hello indicator: 0x%02x (expected 0x%02x)",
                 client->rx_buffer[0], ESPHOME_FRAME_NOISE);
        return ESP_FAIL;
    }

    /* Parse 2-byte big-endian payload length */
    size_t hello_payload_len = (client->rx_buffer[1] << 8) | client->rx_buffer[2];

    /* Bound it before it is used as a receive target.
     *
     * This length is attacker-controlled and 16 bits wide, so it reaches 65535
     * while rx_buffer is ESPHOME_RX_BUFFER_SIZE (1024). Without this check the
     * read loop below hands recv() a count derived from it and writes straight
     * past the buffer, over rx_buffer_len and the noise_ctx pointer that follow
     * it in esphome_client_t. Hello is the first message of the connection, so
     * that was reachable before any authentication — the PSK is not consulted
     * until the handshake. The handshake path a few lines down has always had
     * the equivalent check; this one did not. */
    if (hello_payload_len > sizeof(client->rx_buffer) - ESPHOME_NOISE_FRAME_HEADER_SIZE) {
        ESP_LOGE(TAG, "Hello message too large: %zu", hello_payload_len);
        return ESP_ERR_NO_MEM;
    }

    size_t hello_total = ESPHOME_NOISE_FRAME_HEADER_SIZE + hello_payload_len;

    ESP_LOGD(TAG, "Hello frame: payload=%zu, total=%zu", hello_payload_len, hello_total);

    /* Read remaining Hello payload if any */
    while (client->rx_buffer_len < hello_total) {
        ssize_t received = recv(client->socket,
                                &client->rx_buffer[client->rx_buffer_len],
                                hello_total - client->rx_buffer_len, 0);
        if (received <= 0) {
            ESP_LOGE(TAG, "Failed to receive Hello payload");
            return ESP_FAIL;
        }
        client->rx_buffer_len += received;
    }

    /* Process Hello */
    uint8_t hello_response[128];
    size_t hello_resp_len;
    esp_err_t ret = esphome_noise_process_hello(client->noise_ctx,
                                                 client->rx_buffer + ESPHOME_NOISE_FRAME_HEADER_SIZE,
                                                 hello_payload_len,
                                                 hello_response + ESPHOME_NOISE_FRAME_HEADER_SIZE,
                                                 sizeof(hello_response) - ESPHOME_NOISE_FRAME_HEADER_SIZE,
                                                 &hello_resp_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Noise Hello failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Send Hello response with Noise framing */
    hello_response[0] = ESPHOME_FRAME_NOISE;
    hello_response[1] = (hello_resp_len >> 8) & 0xFF;
    hello_response[2] = hello_resp_len & 0xFF;
    ret = esphome_api_send_raw_bytes(client, hello_response,
                                      ESPHOME_NOISE_FRAME_HEADER_SIZE + hello_resp_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send Hello response");
        return ret;
    }

    /* Preserve any leftover bytes after Hello frame (client sends Hello +
     * Handshake together before waiting for ServerHello response) */
    if (client->rx_buffer_len > hello_total) {
        size_t leftover = client->rx_buffer_len - hello_total;
        memmove(client->rx_buffer, client->rx_buffer + hello_total, leftover);
        client->rx_buffer_len = leftover;
        ESP_LOGD(TAG, "Preserved %zu leftover bytes after Hello", leftover);
    } else {
        client->rx_buffer_len = 0;
    }
    ESP_LOGI(TAG, "Client %d: Noise Hello complete, reading handshake (%zu bytes buffered)",
             client_id, client->rx_buffer_len);

    /* === Phase 2: Read and process Noise Handshake ===
     * Client sends: 0x01 + 2-byte BE length + payload
     * We respond:   0x01 + 2-byte BE length + payload
     * Note: The handshake frame may already be (partially) in the buffer
     * since aioesphomeapi sends Hello + Handshake together at connection time. */

    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /* Read handshake frame header (may already be buffered) */
    while (client->rx_buffer_len < ESPHOME_NOISE_FRAME_HEADER_SIZE) {
        ssize_t received = recv(client->socket,
                                &client->rx_buffer[client->rx_buffer_len],
                                sizeof(client->rx_buffer) - client->rx_buffer_len, 0);
        if (received <= 0) {
            ESP_LOGE(TAG, "Failed to receive handshake header");
            return ESP_FAIL;
        }
        client->rx_buffer_len += received;
    }

    /* Verify Noise frame indicator */
    if (client->rx_buffer[0] != ESPHOME_FRAME_NOISE) {
        ESP_LOGW(TAG, "Bad handshake indicator: 0x%02x (expected 0x%02x)",
                 client->rx_buffer[0], ESPHOME_FRAME_NOISE);
        return ESP_FAIL;
    }

    /* Parse 2-byte big-endian length */
    size_t handshake_len = (client->rx_buffer[1] << 8) | client->rx_buffer[2];
    ESP_LOGD(TAG, "Handshake frame length: %zu", handshake_len);

    if (handshake_len > sizeof(client->rx_buffer) - ESPHOME_NOISE_FRAME_HEADER_SIZE) {
        ESP_LOGE(TAG, "Handshake message too large");
        return ESP_ERR_NO_MEM;
    }

    /* Read rest of handshake */
    size_t needed = ESPHOME_NOISE_FRAME_HEADER_SIZE + handshake_len;
    while (client->rx_buffer_len < needed) {
        ssize_t received = recv(client->socket,
                                &client->rx_buffer[client->rx_buffer_len],
                                needed - client->rx_buffer_len, 0);
        if (received <= 0) {
            ESP_LOGE(TAG, "Failed to receive handshake data");
            return ESP_FAIL;
        }
        client->rx_buffer_len += received;
    }

    /* Process handshake (skip header) */
    uint8_t handshake_response[128];
    size_t response_len;
    ret = esphome_noise_process_handshake(client->noise_ctx,
                                           client->rx_buffer + ESPHOME_NOISE_FRAME_HEADER_SIZE,
                                           handshake_len,
                                           handshake_response + ESPHOME_NOISE_FRAME_HEADER_SIZE,
                                           sizeof(handshake_response) - ESPHOME_NOISE_FRAME_HEADER_SIZE,
                                           &response_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Noise handshake failed: %s", esp_err_to_name(ret));
        if (response_len > 0) {
            /* Send error with Noise framing */
            handshake_response[0] = ESPHOME_FRAME_NOISE;
            handshake_response[1] = (response_len >> 8) & 0xFF;
            handshake_response[2] = response_len & 0xFF;
            esphome_api_send_raw_bytes(client, handshake_response,
                                        ESPHOME_NOISE_FRAME_HEADER_SIZE + response_len);
        }
        return ret;
    }

    /* Send handshake response with Noise framing */
    handshake_response[0] = ESPHOME_FRAME_NOISE;
    handshake_response[1] = (response_len >> 8) & 0xFF;
    handshake_response[2] = response_len & 0xFF;
    ret = esphome_api_send_raw_bytes(client, handshake_response,
                                      ESPHOME_NOISE_FRAME_HEADER_SIZE + response_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send handshake response");
        return ret;
    }

    /* Clear buffer and mark encryption as active */
    client->rx_buffer_len = 0;
    client->encryption_enabled = true;

    ESP_LOGI(TAG, "Client %d: Noise handshake complete - encryption active", client_id);
    return ESP_OK;
}
#endif /* CONFIG_ESPHOME_NOISE_ENCRYPTION */

/* ============================================================================
 * Client Task
 * ============================================================================ */

/**
 * @brief Client handler task
 */
void esphome_api_client_task(void *pvParameters)
{
    uint8_t client_id = (uint8_t)(uintptr_t)pvParameters;
    esphome_api_state_t *state = esphome_api_get_state();
    esphome_client_t *client = &state->clients[client_id];

    ESP_LOGI(TAG, "Client %d handler started", client_id);

    /* Set socket timeout */
    struct timeval timeout = {
        .tv_sec = ESPHOME_SOCKET_TIMEOUT_MS / 1000,
        .tv_usec = (ESPHOME_SOCKET_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
    /* Protocol detection: peek at first byte to determine Noise vs plaintext.
     * Noise frames always use 0x01 indicator. Plaintext uses 0x00. */
    ssize_t first_byte = recv(client->socket, client->rx_buffer, 1, MSG_PEEK);
    if (first_byte == 1) {
        bool is_noise = (client->rx_buffer[0] == ESPHOME_FRAME_NOISE);
        if (is_noise) {
            ESP_LOGI(TAG, "Client %d using Noise protocol (first byte: 0x%02x)",
                     client_id, client->rx_buffer[0]);

            /* Handle Noise handshake */
            esp_err_t ret = esphome_api_handle_noise_handshake(client, client_id);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Client %d Noise handshake failed", client_id);
                goto disconnect;
            }
        }
#ifndef CONFIG_ESPHOME_ALLOW_PLAINTEXT
        else {
            /* Client is using plaintext but encryption is required.
             * We need to read the client's HelloRequest first, then respond
             * with 0x01 to signal encryption is required. This gives the
             * client a chance to properly receive our response. */
            ESP_LOGI(TAG, "Client %d using plaintext - will respond with encryption_required",
                     client_id);
            client->encryption_enabled = false;
            client->require_encryption_disconnect = true;
        }
#endif
    }
#endif /* CONFIG_ESPHOME_NOISE_ENCRYPTION */

    while (client->socket >= 0 && state->running) {
        /* Receive data */
        ssize_t received = recv(client->socket,
                               &client->rx_buffer[client->rx_buffer_len],
                               sizeof(client->rx_buffer) - client->rx_buffer_len, 0);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout - check for keepalive first */
                if (esphome_api_check_client_keepalive(client)) {
                    ESP_LOGW(TAG, "Client %d keepalive timeout", client_id);
                    break;
                }

                /* Then check for idle disconnect */
                uint32_t idle_time = esphome_api_get_timestamp_ms() - client->last_activity;
                if (idle_time > ESPHOME_MAX_IDLE_TIME * 1000) {
                    ESP_LOGW(TAG, "Client %d idle timeout", client_id);
                    break;
                }
                continue;
            }
            ESP_LOGE(TAG, "Client %d recv error: errno=%d", client_id, errno);
            break;
        }

        if (received == 0) {
            ESP_LOGI(TAG, "┌─ ESPHome Client Disconnected ──────────");
            ESP_LOGI(TAG, "│ Client: %s", client->client_info[0] ? client->client_info : "(unknown)");
#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
            ESP_LOGI(TAG, "│ Encrypted: %s", client->encryption_enabled ? "yes" : "no");
#endif
            ESP_LOGI(TAG, "└────────────────────────────────────────");
            break;
        }

        state->stats.bytes_received += received;
        client->rx_buffer_len += received;

#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
        /* Handle encrypted messages */
        if (client->encryption_enabled && client->noise_ctx &&
            esphome_noise_is_ready(client->noise_ctx)) {

            /* Allocate plaintext buffer on heap to reduce stack usage (4KB) */
            uint8_t *plaintext = mem_alloc(ESPHOME_MAX_MESSAGE_SIZE, MEM_CAP_PSRAM);
            if (plaintext == NULL) {
                ESP_LOGE(TAG, "Failed to allocate decryption buffer");
                state->stats.protocol_errors++;
                goto disconnect;
            }

            /* Encrypted frame: [0x01 indicator][2B length BE][ciphertext + MAC] */
            while (client->rx_buffer_len >= ESPHOME_NOISE_FRAME_HEADER_SIZE) {
                /* Verify Noise frame indicator */
                if (client->rx_buffer[0] != ESPHOME_FRAME_NOISE) {
                    ESP_LOGW(TAG, "Expected Noise frame (0x%02x), got 0x%02x",
                             ESPHOME_FRAME_NOISE, client->rx_buffer[0]);
                    state->stats.protocol_errors++;
                    mem_ng_free(plaintext);
                    goto disconnect;
                }

                uint16_t frame_len = (client->rx_buffer[1] << 8) | client->rx_buffer[2];
                size_t total_len = ESPHOME_NOISE_FRAME_HEADER_SIZE + frame_len;

                if (client->rx_buffer_len < total_len) {
                    /* Need more data */
                    break;
                }

                /* Decrypt the frame (skip indicator, pass length + ciphertext) */
                uint16_t msg_type;
                size_t plaintext_len;

                esp_err_t ret = esphome_noise_decrypt(client->noise_ctx,
                                                       client->rx_buffer + ESPHOME_NOISE_FRAME_HEADER_SIZE,
                                                       frame_len,
                                                       &msg_type,
                                                       plaintext, ESPHOME_MAX_MESSAGE_SIZE,
                                                       &plaintext_len);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Decryption failed: %s", esp_err_to_name(ret));
                    state->stats.protocol_errors++;
                    mem_ng_free(plaintext);
                    goto disconnect;
                }

                /* Create message structure */
                esphome_message_t msg = {
                    .type = (esphome_msg_type_t)msg_type,
                    .payload = plaintext,
                    .payload_len = plaintext_len,
                    .length = plaintext_len,
                };

                /* Handle message */
                ret = esphome_api_handle_client_message(client, &msg);
                if (ret == ESPHOME_ERR_NOT_CONNECTED) {
                    mem_ng_free(plaintext);
                    goto disconnect;
                }

                /* Remove processed data from buffer */
                if (total_len < client->rx_buffer_len) {
                    memmove(client->rx_buffer, &client->rx_buffer[total_len],
                            client->rx_buffer_len - total_len);
                }
                client->rx_buffer_len -= total_len;
            }

            mem_ng_free(plaintext);
        } else
#endif /* CONFIG_ESPHOME_NOISE_ENCRYPTION */
        {
            /* Process plaintext messages */
            while (client->rx_buffer_len > 0) {
                esphome_message_t msg;
                size_t consumed;

                esp_err_t ret = esphome_decode_frame(client->rx_buffer, client->rx_buffer_len,
                                                     &msg, &consumed);

                if (ret == ESP_ERR_INVALID_SIZE) {
                    /* Incomplete message, need more data */
                    break;
                }

                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Protocol error: %s", esp_err_to_name(ret));
                    state->stats.protocol_errors++;
                    /* Try to recover by discarding first byte */
                    memmove(client->rx_buffer, &client->rx_buffer[1], client->rx_buffer_len - 1);
                    client->rx_buffer_len--;
                    continue;
                }

                /* Handle message */
                ret = esphome_api_handle_client_message(client, &msg);
                if (ret == ESPHOME_ERR_NOT_CONNECTED) {
                    /* Client requested disconnect */
                    goto disconnect;
                }

                /* Remove processed data from buffer */
                if (consumed < client->rx_buffer_len) {
                    memmove(client->rx_buffer, &client->rx_buffer[consumed],
                           client->rx_buffer_len - consumed);
                }
                client->rx_buffer_len -= consumed;
            }
        }

        /* Prevent buffer overflow */
        if (client->rx_buffer_len >= sizeof(client->rx_buffer)) {
            ESP_LOGE(TAG, "Client %d buffer overflow", client_id);
            state->stats.protocol_errors++;
            client->rx_buffer_len = 0;
        }
    }

disconnect:
    if (xSemaphoreTake(state->mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) == pdTRUE) {
        esphome_api_close_client(client, client_id);
        xSemaphoreGive(state->mutex);
    }

    ESP_LOGI(TAG, "Client %d handler exiting", client_id);
    /* Mark task as self-deleted - PSRAM resources freed when slot is reused */
    psram_task_mark_deleted(&client->psram_task);
    vTaskDelete(NULL);
}

/* ============================================================================
 * Server Task
 * ============================================================================ */

/**
 * @brief Server accept task
 */
void esphome_api_server_task(void *pvParameters)
{
    esphome_api_state_t *state = esphome_api_get_state();

    ESP_LOGI(TAG, "Server task started on port %d", state->config.port);

    while (state->running) {
        /* Check for stop event */
        EventBits_t bits = xEventGroupWaitBits(state->event_group, EVENT_STOP_SERVER,
                                                pdTRUE, pdFALSE, 0);
        if (bits & EVENT_STOP_SERVER) {
            break;
        }

        /* Accept new connection */
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_socket = accept(state->server_socket, (struct sockaddr *)&client_addr,
                                   &addr_len);

        if (client_socket < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(ESPHOME_MUTEX_TIMEOUT_TICKS);
                continue;
            }
            if (state->running) {
                ESP_LOGE(TAG, "Accept failed: errno=%d", errno);
            }
            continue;
        }

        ESP_LOGI(TAG, "New connection from %s:%d",
                inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        if (xSemaphoreTake(state->mutex, ESPHOME_MUTEX_TIMEOUT_EXTENDED_TICKS) != pdTRUE) {
            close(client_socket);
            continue;
        }

        /* Find free client slot */
        int slot = esphome_api_find_free_client_slot();
        if (slot < 0) {
            ESP_LOGW(TAG, "Max clients reached, rejecting connection");
            close(client_socket);
            xSemaphoreGive(state->mutex);
            continue;
        }

        /* Initialize client */
        esphome_client_t *client = &state->clients[slot];
        client->socket = client_socket;
        client->state = ESPHOME_CLIENT_CONNECTED;
        client->subscribed_states = false;
        client->subscribed_logs = false;
        client->last_activity = esphome_api_get_timestamp_ms();
        client->rx_buffer_len = 0;
        memset(client->client_info, 0, sizeof(client->client_info));
#ifdef CONFIG_ESPHOME_NOISE_ENCRYPTION
        client->noise_ctx = NULL;
        client->encryption_enabled = false;
#endif

        state->stats.total_connections++;
        state->stats.active_connections = esphome_api_count_active_clients();

        /* Free any leftover PSRAM resources from previous client on this slot */
        if (client->psram_task.stack != NULL) {
            psram_task_delete(&client->psram_task);
        }

        /* Create client handler task with PSRAM stack (saves ~4KB internal RAM per client) */
        char task_name[ESPHOME_TASK_NAME_SIZE];
        snprintf(task_name, sizeof(task_name), "esph_cli%u", (unsigned)slot & 0xFFU);

        esp_err_t ret = psram_task_create(esphome_api_client_task, task_name,
                                           ESPHOME_CLIENT_TASK_STACK,
                                           (void *)(uintptr_t)slot, ESPHOME_CLIENT_TASK_PRIO,
                                           &client->psram_task);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create client task: %s", esp_err_to_name(ret));
            esphome_api_close_client(client, slot);
        } else {
            /* Notify callback */
            if (state->connection_callback) {
                state->connection_callback(slot, true, false);
            }
        }

        xSemaphoreGive(state->mutex);
    }

    ESP_LOGI(TAG, "Server task exiting");

    /* Mark PSRAM task as self-deleted before vTaskDelete */
    psram_task_mark_deleted(&state->server_psram_task);
    vTaskDelete(NULL);
}
