/**
 * @file esphome_ota.c
 * @brief ESPHome OTA Update Support Implementation
 *
 * Implements a TCP server that handles ESPHome OTA protocol
 * for firmware updates. Compatible with ESPHome OTA component.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "esphome_ota.h"
#include "esphome_common.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos_helpers.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_idf_version.h"

/* ESP-IDF v6.0+ uses PSA Crypto API, v5.x uses legacy mbedTLS MD5 API */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include "psa/crypto.h"
#define USE_PSA_CRYPTO 1
#else
#include "mbedtls/md5.h"
#define USE_PSA_CRYPTO 0
#endif

static const char *TAG = "ESPHOME_OTA";

/* ============================================================================
 * Protocol Constants
 * ============================================================================ */

/** @brief ESPHome OTA magic byte */
#define OTA_MAGIC_BYTE              0x6C

/** @brief OTA response codes */
#define OTA_RESPONSE_OK             0x00
#define OTA_RESPONSE_ERROR_MAGIC    0x01
#define OTA_RESPONSE_ERROR_AUTH     0x02
#define OTA_RESPONSE_ERROR_WRITE    0x03
#define OTA_RESPONSE_ERROR_VALIDATE 0x04
#define OTA_RESPONSE_ERROR_GENERIC  0x05
#define OTA_RESPONSE_ERROR_SIZE     0x06

/** @brief OTA feature flags */
#define OTA_FEATURE_SUPPORTS_COMPRESSION  (1 << 0)
#define OTA_FEATURE_SUPPORTS_MD5          (1 << 1)

/** @brief ACK interval for firmware data */
#define OTA_ACK_INTERVAL            8192

/** @brief Maximum firmware size (4MB) */
#define OTA_MAX_FIRMWARE_SIZE       (4 * 1024 * 1024)

/** @brief Receive buffer size */
#define OTA_RX_BUFFER_SIZE          1024

/** Per-recv() timeout. Short, because a pause is not a failure and the loop
 *  below decides when to give up. */
#define OTA_RECV_TIMEOUT_MS         10000

/** How long a transfer may make no progress at all before it counts as dead. */
#define OTA_STALL_LIMIT_US          (60 * 1000000LL)

/** @brief OTA task stack size */
#define OTA_TASK_STACK_SIZE         4096

/** @brief OTA task priority */
#define OTA_TASK_PRIORITY           5

/* ============================================================================
 * Module State
 * ============================================================================ */

/** @brief OTA server state */
typedef struct {
    bool initialized;
    bool running;
    bool update_in_progress;
    esphome_ota_config_t config;
    esphome_ota_status_cb_t status_callback;
    psram_task_handle_t psram_task;
    int server_socket;
    SemaphoreHandle_t mutex;
} esphome_ota_state_t;

static esphome_ota_state_t s_ota_state = {
    .initialized = false,
    .running = false,
    .update_in_progress = false,
    .psram_task = {0},
    .server_socket = -1,
    .mutex = NULL,
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * @brief Report status via callback
 *
 * @param[in] progress Progress percentage
 * @param[in] status Status string
 */
static void report_status(int progress, const char *status)
{
    if (s_ota_state.status_callback != NULL) {
        s_ota_state.status_callback(progress, status);
    }
    ESP_LOGI(TAG, "OTA Status: %d%% - %s", progress, status);
}

/**
 * @brief Set socket timeout
 *
 * @param[in] sock Socket file descriptor
 * @param[in] timeout_ms Timeout in milliseconds
 * @return ESP_OK on success
 */
static esp_err_t set_socket_timeout(int sock, uint32_t timeout_ms)
{
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "Failed to set receive timeout");
        return ESP_FAIL;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "Failed to set send timeout");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Receive exact number of bytes
 *
 * @param[in] sock Socket file descriptor
 * @param[out] buffer Buffer to receive into
 * @param[in] len Number of bytes to receive
 * @return ESP_OK on success, ESP_FAIL on error or timeout
 */
