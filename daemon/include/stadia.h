/*
 * Google Stadia Controller USB Protocol Definitions
 * Hardware IDs: VID 0x18D1, PID 0x9400
 */

#ifndef STADIA_H
#define STADIA_H

#include <stdint.h>

#define GOOGLE_VID                  0x18D1
#define STADIA_CONTROLLER_PID       0x9400

#define STADIA_REPORT_ID_INPUT      0x03
#define STADIA_REPORT_ID_RUMBLE     0x05

/*
 * Stadia USB Input Report (10 bytes)
 * When Report ID is included at byte 0 (0x03):
 *   [0] report_id: 0x03
 *   [1] dpad: Hat switch (0=Up, 1=UpRight, 2=Right, 3=DownRight, 4=Down, 5=DownLeft, 6=Left, 7=UpLeft, 8=Release)
 *   [2] buttons_main:
 *         bit 0 (0x01): Capture button (Screenshot)
 *         bit 1 (0x02): Google Assistant button
 *         bit 2 (0x04): L2 digital button
 *         bit 3 (0x08): R2 digital button
 *         bit 4 (0x10): Stadia button (Logo / Home)
 *         bit 5 (0x20): Menu button (Three lines / Start / Options)
 *         bit 6 (0x40): Options button (Dots / Select / Back / Share)
 *         bit 7 (0x80): R3 (Right stick click)
 *   [3] buttons_extra:
 *         bit 0 (0x01): L3 (Left stick click)
 *         bit 1 (0x02): R1 (Right Bumper)
 *         bit 2 (0x04): L1 (Left Bumper)
 *         bit 3 (0x08): Y button
 *         bit 4 (0x10): X button
 *         bit 5 (0x20): B button
 *         bit 6 (0x40): A button
 *         bit 7 (0x80): Unused
 *   [4] left_stick_x: (0-255, center 128)
 *   [5] left_stick_y: (0-255, center 128)
 *   [6] right_stick_x: (0-255, center 128)
 *   [7] right_stick_y: (0-255, center 128)
 *   [8] left_trigger: (0-255)
 *   [9] right_trigger: (0-255)
 */
typedef struct __attribute__((packed)) {
    uint8_t report_id;        // 0x03
    uint8_t dpad;             // Hat switch (0..7, 8 = release)
    uint8_t buttons_main;     // Capture(0x01), Assistant(0x02), L2(0x04), R2(0x08), Stadia(0x10), Menu(0x20), Options(0x40), R3(0x80)
    uint8_t buttons_extra;    // L3(0x01), R1(0x02), L1(0x04), Y(0x08), X(0x10), B(0x20), A(0x40)
    uint8_t left_stick_x;     // LX (0-255, center 128)
    uint8_t left_stick_y;     // LY (0-255, center 128)
    uint8_t right_stick_x;    // RX (0-255, center 128)
    uint8_t right_stick_y;    // RY (0-255, center 128)
    uint8_t left_trigger;     // LT (0-255)
    uint8_t right_trigger;    // RT (0-255)
} StadiaInputReport;

#endif // STADIA_H
