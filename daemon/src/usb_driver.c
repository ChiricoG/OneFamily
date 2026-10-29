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
#include "generic_hid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "daemon_syscalls.h"

UsbControllerState g_usb_state;

static int g_hUsbd = 0;
static libusb_device_handle *g_dev_handle = NULL;
static libusb_device *g_cur_dev = NULL;
static int g_claimed_interface = -1;
static int s_claimed_kbd_interface = -1;
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
typedef int32_t (*sceUsbdClearHalt_t)(libusb_device_handle*, uint8_t);
typedef int32_t (*sceUsbdDetachKernelDriver_t)(libusb_device_handle*, int32_t);
typedef int32_t (*sceUsbdKernelDriverActive_t)(libusb_device_handle*, int32_t);
typedef int32_t (*sceUsbdSetInterfaceAltSetting_t)(libusb_device_handle*, int32_t, int32_t);
typedef int32_t (*sceUsbdGetConfiguration_t)(libusb_device_handle*, int32_t*);
typedef int32_t (*sceUsbdSetConfiguration_t)(libusb_device_handle*, int32_t);
typedef int32_t (*sceUsbdGetConfigDescriptor_t)(libusb_device*, uint8_t, struct libusb_config_descriptor**);

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
static sceUsbdClearHalt_t                  p_sceUsbdClearHalt = NULL;
static sceUsbdDetachKernelDriver_t         p_sceUsbdDetachKernelDriver = NULL;
static sceUsbdKernelDriverActive_t         p_sceUsbdKernelDriverActive = NULL;
static sceUsbdSetInterfaceAltSetting_t     p_sceUsbdSetInterfaceAltSetting = NULL;
static sceUsbdGetConfiguration_t           p_sceUsbdGetConfiguration = NULL;
static sceUsbdSetConfiguration_t           p_sceUsbdSetConfiguration = NULL;
static sceUsbdGetConfigDescriptor_t        p_sceUsbdGetConfigDescriptor = NULL;

int32_t usbd_get_active_config_descriptor(libusb_device* dev, struct libusb_config_descriptor** config) {
    if (p_sceUsbdGetActiveConfigDescriptor && p_sceUsbdGetActiveConfigDescriptor(dev, config) == 0 && *config) {
        return 0;
    }
    if (p_sceUsbdGetConfigDescriptor && p_sceUsbdGetConfigDescriptor(dev, 0, config) == 0 && *config) {
        return 0;
    }
    return -1;
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
    daemon_sys_dlsym(g_hUsbd, "sceUsbdClearHalt", (void**)&p_sceUsbdClearHalt);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdDetachKernelDriver", (void**)&p_sceUsbdDetachKernelDriver);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdKernelDriverActive", (void**)&p_sceUsbdKernelDriverActive);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdSetInterfaceAltSetting", (void**)&p_sceUsbdSetInterfaceAltSetting);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdGetConfiguration", (void**)&p_sceUsbdGetConfiguration);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdSetConfiguration", (void**)&p_sceUsbdSetConfiguration);
    daemon_sys_dlsym(g_hUsbd, "sceUsbdGetConfigDescriptor", (void**)&p_sceUsbdGetConfigDescriptor);

    if (p_sceUsbdInit) {
        p_sceUsbdInit();
    }
    daemon_log(LOG_LEVEL_INFO, "libSceUsbd initialized successfully (handle=0x%X)", g_hUsbd);
    return 0;
}

static int s_logged_first_report = 0;
static int s_last_guide_pressed = 0;

static void close_controller(void) {
    bool had_vpad = vpad_is_active();
    vpad_shutdown();
    generic_hid_reset();
    if (g_dev_handle) {
        if (s_claimed_kbd_interface >= 0 && p_sceUsbdReleaseInterface) {
            p_sceUsbdReleaseInterface(g_dev_handle, s_claimed_kbd_interface);
            s_claimed_kbd_interface = -1;
        }
        if (g_claimed_interface >= 0 && p_sceUsbdReleaseInterface) {
            p_sceUsbdReleaseInterface(g_dev_handle, g_claimed_interface);
        }
        if (p_sceUsbdClose) {
            p_sceUsbdClose(g_dev_handle);
        }
        g_dev_handle = NULL;
    }
    s_claimed_kbd_interface = -1;
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
    if (had_vpad) {
        daemon_notify("OneFamily: Controller Disconnected");
    }
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

    // PDP / Victrix / Turtle Beach handshake packet 1 (LED on / handshake)
    const uint8_t pdp_init1[] = { 0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 };
    send_init_packet(pdp_init1, sizeof(pdp_init1));
    daemon_sys_usleep(10000);

    // PDP / Victrix / Turtle Beach handshake packet 2 (Input mode unlock)
    const uint8_t pdp_init2[] = { 0x06, 0x20, 0x00, 0x02, 0x01, 0x00 };
    send_init_packet(pdp_init2, sizeof(pdp_init2));
    daemon_sys_usleep(10000);
}

