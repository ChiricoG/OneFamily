/*
 * Generic (descriptor based) controller detection
 * See controller_detect.h for the rationale.
 */

#include "controller_detect.h"
#include "ps_controllers.h"
#include "switch_controller.h"
#include "stadia.h"
#include "generic_hid.h"
#include "config.h"
#include "log.h"
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
#define XB360_IF_PROTOCOL_WIRED     0x01
#define XB360_IF_PROTOCOL_WIRELESS  0x81    // Wireless receiver (2.4G dongles like 8BitDo / G6)

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
            int is_wireless = 0;

            if (alt->bInterfaceClass != USB_CLASS_VENDOR_SPECIFIC) {
                continue;
            }

            if (alt->bInterfaceSubClass == XB360_IF_SUBCLASS &&
                (alt->bInterfaceProtocol == XB360_IF_PROTOCOL_WIRED ||
                 alt->bInterfaceProtocol == XB360_IF_PROTOCOL_WIRELESS ||
                 alt->bInterfaceProtocol == 0x00)) {
                type = CONTROLLER_XBOX360;
                if (alt->bInterfaceProtocol == XB360_IF_PROTOCOL_WIRELESS) {
                    is_wireless = 1;
                }
            } else if (alt->bInterfaceSubClass == XBONE_IF_SUBCLASS &&
                       alt->bInterfaceProtocol == XBONE_IF_PROTOCOL) {
                type = CONTROLLER_XBOXONE;
            } else {
                continue;
            }

            // First interrupt IN and OUT endpoints of this alt setting
            uint8_t ep_in = 0;
            uint8_t ep_out = 0;
            uint8_t interval = 0;
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor* ep = &alt->endpoint[k];
                if ((ep->bmAttributes & EP_XFER_MASK) != EP_XFER_INTERRUPT) {
                    continue;
                }
                if (ep->bEndpointAddress & EP_DIR_IN) {
                    if (ep_in == 0) {
                        ep_in = ep->bEndpointAddress;
                        interval = ep->bInterval;
                    }
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
            out->bInterval     = interval;
            out->is_wireless   = is_wireless;
            out->kbd_interface_num = 0xFF;
            found = 1;
        }
    }

    if (found) {
        for (int i = 0; i < cfg->bNumInterfaces; i++) {
            const struct libusb_interface* intf = &cfg->interface[i];
            for (int a = 0; a < intf->num_altsetting; a++) {
                const struct libusb_interface_descriptor* alt = &intf->altsetting[a];
                if (alt->bInterfaceClass == USB_CLASS_HID && alt->bInterfaceSubClass == 1 && alt->bInterfaceProtocol == 1) {
                    out->kbd_interface_num = alt->bInterfaceNumber;
                    daemon_log(LOG_LEVEL_INFO, "Composite keyboard interface %d detected on XInput device", out->kbd_interface_num);
                    break;
                }
            }
        }
    }

    usbd_free_config_descriptor(cfg);
    return found;
}

