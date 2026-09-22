/**
 * @file ld2412_enhanced.h
 * @brief Enhanced LD2412C mmWave Radar Driver for ESP32-C6 Zigbee Sensor
 *
 * This enhanced driver extracts ALL available data from the LD2412C sensor:
 * - Target states (moving, static, occupancy)
 * - Distance measurements (moving, static, detection)
 * - Energy values (moving, static)
 * - Per-gate energy values (engineering mode)
 * - Per-gate sensitivity configuration
 * - Firmware version
 *
 * Based on SmartHomeScene SHS01 firmware with significant additions.
 * Protocol reference: HLK-LD2412 Serial Communication Protocol v1.02
 *
 * @author Enhanced by Claude for Thibault
 * @date 2025
 */

#ifndef LD2412_ENHANCED_H
#define LD2412_ENHANCED_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CONFIGURATION CONSTANTS
 * ============================================================================ */

#define LD2412_UART_NUM             UART_NUM_1
#define LD2412_UART_TX_PIN          5
#define LD2412_UART_RX_PIN          4
#define LD2412_UART_BAUD            115200      /* LD2412 default; LD2410 used 256000 */
#define LD2412_UART_BUF_SIZE        512

#define LD2412_MAX_GATES            14          // Gates 0-13 (LD2410 had 9)
#define LD2412_GATE_RESOLUTION_CM   75          // Default resolution 0.75m; see DISTANCE_RES below

/* Frame markers */
#define LD2412_DATA_FRAME_HEADER    0xF4F3F2F1  // Data output frame header
#define LD2412_DATA_FRAME_FOOTER    0xF8F7F6F5  // Data output frame footer
#define LD2412_CMD_FRAME_HEADER     0xFDFCFBFA  // Command frame header
#define LD2412_CMD_FRAME_FOOTER     0x04030201  // Command frame footer

/* Data types in report frames */
#define LD2412_DATA_TYPE_ENGINEERING    0x01
#define LD2412_DATA_TYPE_BASIC          0x02

/* Command words.
 *
 * The frame envelope is identical to the LD2410 (FD FC FB FA | len | cmd |
 * params | 04 03 02 01) and the ACK command word is still cmd | 0x0100.
 * Only the config commands themselves differ: the LD2410's 0x0060/0x0061/
 * 0x0064 group is replaced by the 0x0002/0x0012 basic-config pair and a
 * separate motion/static gate-threshold pair.
 */
#define LD2412_CMD_ENABLE_CONFIG        0x00FF      /* same as LD2410 */
#define LD2412_CMD_END_CONFIG           0x00FE      /* same as LD2410 */
#define LD2412_CMD_SET_BASIC_CONFIG     0x0002      /* min gate, max gate, timeout */
#define LD2412_CMD_READ_BASIC_CONFIG    0x0012
#define LD2412_CMD_SET_MOTION_GATE_SENS 0x0003      /* 14 motion thresholds */
#define LD2412_CMD_READ_MOTION_GATE_SENS 0x0013
#define LD2412_CMD_SET_STATIC_GATE_SENS 0x0004      /* 14 static thresholds */
#define LD2412_CMD_READ_STATIC_GATE_SENS 0x0014
#define LD2412_CMD_SET_DISTANCE_RES     0x0001      /* 0x00=0.75m 0x01=0.5m 0x03=0.2m */
#define LD2412_CMD_READ_DISTANCE_RES    0x0011
#define LD2412_CMD_SET_LIGHT_CONTROL    0x000C
#define LD2412_CMD_READ_LIGHT_CONTROL   0x001C
#define LD2412_CMD_BG_CORRECTION        0x000B      /* start dynamic background correction */
#define LD2412_CMD_READ_BG_CORRECTION   0x001B      /* non-zero reply = still running */
#define LD2412_CMD_ENABLE_ENGINEERING   0x0062      /* same as LD2410 */
#define LD2412_CMD_DISABLE_ENGINEERING  0x0063      /* same as LD2410 */
#define LD2412_CMD_READ_FIRMWARE        0x00A0      /* same as LD2410 */
#define LD2412_CMD_SET_BAUD             0x00A1      /* same as LD2410 */
#define LD2412_CMD_FACTORY_RESET        0x00A2      /* same as LD2410 */
#define LD2412_CMD_RESTART              0x00A3      /* same as LD2410 */
#define LD2412_CMD_BLUETOOTH            0x00A4      /* LD2412 only */
#define LD2412_CMD_READ_MAC             0x00A5      /* LD2412 only */

/* Distance resolution values for LD2412_CMD_SET_DISTANCE_RES */
#define LD2412_DIST_RES_075M            0x00
#define LD2412_DIST_RES_050M            0x01
#define LD2412_DIST_RES_020M            0x03

/* Basic-config parameter slots for LD2412_CMD_SET_BASIC_CONFIG */
#define LD2412_PARAM_MIN_GATE           0x0000
#define LD2412_PARAM_MAX_GATE           0x0001
#define LD2412_PARAM_TIMEOUT            0x0002

