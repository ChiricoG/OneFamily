/*
 * Virtual DualShock 4 Device Handler for OneFamily Daemon
 * Creates native Virtual DualShock 4 (Device Type 1) via libScePad
 * Compatible with XMB, PS4 games, homebrew, and system overlays
 * Supports Multi-User & Player Slot binding via libSceUserService & libSceMbus
 */

#include "vpad.h"
#include "config.h"
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
typedef int32_t (*scePadGetHandle_t)(int32_t userId, uint32_t type, uint32_t index);
typedef int32_t (*scePadOpen_t)(int32_t userId, int32_t type, int32_t index, void *param);
typedef int32_t (*scePadClose_t)(int32_t handle);
typedef int32_t (*scePadGetDeviceId_t)(int32_t handle, int64_t *outDeviceId);
typedef int32_t (*scePadDisconnectPort_t)(int32_t handle);
typedef int32_t (*scePadDisconnectDevice_t)(int32_t handle_or_devId);

// libSceMbus APIs
typedef int (*sceMbusInit_t)(void);
typedef int (*sceMbusBindDeviceWithUserId_t)(int deviceId, int32_t userId);
typedef int (*sceMbusDisconnectDevice_t)(int deviceId);
typedef int32_t (*sceMbusResolveByHandle_t)(int32_t handle, int64_t *outDeviceId);

// libSceHidControl APIs
typedef int32_t (*sceHidControlInit_t)(void);
typedef int32_t (*sceHidControlGetDeviceId_t)(int32_t handle, int64_t *outDeviceId);
typedef int32_t (*sceHidControlDisconnectPort_t)(int32_t handle);
typedef int32_t (*sceHidControlDisconnectDevice_t)(int32_t deviceId);
typedef int32_t (*sceHidControlDisconnectHandle_t)(int32_t handle);

// libSceUserService APIs
#define SCE_USER_SERVICE_MAX_LOGIN_USERS 4
#define SCE_USER_SERVICE_USER_ID_INVALID (-1)

typedef int32_t SceUserServiceUserId;

typedef struct {
    SceUserServiceUserId userId[SCE_USER_SERVICE_MAX_LOGIN_USERS];
} SceUserServiceLoginUserIdList;

typedef int (*sceUserServiceInitialize_t)(void *params);
typedef int (*sceUserServiceGetInitialUser_t)(SceUserServiceUserId *userId);
typedef int (*sceUserServiceGetLoginUserIdList_t)(SceUserServiceLoginUserIdList *userIdList);
typedef int (*sceUserServiceGetUserName_t)(SceUserServiceUserId userId, char *userName, size_t size);

static int g_hMbus = 0;
static int g_hPad = 0;
static int g_hUserService = 0;
static int g_hHidControl = 0;
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
static scePadGetHandle_t                     p_scePadGetHandle = NULL;
static scePadOpen_t                          p_scePadOpen = NULL;
static scePadClose_t                         p_scePadClose = NULL;
static scePadGetDeviceId_t                   p_scePadGetDeviceId = NULL;
static scePadDisconnectPort_t                p_scePadDisconnectPort = NULL;
static scePadDisconnectDevice_t              p_scePadDisconnectDevice = NULL;

static sceMbusInit_t                         p_sceMbusInit = NULL;
static sceMbusBindDeviceWithUserId_t         p_sceMbusBindDeviceWithUserId = NULL;
static sceMbusDisconnectDevice_t             p_sceMbusDisconnectDevice = NULL;
static sceMbusResolveByHandle_t              p_sceMbusResolveByHandle = NULL;

static sceHidControlInit_t                   p_sceHidControlInit = NULL;
static sceHidControlGetDeviceId_t            p_sceHidControlGetDeviceId = NULL;
static sceHidControlDisconnectPort_t         p_sceHidControlDisconnectPort = NULL;
static sceHidControlDisconnectDevice_t       p_sceHidControlDisconnectDevice = NULL;
static sceHidControlDisconnectHandle_t       p_sceHidControlDisconnectHandle = NULL;

