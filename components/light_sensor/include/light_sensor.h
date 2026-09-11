/*
 * SPDX-FileCopyrightText: 2021-2025
 * SPDX-License-Identifier: CC0-1.0
 *
 * Ambient light sensor facade.
 *
 * Owns the I2C bus and picks whichever supported sensor is actually fitted, so
 * one firmware image covers both. They sit at different addresses, so detection
 * is unambiguous:
 *
 *   BH1750  0x23  lux only
 *   LTR390  0x53  lux + UV index
 *
 * Wiring on the SHS01 board (ESP32-C6), identical for either sensor:
 *   VCC  -> 3V3   (NOT the 5V rail the radars use - both parts are 3.3V devices;
 *                  GY-302/GY-30 and most LTR390 breakouts regulate and tolerate
 *                  5V, bare modules do not)
 *   GND  -> GND
 *   SDA  -> GPIO6
 *   SCL  -> GPIO7
 *   ADDR -> GND   (BH1750 only; selects 0x23)
 */

#ifndef LIGHT_SENSOR_H
#define LIGHT_SENSOR_H

#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

/* ---------------- Hardware configuration ---------------- */
#define LIGHT_SENSOR_I2C_PORT       0               /* ESP32-C6 has one HP I2C port */
#define LIGHT_SENSOR_SDA_GPIO       GPIO_NUM_6
#define LIGHT_SENSOR_SCL_GPIO       GPIO_NUM_7
#define LIGHT_SENSOR_I2C_FREQ_HZ    100000

typedef enum {
    LIGHT_SENSOR_NONE = 0,
    LIGHT_SENSOR_BH1750,
    LIGHT_SENSOR_LTR390,
} light_sensor_type_t;

/* Create the bus if needed, then probe for each supported part in turn.
 * Safe to call repeatedly - it doubles as a "retry until one is wired up" probe.
 * Returns ESP_OK only when a sensor answered. */
esp_err_t light_sensor_init(void);

light_sensor_type_t light_sensor_type(void);

/* "BH1750", "LTR390", or "none" - for logging. */
const char *light_sensor_name(void);

/* True when the detected part can report a UV index. */
bool light_sensor_has_uv(void);

/* Ambient light in lux. BLOCKS for 120-180 ms - call from a task. */
esp_err_t light_sensor_read_lux(float *out_lux);

/* UV index. BLOCKS for ~420 ms. Returns ESP_ERR_NOT_SUPPORTED when the
 * detected part has no UV channel. */
esp_err_t light_sensor_read_uvi(float *out_uvi);

#endif /* LIGHT_SENSOR_H */