static void init_dualsense_controller(void) {
    daemon_log(LOG_LEVEL_INFO, "Initializing DualSense controller (EP_IN:0x%02X, EP_OUT:0x%02X, IF:%d)...",
               g_usb_state.endpoint_in, g_usb_state.endpoint_out, g_claimed_interface);

    // Full 64-byte DualSense USB output report (Report ID 0x02 + 63 bytes payload)
    // Matches endpoint wMaxPacketSize (64) and Linux hid-playstation DS_OUTPUT_REPORT_USB_SIZE specification
    uint8_t init_pkt[64];
    memset(init_pkt, 0, sizeof(init_pkt));
    init_pkt[0] = 0x02;        // Report ID
    init_pkt[1] = 0x01 | 0x02; // valid_flag0: COMPATIBLE_VIBRATION (0x01) | HAPTICS_SELECT (0x02)
    init_pkt[2] = 0x04 | 0x10; // valid_flag1: LIGHTBAR_CONTROL_ENABLE (0x04) | PLAYER_INDICATOR_CONTROL_ENABLE (0x10)
    init_pkt[3] = 0x00;        // Small motor
    init_pkt[4] = 0x00;        // Large motor
    init_pkt[39] = 0x02;       // valid_flag2: LIGHTBAR_SETUP_CONTROL_ENABLE (0x02)
    init_pkt[42] = 0x02;       // lightbar_setup: Lightbar normal mode
    init_pkt[44] = 0x04;       // player_leds: Player 1 center dot LED ON
    init_pkt[45] = 0x00;       // Red
    init_pkt[46] = 0x00;       // Green
    init_pkt[47] = 0xFF;       // Blue (PlayStation Blue Lightbar)

    int r = send_init_packet(init_pkt, sizeof(init_pkt));
    daemon_log(LOG_LEVEL_INFO, "DualSense init output report sent via Interrupt OUT (ret=0x%08X)", r);

    // Fallback: If Interrupt OUT transfer failed or returned error, also send via Control Transfer SET_REPORT
    if (r != 0 && p_sceUsbdControlTransfer && g_dev_handle) {
        int cr = p_sceUsbdControlTransfer(g_dev_handle, 0x21, 0x09, 0x0202, g_claimed_interface, init_pkt, sizeof(init_pkt), 100);
        daemon_log(LOG_LEVEL_INFO, "DualSense init SET_REPORT control transfer sent (ret=0x%08X)", cr);
    }
}

static int switch_pro_send_and_wait(const uint8_t* tx, int tx_len, uint8_t expected_ack, int timeout_ms) {
    if (!g_dev_handle || !p_sceUsbdInterruptTransfer || g_usb_state.endpoint_out == 0 || g_usb_state.endpoint_in == 0) return -1;

    int transferred = 0;
    int r = p_sceUsbdInterruptTransfer(g_dev_handle, g_usb_state.endpoint_out, (uint8_t*)tx, tx_len, &transferred, 100);
    if (r != 0) {
        daemon_log(LOG_LEVEL_WARN, "Switch Pro send failed (ret=0x%08X)", r);
        return r;
    }

    uint64_t end_time = daemon_sys_time_us() + (uint64_t)timeout_ms * 1000;
    uint8_t rx[64];
    while (daemon_sys_time_us() < end_time) {
        transferred = 0;
        int in_ret = p_sceUsbdInterruptTransfer(g_dev_handle, g_usb_state.endpoint_in, rx, sizeof(rx), &transferred, 20);
        if (in_ret == 0 && transferred > 0) {
            daemon_log(LOG_LEVEL_INFO, "Switch Pro handshake recv [%d bytes]: rx[0]=0x%02X rx[1]=0x%02X",
                       transferred, rx[0], (transferred > 1 ? rx[1] : 0));
            if (expected_ack == 0) {
                return 0; // Don't care about specific ack, packet successfully drained
            }
            if (rx[0] == 0x81 && (expected_ack == 0xFF || rx[1] == expected_ack)) {
                return 0; // Matched expected proprietary ACK
            }
            if (rx[0] == 0x21 || rx[0] == 0x30) {
                return 0; // Already in subcommand reply or full report mode!
            }
        }
        daemon_sys_usleep(2000);
    }

    return -1; // Timed out waiting for ack
}