static sceUserServiceInitialize_t           p_sceUserServiceInitialize = NULL;
static sceUserServiceGetInitialUser_t       p_sceUserServiceGetInitialUser = NULL;
static sceUserServiceGetLoginUserIdList_t   p_sceUserServiceGetLoginUserIdList = NULL;
static sceUserServiceGetUserName_t          p_sceUserServiceGetUserName = NULL;

static int s_assigned_slot = 1;
static char s_assigned_username[64] = "Player 1";
static int s_last_login_count = -1;

// Smart delta rate-limiting state
static OrbisPadData s_last_sent_data;
static uint64_t s_last_send_time_us = 0;
static bool s_has_sent_first = false;

static inline int abs_diff(int a, int b) {
    int d = a - b;
    return d < 0 ? -d : d;
}

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

static void init_user_service(void) {
    if (g_hUserService == 0) {
        if (load_prx("libSceUserService.sprx", &g_hUserService) == 0 && g_hUserService != 0) {
            daemon_log(LOG_LEVEL_INFO, "libSceUserService.sprx loaded successfully (handle=0x%X)", g_hUserService);
            daemon_sys_dlsym(g_hUserService, "sceUserServiceInitialize", (void**)&p_sceUserServiceInitialize);
            daemon_sys_dlsym(g_hUserService, "sceUserServiceGetInitialUser", (void**)&p_sceUserServiceGetInitialUser);
            daemon_sys_dlsym(g_hUserService, "sceUserServiceGetLoginUserIdList", (void**)&p_sceUserServiceGetLoginUserIdList);
            daemon_sys_dlsym(g_hUserService, "sceUserServiceGetUserName", (void**)&p_sceUserServiceGetUserName);
            if (p_sceUserServiceInitialize) {
                int r = p_sceUserServiceInitialize(NULL);
                daemon_log(LOG_LEVEL_INFO, "sceUserServiceInitialize() = 0x%08X", r);
            }
        } else {
            daemon_log(LOG_LEVEL_WARN, "Failed to load libSceUserService.sprx");
        }
    }
}

static void init_hid_control(void) {
    if (g_hHidControl == 0) {
        if (load_prx("libSceHidControl.sprx", &g_hHidControl) == 0 && g_hHidControl != 0) {
            daemon_log(LOG_LEVEL_INFO, "libSceHidControl.sprx loaded successfully (handle=0x%X)", g_hHidControl);
            daemon_sys_dlsym(g_hHidControl, "sceHidControlInit", (void**)&p_sceHidControlInit);
            daemon_sys_dlsym(g_hHidControl, "sceHidControlGetDeviceId", (void**)&p_sceHidControlGetDeviceId);
            daemon_sys_dlsym(g_hHidControl, "sceHidControlDisconnectPort", (void**)&p_sceHidControlDisconnectPort);
            daemon_sys_dlsym(g_hHidControl, "sceHidControlDisconnectDevice", (void**)&p_sceHidControlDisconnectDevice);
            daemon_sys_dlsym(g_hHidControl, "sceHidControlDisconnectHandle", (void**)&p_sceHidControlDisconnectHandle);
            if (p_sceHidControlInit) {
                int r = p_sceHidControlInit();
                daemon_log(LOG_LEVEL_INFO, "sceHidControlInit() = 0x%08X", r);
            }
        }
    }
}

