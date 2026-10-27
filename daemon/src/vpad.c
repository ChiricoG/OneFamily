/*
 * Virtual DualShock 4 Device Handler for OneFamily Daemon
 * Creates native Virtual DualShock 4 (Device Type 1) via libScePad
 * Compatible with XMB, PS4 games, homebrew, and system overlays
 */

#include "vpad.h"
#include "log.h"
#include "daemon_syscalls.h"
#include "ds4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t flags;
    uint32_t reserved;
} ScePadVirtualDeviceParam;

// libScePad APIs
typedef int (*scePadInit_t)(void);
typedef int (*scePadMbusInit_t)(void);
typedef int (*scePadSetProcessPrivilege_t)(int);
typedef int (*scePadSetConnection_t)(int handle, int connection);
typedef int (*scePadVirtualDeviceAddDevice_t)(const ScePadVirtualDeviceParam *param, int *out_handle);
typedef int (*scePadVirtualDeviceInsertData_t)(int handle, const void *data);
typedef int (*scePadVirtualDeviceDeleteDevice_t)(int handle);
typedef int (*scePadVirtualDeviceGetRemoteSetting_t)(int handle, void *out_setting);

// libSceMbus APIs
typedef int (*sceMbusInit_t)(void);

static int g_hMbus = 0;
static int g_hPad = 0;
static int g_vpad_handle = -1;
static bool g_vpad_active = false;
static vpad_rumble_cb_t g_rumble_cb = NULL;

static scePadInit_t                          p_scePadInit = NULL;
static scePadMbusInit_t                      p_scePadMbusInit = NULL;
static scePadSetProcessPrivilege_t           p_scePadSetProcessPrivilege = NULL;
static scePadSetConnection_t                 p_scePadSetConnection = NULL;
static scePadVirtualDeviceAddDevice_t        p_scePadVirtualDeviceAddDevice = NULL;
static scePadVirtualDeviceInsertData_t       p_scePadVirtualDeviceInsertData = NULL;
static scePadVirtualDeviceDeleteDevice_t     p_scePadVirtualDeviceDeleteDevice = NULL;
static scePadVirtualDeviceGetRemoteSetting_t p_scePadVirtualDeviceGetRemoteSetting = NULL;

static sceMbusInit_t                     p_sceMbusInit = NULL;

static int load_prx(const char *name, int *out_h) {
    char path[256];
    int h = 0;

    // 1. Try full system library path
    snprintf(path, sizeof(path), "/system/common/lib/%s", name);
    if (daemon_sys_load_prx(path, &h) >= 0 && h != 0) {
        *out_h = h;
        return 0;
    }

    // 2. Try raw library name
    if (daemon_sys_load_prx(name, &h) >= 0 && h != 0) {
        *out_h = h;
        return 0;
    }

    return -1;
}

int vpad_init(void) {
    if (g_vpad_active) {
        return 0;
    }

    daemon_log(LOG_LEVEL_INFO, "Initializing Virtual Pad Subsystem (Device Type 1 - Native DualShock 4)...");

    // 1. Mandatory: libSceMbus must be loaded prior to libScePad
    if (g_hMbus == 0) {
        if (load_prx("libSceMbus.sprx", &g_hMbus) < 0) {
            daemon_log(LOG_LEVEL_WARN, "Failed to load libSceMbus.sprx");
        } else {
            daemon_log(LOG_LEVEL_INFO, "libSceMbus.sprx loaded successfully (handle=0x%X)", g_hMbus);
            daemon_sys_dlsym(g_hMbus, "sceMbusInit", (void**)&p_sceMbusInit);
            if (p_sceMbusInit) {
                int r = p_sceMbusInit();
                daemon_log(LOG_LEVEL_INFO, "sceMbusInit() = 0x%08X", r);
            }
        }
    }

    // 2. Load libScePad
    if (g_hPad == 0) {
        if (load_prx("libScePad.sprx", &g_hPad) < 0) {
            daemon_log(LOG_LEVEL_ERROR, "Failed to load libScePad.sprx");
            return -1;
        }
        daemon_log(LOG_LEVEL_INFO, "libScePad.sprx loaded successfully (handle=0x%X)", g_hPad);
    }

    // 3. Resolve Pad Symbols
    daemon_sys_dlsym(g_hPad, "scePadInit", (void**)&p_scePadInit);
    daemon_sys_dlsym(g_hPad, "scePadMbusInit", (void**)&p_scePadMbusInit);
    daemon_sys_dlsym(g_hPad, "scePadSetProcessPrivilege", (void**)&p_scePadSetProcessPrivilege);
    daemon_sys_dlsym(g_hPad, "scePadSetConnection", (void**)&p_scePadSetConnection);
    daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceAddDevice", (void**)&p_scePadVirtualDeviceAddDevice);
    daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceInsertData", (void**)&p_scePadVirtualDeviceInsertData);
    daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceDeleteDevice", (void**)&p_scePadVirtualDeviceDeleteDevice);
    daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceGetRemoteSetting", (void**)&p_scePadVirtualDeviceGetRemoteSetting);

    if (p_scePadInit) {
        int r = p_scePadInit();
        daemon_log(LOG_LEVEL_INFO, "scePadInit() = 0x%08X", r);
    }
    if (p_scePadMbusInit) {
        int r = p_scePadMbusInit();
        daemon_log(LOG_LEVEL_INFO, "scePadMbusInit() = 0x%08X", r);
    }
    if (p_scePadSetProcessPrivilege) {
        int r = p_scePadSetProcessPrivilege(1);
        daemon_log(LOG_LEVEL_INFO, "scePadSetProcessPrivilege(1) = 0x%08X", r);
    }

    if (!p_scePadVirtualDeviceAddDevice || !p_scePadVirtualDeviceInsertData) {
        daemon_log(LOG_LEVEL_ERROR, "VirtualDevice APIs not exported by libScePad");
        return -2;
    }

    // 4. Register Virtual DualShock 4 as Device Type 1
    // Type 1 is the official PS4 virtual DualShock 4 device type recognized by games & OS
    ScePadVirtualDeviceParam param;
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.type = 1; // Device Type 1 = DualShock 4
    param.flags = 0;

    int vHandle = -1;
    int ret = p_scePadVirtualDeviceAddDevice(&param, &vHandle);
    if (ret < 0 || vHandle < 0) {
        daemon_log(LOG_LEVEL_ERROR, "scePadVirtualDeviceAddDevice(type 1) failed: 0x%08X", ret);
        return ret;
    }

    g_vpad_handle = vHandle;
    g_vpad_active = true;
    daemon_log(LOG_LEVEL_INFO, "Virtual DualShock 4 (type 1) registered with handle=0x%X (%d)", g_vpad_handle, g_vpad_handle);

    // 5. Notify Pad Subsystem that device is connected
    if (p_scePadSetConnection) {
        int r = p_scePadSetConnection(g_vpad_handle, 1);
        daemon_log(LOG_LEVEL_INFO, "scePadSetConnection(0x%X, 1) = 0x%08X", g_vpad_handle, r);
    }

    daemon_notify("OneFamily: Virtual DS4 Active");
    return 0;
}