/* Byte offsets into a report frame, counted from the frame header.
 * Basic frame matches the LD2410 through byte 14; engineering differs:
 * the LD2410 carries max-gate bytes at 17/18 before the energies, the
 * LD2412 starts its 14 motion energies at 17 with no preamble.
 */
#define LD2412_OFF_DATA_TYPE            6
#define LD2412_OFF_HEAD_MARKER          7           /* must be 0xAA */
#define LD2412_OFF_TARGET_STATE         8
#define LD2412_OFF_MOVING_DIST_LOW      9
#define LD2412_OFF_MOVING_ENERGY        11
#define LD2412_OFF_STATIC_DIST_LOW      12
#define LD2412_OFF_STATIC_ENERGY        14
/* No detection-distance field: where the LD2410 puts one at 15-16, the
 * LD2412 puts the max-gate preamble. The basic frame therefore carries 11
 * in-frame bytes, not 13. */
#define LD2412_OFF_MAX_MOVING_GATE      15
#define LD2412_OFF_MAX_STATIC_GATE      16
#define LD2412_OFF_MOVING_GATES         17          /* 14 bytes, gates 0-13 */
#define LD2412_OFF_STATIC_GATES         31          /* 14 bytes, gates 0-13 */
#define LD2412_OFF_LIGHT                45

/* Target state bits */
#define LD2412_STATE_NO_TARGET          0x00
#define LD2412_STATE_MOVING             0x01
#define LD2412_STATE_STATIC             0x02
#define LD2412_STATE_MOVING_AND_STATIC  0x03
/* 0x04 and above are dynamic-background-correction status codes rather than
 * an occupancy bitmask, so they must never be tested bitwise: 0x05 would
 * read as a moving target and 0x06 as a static one. */
#define LD2412_STATE_BG_FIRST           0x04

/* ============================================================================
 * DATA STRUCTURES
 * ============================================================================ */

/**
 * @brief Per-gate sensitivity settings
 */
typedef struct {
    uint8_t move_sensitivity;   // Motion sensitivity (0-100)
    uint8_t still_sensitivity;  // Static sensitivity (0-100)
} ld2412_gate_config_t;

/**
 * @brief Per-gate energy readings (engineering mode)
 */
typedef struct {
    uint8_t move_energy;        // Motion energy value (0-100)
    uint8_t still_energy;       // Static energy value (0-100)
} ld2412_gate_energy_t;

/**
 * @brief Firmware version information
 */
typedef struct {
    uint8_t major;              // Major version
    uint8_t minor;              // Minor version
    uint32_t build;             // Build number (date encoded)
    bool valid;                 // Whether version info is valid
} ld2412_firmware_t;

/**
 * @brief Basic target data (always available)
 */
typedef struct {
    uint8_t target_state;       // 0=none, 1=moving, 2=static, 3=both
    uint8_t bg_correction_state;// Raw state byte when >= 0x04, else 0
    uint16_t moving_distance;   // Moving target distance in cm
    uint8_t moving_energy;      // Moving target energy (0-100)
    uint16_t static_distance;   // Static target distance in cm
    uint8_t static_energy;      // Static target energy (0-100)
    uint16_t detection_distance;// Detection distance in cm
} ld2412_target_data_t;

/**
 * @brief Engineering mode data (per-gate values)
 */
typedef struct {
    uint8_t max_moving_gate;    // Mirrored from config (absent from the LD2412 frame)
    uint8_t max_static_gate;    // Mirrored from config (absent from the LD2412 frame)
    ld2412_gate_energy_t gates[LD2412_MAX_GATES];  // Energy per gate
    uint8_t light_level;        // Onboard photosensor 0-255 (LD2412 only)
    bool valid;                 // Whether engineering data is valid
} ld2412_engineering_data_t;

/**
 * @brief Configuration parameters
 */
typedef struct {
    uint8_t min_gate;           // Nearest gate considered (0-13)
    uint8_t max_gate;           // Furthest gate considered (0-13)
    uint16_t timeout_seconds;   // No-one duration in seconds
    ld2412_gate_config_t gates[LD2412_MAX_GATES];  // Per-gate sensitivity
    bool valid;                 // Whether config is valid
} ld2412_config_t;

/**
 * @brief Complete sensor state
 */
typedef struct {
    // Basic target data
    ld2412_target_data_t target;

    // Derived boolean states (with cooldown applied)
    bool moving_detected;
    bool static_detected;
    bool occupancy_detected;

    // Engineering mode data
    ld2412_engineering_data_t engineering;
    bool engineering_mode_enabled;

    // Configuration
    ld2412_config_t config;

    // Firmware info
    ld2412_firmware_t firmware;

    // Connection status
    bool connected;
    uint32_t last_frame_time;
    uint32_t frame_count;
    uint32_t error_count;

    // Cooldown state
    uint32_t moving_cooldown_until;
    uint16_t moving_cooldown_seconds;
    uint16_t occupancy_clear_delay;
} ld2412_state_t;

/* ============================================================================
 * CALLBACK TYPES
 * ============================================================================ */

/**
 * @brief Callback for target state changes
 */
typedef void (*ld2412_state_callback_t)(const ld2412_state_t *state);

/**
 * @brief Callback for distance updates
 */