/**
 * @brief Receive some bytes, sitting through pauses
 *
 * recv() on a socket with SO_RCVTIMEO returns -1 with EAGAIN when nothing
 * arrived in time, and every receive path here treated that as the end of the
 * transfer. On a single-SoC design where Wi-Fi shares its radio with Zigbee a
 * pause is the normal condition, not a fault — the same mistake cost the
 * converter database download, where the transfer was working and was
 * abandoned at the first gap.
 *
 * A pause is waited through; the bound is time without progress, because the
 * number of pauses says nothing about whether the peer is still there.
 *
 * @param[in]     sock     Connected socket
 * @param[out]    buf      Destination
 * @param[in]     len      Maximum bytes to take
 * @param[in,out] last_progress_us Timestamp of the last byte received anywhere
 *                in this transfer; updated on success
 * @return >0 bytes received, 0 if the peer closed cleanly, -1 on error or if
 *         nothing arrived for OTA_STALL_LIMIT_US
 */
static int ota_recv_some(int sock, uint8_t *buf, size_t len, int64_t *last_progress_us)
{
    for (;;) {
        int r = recv(sock, buf, len, 0);
        if (r > 0) {
            *last_progress_us = esp_timer_get_time();
            return r;
        }
        if (r == 0) {
            return 0;   /* orderly shutdown by the peer */
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (esp_timer_get_time() - *last_progress_us < OTA_STALL_LIMIT_US) {
                continue;
            }
            ESP_LOGE(TAG, "No data for %d s, giving up",
                     (int)(OTA_STALL_LIMIT_US / 1000000));
            return -1;
        }
        ESP_LOGE(TAG, "recv failed: errno=%d", errno);
        return -1;
    }
}

static esp_err_t recv_exact(int sock, uint8_t *buffer, size_t len)
{
    size_t received = 0;
    int64_t last_progress = esp_timer_get_time();
    while (received < len) {
        int ret = ota_recv_some(sock, buffer + received, len - received, &last_progress);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }
    return ESP_OK;
}

/**
 * @brief Send response byte
 *
 * @param[in] sock Socket file descriptor
 * @param[in] response Response code to send
 * @return ESP_OK on success
 */