static void vpad_poweroff_physical_ds4(void) {
    if (g_daemon_config.mode == CONFIG_MODE_SECONDARY) {
        daemon_log(LOG_LEVEL_INFO, "Skipping physical DS4 power-off (CONFIG_MODE_SECONDARY active for Player 2 co-op)");
        return;
    }

    init_user_service();
    init_hid_control();

    SceUserServiceLoginUserIdList list;
    memset(&list, 0, sizeof(list));
    for (int i = 0; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        list.userId[i] = SCE_USER_SERVICE_USER_ID_INVALID;
    }

    if (p_sceUserServiceGetLoginUserIdList) {
        p_sceUserServiceGetLoginUserIdList(&list);
    }
    if (list.userId[0] == SCE_USER_SERVICE_USER_ID_INVALID && p_sceUserServiceGetInitialUser) {
        p_sceUserServiceGetInitialUser(&list.userId[0]);
    }

    bool any_disconnected = false;
    for (int i = 0; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        SceUserServiceUserId uid = list.userId[i];
        if (uid == SCE_USER_SERVICE_USER_ID_INVALID || uid == 0 || (uint32_t)uid == 0xFFFFFFFF) {
            continue;
        }

        int32_t pad_handle = -1;
        bool opened_temp = false;
        if (p_scePadGetHandle) {
            pad_handle = p_scePadGetHandle(uid, 0, 0);
        }
        if (pad_handle < 0 && p_scePadOpen) {
            pad_handle = p_scePadOpen(uid, 0, 0, NULL);
            if (pad_handle >= 0) {
                opened_temp = true;
            }
        }

        if (pad_handle >= 0 && pad_handle != g_vpad_handle) {
            int64_t dev_id[4] = {0, 0, 0, 0};
            if (p_scePadGetDeviceId) {
                int r_id = p_scePadGetDeviceId(pad_handle, dev_id);
                daemon_log(LOG_LEVEL_INFO, "Physical DS4 [user=0x%X, handle=0x%X]: scePadGetDeviceId() = 0x%08X, dev_id=0x%llX",
                           uid, pad_handle, r_id, (unsigned long long)dev_id[0]);
            }
            if (dev_id[0] == 0 && p_sceHidControlGetDeviceId) {
                p_sceHidControlGetDeviceId(pad_handle, dev_id);
            }
            if (dev_id[0] == 0 && p_sceMbusResolveByHandle) {
                p_sceMbusResolveByHandle(pad_handle, dev_id);
            }

            daemon_log(LOG_LEVEL_INFO, "Powering off physical DS4 for user 0x%X (handle=0x%X, dev_id=0x%X)...",
                       uid, pad_handle, (int32_t)dev_id[0]);

            if (p_scePadDisconnectPort) {
                int r = p_scePadDisconnectPort(pad_handle);
                daemon_log(LOG_LEVEL_INFO, "scePadDisconnectPort(0x%X) = 0x%08X", pad_handle, r);
            }
            if (dev_id[0] > 0) {
                if (p_sceMbusDisconnectDevice) {
                    int r = p_sceMbusDisconnectDevice((int32_t)dev_id[0]);
                    daemon_log(LOG_LEVEL_INFO, "sceMbusDisconnectDevice(0x%X) = 0x%08X", (int32_t)dev_id[0], r);
                }
                if (p_sceHidControlDisconnectDevice) {
                    int r = p_sceHidControlDisconnectDevice((int32_t)dev_id[0]);
                    daemon_log(LOG_LEVEL_INFO, "sceHidControlDisconnectDevice(0x%X) = 0x%08X", (int32_t)dev_id[0], r);
                }
                if (p_scePadDisconnectDevice) {
                    int r = p_scePadDisconnectDevice((int32_t)dev_id[0]);
                    daemon_log(LOG_LEVEL_INFO, "scePadDisconnectDevice(dev_id=0x%X) = 0x%08X", (int32_t)dev_id[0], r);
                }
            }
            if (p_sceHidControlDisconnectPort) {
                int r = p_sceHidControlDisconnectPort(pad_handle);
                daemon_log(LOG_LEVEL_INFO, "sceHidControlDisconnectPort(0x%X) = 0x%08X", pad_handle, r);
            }
            if (p_sceHidControlDisconnectHandle) {
                int r = p_sceHidControlDisconnectHandle(pad_handle);
                daemon_log(LOG_LEVEL_INFO, "sceHidControlDisconnectHandle(0x%X) = 0x%08X", pad_handle, r);
            }
            if (p_scePadDisconnectDevice) {
                int r = p_scePadDisconnectDevice(pad_handle);
                daemon_log(LOG_LEVEL_INFO, "scePadDisconnectDevice(handle=0x%X) = 0x%08X", pad_handle, r);
            }

            if (opened_temp && p_scePadClose) {
                p_scePadClose(pad_handle);
            }
            any_disconnected = true;
        }
    }

    if (any_disconnected) {
        // Wait 100ms for Bluetooth/Mbus stack to complete physical DS4 power-off
        daemon_sys_usleep(100000);
    }
}

