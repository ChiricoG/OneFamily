/*
 * Generic (descriptor based) controller detection
 * See controller_detect.h for the rationale.
 */

#include "controller_detect.h"
#include "ps_controllers.h"
#include "switch_controller.h"
#include <string.h>

#define XBOX_VID                    0x045E
#define XBOX360_PID                 0x028E
#define PDP_VID                     0x0E6F
#define VICTRIX_PRO_BFG_XBOX_PID    0x024C

static const uint16_t s_xboxone_pids[] = {
    0x02D1, 0x02DD, 0x02E3, 0x02EA, 0x0B00, 0x0B0A, 0x0B12
};

static int is_xone_pid(uint16_t pid) {
    for (size_t i = 0; i < sizeof(s_xboxone_pids) / sizeof(s_xboxone_pids[0]); i++) {
        if (pid == s_xboxone_pids[i]) return 1;
    }
    return 0;
}

#define USB_CLASS_VENDOR_SPECIFIC   0xFF
#define USB_CLASS_HID               0x03

// Xbox 360 compatible (XInput) interface signature
#define XB360_IF_SUBCLASS           0x5D
#define XB360_IF_PROTOCOL           0x01    // 0x81 would be the wireless receiver (not supported)

// Xbox One / GIP interface signature
#define XBONE_IF_SUBCLASS           0x47
#define XBONE_IF_PROTOCOL           0xD0

#define EP_DIR_IN                   0x80
#define EP_XFER_MASK                0x03
#define EP_XFER_INTERRUPT           0x03

int controller_detect_by_descriptor(libusb_device* dev, DetectedController* out) {
    if (dev == NULL || out == NULL) {
        return 0;
    }

    struct libusb_config_descriptor* cfg = NULL;
    if (usbd_get_active_config_descriptor(dev, &cfg) != 0 || cfg == NULL) {
        return 0;
    }

    int found = 0;

    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface* intf = &cfg->interface[i];

        for (int a = 0; a < intf->num_altsetting && !found; a++) {
            const struct libusb_interface_descriptor* alt = &intf->altsetting[a];
            ControllerType type = CONTROLLER_NONE;

            if (alt->bInterfaceClass != USB_CLASS_VENDOR_SPECIFIC) {
                continue;
            }

            if (alt->bInterfaceSubClass == XB360_IF_SUBCLASS &&
                alt->bInterfaceProtocol == XB360_IF_PROTOCOL) {
                type = CONTROLLER_XBOX360;
            } else if (alt->bInterfaceSubClass == XBONE_IF_SUBCLASS &&
                       alt->bInterfaceProtocol == XBONE_IF_PROTOCOL) {
                type = CONTROLLER_XBOXONE;
            } else {
                continue;
            }

            // First interrupt IN and OUT endpoints of this alt setting
            uint8_t ep_in = 0;
            uint8_t ep_out = 0;
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor* ep = &alt->endpoint[k];
                if ((ep->bmAttributes & EP_XFER_MASK) != EP_XFER_INTERRUPT) {
                    continue;
                }
                if (ep->bEndpointAddress & EP_DIR_IN) {
                    if (ep_in == 0) ep_in = ep->bEndpointAddress;
                } else {
                    if (ep_out == 0) ep_out = ep->bEndpointAddress;
                }
            }

            // An alt setting without an input endpoint is of no use to us
            // (e.g. Xbox One audio interface alt 0) - keep looking.
            if (ep_in == 0) {
                continue;
            }

            memset(out, 0, sizeof(*out));
            out->type          = type;
            out->interface_num = alt->bInterfaceNumber;
            out->alt_setting   = alt->bAlternateSetting;
            out->ep_in         = ep_in;
            out->ep_out        = ep_out;
            found = 1;
        }
    }

    usbd_free_config_descriptor(cfg);
    return found;
}

static int detect_dualsense_endpoints(libusb_device* dev, DetectedController* out) {
    if (!dev || !out) return 0;

    struct libusb_config_descriptor* cfg = NULL;
    if (usbd_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
        return 0;
    }

    int found = 0;
    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface* intf = &cfg->interface[i];
        for (int a = 0; a < intf->num_altsetting && !found; a++) {
            const struct libusb_interface_descriptor* alt = &intf->altsetting[a];

            if (alt->bInterfaceClass != USB_CLASS_HID) continue;

            uint8_t ep_in = 0;
            uint8_t ep_out = 0;
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor* ep = &alt->endpoint[k];
                if ((ep->bmAttributes & EP_XFER_MASK) == EP_XFER_INTERRUPT) {
                    if (ep->bEndpointAddress & EP_DIR_IN) {
                        if (ep_in == 0) ep_in = ep->bEndpointAddress;
                    } else {
                        if (ep_out == 0) ep_out = ep->bEndpointAddress;
                    }
                }
            }

            if (ep_in != 0) {
                out->type          = CONTROLLER_DUALSENSE;
                out->interface_num = alt->bInterfaceNumber;
                out->alt_setting   = alt->bAlternateSetting;
                out->ep_in         = ep_in;
                out->ep_out        = ep_out ? ep_out : 0x03;
                found = 1;
            }
        }
    }

    usbd_free_config_descriptor(cfg);
    return found;
}