void vpad_shutdown(void) {
    if (g_vpad_active && g_vpad_handle >= 0) {
        daemon_log(LOG_LEVEL_INFO, "Deleting Virtual Pad handle=0x%X", g_vpad_handle);
        if (p_scePadSetConnection) {
            p_scePadSetConnection(g_vpad_handle, 0);
        }
        if (p_scePadVirtualDeviceDeleteDevice) {
            p_scePadVirtualDeviceDeleteDevice(g_vpad_handle);
        }
        g_vpad_handle = -1;
        g_vpad_active = false;
    }
}

bool vpad_is_active(void) {
    return g_vpad_active;
}

static uint32_t s_last_logged_buttons = 0;
static int s_logged_insert_packets = 0;
static int s_last_ps_state = 0;

int vpad_send_data(const OrbisPadData *data) {
    if (!g_vpad_active || g_vpad_handle < 0 || !p_scePadVirtualDeviceInsertData || !data) {
        return -1;
    }

    int ret = p_scePadVirtualDeviceInsertData(g_vpad_handle, data);

    int ps_now = (data->buttons & DS4_BUTTON_PS) ? 1 : 0;
    if (ps_now != s_last_ps_state) {
        s_last_ps_state = ps_now;
        daemon_log(LOG_LEVEL_INFO, "vpad_send_data: PS Button %s (btn=0x%08X, ret=0x%08X)",
                   ps_now ? "ACTIVE" : "RELEASED", data->buttons, ret);
    }

    // Diagnostic logging: log first 5 packets, button state changes, or errors
    if (s_logged_insert_packets < 5 || (ret != 0 && s_logged_insert_packets < 25) ||
        (data->buttons != s_last_logged_buttons && s_logged_insert_packets < 60)) {
        daemon_log(LOG_LEVEL_INFO, "InsertData(h=0x%X, btn=0x%08X, lx=%d, ly=%d) -> 0x%08X",
                   g_vpad_handle, data->buttons, data->leftStick.x, data->leftStick.y, ret);
        s_last_logged_buttons = data->buttons;
        s_logged_insert_packets++;
    }

    return ret;
}

void vpad_set_rumble_callback(vpad_rumble_cb_t cb) {
    g_rumble_cb = cb;
}

void vpad_poll_rumble(void) {
    if (!g_vpad_active || g_vpad_handle < 0 || !p_scePadVirtualDeviceGetRemoteSetting || !g_rumble_cb) {
        return;
    }

    static uint8_t s_last_lg = 0;
    static uint8_t s_last_sm = 0;

    uint8_t setting[128];
    memset(setting, 0, sizeof(setting));
    int ret = p_scePadVirtualDeviceGetRemoteSetting(g_vpad_handle, setting);
    if (ret == 0) {
        uint8_t lg = setting[0];
        uint8_t sm = setting[1];
        if (lg != s_last_lg || sm != s_last_sm) {
            s_last_lg = lg;
            s_last_sm = sm;
            g_rumble_cb(lg, sm);
        }
    }
}