int vpad_bind_user(ConfigMode mode, int *out_slot, char *out_username, size_t username_len) {
    init_user_service();

    SceUserServiceLoginUserIdList list;
    memset(&list, 0, sizeof(list));
    for (int i = 0; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        list.userId[i] = SCE_USER_SERVICE_USER_ID_INVALID;
    }

    if (p_sceUserServiceGetLoginUserIdList) {
        int r = p_sceUserServiceGetLoginUserIdList(&list);
        daemon_log(LOG_LEVEL_INFO, "sceUserServiceGetLoginUserIdList() = 0x%08X (u0=0x%X, u1=0x%X, u2=0x%X, u3=0x%X)",
                   r, list.userId[0], list.userId[1], list.userId[2], list.userId[3]);
    }

    int num_logged_in = 0;
    for (int i = 0; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        if (list.userId[i] != SCE_USER_SERVICE_USER_ID_INVALID && list.userId[i] != 0 && (uint32_t)list.userId[i] != 0xFFFFFFFF) {
            num_logged_in++;
        }
    }
    s_last_login_count = num_logged_in;

    SceUserServiceUserId target_user = SCE_USER_SERVICE_USER_ID_INVALID;
    int slot = 1;

    if (mode == CONFIG_MODE_SECONDARY) {
        // Explicitly forced to Player 2
        if (num_logged_in >= 2 && list.userId[1] != SCE_USER_SERVICE_USER_ID_INVALID) {
            target_user = list.userId[1];
            slot = 2;
        } else if (num_logged_in >= 1) {
            target_user = list.userId[0];
            slot = 2; // Tagged as slot 2 for coexistence
        }
    } else if (mode == CONFIG_MODE_MERGE) {
        // Explicitly forced to Player 1
        if (num_logged_in >= 1) {
            target_user = list.userId[0];
            slot = 1;
        }
    } else {
        // CONFIG_MODE_AUTO (default):
        // If 2 or more users logged in (e.g. DS4 is on P1), virtual pad becomes P2!
        if (num_logged_in >= 2 && list.userId[1] != SCE_USER_SERVICE_USER_ID_INVALID) {
            target_user = list.userId[1];
            slot = 2;
        } else if (num_logged_in >= 1) {
            target_user = list.userId[0];
            slot = 1;
        }
    }

    if (target_user == SCE_USER_SERVICE_USER_ID_INVALID && p_sceUserServiceGetInitialUser) {
        p_sceUserServiceGetInitialUser(&target_user);
        slot = 1;
    }

    s_assigned_slot = slot;
    s_assigned_username[0] = '\0';

    if (target_user != SCE_USER_SERVICE_USER_ID_INVALID && p_sceUserServiceGetUserName) {
        p_sceUserServiceGetUserName(target_user, s_assigned_username, sizeof(s_assigned_username));
    }
    if (s_assigned_username[0] == '\0') {
        snprintf(s_assigned_username, sizeof(s_assigned_username), "User %d", slot);
    }

    if (mode != CONFIG_MODE_AUTO && target_user != SCE_USER_SERVICE_USER_ID_INVALID && p_sceMbusBindDeviceWithUserId && g_vpad_handle >= 0) {
        int r = p_sceMbusBindDeviceWithUserId(g_vpad_handle, target_user);
        daemon_log(LOG_LEVEL_INFO, "sceMbusBindDeviceWithUserId(handle=0x%X, user=0x%08X, slot=%d, name='%s') = 0x%08X",
                   g_vpad_handle, target_user, slot, s_assigned_username, r);
    } else {
        daemon_log(LOG_LEVEL_INFO, "Virtual DS4 left for native shell assignment (mode=%d, slot=%d, active User='%s', id=0x%08X)",
                   mode, slot, s_assigned_username, target_user);
    }

    if (out_slot) *out_slot = slot;
    if (out_username && username_len > 0) {
        strncpy(out_username, s_assigned_username, username_len - 1);
        out_username[username_len - 1] = '\0';
    }

    return 0;
}

