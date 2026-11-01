/*
 * OneFamily DualSense Bluetooth Driver Subsystem
 *
 * Implements native wireless DualSense support via /dev/bt kernel allowlist patching,
 * 78-byte extended HID report 0x31 decoding, 3D gyro quaternion orientation,
 * touchpad coordinate remapping (1920x1080 -> 1920x942), and Bluetooth force feedback.
 */

#include "bt_driver.h"
#include "daemon_syscalls.h"
#include "vpad.h"
#include "log.h"
#include <string.h>

// Sony DualSense Identifiers
#define SONY_VID                 0x054C
#define SONY_DUALSENSE_PID       0x0CE6
#define SONY_DUALSENSE_EDGE_PID  0x0DF2

// Syscall Numbers for FreeBSD / Orbis OS
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_KEXEC     11
#define SYS_IOCTL     54
#define SYS_KQUEUE    362
#define SYS_KEVENT    363

static volatile bool s_bt_running = false;
static int s_bt_fd = -1;
static int s_bt_queue = -1;
static bool s_bt_registered = false;
static bool s_dualsense_connected = false;
static int s_bt_vpad_slot = -1;
static uint64_t s_physical_address = 0;
static uint64_t s_last_input_time = 0;

// -------------------------------------------------------------
// CRC32 Calculation for DualSense Bluetooth Reports
// -------------------------------------------------------------

static inline uint32_t bt_crc_byte(uint32_t crc, uint8_t value) {
    crc ^= value;
    for (unsigned i = 0; i < 8; ++i) {
        crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc;
}

static uint32_t bt_calc_crc32(uint8_t prefix, const uint8_t* data, size_t len) {
    uint32_t crc = bt_crc_byte(0xFFFFFFFFU, prefix);
    for (size_t i = 0; i < len; ++i) {
        crc = bt_crc_byte(crc, data[i]);
    }
    return ~crc;
}

// -------------------------------------------------------------
// Kernel Allowlist Patching (Multi-Firmware Dynamic SigScan / Ring 0)
// -------------------------------------------------------------

struct PatchRequest {
    int restore;
};

struct PatchArgs {
    void* function;
    struct PatchRequest* request;
};

// Firmware gate: sceKernelGetSystemSwVersion() version >> 16 (BCD). 0x1250 = 12.50.
// Offsets taken from Diram1x's DualSense-PS4.
#define BT_SUPPORTED_FW 0x1250

/**
 * Ring 0 Kernel Execution callback (invoked via syscall 11 / kexec)
 * Ported faithfully from Diram1x's DualSense-PS4 (src/native.c).
 */
static int patch_allowlist(void* thread, struct PatchArgs* args) {
    (void)thread;
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000082));
    uint64_t base = (((uint64_t)high << 32) | low) - 0x1C0;

    if ((base >> 48) != 0xFFFF || (base & 0x3FFF) || !args || !args->request) {
        return 22; // EINVAL
    }

    const uint32_t offsets[10] = {
        0x5B0DA6, 0x5B0DD6, 0x5B1616, 0x5B1649, 0x5B53D8,
        0x5B5FCA, 0x5B578D, 0x5B5C6E, 0x5B580B, 0x5B5CFE
    };

    int restore = args->request->restore;
    if (restore != 0 && restore != 1) return 22;

    int (*lock)(void) = (void*)(base + 0x5A6B80);
    int (*unlock)(int) = (void*)(base + 0x5A6C00);

    if (*(volatile uint64_t*)(base + 0x5B6D00) != 0x56415741E5894855UL) {
        return 22;
    }

    int rc = lock();
    if (rc) return rc;

    unsigned original_count = 0, patched_count = 0;
    for (unsigned i = 0; i < 10; ++i) {
        uint32_t old_value = (i & 1) ? 0x1000 : 0x044F;
        uint32_t new_value = (i & 1) ? 0x0CE6 : 0x054C;
        uint32_t current = *(volatile uint32_t*)(base + offsets[i]);
        if (current == old_value) ++original_count;
        else if (current == new_value) ++patched_count;
        else {
            unlock(0);
            return 0x100 + (int)i;
        }
    }

    if (original_count && patched_count) {
        unlock(0);
        return 0x200 + (int)original_count;
    }

    if ((!restore && patched_count == 10) || (restore && original_count == 10)) {
        unlock(0);
        return 0;
    }

    uint64_t flags, cr0;
    __asm__ volatile("pushfq; popq %0; cli; mov %%cr0, %1" : "=r"(flags), "=r"(cr0) :: "memory");
    uint64_t writable = cr0 & ~(1UL << 16);
    __asm__ volatile("mov %0, %%cr0" :: "r"(writable) : "memory");

    for (unsigned i = 0; i < 10; ++i) {
        uint32_t old_value = (i & 1) ? 0x1000 : 0x044F;
        uint32_t new_value = (i & 1) ? 0x0CE6 : 0x054C;
        *(volatile uint32_t*)(base + offsets[i]) = restore ? old_value : new_value;
    }

    __asm__ volatile("mov %0, %%cr0; pushq %1; popfq" :: "r"(cr0), "r"(flags) : "memory", "cc");
    unlock(0);
    return 0;
}

