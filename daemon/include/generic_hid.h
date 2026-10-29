/*
 * Generic HID Controller Support for OneFamily
 * Architecture based on AnyPad-PS4 (Vld439 / elmonomalvad0)
 *
 * Provides dynamic HID report descriptor parsing, universal mapping,
 * secondary composite keyboard interface detaching, and rumble support.
 */

#ifndef GENERIC_HID_H
#define GENERIC_HID_H

#include <stdint.h>
#include "ds4.h"
#include "translator.h"
#include "controller_detect.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Detect if device has a valid generic HID gamepad interface.
 * Also scans for and records any secondary composite boot keyboard interface.
 *
 * @param dev  libusb device
 * @param out  Output DetectedController struct
 * @return 1 if generic HID gamepad found, 0 otherwise
 */
int generic_hid_detect(libusb_device* dev, DetectedController* out);

/*
 * Query and parse the HID report descriptor, apply built-in or user .map overrides.
 *
 * @param h              libusb device handle
 * @param vid            Vendor ID
 * @param pid            Product ID
 * @param iface          Claimed gamepad interface number
 * @param ctrl_transfer  Pointer to sceUsbdControlTransfer
 * @return 1 on success, 0 on failure
 */
int generic_hid_setup(libusb_device_handle* h, uint16_t vid, uint16_t pid, uint8_t iface,
                      int (*ctrl_transfer)(libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, uint32_t));

/*
 * Reset internal generic HID parser state on disconnect.
 */
void generic_hid_reset(void);

/*
 * Get detected button count for the current generic HID controller.
 */
int generic_hid_get_nbuttons(void);

/*
 * Reload controller .ini mapping for the specified device.
 */
void generic_hid_reload(uint16_t vid, uint16_t pid);

/*
 * Parse an incoming USB interrupt report into OrbisPadData.
 *
 * @param rx    Raw USB report buffer
 * @param len   Length of received packet
 * @param out   Output DualShock 4 data structure
 * @param cfg   Translator configuration (deadzones, triggers)
 * @return 1 if report was handled, 0 otherwise
 */
int generic_hid_parse_report(const uint8_t* rx, int len, OrbisPadData* out, const TranslatorConfig* cfg);

/*
 * Send rumble feedback to generic HID / 8BitDo controller.
 *
 * @param h              libusb device handle
 * @param vid            Vendor ID
 * @param pid            Product ID
 * @param ep_out         OUT endpoint address (0 if none)
 * @param iface          Gamepad interface number
 * @param large_motor    Left motor (strong) 0-255
 * @param small_motor    Right motor (weak) 0-255
 * @param intr_transfer  Pointer to sceUsbdInterruptTransfer
 * @param ctrl_transfer  Pointer to sceUsbdControlTransfer
 */
void generic_hid_send_rumble(libusb_device_handle* h, uint16_t vid, uint16_t pid,
                             uint8_t ep_out, uint8_t iface,
                             uint8_t large_motor, uint8_t small_motor,
                             int (*intr_transfer)(libusb_device_handle*, uint8_t, uint8_t*, int32_t, int32_t*, uint32_t),
                             int (*ctrl_transfer)(libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, uint32_t));

#ifdef __cplusplus
}
#endif

#endif // GENERIC_HID_H