typedef void (*ld2412_distance_callback_t)(
    uint16_t moving_distance,
    uint16_t static_distance,
    uint16_t detection_distance
);

/**
 * @brief Callback for energy updates
 */
typedef void (*ld2412_energy_callback_t)(
    uint8_t moving_energy,
    uint8_t static_energy
);

/**
 * @brief Callback for engineering mode gate data
 */
typedef void (*ld2412_gate_callback_t)(const ld2412_engineering_data_t *data);

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

/**
 * @brief Initialize the LD2412 driver
 * @return ESP_OK on success
 */
esp_err_t ld2412_init(void);

/**
 * @brief Deinitialize the LD2412 driver
 */
void ld2412_deinit(void);

/**
 * @brief Get current sensor state
 * @return Pointer to current state (read-only)
 */
const ld2412_state_t* ld2412_get_state(void);

/**
 * @brief Process incoming UART data (call from main loop or task)
 */
void ld2412_process(void);

/* Configuration Commands */

/**
 * @brief Enable engineering mode (per-gate energy values)
 * @return ESP_OK on success
 */
esp_err_t ld2412_enable_engineering_mode(void);

/**
 * @brief Disable engineering mode
 * @return ESP_OK on success
 */
esp_err_t ld2412_disable_engineering_mode(void);

/**
 * @brief Set maximum detection gates and timeout
 * @param max_moving_gate Max gate for motion (0-8)
 * @param max_static_gate Max gate for static (2-8)
 * @param timeout_seconds No-one duration in seconds
 * @return ESP_OK on success
 */
esp_err_t ld2412_set_basic_config(
    uint8_t max_moving_gate,
    uint8_t max_static_gate,
    uint16_t timeout_seconds
);

/**
 * @brief Set sensitivity for a specific gate
 * @param gate Gate number (0-8)
 * @param move_sensitivity Motion sensitivity (0-100)
 * @param still_sensitivity Static sensitivity (0-100)
 * @return ESP_OK on success
 */
esp_err_t ld2412_set_gate_sensitivity(
    uint8_t gate,
    uint8_t move_sensitivity,
    uint8_t still_sensitivity
);

/**
 * @brief Set sensitivity for all gates at once
 * @param move_sensitivity Motion sensitivity (0-100)
 * @param still_sensitivity Static sensitivity (0-100)
 * @return ESP_OK on success
 */
esp_err_t ld2412_set_all_sensitivity(
    uint8_t move_sensitivity,
    uint8_t still_sensitivity
);

/**
 * @brief Read current configuration from sensor
 * @return ESP_OK on success
 */
esp_err_t ld2412_read_config(void);

/**
 * @brief Read firmware version
 * @return ESP_OK on success
 */
esp_err_t ld2412_read_firmware_version(void);

/**
 * @brief Factory reset the sensor
 * @return ESP_OK on success
 */
esp_err_t ld2412_factory_reset(void);

/**
 * @brief Restart the sensor
 * @return ESP_OK on success
 */
esp_err_t ld2412_restart(void);

/* Cooldown Configuration */

/**
 * @brief Set movement detection cooldown
 * @param seconds Cooldown time (0 to disable)
 */
void ld2412_set_moving_cooldown(uint16_t seconds);

/**
 * @brief Set occupancy clear delay
 * @param seconds Delay before occupancy clears
 */
void ld2412_set_occupancy_delay(uint16_t seconds);

/* Callback Registration */

/**
 * @brief Register callback for state changes
 */
void ld2412_register_state_callback(ld2412_state_callback_t callback);

/**
 * @brief Register callback for distance updates
 */
void ld2412_register_distance_callback(ld2412_distance_callback_t callback);

/**
 * @brief Register callback for energy updates
 */
void ld2412_register_energy_callback(ld2412_energy_callback_t callback);

/**
 * @brief Register callback for gate data (engineering mode)
 */
void ld2412_register_gate_callback(ld2412_gate_callback_t callback);

/* Utility Functions */

/**
 * @brief Convert gate number to distance in cm
 */
static inline uint16_t ld2412_gate_to_cm(uint8_t gate) {
    return gate * LD2412_GATE_RESOLUTION_CM;
}

/**
 * @brief Convert distance in cm to gate number
 */
static inline uint8_t ld2412_cm_to_gate(uint16_t cm) {
    return cm / LD2412_GATE_RESOLUTION_CM;
}

/**
 * @brief Check if sensor is connected and responding
 */
bool ld2412_is_connected(void);

/**
 * @brief Start dynamic background correction
 *
 * The LD2412 learns the static clutter in front of it and subtracts it.
 * It takes several seconds and the module gives no completion signal, so
 * poll ld2412_bg_correction_running() to find out when it has finished.
 *
 * @return ESP_OK on success
 */
esp_err_t ld2412_start_bg_correction(void);

/**
 * @brief Query whether background correction is still running
 *
 * @param[out] running Set true while the correction is in progress
 * @return ESP_OK on success
 */
esp_err_t ld2412_bg_correction_running(bool *running);

/**
 * @brief Get string representation of target state
 */
const char* ld2412_state_to_string(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif /* LD2412_ENHANCED_H */
