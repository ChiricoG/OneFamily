/*
 * USB Controller Driver Subsystem for OneFamily Daemon
 * Handles 250 Hz asynchronous USB polling, GIP handshakes, hotplug, and rumble
 */

#include "usb_driver.h"
#include "vpad.h"
#include "config.h"
#include "log.h"
#include "ds4.h"
#include "xbox360.h"
#include "xboxone.h"
#include "switch_controller.h"
#include "ps_controllers.h"
#include "controller_detect.h"
#include "translator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "daemon_syscalls.h"

UsbControllerState g_usb_state;

static int g_hUsbd = 0;
static libusb_device_handle *g_dev_handle = NULL;
static libusb_device *g_cur_dev = NULL;
static int g_claimed_interface = -1;
static uint8_t s_xboxone_guide = 0;
static OrbisPadData s_last_ds4;

// libSceUsbd dynamic function pointers
typedef int32_t (*sceUsbdInit_t)(void);
typedef void    (*sceUsbdExit_t)(void);
typedef ssize_t (*sceUsbdGetDeviceList_t)(libusb_device***);
typedef void    (*sceUsbdFreeDeviceList_t)(libusb_device**);
typedef int32_t (*sceUsbdGetDeviceDescriptor_t)(libusb_device*, struct libusb_device_descriptor*);
typedef int32_t (*sceUsbdGetActiveConfigDescriptor_t)(libusb_device*, struct libusb_config_descriptor**);
typedef void    (*sceUsbdFreeConfigDescriptor_t)(struct libusb_config_descriptor*);
typedef int32_t (*sceUsbdOpen_t)(libusb_device*, libusb_device_handle**);
typedef void    (*sceUsbdClose_t)(libusb_device_handle*);
typedef int32_t (*sceUsbdClaimInterface_t)(libusb_device_handle*, int32_t);
typedef int32_t (*sceUsbdReleaseInterface_t)(libusb_device_handle*, int32_t);
typedef int32_t (*sceUsbdInterruptTransfer_t)(libusb_device_handle*, uint8_t, uint8_t*, int32_t, int32_t*, uint32_t);
typedef int32_t (*sceUsbdControlTransfer_t)(libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, uint32_t);
typedef int32_t (*sceUsbdCheckConnected_t)(libusb_device_handle*);

static sceUsbdInit_t                       p_sceUsbdInit = NULL;
static sceUsbdExit_t                       p_sceUsbdExit = NULL;
static sceUsbdGetDeviceList_t              p_sceUsbdGetDeviceList = NULL;
static sceUsbdFreeDeviceList_t             p_sceUsbdFreeDeviceList = NULL;
static sceUsbdGetDeviceDescriptor_t        p_sceUsbdGetDeviceDescriptor = NULL;
static sceUsbdGetActiveConfigDescriptor_t  p_sceUsbdGetActiveConfigDescriptor = NULL;
static sceUsbdFreeConfigDescriptor_t       p_sceUsbdFreeConfigDescriptor = NULL;
static sceUsbdOpen_t                       p_sceUsbdOpen = NULL;
static sceUsbdClose_t                      p_sceUsbdClose = NULL;
static sceUsbdClaimInterface_t             p_sceUsbdClaimInterface = NULL;
static sceUsbdReleaseInterface_t           p_sceUsbdReleaseInterface = NULL;
static sceUsbdInterruptTransfer_t          p_sceUsbdInterruptTransfer = NULL;
static sceUsbdControlTransfer_t            p_sceUsbdControlTransfer = NULL;
static sceUsbdCheckConnected_t             p_sceUsbdCheckConnected = NULL;

int32_t usbd_get_active_config_descriptor(libusb_device* dev, struct libusb_config_descriptor** config) {
    if (!p_sceUsbdGetActiveConfigDescriptor) return -1;
    return p_sceUsbdGetActiveConfigDescriptor(dev, config);
}

