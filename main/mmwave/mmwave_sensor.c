/**
 * @file mmwave_sensor.c
 * @brief HMMD mmWave Presence Sensor Driver (S3KM1110)
 *
 * Parses UART frames from the S3KM1110 24GHz FMCW radar module.
 *
 * Report frame format:
 *   Header:  F4 F3 F2 F1
 *   Length:  2 bytes (little-endian)
 *   Data:    [result(1)] [dist_lo dist_hi] [32 bytes gate energy]
 *   Tail:    F8 F7 F6 F5
 *
 * Command frame format:
 *   Header:  FD FC FB FA
 *   Length:  2 bytes (little-endian)
 *   Data:    [cmd_lo cmd_hi] [payload...]
 *   Tail:    04 03 02 01
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "mmwave_sensor.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "MMWAVE";

/* UART configuration */
#define MMWAVE_UART_NUM       UART_NUM_1
#define MMWAVE_UART_BAUD      115200
#define MMWAVE_UART_BUF_SIZE  512

/* Frame markers */
static const uint8_t REPORT_HEADER[] = { 0xF4, 0xF3, 0xF2, 0xF1 };
static const uint8_t REPORT_TAIL[]   = { 0xF8, 0xF7, 0xF6, 0xF5 };
static const uint8_t CMD_HEADER[]    = { 0xFD, 0xFC, 0xFB, 0xFA };
static const uint8_t CMD_TAIL[]      = { 0x04, 0x03, 0x02, 0x01 };

/* Sensor timeout: no data for 5 seconds = disconnected */
#define MMWAVE_TIMEOUT_US  (5 * 1000000LL)

/* State */
static mmwave_state_t s_state;
static bool s_prev_presence;      /* Track changes for ESPHome push */
static uint16_t s_prev_distance;
static TaskHandle_t s_task_handle;
static bool s_initialized;

/* Optional callback for state changes (set by ESPHome adapter) */
static void (*s_state_callback)(const mmwave_state_t *state) = NULL;

/* Ring buffer for frame assembly */
#define RX_BUF_SIZE  256
static uint8_t s_rx_buf[RX_BUF_SIZE];
static size_t  s_rx_len;

/* ========================================================================== */
/* Frame Parsing                                                              */
/* ========================================================================== */

static bool find_marker(const uint8_t *buf, size_t len, const uint8_t *marker,
                        size_t marker_len, size_t *pos)
{
    if (len < marker_len) return false;
    for (size_t i = 0; i <= len - marker_len; i++) {
        if (memcmp(&buf[i], marker, marker_len) == 0) {
            *pos = i;
            return true;
        }
    }
    return false;
}

/**
 * @brief Parse a report frame: [result(1)] [dist(2)] [energy(32)]
 * Total data = 35 bytes
 */
static void parse_report_frame(const uint8_t *data, uint16_t data_len)
{
    if (data_len < 3) {
        ESP_LOGD(TAG, "Report frame too short: %u bytes", data_len);
        return;
    }

    s_state.presence = (data[0] != 0x00);
    s_state.distance_cm = (uint16_t)(data[1] | (data[2] << 8));

    /* Parse gate energy values if available (16 gates x 2 bytes = 32 bytes) */
    if (data_len >= 35) {
        for (int i = 0; i < MMWAVE_NUM_GATES; i++) {
            s_state.gate_energy[i] = (uint16_t)(data[3 + i * 2] | (data[4 + i * 2] << 8));
        }
    }

    s_state.last_update_us = esp_timer_get_time();

    /* Notify on state change (or periodically every ~1s for distance) */
    bool changed = (s_state.presence != s_prev_presence) ||
                   (s_state.presence && abs((int)s_state.distance_cm - (int)s_prev_distance) > 5);
    if (changed) {
        s_prev_presence = s_state.presence;
        s_prev_distance = s_state.distance_cm;
        if (s_state_callback) {
            s_state_callback(&s_state);
        }
    }

    ESP_LOGD(TAG, "Report: presence=%d dist=%ucm", s_state.presence, s_state.distance_cm);
}

/**
 * @brief Parse a command response frame
 */
static void parse_cmd_response(const uint8_t *data, uint16_t data_len)
{
    if (data_len < 4) return;

    uint16_t cmd = (uint16_t)(data[0] | (data[1] << 8));
    uint16_t status = (uint16_t)(data[2] | (data[3] << 8));

    ESP_LOGI(TAG, "CMD response: cmd=0x%04X status=0x%04X (len=%u)", cmd, status, data_len);
}

