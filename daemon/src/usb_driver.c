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
#include "bt_driver.h"
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
static const char* controller_type_to_str(ControllerType type);
static const char* controller_type_to_id(ControllerType type);

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
static int s_logged_x360_report = 0;
static int s_logged_generic_report = 0;
static int s_last_guide_pressed = 0;
static int s_consecutive_disconnects = 0;
static int s_consecutive_transfer_errors = 0;
static int s_consecutive_timeouts = 0;
static int s_x360_ep_out_stalls = 0;
static int s_xone_ep_out_stalls = 0;
static int s_x360_wireless_present = -1;

static void write_status_file(bool connected, ControllerType type, uint16_t vid, uint16_t pid, const char* name, int nbuttons) {
    daemon_sys_mkdir("/data", 0777);
    daemon_sys_mkdir("/data/onefamily", 0777);
    int fd = daemon_sys_open("/data/onefamily/onefamily.status", 0x0001 | 0x0200 | 0x0400, 0666);
    if (fd >= 0) {
        char buf[256];
        int len = snprintf(buf, sizeof(buf),
            "connected = %d\n"
            "type = %s\n"
            "vid = 0x%04X\n"
            "pid = 0x%04X\n"
            "name = %s\n"
            "nbuttons = %d\n"
            "player_slot = %d\n"
            "user_name = %s\n",
            connected ? 1 : 0,
            controller_type_to_id(type),
            vid, pid,
            name ? name : "Unknown",
            nbuttons,
            connected ? vpad_get_assigned_slot() : 1,
            connected ? vpad_get_assigned_user() : "None"
        );
        daemon_sys_write(fd, buf, len);
        daemon_sys_close(fd);
    }
}

static void close_controller(void);

static uint16_t s_failed_cooldown_vid = 0;
static uint16_t s_failed_cooldown_pid = 0;
static uint64_t s_failed_cooldown_until_us = 0;

static int      s_usb_vpad_slot = -1;
static bool     s_share_combo_active = false;