static void init_switch_pro_controller(void) {
    daemon_log(LOG_LEVEL_INFO, "Starting synchronous Switch Pro USB handshake...");

    uint8_t pkt[64];
    uint8_t rx[64];
    int transferred = 0;

    // Drain any leftover input in the USB FIFO first
    for (int i = 0; i < 5; i++) {
        transferred = 0;
        int dr = p_sceUsbdInterruptTransfer(g_dev_handle, g_usb_state.endpoint_in, rx, sizeof(rx), &transferred, 10);
        if (dr != 0 || transferred == 0) break;
        daemon_log(LOG_LEVEL_INFO, "Drained initial Switch packet [%d bytes]: 0x%02X", transferred, rx[0]);
    }

    // Step 1: Handshake (0x80, 0x02) - with up to 3 attempts
    for (int attempt = 1; attempt <= 3; attempt++) {
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x80;
        pkt[1] = 0x02;
        int r = switch_pro_send_and_wait(pkt, sizeof(pkt), 0x02, 100);
        daemon_log(LOG_LEVEL_INFO, "Switch Pro Handshake (0x80, 0x02) attempt %d ret=%d", attempt, r);
        if (r == 0) break;
        daemon_sys_usleep(10000);
    }

    // Step 2: High Speed baudrate (0x80, 0x03)
    for (int attempt = 1; attempt <= 3; attempt++) {
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x80;
        pkt[1] = 0x03;
        int r = switch_pro_send_and_wait(pkt, sizeof(pkt), 0x03, 100);
        daemon_log(LOG_LEVEL_INFO, "Switch Pro HighSpeed (0x80, 0x03) attempt %d ret=%d", attempt, r);
        if (r == 0) break;
        daemon_sys_usleep(10000);
    }

    // Step 3: Handshake repeat (0x80, 0x02)
    for (int attempt = 1; attempt <= 3; attempt++) {
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x80;
        pkt[1] = 0x02;
        int r = switch_pro_send_and_wait(pkt, sizeof(pkt), 0x02, 100);
        daemon_log(LOG_LEVEL_INFO, "Switch Pro Handshake 2 (0x80, 0x02) attempt %d ret=%d", attempt, r);
        if (r == 0) break;
        daemon_sys_usleep(10000);
    }

    // Step 4: Force USB (0x80, 0x04)
    for (int attempt = 1; attempt <= 3; attempt++) {
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x80;
        pkt[1] = 0x04;
        int r = switch_pro_send_and_wait(pkt, sizeof(pkt), 0x04, 100);
        daemon_log(LOG_LEVEL_INFO, "Switch Pro ForceUSB (0x80, 0x04) attempt %d ret=%d", attempt, r);
        if (r == 0) break;
        daemon_sys_usleep(10000);
    }

    // Step 5: Subcommand 0x03 (Set Input Report Mode to 0x30 Full Mode)
    for (int attempt = 1; attempt <= 3; attempt++) {
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x01; // Output report ID
        pkt[1] = attempt; // Packet counter
        pkt[2] = 0x00; pkt[3] = 0x01; pkt[4] = 0x40; pkt[5] = 0x40; // Neutral rumble L
        pkt[6] = 0x00; pkt[7] = 0x01; pkt[8] = 0x40; pkt[9] = 0x40; // Neutral rumble R
        pkt[10] = 0x03; // Subcommand: Set Input Mode
        pkt[11] = 0x30; // 0x30 = Standard full report mode
        int r = switch_pro_send_and_wait(pkt, sizeof(pkt), 0, 150); // wait for 0x21 or 0x30
        daemon_log(LOG_LEVEL_INFO, "Switch Pro SetInputMode 0x30 attempt %d ret=%d", attempt, r);
        if (r == 0) break;
        daemon_sys_usleep(10000);
    }

    // Step 6: Subcommand 0x30 (Player 1 LED)
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x01;
    pkt[1] = 0x04;
    pkt[2] = 0x00; pkt[3] = 0x01; pkt[4] = 0x40; pkt[5] = 0x40;
    pkt[6] = 0x00; pkt[7] = 0x01; pkt[8] = 0x40; pkt[9] = 0x40;
    pkt[10] = 0x30; // Subcommand: Set Player Lights
    pkt[11] = 0x01; // LED 1 ON
    switch_pro_send_and_wait(pkt, sizeof(pkt), 0, 100);

    daemon_log(LOG_LEVEL_INFO, "Switch Pro initialization sequence completed");
}