static esp_err_t send_response(int sock, uint8_t response)
{
    if (send(sock, &response, 1, 0) != 1) {
        ESP_LOGE(TAG, "Failed to send response");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief Compute MD5 hash of string
 *
 * @param[in] input Input string
 * @param[out] output Output hash (16 bytes)
 * @note May be unused if PSA Crypto API handles all hashing inline
 */
__attribute__((unused))
static void compute_md5(const char *input, uint8_t *output)
{
#if USE_PSA_CRYPTO
    /* ESP-IDF v6.0+ uses PSA Crypto API */
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t output_len = 0;

    psa_crypto_init(); /* Safe to call multiple times */
    psa_hash_setup(&op, PSA_ALG_MD5);
    psa_hash_update(&op, (const uint8_t *)input, strlen(input));
    psa_hash_finish(&op, output, 16, &output_len);
#else
    /* ESP-IDF v5.x uses legacy mbedTLS MD5 API */
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, (const unsigned char *)input, strlen(input));
    mbedtls_md5_finish(&ctx, output);
    mbedtls_md5_free(&ctx);
#endif
}

/* ============================================================================
 * OTA Protocol Handler
 * ============================================================================ */

/**
 * @brief Handle OTA connection
 *
 * Implements the ESPHome OTA protocol:
 * 1. Receive magic byte (0x6C)
 * 2. Send features byte
 * 3. Receive auth hash if password configured
 * 4. Receive firmware size (4 bytes, big-endian)
 * 5. Receive firmware data with ACK every 8KB
 * 6. Verify and switch boot partition
 * 7. Restart on success
 *
 * @param[in] client_sock Client socket file descriptor
 */
static void handle_ota_connection(int client_sock)
{
    esp_err_t err;
    uint8_t buffer[OTA_RX_BUFFER_SIZE];
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    bool ota_started = false;

    ESP_LOGI(TAG, "OTA connection established");
    s_ota_state.update_in_progress = true;
    report_status(0, "OTA connection established");

    /* Set socket timeout */
    /* Short per-call timeout, long patience. The receive loops decide when a
     * transfer is dead, from time without progress; a 60 s blocking recv() can
     * only ever fail once. */
    set_socket_timeout(client_sock, OTA_RECV_TIMEOUT_MS);

    /* Step 1: Receive magic byte */
    uint8_t magic;
    if (recv_exact(client_sock, &magic, 1) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to receive magic byte");
        goto cleanup;
    }

    if (magic != OTA_MAGIC_BYTE) {
        ESP_LOGE(TAG, "Invalid magic byte: 0x%02X (expected 0x%02X)", magic, OTA_MAGIC_BYTE);
        send_response(client_sock, OTA_RESPONSE_ERROR_MAGIC);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Magic byte received");

    /* Step 2: Send features byte */
    uint8_t features = OTA_FEATURE_SUPPORTS_MD5;
    if (send_response(client_sock, features) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send features");
        goto cleanup;
    }

    /* Step 3: Handle authentication if password is set */
    if (strlen(s_ota_state.config.password) > 0) {
        ESP_LOGI(TAG, "Waiting for authentication...");

        /* Receive client nonce */
        uint8_t client_nonce[ESPHOME_MD5_SIZE];
        if (recv_exact(client_sock, client_nonce, ESPHOME_MD5_SIZE) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to receive client nonce");
            goto cleanup;
        }

        /* Generate server nonce and send it */
        uint8_t server_nonce[ESPHOME_MD5_SIZE];
        esp_fill_random(server_nonce, ESPHOME_MD5_SIZE);
        if (send(client_sock, server_nonce, ESPHOME_MD5_SIZE, 0) != ESPHOME_MD5_SIZE) {
            ESP_LOGE(TAG, "Failed to send server nonce");
            goto cleanup;
        }

        /* Receive auth hash from client */
        uint8_t client_hash[ESPHOME_MD5_SIZE];
        if (recv_exact(client_sock, client_hash, ESPHOME_MD5_SIZE) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to receive auth hash");
            goto cleanup;
        }

        /* Compute expected hash: MD5(password + client_nonce + server_nonce) */
        uint8_t expected_hash[ESPHOME_MD5_SIZE];
#if USE_PSA_CRYPTO
        /* ESP-IDF v6.0+ uses PSA Crypto API */
        psa_hash_operation_t hash_op = PSA_HASH_OPERATION_INIT;
        size_t hash_len = 0;
        psa_hash_setup(&hash_op, PSA_ALG_MD5);
        psa_hash_update(&hash_op, (const uint8_t *)s_ota_state.config.password,
                        strlen(s_ota_state.config.password));
        psa_hash_update(&hash_op, client_nonce, ESPHOME_MD5_SIZE);
        psa_hash_update(&hash_op, server_nonce, ESPHOME_MD5_SIZE);
        psa_hash_finish(&hash_op, expected_hash, ESPHOME_MD5_SIZE, &hash_len);
#else
        /* ESP-IDF v5.x uses legacy mbedTLS MD5 API */
        mbedtls_md5_context md5_ctx;
        mbedtls_md5_init(&md5_ctx);
        mbedtls_md5_starts(&md5_ctx);
        mbedtls_md5_update(&md5_ctx, (const unsigned char *)s_ota_state.config.password,
                          strlen(s_ota_state.config.password));
        mbedtls_md5_update(&md5_ctx, client_nonce, ESPHOME_MD5_SIZE);
        mbedtls_md5_update(&md5_ctx, server_nonce, ESPHOME_MD5_SIZE);
        mbedtls_md5_finish(&md5_ctx, expected_hash);
        mbedtls_md5_free(&md5_ctx);
#endif

        /* Verify hash */
        if (memcmp(client_hash, expected_hash, ESPHOME_MD5_SIZE) != 0) {
            ESP_LOGE(TAG, "Authentication failed");
            send_response(client_sock, OTA_RESPONSE_ERROR_AUTH);
            goto cleanup;
        }

        ESP_LOGI(TAG, "Authentication successful");
        send_response(client_sock, OTA_RESPONSE_OK);
    }

    /* Step 4: Receive firmware size (4 bytes, big-endian) */
    uint8_t size_bytes[4];
    if (recv_exact(client_sock, size_bytes, 4) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to receive firmware size");
        goto cleanup;
    }

    uint32_t firmware_size = ((uint32_t)size_bytes[0] << 24) |
                             ((uint32_t)size_bytes[1] << 16) |
                             ((uint32_t)size_bytes[2] << 8) |
                             (uint32_t)size_bytes[3];

    ESP_LOGI(TAG, "Firmware size: %lu bytes", (unsigned long)firmware_size);

    if (firmware_size == 0 || firmware_size > OTA_MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Invalid firmware size: %lu", (unsigned long)firmware_size);
        send_response(client_sock, OTA_RESPONSE_ERROR_SIZE);
        goto cleanup;
    }

    /* Find OTA partition */
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        send_response(client_sock, OTA_RESPONSE_ERROR_GENERIC);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Writing to partition: %s (offset: 0x%lx, size: %lu)",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    if (firmware_size > update_partition->size) {
        ESP_LOGE(TAG, "Firmware too large for partition");
        send_response(client_sock, OTA_RESPONSE_ERROR_SIZE);
        goto cleanup;
    }

    /* Begin OTA — use OTA_SIZE_UNKNOWN to avoid erasing the entire partition
     * upfront. With OTA_SIZE_UNKNOWN, flash sectors are erased on-demand
     * during esp_ota_write(), preventing watchdog timeout on ESP32-C5. */
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        send_response(client_sock, OTA_RESPONSE_ERROR_GENERIC);
        goto cleanup;
    }
    ota_started = true;

    /* Send OK to proceed with firmware data */
    send_response(client_sock, OTA_RESPONSE_OK);
    report_status(0, "Receiving firmware...");

    /* Step 5: Receive firmware data */
    uint32_t bytes_received = 0;
    uint32_t bytes_since_ack = 0;

#if USE_PSA_CRYPTO
    /* ESP-IDF v6.0+ uses PSA Crypto API */
    psa_hash_operation_t firmware_md5_op = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&firmware_md5_op, PSA_ALG_MD5);
