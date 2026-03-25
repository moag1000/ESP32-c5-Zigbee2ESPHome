/**
 * @file mmwave_sensor.h
 * @brief HMMD mmWave Presence Sensor Driver (S3KM1110)
 *
 * UART-based 24GHz FMCW radar sensor for human presence detection.
 * Parses report frames and exposes presence, distance, and energy data.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define MMWAVE_NUM_GATES  16   /**< Number of distance gates (each ~70cm) */

/**
 * @brief mmWave sensor state (updated every ~100ms by sensor)
 */
typedef struct {
    bool     presence;                       /**< true = human detected */
    uint16_t distance_cm;                    /**< Target distance in cm */
    uint16_t gate_energy[MMWAVE_NUM_GATES];  /**< Energy per gate (16 gates) */
    int64_t  last_update_us;                 /**< Timestamp of last report */
} mmwave_state_t;

/**
 * @brief Initialize the mmWave sensor driver
 *
 * Sets up UART1 on configured GPIOs, starts the parser task.
 *
 * @return ESP_OK on success
 */
esp_err_t mmwave_sensor_init(void);

/**
 * @brief Get current sensor state
 *
 * @param[out] state Pointer to receive current state
 * @return ESP_OK on success
 */
esp_err_t mmwave_sensor_get_state(mmwave_state_t *state);

/**
 * @brief Set maximum detection distance gate
 *
 * @param gate Gate number 0-15 (each gate = 70cm, so gate 15 = 10.5m)
 * @return ESP_OK on success
 */
esp_err_t mmwave_sensor_set_max_gate(uint8_t gate);

/**
 * @brief Set target disappearance delay
 *
 * @param seconds Delay in seconds (0-65535)
 * @return ESP_OK on success
 */
esp_err_t mmwave_sensor_set_absence_delay(uint16_t seconds);

/**
 * @brief Check if sensor is connected and sending data
 */
bool mmwave_sensor_is_connected(void);

/**
 * @brief Register callback for state changes
 *
 * Called from UART task context when presence or distance changes.
 */
void mmwave_sensor_set_callback(void (*cb)(const mmwave_state_t *state));