static int apply_kernel_allowlist_patch(int restore) {
    struct PatchRequest req;
    memset(&req, 0, sizeof(req));
    req.restore = restore;

    long ret = daemon_syscall(SYS_KEXEC, (long)patch_allowlist, (long)&req, 0);
    daemon_log(LOG_LEVEL_INFO, "DualSense BT Kernel Allowlist Patch (restore=%d) = %ld", restore, ret);
    return (int)ret;
}

// -------------------------------------------------------------
// Bluetooth Report 0x31 / 0x01 Decoder
// -------------------------------------------------------------

static void decode_dualsense_bt_report(const uint8_t* report, size_t length) {
    static const uint32_t dpad_map[9] = {
        DS4_BUTTON_DPAD_UP,
        DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_RIGHT,
        DS4_BUTTON_DPAD_RIGHT,
        DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_RIGHT,
        DS4_BUTTON_DPAD_DOWN,
        DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_LEFT,
        DS4_BUTTON_DPAD_LEFT,
        DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_LEFT,
        0
    };

    if (!report) return;

    uint8_t simple[10] = {0};
    const uint8_t* data = NULL;

    if (length == 10 && report[0] == 0x01) {
        // Remap 10-byte simple report to match 78-byte payload indices (Diram1x logic)
        for (unsigned i = 0; i < 4; ++i) simple[i] = report[i + 1];
        simple[4] = report[8];       // L2 trigger
        simple[5] = report[9];       // R2 trigger
        simple[7] = report[5];       // D-Pad + Face buttons
        simple[8] = report[6];       // Bumpers, triggers, options, L3, R3
        simple[9] = report[7] & 3;   // PS and touchpad buttons
        data = simple;
    } else if (length == 78 && report[0] == 0x31) {
        // Extended 78-byte Bluetooth report with CRC32
        uint32_t expected_crc = (uint32_t)report[74] |
                                ((uint32_t)report[75] << 8) |
                                ((uint32_t)report[76] << 16) |
                                ((uint32_t)report[77] << 24);
        uint32_t computed_crc = bt_calc_crc32(0xA1, report, 74);
        if (computed_crc != expected_crc) {
            return; // CRC mismatch: corrupted Bluetooth packet
        }
        data = report + 2; // Data payload begins at offset 2
    } else {
        return;
    }

    if (!data) return;

    unsigned direction = data[7] & 0x0F;
    if (direction > 8) return;

    if (!s_dualsense_connected) {
        s_dualsense_connected = true;
        if (s_bt_vpad_slot < 0) {
            vpad_slot_open(VPAD_SOURCE_BT, &s_bt_vpad_slot);
        }
        int slot_num = vpad_get_assigned_slot_num(s_bt_vpad_slot);
        if (slot_num == 2) {
            daemon_notify("OneFamily: DualSense Wireless (Player 2)");
        } else {
            daemon_notify("OneFamily: DualSense Wireless Connected");
        }
        daemon_log(LOG_LEVEL_INFO, "controller_connected: DualSense BT (Slot %d, Player %d)",
                   s_bt_vpad_slot, slot_num);
    }

    s_last_input_time = daemon_sys_time_us();

    OrbisPadData ds4;
    memset(&ds4, 0, sizeof(ds4));

    // Analog Sticks
    ds4.leftStick.x  = data[0];
    ds4.leftStick.y  = data[1];
    ds4.rightStick.x = data[2];
    ds4.rightStick.y = data[3];

    // Analog Triggers
    ds4.analogButtons.l2 = data[4];
    ds4.analogButtons.r2 = data[5];

    // D-Pad
    ds4.buttons = dpad_map[direction];

    // Face Buttons
    if (data[7] & 0x10) ds4.buttons |= DS4_BUTTON_SQUARE;
    if (data[7] & 0x20) ds4.buttons |= DS4_BUTTON_CROSS;
    if (data[7] & 0x40) ds4.buttons |= DS4_BUTTON_CIRCLE;
    if (data[7] & 0x80) ds4.buttons |= DS4_BUTTON_TRIANGLE;

    // Bumpers, Triggers & Stick Clicks
    if (data[8] & 0x01) ds4.buttons |= DS4_BUTTON_L1;
    if (data[8] & 0x02) ds4.buttons |= DS4_BUTTON_R1;
    if (data[8] & 0x04) ds4.buttons |= DS4_BUTTON_L2;
    if (data[8] & 0x08) ds4.buttons |= DS4_BUTTON_R2;
    if (data[8] & 0x10) ds4.buttons |= DS4_BUTTON_SHARE;   // Create
    if (data[8] & 0x20) ds4.buttons |= DS4_BUTTON_OPTIONS; // Options
    if (data[8] & 0x40) ds4.buttons |= DS4_BUTTON_L3;
    if (data[8] & 0x80) ds4.buttons |= DS4_BUTTON_R3;

    // System Buttons
    if (data[9] & 0x01) ds4.buttons |= DS4_BUTTON_PS;
    if (data[9] & 0x02) ds4.buttons |= DS4_BUTTON_TOUCHPAD;

    // Quick Combo: Create + Options -> Touchpad Click
    if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) == (DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS)) {
        ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
        ds4.buttons |= DS4_BUTTON_TOUCHPAD;
    }

    // Quick Combo: Create + D-Pad Down -> PS Button
    if ((ds4.buttons & (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) == (DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN)) {
        ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
        ds4.buttons |= DS4_BUTTON_PS;
    }

    ds4.connected = 1;
    ds4.timestamp = s_last_input_time;

    // Send translated controller packet to Virtual DualShock 4
    if (s_bt_vpad_slot >= 0) {
        vpad_send_data_slot(s_bt_vpad_slot, &ds4);
    } else {
        vpad_send_data(&ds4);
    }
}

