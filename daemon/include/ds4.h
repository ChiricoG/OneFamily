/*
 * DualShock 4 Controller Data Structures
 * Uses OpenOrbis definitions from orbis/_types/pad.h
 */

#ifndef DS4_H
#define DS4_H

#include <stdint.h>

// Include the actual OpenOrbis pad types
#include <orbis/_types/pad.h>

/*
 * Re-export OrbisPadButton values with DS4_ prefix for clarity
 * These match the actual PS4 SDK values
 */
#define DS4_BUTTON_L3           ORBIS_PAD_BUTTON_L3         // 0x0002
#define DS4_BUTTON_R3           ORBIS_PAD_BUTTON_R3         // 0x0004
#define DS4_BUTTON_OPTIONS      ORBIS_PAD_BUTTON_OPTIONS    // 0x0008
#define DS4_BUTTON_DPAD_UP      ORBIS_PAD_BUTTON_UP         // 0x0010
#define DS4_BUTTON_DPAD_RIGHT   ORBIS_PAD_BUTTON_RIGHT      // 0x0020
#define DS4_BUTTON_DPAD_DOWN    ORBIS_PAD_BUTTON_DOWN       // 0x0040
#define DS4_BUTTON_DPAD_LEFT    ORBIS_PAD_BUTTON_LEFT       // 0x0080
#define DS4_BUTTON_L2           ORBIS_PAD_BUTTON_L2         // 0x0100
#define DS4_BUTTON_R2           ORBIS_PAD_BUTTON_R2         // 0x0200
#define DS4_BUTTON_L1           ORBIS_PAD_BUTTON_L1         // 0x0400
#define DS4_BUTTON_R1           ORBIS_PAD_BUTTON_R1         // 0x0800
#define DS4_BUTTON_TRIANGLE     ORBIS_PAD_BUTTON_TRIANGLE   // 0x1000
#define DS4_BUTTON_CIRCLE       ORBIS_PAD_BUTTON_CIRCLE     // 0x2000
#define DS4_BUTTON_CROSS        ORBIS_PAD_BUTTON_CROSS      // 0x4000
#define DS4_BUTTON_SQUARE       ORBIS_PAD_BUTTON_SQUARE     // 0x8000
#define DS4_BUTTON_TOUCHPAD     ORBIS_PAD_BUTTON_TOUCH_PAD  // 0x100000

// Share button - not in enum, typically 0x01
#define DS4_BUTTON_SHARE        0x0001

// PS button - not in standard enum
#define DS4_BUTTON_PS           0x010000

/*
 * DS4 Analog stick constants
 */
#define DS4_STICK_MIN       0
#define DS4_STICK_CENTER    128
#define DS4_STICK_MAX       255

/*
 * DS4 Trigger constants
 */
#define DS4_TRIGGER_MIN     0
#define DS4_TRIGGER_MAX     255

/*
 * D-Pad direction enum (for conversion utilities)
 */
typedef enum {
    DS4_DPAD_N      = 0,    // North (Up)
    DS4_DPAD_NE     = 1,    // North-East
    DS4_DPAD_E      = 2,    // East (Right)
    DS4_DPAD_SE     = 3,    // South-East
    DS4_DPAD_S      = 4,    // South (Down)
    DS4_DPAD_SW     = 5,    // South-West
    DS4_DPAD_W      = 6,    // West (Left)
    DS4_DPAD_NW     = 7,    // North-West
    DS4_DPAD_NONE   = 8     // Released/Neutral
} Ds4DpadDirection;

/*
 * Convert D-pad bits to button flags
 * Input: 4-bit D-pad (bit0=up, bit1=down, bit2=left, bit3=right)
 * Output: OrbisPadButton flags
 */
static inline uint32_t dpad_bits_to_buttons(uint8_t dpad_bits) {
    uint32_t buttons = 0;

    if (dpad_bits & 0x01) buttons |= DS4_BUTTON_DPAD_UP;
    if (dpad_bits & 0x02) buttons |= DS4_BUTTON_DPAD_DOWN;
    if (dpad_bits & 0x04) buttons |= DS4_BUTTON_DPAD_LEFT;
    if (dpad_bits & 0x08) buttons |= DS4_BUTTON_DPAD_RIGHT;

    return buttons;
}

#endif // DS4_H