static int detect_switch_pro_endpoints(libusb_device* dev, DetectedController* out) {
    if (!dev || !out) return 0;

    struct libusb_config_descriptor* cfg = NULL;
    if (usbd_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
        return 0;
    }

    int found = 0;
    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface* intf = &cfg->interface[i];
        for (int a = 0; a < intf->num_altsetting && !found; a++) {
            const struct libusb_interface_descriptor* alt = &intf->altsetting[a];

            if (alt->bInterfaceClass != USB_CLASS_HID) continue;

            uint8_t ep_in = 0;
            uint8_t ep_out = 0;
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor* ep = &alt->endpoint[k];
                if ((ep->bmAttributes & EP_XFER_MASK) == EP_XFER_INTERRUPT) {
                    if (ep->bEndpointAddress & EP_DIR_IN) {
                        if (ep_in == 0) ep_in = ep->bEndpointAddress;
                    } else {
                        if (ep_out == 0) ep_out = ep->bEndpointAddress;
                    }
                }
            }

            if (ep_in != 0) {
                out->type          = CONTROLLER_SWITCH_PRO;
                out->interface_num = alt->bInterfaceNumber;
                out->alt_setting   = alt->bAlternateSetting;
                out->ep_in         = ep_in;
                out->ep_out        = ep_out ? ep_out : 0x01;
                found = 1;
            }
        }
    }

    usbd_free_config_descriptor(cfg);
    return found;
}

int controller_identify(libusb_device* dev, const struct libusb_device_descriptor* desc, DetectedController* out) {
    if (!dev || !desc || !out) return 0;
    memset(out, 0, sizeof(*out));

    // 1. Sony DualShock 3 & DualSense
    if (desc->idVendor == SONY_VID) {
        if (desc->idProduct == SONY_DS3_PID) {
            out->type = CONTROLLER_DS3;
            out->vid = desc->idVendor;
            out->pid = desc->idProduct;
            out->interface_num = 0;
            out->ep_in = 0x81;
            out->ep_out = 0x02;
            return 1;
        } else if (desc->idProduct == SONY_DUALSENSE_PID || desc->idProduct == SONY_DUALSENSE_EDGE_PID) {
            out->type = CONTROLLER_DUALSENSE;
            out->vid = desc->idVendor;
            out->pid = desc->idProduct;
            if (!detect_dualsense_endpoints(dev, out)) {
                // Fallback to standard DualSense HID interface if descriptor parse failed
                out->interface_num = 3;
                out->ep_in = 0x84;
                out->ep_out = 0x03;
            }
            return 1;
        }
    }

    // 2. Microsoft Xbox 360 & Xbox One / Series / PDP Victrix
    if (desc->idVendor == XBOX_VID) {
        if (desc->idProduct == XBOX360_PID) {
            out->type = CONTROLLER_XBOX360;
            out->vid = desc->idVendor;
            out->pid = desc->idProduct;
            out->interface_num = 0;
            out->ep_in = 0x81;
            out->ep_out = 0x01;
            return 1;
        } else if (is_xone_pid(desc->idProduct)) {
            out->type = CONTROLLER_XBOXONE;
            out->vid = desc->idVendor;
            out->pid = desc->idProduct;
            out->interface_num = 0;
            out->ep_in = 0x82;
            out->ep_out = 0x02;
            return 1;
        }
    } else if (desc->idVendor == PDP_VID) {
        if (desc->idProduct == VICTRIX_PRO_BFG_XBOX_PID) {
            out->type = CONTROLLER_XBOXONE;
            out->vid = desc->idVendor;
            out->pid = desc->idProduct;
            if (!controller_detect_by_descriptor(dev, out)) {
                out->interface_num = 0;
                out->ep_in = 0x82;
                out->ep_out = 0x02;
            }
            return 1;
        }
    }

    // 3. Nintendo Switch Pro & Official Controllers
    if (is_switch_pro_controller(desc->idVendor, desc->idProduct)) {
        out->type = CONTROLLER_SWITCH_PRO;
        out->vid = desc->idVendor;
        out->pid = desc->idProduct;
        if (!detect_switch_pro_endpoints(dev, out)) {
            out->interface_num = 0;
            out->ep_in = 0x81;
            out->ep_out = 0x01;
        }
        return 1;
    }

    // 4. Nintendo Switch Third-Party Input-Only (PDP, etc.)
    if (is_switch_input_only_controller(desc->idVendor, desc->idProduct)) {
        out->type = CONTROLLER_SWITCH;
        out->vid = desc->idVendor;
        out->pid = desc->idProduct;
        out->interface_num = 0;
        out->ep_in = 0x81;
        out->ep_out = 0;
        return 1;
    }

    // 5. Generic Descriptor-based detection (Third-Party Xbox 360 / Xbox One)
    if (controller_detect_by_descriptor(dev, out)) {
        out->generic = 1;
        out->vid = desc->idVendor;
        out->pid = desc->idProduct;
        return 1;
    }

    return 0;
}