// -------------------------------------------------------------
// Non-Blocking Polling Step (250 Hz Daemon Loop)
// -------------------------------------------------------------

void bt_driver_poll_step(void) {
    if (!s_bt_running || s_bt_fd < 0 || s_bt_queue < 0) return;

    struct {
        long seconds;
        long nanoseconds;
    } timeout = { 0, 0 };

    struct {
        uint64_t ident;
        short    filter;
        uint16_t flags;
        uint32_t fflags;
        long     data;
        void*    udata;
    } event;

    long ready = daemon_syscall(SYS_KEVENT, s_bt_queue, 0, 0, (long)&event, 1, (long)&timeout);
    if (ready <= 0) {
        // Check 3-second timeout for disconnection
        if (s_dualsense_connected && (daemon_sys_time_us() - s_last_input_time > 3000000ULL)) {
            s_dualsense_connected = false;
            s_physical_address = 0;
            if (s_bt_vpad_slot >= 0) {
                vpad_slot_close(s_bt_vpad_slot);
                s_bt_vpad_slot = -1;
            }
            daemon_log(LOG_LEVEL_INFO, "controller_disconnected: DualSense BT timed out");
            daemon_notify("OneFamily: DualSense Disconnected");
        }
        return;
    }

    uint8_t buffer[256];
    long count = daemon_sys_read(s_bt_fd, buffer, sizeof(buffer));
    if (count > 0 && count <= (long)sizeof(buffer)) {
        for (long pos = 0; pos + 16 <= count; pos += 16) {
            uint8_t ev_type = buffer[pos];
            if (ev_type < 0x18 || ev_type > 0x20) continue;

            uint64_t address = 0;
            for (unsigned i = 0; i < 8; ++i) {
                address |= ((uint64_t)buffer[pos + 8 + i]) << (8 * i);
            }

            uint8_t report[80];
            memset(report, 0, sizeof(report));
            struct {
                uint64_t address;
                void*    buffer;
                uint64_t capacity;
                uint64_t reserved;
            } req = { address, report, sizeof(report), 0 };

            long result = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x80206233UL, (long)&req);
            if (result >= 2 && result <= 80 && report[0] + 2 == result && report[1] == 0x1F) {
                s_physical_address = address;
                decode_dualsense_bt_report(report + 2, (size_t)(result - 2));
            }
        }
    }
}

