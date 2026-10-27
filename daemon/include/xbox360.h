/*
 * Xbox 360 Controller USB Protocol Definitions
 * Based on reverse engineering documentation from:
 * https://www.partsnotincluded.com/understanding-the-xbox-360-wired-controllers-usb-data/
 */

#ifndef XBOX360_H
#define XBOX360_H

#include <stdint.h>

/*
 * Xbox 360 Input Report Structure (20 bytes)
 *
 * Byte layout:
 *   [0]     Message type (always 0x00)
 *   [1]     Message length (always 0x14 = 20)
 *   [2]     Buttons low byte (D-pad, Start, Back, L3, R3)
 *   [3]     Buttons high byte (LB, RB, Guide, A, B, X, Y)
 *   [4]     Left trigger (0-255)
 *   [5]     Right trigger (0-255)
 *   [6-7]   Left stick X (int16_t, little-endian)
 *   [8-9]   Left stick Y (int16_t, little-endian)
 *   [10-11] Right stick X (int16_t, little-endian)
 *   [12-13] Right stick Y (int16_t, little-endian)
 *   [14-19] Reserved/unused
 */
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;          // Always 0x00 for input report
    uint8_t  msg_length;        // Always 0x14 (20 decimal)
    uint8_t  buttons_low;       // D-pad and system buttons
    uint8_t  buttons_high;      // Face and shoulder buttons
    uint8_t  left_trigger;      // LT analog value (0-255)
    uint8_t  right_trigger;     // RT analog value (0-255)
    int16_t  left_stick_x;      // Left stick X (-32768 to 32767)
    int16_t  left_stick_y;      // Left stick Y (-32768 to 32767)
    int16_t  right_stick_x;     // Right stick X (-32768 to 32767)
    int16_t  right_stick_y;     // Right stick Y (-32768 to 32767)
    uint8_t  reserved[6];       // Unused bytes
} Xbox360Report;

/*
 * Xbox 360 Output Report Structure (8 bytes)
 * Used for rumble motor control
 */
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;          // 0x00
    uint8_t  msg_length;        // 0x08
    uint8_t  padding1;          // 0x00
    uint8_t  left_motor;        // Large/low-frequency motor (0-255)
    uint8_t  right_motor;       // Small/high-frequency motor (0-255)
    uint8_t  padding2[3];       // 0x00, 0x00, 0x00
} Xbox360OutputReport;

/*
 * Button bit definitions for buttons_low (byte 2)
 */
#define XBOX360_DPAD_UP         (1 << 0)    // 0x01
#define XBOX360_DPAD_DOWN       (1 << 1)    // 0x02
#define XBOX360_DPAD_LEFT       (1 << 2)    // 0x04
#define XBOX360_DPAD_RIGHT      (1 << 3)    // 0x08
#define XBOX360_START           (1 << 4)    // 0x10
#define XBOX360_BACK            (1 << 5)    // 0x20
#define XBOX360_LEFT_STICK      (1 << 6)    // 0x40 (L3)
#define XBOX360_RIGHT_STICK     (1 << 7)    // 0x80 (R3)

/*
 * Button bit definitions for buttons_high (byte 3)
 */
#define XBOX360_LB              (1 << 0)    // 0x01 - Left bumper
#define XBOX360_RB              (1 << 1)    // 0x02 - Right bumper
#define XBOX360_GUIDE           (1 << 2)    // 0x04 - Xbox button
#define XBOX360_UNUSED          (1 << 3)    // 0x08 - Not used
#define XBOX360_A               (1 << 4)    // 0x10
#define XBOX360_B               (1 << 5)    // 0x20
#define XBOX360_X               (1 << 6)    // 0x40
#define XBOX360_Y               (1 << 7)    // 0x80

/*
 * Combined button masks (for 16-bit combined value)
 * Use with: uint16_t buttons = report.buttons_low | (report.buttons_high << 8)
 */
#define XBOX360_BTN_DPAD_UP     (1 << 0)
#define XBOX360_BTN_DPAD_DOWN   (1 << 1)
#define XBOX360_BTN_DPAD_LEFT   (1 << 2)
#define XBOX360_BTN_DPAD_RIGHT  (1 << 3)
#define XBOX360_BTN_START       (1 << 4)
#define XBOX360_BTN_BACK        (1 << 5)
#define XBOX360_BTN_L3          (1 << 6)
#define XBOX360_BTN_R3          (1 << 7)
#define XBOX360_BTN_LB          (1 << 8)
#define XBOX360_BTN_RB          (1 << 9)
#define XBOX360_BTN_GUIDE       (1 << 10)
#define XBOX360_BTN_A           (1 << 12)
#define XBOX360_BTN_B           (1 << 13)
#define XBOX360_BTN_X           (1 << 14)
#define XBOX360_BTN_Y           (1 << 15)

/*
 * Analog stick constants
 */
#define XBOX360_STICK_MIN       (-32768)
#define XBOX360_STICK_MAX       (32767)
#define XBOX360_STICK_CENTER    (0)

/*
 * Trigger constants
 */
#define XBOX360_TRIGGER_MIN     (0)
#define XBOX360_TRIGGER_MAX     (255)

/*
 * LED patterns for output reports
 */
typedef enum {
    XBOX360_LED_OFF         = 0x00,
    XBOX360_LED_BLINK       = 0x01,
    XBOX360_LED_FLASH1      = 0x02,
    XBOX360_LED_FLASH2      = 0x03,
    XBOX360_LED_FLASH3      = 0x04,
    XBOX360_LED_FLASH4      = 0x05,
    XBOX360_LED_ON1         = 0x06,
    XBOX360_LED_ON2         = 0x07,
    XBOX360_LED_ON3         = 0x08,
    XBOX360_LED_ON4         = 0x09,
    XBOX360_LED_ROTATE      = 0x0A,
    XBOX360_LED_BLINK_PREV  = 0x0B,
    XBOX360_LED_BLINK_SLOW  = 0x0C,
    XBOX360_LED_ALTERNATE   = 0x0D
} Xbox360LedPattern;

/*
 * Utility functions (inline for performance)
 */

// Get combined 16-bit button state
static inline uint16_t xbox360_get_buttons(const Xbox360Report* report) {
    return report->buttons_low | ((uint16_t)report->buttons_high << 8);
}

// Check if a specific button is pressed
static inline int xbox360_button_pressed(const Xbox360Report* report, uint16_t button_mask) {
    return (xbox360_get_buttons(report) & button_mask) != 0;
}

// Get D-pad as 4-bit value (up=1, down=2, left=4, right=8)
static inline uint8_t xbox360_get_dpad(const Xbox360Report* report) {
    return report->buttons_low & 0x0F;
}

// Check if report is valid (correct header)
static inline int xbox360_report_valid(const Xbox360Report* report) {
    return report->msg_type == 0x00 && report->msg_length == 0x14;
}

// Initialize output report for rumble
static inline void xbox360_init_rumble(Xbox360OutputReport* out, uint8_t left, uint8_t right) {
    out->msg_type = 0x00;
    out->msg_length = 0x08;
    out->padding1 = 0x00;
    out->left_motor = left;
    out->right_motor = right;
    out->padding2[0] = 0x00;
    out->padding2[1] = 0x00;
    out->padding2[2] = 0x00;
}

#endif // XBOX360_H
