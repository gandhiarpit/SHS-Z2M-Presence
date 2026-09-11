/*
 * SPDX-FileCopyrightText: 2021-2025
 * SPDX-License-Identifier: CC0-1.0
 *
 * Ambient light sensor facade - owns the I2C bus, dispatches to BH1750 or LTR390
 */

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "driver/i2c_master.h"

#include "light_sensor.h"
#include "bh1750.h"
#include "ltr390.h"

static const char *LS_TAG = "LIGHTSENS";

/* Probe timeouts. The scan uses a short one because it walks 112 addresses and
 * a dead bus times out on every single one. */
#define LS_SCAN_TIMEOUT_MS      20
#define LS_PROBE_TIMEOUT_MS     100

static i2c_master_bus_handle_t s_bus = NULL;
static light_sensor_type_t s_type = LIGHT_SENSOR_NONE;
static bool s_scanned = false;

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

/* Walk the whole 7-bit address space once and report what is out there. An
 * unsupported-but-present chip is the difference between "wired wrong" and
 * "wired fine, wrong part" - guessing between those wastes far more time than
 * this scan costs. Runs once per boot; the per-address retry below is cheap. */
static void light_sensor_scan(void)
{
    int found = 0;

    ESP_LOGI(LS_TAG, "Scanning I2C bus for devices...");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, LS_SCAN_TIMEOUT_MS) == ESP_OK) {
            const char *known = "";
            if (addr == BH1750_ADDR)      known = "  (BH1750)";
            else if (addr == 0x5C)        known = "  (BH1750, ADDR tied high - not the address this firmware uses)";
            else if (addr == LTR390_ADDR) known = "  (LTR390)";
            ESP_LOGI(LS_TAG, "  responding: 0x%02X%s", addr, known);
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGW(LS_TAG, "  nothing responded - check 3V3 power, SDA/SCL orientation and pull-ups");
    } else {
        ESP_LOGI(LS_TAG, "Scan complete: %d device(s)", found);
    }
}

esp_err_t light_sensor_init(void)
{
    esp_err_t rc = light_sensor_bus_create();
    if (rc != ESP_OK) return rc;

    if (!s_scanned) {
        light_sensor_scan();
        s_scanned = true;
    }

    /* Attach only to an address that actually answered, so a part that is absent
     * never has a device handle added or a transaction attempted against it. */
    if (i2c_master_probe(s_bus, LTR390_ADDR, LS_PROBE_TIMEOUT_MS) == ESP_OK) {
        rc = ltr390_init(s_bus);
        if (rc == ESP_OK) {
            s_type = LIGHT_SENSOR_LTR390;
            return ESP_OK;
        }
        ESP_LOGW(LS_TAG, "0x%02X answered but did not identify as an LTR390: %s",
                 LTR390_ADDR, esp_err_to_name(rc));
    }

    if (i2c_master_probe(s_bus, BH1750_ADDR, LS_PROBE_TIMEOUT_MS) == ESP_OK) {
        rc = bh1750_init(s_bus);
        if (rc == ESP_OK) {
            s_type = LIGHT_SENSOR_BH1750;
            return ESP_OK;
        }
        ESP_LOGW(LS_TAG, "0x%02X answered but would not initialise as a BH1750: %s",
                 BH1750_ADDR, esp_err_to_name(rc));
    }

    ESP_LOGW(LS_TAG, "No supported light sensor: no response at 0x%02X (LTR390) or 0x%02X (BH1750)",
             LTR390_ADDR, BH1750_ADDR);
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