void vpad_pulse_ps_button(void) {
    if (!g_vpad_active || g_vpad_handle < 0 || !p_scePadVirtualDeviceInsertData) {
        return;
    }
    daemon_log(LOG_LEVEL_INFO, "vpad_pulse_ps_button: Sending native PS button pulse (120ms) to trigger user selection...");

    OrbisPadData pulse;
    memset(&pulse, 0, sizeof(pulse));
    pulse.leftStick.x = 128;
    pulse.leftStick.y = 128;
    pulse.rightStick.x = 128;
    pulse.rightStick.y = 128;
    pulse.connected = 1;
    pulse.count = 1;
    pulse.buttons = DS4_BUTTON_PS;
    pulse.timestamp = daemon_sys_time_us();

    // 1. Send PS button pressed
    p_scePadVirtualDeviceInsertData(g_vpad_handle, &pulse);
    s_last_send_time_us = pulse.timestamp;
    s_last_sent_data = pulse;
    s_has_sent_first = true;

    // 2. Hold pulse for 120 ms so SceShellCore latches it cleanly
    daemon_sys_usleep(120000);

    // 3. Send PS button released
    pulse.buttons = 0;
    pulse.timestamp = daemon_sys_time_us();
    p_scePadVirtualDeviceInsertData(g_vpad_handle, &pulse);
    s_last_send_time_us = pulse.timestamp;
    s_last_sent_data = pulse;

    daemon_log(LOG_LEVEL_INFO, "vpad_pulse_ps_button: PS button released, native user picker launched");
}

int vpad_get_assigned_slot(void) {
    return s_assigned_slot;
}

const char* vpad_get_assigned_user(void) {
    return s_assigned_username;
}

