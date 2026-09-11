/*
 * SPDX-FileCopyrightText: 2021-2025
 * SPDX-License-Identifier: CC0-1.0
 *
 * BH1750 ambient light sensor driver (I2C, one-shot high-resolution mode)
 */

#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bh1750.h"

static const char *BH1750_TAG = "BH1750";

/* BH1750 opcodes (datasheet rev. D, "Instruction Set Architecture") */
#define BH1750_CMD_POWER_DOWN       0x00
#define BH1750_CMD_POWER_ON         0x01
#define BH1750_CMD_RESET            0x07    /* clears the data register; needs power on */
#define BH1750_CMD_ONE_TIME_H_RES   0x20    /* 1 lx resolution, auto power-down after */

/* Counts-to-lux divisor for the default measurement time register (MTreg = 69) */
#define BH1750_LUX_DIVISOR          1.2f

#define BH1750_I2C_TIMEOUT_MS       100

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t bh1750_write_cmd(uint8_t cmd)
{
    return i2c_master_transmit(s_dev, &cmd, 1, BH1750_I2C_TIMEOUT_MS);
}

esp_err_t bh1750_init(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) return ESP_ERR_INVALID_ARG;
    s_bus = bus;

    if (s_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BH1750_ADDR,
            .scl_speed_hz = 100000,
        };
        esp_err_t rc = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
        if (rc != ESP_OK) {
            ESP_LOGE(BH1750_TAG, "bus_add_device failed: %s", esp_err_to_name(rc));
            s_dev = NULL;
            return rc;
        }
    }

    /* The BH1750 has no ID register. The facade has already confirmed something
     * ACKs at this address, so presence here means the part accepts a command. */
    esp_err_t rc = bh1750_write_cmd(BH1750_CMD_POWER_ON);
    if (rc == ESP_OK) {
        rc = bh1750_write_cmd(BH1750_CMD_RESET);
    }
    if (rc != ESP_OK) {
        ESP_LOGW(BH1750_TAG, "power-on/reset failed: %s", esp_err_to_name(rc));
        return rc;
    }

    ESP_LOGI(BH1750_TAG, "BH1750 detected at 0x%02X", BH1750_ADDR);
    return ESP_OK;
}

esp_err_t bh1750_read_lux(float *out_lux)
{
    if (out_lux == NULL) return ESP_ERR_INVALID_ARG;
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;

    /* One-shot mode powers the sensor down after every measurement, so each
     * reading is: power on -> trigger -> wait for conversion -> read 2 bytes. */
    esp_err_t rc = bh1750_write_cmd(BH1750_CMD_POWER_ON);
    if (rc != ESP_OK) return rc;

    rc = bh1750_write_cmd(BH1750_CMD_ONE_TIME_H_RES);
    if (rc != ESP_OK) return rc;

    vTaskDelay(pdMS_TO_TICKS(BH1750_MEAS_DELAY_MS));

    uint8_t buf[2] = {0, 0};
    rc = i2c_master_receive(s_dev, buf, sizeof(buf), BH1750_I2C_TIMEOUT_MS);
    if (rc != ESP_OK) return rc;

    uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
    *out_lux = (float)raw / BH1750_LUX_DIVISOR;
    return ESP_OK;
}