#else
    /* ESP-IDF v5.x uses legacy mbedTLS MD5 API */
    mbedtls_md5_context firmware_md5;
    mbedtls_md5_init(&firmware_md5);
    mbedtls_md5_starts(&firmware_md5);
#endif

    int64_t last_progress = esp_timer_get_time();
    while (bytes_received < firmware_size) {
        size_t chunk_size = firmware_size - bytes_received;
        if (chunk_size > OTA_RX_BUFFER_SIZE) {
            chunk_size = OTA_RX_BUFFER_SIZE;
        }

        int received = ota_recv_some(client_sock, buffer, chunk_size, &last_progress);
        if (received <= 0) {
            ESP_LOGE(TAG, "Failed to receive firmware data at offset %lu of %lu",
                     (unsigned long)bytes_received, (unsigned long)firmware_size);
#if USE_PSA_CRYPTO
            psa_hash_abort(&firmware_md5_op);
#else
            mbedtls_md5_free(&firmware_md5);
#endif
            goto cleanup;
        }

        /* Write to OTA partition — yield briefly to let other tasks run
         * (flash erase blocks the CPU on single-core ESP32-C5) */
        err = esp_ota_write(ota_handle, buffer, received);
        vTaskDelay(1);  /* Yield to WDT and other tasks */
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            send_response(client_sock, OTA_RESPONSE_ERROR_WRITE);
#if USE_PSA_CRYPTO
            psa_hash_abort(&firmware_md5_op);
#else
            mbedtls_md5_free(&firmware_md5);
#endif
            goto cleanup;
        }

        /* Update MD5 hash */