void usbd_free_config_descriptor(struct libusb_config_descriptor* config) {
    if (p_sceUsbdFreeConfigDescriptor && config) {
        p_sceUsbdFreeConfigDescriptor(config);
    }
}

static int load_usbd(void) {
    if (g_hUsbd != 0) return 0;

    int h = 0;
    if (daemon_sys_load_prx("/system/common/lib/libSceUsbd.sprx", &h) < 0 || h == 0) {
        if (daemon_sys_load_prx("libSceUsbd.sprx", &h) < 0 || h == 0) {
            daemon_log(LOG_LEVEL_ERROR, "Failed to load libSceUsbd.sprx");
            return -1;
        }
    }
    g_hUsbd = h;

    daemon_sys_dlsym(g_hUsbd, "sceUsbdInit", (void**)&p_sceUsbdInit);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdExit", (void**)&p_sceUsbdExit);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdGetDeviceList", (void**)&p_sceUsbdGetDeviceList);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdFreeDeviceList", (void**)&p_sceUsbdFreeDeviceList);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdGetDeviceDescriptor", (void**)&p_sceUsbdGetDeviceDescriptor);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdGetActiveConfigDescriptor", (void**)&p_sceUsbdGetActiveConfigDescriptor);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdFreeConfigDescriptor", (void**)&p_sceUsbdFreeConfigDescriptor);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdOpen", (void**)&p_sceUsbdOpen);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdClose", (void**)&p_sceUsbdClose);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdClaimInterface", (void**)&p_sceUsbdClaimInterface);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdReleaseInterface", (void**)&p_sceUsbdReleaseInterface);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdInterruptTransfer", (void**)&p_sceUsbdInterruptTransfer);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdControlTransfer", (void**)&p_sceUsbdControlTransfer);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdCheckConnected", (void**)&p_sceUsbdCheckConnected);

    if (p_sceUsbdInit) {
        p_sceUsbdInit();
    }
    daemon_log(LOG_LEVEL_INFO, "libSceUsbd initialized successfully (handle=0x%X)", g_hUsbd);
    return 0;
}

static int s_logged_first_report = 0;
static int s_last_guide_pressed = 0;

static void close_controller(void) {
    vpad_shutdown();
    if (g_dev_handle) {
        if (g_claimed_interface >= 0 && p_sceUsbdReleaseInterface) {
            p_sceUsbdReleaseInterface(g_dev_handle, g_claimed_interface);
        }
        if (p_sceUsbdClose) {
            p_sceUsbdClose(g_dev_handle);
        }
        g_dev_handle = NULL;
    }
    g_claimed_interface = -1;
    g_cur_dev = NULL;
    g_usb_state.connected = false;
    s_xboxone_guide = 0;
    s_logged_first_report = 0;
    s_last_guide_pressed = 0;
    memset(&s_last_ds4, 0, sizeof(s_last_ds4));
    s_last_ds4.leftStick.x = 128;
    s_last_ds4.leftStick.y = 128;
    s_last_ds4.rightStick.x = 128;
    s_last_ds4.rightStick.y = 128;
    daemon_log(LOG_LEVEL_INFO, "Controller disconnected from USB");
    daemon_notify("OneFamily: Controller Disconnected");
}

static int send_init_packet(const uint8_t* pkt, int len) {
    if (!g_dev_handle || !p_sceUsbdInterruptTransfer || g_usb_state.endpoint_out == 0) return -1;
    int transferred = 0;
    return p_sceUsbdInterruptTransfer(g_dev_handle, g_usb_state.endpoint_out, (uint8_t*)pkt, len, &transferred, 100);
}

static void init_gip_controller(void) {
    daemon_log(LOG_LEVEL_INFO, "Sending GIP initialization handshake...");
    const uint8_t start_cmd[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };
    send_init_packet(start_cmd, sizeof(start_cmd));
    daemon_sys_usleep(10000);

    const uint8_t powera_init[] = { 0x01, 0x20, 0x00, 0x09, 0x00, 0x04, 0x20, 0x3a, 0x00, 0x00, 0x00, 0x80, 0x00 };
    send_init_packet(powera_init, sizeof(powera_init));
    daemon_sys_usleep(10000);

    const uint8_t pdp_init[] = { 0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 };
    send_init_packet(pdp_init, sizeof(pdp_init));
    daemon_sys_usleep(10000);
}

