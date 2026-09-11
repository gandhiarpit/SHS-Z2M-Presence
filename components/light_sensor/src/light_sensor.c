/*
 * SPDX-FileCopyrightText: 2021-2025
 * SPDX-License-Identifier: CC0-1.0
 *
 * Ambient light sensor facade - owns the I2C bus, dispatches to BH1750 or LTR390
 */

#include <stdbool.h>

#include "esp_log.h"
#include "driver/i2c_master.h"

#include "light_sensor.h"
#include "bh1750.h"
#include "ltr390.h"

static const char *LS_TAG = "LIGHTSENS";

static i2c_master_bus_handle_t s_bus = NULL;
static light_sensor_type_t s_type = LIGHT_SENSOR_NONE;

static esp_err_t light_sensor_bus_create(void)
{
    if (s_bus != NULL) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = LIGHT_SENSOR_I2C_PORT,
        .sda_io_num = LIGHT_SENSOR_SDA_GPIO,
        .scl_io_num = LIGHT_SENSOR_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t rc = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (rc != ESP_OK) {
        ESP_LOGE(LS_TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(rc));
        s_bus = NULL;
        return rc;
    }

    ESP_LOGI(LS_TAG, "I2C bus created on SDA=GPIO%d SCL=GPIO%d",
             (int)LIGHT_SENSOR_SDA_GPIO, (int)LIGHT_SENSOR_SCL_GPIO);
    return ESP_OK;
}

esp_err_t light_sensor_init(void)
{
    esp_err_t rc = light_sensor_bus_create();
    if (rc != ESP_OK) return rc;

    /* Probe in address order. Both parts stay attached to the bus once added,
     * so a re-probe after a transient fault costs nothing. */
    if (bh1750_init(s_bus) == ESP_OK) {
        s_type = LIGHT_SENSOR_BH1750;
        return ESP_OK;
    }

    if (ltr390_init(s_bus) == ESP_OK) {
        s_type = LIGHT_SENSOR_LTR390;
        return ESP_OK;
    }

    s_type = LIGHT_SENSOR_NONE;
    return ESP_ERR_NOT_FOUND;
}

light_sensor_type_t light_sensor_type(void)
{
    return s_type;
}

const char *light_sensor_name(void)
{
    switch (s_type) {
        case LIGHT_SENSOR_BH1750: return "BH1750";
        case LIGHT_SENSOR_LTR390: return "LTR390";
        default:                  return "none";
    }
}

bool light_sensor_has_uv(void)
{
    return s_type == LIGHT_SENSOR_LTR390;
}

esp_err_t light_sensor_read_lux(float *out_lux)
{
    switch (s_type) {
        case LIGHT_SENSOR_BH1750: return bh1750_read_lux(out_lux);
        case LIGHT_SENSOR_LTR390: return ltr390_read_lux(out_lux);
        default:                  return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t light_sensor_read_uvi(float *out_uvi)
{
    if (s_type != LIGHT_SENSOR_LTR390) return ESP_ERR_NOT_SUPPORTED;
    return ltr390_read_uvi(out_uvi);
}