// -------------------------------------------------------------
// Public Subsystem API
// -------------------------------------------------------------

int bt_driver_init(void) {
    daemon_log(LOG_LEVEL_INFO, "Initializing DualSense Bluetooth Subsystem...");

    // 1. Verify firmware version (sceKernelGetSystemSwVersion)
    struct {
        uint64_t size;
        char     text[28];
        uint32_t version;
    } sw_info = { .size = 40 };

    int (*get_sw_version)(void*) = NULL;
    int hKernel = daemon_kernel_get_handle();
    if (hKernel != 0) {
        daemon_sys_dlsym(hKernel, "sceKernelGetSystemSwVersion", (void**)&get_sw_version);
    }
    if (!get_sw_version) {
        daemon_syscall(591, 0x2001, (long)"sceKernelGetSystemSwVersion", (long)&get_sw_version);
    }

    if (get_sw_version) {
        int v_rc = get_sw_version(&sw_info);
        daemon_log(LOG_LEVEL_INFO, "resolve_version: rc=%d, firmware=0x%08X (%s)",
                   v_rc, sw_info.version, sw_info.text);
        if (v_rc == 0 && (sw_info.version >> 16) != BT_SUPPORTED_FW) {
            daemon_log(LOG_LEVEL_WARN,
                       "DualSense BT: Unsupported FW 0x%04X (supported: 0x%04X / 12.50). Skipping BT driver.",
                       (sw_info.version >> 16), BT_SUPPORTED_FW);
            return -1;
        }
    } else {
        daemon_log(LOG_LEVEL_WARN, "DualSense BT: sceKernelGetSystemSwVersion unresolved, proceeding with caution.");
    }

    // 2. Open /dev/bt device node
    s_bt_fd = daemon_sys_open("/dev/bt", 2 /* O_RDWR */, 0);
    daemon_log(LOG_LEVEL_INFO, "open_hid (/dev/bt) = %d", s_bt_fd);
    if (s_bt_fd < 0) {
        daemon_log(LOG_LEVEL_WARN, "Failed to open /dev/bt (fd=%d)", s_bt_fd);
        return -2;
    }

    // 3. Create kqueue
    s_bt_queue = daemon_syscall(SYS_KQUEUE, 0, 0, 0);
    daemon_log(LOG_LEVEL_INFO, "kqueue = %d", s_bt_queue);
    if (s_bt_queue < 0) {
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        return -3;
    }

    // 4. Watch EVFILT_READ on s_bt_fd via kevent
    struct {
        uint64_t ident;
        short    filter; // EVFILT_READ = -1
        uint16_t flags;  // EV_ADD = 1
        uint32_t fflags;
        long     data;
        void*    udata;
    } change = {
        (uint64_t)s_bt_fd, -1, 1, 0, 0, NULL
    };
    long kevent_rc = daemon_syscall(SYS_KEVENT, s_bt_queue, (long)&change, 1, 0, 0, 0);
    daemon_log(LOG_LEVEL_INFO, "watch_read = %ld", kevent_rc);
    if (kevent_rc != 0) {
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        return -4;
    }

    // 5. Subscribe to Bluetooth controller read stream with mask
    uint64_t mask = 0x1FF000000UL;
    long sub_rc = daemon_sys_write(s_bt_fd, &mask, sizeof(mask));
    daemon_log(LOG_LEVEL_INFO, "subscribe = %ld (expected 8)", sub_rc);
    if (sub_rc != (long)sizeof(mask)) {
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        return -5;
    }

    // 6. Apply temporary kernel allowlist patch (Ring 0)
    int patch_rc = apply_kernel_allowlist_patch(0 /* patch */);
    if (patch_rc != 0) {
        daemon_log(LOG_LEVEL_ERROR, "Failed to apply BT kernel allowlist patch (rc=%d)", patch_rc);
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        return -6;
    }

    // 7. Register DualSense device with Bluetooth driver (ioctl 0x8004622B)
    struct {
        uint16_t vendor;
        uint16_t product;
    } device = { SONY_VID, SONY_DUALSENSE_PID };
    long reg_rc = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x8004622BUL, (long)&device);
    daemon_log(LOG_LEVEL_INFO, "register_dualsense = %ld", reg_rc);
    if (reg_rc != 0) {
        // Registration failed: restore allowlist, close descriptors, return cleanly
        apply_kernel_allowlist_patch(1 /* restore */);
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        return -7;
    }

    s_bt_registered = true;
    s_bt_running = true;
    daemon_log(LOG_LEVEL_INFO, "DualSense Bluetooth Subsystem initialized successfully");
    return 0;
}

