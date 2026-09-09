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
#include "driver/i2c_master.h"

#include "bh1750.h"

static const char *BH1750_TAG = "BH1750";

/* BH1750 opcodes (datasheet rev. D, section "Instruction Set Architecture") */
#define BH1750_CMD_POWER_DOWN       0x00
#define BH1750_CMD_POWER_ON         0x01
#define BH1750_CMD_RESET            0x07    /* clears the data register; needs power on */
#define BH1750_CMD_ONE_TIME_H_RES   0x20    /* 1 lx resolution, auto power-down after */

/* Counts-to-lux divisor for the default measurement time register (MTreg = 69) */
#define BH1750_LUX_DIVISOR          1.2f

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_present = false;

static esp_err_t bh1750_write_cmd(uint8_t cmd)
{
    return i2c_master_transmit(s_dev, &cmd, 1, BH1750_I2C_TIMEOUT_MS);
}

esp_err_t bh1750_init(void)
{
    esp_err_t rc;

    if (s_bus == NULL) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = BH1750_I2C_PORT,
            .sda_io_num = BH1750_SDA_GPIO,
            .scl_io_num = BH1750_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        rc = i2c_new_master_bus(&bus_cfg, &s_bus);
        if (rc != ESP_OK) {
            ESP_LOGE(BH1750_TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(rc));
            s_bus = NULL;
            return rc;
        }
        ESP_LOGI(BH1750_TAG, "I2C bus created on SDA=GPIO%d SCL=GPIO%d",
                 (int)BH1750_SDA_GPIO, (int)BH1750_SCL_GPIO);
    }

    if (s_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BH1750_ADDR,
            .scl_speed_hz = BH1750_I2C_FREQ_HZ,
        };
        rc = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
        if (rc != ESP_OK) {
            ESP_LOGE(BH1750_TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(rc));
            s_dev = NULL;
            return rc;
        }
    }

    /* Does anything actually answer at 0x23? */
    rc = i2c_master_probe(s_bus, BH1750_ADDR, BH1750_I2C_TIMEOUT_MS);
    if (rc != ESP_OK) {
        s_present = false;
        return rc;
    }

    rc = bh1750_write_cmd(BH1750_CMD_POWER_ON);
    if (rc == ESP_OK) {
        rc = bh1750_write_cmd(BH1750_CMD_RESET);
    }
    if (rc != ESP_OK) {
        ESP_LOGW(BH1750_TAG, "power-on/reset failed: %s", esp_err_to_name(rc));
        s_present = false;
        return rc;
    }

    s_present = true;
    ESP_LOGI(BH1750_TAG, "BH1750 detected at 0x%02X", BH1750_ADDR);
    return ESP_OK;
}

bool bh1750_is_present(void)
{
    return s_present;
}

esp_err_t bh1750_read_lux(float *out_lux)
{
    if (out_lux == NULL) return ESP_ERR_INVALID_ARG;
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;

    /* One-shot mode powers the sensor down after every measurement, so each
     * reading is: power on -> trigger -> wait for conversion -> read 2 bytes. */
    esp_err_t rc = bh1750_write_cmd(BH1750_CMD_POWER_ON);
    if (rc != ESP_OK) {
        s_present = false;
        return rc;
    }

    rc = bh1750_write_cmd(BH1750_CMD_ONE_TIME_H_RES);
    if (rc != ESP_OK) {
        s_present = false;
        return rc;
    }

    vTaskDelay(pdMS_TO_TICKS(BH1750_MEAS_DELAY_MS));

    uint8_t buf[2] = {0, 0};
    rc = i2c_master_receive(s_dev, buf, sizeof(buf), BH1750_I2C_TIMEOUT_MS);
    if (rc != ESP_OK) {
        s_present = false;
        return rc;
    }

    uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
    *out_lux = (float)raw / BH1750_LUX_DIVISOR;
    s_present = true;
    return ESP_OK;
}