static const char* controller_type_to_str(ControllerType type) {
    switch (type) {
        case CONTROLLER_XBOX360:  return "Xbox 360";
        case CONTROLLER_XBOXONE:  return "Xbox One/Series";
        case CONTROLLER_SWITCH:   return "Switch";
        case CONTROLLER_DS3:      return "DualShock 3";
        case CONTROLLER_DUALSENSE:return "DualSense";
        default:                  return "Unknown";
    }
}

static int s_logged_first_scan = 0;

static int scan_and_open(void) {
    if (g_dev_handle != NULL) return 0;
    if (!p_sceUsbdGetDeviceList || !p_sceUsbdFreeDeviceList) return -1;

    libusb_device** list = NULL;
    ssize_t count = p_sceUsbdGetDeviceList(&list);
    if (count <= 0 || !list) {
        if (!s_logged_first_scan) {
            daemon_log(LOG_LEVEL_INFO, "USB Bus Scan: no devices found (count=%d)", (int)count);
            s_logged_first_scan = 1;
        }
        if (list) p_sceUsbdFreeDeviceList(list);
        return -1;
    }

    if (!s_logged_first_scan) {
        daemon_log(LOG_LEVEL_INFO, "USB Bus Scan: %d devices detected on bus", (int)count);
        s_logged_first_scan = 1;
    }

    int found = 0;
    for (ssize_t i = 0; i < count && !found; i++) {
        libusb_device* dev = list[i];
        struct libusb_device_descriptor desc;
        if (p_sceUsbdGetDeviceDescriptor(dev, &desc) != 0) continue;

        daemon_log(LOG_LEVEL_INFO, "USB Device [%d]: VID:0x%04X PID:0x%04X (Class:0x%02X)",
                   (int)i, desc.idVendor, desc.idProduct, desc.bDeviceClass);

        // Skip USB Hubs (Class 0x09) and Mass Storage (Class 0x08)
        if (desc.bDeviceClass == 0x09 || desc.bDeviceClass == 0x08) continue;

        DetectedController det;
        memset(&det, 0, sizeof(det));
        if (controller_identify(dev, &desc, &det)) {
            libusb_device_handle* h = NULL;
            if (p_sceUsbdOpen(dev, &h) == 0 && h != NULL) {
                if (p_sceUsbdClaimInterface(h, det.interface_num) == 0) {
                    g_dev_handle = h;
                    g_cur_dev = dev;
                    g_claimed_interface = det.interface_num;
                    g_usb_state.connected = true;
                    g_usb_state.vid = desc.idVendor;
                    g_usb_state.pid = desc.idProduct;
                    g_usb_state.type = det.type;
                    g_usb_state.endpoint_in = det.ep_in;
                    g_usb_state.endpoint_out = det.ep_out;
                    found = 1;

                    daemon_log(LOG_LEVEL_INFO, "Controller claimed: %s (VID:0x%04X PID:0x%04X, EP_IN:0x%02X)",
                               controller_type_to_str(det.type), desc.idVendor, desc.idProduct, det.ep_in);

                    memset(&s_last_ds4, 0, sizeof(s_last_ds4));
                    s_last_ds4.leftStick.x = 128;
                    s_last_ds4.leftStick.y = 128;
                    s_last_ds4.rightStick.x = 128;
                    s_last_ds4.rightStick.y = 128;
                    s_last_ds4.connected = 1;
                    s_last_ds4.count = 1;

                    // Initialize Virtual DualShock 4 (Type 1) now that controller is ready to answer
                    vpad_init();

                    if (det.type == CONTROLLER_XBOXONE) {
                        init_gip_controller();
                    } else if (det.type == CONTROLLER_DS3) {
                        uint8_t magic[5] = { 0xF4, 0x42, 0x03, 0x00, 0x00 };
                        if (p_sceUsbdControlTransfer) {
                            int cret = p_sceUsbdControlTransfer(h, DS3_MAGIC_REQ_TYPE, DS3_MAGIC_REQUEST, DS3_MAGIC_VALUE, det.interface_num, magic, sizeof(magic), 1000);
                            daemon_log(LOG_LEVEL_INFO, "DualShock 3 operational handshake sent (ret=%d)", cret);
                        }
                    }

                    daemon_notify("OneFamily: %s Connected",
                                  controller_type_to_str(det.type));
                } else {
                    p_sceUsbdClose(h);
                }
            }
        }
    }

    p_sceUsbdFreeDeviceList(list);
    return found ? 0 : -1;
}

