#ifndef ONEFAMILY_BT_DRIVER_H
#define ONEFAMILY_BT_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

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
 * Shutdown the Bluetooth Driver Subsystem and restore original kernel allowlist.
 */
void bt_driver_shutdown(void);

/**
 * Check if a DualSense controller is currently connected via Bluetooth.
 */
bool bt_driver_is_connected(void);

/**
 * Send rumble and LED color feedback to the Bluetooth-connected DualSense.
 *
 * @param large Large rumble motor intensity (0-255).
 * @param small Small rumble motor intensity (0-255).
 * @param color RGB lightbar color [R, G, B].
 */
void bt_driver_send_rumble(uint8_t large, uint8_t small, const uint8_t color[3]);

#ifdef __cplusplus
}
#endif

#endif // ONEFAMILY_BT_DRIVER_H