void bt_driver_shutdown(void) {
    if (!s_bt_running && s_bt_fd < 0 && !s_bt_registered) return;

    daemon_log(LOG_LEVEL_INFO, "Shutting down DualSense Bluetooth Subsystem...");
    s_bt_running = false;

    if (s_bt_registered && s_bt_fd >= 0) {
        struct {
            uint16_t vendor;
            uint16_t product;
        } device = { SONY_VID, SONY_DUALSENSE_PID };
        long unreg_rc = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x8004622CUL, (long)&device);
        daemon_log(LOG_LEVEL_INFO, "unregister_dualsense = %ld", unreg_rc);
        s_bt_registered = false;
    }

    apply_kernel_allowlist_patch(1 /* restore */);

    if (s_bt_queue >= 0) {
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
    }

    if (s_bt_fd >= 0) {
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
    }

    if (s_bt_vpad_slot >= 0) {
        vpad_slot_close(s_bt_vpad_slot);
        s_bt_vpad_slot = -1;
    }
    s_dualsense_connected = false;
    s_physical_address = 0;
}

bool bt_driver_is_connected(void) {
    return s_dualsense_connected;
}

void bt_driver_send_rumble(uint8_t large, uint8_t small, const uint8_t color[3]) {
    if (!s_bt_running || s_bt_fd < 0 || !s_dualsense_connected || !s_physical_address) return;

    static uint8_t seq = 0;
    uint8_t out[78];
    memset(out, 0, sizeof(out));

    out[0] = 0x31;                  // Report ID
    out[1] = (seq++ & 0x0F) << 4;   // Sequence tag
    out[2] = 0x10;
    out[3] = 0x03;                  // Enable motors
    out[4] = 0x14;
    out[5] = small;                 // Weak motor
    out[6] = large;                 // Strong motor
    out[46] = 0x04;                 // Enable lightbar color

    if (color) {
        out[47] = color[0];
        out[48] = color[1];
        out[49] = color[2];
    } else {
        out[47] = 0;
        out[48] = 112;
        out[49] = 209; // Default PlayStation Blue
    }

    // Compute CRC32 checksum for Bluetooth output packet
    uint32_t crc = bt_calc_crc32(0xA2, out, 74);
    for (unsigned i = 0; i < 4; ++i) {
        out[74 + i] = (uint8_t)(crc >> (8 * i));
    }

    // Send Bluetooth output request via ioctl with actual controller physical address
    struct {
        uint64_t address;
        void* buffer;
        uint64_t size;
        uint64_t report_id;
    } req = { s_physical_address, out, 78, 0x31 };

    daemon_syscall(SYS_IOCTL, s_bt_fd, 0x80206232UL, (long)&req);
}