void vpad_check_user_rebind(ConfigMode mode) {
    if (!g_vpad_active || g_vpad_handle < 0 || mode != CONFIG_MODE_AUTO || !p_sceUserServiceGetLoginUserIdList) {
        return;
    }

    SceUserServiceLoginUserIdList list;
    memset(&list, 0, sizeof(list));
    if (p_sceUserServiceGetLoginUserIdList(&list) == 0) {
        int num_logged_in = 0;
        for (int i = 0; i < SCE_USER_SERVICE_MAX_LOGIN_USERS; i++) {
            if (list.userId[i] != SCE_USER_SERVICE_USER_ID_INVALID && list.userId[i] != 0 && (uint32_t)list.userId[i] != 0xFFFFFFFF) {
                num_logged_in++;
            }
        }
        if (num_logged_in != s_last_login_count) {
            daemon_log(LOG_LEVEL_INFO, "Logged in user count changed (%d -> %d), updating virtual pad binding...",
                       s_last_login_count, num_logged_in);
            vpad_bind_user(mode, NULL, NULL, 0);
        }
    }
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
            daemon_sys_dlsym(g_hMbus, "sceMbusBindDeviceWithUserId", (void**)&p_sceMbusBindDeviceWithUserId);
            daemon_sys_dlsym(g_hMbus, "sceMbusDisconnectDevice", (void**)&p_sceMbusDisconnectDevice);
            daemon_sys_dlsym(g_hMbus, "sceMbusResolveByHandle", (void**)&p_sceMbusResolveByHandle);
            if (p_sceMbusInit) {
                int r = p_sceMbusInit();
                daemon_log(LOG_LEVEL_INFO, "sceMbusInit() = 0x%08X", r);
            }
        }
    }

    // 2. Load libScePad and initialize Pad/Mbus once
    if (g_hPad == 0) {
        if (load_prx("libScePad.sprx", &g_hPad) < 0) {
            daemon_log(LOG_LEVEL_ERROR, "Failed to load libScePad.sprx");
            return -1;
        }
        daemon_log(LOG_LEVEL_INFO, "libScePad.sprx loaded successfully (handle=0x%X)", g_hPad);

        // 3. Resolve Pad Symbols
        daemon_sys_dlsym(g_hPad, "scePadInit", (void**)&p_scePadInit);
        daemon_sys_dlsym(g_hPad, "scePadMbusInit", (void**)&p_scePadMbusInit);
        daemon_sys_dlsym(g_hPad, "scePadSetProcessPrivilege", (void**)&p_scePadSetProcessPrivilege);
        daemon_sys_dlsym(g_hPad, "scePadSetConnection", (void**)&p_scePadSetConnection);
        daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceAddDevice", (void**)&p_scePadVirtualDeviceAddDevice);
        daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceInsertData", (void**)&p_scePadVirtualDeviceInsertData);
        daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceDeleteDevice", (void**)&p_scePadVirtualDeviceDeleteDevice);
        daemon_sys_dlsym(g_hPad, "scePadVirtualDeviceGetRemoteSetting", (void**)&p_scePadVirtualDeviceGetRemoteSetting);
        daemon_sys_dlsym(g_hPad, "scePadGetHandle", (void**)&p_scePadGetHandle);
        daemon_sys_dlsym(g_hPad, "scePadOpen", (void**)&p_scePadOpen);
        daemon_sys_dlsym(g_hPad, "scePadClose", (void**)&p_scePadClose);
        daemon_sys_dlsym(g_hPad, "scePadGetDeviceId", (void**)&p_scePadGetDeviceId);
        daemon_sys_dlsym(g_hPad, "scePadDisconnectPort", (void**)&p_scePadDisconnectPort);
        daemon_sys_dlsym(g_hPad, "scePadDisconnectDevice", (void**)&p_scePadDisconnectDevice);

        // Fallback: check if sceMbusBindDeviceWithUserId is exported from libScePad
        if (!p_sceMbusBindDeviceWithUserId) {
            daemon_sys_dlsym(g_hPad, "sceMbusBindDeviceWithUserId", (void**)&p_sceMbusBindDeviceWithUserId);
        }

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
    }

    if (!p_scePadVirtualDeviceAddDevice || !p_scePadVirtualDeviceInsertData) {
        daemon_log(LOG_LEVEL_ERROR, "VirtualDevice APIs not exported by libScePad");
        return -2;
    }

    // 3b. Power off / disconnect any active physical DualShock 4 before registering Virtual DS4
    //     so its Bluetooth link drops cleanly (LED turns off) and never gets stuck in an unbound state
    vpad_poweroff_physical_ds4();

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

    // 6. Bind to User Profile / Player Slot
    int slot = 1;
    char username[64];
    vpad_bind_user(g_daemon_config.mode, &slot, username, sizeof(username));

    // Reset rate-limiting state
    s_has_sent_first = false;
    memset(&s_last_sent_data, 0, sizeof(s_last_sent_data));
    s_last_send_time_us = 0;

    char notif[128];
    if (slot == 2) {
        snprintf(notif, sizeof(notif), "OneFamily: Virtual DS4 Active (Player 2: %s)", username);
    } else {
        snprintf(notif, sizeof(notif), "OneFamily: Virtual DS4 Active (%s)", username);
    }
    daemon_notify(notif);
    return 0;
}

// Rumble polling state
static uint8_t s_last_rumble_lg = 0;
static uint8_t s_last_rumble_sm = 0;
static uint64_t s_last_rumble_send_us = 0;

