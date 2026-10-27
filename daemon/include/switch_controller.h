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
