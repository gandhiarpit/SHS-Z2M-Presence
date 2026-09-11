/*
 * SPDX-FileCopyrightText: 2021-2025
 * SPDX-License-Identifier: CC0-1.0
 *
 * BH1750 ambient light sensor driver (I2C, one-shot high-resolution mode)
 *
 * The bus is owned by the light_sensor facade - this driver only borrows it.
 * See light_sensor.h for the wiring and for which sensors are supported.
 */

#ifndef BH1750_H
#define BH1750_H

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define BH1750_ADDR             0x23    /* ADDR pin low; 0x5C when high */

/* One-shot H-resolution conversion time: 120 ms typical, 180 ms max (MTreg = 69) */
#define BH1750_MEAS_DELAY_MS    180

/* Attach to an existing I2C bus and confirm the sensor answers.
 * Safe to call repeatedly, so it doubles as a "retry until wired up" probe. */
esp_err_t bh1750_init(i2c_master_bus_handle_t bus);

/* Trigger a one-shot measurement and return the result in lux.
 * BLOCKS for BH1750_MEAS_DELAY_MS - call from a task, never from a callback. */
esp_err_t bh1750_read_lux(float *out_lux);

#endif /* BH1750_H */