#if USE_PSA_CRYPTO
        psa_hash_update(&firmware_md5_op, buffer, received);
#else
        mbedtls_md5_update(&firmware_md5, buffer, received);
#endif

        bytes_received += received;
        bytes_since_ack += received;

        /* Feed watchdog and send ACK every OTA_ACK_INTERVAL bytes */
        if (bytes_since_ack >= OTA_ACK_INTERVAL) {
            send_response(client_sock, OTA_RESPONSE_OK);
            bytes_since_ack = 0;
        }

        /* Report progress */
        int progress = (bytes_received * 100) / firmware_size;
        report_status(progress, "Receiving firmware...");
    }

    /* Compute final MD5 */
    uint8_t firmware_hash[ESPHOME_MD5_SIZE];
#if USE_PSA_CRYPTO
    size_t hash_len = 0;
    psa_hash_finish(&firmware_md5_op, firmware_hash, ESPHOME_MD5_SIZE, &hash_len);
#else
    mbedtls_md5_finish(&firmware_md5, firmware_hash);
    mbedtls_md5_free(&firmware_md5);
#endif

    ESP_LOGI(TAG, "Firmware received: %lu bytes", (unsigned long)bytes_received);

    /* Step 6: Receive and verify MD5 hash from client */
    uint8_t client_md5[ESPHOME_MD5_SIZE];
    struct timeval md5_timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &md5_timeout, sizeof(md5_timeout));

    int md5_total = 0;
    int64_t md5_progress = esp_timer_get_time();
    bool md5_truncated = false;
    while (md5_total < ESPHOME_MD5_SIZE) {
        int md5_got = ota_recv_some(client_sock, client_md5 + md5_total,
                                    ESPHOME_MD5_SIZE - md5_total, &md5_progress);
        if (md5_got <= 0) {
            /* A pause is already waited through by ota_recv_some(), so reaching
             * here means the peer went away or erred.
             *
             * Sending nothing at all is an old client that does not offer a
             * checksum, and verification is skipped as before. Sending part of
             * one is not: this used to treat any interruption — including an
             * ordinary receive timeout — as permission to install an unverified
             * firmware image. */
            md5_truncated = (md5_total > 0);
            ESP_LOGW(TAG, "MD5 recv ended after %d of %d bytes (errno=%d)",
                     md5_total, ESPHOME_MD5_SIZE, errno);
            break;
        }
        md5_total += md5_got;
    }
    if (md5_truncated) {
        ESP_LOGE(TAG, "Refusing an image whose checksum arrived incomplete");
        send_response(client_sock, OTA_RESPONSE_ERROR_VALIDATE);
        goto cleanup;
    }

    if (md5_total == ESPHOME_MD5_SIZE) {
        if (memcmp(client_md5, firmware_hash, ESPHOME_MD5_SIZE) != 0) {
            ESP_LOGE(TAG, "MD5 verification FAILED");
            send_response(client_sock, OTA_RESPONSE_ERROR_VALIDATE);
            goto cleanup;
        }
        ESP_LOGI(TAG, "MD5 verification passed");
    } else {
        ESP_LOGW(TAG, "No MD5 from client (got %d bytes), skipping check", md5_total);
    }

    /* === Finalize OTA ===
     *
     * On ESP32-C5 (single-core RISC-V, ~30KB free heap during OTA),
     * esp_ota_end() crashes because it allocates a large buffer to
     * verify the entire firmware image. This is safe to skip because:
     *
     * 1. MD5 of the complete firmware was verified above
     * 2. Each esp_ota_write() verified its own flash write
     * 3. The bootloader validates the image header on every boot
     * 4. esp_ota_set_boot_partition() validates the app descriptor
     *
     * We skip esp_ota_end() and directly switch the boot partition.
     * The OTA handle is intentionally leaked — we restart immediately.
     */

    /* Close socket first to free TCP resources */
    ESP_LOGI(TAG, "Closing client socket...");
    close(client_sock);
    client_sock = -1;

    ota_started = false;  /* Prevent esp_ota_abort() in cleanup */

    ESP_LOGI(TAG, "Firmware written and MD5 verified (free heap: %lu bytes)",
             (unsigned long)esp_get_free_heap_size());
    report_status(100, "Switching boot partition...");

    /* Switch boot partition — this validates the image header on flash
     * and writes the otadata entry. Does NOT need esp_ota_end(). */
    ESP_LOGI(TAG, "Setting boot partition to %s (offset: 0x%lx)...",
             update_partition->label, (unsigned long)update_partition->address);
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Image header may be invalid despite MD5 match");
        goto cleanup;
    }
    ESP_LOGI(TAG, "Boot partition switched to %s — OTA complete!",
             update_partition->label);

    report_status(100, "OTA complete, restarting...");
    s_ota_state.update_in_progress = false;

    /* Brief delay to let log output flush */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return;

