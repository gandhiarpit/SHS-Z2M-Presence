/*
 * SPDX-FileCopyrightText: 2021-2025
 * SPDX-License-Identifier: CC0-1.0
 *
 * BH1750 ambient light sensor driver (I2C, one-shot high-resolution mode)
 *
 * Wiring on the SHS01 board (ESP32-C6):
 *   BH1750 VCC -> 3V3   (NOT the 5V rail the radars use - the die is 2.4-3.6V;
 *                        GY-302/GY-30 breakouts have a regulator and tolerate 5V,
 *                        a bare module does not)
 *   BH1750 GND -> GND
 *   BH1750 SDA -> GPIO6
 *   BH1750 SCL -> GPIO7
 *   BH1750 ADDR-> GND   (address 0x23; tie high for 0x5C)
 */

#ifndef BH1750_H
#define BH1750_H

#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

/* ---------------- Hardware configuration ---------------- */
#define BH1750_I2C_PORT         0               /* ESP32-C6 has one HP I2C port */
#define BH1750_SDA_GPIO         GPIO_NUM_6
#define BH1750_SCL_GPIO         GPIO_NUM_7
#define BH1750_I2C_FREQ_HZ      100000          /* 100 kHz - BH1750 supports up to 400 kHz */
#define BH1750_ADDR             0x23            /* ADDR pin low */
#define BH1750_I2C_TIMEOUT_MS   100

/* One-shot H-resolution conversion time: 120 ms typical, 180 ms max (MTreg = 69) */
#define BH1750_MEAS_DELAY_MS    180

/* Initialise the I2C bus and probe the sensor.
 * Safe to call repeatedly - the bus is only created once, so this doubles as a
 * "retry until the sensor is wired up" probe. Returns ESP_OK only when the
 * sensor actually answers on the bus. */
esp_err_t bh1750_init(void);

/* True when the last init/read succeeded. */
bool bh1750_is_present(void);

/* Trigger a one-shot measurement and return the result in lux.
 * BLOCKS for BH1750_MEAS_DELAY_MS - call from a task, never from a callback. */
esp_err_t bh1750_read_lux(float *out_lux);

#endif /* BH1750_H */
