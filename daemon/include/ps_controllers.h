/*
 * PlayStation 3 (DualShock 3 / Sixaxis) and PlayStation 5 (DualSense)
 * USB Protocol Definitions and Report Structures
 */

#ifndef PS_CONTROLLERS_H
#define PS_CONTROLLERS_H

#include <stdint.h>

#define SONY_VID                        0x054C

// DualShock 3 / Sixaxis
#define SONY_DS3_PID                    0x0268

// DualShock 4 PIDs (Handled natively by PS4 Orbis OS, daemon ignores these)
#define SONY_DS4_V1_PID                 0x05C4
#define SONY_DS4_V2_PID                 0x09CC
#define SONY_DS4_DONGLE_PID             0x0BA0

static inline int is_native_ps4_controller(uint16_t vid, uint16_t pid) {
    if (vid == SONY_VID) {
        if (pid == SONY_DS4_V1_PID || pid == SONY_DS4_V2_PID || pid == 0x05C5 || pid == SONY_DS4_DONGLE_PID) {
            return 1;
        }
    }
    return 0;
}

// Known third-party controllers operating in PS4 / DualShock 4 mode
static inline int is_ds4_compatible_controller(uint16_t vid, uint16_t pid) {
    if (vid == 0x0F0D) {         // Hori PS4 controllers & fightsticks
        if (pid == 0x0055 || pid == 0x0066 || pid == 0x0084 || pid == 0x00EE ||
            pid == 0x008A || pid == 0x0070 || pid == 0x008B || pid == 0x009C) return 1;
    }
    if (vid == 0x146B) return 1; // Nacon PS4 (Revolution Pro, Daija, Compact, etc.)
    if (vid == 0x1532) {         // Razer PS4 (Raiju, Panthera, Atrox PS4, etc.)
        if (pid == 0x1000 || pid == 0x1004 || pid == 0x1007 || pid == 0x1008 || pid == 0x1009) return 1;
    }
    if (vid == 0x0C12) return 1; // Armor3 / Zeroplus
    if (vid == 0x7545) return 1; // Brook converters & adapters
    if (vid == 0x20D6 && (pid == 0xA711 || pid == 0xA712 || pid == 0xA713)) return 1; // PowerA PS4
    if (vid == 0x24C6 && (pid == 0x5501 || pid == 0x5502 || pid == 0x5503)) return 1; // Thrustmaster / PowerA
    if (vid == 0x044F && (pid == 0xB300 || pid == 0xB315 || pid == 0xD007)) return 1; // Thrustmaster PS4
    if (vid == 0x2C22) return 1; // Qanba PS4
    if (vid == 0x11C0) return 1; // ShanWan PS4 clones
    return 0;
}

// DualSense (PS5) & DualSense Edge
#define SONY_DUALSENSE_PID              0x0CE6
#define SONY_DUALSENSE_EDGE_PID         0x0DF2

// Magic activation packet for DualShock 3 over USB
// SET_REPORT Feature 0xF4
#define DS3_MAGIC_REQ_TYPE              0x21
#define DS3_MAGIC_REQUEST               0x09
#define DS3_MAGIC_VALUE                 0x03F4

/*
 * DualShock 3 USB Input Report (49 bytes, Report ID 0x01)
 */
typedef struct __attribute__((packed)) {
    uint8_t  report_id;         // 0: Always 0x01
    uint8_t  reserved_0;        // 1
    // Digital buttons:
    uint8_t  buttons1;          // 2: Select(0x01), L3(0x02), R3(0x04), Start(0x08), Up(0x10), Right(0x20), Down(0x40), Left(0x80)
    uint8_t  buttons2;          // 3: L2(0x01), R2(0x02), L1(0x04), R1(0x08), Triangle(0x10), Circle(0x20), Cross(0x40), Square(0x80)
    uint8_t  buttons3;          // 4: PS Home button (0x01)
    uint8_t  reserved_1;        // 5
    // Analog Sticks:
    uint8_t  left_stick_x;      // 6: LX (0-255, center 128)
    uint8_t  left_stick_y;      // 7: LY (0-255, center 128)
    uint8_t  right_stick_x;     // 8: RX (0-255, center 128)
    uint8_t  right_stick_y;     // 9: RY (0-255, center 128)
    uint8_t  reserved_2[4];     // 10-13
    // Pressure-sensitive analog values:
    uint8_t  pressure_up;       // 14
    uint8_t  pressure_right;    // 15
    uint8_t  pressure_down;     // 16
    uint8_t  pressure_left;     // 17
    uint8_t  pressure_l2;       // 18: L2 trigger (0-255)
    uint8_t  pressure_r2;       // 19: R2 trigger (0-255)
    uint8_t  pressure_l1;       // 20
    uint8_t  pressure_r1;       // 21
    uint8_t  pressure_triangle; // 22
    uint8_t  pressure_circle;   // 23
    uint8_t  pressure_cross;    // 24
    uint8_t  pressure_square;   // 25
    uint8_t  reserved_3[23];    // 26-48 (Sensors, battery, etc.)
} DS3Report;

/*
 * DualSense USB Input Report (64 bytes, Report ID 0x01)
 * Matches struct dualsense_input_report in Linux drivers/hid/hid-playstation.c
 */
