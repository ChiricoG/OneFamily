/*
 * Nintendo Switch Input-Only Controller Support
 * For PDP Rock Candy and similar third-party Switch controllers
 *
 * Based on SDL hidapi_switch.c SwitchInputOnlyControllerStatePacket_t
 */

#ifndef SWITCH_CONTROLLER_H
#define SWITCH_CONTROLLER_H

#include <stdint.h>

// USB IDs for supported controllers
#define SWITCH_ROCKCAND_VID  0x0e6f
#define SWITCH_ROCKCANDY_PID 0x0187

// Official Nintendo Switch IDs
#define SWITCH_PRO_VID                 0x057E
#define SWITCH_PRO_PID                 0x2009
#define SWITCH_JOYCON_GRIP_PID         0x200E
#define SWITCH_SNES_PID                0x2017
#define SWITCH_GENESIS_PID             0x2019
#define SWITCH_N64_PID                 0x201E

// Report size
#define SWITCH_INPUT_ONLY_REPORT_SIZE 7

/*
 * Switch Input-Only Controller Report Structure
 * Total: 7 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t buttons0;       // Face buttons, shoulders, triggers
    uint8_t buttons1;       // Menu buttons, stick clicks
    uint8_t hat;            // D-pad (0-7 = directions, 8+ = centered)
    uint8_t left_stick_x;   // 0-255, center = 128
    uint8_t left_stick_y;   // 0-255, center = 128
    uint8_t right_stick_x;  // 0-255, center = 128
    uint8_t right_stick_y;  // 0-255, center = 128
} SwitchInputOnlyReport;

/*
 * Switch Pro Controller Full Report Structure (Reports 0x30, 0x21, 0x31)
 */
typedef struct __attribute__((packed)) {
    uint8_t report_id;          // 0x30, 0x21, 0x31
    uint8_t timer;              // Packet counter
    uint8_t battery_conn;       // Battery level & connection status
    uint8_t btn_right;          // Y, X, B, A, SR, SL, R, ZR
    uint8_t btn_shared;         // Minus, Plus, R3, L3, Home, Capture
    uint8_t btn_left;           // Down, Up, Right, Left, SR, SL, L, ZL
    uint8_t left_stick[3];      // 12-bit X, 12-bit Y
    uint8_t right_stick[3];     // 12-bit X, 12-bit Y
    uint8_t vibrator;           // Vibrator report
} SwitchFullReport;

/*
 * Switch Pro Controller Simple Report Structure (Report 0x3F)
 */
typedef struct __attribute__((packed)) {
    uint8_t report_id;         // 0x3F
    uint8_t buttons0;          // Y(1), B(2), A(4), X(8), L(16), R(32), ZL(64), ZR(128)
    uint8_t buttons1;          // Minus(1), Plus(2), L3(4), R3(8), Home(16), Capture(32)
    uint8_t hat;               // D-pad (0-7 directions, 8 centered)
    int16_t left_stick_x;
    int16_t left_stick_y;
    int16_t right_stick_x;
    int16_t right_stick_y;
} SwitchSimpleReport;

// Button byte 0 masks
#define SWITCH_BTN_Y           0x01  // West (Square position)
#define SWITCH_BTN_B           0x02  // South (Cross position)
#define SWITCH_BTN_A           0x04  // East (Circle position)
#define SWITCH_BTN_X           0x08  // North (Triangle position)
#define SWITCH_BTN_L           0x10  // Left shoulder (L1)
#define SWITCH_BTN_R           0x20  // Right shoulder (R1)
#define SWITCH_BTN_ZL          0x40  // Left trigger (L2) - digital
#define SWITCH_BTN_ZR          0x80  // Right trigger (R2) - digital

// Button byte 1 masks
#define SWITCH_BTN_MINUS       0x01  // Select/Share
#define SWITCH_BTN_PLUS        0x02  // Start/Options
#define SWITCH_BTN_L3          0x04  // Left stick click
#define SWITCH_BTN_R3          0x08  // Right stick click
#define SWITCH_BTN_HOME        0x10  // Home/PS button
#define SWITCH_BTN_CAPTURE     0x20  // Capture (unused on PS4)

// Hat/D-pad values
#define SWITCH_HAT_UP          0
#define SWITCH_HAT_UP_RIGHT    1
#define SWITCH_HAT_RIGHT       2
#define SWITCH_HAT_DOWN_RIGHT  3
#define SWITCH_HAT_DOWN        4
#define SWITCH_HAT_DOWN_LEFT   5
#define SWITCH_HAT_LEFT        6
#define SWITCH_HAT_UP_LEFT     7
#define SWITCH_HAT_CENTERED    8  // 8 or higher = no direction

/*
 * Helper to check if controller is an official Switch Pro / Joy-Con controller
 */
static inline int is_switch_pro_controller(uint16_t vid, uint16_t pid) {
    if (vid == SWITCH_PRO_VID) {
        if (pid == SWITCH_PRO_PID || pid == SWITCH_JOYCON_GRIP_PID ||
            pid == SWITCH_SNES_PID || pid == SWITCH_GENESIS_PID || pid == SWITCH_N64_PID) {
            return 1;
        }
    }
    return 0;
}

/*
 * Helper to check if controller is a Switch Input-Only type
 */
static inline int is_switch_input_only_controller(uint16_t vid, uint16_t pid) {
    // PDP Rock Candy
    if (vid == 0x0e6f && pid == 0x0187) return 1;
    // PDP Faceoff Wired Pro
    if (vid == 0x0e6f && pid == 0x0180) return 1;
    // PDP Faceoff Deluxe Wired Pro
    if (vid == 0x0e6f && pid == 0x0181) return 1;
    // PDP Wired Fight Pad Pro
    if (vid == 0x0e6f && pid == 0x0185) return 1;

    return 0;
}

#endif // SWITCH_CONTROLLER_H
