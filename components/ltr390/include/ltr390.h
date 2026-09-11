/*
 * SPDX-FileCopyrightText: 2021-2025
 * SPDX-License-Identifier: CC0-1.0
 *
 * LTR390-UV-01 ambient light + UV sensor driver (I2C).
 *
 * The chip measures ALS *or* UVS, never both at once, so a full read cycle
 * switches modes with a settling delay in between. Each mode also gets its own
 * gain/resolution: the datasheet only gives an accurate UV Index conversion at
 * 18x gain with 20-bit resolution, while ALS uses a lower gain so that ordinary
 * indoor lighting does not saturate the channel.
 *
 * The bus is owned by the light_sensor facade - this driver only borrows it.
 */

#ifndef LTR390_H
#define LTR390_H

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define LTR390_ADDR             0x53

/* Attach to an existing I2C bus and verify the part is present. */
esp_err_t ltr390_init(i2c_master_bus_handle_t bus);

/* Ambient light in lux. BLOCKS for roughly 120 ms - call from a task. */
esp_err_t ltr390_read_lux(float *out_lux);

/* UV index. BLOCKS for roughly 420 ms - call from a task. */
esp_err_t ltr390_read_uvi(float *out_uvi);

#endif /* LTR390_H */