typedef struct __attribute__((packed)) {
    uint8_t  report_id;         // 0: Always 0x01
    uint8_t  left_stick_x;      // 1: LX (0-255, center 128)
    uint8_t  left_stick_y;      // 2: LY (0-255, center 128)
    uint8_t  right_stick_x;     // 3: RX (0-255, center 128)
    uint8_t  right_stick_y;     // 4: RY (0-255, center 128)
    uint8_t  left_trigger;      // 5: L2 analog (0-255)
    uint8_t  right_trigger;     // 6: R2 analog (0-255)
    uint8_t  seq_number;        // 7: Sequence counter
    uint8_t  buttons[4];        // 8: Hat switch (lower 4 bits: 0-7, 8=release) + Square(0x10), Cross(0x20), Circle(0x40), Triangle(0x80)
                                // 9: L1(0x01), R1(0x02), L2_click(0x04), R2_click(0x08), Create/Share(0x10), Options(0x20), L3(0x40), R3(0x80)
                                // 10: PS(0x01), Touchpad_click(0x02), Mic_mute(0x04)
                                // 11: Reserved
    uint8_t  reserved[4];       // 12-15
    uint16_t gyro[3];           // 16-21: Gyroscope X, Y, Z (LE16)
    uint16_t accel[3];          // 22-27: Accelerometer X, Y, Z (LE16)
    uint32_t sensor_timestamp;  // 28-31: Sensor timestamp (LE32)
    uint8_t  reserved2;         // 32
    // Touchpad data: 2 touch points (offsets 33-40)
    struct {
        uint8_t  contact;       // bit 7 == 0: active touch, bit 7 == 1: released
        uint8_t  x_lo;
        uint8_t  x_hi_y_lo;     // low nibble = x_hi, high nibble = y_lo
        uint8_t  y_hi;
    } touch_points[2];          // 33-40
    uint8_t  reserved3[12];     // 41-52
    uint8_t  status[3];         // 53-55
    uint8_t  reserved4[8];      // 56-63
} DualSenseReport;

/*
 * DualShock 4 USB Input Report (64 bytes, Report ID 0x01)
 * Matches struct dualshock4_input_report_usb in Linux drivers/hid/hid-playstation.c
 */
typedef struct __attribute__((packed)) {
    uint8_t  report_id;         // 0: Always 0x01
    uint8_t  left_stick_x;      // 1: LX (0-255, center 128)
    uint8_t  left_stick_y;      // 2: LY (0-255, center 128)
    uint8_t  right_stick_x;     // 3: RX (0-255, center 128)
    uint8_t  right_stick_y;     // 4: RY (0-255, center 128)
    uint8_t  buttons[3];        // 5: Hat (0-7, 8=release) + Square(0x10), Cross(0x20), Circle(0x40), Triangle(0x80)
                                // 6: L1(0x01), R1(0x02), L2_click(0x04), R2_click(0x08), Share(0x10), Options(0x20), L3(0x40), R3(0x80)
                                // 7: PS(0x01), Touchpad_click(0x02), counter(bits 2-7)
    uint8_t  left_trigger;      // 8: L2 analog (0-255)
    uint8_t  right_trigger;     // 9: R2 analog (0-255)
    uint16_t sensor_timestamp;  // 10-11
    uint8_t  sensor_temperature;// 12
    int16_t  gyro[3];           // 13-18: Gyro X, Y, Z (LE16)
    int16_t  accel[3];          // 19-24: Accel X, Y, Z (LE16)
    uint8_t  reserved2[5];      // 25-29
    uint8_t  status[2];         // 30-31: battery / status
    uint8_t  reserved3;         // 32
    uint8_t  num_touch_reports; // 33
    struct {
        uint8_t timestamp;      // 0
        struct {
            uint8_t contact;    // bit 7 == 0: active touch, bit 7 == 1: released
            uint8_t x_lo;
            uint8_t x_hi_y_lo;
            uint8_t y_hi;
        } points[2];            // 1-8
    } touch_reports[3];         // 34-60 (3 * 9 = 27 bytes)
    uint8_t  reserved4[3];      // 61-63
} DS4Report;

/*
 * DualShock 4 USB Output Report (32 bytes, Report ID 0x05)
 * Matches struct dualshock4_output_report_usb in Linux drivers/hid/hid-playstation.c
 */
typedef struct __attribute__((packed)) {
    uint8_t report_id;          // 0: Always 0x05
    uint8_t valid_flag0;        // 1: 0x01 = rumble, 0x02 = LED
    uint8_t valid_flag1;        // 2: 0x00
    uint8_t reserved;           // 3: 0x00
    uint8_t motor_right;        // 4: weak / high frequency motor (0-255)
    uint8_t motor_left;         // 5: strong / low frequency motor (0-255)
    uint8_t lightbar_red;       // 6: Red LED (0-255)
    uint8_t lightbar_green;     // 7: Green LED (0-255)
    uint8_t lightbar_blue;      // 8: Blue LED (0-255)
    uint8_t lightbar_blink_on;  // 9: Blink on duration
    uint8_t lightbar_blink_off; // 10: Blink off duration
    uint8_t reserved2[21];      // 11-31: Padding to 32 bytes
} DS4OutputReport;

#endif // PS_CONTROLLERS_H
