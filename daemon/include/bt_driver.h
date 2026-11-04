#ifndef ONEFAMILY_BT_DRIVER_H
#define ONEFAMILY_BT_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#define BT_MAX_DEVICES 4

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the DualSense Bluetooth Driver Subsystem.
 * Performs kernel allowlist patching (via SigScan / Firmware matrix)
 * and starts the background /dev/bt listener thread.
 *
 * @return 0 on success, negative error code on failure.
 */
int bt_driver_init(void);

/**
 * Non-blocking poll step executed in the main daemon loop (250 Hz).
 * Checks kqueue for /dev/bt events, queries reports via ioctl 0x80206233,
 * decodes input and feeds the virtual DualShock 4 controller.
 */
void bt_driver_poll_step(void);

/**
 * Shutdown the Bluetooth Driver Subsystem and restore original kernel allowlist.
 */
void bt_driver_shutdown(void);

/**
 * Check if the DualSense Bluetooth Subsystem is active and supported on this firmware.
 */
bool bt_driver_is_supported(void);

/**
 * Check if at least one DualSense controller is connected via Bluetooth.
 */
bool bt_driver_is_connected(void);

/**
 * Number of DualSense controllers currently connected over Bluetooth.
 */
int bt_driver_connected_count(void);

/**
 * Send rumble and LED color feedback to the Bluetooth-connected DualSense.
 *
 * @param large Large rumble motor intensity (0-255).
 * @param small Small rumble motor intensity (0-255).
 * @param color RGB lightbar color [R, G, B].
 */
void bt_driver_send_rumble(uint8_t large, uint8_t small, const uint8_t color[3]);

/**
 * Send rumble and LED feedback to the DualSense bound to a specific virtual pad slot.
 *
 * @param vpad_slot Virtual pad slot index (0..3).
 * @param large Large rumble motor intensity (0-255).
 * @param small Small rumble motor intensity (0-255).
 * @param color RGB lightbar color [R, G, B].
 */
void bt_driver_send_rumble_slot(int vpad_slot, uint8_t large, uint8_t small, const uint8_t color[3]);

/**
 * Re-send current hardware configuration (lightbar, brightness, player LEDs, trigger effects)
 * to all connected DualSense controllers on the fly without waiting for game rumble events.
 */
void bt_driver_refresh_settings(void);

#ifdef __cplusplus
}
#endif

#endif // ONEFAMILY_BT_DRIVER_H