static int detect_dualsense_endpoints(libusb_device* dev, DetectedController* out) {
    if (!dev || !out) return 0;

    struct libusb_config_descriptor* cfg = NULL;
    if (usbd_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
        daemon_log(LOG_LEVEL_WARN, "DualSense: usbd_get_active_config_descriptor returned error or NULL");
        return 0;
    }

    daemon_log(LOG_LEVEL_INFO, "DualSense Config: bNumInterfaces=%d", cfg->bNumInterfaces);
    int found = 0;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface* intf = &cfg->interface[i];
        for (int a = 0; a < intf->num_altsetting; a++) {
            const struct libusb_interface_descriptor* alt = &intf->altsetting[a];
            daemon_log(LOG_LEVEL_INFO, "DualSense IF %d (alt %d): class=0x%02X subclass=0x%02X proto=0x%02X ep_count=%d",
                       alt->bInterfaceNumber, alt->bAlternateSetting, alt->bInterfaceClass,
                       alt->bInterfaceSubClass, alt->bInterfaceProtocol, alt->bNumEndpoints);

            if (alt->bInterfaceClass != USB_CLASS_HID) continue;

            uint8_t ep_in = 0;
            uint8_t ep_out = 0;
            uint8_t interval = 0;
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor* ep = &alt->endpoint[k];
                daemon_log(LOG_LEVEL_INFO, "  DualSense HID EP [%d]: addr=0x%02X attr=0x%02X maxPkt=%d",
                           k, ep->bEndpointAddress, ep->bmAttributes, ep->wMaxPacketSize);
                if ((ep->bmAttributes & EP_XFER_MASK) == EP_XFER_INTERRUPT) {
                    if (ep->bEndpointAddress & EP_DIR_IN) {
                        if (ep_in == 0) {
                            ep_in = ep->bEndpointAddress;
                            interval = ep->bInterval;
                        }
                    } else {
                        if (ep_out == 0) ep_out = ep->bEndpointAddress;
                    }
                }
            }

            if (ep_in != 0 && !found) {
                out->type          = CONTROLLER_DUALSENSE;
                out->interface_num = alt->bInterfaceNumber;
                out->alt_setting   = alt->bAlternateSetting;
                out->ep_in         = ep_in;
                out->ep_out        = ep_out ? ep_out : 0x03;
                out->bInterval     = interval;
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
            uint8_t interval = 0;
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor* ep = &alt->endpoint[k];
                if ((ep->bmAttributes & EP_XFER_MASK) == EP_XFER_INTERRUPT) {
                    if (ep->bEndpointAddress & EP_DIR_IN) {
                        if (ep_in == 0) {
                            ep_in = ep->bEndpointAddress;
                            interval = ep->bInterval;
                        }
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
                out->ep_out        = ep_out;
                out->bInterval     = interval;
                found = 1;
            }
        }
    }

    usbd_free_config_descriptor(cfg);
    return found;
}

static int detect_stadia_endpoints(libusb_device* dev, DetectedController* out) {
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
            uint8_t interval = 0;
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor* ep = &alt->endpoint[k];
                if ((ep->bmAttributes & EP_XFER_MASK) == EP_XFER_INTERRUPT) {
                    if (ep->bEndpointAddress & EP_DIR_IN) {
                        if (ep_in == 0) {
                            ep_in = ep->bEndpointAddress;
                            interval = ep->bInterval;
                        }
                    } else {
                        if (ep_out == 0) ep_out = ep->bEndpointAddress;
                    }
                }
            }

            if (ep_in != 0) {
                out->type          = CONTROLLER_STADIA;
                out->interface_num = alt->bInterfaceNumber;
                out->alt_setting   = alt->bAlternateSetting;
                out->ep_in         = ep_in;
                out->ep_out        = ep_out;
                out->bInterval     = interval;
                found = 1;
            }
        }
    }

    usbd_free_config_descriptor(cfg);
    return found;
}