static const char* controller_type_to_str(ControllerType type) {
    switch (type) {
        case CONTROLLER_XBOX360:  return "Xbox 360";
        case CONTROLLER_XBOXONE:  return "Xbox One/Series";
        case CONTROLLER_SWITCH:   return "Switch";
        case CONTROLLER_SWITCH_PRO:return "Switch Pro";
        case CONTROLLER_DS3:      return "DualShock 3";
        case CONTROLLER_DUALSENSE:return "DualSense";
        case CONTROLLER_STADIA:   return "Google Stadia";
        case CONTROLLER_GENERIC_HID:return "Generic HID Gamepad";
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
    static int s_dualsense_notified = 0;
    int dualsense_seen = 0;

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
            // DualSense support is currently inhibited / in development
            if (det.type == CONTROLLER_DUALSENSE) {
                dualsense_seen = 1;
                if (!s_dualsense_notified) {
                    daemon_notify("OneFamily: DualSense support is in development");
                    daemon_log(LOG_LEVEL_INFO, "DualSense detected (VID:0x%04X PID:0x%04X) - Support is in development, skipping virtual pad instantiation",
                               desc.idVendor, desc.idProduct);
                    s_dualsense_notified = 1;
                }
                continue;
            }

            libusb_device_handle* h = NULL;
            if (p_sceUsbdOpen(dev, &h) == 0 && h != NULL) {
                if (det.type == CONTROLLER_DUALSENSE) {
                    int curr_cfg = -1;
                    if (p_sceUsbdGetConfiguration) {
                        int gcr = p_sceUsbdGetConfiguration(h, &curr_cfg);
                        daemon_log(LOG_LEVEL_INFO, "DualSense: GetConfiguration = 0x%08X (curr_cfg=%d)", gcr, curr_cfg);
                    }
                    if (curr_cfg <= 0 && p_sceUsbdSetConfiguration) {
                        int scr = p_sceUsbdSetConfiguration(h, 1);
                        daemon_log(LOG_LEVEL_INFO, "DualSense: SetConfiguration(1) = 0x%08X", scr);
                    }

                    // Detach kernel drivers from all interfaces (Audio 0,1,2 + HID 3)
                    if (p_sceUsbdDetachKernelDriver) {
                        for (int iface = 0; iface < 4; iface++) {
                            int dr = p_sceUsbdDetachKernelDriver(h, iface);
                            daemon_log(LOG_LEVEL_INFO, "DualSense: DetachKernelDriver(IF=%d) = 0x%08X", iface, dr);
                        }
                    }
                }

                // If device exposed a secondary composite boot keyboard interface (e.g. 8BitDo dongles),
                // detach and claim it so Orbis OS ukbd driver will not recognize it as a keyboard.
                if (det.kbd_interface_num != 0xFF) {
                    if (p_sceUsbdDetachKernelDriver) {
                        int dkr = p_sceUsbdDetachKernelDriver(h, det.kbd_interface_num);
                        daemon_log(LOG_LEVEL_INFO, "Secondary Keyboard IF %d DetachKernelDriver = 0x%08X", det.kbd_interface_num, dkr);
                    }
                    if (p_sceUsbdClaimInterface) {
                        int ckr = p_sceUsbdClaimInterface(h, det.kbd_interface_num);
                        daemon_log(LOG_LEVEL_INFO, "Secondary Keyboard IF %d ClaimInterface = 0x%08X", det.kbd_interface_num, ckr);
                        if (ckr == 0) {
                            s_claimed_kbd_interface = det.kbd_interface_num;
                        }
                    }
                }

                if (p_sceUsbdDetachKernelDriver && det.type == CONTROLLER_GENERIC_HID) {
                    p_sceUsbdDetachKernelDriver(h, det.interface_num);
                }

                if (p_sceUsbdClaimInterface(h, det.interface_num) == 0) {
                    if (det.alt_setting > 0 && p_sceUsbdSetInterfaceAltSetting) {
                        int as_ret = p_sceUsbdSetInterfaceAltSetting(h, det.interface_num, det.alt_setting);
                        daemon_log(LOG_LEVEL_INFO, "SetInterfaceAltSetting(IF=%d, alt=%d) = 0x%08X", det.interface_num, det.alt_setting, as_ret);
                    }

                    if (p_sceUsbdClearHalt) {
                        int chr_in = p_sceUsbdClearHalt(h, det.ep_in);
                        int chr_out = det.ep_out ? p_sceUsbdClearHalt(h, det.ep_out) : 0;
                        if (det.type == CONTROLLER_DUALSENSE) {
                            daemon_log(LOG_LEVEL_INFO, "DualSense: ClearHalt IN(0x%02X)=0x%08X, OUT(0x%02X)=0x%08X",
                                       det.ep_in, chr_in, det.ep_out, chr_out);
                        }
                    }

                    g_dev_handle = h;
                    g_cur_dev = dev;
                    g_claimed_interface = det.interface_num;
                    g_usb_state.connected = true;
                    g_usb_state.vid = desc.idVendor;
                    g_usb_state.pid = desc.idProduct;
                    g_usb_state.type = det.type;
                    g_usb_state.endpoint_in = det.ep_in;
                    g_usb_state.endpoint_out = det.ep_out;
                    g_usb_state.is_wireless = det.is_wireless;
                    found = 1;

                    daemon_log(LOG_LEVEL_INFO, "Controller claimed: %s (VID:0x%04X PID:0x%04X, EP_IN:0x%02X, wireless=%d)",
                               controller_type_to_str(det.type), desc.idVendor, desc.idProduct, det.ep_in, det.is_wireless);

                    memset(&s_last_ds4, 0, sizeof(s_last_ds4));
                    s_last_ds4.leftStick.x = 128;
                    s_last_ds4.leftStick.y = 128;
                    s_last_ds4.rightStick.x = 128;
                    s_last_ds4.rightStick.y = 128;
                    s_last_ds4.connected = 1;
                    s_last_ds4.count = 1;

                    if (det.type != CONTROLLER_DUALSENSE && det.type != CONTROLLER_GENERIC_HID) {
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
                        } else if (det.type == CONTROLLER_SWITCH_PRO) {
                            init_switch_pro_controller();
                        }

                        daemon_notify("OneFamily: %s Connected",
                                      controller_type_to_str(det.type));
                    } else if (det.type == CONTROLLER_GENERIC_HID) {
                        generic_hid_setup(h, desc.idVendor, desc.idProduct, det.interface_num, p_sceUsbdControlTransfer);
                        daemon_log(LOG_LEVEL_INFO, "Generic HID claimed: awaiting first valid input report before activating Virtual DS4");
                    } else {
                        daemon_log(LOG_LEVEL_INFO, "DualSense claimed: awaiting first valid input report before activating Virtual DS4");
                    }
                } else {
                    p_sceUsbdClose(h);
                }
            }
        }
    }

    if (!dualsense_seen) {
        s_dualsense_notified = 0;
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
        if (g_usb_state.is_wireless) {
            // Wireless Xbox 360 Receiver Rumble (12 bytes)
            uint8_t pkt12[12] = {
                0x00, 0x01, 0x0F, 0xC0,
                0x00, large_motor, small_motor, 0x00,
                0x00, 0x00, 0x00, 0x00
            };
            p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt12, sizeof(pkt12), &transferred, 50);
        } else {
            uint8_t pkt[8] = { 0x00, 0x08, 0x00, large_motor, small_motor, 0x00, 0x00, 0x00 };
            int ret = p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 50);
            if (ret != 0) {
                uint8_t pkt4[4] = { 0x00, 0x04, large_motor, small_motor };
                p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt4, sizeof(pkt4), &transferred, 50);
            }
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
        if (!vpad_is_active()) return;
        uint8_t pkt[64];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x02;        // DualSense Output Report
        pkt[1] = 0x01 | 0x02; // Enable compatible rumble & haptics
        pkt[2] = 0x04 | 0x10; // Lightbar & Player LED keepalive
        pkt[3] = small_motor;
        pkt[4] = large_motor;
        pkt[39] = 0x02;       // valid_flag2: Lightbar setup
        pkt[42] = 0x02;       // Lightbar normal
        pkt[44] = 0x04;       // Player 1 LED
        pkt[47] = 0xFF;       // Blue lightbar
        int r = p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 50);
        if (r != 0 && p_sceUsbdControlTransfer) {
            p_sceUsbdControlTransfer(g_dev_handle, 0x21, 0x09, 0x0202, g_claimed_interface, pkt, sizeof(pkt), 50);
        }
    } else if (g_usb_state.type == CONTROLLER_STADIA) {
        // Stadia Rumble Output Report (Report ID 0x05, 5 bytes)
        // [0] = 0x05
        // [1..2] = strong_magnitude (16-bit LE, 0..65535)
        // [3..4] = weak_magnitude (16-bit LE, 0..65535)
        uint16_t strong = (uint16_t)large_motor * 257;
        uint16_t weak   = (uint16_t)small_motor * 257;
        uint8_t pkt[5] = {
            STADIA_REPORT_ID_RUMBLE,
            (uint8_t)(strong & 0xFF),
            (uint8_t)((strong >> 8) & 0xFF),
            (uint8_t)(weak & 0xFF),
            (uint8_t)((weak >> 8) & 0xFF)
        };
        int sent = 0;
        if (ep != 0) {
            if (p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 50) == 0) {
                sent = 1;
            }
        }
        if (!sent && p_sceUsbdControlTransfer) {
            p_sceUsbdControlTransfer(g_dev_handle, 0x21, 0x09, 0x0205, g_claimed_interface, pkt, sizeof(pkt), 50);
        }
    } else if (g_usb_state.type == CONTROLLER_GENERIC_HID) {
        generic_hid_send_rumble(g_dev_handle, g_usb_state.vid, g_usb_state.pid,
                                g_usb_state.endpoint_out, g_claimed_interface,
                                large_motor, small_motor,
                                p_sceUsbdInterruptTransfer, p_sceUsbdControlTransfer);
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
    uint8_t rx[128];
    int transferred = 0;
    int read_len = sizeof(rx);
    if (g_usb_state.type == CONTROLLER_DUALSENSE) {
        read_len = 64; // DualSense EP 0x84 wMaxPacketSize is strictly 64 bytes
    }
    int ret = p_sceUsbdInterruptTransfer(g_dev_handle, g_usb_state.endpoint_in, rx, read_len, &transferred, 16);

    // DualSense fallback: If Interrupt IN transfer failed, query via standard USB HID GET_REPORT on control pipe
    if (g_usb_state.type == CONTROLLER_DUALSENSE && ret != 0 && p_sceUsbdControlTransfer) {
        // bmRequestType: 0xA1, bRequest: 0x01 (GET_REPORT), wValue: 0x0101 (Report 1, Input), wIndex: g_claimed_interface (3)
        int cr = p_sceUsbdControlTransfer(g_dev_handle, 0xA1, 0x01, 0x0101, g_claimed_interface, rx, read_len, 20);
        if (cr >= 10) {
            ret = 0;
            transferred = cr;
            static int s_logged_ds_ctrl_rx = 0;
            if (s_logged_ds_ctrl_rx < 5) {
                daemon_log(LOG_LEVEL_INFO, "DualSense input report received via GET_REPORT control transfer (%d bytes)", transferred);
                s_logged_ds_ctrl_rx++;
            }
        } else {
            static int s_logged_ds_ctrl_err = 0;
            if (s_logged_ds_ctrl_err < 5) {
                daemon_log(LOG_LEVEL_WARN, "DualSense GET_REPORT control transfer returned 0x%08X (wIndex=%d)", cr, g_claimed_interface);
                s_logged_ds_ctrl_err++;
            }
        }
    }

    static int s_consecutive_disconnects = 0;
    static int s_consecutive_transfer_errors = 0;

    if (ret == 0 && transferred > 0) {
        s_consecutive_disconnects = 0;
        s_consecutive_transfer_errors = 0;
        if (!s_logged_first_report) {
            daemon_log(LOG_LEVEL_INFO, "USB first input report received (%d bytes) from %s",
                       transferred, controller_type_to_str(g_usb_state.type));
            s_logged_first_report = 1;
        }

        // DualSense: Activate Virtual DS4 and initialize output report once first valid input arrives
        if (g_usb_state.type == CONTROLLER_DUALSENSE && !vpad_is_active()) {
            daemon_log(LOG_LEVEL_INFO, "DualSense first valid report received (%d bytes) - Activating Virtual DS4", transferred);
            vpad_init();
            init_dualsense_controller();
            daemon_notify("OneFamily: DualSense Connected");
        }

        OrbisPadData ds4;
        memset(&ds4, 0, sizeof(ds4));

        // Sync user config settings
        s_trans_cfg.stick_deadzone = g_daemon_config.stick_deadzone;
        s_trans_cfg.trigger_threshold = g_daemon_config.trigger_threshold;

        if (g_usb_state.type == CONTROLLER_SWITCH || g_usb_state.type == CONTROLLER_SWITCH_PRO) {
            s_trans_cfg.swap_ab = (g_daemon_config.switch_positional == 0);
            s_trans_cfg.swap_xy = (g_daemon_config.switch_positional == 0);
        } else {
            // Xbox and PlayStation controllers: never swap face buttons (A=Cross, B=Circle, X=Square, Y=Triangle)
            s_trans_cfg.swap_ab = 0;
            s_trans_cfg.swap_xy = 0;
        }

        if (g_usb_state.type == CONTROLLER_XBOX360 && transferred >= 14) {
            static int s_logged_x360_report = 0;
            if (s_logged_x360_report < 5) {
                daemon_log(LOG_LEVEL_INFO, "Xbox 360 report [%d bytes, wireless=%d]: rx[0]=0x%02X rx[1]=0x%02X rx[2]=0x%02X rx[3]=0x%02X",
                           transferred, g_usb_state.is_wireless, rx[0], rx[1], rx[2], rx[3]);
                s_logged_x360_report++;
            }

            const Xbox360Report* x360 = NULL;
            Xbox360Report temp_report;

            if (g_usb_state.is_wireless && transferred >= 24 && rx[4] == XBOX360_REPORT_INPUT) {
                // Wireless receiver report (4-byte RF/slot header prefix, payload at &rx[4])
                if (rx[1] == 0x01 || rx[1] == 0x00) {
                    x360 = (const Xbox360Report*)(&rx[4]);
                }
            } else if (rx[0] == XBOX360_REPORT_INPUT) {
                // Wired Xbox 360 report (Linux xpad style: accepts any report starting with type 0x00)
                if (transferred < (int)sizeof(Xbox360Report)) {
                    memset(&temp_report, 0, sizeof(temp_report));
                    memcpy(&temp_report, rx, transferred);
                    x360 = &temp_report;
                } else {
                    x360 = (const Xbox360Report*)rx;
                }
            } else if (transferred >= 24 && rx[4] == XBOX360_REPORT_INPUT && (rx[1] == 0x01 || rx[1] == 0x00)) {
                // Wireless receiver report fallback if descriptor flag was wired
                x360 = (const Xbox360Report*)(&rx[4]);
            }

            if (x360 != NULL) {
                translator_convert(x360, &ds4, &s_trans_cfg);
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
                // Send GIP ACK to controller if requested (GIP_OPT_ACK: 0x10 or 0x01)
                if ((rx[1] & 0x10) || (rx[1] & 0x01)) {
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
            } else {
                // Handle general GIP control / status / announce packets requiring host ACK
                if (transferred >= 3 && ((rx[1] & 0x10) || (rx[1] & 0x01))) {
                    uint8_t ack_pkt[13] = {
                        0x01, 0x20, rx[2], 0x09, 0x00, rx[0], 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
                    };
                    send_init_packet(ack_pkt, sizeof(ack_pkt));
                    daemon_log(LOG_LEVEL_INFO, "GIP ACK sent for cmd 0x%02X (seq 0x%02X, len %d)", rx[0], rx[2], transferred);
                }
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
        } else if (g_usb_state.type == CONTROLLER_SWITCH_PRO && transferred >= 11) {
            static int s_logged_sw_report = 0;
            if (s_logged_sw_report < 10) {
                daemon_log(LOG_LEVEL_INFO, "Switch Pro report [%d bytes]: rx[0]=0x%02X b3=0x%02X b4=0x%02X b5=0x%02X",
                           transferred, rx[0], (transferred > 3 ? rx[3] : 0), (transferred > 4 ? rx[4] : 0), (transferred > 5 ? rx[5] : 0));
                s_logged_sw_report++;
            }

            if (rx[0] == 0x30 || rx[0] == 0x21 || rx[0] == 0x31) {
                if (transferred >= 12) {
                    translator_convert_switch_pro((const SwitchFullReport*)rx, &ds4, &s_trans_cfg);

                    // Combo: Minus (Share) + Plus (Options) -> Touchpad Click
                    if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) == (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) {
                        ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
                        ds4.buttons |= DS4_BUTTON_TOUCHPAD;
                    }
                    // Combo: Minus (Share) + D-Pad Down -> PS Button (Home)
                    if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
                        ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
                        ds4.buttons |= DS4_BUTTON_PS;
                    }

                    static uint32_t s_last_sw_btn = 0;
                    if (ds4.buttons != s_last_sw_btn) {
                        s_last_sw_btn = ds4.buttons;
                        daemon_log(LOG_LEVEL_INFO, "Switch Pro buttons active: 0x%08X", ds4.buttons);
                    }

                    s_last_ds4 = ds4;
                    vpad_send_data(&ds4);
                }
            } else if (rx[0] == 0x3F && transferred >= sizeof(SwitchSimpleReport)) {
                const SwitchSimpleReport* simple = (const SwitchSimpleReport*)rx;
                SwitchInputOnlyReport sw_fallback;
                sw_fallback.buttons0 = simple->buttons0;
                sw_fallback.buttons1 = simple->buttons1;
                sw_fallback.hat = simple->hat;
                sw_fallback.left_stick_x = (uint8_t)(((int32_t)simple->left_stick_x + 32768) >> 8);
                sw_fallback.left_stick_y = (uint8_t)(((int32_t)simple->left_stick_y + 32768) >> 8);
                sw_fallback.right_stick_x = (uint8_t)(((int32_t)simple->right_stick_x + 32768) >> 8);
                sw_fallback.right_stick_y = (uint8_t)(((int32_t)simple->right_stick_y + 32768) >> 8);

                translator_convert_switch(&sw_fallback, &ds4, &s_trans_cfg);
                if (sw_fallback.buttons1 & SWITCH_BTN_HOME) {
                    ds4.buttons |= get_guide_button_mask();
                }
                s_last_ds4 = ds4;
                vpad_send_data(&ds4);
            }
        } else if (g_usb_state.type == CONTROLLER_DS3 && transferred >= sizeof(DS3Report)) {
            translator_convert_ds3((const DS3Report*)rx, &ds4, &s_trans_cfg);
            if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
                ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
                ds4.buttons |= DS4_BUTTON_PS;
            }
            s_last_ds4 = ds4;
            vpad_send_data(&ds4);
        } else if (g_usb_state.type == CONTROLLER_DUALSENSE && transferred >= 10) {
            static int s_logged_ds_report = 0;
            if (s_logged_ds_report < 10) {
                daemon_log(LOG_LEVEL_INFO, "DualSense report [%d bytes]: rx[0]=0x%02X b0=0x%02X b1=0x%02X b2=0x%02X",
                           transferred, rx[0], (transferred > 8 ? rx[8] : 0), (transferred > 9 ? rx[9] : 0), (transferred > 10 ? rx[10] : 0));
                s_logged_ds_report++;
            }

            const DualSenseReport* ps5 = NULL;
            DualSenseReport temp_report;
            if (rx[0] == 0x01 && transferred >= 11) {
                ps5 = (const DualSenseReport*)rx;
            } else if (transferred >= 10) {
                memset(&temp_report, 0, sizeof(temp_report));
                temp_report.report_id = 0x01;
                int copy_len = transferred > 63 ? 63 : transferred;
                memcpy(&temp_report.left_stick_x, rx, copy_len);
                ps5 = &temp_report;
            }
            if (ps5) {
                translator_convert_dualsense(ps5, &ds4, &s_trans_cfg);
                // Combo: Create (Share) + Options -> Touchpad Click (Map/Inventory in games)
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) == (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
                    ds4.buttons |= DS4_BUTTON_TOUCHPAD;
                }
                // Combo: Create (Share) + D-Pad Down -> PS Button (Home)
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
                    ds4.buttons |= DS4_BUTTON_PS;
                }

                static uint32_t s_last_ds_btn = 0;
                if (ds4.buttons != s_last_ds_btn) {
                    s_last_ds_btn = ds4.buttons;
                    daemon_log(LOG_LEVEL_INFO, "DualSense buttons active: 0x%08X", ds4.buttons);
                }

                s_last_ds4 = ds4;
                vpad_send_data(&ds4);
            }
        } else if (g_usb_state.type == CONTROLLER_STADIA && transferred >= 9) {
            static int s_logged_stadia_report = 0;
            if (s_logged_stadia_report < 10) {
                daemon_log(LOG_LEVEL_INFO, "Stadia report [%d bytes]: rx[0]=0x%02X rx[1]=0x%02X rx[2]=0x%02X rx[3]=0x%02X",
                           transferred, rx[0], (transferred > 1 ? rx[1] : 0), (transferred > 2 ? rx[2] : 0), (transferred > 3 ? rx[3] : 0));
                s_logged_stadia_report++;
            }

            const StadiaInputReport* stadia = NULL;
            StadiaInputReport temp_report;
            if (rx[0] == STADIA_REPORT_ID_INPUT && transferred >= (int)sizeof(StadiaInputReport)) {
                stadia = (const StadiaInputReport*)rx;
            } else if (transferred >= 9) {
                // If report ID 0x03 was stripped by USB host
                memset(&temp_report, 0, sizeof(temp_report));
                temp_report.report_id = STADIA_REPORT_ID_INPUT;
                int copy_len = transferred > (int)(sizeof(StadiaInputReport) - 1) ? (int)(sizeof(StadiaInputReport) - 1) : transferred;
                memcpy(&temp_report.dpad, rx, copy_len);
                stadia = &temp_report;
            }

            if (stadia) {
                translator_convert_stadia(stadia, &ds4, &s_trans_cfg);

                // Combo: Options (Share) + Menu (Options) -> Touchpad Click (Map/Inventory in games)
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) == (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
                    ds4.buttons |= DS4_BUTTON_TOUCHPAD;
                }
                // Combo: Options (Share) + D-Pad Down -> PS Button (Home)
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
                    ds4.buttons |= DS4_BUTTON_PS;
                }

                static uint32_t s_last_stadia_btn = 0;
                if (ds4.buttons != s_last_stadia_btn) {
                    s_last_stadia_btn = ds4.buttons;
                    daemon_log(LOG_LEVEL_INFO, "Stadia buttons active: 0x%08X", ds4.buttons);
                }

                s_last_ds4 = ds4;
                vpad_send_data(&ds4);
            }
        } else if (g_usb_state.type == CONTROLLER_GENERIC_HID && transferred > 0) {
            static int s_logged_generic_report = 0;
            if (s_logged_generic_report < 10) {
                daemon_log(LOG_LEVEL_INFO, "Generic HID report [%d bytes]: rx[0]=0x%02X rx[1]=0x%02X rx[2]=0x%02X rx[3]=0x%02X",
                           transferred, rx[0], (transferred > 1 ? rx[1] : 0), (transferred > 2 ? rx[2] : 0), (transferred > 3 ? rx[3] : 0));
                s_logged_generic_report++;
            }

            if (generic_hid_parse_report(rx, transferred, &ds4, &s_trans_cfg)) {
                if (!vpad_is_active()) {
                    daemon_log(LOG_LEVEL_INFO, "Generic HID first valid report received (%d bytes) - Activating Virtual DS4", transferred);
                    vpad_init();
                    daemon_notify("OneFamily: Generic Gamepad Connected");
                }

                // Combo: Share + Options -> Touchpad Click (Map/Inventory in games)
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) == (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
                    ds4.buttons |= DS4_BUTTON_TOUCHPAD;
                }
                // Combo: Share + D-Pad Down -> PS Button (Home)
                if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
                    ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
                    ds4.buttons |= DS4_BUTTON_PS;
                }

                s_last_ds4 = ds4;
                vpad_send_data(&ds4);
            }
        }
    } else {
        // Check for physical disconnect on any transfer error/timeout
        if (p_sceUsbdCheckConnected != NULL && p_sceUsbdCheckConnected(g_dev_handle) != 0) {
            s_consecutive_disconnects++;
            if (s_consecutive_disconnects >= 3) {
                daemon_log(LOG_LEVEL_INFO, "USB controller physical disconnect confirmed (ret=0x%08X)", ret);
                close_controller();
                s_consecutive_disconnects = 0;
                return;
            }
        } else {
            s_consecutive_disconnects = 0;
        }

        if (ret == 0x80240007 || ret == -7) {
            if (g_usb_state.type == CONTROLLER_DUALSENSE) {
                static int s_logged_ds_timeout = 0;
                if (s_logged_ds_timeout < 5) {
                    daemon_log(LOG_LEVEL_WARN, "DualSense USB interrupt transfer timed out (0x80240007, ep=0x%02X)", g_usb_state.endpoint_in);
                    s_logged_ds_timeout++;
                }
            }
            // Normal USB interrupt timeout: keep 250 Hz stream alive with last known state
            if (s_last_ds4.connected && vpad_is_active()) {
                s_last_ds4.timestamp = daemon_sys_time_us();
                vpad_send_data(&s_last_ds4);
            }
        } else if (ret != 0) {
            s_consecutive_transfer_errors++;
            if (g_usb_state.type == CONTROLLER_DUALSENSE) {
                static int s_logged_ds_err = 0;
                if (s_logged_ds_err < 10) {
                    daemon_log(LOG_LEVEL_WARN, "DualSense interrupt transfer returned 0x%08X (ep=0x%02X, count=%d)",
                               ret, g_usb_state.endpoint_in, s_consecutive_transfer_errors);
                    s_logged_ds_err++;
                }
                // If stalled, attempt ClearHalt on input endpoint
                if ((ret == 0x802400FF || ret == 0x80240009) && p_sceUsbdClearHalt && (s_consecutive_transfer_errors % 50 == 1)) {
                    p_sceUsbdClearHalt(g_dev_handle, g_usb_state.endpoint_in);
                }
            } else if (g_usb_state.type == CONTROLLER_SWITCH_PRO) {
                daemon_log(LOG_LEVEL_WARN, "Switch Pro interrupt transfer returned 0x%08X (ep=0x%02X, count=%d)",
                           ret, g_usb_state.endpoint_in, s_consecutive_transfer_errors);
            } else if (g_usb_state.type == CONTROLLER_STADIA) {
                daemon_log(LOG_LEVEL_WARN, "Stadia interrupt transfer returned 0x%08X (ep=0x%02X, count=%d)",
                           ret, g_usb_state.endpoint_in, s_consecutive_transfer_errors);
            } else if (g_usb_state.type == CONTROLLER_GENERIC_HID) {
                static int s_logged_gen_err = 0;
                if (s_logged_gen_err < 10) {
                    daemon_log(LOG_LEVEL_WARN, "Generic HID interrupt transfer returned 0x%08X (ep=0x%02X, count=%d)",
                               ret, g_usb_state.endpoint_in, s_consecutive_transfer_errors);
                    s_logged_gen_err++;
                }
            }

            // Check if device is physically disconnected or persistent transfer failures occur
            int max_allowed_errors = (g_usb_state.type == CONTROLLER_DUALSENSE || g_usb_state.type == CONTROLLER_GENERIC_HID) ? 250 : 25;
            int physically_disconnected = (p_sceUsbdCheckConnected != NULL && p_sceUsbdCheckConnected(g_dev_handle) != 0);
            if (physically_disconnected || s_consecutive_transfer_errors >= max_allowed_errors) {
                daemon_log(LOG_LEVEL_INFO, "Closing controller after persistent transfer failure (ret=0x%08X, err_count=%d)",
                           ret, s_consecutive_transfer_errors);
                s_consecutive_transfer_errors = 0;
                close_controller();
            } else {
                // Keep 250 Hz alive with last known state during transient error recovery
                if (s_last_ds4.connected && vpad_is_active()) {
                    s_last_ds4.timestamp = daemon_sys_time_us();
                    vpad_send_data(&s_last_ds4);
                }
            }
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