void vpad_shutdown(void) {
    if (g_vpad_active && g_vpad_handle >= 0) {
        daemon_log(LOG_LEVEL_INFO, "Deleting Virtual Pad handle=0x%X", g_vpad_handle);
        if (p_scePadSetConnection) {
            p_scePadSetConnection(g_vpad_handle, 0);
        }
        if (p_sceMbusDisconnectDevice) {
            p_sceMbusDisconnectDevice(g_vpad_handle);
        }
        daemon_sys_usleep(50000);
        if (p_scePadVirtualDeviceDeleteDevice) {
            p_scePadVirtualDeviceDeleteDevice(g_vpad_handle);
        }
        g_vpad_handle = -1;
        g_vpad_active = false;
        s_has_sent_first = false;
        memset(&s_last_sent_data, 0, sizeof(s_last_sent_data));
        s_last_send_time_us = 0;
        s_last_rumble_lg = 0;
        s_last_rumble_sm = 0;
        s_last_rumble_send_us = 0;
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

    uint64_t now = daemon_sys_time_us();

    // Determine if pad is in neutral / resting state
    bool is_neutral = (data->buttons == 0 &&
                       data->analogButtons.l2 == 0 &&
                       data->analogButtons.r2 == 0 &&
                       abs_diff(data->leftStick.x, 128) <= 4 &&
                       abs_diff(data->leftStick.y, 128) <= 4 &&
                       abs_diff(data->rightStick.x, 128) <= 4 &&
                       abs_diff(data->rightStick.y, 128) <= 4);

    bool state_changed = false;
    if (!s_has_sent_first) {
        state_changed = true;
    } else if (data->buttons != s_last_sent_data.buttons) {
        state_changed = true;
    } else if (abs_diff(data->leftStick.x, s_last_sent_data.leftStick.x) > 1 ||
               abs_diff(data->leftStick.y, s_last_sent_data.leftStick.y) > 1 ||
               abs_diff(data->rightStick.x, s_last_sent_data.rightStick.x) > 1 ||
               abs_diff(data->rightStick.y, s_last_sent_data.rightStick.y) > 1) {
        state_changed = true;
    } else if (abs_diff(data->analogButtons.l2, s_last_sent_data.analogButtons.l2) > 2 ||
               abs_diff(data->analogButtons.r2, s_last_sent_data.analogButtons.r2) > 2) {
        state_changed = true;
    }

    // Rate-limiting / Throttling:
    // If state did NOT change:
    // - Neutral resting state: limit keep-alive to 5 Hz (200,000 us = 200 ms).
    //   This frees 98% of bus traffic so physical DS4 controllers operate with zero interference!
    // - Active held state: refresh at 50 Hz (20,000 us = 20 ms).
    if (!state_changed) {
        uint64_t interval = is_neutral ? 200000 : 20000;
        if ((now - s_last_send_time_us) < interval) {
            return 0; // Throttled cleanly
        }
    }

    int ret = p_scePadVirtualDeviceInsertData(g_vpad_handle, data);
    s_last_send_time_us = now;
    s_last_sent_data = *data;
    s_has_sent_first = true;

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

    uint8_t setting[128];
    memset(setting, 0, sizeof(setting));
    int ret = p_scePadVirtualDeviceGetRemoteSetting(g_vpad_handle, setting);
    if (ret == 0) {
        // scePadVirtualDeviceGetRemoteSetting buffer layout (DS4 Output Report without Report ID):
        // [0]    = update flags (bit 0: rumble, bit 1: light bar)
        // [1..2] = reserved (0x00)
        // [3]    = large motor (slow/strong, 0..255)
        // [4]    = small motor (fast/weak, 0..255)
        // [5..7] = light bar RGB
        uint8_t lg = setting[3];
        uint8_t sm = setting[4];
        uint64_t now = daemon_sys_time_us();

        bool changed = (lg != s_last_rumble_lg || sm != s_last_rumble_sm);
        // Periodic 100ms keep-alive while rumbling so Switch / 2.4GHz dongles do not time out
        bool keepalive = ((lg > 0 || sm > 0) && (now - s_last_rumble_send_us >= 100000ULL));

        if (changed || keepalive) {
            s_last_rumble_lg = lg;
            s_last_rumble_sm = sm;
            s_last_rumble_send_us = now;
            g_rumble_cb(lg, sm);
        }
    }
}