static int detect_ds4_endpoints(libusb_device* dev, DetectedController* out) {
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
            // Skip boot keyboard and boot mouse
            if (alt->bInterfaceSubClass == 1 && (alt->bInterfaceProtocol == 1 || alt->bInterfaceProtocol == 2)) {
                continue;
            }

            uint8_t ep_in = 0;
            uint8_t ep_out = 0;
            uint8_t interval = 0;
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor* ep = &alt->endpoint[k];
                if ((ep->bmAttributes & EP_XFER_MASK) == EP_XFER_INTERRUPT) {
                    if (ep->bEndpointAddress & EP_DIR_IN) {
                        if (ep_in == 0) {
                            ep_in = ep->bEndpointAddress;
                            interval = ep->bInterval;
                        }
                    } else {
                        if (ep_out == 0) ep_out = ep->bEndpointAddress;
                    }
                }
            }

            if (ep_in != 0) {
                out->type          = CONTROLLER_DS4;
                out->interface_num = alt->bInterfaceNumber;
                out->alt_setting   = alt->bAlternateSetting;
                out->ep_in         = ep_in;
                out->ep_out        = ep_out ? ep_out : 0x03;
                out->bInterval     = interval;
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
    out->kbd_interface_num = 0xFF;

    // 0. PlayStation 4 controllers:
    // If it's a native PS4 controller (Sony VID 0x054C with DS4 PID 0x05C4, 0x09CC, 0x0BA0):
    if (is_native_ps4_controller(desc->idVendor, desc->idProduct)) {
        if (usbd_is_sony_device(dev, desc)) {
            daemon_log(LOG_LEVEL_INFO, "Skipping native Sony DualShock 4 (VID=0x%04x, PID=0x%04x) for Orbis OS driver",
                       desc->idVendor, desc->idProduct);
            return 0;
        } else {
            daemon_log(LOG_LEVEL_INFO, "Third-party controller in DS4 mode (VID=0x%04X PID=0x%04X) — claiming as CONTROLLER_DS4 to prevent 8-minute timeout",
                       desc->idVendor, desc->idProduct);
            if (detect_ds4_endpoints(dev, out)) {
                out->vid = desc->idVendor;
                out->pid = desc->idProduct;
                return 1;
            }
        }
    }

    // Check known third-party controllers with non-Sony VID in PS4 mode (GameSir, Hori, Nacon, Razer, etc.)
    if (is_ds4_compatible_controller(desc->idVendor, desc->idProduct)) {
        if (detect_ds4_endpoints(dev, out)) {
            out->vid = desc->idVendor;
            out->pid = desc->idProduct;
            daemon_log(LOG_LEVEL_INFO, "Third-party PS4 mode controller claimed (VID:0x%04X PID:0x%04X) as CONTROLLER_DS4",
                       desc->idVendor, desc->idProduct);
            return 1;
        }
    }

    // 1. Sony DualShock 3 & DualSense
    if (desc->idVendor == SONY_VID) {
        if (desc->idProduct == SONY_DS3_PID) {
            out->type = CONTROLLER_DS3;
            out->vid = desc->idVendor;
            out->pid = desc->idProduct;
            out->interface_num = 0;
            out->ep_in = 0x81;
            out->ep_out = 0x02;
            out->bInterval = 1;
            out->kbd_interface_num = 0xFF;
            return 1;
        } else if (desc->idProduct == SONY_DUALSENSE_PID || desc->idProduct == SONY_DUALSENSE_EDGE_PID) {
            out->type = CONTROLLER_DUALSENSE;
            out->vid = desc->idVendor;
            out->pid = desc->idProduct;
            if (!detect_dualsense_endpoints(dev, out)) {
                daemon_log(LOG_LEVEL_WARN, "DualSense descriptor parse failed, using fallback IF=3 EP_IN=0x84 EP_OUT=0x03");
                out->interface_num = 3;
                out->ep_in = 0x84;
                out->ep_out = 0x03;
            }
            return 1;
        }
        // Non-controller Sony peripherals (PlayStation VR Processor Unit 0x09AF/0x09B0,
        // PS Move, Wireless Headset dongles, etc.) must never be hijacked by generic HID detection.
        // Leave them completely untouched for native Orbis OS drivers!
        daemon_log(LOG_LEVEL_INFO, "Skipping non-controller Sony peripheral (VID:0x%04X PID:0x%04X) for Orbis OS",
                   desc->idVendor, desc->idProduct);
        return 0;
    }

    // 2. Microsoft Xbox 360 & Xbox One / Series / PDP Victrix
    if (desc->idVendor == XBOX_VID) {
            if (desc->idProduct == XBOX360_PID) {
                // Use descriptor parsing to capture actual endpoints and bInterval
                // (e.g. 1000Hz dongles like GuliKit KK3 Max 2.4GHz Hyperlink with bInterval=1)
                if (!controller_detect_by_descriptor(dev, out)) {
                    out->type = CONTROLLER_XBOX360;
                    out->interface_num = 0;
                    out->ep_in = 0x81;
                    out->ep_out = 0x01;
                    out->bInterval = 4; // Safe standard 250Hz fallback
                }
                out->type = CONTROLLER_XBOX360;
                out->vid = desc->idVendor;
                out->pid = desc->idProduct;
                if (out->bInterval == 0) out->bInterval = 4;
                return 1;
            } else if (is_xone_pid(desc->idProduct)) {
                out->type = CONTROLLER_XBOXONE;
                out->vid = desc->idVendor;
                out->pid = desc->idProduct;
                if (!controller_detect_by_descriptor(dev, out)) {
                    out->interface_num = 0;
                    out->alt_setting = 1; // Explicitly activate alternate setting 1 for GIP interrupt endpoints!
                    out->ep_in = 0x82;
                    out->ep_out = 0x02;
                }
                return 1;
            }
        } else if (desc->idVendor == PDP_VID && desc->idProduct == VICTRIX_PRO_BFG_XBOX_PID) {
            out->type = CONTROLLER_XBOXONE;
            out->vid = desc->idVendor;
            out->pid = desc->idProduct;
            if (!controller_detect_by_descriptor(dev, out)) {
                out->interface_num = 0;
                out->alt_setting = 1;
                out->ep_in = 0x82;
                out->ep_out = 0x02;
            }
            return 1;
        }

    // 3. PRIORITY CHECK: Generic Descriptor-based detection (Third-Party Xbox 360 / Xbox One GIP)
    // CRITICAL: Executed BEFORE Switch Pro detection so that multi-mode controllers
    // (such as Machenike, EasySMX, etc.) that expose an XInput interface
    // are claimed immediately as Xbox controllers with native analog triggers, rumble, and no 0x80 crashes!
    if (controller_detect_by_descriptor(dev, out)) {
        out->generic = 1;
        out->vid = desc->idVendor;
        out->pid = desc->idProduct;
        daemon_log(LOG_LEVEL_INFO, "Device VID:0x%04X PID:0x%04X claimed via XInput descriptor (%s)",
                   desc->idVendor, desc->idProduct, (out->type == CONTROLLER_XBOXONE ? "Xbox One" : "Xbox 360"));
        return 1;
    }

    // 4. Nintendo Switch Pro & Official / Clone Controllers (only if no XInput interface was exposed)
    if (is_switch_pro_controller(desc->idVendor, desc->idProduct)) {
        out->type = CONTROLLER_SWITCH_PRO;
        out->vid = desc->idVendor;
        out->pid = desc->idProduct;
        if (!detect_switch_pro_endpoints(dev, out)) {
            out->interface_num = 0;
            out->ep_in = 0x81;
            out->ep_out = 0;
        }
        return 1;
    }

    // 5. Nintendo Switch Third-Party Input-Only (PDP Rock Candy, etc.)
    if (is_switch_input_only_controller(desc->idVendor, desc->idProduct)) {
        out->type = CONTROLLER_SWITCH;
        out->vid = desc->idVendor;
        out->pid = desc->idProduct;
        out->interface_num = 0;
        out->ep_in = 0x81;
        out->ep_out = 0;
        return 1;
    }

    // 6. Google Stadia Controller
    if (desc->idVendor == GOOGLE_VID && desc->idProduct == STADIA_CONTROLLER_PID) {
        out->type = CONTROLLER_STADIA;
        out->vid = desc->idVendor;
        out->pid = desc->idProduct;
        if (!detect_stadia_endpoints(dev, out)) {
            out->interface_num = 0;
            out->ep_in = 0x81;
            out->ep_out = 0x01;
        }
        return 1;
    }

    // 7. Generic HID Controller Fallback (AnyPad model: 8BitDo, generic DInput pads, fightsticks)
    if (desc->idVendor == 0x2DC8 && desc->idProduct == 0x301C) {
        // 8BitDo Ultimate 2C 2.4G wireless adapter in idle receiver state (controller turned OFF).
        // The adapter automatically switches to 0x310A (Xbox 360 mode) as soon as the controller is turned ON.
        // Ignore idle receiver to prevent false gamepad claiming and user picker popups!
        daemon_log(LOG_LEVEL_INFO, "Device 0x2DC8:0x301C is 8BitDo idle wireless receiver (controller OFF) — ignoring");
        return 0;
    }

    if (generic_hid_detect(dev, out)) {
        out->generic = 1;
        out->vid = desc->idVendor;
        out->pid = desc->idProduct;
        return 1;
    }

    return 0;
}