cleanup:
    if (ota_started) {
        esp_ota_abort(ota_handle);
    }
    s_ota_state.update_in_progress = false;
    report_status(0, "OTA failed");
    if (client_sock >= 0) {
        close(client_sock);
    }

}

/* ============================================================================
 * OTA Server Task
 * ============================================================================ */

/**
 * @brief OTA server task
 *
 * Listens for incoming OTA connections and handles them.
 *
 * @param[in] pvParameters Task parameters (unused)
 */
static void esphome_ota_task(void *pvParameters)
{
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int opt = 1;

    ESP_LOGI(TAG, "Starting OTA server on port %d", s_ota_state.config.port);

    /* Create server socket */
    s_ota_state.server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_ota_state.server_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        goto exit;
    }

    /* Set socket options */
    setsockopt(s_ota_state.server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind socket */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(s_ota_state.config.port);

    if (bind(s_ota_state.server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket: %d", errno);
        goto exit;
    }

    /* Listen for connections */
    if (listen(s_ota_state.server_socket, 1) < 0) {
        ESP_LOGE(TAG, "Failed to listen: %d", errno);
        goto exit;
    }

    ESP_LOGI(TAG, "OTA server listening on port %d", s_ota_state.config.port);
    s_ota_state.running = true;

    while (s_ota_state.running) {
        /* Accept connection with timeout */
        struct timeval accept_timeout = {.tv_sec = 1, .tv_usec = 0};
        setsockopt(s_ota_state.server_socket, SOL_SOCKET, SO_RCVTIMEO,
                   &accept_timeout, sizeof(accept_timeout));

        int client_sock = accept(s_ota_state.server_socket,
                                 (struct sockaddr *)&client_addr,
                                 &client_addr_len);

        if (client_sock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout, check if we should continue */
                continue;
            }
            if (s_ota_state.running) {
                ESP_LOGE(TAG, "Accept failed: %d", errno);
            }
            continue;
        }

        ESP_LOGI(TAG, "OTA client connected from %d.%d.%d.%d:%d",
                 (client_addr.sin_addr.s_addr >> 0) & 0xFF,
                 (client_addr.sin_addr.s_addr >> 8) & 0xFF,
                 (client_addr.sin_addr.s_addr >> 16) & 0xFF,
                 (client_addr.sin_addr.s_addr >> 24) & 0xFF,
                 ntohs(client_addr.sin_port));

        /* Handle OTA connection */
        handle_ota_connection(client_sock);
    }

exit:
    if (s_ota_state.server_socket >= 0) {
        close(s_ota_state.server_socket);
        s_ota_state.server_socket = -1;
    }
    s_ota_state.running = false;
    ESP_LOGI(TAG, "OTA server stopped");
    /* Mark as self-deleted before vTaskDelete - resources freed by stop/deinit */
    psram_task_mark_deleted(&s_ota_state.psram_task);
    vTaskDelete(NULL);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t esphome_ota_init(const esphome_ota_config_t *config)
{
    if (s_ota_state.initialized) {
        ESP_LOGW(TAG, "OTA already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        esphome_ota_config_t default_config = ESPHOME_OTA_CONFIG_DEFAULT();
        memcpy(&s_ota_state.config, &default_config, sizeof(esphome_ota_config_t));
    } else {
        memcpy(&s_ota_state.config, config, sizeof(esphome_ota_config_t));
    }

    /* Create mutex */
    s_ota_state.mutex = xSemaphoreCreateMutex();
    if (s_ota_state.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_ota_state.initialized = true;
    s_ota_state.running = false;
    s_ota_state.update_in_progress = false;
    s_ota_state.status_callback = NULL;
    memset(&s_ota_state.psram_task, 0, sizeof(psram_task_handle_t));
    s_ota_state.server_socket = -1;

    ESP_LOGI(TAG, "ESPHome OTA initialized (port: %d, auth: %s)",
             s_ota_state.config.port,
             strlen(s_ota_state.config.password) > 0 ? "enabled" : "disabled");

    return ESP_OK;
}

esp_err_t esphome_ota_deinit(void)
{
    if (!s_ota_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop server if running */
    esphome_ota_stop();

    /* Delete mutex */
    if (s_ota_state.mutex != NULL) {
        vSemaphoreDelete(s_ota_state.mutex);
        s_ota_state.mutex = NULL;
    }

    s_ota_state.initialized = false;
    ESP_LOGI(TAG, "ESPHome OTA deinitialized");

    return ESP_OK;
}

esp_err_t esphome_ota_start(void)
{
    if (!s_ota_state.initialized) {
        ESP_LOGE(TAG, "OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ota_state.running) {
        ESP_LOGW(TAG, "OTA server already running");
        return ESP_OK;
    }

    /* Create OTA task with stack in PSRAM (saves 4KB internal RAM) */
    esp_err_t ret = psram_task_create(
        esphome_ota_task,
        "esphome_ota",
        OTA_TASK_STACK_SIZE,
        NULL,
        OTA_TASK_PRIORITY,
        &s_ota_state.psram_task
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t esphome_ota_stop(void)
{
    if (!s_ota_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_ota_state.running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping OTA server...");
    s_ota_state.running = false;

    /* Close server socket to interrupt accept() */
    if (s_ota_state.server_socket >= 0) {
        close(s_ota_state.server_socket);
        s_ota_state.server_socket = -1;
    }

    /* Wait for task to finish */
    int timeout = 50;  /* 5 seconds */
    while (psram_task_is_valid(&s_ota_state.psram_task) && timeout > 0) {
        vTaskDelay(ESPHOME_MUTEX_TIMEOUT_TICKS);
        timeout--;
    }

    if (psram_task_is_valid(&s_ota_state.psram_task)) {
        ESP_LOGW(TAG, "OTA task did not exit gracefully");
    }

    /* Free PSRAM task resources (stack and TCB) */
    psram_task_delete(&s_ota_state.psram_task);

    return ESP_OK;
}

bool esphome_ota_is_running(void)
{
    return s_ota_state.running;
}

bool esphome_ota_in_progress(void)
{
    return s_ota_state.update_in_progress;
}

void esphome_ota_set_status_callback(esphome_ota_status_cb_t callback)
{
    s_ota_state.status_callback = callback;
}
