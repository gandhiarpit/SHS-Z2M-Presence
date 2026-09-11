/*
 * SPDX-FileCopyrightText: 2021-2025
 * SPDX-License-Identifier: CC0-1.0
 *
 * LTR390-UV-01 ambient light + UV sensor driver (I2C)
 */

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ltr390.h"

static const char *LTR390_TAG = "LTR390";

/* Register map (LTR-390UV-01 datasheet) */
#define LTR390_REG_MAIN_CTRL        0x00
#define LTR390_REG_MEAS_RATE        0x04
#define LTR390_REG_GAIN             0x05
#define LTR390_REG_PART_ID          0x06
#define LTR390_REG_MAIN_STATUS      0x07
#define LTR390_REG_ALSDATA          0x0D    /* 3 bytes, LSB first */
#define LTR390_REG_UVSDATA          0x10    /* 3 bytes, LSB first */

/* MAIN_CTRL bits */
#define LTR390_CTRL_ENABLE          (1 << 1)
#define LTR390_CTRL_MODE_UVS        (1 << 3)    /* clear = ALS, set = UVS */

/* MAIN_STATUS bit 3 = new data ready */
#define LTR390_STATUS_DATA_READY    (1 << 3)

/* PART_ID upper nibble identifies the part; the lower nibble is the revision */
#define LTR390_PART_NUMBER          0x0B

/* Gain register values and their multipliers */
#define LTR390_GAIN_3               0x01
#define LTR390_GAIN_18              0x04
#define LTR390_GAIN_3_VALUE         3.0f
#define LTR390_GAIN_18_VALUE        18.0f

/* MEAS_RATE: resolution in bits [6:4], measurement rate in bits [2:0].
 * Resolution 2 = 18-bit / 100 ms, 0 = 20-bit / 400 ms.
 * Rate 2 = 100 ms, 3 = 200 ms - the rate must not be shorter than the
 * integration time or the chip silently keeps the previous conversion. */
#define LTR390_MEAS_18BIT_100MS     ((2 << 4) | 0x02)
#define LTR390_MEAS_20BIT_400MS     ((0 << 4) | 0x04)

/* Integration time expressed the way the datasheet's formulas want it:
 * lux divides by (int_time_ms / 100), UVI scales sensitivity by (int_time_ms / 400). */
#define LTR390_INT_100MS_SCALE      1.0f
#define LTR390_INT_400MS_SCALE      4.0f

/* Rated UV sensitivity: 1 UVI per 2300 counts at 18x gain, 20-bit resolution */
#define LTR390_UV_SENSITIVITY       2300.0f

/* Conversion waits: integration time plus the datasheet's wake-up and settle allowance */
#define LTR390_WAKEUP_MS            10
#define LTR390_SETTLE_MS            5

#define LTR390_I2C_TIMEOUT_MS       100

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

/* A NACK or bus fault leaves the I2C state machine wedged: every later
 * transaction then returns ESP_ERR_INVALID_STATE regardless of the hardware.
 * Reset the bus so the next attempt starts clean instead of inheriting it. */
static void ltr390_bus_recover(const char *what)
{
    if (s_bus == NULL) return;
    esp_err_t rc = i2c_master_bus_reset(s_bus);
    ESP_LOGW(LTR390_TAG, "bus reset after %s: %s", what, esp_err_to_name(rc));
}

static esp_err_t ltr390_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), LTR390_I2C_TIMEOUT_MS);
}

static esp_err_t ltr390_read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, LTR390_I2C_TIMEOUT_MS);
}