/**
 * @brief Try to extract and process complete frames from the buffer
 */
static void process_buffer(void)
{
    while (s_rx_len >= 8) {  /* Minimum frame: 4 header + 2 len + 0 data + ... actually need tail too */
        size_t pos;

        /* Look for report frame header */
        if (find_marker(s_rx_buf, s_rx_len, REPORT_HEADER, 4, &pos)) {
            if (pos > 0) {
                /* Discard bytes before header */
                memmove(s_rx_buf, &s_rx_buf[pos], s_rx_len - pos);
                s_rx_len -= pos;
            }

            /* Need at least header(4) + length(2) */
            if (s_rx_len < 6) break;

            uint16_t data_len = (uint16_t)(s_rx_buf[4] | (s_rx_buf[5] << 8));
            size_t frame_size = 4 + 2 + data_len + 4;  /* header + len + data + tail */

            if (frame_size > RX_BUF_SIZE) {
                /* Invalid frame, skip header */
                memmove(s_rx_buf, &s_rx_buf[4], s_rx_len - 4);
                s_rx_len -= 4;
                continue;
            }

            if (s_rx_len < frame_size) break;  /* Need more data */

            /* Verify tail */
            if (memcmp(&s_rx_buf[frame_size - 4], REPORT_TAIL, 4) == 0) {
                parse_report_frame(&s_rx_buf[6], data_len);
            } else {
                ESP_LOGD(TAG, "Report frame bad tail");
            }

            /* Remove frame from buffer */
            memmove(s_rx_buf, &s_rx_buf[frame_size], s_rx_len - frame_size);
            s_rx_len -= frame_size;
            continue;
        }

        /* Look for command response header */
        if (find_marker(s_rx_buf, s_rx_len, CMD_HEADER, 4, &pos)) {
            if (pos > 0) {
                memmove(s_rx_buf, &s_rx_buf[pos], s_rx_len - pos);
                s_rx_len -= pos;
            }

            if (s_rx_len < 6) break;

            uint16_t data_len = (uint16_t)(s_rx_buf[4] | (s_rx_buf[5] << 8));
            size_t frame_size = 4 + 2 + data_len + 4;

            if (frame_size > RX_BUF_SIZE) {
                memmove(s_rx_buf, &s_rx_buf[4], s_rx_len - 4);
                s_rx_len -= 4;
                continue;
            }

            if (s_rx_len < frame_size) break;

            if (memcmp(&s_rx_buf[frame_size - 4], CMD_TAIL, 4) == 0) {
                parse_cmd_response(&s_rx_buf[6], data_len);
            } else {
                ESP_LOGD(TAG, "CMD frame bad tail");
            }

            memmove(s_rx_buf, &s_rx_buf[frame_size], s_rx_len - frame_size);
            s_rx_len -= frame_size;
            continue;
        }

        /* No header found — discard one byte and retry */
        memmove(s_rx_buf, &s_rx_buf[1], s_rx_len - 1);
        s_rx_len--;
    }
}

/* ========================================================================== */
/* Command Sending                                                            */
/* ========================================================================== */

static esp_err_t send_command(uint16_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    uint16_t data_len = 2 + payload_len;  /* cmd(2) + payload */
    size_t frame_size = 4 + 2 + data_len + 4;

    uint8_t frame[64];
    if (frame_size > sizeof(frame)) return ESP_ERR_NO_MEM;

    size_t i = 0;
    memcpy(&frame[i], CMD_HEADER, 4); i += 4;
    frame[i++] = (uint8_t)(data_len & 0xFF);
    frame[i++] = (uint8_t)(data_len >> 8);
    frame[i++] = (uint8_t)(cmd & 0xFF);
    frame[i++] = (uint8_t)(cmd >> 8);
    if (payload_len > 0 && payload) {
        memcpy(&frame[i], payload, payload_len);
        i += payload_len;
    }
    memcpy(&frame[i], CMD_TAIL, 4); i += 4;

    int written = uart_write_bytes(MMWAVE_UART_NUM, frame, i);
    if (written < 0) return ESP_FAIL;

    ESP_LOGD(TAG, "Sent cmd 0x%04X (%zu bytes)", cmd, i);
    return ESP_OK;
}

/* ========================================================================== */
/* UART Reader Task                                                           */
/* ========================================================================== */

