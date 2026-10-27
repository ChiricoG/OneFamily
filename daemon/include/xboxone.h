/*
 * Xbox One Controller USB Protocol Definitions
 * Based on USB Host Shield 2.0 library and reverse engineering
 */

#ifndef XBOXONE_H
#define XBOXONE_H

#include <stdint.h>

/*
 * Xbox One Input Report Structure
 * Note: Xbox One uses a different report format than Xbox 360
 *
 * Byte layout:
 *   [0]     Report type (0x20 for input report)
 *   [1]     Unknown/flags
 *   [2]     Report counter
 *   [3]     Report length
 *   [4]     Buttons low byte (Sync, unused, Menu, View, A, B, X, Y)
 *   [5]     Buttons high byte (D-pad, LB, RB, L3, R3)
 *   [6-7]   Left trigger (uint16_t, little-endian, 0-1023)
 *   [8-9]   Right trigger (uint16_t, little-endian, 0-1023)
 *   [10-11] Left stick X (int16_t, little-endian)
 *   [12-13] Left stick Y (int16_t, little-endian)
 *   [14-15] Right stick X (int16_t, little-endian)
 *   [16-17] Right stick Y (int16_t, little-endian)
 */
typedef struct __attribute__((packed)) {
    uint8_t  report_type;       // 0x20 for input report
    uint8_t  flags;             // Unknown flags
    uint8_t  counter;           // Report counter
    uint8_t  length;            // Report length
    uint8_t  buttons_low;       // Face buttons and menu
    uint8_t  buttons_high;      // D-pad, bumpers, stick clicks
    uint16_t left_trigger;      // LT analog value (0-1023)
    uint16_t right_trigger;     // RT analog value (0-1023)
    int16_t  left_stick_x;      // Left stick X (-32768 to 32767)
    int16_t  left_stick_y;      // Left stick Y (-32768 to 32767)
    int16_t  right_stick_x;     // Right stick X (-32768 to 32767)
    int16_t  right_stick_y;     // Right stick Y (-32768 to 32767)
} XboxOneReport;

/*
 * Button bit definitions for buttons_low (byte 4)
 */
#define XBOXONE_SYNC            (1 << 0)    // 0x01 - Sync button
#define XBOXONE_UNUSED1         (1 << 1)    // 0x02 - Unused
#define XBOXONE_MENU            (1 << 2)    // 0x04 - Menu (Start)
#define XBOXONE_VIEW            (1 << 3)    // 0x08 - View (Back/Select)
#define XBOXONE_A               (1 << 4)    // 0x10
#define XBOXONE_B               (1 << 5)    // 0x20
#define XBOXONE_X               (1 << 6)    // 0x40
#define XBOXONE_Y               (1 << 7)    // 0x80

/*
 * Button bit definitions for buttons_high (byte 5)
 */
#define XBOXONE_DPAD_UP         (1 << 0)    // 0x01
#define XBOXONE_DPAD_DOWN       (1 << 1)    // 0x02
#define XBOXONE_DPAD_LEFT       (1 << 2)    // 0x04
#define XBOXONE_DPAD_RIGHT      (1 << 3)    // 0x08
#define XBOXONE_LB              (1 << 4)    // 0x10 - Left bumper
#define XBOXONE_RB              (1 << 5)    // 0x20 - Right bumper
#define XBOXONE_LEFT_STICK      (1 << 6)    // 0x40 (L3)
#define XBOXONE_RIGHT_STICK     (1 << 7)    // 0x80 (R3)

/*
 * Xbox button comes in separate report (type 0x07)
 */
#define XBOXONE_REPORT_INPUT    0x20
#define XBOXONE_REPORT_GUIDE    0x07

/*
 * Trigger constants (Xbox One uses 10-bit triggers)
 */
#define XBOXONE_TRIGGER_MIN     (0)
#define XBOXONE_TRIGGER_MAX     (1023)

/*
 * Known Xbox One Controller Product IDs (VID is always 0x045E)
 */
#define XBOXONE_PID_ORIGINAL    0x02D1  // Original Xbox One controller
#define XBOXONE_PID_S_USB       0x02EA  // Xbox One S controller (USB)
#define XBOXONE_PID_S_BT        0x02E0  // Xbox One S controller (Bluetooth)
#define XBOXONE_PID_ELITE       0x02E3  // Xbox Elite controller
#define XBOXONE_PID_ELITE2      0x0B00  // Xbox Elite 2 controller
#define XBOXONE_PID_ADAPTIVE    0x0B0A  // Xbox Adaptive controller
#define XBOXONE_PID_SERIES_USB  0x0B12  // Xbox Series X|S controller (USB)
#define XBOXONE_PID_SERIES_BT   0x0B13  // Xbox Series X|S controller (Bluetooth)
#define XBOXONE_PID_2021        0x0B20  // 2021 Xbox controller

/*
 * Utility functions (inline for performance)
 */

// Check if report is valid Xbox One input report
static inline int xboxone_report_valid(const XboxOneReport* report) {
    return report->report_type == XBOXONE_REPORT_INPUT;
}

// Get D-pad as 4-bit value (up=1, down=2, left=4, right=8)
static inline uint8_t xboxone_get_dpad(const XboxOneReport* report) {
    return report->buttons_high & 0x0F;
}

// Convert 10-bit trigger to 8-bit (for DS4 compatibility)
static inline uint8_t xboxone_trigger_to_8bit(uint16_t trigger) {
    return (uint8_t)(trigger >> 2);  // 1023 -> 255
}

#endif // XBOXONE_H