static void close_controller(void) {
    bool had_vpad = (s_usb_vpad_slot >= 0 ? vpad_slot_is_active(s_usb_vpad_slot) : vpad_is_active());
    uint16_t closing_vid = g_usb_state.vid;
    uint16_t closing_pid = g_usb_state.pid;
    int had_any_reports = s_logged_first_report;
    int had_errors = s_consecutive_transfer_errors;

    if (s_usb_vpad_slot >= 0) {
        vpad_slot_close(s_usb_vpad_slot);
        s_usb_vpad_slot = -1;
    } else {
        vpad_shutdown();
    }
    generic_hid_reset();
    write_status_file(false, CONTROLLER_NONE, 0, 0, "None", 0);
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
    s_logged_x360_report = 0;
    s_logged_generic_report = 0;
    s_last_guide_pressed = 0;
    s_consecutive_disconnects = 0;
    s_consecutive_transfer_errors = 0;
    s_consecutive_timeouts = 0;
    s_x360_ep_out_stalls = 0;
    s_xone_ep_out_stalls = 0;
    s_x360_wireless_present = -1;
    s_share_combo_active = false;
    memset(&s_last_ds4, 0, sizeof(s_last_ds4));
    s_last_ds4.leftStick.x = 128;
    s_last_ds4.leftStick.y = 128;
    s_last_ds4.rightStick.x = 128;
    s_last_ds4.rightStick.y = 128;

    // Notification loop mitigation: if controller closed due to persistent transfer errors without streaming,
    // apply a 15-second cooldown before attempting to claim this device again.
    if (!had_any_reports && closing_vid != 0 && had_errors >= 25) {
        s_failed_cooldown_vid = closing_vid;
        s_failed_cooldown_pid = closing_pid;
        s_failed_cooldown_until_us = daemon_sys_time_us() + 15000000ULL;
        daemon_log(LOG_LEVEL_WARN, "Controller VID:0x%04X PID:0x%04X entered 15s cooldown (persistent transfer errors=%d)",
                   closing_vid, closing_pid, had_errors);
    }

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

static int send_gip_ack(const uint8_t* pkt, int len) {
    if (!g_dev_handle || !p_sceUsbdInterruptTransfer || g_usb_state.endpoint_out == 0) return -1;
    int transferred = 0;
    return p_sceUsbdInterruptTransfer(g_dev_handle, g_usb_state.endpoint_out, (uint8_t*)pkt, len, &transferred, 5);
}

static void init_gip_controller(void) {
    daemon_log(LOG_LEVEL_INFO, "Sending GIP initialization handshake...");
    static uint8_t s_gip_init_seq = 1;

    // 1. Power On / Host connection request (Generic Xbox One baseline)
    uint8_t start_cmd[] = { 0x05, 0x20, s_gip_init_seq++, 0x01, 0x00 };
    send_init_packet(start_cmd, sizeof(start_cmd));
    daemon_sys_usleep(10000);

    // 2. Mode switch / host wake-up packet for Xbox One S / Elite Series 2
    uint8_t s_init[] = { 0x05, 0x20, s_gip_init_seq++, 0x0F, 0x06 };
    send_init_packet(s_init, sizeof(s_init));
    daemon_sys_usleep(10000);

    // 3. Hori & Titanfall 2 identify/stick-ack sequence (PowerA / Hori / Clones)
    uint8_t powera_init[] = { 0x01, 0x20, s_gip_init_seq++, 0x09, 0x00, 0x04, 0x20, 0x3a, 0x00, 0x00, 0x00, 0x80, 0x00 };
    send_init_packet(powera_init, sizeof(powera_init));
    daemon_sys_usleep(10000);

    // 4. PDP / Victrix / Turtle Beach handshake packet 1 (LED on / handshake)
    uint8_t pdp_init1[] = { 0x0a, 0x20, s_gip_init_seq++, 0x03, 0x00, 0x01, 0x14 };
    send_init_packet(pdp_init1, sizeof(pdp_init1));
    daemon_sys_usleep(10000);

    // 5. PDP / Victrix / Turtle Beach handshake packet 2 (Input mode unlock)
    uint8_t pdp_init2[] = { 0x06, 0x20, s_gip_init_seq++, 0x02, 0x01, 0x00 };
    send_init_packet(pdp_init2, sizeof(pdp_init2));
    daemon_sys_usleep(10000);

    // 6. PowerA force feedback subsystem activation ("rumble kick" + stop per Linux xpad.c)
    uint8_t powera_rumble_start[] = {
        0x09, 0x00, s_gip_init_seq++, 0x09, 0x00, 0x0F, 0x00, 0x00, 0x1D, 0x1D, 0xFF, 0x00, 0x00
    };
    send_init_packet(powera_rumble_start, sizeof(powera_rumble_start));
    daemon_sys_usleep(10000);

    uint8_t powera_rumble_stop[] = {
        0x09, 0x00, s_gip_init_seq++, 0x09, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    send_init_packet(powera_rumble_stop, sizeof(powera_rumble_stop));
    daemon_sys_usleep(10000);
}

static void init_xbox360_controller(libusb_device_handle* h, uint8_t interface_num, uint8_t ep_out, int is_wireless) {
    if (h && p_sceUsbdClearHalt && g_usb_state.endpoint_in) {
        p_sceUsbdClearHalt(h, g_usb_state.endpoint_in);
        daemon_sys_usleep(5000);
    }

    daemon_log(LOG_LEVEL_INFO, "Sending Xbox 360 clone wake-up handshake (Shanwan/xpad, IF=%d)...", interface_num);
    uint8_t dummy[20];
    memset(dummy, 0, sizeof(dummy));
    if (p_sceUsbdControlTransfer && h) {
        // Linux xpad: bmRequestType=0xC1 (Vendor Interface IN), bRequest=0x01, wValue=0x0100, wIndex=interface_num, len=20
        int ret = p_sceUsbdControlTransfer(h, 0xC1, 0x01, 0x0100, interface_num, dummy, sizeof(dummy), 100);
        daemon_log(LOG_LEVEL_INFO, "Xbox 360 clone control handshake sent (ret=0x%08X)", ret);
    }

    if (ep_out && p_sceUsbdInterruptTransfer && h) {
        int transferred = 0;
        int ir = 0;
        if (is_wireless) {
            // Linux xpad: XTYPE_XBOX360W sends a 12-byte LED command via RF header:
            // byte[0]=0x00, byte[1]=0x00, byte[2]=0x08, byte[3]=0x40 + (cmd % 16), byte[4..11]=0x00
            // Command 0x02: LED 1 / top-left blink, then steady on (Player 1)
            uint8_t led_pkt[12] = { 0x00, 0x00, 0x08, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            ir = p_sceUsbdInterruptTransfer(h, ep_out, led_pkt, sizeof(led_pkt), &transferred, 100);
        } else {
            uint8_t led_pkt[3] = { 0x01, 0x03, 0x02 }; // LED 1 ON (Player 1)
            ir = p_sceUsbdInterruptTransfer(h, ep_out, led_pkt, sizeof(led_pkt), &transferred, 100);
        }
        daemon_log(LOG_LEVEL_INFO, "Xbox 360 LED 1 command sent (ret=0x%08X, wireless=%d)", ir, is_wireless);
        if (ir != 0 && p_sceUsbdClearHalt) {
            daemon_log(LOG_LEVEL_WARN, "Xbox 360 LED command rejected (0x%08X), executing ClearHalt on ep_out=0x%02X", ir, ep_out);
            p_sceUsbdClearHalt(h, ep_out);
            daemon_sys_usleep(5000);
        }
        if (ir == 0x802400FF || ir == 0x80240009) {
            // Wireless dongles (e.g. GuliKit KK3 Max 2.4GHz adapter) do not have LEDs and return 0x802400FF (Pipe Stall).
            // Permanently disable EP_OUT commands to avoid stalling the dongle's USB engine or flooding retries.
            // Note: Transient timeouts (0x80240007) are ignored to preserve rumble for wired controllers that answer slowly.
            s_x360_ep_out_stalls = 5;
        }
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

static bool is_official_nintendo_switch_pro(libusb_device_handle* h, const struct libusb_device_descriptor* desc) {
    if (!h || !p_sceUsbdControlTransfer || !desc) return false;
    if (desc->idVendor != SWITCH_PRO_VID || desc->idProduct != SWITCH_PRO_PID) return false;

    // 1. Query Manufacturer String Descriptor (index desc->iManufacturer)
    if (desc->iManufacturer != 0) {
        uint8_t str_buf[64];
        memset(str_buf, 0, sizeof(str_buf));
        int ret = p_sceUsbdControlTransfer(h, 0x80, 0x06, (0x03 << 8) | desc->iManufacturer, 0x0409, str_buf, sizeof(str_buf), 100);
        if (ret < 0) {
            ret = p_sceUsbdControlTransfer(h, 0x80, 0x06, (0x03 << 8) | desc->iManufacturer, 0x0000, str_buf, sizeof(str_buf), 100);
        }

        if (ret >= 18 && str_buf[0] >= 18 && str_buf[1] == 0x03) {
            // Check for "Nintendo" in UTF-16LE: 'N', 0, 'i', 0, 'n', 0, 't', 0, 'e', 0, 'n', 0, 'd', 0, 'o', 0
            static const uint8_t nintendo_sig[16] = {
                'N', 0x00, 'i', 0x00, 'n', 0x00, 't', 0x00,
                'e', 0x00, 'n', 0x00, 'd', 0x00, 'o', 0x00
            };
            if (memcmp(&str_buf[2], nintendo_sig, sizeof(nintendo_sig)) == 0) {
                daemon_log(LOG_LEVEL_INFO, "Device verified as Official Nintendo Switch Pro Controller via USB manufacturer descriptor");
                return true;
            }
        }
    }

    // 2. Query Product String Descriptor (index desc->iProduct)
    if (desc->iProduct != 0) {
        uint8_t str_buf[64];
        memset(str_buf, 0, sizeof(str_buf));
        int ret = p_sceUsbdControlTransfer(h, 0x80, 0x06, (0x03 << 8) | desc->iProduct, 0x0409, str_buf, sizeof(str_buf), 100);
        if (ret < 0) {
            ret = p_sceUsbdControlTransfer(h, 0x80, 0x06, (0x03 << 8) | desc->iProduct, 0x0000, str_buf, sizeof(str_buf), 100);
        }

        if (ret >= 30 && str_buf[0] >= 30 && str_buf[1] == 0x03) {
            // Check for "Pro Controller" in UTF-16LE
            static const uint8_t pro_ctrl_sig[28] = {
                'P', 0x00, 'r', 0x00, 'o', 0x00, ' ', 0x00,
                'C', 0x00, 'o', 0x00, 'n', 0x00, 't', 0x00,
                'r', 0x00, 'o', 0x00, 'l', 0x00, 'l', 0x00,
                'e', 0x00, 'r', 0x00
            };
            if (memcmp(&str_buf[2], pro_ctrl_sig, sizeof(pro_ctrl_sig)) == 0) {
                daemon_log(LOG_LEVEL_INFO, "Device verified as Official Nintendo Pro Controller via USB product descriptor");
                return true;
            }
        }
    }

    daemon_log(LOG_LEVEL_INFO, "Device is a Switch Pro Clone (non-Nintendo descriptor) - passive mode enforced");
    return false;
}

static void init_switch_pro_controller(libusb_device_handle* h, const struct libusb_device_descriptor* desc) {
    daemon_log(LOG_LEVEL_INFO, "Starting Switch Pro / Clone wake-up sequence...");

    // 1. Distinguish official Nintendo from third-party clones:
    // Third-party Switch Pro clones (like Trust GXT Muta, Nyko, PowerA, Shanwan)
    // start streaming standard HID input reports immediately upon connection without needing any 0x80 handshake.
    // Transmitting 0x80 handshake packets on EP_OUT crashes clone microcontrollers (0x80240004 / disconnect loop).
    // ONLY official Nintendo Switch Pro controllers (iManufacturer = "Nintendo Co., Ltd.")
    // require the 0x80 handshake to lock the controller into USB mode (0x80 0x04) and prevent fallback to Bluetooth.
    bool is_official = is_official_nintendo_switch_pro(h, desc);
    if (!is_official) {
        daemon_log(LOG_LEVEL_INFO, "Switch Pro Clone detected - skipping 0x80 wake-up handshake (passive mode)");
        return;
    }

    // Drain any leftover input in the USB FIFO first
    uint8_t probe_rx[64];
    int probe_len = 0;
    for (int i = 0; i < 3; i++) {
        probe_len = 0;
        int dr = p_sceUsbdInterruptTransfer(g_dev_handle, g_usb_state.endpoint_in, probe_rx, sizeof(probe_rx), &probe_len, 10);
        if (dr != 0 || probe_len == 0) break;
    }

    // If device has no OUT endpoint, it is an input-only clone: do NOT attempt OUT transfers
    if (g_usb_state.endpoint_out == 0) {
        daemon_log(LOG_LEVEL_INFO, "Switch Pro / Clone has no OUT endpoint — skipping 0x80 wake-up handshake (passive mode)");
        return;
    }

    // 2. Official Switch Pro USB Handshake (0x80 commands):
    // The 0x80 command packet is strictly 2 BYTES (u8 buf[2] = { 0x80, cmd }),
    // matching Linux kernel hid-nintendo (joycon_send_usb).
    uint8_t cmd_pkt[2];

    cmd_pkt[0] = 0x80; cmd_pkt[1] = 0x02; // Handshake
    int r = switch_pro_send_and_wait(cmd_pkt, sizeof(cmd_pkt), 0x02, 150);
    if (r != 0) {
        daemon_log(LOG_LEVEL_INFO, "Switch Pro 0x80 handshake ret=0x%08X (clone/unsupported) — preserving passive mode", r);
        return;
    }
    daemon_sys_usleep(5000);

    cmd_pkt[0] = 0x80; cmd_pkt[1] = 0x03; // HighSpeed baudrate
    switch_pro_send_and_wait(cmd_pkt, sizeof(cmd_pkt), 0x03, 150);
    daemon_sys_usleep(5000);

    cmd_pkt[0] = 0x80; cmd_pkt[1] = 0x02; // Handshake repeat
    switch_pro_send_and_wait(cmd_pkt, sizeof(cmd_pkt), 0x02, 150);
    daemon_sys_usleep(5000);

    cmd_pkt[0] = 0x80; cmd_pkt[1] = 0x04; // Force USB
    switch_pro_send_and_wait(cmd_pkt, sizeof(cmd_pkt), 0x04, 150);
    daemon_sys_usleep(5000);

    uint8_t pkt[64];
    // Standard Switch Subcommands (AnyPad / Linux hid-nintendo model): Works on ALL official and clone controllers!
    // Step 1: Subcommand 0x03 (Set Input Report Mode to 0x30 Full Mode)
    for (int attempt = 1; attempt <= 3; attempt++) {
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x01; // Output report ID
        pkt[1] = attempt; // Packet counter
        pkt[2] = 0x00; pkt[3] = 0x01; pkt[4] = 0x40; pkt[5] = 0x40; // Neutral rumble L
        pkt[6] = 0x00; pkt[7] = 0x01; pkt[8] = 0x40; pkt[9] = 0x40; // Neutral rumble R
        pkt[10] = 0x03; // Subcommand: Set Input Mode
        pkt[11] = 0x30; // 0x30 = Standard full report mode
        int r = switch_pro_send_and_wait(pkt, sizeof(pkt), 0, 100);
        daemon_log(LOG_LEVEL_INFO, "Switch Pro SetInputMode 0x30 attempt %d ret=%d", attempt, r);
        if (r != 0 && p_sceUsbdControlTransfer && g_dev_handle) {
            p_sceUsbdControlTransfer(g_dev_handle, 0x21, 0x09, 0x0201, g_claimed_interface, pkt, sizeof(pkt), 50);
        }
        if (r == 0) break;
        daemon_sys_usleep(10000);
    }

    // Step 2: Subcommand 0x40 (Enable IMU / Motion Sensors)
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x01;
    pkt[1] = 0x04;
    pkt[2] = 0x00; pkt[3] = 0x01; pkt[4] = 0x40; pkt[5] = 0x40;
    pkt[6] = 0x00; pkt[7] = 0x01; pkt[8] = 0x40; pkt[9] = 0x40;
    pkt[10] = 0x40; // Subcommand: Enable IMU
    pkt[11] = 0x01;
    switch_pro_send_and_wait(pkt, sizeof(pkt), 0, 50);

    // Step 3: Subcommand 0x48 (Enable Vibration)
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x01;
    pkt[1] = 0x05;
    pkt[2] = 0x00; pkt[3] = 0x01; pkt[4] = 0x40; pkt[5] = 0x40;
    pkt[6] = 0x00; pkt[7] = 0x01; pkt[8] = 0x40; pkt[9] = 0x40;
    pkt[10] = 0x48; // Subcommand: Enable Vibration
    pkt[11] = 0x01;
    switch_pro_send_and_wait(pkt, sizeof(pkt), 0, 50);

    // Step 4: Subcommand 0x30 (Player 1 LED)
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x01;
    pkt[1] = 0x06;
    pkt[2] = 0x00; pkt[3] = 0x01; pkt[4] = 0x40; pkt[5] = 0x40;
    pkt[6] = 0x00; pkt[7] = 0x01; pkt[8] = 0x40; pkt[9] = 0x40;
    pkt[10] = 0x30; // Subcommand: Set Player Lights
    pkt[11] = 0x01; // LED 1 ON
    switch_pro_send_and_wait(pkt, sizeof(pkt), 0, 50);

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
        case CONTROLLER_DS4:      return "DualShock 4";
        default:                  return "Unknown";
    }
}

static const char* controller_type_to_id(ControllerType type) {
    switch (type) {
        case CONTROLLER_XBOX360:   return "xbox360";
        case CONTROLLER_XBOXONE:   return "xboxone";
        case CONTROLLER_SWITCH:    return "switch";
        case CONTROLLER_SWITCH_PRO:return "switch_pro";
        case CONTROLLER_DS3:       return "ds3";
        case CONTROLLER_DUALSENSE: return "dualsense";
        case CONTROLLER_STADIA:    return "stadia";
        case CONTROLLER_GENERIC_HID:return "generic_hid";
        case CONTROLLER_DS4:       return "ds4";
        default:                   return "none";
    }
}

static int usb_get_string_ascii(libusb_device_handle* h, uint8_t idx, char* buf, size_t max_len) {
    if (!h || idx == 0 || !buf || max_len == 0 || !p_sceUsbdControlTransfer) return -1;
    uint8_t desc[256];
    memset(desc, 0, sizeof(desc));
    int ret = p_sceUsbdControlTransfer(h, 0x80, 0x06, (0x03 << 8) | idx, 0x0409, desc, sizeof(desc), 100);
    if (ret < 0) {
        ret = p_sceUsbdControlTransfer(h, 0x80, 0x06, (0x03 << 8) | idx, 0x0000, desc, sizeof(desc), 100);
    }
    if (ret >= 2 && desc[1] == 0x03) {
        int len = desc[0];
        if (len > ret) len = ret;
        size_t out_idx = 0;
        for (int i = 2; i < len && out_idx < max_len - 1; i += 2) {
            buf[out_idx++] = (char)desc[i];
        }
        buf[out_idx] = '\0';
        return (int)out_idx;
    }
    return -1;
}

int usbd_is_sony_device(libusb_device* dev, const struct libusb_device_descriptor* desc) {
    if (!dev || !desc || desc->idVendor != SONY_VID) return 0;

    libusb_device_handle* h = NULL;
    if (p_sceUsbdOpen && p_sceUsbdOpen(dev, &h) == 0 && h != NULL) {
        char mfg[128] = {0};
        int r = (desc->iManufacturer != 0) ? usb_get_string_ascii(h, desc->iManufacturer, mfg, sizeof(mfg)) : -1;
        if (p_sceUsbdClose) p_sceUsbdClose(h);

        if (r > 0) {
            daemon_log(LOG_LEVEL_INFO, "Device 0x%04X:0x%04X iManufacturer='%s'", desc->idVendor, desc->idProduct, mfg);
            for (int i = 0; mfg[i]; i++) {
                if ((mfg[i] == 'S' || mfg[i] == 's') &&
                    (mfg[i+1] == 'O' || mfg[i+1] == 'o') &&
                    (mfg[i+2] == 'N' || mfg[i+2] == 'n') &&
                    (mfg[i+3] == 'Y' || mfg[i+3] == 'y')) {
                    return 1;
                }
            }
            return 0; // Read successfully, but does not contain "Sony" (e.g. GameSir)
        }
    }
    // If opening or descriptor read failed, default to treating Sony VID as genuine Sony to be safe
    return 1;
}

#define USB_MAX_SNAPSHOT_DEVICES 16

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint8_t  bDeviceClass;
} UsbDeviceSnapshot;

static UsbDeviceSnapshot s_last_bus_devices[USB_MAX_SNAPSHOT_DEVICES];
static int s_last_bus_device_count = -1; // -1 = first scan pending

static bool probe_microsoft_os_descriptor(libusb_device_handle* h) {
    if (!p_sceUsbdControlTransfer || !h) return false;

    // Microsoft OS 1.0 String Descriptor probe
    // Standard Device Request:
    // bmRequestType: 0x80 (DEVICE_TO_HOST | STANDARD | DEVICE)
    // bRequest:      0x06 (GET_DESCRIPTOR)
    // wValue:        0x03EE (Descriptor Type 0x03 = String, Index 0xEE)
    // wIndex:        0x0000 (neutral language / OS string)
    // wLength:       18 (0x12)
    uint8_t ms_desc[18];
    memset(ms_desc, 0, sizeof(ms_desc));

    int ret = p_sceUsbdControlTransfer(h, 0x80, 0x06, 0x03EE, 0x0000, ms_desc, sizeof(ms_desc), 100);
    if (ret < 0) {
        // Fallback: try with wIndex = 0x0409 (English US) as some firmware requires language ID
        ret = p_sceUsbdControlTransfer(h, 0x80, 0x06, 0x03EE, 0x0409, ms_desc, sizeof(ms_desc), 100);
    }

    if (ret >= 14 && ms_desc[0] >= 14 && ms_desc[1] == 0x03) {
        // Verify "MSFT100" in UTF-16LE: 'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0
        static const uint8_t msft100_sig[14] = {
            'M', 0x00, 'S', 0x00, 'F', 0x00, 'T', 0x00,
            '1', 0x00, '0', 0x00, '0', 0x00
        };
        if (memcmp(&ms_desc[2], msft100_sig, 14) == 0) {
            uint8_t vendor_code = (ret >= 17) ? ms_desc[16] : 0;
            daemon_log(LOG_LEVEL_INFO, "Device acknowledged Microsoft OS Descriptor (MSFT100, vendor_code=0x%02X)", vendor_code);

            // In Microsoft's protocol, if MSFT100 is returned, the host issues the vendor-specific query:
            // bmRequestType = 0xC0 (DEVICE_TO_HOST | VENDOR | DEVICE)
            // bRequest = vendor_code
            // wValue = 0x0000
            // wIndex = 0x0004 (Extended Compat ID feature descriptor)
            // Multi-mode controllers (Trust GXT Muta, etc.) use this secondary request to trigger personality switch to XInput.
            if (vendor_code != 0) {
                uint8_t compat_id[40];
                memset(compat_id, 0, sizeof(compat_id));
                int vc_ret = p_sceUsbdControlTransfer(h, 0xC0, vendor_code, 0x0000, 0x0004, compat_id, sizeof(compat_id), 100);
                daemon_log(LOG_LEVEL_INFO, "Microsoft Extended Compat ID query (code=0x%02X) returned %d", vendor_code, vc_ret);
            }
            return true;
        }
    }
    return false;
}

static int scan_and_open(void) {
    if (g_dev_handle != NULL) return 0;
    if (!p_sceUsbdGetDeviceList || !p_sceUsbdFreeDeviceList) return -1;

    libusb_device** list = NULL;
    ssize_t count = p_sceUsbdGetDeviceList(&list);
    if (count <= 0 || !list) {
        if (s_last_bus_device_count != 0) {
            daemon_log(LOG_LEVEL_INFO, "USB Bus: 0 devices detected (bus empty)");
            s_last_bus_device_count = 0;
            memset(s_last_bus_devices, 0, sizeof(s_last_bus_devices));
        }
        if (s_failed_cooldown_vid != 0) {
            s_failed_cooldown_vid = 0;
            s_failed_cooldown_pid = 0;
            s_failed_cooldown_until_us = 0;
        }
        if (list) p_sceUsbdFreeDeviceList(list);
        return -1;
    }

    // Capture current bus topology snapshot
    UsbDeviceSnapshot cur_devices[USB_MAX_SNAPSHOT_DEVICES];
    int cur_count = (count > USB_MAX_SNAPSHOT_DEVICES) ? USB_MAX_SNAPSHOT_DEVICES : (int)count;
    memset(cur_devices, 0, sizeof(cur_devices));

    for (int i = 0; i < cur_count; i++) {
        struct libusb_device_descriptor d;
        if (p_sceUsbdGetDeviceDescriptor(list[i], &d) == 0) {
            cur_devices[i].vid = d.idVendor;
            cur_devices[i].pid = d.idProduct;
            cur_devices[i].bDeviceClass = d.bDeviceClass;
        }
    }

    // Detect topology changes (device connected, disconnected, or changed)
    bool bus_changed = (cur_count != s_last_bus_device_count);
    if (!bus_changed) {
        for (int i = 0; i < cur_count; i++) {
            if (cur_devices[i].vid != s_last_bus_devices[i].vid ||
                cur_devices[i].pid != s_last_bus_devices[i].pid ||
                cur_devices[i].bDeviceClass != s_last_bus_devices[i].bDeviceClass) {
                bus_changed = true;
                break;
            }
        }
    }

    if (bus_changed) {
        daemon_log(LOG_LEVEL_INFO, "USB Bus topology change detected (%d devices on bus):", (int)count);
        for (int i = 0; i < cur_count; i++) {
            daemon_log(LOG_LEVEL_INFO, "  USB Device [%d]: VID:0x%04X PID:0x%04X (Class:0x%02X)",
                       i, cur_devices[i].vid, cur_devices[i].pid, cur_devices[i].bDeviceClass);
        }
        memcpy(s_last_bus_devices, cur_devices, sizeof(cur_devices));
        s_last_bus_device_count = cur_count;
    }

    int found = 0;
    int cooldown_seen = 0;

    for (ssize_t i = 0; i < count && !found; i++) {
        libusb_device* dev = list[i];
        struct libusb_device_descriptor desc;
        if (p_sceUsbdGetDeviceDescriptor(dev, &desc) != 0) continue;

        // Skip device if currently under failure cooldown to prevent notification loops
        if (s_failed_cooldown_vid != 0 && desc.idVendor == s_failed_cooldown_vid && desc.idProduct == s_failed_cooldown_pid) {
            cooldown_seen = 1;
            uint64_t now_us = daemon_sys_time_us();
            if (now_us < s_failed_cooldown_until_us) {
                continue;
            } else {
                s_failed_cooldown_vid = 0;
                s_failed_cooldown_pid = 0;
                s_failed_cooldown_until_us = 0;
            }
        }

        // Skip USB Hubs (Class 0x09) and Mass Storage (Class 0x08)
        if (desc.bDeviceClass == 0x09 || desc.bDeviceClass == 0x08) continue;

        DetectedController det;
        memset(&det, 0, sizeof(det));
        if (controller_identify(dev, &desc, &det)) {
            if (det.type == CONTROLLER_DUALSENSE) {
                static bool s_ds_usb_logged = false;
                if (!s_ds_usb_logged) {
                    daemon_log(LOG_LEVEL_INFO, "DualSense detected on USB: bypassing interface claim (charging mode only)");
                    s_ds_usb_logged = true;
                }
                continue; // Leave USB port free for charging, do not claim or stall
            }

            libusb_device_handle* h = NULL;
            if (p_sceUsbdOpen(dev, &h) == 0 && h != NULL) {

                // Probe Microsoft OS String Descriptor (Index 0xEE "MSFT100"):
                // Replicates Windows USB host enumeration behavior. Many multi-mode controllers
                // (e.g. Trust GXT Muta, 8BitDo, Machenike) default to Switch Pro mode on non-Windows hosts,
                // but switch personality to native Xbox 360 (045E:028E) when probed with MS OS Descriptors!
                // Skip MS OS probe for legacy PC DirectInput controllers (Logitech 0x046D, ShanWan 0x2563, Betop 0x20BC)
                // whose firmware hangs or pipe-stalls (LG_NOGET hardware quirk) upon receiving unsupported control descriptors.
                bool is_legacy_hid = (desc.idVendor == 0x046D || desc.idVendor == 0x2563 || desc.idVendor == 0x20BC);
                if ((det.type == CONTROLLER_SWITCH_PRO || det.type == CONTROLLER_SWITCH || det.type == CONTROLLER_GENERIC_HID) && !is_legacy_hid) {
                    if (probe_microsoft_os_descriptor(h)) {
                        daemon_sys_usleep(20000);
                        if (p_sceUsbdCheckConnected && p_sceUsbdCheckConnected(h) != 0) {
                            daemon_log(LOG_LEVEL_INFO, "Device detached following MS OS Descriptor probe (re-enumerating personality)");
                            p_sceUsbdClose(h);
                            continue;
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

                if (p_sceUsbdDetachKernelDriver && (det.type == CONTROLLER_GENERIC_HID || det.type == CONTROLLER_DS4 || det.type == CONTROLLER_DS3)) {
                    p_sceUsbdDetachKernelDriver(h, det.interface_num);
                }

                if (p_sceUsbdClaimInterface(h, det.interface_num) == 0) {
                    if (det.alt_setting > 0 && p_sceUsbdSetInterfaceAltSetting) {
                        int as_ret = p_sceUsbdSetInterfaceAltSetting(h, det.interface_num, det.alt_setting);
                        daemon_log(LOG_LEVEL_INFO, "SetInterfaceAltSetting(IF=%d, alt=%d) = 0x%08X", det.interface_num, det.alt_setting, as_ret);
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
                    g_usb_state.bInterval = det.bInterval;
                    found = 1;

                    if (det.type == CONTROLLER_GENERIC_HID) {
                        if (!generic_hid_setup(h, desc.idVendor, desc.idProduct, det.interface_num, p_sceUsbdControlTransfer)) {
                            daemon_log(LOG_LEVEL_INFO, "Generic HID (0x%04X:0x%04X) rejected: not an active gamepad — releasing interface",
                                       desc.idVendor, desc.idProduct);
                            if (s_claimed_kbd_interface >= 0 && p_sceUsbdReleaseInterface) {
                                p_sceUsbdReleaseInterface(h, s_claimed_kbd_interface);
                                s_claimed_kbd_interface = -1;
                            }
                            if (p_sceUsbdReleaseInterface) {
                                p_sceUsbdReleaseInterface(h, det.interface_num);
                            }
                            p_sceUsbdClose(h);
                            g_dev_handle = NULL;
                            g_usb_state.connected = false;
                            found = 0;
                            break;
                        }
                    }

                    write_status_file(true, det.type, desc.idVendor, desc.idProduct,
                                      controller_type_to_str(det.type),
                                      (det.type == CONTROLLER_GENERIC_HID) ? generic_hid_get_nbuttons() : 16);

                    daemon_log(LOG_LEVEL_INFO, "Controller claimed: %s (VID:0x%04X PID:0x%04X, EP_IN:0x%02X, EP_OUT:0x%02X, interval=%dms, wireless=%d)",
                               controller_type_to_str(det.type), desc.idVendor, desc.idProduct, det.ep_in, det.ep_out, det.bInterval, det.is_wireless);

                    memset(&s_last_ds4, 0, sizeof(s_last_ds4));
                    s_last_ds4.leftStick.x = 128;
                    s_last_ds4.leftStick.y = 128;
                    s_last_ds4.rightStick.x = 128;
                    s_last_ds4.rightStick.y = 128;
                    s_last_ds4.connected = 1;
                    s_last_ds4.count = 1;

                    if (det.type == CONTROLLER_XBOX360) {
                        init_xbox360_controller(h, det.interface_num, det.ep_out, det.is_wireless);
                    } else if (det.type == CONTROLLER_XBOXONE) {
                        init_gip_controller();
                    } else if (det.type == CONTROLLER_DS3) {
                        if (h && p_sceUsbdClearHalt && g_usb_state.endpoint_in) {
                            p_sceUsbdClearHalt(h, g_usb_state.endpoint_in);
                            daemon_sys_usleep(5000);
                        }
                        if (p_sceUsbdControlTransfer) {
                            // 1. Linux hid-sony / Sixaxis operational mode activation: GET_REPORT Feature 0xF2
                            uint8_t op_buf[17];
                            memset(op_buf, 0, sizeof(op_buf));
                            int r_f2 = p_sceUsbdControlTransfer(h, 0xA1, 0x01, 0x03F2, det.interface_num, op_buf, sizeof(op_buf), 100);
                            daemon_log(LOG_LEVEL_INFO, "DualShock 3 operational mode query 0xF2 sent (ret=%d)", r_f2);

                            // 2. SET_REPORT Feature 0xF4 magic packet (wake-up command)
                            uint8_t magic[5] = { 0xF4, 0x42, 0x03, 0x00, 0x00 };
                            int cret = p_sceUsbdControlTransfer(h, DS3_MAGIC_REQ_TYPE, DS3_MAGIC_REQUEST, DS3_MAGIC_VALUE, det.interface_num, magic, sizeof(magic), 1000);
                            daemon_log(LOG_LEVEL_INFO, "DualShock 3 operational handshake sent (ret=%d)", cret);

                            // 3. Shanwan / Gasia DS3 clone wake-up: query Feature Report 0xF5 via GET_REPORT
                            uint8_t dummy[64];
                            p_sceUsbdControlTransfer(h, 0xA1, 0x01, 0x03F5, det.interface_num, dummy, 8, 100);
                        }
                    } else if (det.type == CONTROLLER_SWITCH_PRO) {
                        init_switch_pro_controller(h, &desc);
                    }

                    // Check if controller is still connected and alive after wake-up/handshake
                    if (p_sceUsbdCheckConnected && p_sceUsbdCheckConnected(h) != 0) {
                        daemon_log(LOG_LEVEL_WARN, "Controller detached during initialization handshake — aborting vpad allocation");
                        p_sceUsbdClose(h);
                        g_dev_handle = NULL;
                        g_usb_state.connected = false;
                        found = 0;
                        break;
                    }

                    // Initialize Virtual DualShock 4 (Type 1) ONLY once controller is confirmed ready!
                    if (s_usb_vpad_slot < 0) {
                        vpad_slot_open(VPAD_SOURCE_USB, &s_usb_vpad_slot);
                    }
                    if (s_usb_vpad_slot >= 0) {
                        vpad_send_data_slot(s_usb_vpad_slot, &s_last_ds4);
                    }

                    int slot = vpad_get_assigned_slot_num(s_usb_vpad_slot);
                    if (slot == 2) {
                        daemon_notify("OneFamily: %s (Player 2)", controller_type_to_str(det.type));
                    } else {
                        daemon_notify("OneFamily: %s Connected", controller_type_to_str(det.type));
                    }
                } else {
                    p_sceUsbdClose(h);
                }
            }
        }
    }

    if (s_failed_cooldown_vid != 0 && !cooldown_seen) {
        s_failed_cooldown_vid = 0;
        s_failed_cooldown_pid = 0;
        s_failed_cooldown_until_us = 0;
    }

    p_sceUsbdFreeDeviceList(list);
    return found ? 0 : -1;
}

static void encode_switch_rumble(uint8_t* out, uint8_t motor_large, uint8_t motor_small) {
    if (motor_large == 0 && motor_small == 0) {
        out[0] = 0x00;
        out[1] = 0x01;
        out[2] = 0x40;
        out[3] = 0x40;
        return;
    }
    // High-frequency (small motor): 320 Hz (hf = 0x0074)
    // Low-frequency (large motor): 160 Hz (lf = 0x3D)
    uint16_t hf = 0x0074;
    uint8_t lf = 0x3D;

    // Amplitude encoding:
    // High amp: 0x00 to 0xC8 (scale motor_small: 0..255 -> 0..0xC8)
    uint8_t hf_amp = (uint8_t)(((uint32_t)motor_small * 0xC8) / 255);
    // Low amp: 0x00 to 0x7F (scale motor_large: 0..255 -> 0..0x7F)
    uint16_t lf_amp = (uint16_t)(((uint32_t)motor_large * 0x7F) / 255);

    out[0] = (uint8_t)(hf & 0xFF);
    out[1] = (uint8_t)(hf_amp | ((hf >> 8) & 0x01));
    out[2] = (uint8_t)(lf | ((lf_amp >> 8) & 0x80));
    out[3] = (uint8_t)(lf_amp & 0xFF);
}

void usb_driver_send_rumble(uint8_t large_motor, uint8_t small_motor) {
    if (!g_dev_handle || !g_usb_state.connected || !p_sceUsbdInterruptTransfer) {
        return;
    }
    if (!g_daemon_config.enable_rumble) {
        return;
    }

    // Switch input-only pads (PowerA, PDP, etc.) have no rumble hardware or endpoint OUT
    if (g_usb_state.type == CONTROLLER_SWITCH) {
        return;
    }

    uint8_t ep = g_usb_state.endpoint_out;
    if (ep == 0) {
        ep = (g_usb_state.type == CONTROLLER_XBOXONE) ? 0x02 : 0x01;
    }

    // Do not spam synchronous disk I/O on dynamic rumble steps to avoid thread latency
    int transferred = 0;
    if (g_usb_state.type == CONTROLLER_XBOX360) {
        if (g_usb_state.is_wireless) {
            // Wireless Xbox 360 Receiver Rumble (12 bytes)
            uint8_t pkt12[12] = {
                0x00, 0x01, 0x0F, 0xC0,
                0x00, large_motor, small_motor, 0x00,
                0x00, 0x00, 0x00, 0x00
            };
            p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt12, sizeof(pkt12), &transferred, 2);
        } else {
            if (s_x360_ep_out_stalls >= 5) {
                // If endpoint OUT is permanently stalled on this controller, avoid flooding
                return;
            }
            uint8_t pkt[8] = { 0x00, 0x08, 0x00, large_motor, small_motor, 0x00, 0x00, 0x00 };
            int ret = p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 2);
            if (ret != 0) {
                if (ret == 0x802400FF || ret == 0x80240009) {
                    s_x360_ep_out_stalls += 2;
                }
                uint8_t pkt4[4] = { 0x00, 0x04, large_motor, small_motor };
                int ret4 = p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt4, sizeof(pkt4), &transferred, 2);
                if (ret4 != 0) {
                    s_x360_ep_out_stalls += 3;
                } else {
                    s_x360_ep_out_stalls = 0;
                }
            } else {
                s_x360_ep_out_stalls = 0;
            }
        }
    } else if (g_usb_state.type == CONTROLLER_XBOXONE) {
        if (s_xone_ep_out_stalls >= 5) {
            return;
        }
        // Standard Linux xpad.c Xbox One / GIP rumble packet (12 bytes)
        // Motor mask 0x03 targets left and right grip motors only, avoiding trigger motor stalls on 3rd-party/PowerA pads
        uint8_t pkt[12] = {
            0x09, 0x08, 0x00, 0x08,
            0x00, 0x03, // Mode 0x00, Motor select: 0x01 (Left) | 0x02 (Right) = 0x03
            0x00, 0x00, // Left/Right trigger motors: 0x00 (not present on 3rd-party)
            large_motor, // Left heavy grip motor (0..255)
            small_motor, // Right light grip motor (0..255)
            (large_motor > 0 || small_motor > 0) ? 0x80 : 0x00, // Duration / pulse length (0x80 continuous, 0x00 stop)
            0x00  // Period of pulse
        };
        int ret = p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 2);
        if (ret != 0) {
            if (ret == 0x802400FF || ret == 0x80240009) {
                s_xone_ep_out_stalls += 2;
                if (p_sceUsbdClearHalt) {
                    p_sceUsbdClearHalt(g_dev_handle, ep);
                }
            } else {
                s_xone_ep_out_stalls++;
            }
        } else {
            s_xone_ep_out_stalls = 0;
        }
    } else if (g_usb_state.type == CONTROLLER_DS3) {
        uint8_t pkt[8] = { 0x00, 0xFF, 0x00, (small_motor > 0) ? 1 : 0, large_motor, 0x00, 0x00, 0x00 };
        p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 2);
    } else if (g_usb_state.type == CONTROLLER_SWITCH_PRO || g_usb_state.type == CONTROLLER_SWITCH) {
        uint8_t pkt[10];
        static uint8_t s_switch_rumble_cnt = 0;
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x10; // Output report ID: Rumble only
        pkt[1] = s_switch_rumble_cnt++ & 0x0F;
        encode_switch_rumble(&pkt[2], large_motor, small_motor); // Left motor
        encode_switch_rumble(&pkt[6], large_motor, small_motor); // Right motor

        int ret = p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 2);
        if (ret != 0 && p_sceUsbdControlTransfer) {
            p_sceUsbdControlTransfer(g_dev_handle, 0x21, 0x09, 0x0210, g_claimed_interface, pkt, sizeof(pkt), 2);
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
            if (p_sceUsbdInterruptTransfer(g_dev_handle, ep, pkt, sizeof(pkt), &transferred, 2) == 0) {
                sent = 1;
            }
        }
        if (!sent && p_sceUsbdControlTransfer) {
            p_sceUsbdControlTransfer(g_dev_handle, 0x21, 0x09, 0x0205, g_claimed_interface, pkt, sizeof(pkt), 2);
        }
    } else if (g_usb_state.type == CONTROLLER_DS4) {
        DS4OutputReport pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.report_id = 0x05;
        pkt.valid_flag0 = 0x07; // Rumble + LED valid
        pkt.motor_right = small_motor;
        pkt.motor_left = large_motor;
        pkt.lightbar_blue = 255; // Default blue LED
        int sent = 0;
        if (ep != 0) {
            if (p_sceUsbdInterruptTransfer(g_dev_handle, ep, (uint8_t*)&pkt, sizeof(pkt), &transferred, 2) == 0) {
                sent = 1;
            }
        }
        if (!sent && p_sceUsbdControlTransfer) {
            p_sceUsbdControlTransfer(g_dev_handle, 0x21, 0x09, 0x0205, g_claimed_interface, (unsigned char*)&pkt, sizeof(pkt), 2);
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

static bool is_current_device_present_on_bus(void) {
    if (!p_sceUsbdGetDeviceList || !p_sceUsbdFreeDeviceList) {
        return true;
    }
    libusb_device** list = NULL;
    ssize_t count = p_sceUsbdGetDeviceList(&list);
    if (count <= 0 || !list) {
        if (list) p_sceUsbdFreeDeviceList(list);
        return false;
    }
    bool found = false;
    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        if (p_sceUsbdGetDeviceDescriptor && p_sceUsbdGetDeviceDescriptor(list[i], &desc) == 0) {
            if (desc.idVendor == g_usb_state.vid && desc.idProduct == g_usb_state.pid) {
                found = true;
                break;
            }
        } else if (list[i] == g_cur_dev) {
            found = true;
            break;
        }
    }
    p_sceUsbdFreeDeviceList(list);
    return found;
}

int usb_driver_init(void) {
    if (load_usbd() != 0) {
        return -1;
    }

    translator_init(&s_trans_cfg);
    vpad_set_rumble_callback(usb_driver_send_rumble);
    daemon_log(LOG_LEVEL_INFO, "USB Driver initialized successfully, ready for main loop polling");
    return 0;
}

static void apply_universal_shortcuts(OrbisPadData* ds4) {
    bool share_down = (ds4->buttons & DS4_BUTTON_SHARE) != 0;

    if (share_down) {
        bool combo_matched = false;

        // 1. Share + Options (Back + Start / Minus + Plus / Select + Start) -> Touchpad Click
        if (ds4->buttons & DS4_BUTTON_OPTIONS) {
            ds4->buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
            ds4->buttons |= DS4_BUTTON_TOUCHPAD;
            combo_matched = true;
        }
        // 2. Share + D-Pad Up (Back + D-Pad Up) -> Touchpad Click
        else if (ds4->buttons & DS4_BUTTON_DPAD_UP) {
            ds4->buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_UP);
            ds4->buttons |= DS4_BUTTON_TOUCHPAD;
            combo_matched = true;
        }
        // 3. Share + D-Pad Down (Back + D-Pad Down) -> PS Button (Home)
        else if (ds4->buttons & DS4_BUTTON_DPAD_DOWN) {
            ds4->buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
            ds4->buttons |= DS4_BUTTON_PS;
            combo_matched = true;
        }

        if (combo_matched) {
            s_share_combo_active = true;
        } else if (s_share_combo_active) {
            // Asynchronous release de-ghosting:
            // The secondary button was released slightly before Share.
            // Suppress Share until Share is physically released to avoid accidental screenshots / Share menu.
            ds4->buttons &= ~DS4_BUTTON_SHARE;
        }
    } else {
        // Share was fully released: reset the combo lock
        s_share_combo_active = false;
    }
}

static void handle_pad_input_and_send(const OrbisPadData* ds4) {
    OrbisPadData out_ds4 = *ds4;
    apply_universal_shortcuts(&out_ds4);

    s_last_ds4 = out_ds4;
    if (s_usb_vpad_slot >= 0) {
        vpad_send_data_slot(s_usb_vpad_slot, &out_ds4);
    } else {
        vpad_send_data(&out_ds4);
    }
}

void usb_driver_poll_step(void) {
    uint64_t now_us = daemon_sys_time_us();

    // 1. If disconnected, rescan USB bus at throttled pace (every 1.5 seconds)
    if (!g_usb_state.connected || g_dev_handle == NULL) {
        if (now_us - s_last_rescan_us >= 1500000ULL) {
            s_last_rescan_us = now_us;
            scan_and_open();
        }
        return;
    }

    // 2. Read from connected controller
    uint8_t rx[128];
    int transferred = 0;
    int read_len = sizeof(rx);
    if (g_usb_state.type == CONTROLLER_DUALSENSE || g_usb_state.type == CONTROLLER_GENERIC_HID) {
        read_len = 64; // DualSense and Generic HID endpoints have wMaxPacketSize <= 64 bytes
    }
    // Timeout: 8ms for high-polling (bInterval < 4ms), 16ms for standard 125Hz/250Hz controllers (as in v1.2.1)
    int poll_timeout_ms = (g_usb_state.bInterval > 0 && g_usb_state.bInterval < 4) ? 8 : 16;
    uint64_t xfer_start_us = daemon_sys_time_us();
    int ret = p_sceUsbdInterruptTransfer(g_dev_handle, g_usb_state.endpoint_in, rx, read_len, &transferred, poll_timeout_ms);
    uint64_t xfer_dur_us = daemon_sys_time_us() - xfer_start_us;

    // High-Polling USB FIFO Drain (Targeted exclusively for high-polling 500Hz/1000Hz controllers with bInterval < 4ms):
    // Standard 125Hz/250Hz controllers (bInterval >= 4) never accumulate backlogs in our 250 Hz loop and must NOT run this drain,
    // avoiding 1ms blocking timeouts and frame stalling.
    // For high-polling controllers (e.g. GuliKit KK3 Max 1000Hz, PowerA 1000Hz), drain pending packets to the newest real-time head.
    // NEVER bitwise-OR button masks: gamepads send absolute physical snapshots; OR-merging corrupts release packets into stuck buttons!
    bool is_high_polling = (g_usb_state.bInterval > 0 && g_usb_state.bInterval < 4);
    if (ret == 0 && is_high_polling && xfer_dur_us < 2000ULL &&
        ((g_usb_state.type == CONTROLLER_XBOX360 && transferred >= 14) ||
         (g_usb_state.type == CONTROLLER_XBOXONE && transferred >= 4))) {
        uint8_t next_rx[128];
        int next_transferred = 0;
        int drain_count = 0;
        while (drain_count < 8) {
            int drain_ret = p_sceUsbdInterruptTransfer(g_dev_handle, g_usb_state.endpoint_in, next_rx, read_len, &next_transferred, 1);
            if (drain_ret == 0 && next_transferred > 0) {
                if (g_usb_state.type == CONTROLLER_XBOX360) {
                    if (g_usb_state.is_wireless) {
                        // Wireless receiver report
                        if (next_transferred >= 24 && next_rx[1] == 0x01 && next_rx[4] == XBOX360_REPORT_INPUT) {
                            memcpy(rx, next_rx, next_transferred);
                            transferred = next_transferred;
                        } else {
                            // Non-input RF report (keepalive 0x00, status 0x0F, announce, etc.)
                            if (next_rx[0] & 0x08) {
                                s_x360_wireless_present = (next_rx[1] & 0x80) ? 1 : 0;
                            }
                            // Only overwrite rx if rx is not already holding a valid input report
                            if (!(transferred >= 24 && rx[1] == 0x01 && rx[4] == XBOX360_REPORT_INPUT)) {
                                memcpy(rx, next_rx, next_transferred);
                                transferred = next_transferred;
                            }
                        }
                    } else if (next_transferred >= 14) {
                        // Wired report: adopt newest state directly without button OR-merging
                        memcpy(rx, next_rx, next_transferred);
                        transferred = next_transferred;
                    }
                } else if (g_usb_state.type == CONTROLLER_XBOXONE) {
                    if (next_rx[0] == XBOXONE_REPORT_INPUT && next_transferred >= 6) {
                        // Adopt newest input report directly without button OR-merging
                        memcpy(rx, next_rx, next_transferred);
                        transferred = next_transferred;
                    } else if (next_rx[0] == XBOXONE_REPORT_GUIDE) {
                        // Guide report (0x07): send ACK if requested and update Guide state
                        if ((next_rx[1] & 0x10) || (next_rx[1] & 0x01)) {
                            uint8_t ack_pkt[13] = {
                                0x01, 0x20, next_rx[2], 0x09, 0x00, 0x07, 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
                            };
                            send_gip_ack(ack_pkt, sizeof(ack_pkt));
                        }
                        uint8_t gval = (next_transferred >= 5) ? next_rx[4] : next_rx[3];
                        s_xboxone_guide = (gval & 0x01) ? 1 : 0;
                        // Only overwrite rx if rx is not already holding a valid input report
                        if (!(rx[0] == XBOXONE_REPORT_INPUT && transferred >= 6)) {
                            memcpy(rx, next_rx, next_transferred);
                            transferred = next_transferred;
                        }
                    } else {
                        // General GIP packet (ACK, announce, status): send ACK if requested
                        if (next_transferred >= 3 && ((next_rx[1] & 0x10) || (next_rx[1] & 0x01))) {
                            uint8_t ack_pkt[13] = {
                                0x01, 0x20, next_rx[2], 0x09, 0x00, next_rx[0], 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
                            };
                            send_gip_ack(ack_pkt, sizeof(ack_pkt));
                        }
                        // Only overwrite rx if rx is not already holding a valid input report
                        if (!(rx[0] == XBOXONE_REPORT_INPUT && transferred >= 6)) {
                            memcpy(rx, next_rx, next_transferred);
                            transferred = next_transferred;
                        }
                    }
                }
                drain_count++;
            } else {
                break; // Hardware FIFO queue is now fully drained!
            }
        }
    }

    if (ret == 0 && transferred > 0) {
        s_consecutive_disconnects = 0;
        s_consecutive_transfer_errors = 0;
        s_consecutive_timeouts = 0;
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

        if (g_usb_state.type == CONTROLLER_SWITCH || g_usb_state.type == CONTROLLER_SWITCH_PRO) {
            s_trans_cfg.swap_ab = (g_daemon_config.switch_positional == 0);
            s_trans_cfg.swap_xy = (g_daemon_config.switch_positional == 0);
            s_trans_cfg.invert_left_y = 0;
            s_trans_cfg.invert_right_y = 0;
        } else {
            // Xbox and PlayStation controllers: never swap face buttons (A=Cross, B=Circle, X=Square, Y=Triangle)
            s_trans_cfg.swap_ab = 0;
            s_trans_cfg.swap_xy = 0;
            s_trans_cfg.invert_left_y = 1;
            s_trans_cfg.invert_right_y = 1;
        }

        if (g_usb_state.type == CONTROLLER_XBOX360 && transferred >= 14) {
            if (s_logged_x360_report < 5) {
                daemon_log(LOG_LEVEL_INFO, "Xbox 360 report [%d bytes, wireless=%d]: rx[0]=0x%02X rx[1]=0x%02X rx[2]=0x%02X rx[3]=0x%02X",
                           transferred, g_usb_state.is_wireless, rx[0], rx[1], rx[2], rx[3]);
                s_logged_x360_report++;
            }

            const Xbox360Report* x360 = NULL;
            Xbox360Report temp_report;

            if (g_usb_state.is_wireless) {
                // Linux xpad360w: byte 0 bit 3 (rx[0] & 0x08) indicates a presence change event
                if (rx[0] & 0x08) {
                    int pad_present = (rx[1] & 0x80) ? 1 : 0;
                    if (pad_present != s_x360_wireless_present) {
                        s_x360_wireless_present = pad_present;
                        daemon_log(LOG_LEVEL_INFO, "Xbox 360 Wireless pad %s (slot %d, flags=0x%02X)",
                                   pad_present ? "Connected" : "Disconnected / Idle", rx[0] & 0x03, rx[1]);
                        if (!pad_present) {
                            // Reset virtual pad to neutral state when wireless pad disconnects or sleeps
                            memset(&ds4, 0, sizeof(ds4));
                            ds4.leftStick.x = 128;
                            ds4.leftStick.y = 128;
                            ds4.rightStick.x = 128;
                            ds4.rightStick.y = 128;
                            handle_pad_input_and_send(&ds4);
                        }
                    }
                }

                // Wireless receiver report (4-byte RF/slot header prefix, payload at &rx[4])
                // Linux xpad360w: ONLY rx[1] == 0x01 indicates a valid controller input report.
                // Status, keepalive, and announce packets (rx[1] == 0x00, 0x0F, 0xF8) must NOT be parsed as input.
                if (transferred >= 24 && rx[1] == 0x01 && rx[4] == XBOX360_REPORT_INPUT) {
                    x360 = (const Xbox360Report*)(&rx[4]);
                }
            } else if (!g_usb_state.is_wireless && rx[0] == XBOX360_REPORT_INPUT) {
                // Wired Xbox 360 report (Linux xpad style: accepts any report starting with type 0x00)
                if (transferred < (int)sizeof(Xbox360Report)) {
                    memset(&temp_report, 0, sizeof(temp_report));
                    memcpy(&temp_report, rx, transferred);
                    x360 = &temp_report;
                } else {
                    x360 = (const Xbox360Report*)rx;
                }
            } else if (transferred >= 24 && rx[1] == 0x01 && rx[4] == XBOX360_REPORT_INPUT) {
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
                handle_pad_input_and_send(&ds4);
            }
        } else if (g_usb_state.type == CONTROLLER_XBOXONE && transferred >= 4) {
            if (rx[0] == XBOXONE_REPORT_INPUT && transferred >= 18) {
                translator_convert_xboxone((const XboxOneReport*)rx, &ds4, &s_trans_cfg);
                if (s_xboxone_guide) {
                    ds4.buttons |= get_guide_button_mask();
                }
                handle_pad_input_and_send(&ds4);
            } else if (rx[0] == XBOXONE_REPORT_GUIDE) {
                // Send GIP ACK to controller if requested (GIP_OPT_ACK: 0x10 or 0x01)
                if ((rx[1] & 0x10) || (rx[1] & 0x01)) {
                    uint8_t ack_pkt[13] = {
                        0x01, 0x20, rx[2], 0x09, 0x00, 0x07, 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
                    };
                    send_gip_ack(ack_pkt, sizeof(ack_pkt));
                }

                uint8_t gval = (transferred >= 5) ? rx[4] : rx[3];
                int guide_pressed = (gval & 0x01) ? 1 : 0;
                if (guide_pressed != s_last_guide_pressed) {
                    s_last_guide_pressed = guide_pressed;
                    daemon_log(LOG_LEVEL_INFO, "Xbox One Guide button %s (val=0x%02X, len=%d, mask=0x%08X)",
                               guide_pressed ? "PRESSED" : "RELEASED", gval, transferred, get_guide_button_mask());
                }
                s_xboxone_guide = guide_pressed;
                s_last_ds4.buttons &= ~(DS4_BUTTON_PS | DS4_BUTTON_TOUCHPAD | DS4_BUTTON_SHARE);
                if (s_xboxone_guide) {
                    s_last_ds4.buttons |= get_guide_button_mask();
                }
                s_last_ds4.timestamp = daemon_sys_time_us();
                if (s_usb_vpad_slot >= 0) {
                    vpad_send_data_slot(s_usb_vpad_slot, &s_last_ds4);
                } else {
                    vpad_send_data(&s_last_ds4);
                }
            } else {
                // Handle general GIP control / status / announce packets requiring host ACK
                if (transferred >= 3 && ((rx[1] & 0x10) || (rx[1] & 0x01))) {
                    uint8_t ack_pkt[13] = {
                        0x01, 0x20, rx[2], 0x09, 0x00, rx[0], 0x20, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00
                    };
                    send_gip_ack(ack_pkt, sizeof(ack_pkt));
                    daemon_log(LOG_LEVEL_INFO, "GIP ACK sent for cmd 0x%02X (seq 0x%02X, len %d)", rx[0], rx[2], transferred);
                }
            }
        } else if (g_usb_state.type == CONTROLLER_SWITCH && transferred >= sizeof(SwitchInputOnlyReport)) {
            translator_convert_switch((const SwitchInputOnlyReport*)rx, &ds4, &s_trans_cfg);
            const SwitchInputOnlyReport* sw = (const SwitchInputOnlyReport*)rx;
            if (sw->buttons1 & SWITCH_BTN_HOME) {
                ds4.buttons |= get_guide_button_mask();
            }
            handle_pad_input_and_send(&ds4);
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

                    static uint32_t s_last_sw_btn = 0;
                    if (ds4.buttons != s_last_sw_btn) {
                        s_last_sw_btn = ds4.buttons;
                        daemon_log(LOG_LEVEL_INFO, "Switch Pro buttons active: 0x%08X", ds4.buttons);
                    }

                    handle_pad_input_and_send(&ds4);
                }
            } else if (rx[0] == 0x3F && transferred >= sizeof(SwitchSimpleReport)) {
                const SwitchSimpleReport* simple = (const SwitchSimpleReport*)rx;
                SwitchInputOnlyReport sw_fallback;
                sw_fallback.buttons0 = simple->buttons0;
                sw_fallback.buttons1 = simple->buttons1;
                sw_fallback.hat = simple->hat;
                sw_fallback.left_stick_x = (uint8_t)(simple->left_stick_x >> 8);
                sw_fallback.left_stick_y = (uint8_t)(simple->left_stick_y >> 8);
                sw_fallback.right_stick_x = (uint8_t)(simple->right_stick_x >> 8);
                sw_fallback.right_stick_y = (uint8_t)(simple->right_stick_y >> 8);

                translator_convert_switch(&sw_fallback, &ds4, &s_trans_cfg);
                if (sw_fallback.buttons1 & SWITCH_BTN_HOME) {
                    ds4.buttons |= get_guide_button_mask();
                }
                handle_pad_input_and_send(&ds4);
            } else if (transferred >= (int)sizeof(SwitchInputOnlyReport) && rx[0] != 0x81) {
                // Passive mode fallback for clone controllers streaming direct reports without 0x30/0x3F prefix
                translator_convert_switch((const SwitchInputOnlyReport*)rx, &ds4, &s_trans_cfg);
                const SwitchInputOnlyReport* sw = (const SwitchInputOnlyReport*)rx;
                if (sw->buttons1 & SWITCH_BTN_HOME) {
                    ds4.buttons |= get_guide_button_mask();
                }
                handle_pad_input_and_send(&ds4);
            }
        } else if (g_usb_state.type == CONTROLLER_DS3 && transferred >= sizeof(DS3Report)) {
            translator_convert_ds3((const DS3Report*)rx, &ds4, &s_trans_cfg);
            handle_pad_input_and_send(&ds4);
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

                static uint32_t s_last_stadia_btn = 0;
                if (ds4.buttons != s_last_stadia_btn) {
                    s_last_stadia_btn = ds4.buttons;
                    daemon_log(LOG_LEVEL_INFO, "Stadia buttons active: 0x%08X", ds4.buttons);
                }

                handle_pad_input_and_send(&ds4);
            }
        } else if (g_usb_state.type == CONTROLLER_DS4 && transferred >= 10) {
            static int s_logged_ds4_report = 0;
            if (s_logged_ds4_report < 10) {
                daemon_log(LOG_LEVEL_INFO, "DS4 report [%d bytes]: rx[0]=0x%02X rx[1]=0x%02X rx[2]=0x%02X rx[3]=0x%02X",
                           transferred, rx[0], (transferred > 1 ? rx[1] : 0), (transferred > 2 ? rx[2] : 0), (transferred > 3 ? rx[3] : 0));
                s_logged_ds4_report++;
            }

            DS4Report ds4_pkt;
            const DS4Report* ds4_report = NULL;
            if (rx[0] == 0x01 && transferred >= (int)sizeof(DS4Report)) {
                ds4_report = (const DS4Report*)rx;
            } else if (rx[0] == 0x01) {
                memset(&ds4_pkt, 0, sizeof(ds4_pkt));
                memcpy(&ds4_pkt, rx, transferred < (int)sizeof(ds4_pkt) ? transferred : sizeof(ds4_pkt));
                ds4_report = &ds4_pkt;
            } else if (transferred >= 9) {
                memset(&ds4_pkt, 0, sizeof(ds4_pkt));
                ds4_pkt.report_id = 0x01;
                memcpy(&ds4_pkt.left_stick_x, rx, transferred < (int)(sizeof(ds4_pkt) - 1) ? transferred : (sizeof(ds4_pkt) - 1));
                ds4_report = &ds4_pkt;
            }

            if (ds4_report) {
                translator_convert_ds4(ds4_report, &ds4, &s_trans_cfg);
                handle_pad_input_and_send(&ds4);
            }
        } else if (g_usb_state.type == CONTROLLER_GENERIC_HID && transferred > 0) {
            if (s_logged_generic_report < 10) {
                daemon_log(LOG_LEVEL_INFO, "Generic HID report [%d bytes]: rx[0]=0x%02X rx[1]=0x%02X rx[2]=0x%02X rx[3]=0x%02X",
                           transferred, rx[0], (transferred > 1 ? rx[1] : 0), (transferred > 2 ? rx[2] : 0), (transferred > 3 ? rx[3] : 0));
                s_logged_generic_report++;
            }

            if (generic_hid_parse_report(rx, transferred, &ds4, &s_trans_cfg)) {
                handle_pad_input_and_send(&ds4);
            }
        }
    } else {
        // Check for physical disconnect on any transfer error/timeout
        if (p_sceUsbdCheckConnected != NULL && p_sceUsbdCheckConnected(g_dev_handle) != 0) {
            s_consecutive_disconnects++;
            if (s_consecutive_disconnects >= 10 || ret == 0x80240004 || ret == -4) {
                if (!is_current_device_present_on_bus()) {
                    daemon_log(LOG_LEVEL_INFO, "USB controller physical disconnect confirmed (ret=0x%08X)", ret);
                    close_controller();
                    s_consecutive_disconnects = 0;
                    return;
                }
            }
        } else {
            s_consecutive_disconnects = 0;
        }

        if (ret == 0x80240007 || ret == -7) {
            s_consecutive_timeouts++;
            // Active bus presence probe: if device has timed out for ~30 consecutive polls (~500ms),
            // verify if the device is still physically enumerated on the USB bus.
            if (s_consecutive_timeouts >= 30) {
                if (!is_current_device_present_on_bus()) {
                    daemon_log(LOG_LEVEL_INFO, "USB controller physical disconnect confirmed (device gone from bus)");
                    s_consecutive_timeouts = 0;
                    s_consecutive_transfer_errors = 0;
                    close_controller();
                    return;
                }
                // Controller is still present on the bus, just idle
                s_consecutive_timeouts = 0;
            }

            // Normal USB interrupt timeout: keep 250 Hz stream alive with last known state
            if (s_last_ds4.connected && (s_usb_vpad_slot >= 0 ? vpad_slot_is_active(s_usb_vpad_slot) : vpad_is_active())) {
                s_last_ds4.timestamp = daemon_sys_time_us();
                if (s_usb_vpad_slot >= 0) {
                    vpad_send_data_slot(s_usb_vpad_slot, &s_last_ds4);
                } else {
                    vpad_send_data(&s_last_ds4);
                }
            }
        } else if (ret != 0) {
            s_consecutive_transfer_errors++;
            if (g_usb_state.type == CONTROLLER_XBOX360 || g_usb_state.type == CONTROLLER_XBOXONE) {
                static int s_logged_xb_err = 0;
                if (s_logged_xb_err < 10) {
                    daemon_log(LOG_LEVEL_WARN, "%s interrupt transfer returned 0x%08X (ep=0x%02X, count=%d)",
                               controller_type_to_str(g_usb_state.type), ret, g_usb_state.endpoint_in, s_consecutive_transfer_errors);
                    s_logged_xb_err++;
                }
                int is_ep_error = (ret == 0x80240009 || ret == 0x802400FF);
                if (is_ep_error && p_sceUsbdClearHalt && (s_consecutive_transfer_errors == 1 || s_consecutive_transfer_errors % 25 == 0)) {
                    daemon_log(LOG_LEVEL_WARN, "%s EP_IN error 0x%08X — attempting ClearHalt recovery (count=%d)",
                               controller_type_to_str(g_usb_state.type), ret, s_consecutive_transfer_errors);
                    p_sceUsbdClearHalt(g_dev_handle, g_usb_state.endpoint_in);
                    daemon_sys_usleep(10000);
                }
            } else if (g_usb_state.type == CONTROLLER_SWITCH_PRO) {
                daemon_log(LOG_LEVEL_WARN, "Switch Pro interrupt transfer returned 0x%08X (ep=0x%02X, count=%d)",
                           ret, g_usb_state.endpoint_in, s_consecutive_transfer_errors);
            } else if (g_usb_state.type == CONTROLLER_STADIA) {
                daemon_log(LOG_LEVEL_WARN, "Stadia interrupt transfer returned 0x%08X (ep=0x%02X, count=%d)",
                           ret, g_usb_state.endpoint_in, s_consecutive_transfer_errors);
            } else if (g_usb_state.type == CONTROLLER_DS4) {
                static int s_logged_ds4_err = 0;
                if (s_logged_ds4_err < 10) {
                    daemon_log(LOG_LEVEL_WARN, "DualShock 4 interrupt transfer returned 0x%08X (ep=0x%02X, count=%d)",
                               ret, g_usb_state.endpoint_in, s_consecutive_transfer_errors);
                    s_logged_ds4_err++;
                }
                int is_ep_error = (ret == 0x80240009 || ret == 0x802400FF);
                if (is_ep_error && p_sceUsbdClearHalt && (s_consecutive_transfer_errors == 1 || s_consecutive_transfer_errors % 25 == 0)) {
                    daemon_log(LOG_LEVEL_WARN, "DualShock 4 EP_IN error 0x%08X — attempting ClearHalt recovery (count=%d)",
                               ret, s_consecutive_transfer_errors);
                    p_sceUsbdClearHalt(g_dev_handle, g_usb_state.endpoint_in);
                    daemon_sys_usleep(10000);
                }
            } else if (g_usb_state.type == CONTROLLER_DS3) {
                static int s_logged_ds3_err = 0;
                if (s_logged_ds3_err < 10) {
                    daemon_log(LOG_LEVEL_WARN, "DualShock 3 interrupt transfer returned 0x%08X (ep=0x%02X, count=%d)",
                               ret, g_usb_state.endpoint_in, s_consecutive_transfer_errors);
                    s_logged_ds3_err++;
                }
                int is_ep_error = (ret == 0x80240009 || ret == 0x802400FF);
                if (is_ep_error && p_sceUsbdClearHalt && (s_consecutive_transfer_errors == 1 || s_consecutive_transfer_errors % 25 == 0)) {
                    daemon_log(LOG_LEVEL_WARN, "DualShock 3 EP_IN error 0x%08X — attempting ClearHalt recovery (count=%d)",
                               ret, s_consecutive_transfer_errors);
                    p_sceUsbdClearHalt(g_dev_handle, g_usb_state.endpoint_in);
                    daemon_sys_usleep(10000);
                }
            } else if (g_usb_state.type == CONTROLLER_GENERIC_HID) {
                static int s_logged_gen_err = 0;
                if (s_logged_gen_err < 10) {
                    daemon_log(LOG_LEVEL_WARN, "Generic HID interrupt transfer returned 0x%08X (ep=0x%02X, count=%d)",
                               ret, g_usb_state.endpoint_in, s_consecutive_transfer_errors);
                    s_logged_gen_err++;
                }
                int is_ep_error = (ret == 0x80240009 || ret == 0x802400FF);
                if (is_ep_error && p_sceUsbdClearHalt && (s_consecutive_transfer_errors == 1 || s_consecutive_transfer_errors % 10 == 0)) {
                    daemon_log(LOG_LEVEL_WARN, "Generic HID EP_IN error 0x%08X — attempting ClearHalt recovery (count=%d)",
                               ret, s_consecutive_transfer_errors);
                    p_sceUsbdClearHalt(g_dev_handle, g_usb_state.endpoint_in);
                    daemon_sys_usleep(10000);
                }
            }

            // Check if device has persistent transfer failures
            int max_allowed_errors = (g_usb_state.type == CONTROLLER_GENERIC_HID) ? 250 : 100;
            if (s_consecutive_transfer_errors >= max_allowed_errors) {
                daemon_log(LOG_LEVEL_INFO, "Closing controller after persistent transfer failure (ret=0x%08X, err_count=%d)",
                           ret, s_consecutive_transfer_errors);
                s_consecutive_transfer_errors = 0;
                s_consecutive_disconnects = 0;
                close_controller();
            } else {
                // Keep 250 Hz alive with last known state during transient error recovery
                if (s_last_ds4.connected && (s_usb_vpad_slot >= 0 ? vpad_slot_is_active(s_usb_vpad_slot) : vpad_is_active())) {
                    s_last_ds4.timestamp = daemon_sys_time_us();
                    if (s_usb_vpad_slot >= 0) {
                        vpad_send_data_slot(s_usb_vpad_slot, &s_last_ds4);
                    } else {
                        vpad_send_data(&s_last_ds4);
                    }
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
