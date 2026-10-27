/*
 * Generic (descriptor based) controller detection
 *
 * Third-party Xbox controllers use their own VID/PID but expose the same
 * USB interface signature as the official ones (this is how the Linux
 * "xpad" driver recognises them too):
 *
 *   Xbox 360 compatible : class 0xFF, subclass 0x5D, protocol 0x01
 *   Xbox One / GIP      : class 0xFF, subclass 0x47, protocol 0xD0
 *
 * Instead of a VID/PID whitelist we read the active configuration
 * descriptor, find the matching interface and take the interrupt
 * endpoints from it.
 */

#ifndef CONTROLLER_DETECT_H
#define CONTROLLER_DETECT_H

#include <stdint.h>
#include "libusb_endpoint_patch.h"

#include "translator.h"     // ControllerType

typedef struct {
    ControllerType type;
    uint16_t vid;
    uint16_t pid;
    uint8_t  interface_num;     // interface to claim
    uint8_t  alt_setting;       // alternate setting that owns the endpoints
    uint8_t  ep_in;             // interrupt IN endpoint address (e.g. 0x81)
    uint8_t  ep_out;            // interrupt OUT endpoint address, 0 if none
    int      generic;           // 1 = found via descriptor, 0 = known VID/PID
} DetectedController;

/*
 * Inspect the active configuration descriptor of a device.
 *
 * @param dev   USB device from sceUsbdGetDeviceList
 * @param out   Filled on success (type, interface, alt setting, endpoints).
 *              vid/pid/generic are NOT set here, the caller does that.
 * @return 1 if an Xbox 360 / Xbox One compatible interface was found,
 *         0 otherwise (or if the descriptor could not be read)
 */
int controller_detect_by_descriptor(libusb_device* dev, DetectedController* out);

/*
 * Identify any supported controller (Sony DS3/DualSense, Xbox 360, Xbox One, Switch, Generic XInput)
 *
 * @param dev   USB device from sceUsbdGetDeviceList
 * @param desc  USB device descriptor
 * @param out   Filled on success
 * @return 1 if recognized, 0 otherwise
 */
int controller_identify(libusb_device* dev, const struct libusb_device_descriptor* desc, DetectedController* out);

// Dynamically resolved USB config descriptor helpers (implemented in hooks.c)
int32_t usbd_get_active_config_descriptor(libusb_device* dev, struct libusb_config_descriptor** config);
void usbd_free_config_descriptor(struct libusb_config_descriptor* config);

#endif // CONTROLLER_DETECT_H