void usb_driver_send_rumble(uint8_t large_motor, uint8_t small_motor) {
    if (!g_dev_handle || !g_usb_state.connected || !p_sceUsbdInterruptTransfer) {
        return;
    }
    if (!g_daemon_config.enable_rumble) {
        return;
    }

    uint8_t ep = g_usb_state.endpoint_out;
    if (ep == 0) {
        ep = (g_usb_state.type == CONTROLLER_XBOXONE) ? 0x02 : 0x01;
    }

    static uint8_t s_log_lg = 0xFF;
    static uint8_t s_log_sm = 0xFF;
    if (large_motor != s_log_lg || small_motor != s_log_sm) {
        s_log_lg = large_motor;
        s_log_sm = small_motor;
        daemon_log(LOG_LEVEL_INFO, "Rumble requested: L=%d R=%d (type=%d ep=0x%02X)",
                   large_motor, small_motor, g_usb_state.type, ep);
    }

    int transferred = 0;
    if (g_usb_state.type == CONTROLLER_XBOX360) {
        uint8_t pkt[8] = { 0x00, 0x08, 0x00, large_motor, small_motor, 0x00, 0x00, 0x00 };
        int ret = p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 50);
        if (ret != 0) {
            uint8_t pkt4[4] = { 0x00, 0x04, large_motor, small_motor };
            p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt4, sizeof(pkt4), &transferred, 50);
        }
    } else if (g_usb_state.type == CONTROLLER_XBOXONE) {
        static uint8_t s_xone_seq = 0;
        uint8_t pkt[13] = {
            0x09, 0x00, s_xone_seq++, 0x09,
            0x00, 0x0F,
            (uint8_t)(large_motor > 0 ? (large_motor / 2) : 0),
            (uint8_t)(small_motor > 0 ? (small_motor / 2) : 0),
            large_motor,
            small_motor,
            0xFF, 0x00, 0xFF
        };
        p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 50);
    } else if (g_usb_state.type == CONTROLLER_DS3) {
        uint8_t pkt[8] = { 0x00, 0xFF, 0x00, (small_motor > 0) ? 1 : 0, large_motor, 0x00, 0x00, 0x00 };
        p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 50);
    } else if (g_usb_state.type == CONTROLLER_DUALSENSE) {
        uint8_t pkt[48];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x02; // DualSense Output Report
        pkt[1] = 0x03; // Enable right & left rumble
        pkt[3] = small_motor;
        pkt[4] = large_motor;
        p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 50);
    }
}

static inline uint32_t get_guide_button_mask(void) {
    if (g_daemon_config.guide_action == CONFIG_GUIDE_TOUCHPAD) {
        return DS4_BUTTON_TOUCHPAD;
    } else if (g_daemon_config.guide_action == CONFIG_GUIDE_SHARE) {
        return DS4_BUTTON_SHARE;
    } else {
        // CONFIG_GUIDE_NONE (Default): Focus strictly on native PS Button!
        return DS4_BUTTON_PS;
    }
}