static void mmwave_uart_task(void *arg)
{
    (void)arg;
    uint8_t tmp[128];

    ESP_LOGI(TAG, "UART reader task started (GPIO TX=%d, RX=%d)",
             CONFIG_MMWAVE_GPIO_TX, CONFIG_MMWAVE_GPIO_RX);

    /* Request firmware version on startup */
    vTaskDelay(pdMS_TO_TICKS(500));
    send_command(0x0000, NULL, 0);

    while (true) {
        int len = uart_read_bytes(MMWAVE_UART_NUM, tmp, sizeof(tmp), pdMS_TO_TICKS(100));
        if (len > 0) {
            /* Append to ring buffer */
            size_t copy = len;
            if (s_rx_len + copy > RX_BUF_SIZE) {
                /* Buffer overflow — discard old data */
                size_t discard = (s_rx_len + copy) - RX_BUF_SIZE;
                memmove(s_rx_buf, &s_rx_buf[discard], s_rx_len - discard);
                s_rx_len -= discard;
                ESP_LOGW(TAG, "RX buffer overflow, discarded %zu bytes", discard);
            }
            memcpy(&s_rx_buf[s_rx_len], tmp, copy);
            s_rx_len += copy;

            process_buffer();
        }
    }
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

esp_err_t mmwave_sensor_init(void)
{
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Initializing HMMD mmWave sensor (S3KM1110)");
    ESP_LOGI(TAG, "  UART%d: TX=GPIO%d, RX=GPIO%d, baud=%d",
             MMWAVE_UART_NUM, CONFIG_MMWAVE_GPIO_TX, CONFIG_MMWAVE_GPIO_RX,
             MMWAVE_UART_BAUD);

    /* Configure UART */
    uart_config_t uart_cfg = {
        .baud_rate  = MMWAVE_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(MMWAVE_UART_NUM, MMWAVE_UART_BUF_SIZE,
                                         MMWAVE_UART_BUF_SIZE, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(MMWAVE_UART_NUM, &uart_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(ret));
        uart_driver_delete(MMWAVE_UART_NUM);
        return ret;
    }

    ret = uart_set_pin(MMWAVE_UART_NUM,
                       CONFIG_MMWAVE_GPIO_TX,   /* ESP TX → Sensor RX */
                       CONFIG_MMWAVE_GPIO_RX,   /* ESP RX ← Sensor TX */
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %s", esp_err_to_name(ret));
        uart_driver_delete(MMWAVE_UART_NUM);
        return ret;
    }

    /* Init state */
    memset(&s_state, 0, sizeof(s_state));
    s_rx_len = 0;

    /* Start reader task */
    BaseType_t xret = xTaskCreate(mmwave_uart_task, "mmwave", 3072, NULL, 5, &s_task_handle);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART task");
        uart_driver_delete(MMWAVE_UART_NUM);
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "mmWave sensor initialized");
    return ESP_OK;
}

esp_err_t mmwave_sensor_get_state(mmwave_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    *state = s_state;
    return ESP_OK;
}

esp_err_t mmwave_sensor_set_max_gate(uint8_t gate)
{
    if (gate > 15) gate = 15;

    /* Write parameter: ID=0x0001 (max distance gate), value=gate */
    uint8_t payload[6];
    payload[0] = 0x01; payload[1] = 0x00;  /* Parameter ID */
    payload[2] = gate;  payload[3] = 0x00;
    payload[4] = 0x00;  payload[5] = 0x00;

    ESP_LOGI(TAG, "Setting max gate to %d (range: %.1fm)", gate, gate * 0.7f);
    return send_command(0x0007, payload, sizeof(payload));  /* 0x0007 = write parameters */
}

esp_err_t mmwave_sensor_set_absence_delay(uint16_t seconds)
{
    /* Write parameter: ID=0x0002 (disappearance delay), value=seconds */
    uint8_t payload[6];
    payload[0] = 0x02; payload[1] = 0x00;  /* Parameter ID */
    payload[2] = (uint8_t)(seconds & 0xFF);
    payload[3] = (uint8_t)(seconds >> 8);
    payload[4] = 0x00;  payload[5] = 0x00;

    ESP_LOGI(TAG, "Setting absence delay to %us", seconds);
    return send_command(0x0007, payload, sizeof(payload));
}

bool mmwave_sensor_is_connected(void)
{
    if (!s_initialized || s_state.last_update_us == 0) return false;
    return (esp_timer_get_time() - s_state.last_update_us) < MMWAVE_TIMEOUT_US;
}

void mmwave_sensor_set_callback(void (*cb)(const mmwave_state_t *state))
{
    s_state_callback = cb;
}