esp_err_t ltr390_init(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) return ESP_ERR_INVALID_ARG;
    s_bus = bus;

    if (s_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = LTR390_ADDR,
            .scl_speed_hz = 100000,
        };
        esp_err_t rc = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
        if (rc != ESP_OK) {
            ESP_LOGE(LTR390_TAG, "bus_add_device failed: %s", esp_err_to_name(rc));
            s_dev = NULL;
            return rc;
        }
    }

    uint8_t part_id = 0;
    esp_err_t rc = ltr390_read_regs(LTR390_REG_PART_ID, &part_id, 1);
    if (rc != ESP_OK) return rc;

    if ((part_id >> 4) != LTR390_PART_NUMBER) {
        ESP_LOGW(LTR390_TAG, "unexpected PART_ID 0x%02X at 0x%02X - not an LTR390",
                 part_id, LTR390_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(LTR390_TAG, "LTR390 detected at 0x%02X (PART_ID 0x%02X)", LTR390_ADDR, part_id);
    return ESP_OK;
}

/* Configure one mode, wait out the conversion, and return the 20-bit sample.
 * Every transaction is logged on failure: ESP_ERR_INVALID_STATE covers both a
 * NACK and a bus fault in IDF 5.x, so knowing which step failed is the only way
 * to tell a wiring problem from a register-sequence problem. */
static esp_err_t ltr390_measure(bool uvs_mode, uint8_t gain, uint8_t meas_rate,
                                uint32_t int_time_ms, uint32_t *out_raw)
{
    const char *mode = uvs_mode ? "UVS" : "ALS";

    if (s_dev == NULL) {
        ESP_LOGW(LTR390_TAG, "%s: no device handle", mode);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = ltr390_write_reg(LTR390_REG_GAIN, gain);
    if (rc != ESP_OK) {
        ESP_LOGW(LTR390_TAG, "%s: write GAIN=0x%02X failed: %s", mode, gain, esp_err_to_name(rc));
        ltr390_bus_recover("GAIN write");
        return rc;
    }

    rc = ltr390_write_reg(LTR390_REG_MEAS_RATE, meas_rate);
    if (rc != ESP_OK) {
        ESP_LOGW(LTR390_TAG, "%s: write MEAS_RATE=0x%02X failed: %s", mode, meas_rate, esp_err_to_name(rc));
        ltr390_bus_recover("MEAS_RATE write");
        return rc;
    }

    uint8_t ctrl = LTR390_CTRL_ENABLE | (uvs_mode ? LTR390_CTRL_MODE_UVS : 0);
    rc = ltr390_write_reg(LTR390_REG_MAIN_CTRL, ctrl);
    if (rc != ESP_OK) {
        ESP_LOGW(LTR390_TAG, "%s: write MAIN_CTRL=0x%02X failed: %s", mode, ctrl, esp_err_to_name(rc));
        ltr390_bus_recover("MAIN_CTRL write");
        return rc;
    }

    /* The new I2C driver can report ESP_OK for a transaction that did not
     * actually land, so read the three registers back: if they do not hold
     * what was just written, the writes are the fault, not the read. */
    uint8_t cfg[3] = {0xFF, 0xFF, 0xFF};
    if (ltr390_read_regs(LTR390_REG_MAIN_CTRL, &cfg[0], 1) == ESP_OK &&
        ltr390_read_regs(LTR390_REG_MEAS_RATE, &cfg[1], 1) == ESP_OK &&
        ltr390_read_regs(LTR390_REG_GAIN,      &cfg[2], 1) == ESP_OK) {
        ESP_LOGD(LTR390_TAG, "%s: readback MAIN_CTRL=0x%02X (wrote 0x%02X) MEAS_RATE=0x%02X (0x%02X) GAIN=0x%02X (0x%02X)",
                 mode, cfg[0], ctrl, cfg[1], meas_rate, cfg[2], gain);
    } else {
        ESP_LOGW(LTR390_TAG, "%s: config readback failed", mode);
        ltr390_bus_recover("config readback");
        return ESP_ERR_INVALID_STATE;
    }

    vTaskDelay(pdMS_TO_TICKS(int_time_ms + LTR390_WAKEUP_MS + LTR390_SETTLE_MS));

    /* One retry: the first conversion after a mode switch is occasionally still
     * in flight when the nominal integration time expires. */
    uint8_t status = 0;
    bool ready = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        rc = ltr390_read_regs(LTR390_REG_MAIN_STATUS, &status, 1);
        if (rc != ESP_OK) {
            ESP_LOGW(LTR390_TAG, "%s: read MAIN_STATUS failed: %s", mode, esp_err_to_name(rc));
            ltr390_bus_recover("MAIN_STATUS read");
            return rc;
        }
        if (status & LTR390_STATUS_DATA_READY) {
            ready = true;
            break;
        }
        ESP_LOGD(LTR390_TAG, "%s: status=0x%02X not ready (attempt %d)", mode, status, attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(int_time_ms / 2 + LTR390_SETTLE_MS));
    }

    if (!ready) {
        ESP_LOGW(LTR390_TAG, "%s: data never became ready (status=0x%02X)", mode, status);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t buf[3] = {0, 0, 0};
    rc = ltr390_read_regs(uvs_mode ? LTR390_REG_UVSDATA : LTR390_REG_ALSDATA, buf, sizeof(buf));
    if (rc != ESP_OK) {
        ESP_LOGW(LTR390_TAG, "%s: read data failed: %s", mode, esp_err_to_name(rc));
        ltr390_bus_recover("data read");
        return rc;
    }

    /* 20-bit result, LSB first; the top 4 bits of the third byte are reserved */
    *out_raw = ((uint32_t)(buf[2] & 0x0F) << 16) | ((uint32_t)buf[1] << 8) | buf[0];
    ESP_LOGD(LTR390_TAG, "%s: status=0x%02X raw=%lu (%02X %02X %02X)",
             mode, status, (unsigned long)*out_raw, buf[0], buf[1], buf[2]);
    return ESP_OK;
}

esp_err_t ltr390_read_lux(float *out_lux)
{
    if (out_lux == NULL) return ESP_ERR_INVALID_ARG;

    uint32_t raw = 0;
    esp_err_t rc = ltr390_measure(false, LTR390_GAIN_3, LTR390_MEAS_18BIT_100MS, 100, &raw);
    if (rc != ESP_OK) return rc;

    /* lux = 0.6 * ALS / (gain * (int_time_ms / 100)) */
    *out_lux = (0.6f * (float)raw) / (LTR390_GAIN_3_VALUE * LTR390_INT_100MS_SCALE);
    return ESP_OK;
}

esp_err_t ltr390_read_uvi(float *out_uvi)
{
    if (out_uvi == NULL) return ESP_ERR_INVALID_ARG;

    uint32_t raw = 0;
    esp_err_t rc = ltr390_measure(true, LTR390_GAIN_18, LTR390_MEAS_20BIT_400MS, 400, &raw);
    if (rc != ESP_OK) return rc;

    /* At 18x gain and 20-bit resolution the sensitivity scale factors are both 1,
     * which is the only combination the datasheet gives an accurate formula for. */
    float sensitivity = LTR390_UV_SENSITIVITY
                      * (LTR390_GAIN_18_VALUE / 18.0f)
                      * (LTR390_INT_400MS_SCALE / 4.0f);
    *out_uvi = (float)raw / sensitivity;
    return ESP_OK;
}