static TranslatorConfig s_trans_cfg;
static uint64_t s_last_rescan_us = 0;

int usb_driver_init(void) {
    if (load_usbd() != 0) {
        return -1;
    }

    translator_init(&s_trans_cfg);
    vpad_set_rumble_callback(usb_driver_send_rumble);
    daemon_log(LOG_LEVEL_INFO, "USB Driver initialized successfully, ready for main loop polling");
    return 0;
}

void usb_driver_poll_step(void) {
    uint64_t now_us = daemon_sys_time_us();

    // 1. If disconnected, rescan USB bus at 1 Hz
    if (!g_usb_state.connected || g_dev_handle == NULL) {
        if (now_us - s_last_rescan_us >= 1000000ULL) {
            s_last_rescan_us = now_us;
            scan_and_open();
        }
        return;
    }

    // 2. Read from connected controller
    uint8_t rx[64];
    int transferred = 0;
    int ret = p_sceUsbdInterruptTransfer(g_dev_handle, g_usb_state.endpoint_in, rx, sizeof(rx), &transferred, 4);

    if (ret == 0 && transferred > 0) {
        if (!s_logged_first_report) {
            daemon_log(LOG_LEVEL_INFO, "USB first input report received (%d bytes) from %s",
                       transferred, controller_type_to_str(g_usb_state.type));
            s_logged_first_report = 1;
        }

        OrbisPadData ds4;
        memset(&ds4, 0, sizeof(ds4));

        // Sync user config settings
        s_trans_cfg.stick_deadzone = g_daemon_config.stick_deadzone;
        s_trans_cfg.trigger_threshold = g_daemon_config.trigger_threshold;

        if (g_usb_state.type == CONTROLLER_SWITCH) {
            s_trans_cfg.swap_ab = (g_daemon_config.switch_positional == 0);
            s_trans_cfg.swap_xy = (g_daemon_config.switch_positional == 0);
        } else {
            // Xbox and PlayStation controllers: never swap face buttons (A=Cross, B=Circle, X=Square, Y=Triangle)
            s_trans_cfg.swap_ab = 0;
            s_trans_cfg.swap_xy = 0;
        }

        if (g_usb_state.type == CONTROLLER_XBOX360 && transferred >= 20) {
            if (xbox360_report_valid((const Xbox360Report*)rx)) {
                translator_convert((const Xbox360Report*)rx, &ds4, &s_trans_cfg);
                const Xbox360Report* x360 = (const Xbox360Report*)rx;
                int guide_pressed = (x360->buttons_high & XBOX360_GUIDE) ? 1 : 0;
                if (guide_pressed != s_last_guide_pressed) {
                    s_last_guide_pressed = guide_pressed;
                    daemon_log(LOG_LEVEL_INFO, "Xbox 360 Guide button %s (mask=0x%08X)",
                               guide_pressed ? "PRESSED" : "RELEASED", get_guide_button_mask());
                }
                if (guide_pressed) {
                    ds4.buttons |= get_guide_button_mask();
                }
                // Combo: Back (Share) + Start (Options) -> Touchpad Click (Map/Inventory in games)
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) == (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
                    ds4.buttons |= DS4_BUTTON_TOUCHPAD;
                }
                // Combo: Back (Share) + D-Pad Down -> PS Button (Home)
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
                    ds4.buttons |= DS4_BUTTON_PS;
                }
                s_last_ds4 = ds4;
                vpad_send_data(&ds4);
            }
        } else if (g_usb_state.type == CONTROLLER_XBOXONE && transferred >= 4) {
            if (rx[0] == XBOXONE_REPORT_INPUT && transferred >= 18) {
                translator_convert_xboxone((const XboxOneReport*)rx, &ds4, &s_trans_cfg);
                if (s_xboxone_guide) {
                    ds4.buttons |= get_guide_button_mask();
                }
                // Combo: Back (Share) + Start (Options) -> Touchpad Click (Map/Inventory in games)
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) == (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
                    ds4.buttons |= DS4_BUTTON_TOUCHPAD;
                }
                // Combo: Back (Share) + D-Pad Down -> PS Button (Home)
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
                    ds4.buttons |= DS4_BUTTON_PS;
                }
                s_last_ds4 = ds4;
                vpad_send_data(&ds4);
            } else if (rx[0] == XBOXONE_REPORT_GUIDE) {
                // Send GIP ACK to controller if requested (GIP_OPT_ACK)
                if (rx[1] & 0x01) {
                    uint8_t ack_pkt[13] = {
                        0x01, 0x20, rx[2], 0x09, 0x00, 0x07, 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
                    };
                    send_init_packet(ack_pkt, sizeof(ack_pkt));
                }

                uint8_t gval = (transferred >= 5) ? rx[4] : rx[3];
                int guide_pressed = (gval & 0x01) ? 1 : 0;
                if (guide_pressed != s_last_guide_pressed) {
                    s_last_guide_pressed = guide_pressed;
                    daemon_log(LOG_LEVEL_INFO, "Xbox One Guide button %s (val=0x%02X, len=%d, mask=0x%08X)",
                               guide_pressed ? "PRESSED" : "RELEASED", gval, transferred, get_guide_button_mask());
                }
                s_xboxone_guide = guide_pressed;
                s_last_ds4.buttons &= ~DS4_BUTTON_PS;
                if (s_xboxone_guide) {
                    s_last_ds4.buttons |= get_guide_button_mask();
                }
                s_last_ds4.timestamp = daemon_sys_time_us();
                vpad_send_data(&s_last_ds4);
            }
        } else if (g_usb_state.type == CONTROLLER_SWITCH && transferred >= sizeof(SwitchInputOnlyReport)) {
            translator_convert_switch((const SwitchInputOnlyReport*)rx, &ds4, &s_trans_cfg);
            const SwitchInputOnlyReport* sw = (const SwitchInputOnlyReport*)rx;
            if (sw->buttons1 & SWITCH_BTN_HOME) {
                ds4.buttons |= get_guide_button_mask();
            }
            if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) == (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) {
                ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
                ds4.buttons |= DS4_BUTTON_TOUCHPAD;
            }
            if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
                ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
                ds4.buttons |= DS4_BUTTON_PS;
            }
            s_last_ds4 = ds4;
            vpad_send_data(&ds4);
        } else if (g_usb_state.type == CONTROLLER_DS3 && transferred >= sizeof(DS3Report)) {
            translator_convert_ds3((const DS3Report*)rx, &ds4, &s_trans_cfg);
            if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
                ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
                ds4.buttons |= DS4_BUTTON_PS;
            }
            s_last_ds4 = ds4;
            vpad_send_data(&ds4);
        } else if (g_usb_state.type == CONTROLLER_DUALSENSE && transferred >= sizeof(DualSenseReport)) {
            if (rx[0] == 0x01) {
                translator_convert_dualsense((const DualSenseReport*)rx, &ds4, &s_trans_cfg);
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
                    ds4.buttons |= DS4_BUTTON_PS;
                }
                s_last_ds4 = ds4;
                vpad_send_data(&ds4);
            }
        }
    } else if (ret == 0x80240007 || ret == -7) {
        // Normal USB interrupt timeout: keep 250 Hz stream alive with last known state
        if (s_last_ds4.connected) {
            s_last_ds4.timestamp = daemon_sys_time_us();
            vpad_send_data(&s_last_ds4);
        }
    } else if (ret != 0) {
        // Physical disconnect or USB transfer failure
        if (!p_sceUsbdCheckConnected || p_sceUsbdCheckConnected(g_dev_handle) != 0) {
            close_controller();
        }
    }
}

void usb_driver_shutdown(void) {
    close_controller();
    if (p_sceUsbdExit) {
        p_sceUsbdExit();
    }
}

bool usb_driver_is_connected(void) {
    return g_usb_state.connected;
}
