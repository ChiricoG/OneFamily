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
#include "config.h"
#include <string.h>
#include <stdio.h>

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
static int s_bt_reg_count = 0;
static int s_bt_edge_reg_count = 0;
static bool s_bt_supported = false;
static uint32_t s_current_fw_bcd = 0x1250;

// DualSense IMU Calibration & 3D Motion Orientation State
typedef struct {
    float gyro_scale[3];
    float accel_scale[3];
    float accel_bias[3];
} DualSenseCalibration;

#define BT_INPUT_TIMEOUT_US 60000000ULL
#define BT_PAD_GRACE_US     600000000ULL

typedef struct {
    uint64_t address;         // Bluetooth MAC address (0 = free entry)
    int      vpad_slot;       // virtual pad slot index (0..3, or -1)
    bool     connected;       // reports currently arriving
    uint64_t last_input_time; // timestamp of last report
    uint64_t away_since;      // timestamp when pad went silent during grace period
    uint64_t last_output_time;// timestamp of last sent output report (rumble / keep-alive)
    uint8_t  last_color[3];   // cached current lightbar color for keep-alive reports
    uint32_t reports_seen;
    uint32_t decode_drops;
    uint8_t  last_drop_len;
    uint8_t  last_drop_id;
    uint8_t  rumble_seq;      // sequence tag for output reports
    uint8_t  battery_pct;
    uint8_t  battery_status;
    bool     share_combo_active;
    float    orientation[4];  // x, y, z, w
    uint64_t last_motion_time;
    DualSenseCalibration calibration;
    bool     calibration_valid;
    bool     calibration_requested;
    uint64_t last_calibration_attempt;
} BtDevice;

static BtDevice s_devices[BT_MAX_DEVICES];

static void bt_send_rumble_dev(BtDevice* dev, uint8_t large, uint8_t small, const uint8_t color[3]);

static void bt_device_init_entry(BtDevice *dev) {
    if (!dev) return;
    memset(dev, 0, sizeof(*dev));
    dev->vpad_slot = -1;
    dev->battery_pct = 100;
    dev->orientation[3] = 1.0f;
    dev->last_color[0] = 0;
    dev->last_color[1] = 112;
    dev->last_color[2] = 209; // Default PlayStation Blue
    for (int a = 0; a < 3; ++a) {
        dev->calibration.gyro_scale[a] = (2000.0f * 0.01745329252f) / 32768.0f;
        dev->calibration.accel_scale[a] = 1.0f / 8192.0f;
        dev->calibration.accel_bias[a] = 0.0f;
    }
}

static void bt_devices_reset(void) {
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        bt_device_init_entry(&s_devices[i]);
    }
}

static void bt_dev_release(BtDevice *dev) {
    if (!dev) return;
    if (dev->vpad_slot >= 0) {
        vpad_slot_close(dev->vpad_slot);
    }
    bt_device_init_entry(dev);
}

static void bt_dev_mark_away(BtDevice *dev, uint64_t now) {
    if (!dev) return;
    dev->connected = false;
    dev->away_since = now;

    if (dev->vpad_slot >= 0) {
        OrbisPadData neutral;
        memset(&neutral, 0, sizeof(neutral));
        neutral.leftStick.x = 128;
        neutral.leftStick.y = 128;
        neutral.rightStick.x = 128;
        neutral.rightStick.y = 128;
        vpad_send_data_slot(dev->vpad_slot, &neutral);
    }
}

static BtDevice* bt_dev_find(uint64_t address) {
    if (!address) return NULL;
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].address == address) return &s_devices[i];
    }
    return NULL;
}

static BtDevice* bt_dev_acquire(uint64_t address) {
    BtDevice *dev = bt_dev_find(address);
    if (dev) return dev;
    if (!address) return NULL;

    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].address == 0) {
            bt_device_init_entry(&s_devices[i]);
            s_devices[i].address = address;
            s_devices[i].last_input_time = daemon_sys_time_us();
            daemon_log(LOG_LEVEL_INFO, "DualSense BT: new device 0x%llX (entry %d)",
                       (unsigned long long)address, i);
            return &s_devices[i];
        }
    }

    BtDevice *oldest = NULL;
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].away_since != 0) {
            if (!oldest || s_devices[i].away_since < oldest->away_since) {
                oldest = &s_devices[i];
            }
        }
    }
    if (oldest) {
        daemon_log(LOG_LEVEL_INFO,
                   "DualSense BT: all %d entries in use; reclaiming 0x%llX (Slot %d), away longest, for 0x%llX",
                   BT_MAX_DEVICES, (unsigned long long)oldest->address, oldest->vpad_slot,
                   (unsigned long long)address);
        bt_dev_release(oldest);
        return bt_dev_acquire(address);
    }

    daemon_log(LOG_LEVEL_WARN,
               "DualSense BT: all %d device entries in use and connected, ignoring address 0x%llX",
               BT_MAX_DEVICES, (unsigned long long)address);
    return NULL;
}

static long bt_register_pair(uint16_t vid, uint16_t pid, const char *what) {
    struct {
        uint16_t vendor;
        uint16_t product;
    } device = { vid, pid };
    long rc = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x8004622BUL, (long)&device);
    daemon_log(LOG_LEVEL_INFO, "register %s (%04X:%04X) = %ld", what, vid, pid, rc);
    return rc;
}

static long bt_unregister_pair(uint16_t vid, uint16_t pid) {
    struct {
        uint16_t vendor;
        uint16_t product;
    } device = { vid, pid };
    long rc = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x8004622CUL, (long)&device);
    daemon_log(LOG_LEVEL_INFO, "unregister (%04X:%04X) = %ld", vid, pid, rc);
    return rc;
}

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
// Kernel Allowlist Patching (Multi-Firmware Hybrid: Verified Table + Safe AOB SigScan)
// -------------------------------------------------------------

struct PatchRequest {
    int restore;
    uint32_t fw_version;             // BCD (e.g. 0x1250, 0x1100, 0x0900, 0x1352)
    int is_dynamic;                  // Output: 0 for static table, 1 for dynamic resolution
    int detail_code;                 // Output: internal diagnostic code
    uint32_t pair_count;             // Output: number of pairs patched (5 or 6)
    uint32_t n_vid_found;
    uint32_t n_pid_found;
    uint32_t discovered_vids[8];
    uint32_t discovered_pids[8];
    uint32_t discovered_offsets[12]; // Output: Discovered offsets for future static tables (up to 6 pairs)
    uint32_t discovered_lock;        // Output: Discovered lock offset
    uint32_t discovered_unlock;      // Output: Discovered unlock offset
};

struct PatchArgs {
    void* function;
    struct PatchRequest* request;
};

// -------------------------------------------------------------
// Dynamic AOB Scanner Cache
// -------------------------------------------------------------
static uint32_t s_dyn_offsets[12];
static uint32_t s_dyn_offsets_count = 0;
static bool     s_dyn_offsets_valid = false;

// Helper: Check if p points to a CMP immediate operand
static inline bool aob_is_cmp_imm(const uint8_t* p) {
    uint8_t b1 = p[-1];
    uint8_t b2 = p[-2];
    uint8_t b3 = p[-3];
    // 3D id (CMP eax, id)
    if (b1 == 0x3D) return true;
    // 81 /7 id (CMP r/m32, id) -> ModR/M byte has reg field == 7 ((b >> 3) & 7 == 7)
    if (b2 == 0x81 && ((b1 >> 3) & 7) == 7) return true;
    if (b3 == 0x81 && ((b2 >> 3) & 7) == 7) return true;
    return false;
}

#define AOB_SAFE_SCAN_START 0x004E0000UL
#define AOB_SAFE_SCAN_END   0x00660000UL

static int aob_find_allowlist_offsets(uint64_t base, uint32_t out_offsets[12], uint32_t* out_count, uint32_t* out_lock, uint32_t* out_unlock, struct PatchRequest* req) {
    uint32_t raw_vids[12];
    uint32_t vids[8];
    uint32_t pids[8];
    int n_raw_vid = 0, n_vid = 0, n_pid = 0;

    // 1. VID Phase: Scan safe kernel window for CMP imm32 == 0x044F (Thrustmaster VID)
    for (uint64_t a = base + AOB_SAFE_SCAN_START; a + 4 <= base + AOB_SAFE_SCAN_END; ++a) {
        if (*(const volatile uint32_t*)a == 0x0000044FUL) {
            if (aob_is_cmp_imm((const uint8_t*)a)) {
                if (n_raw_vid < 12) raw_vids[n_raw_vid++] = (uint32_t)(a - base);
            }
        }
    }

    // Locate the 5-VID bthid allowlist cluster (spanning ~0x4A65 bytes, <= 0x8000 bytes).
    // On FW 12.00..13.52 an isolated 0x044F comparison exists ~85 KB earlier at 0x59xxxx;
    // selecting the compact 5-VID cluster skips any isolated pre/post false positives.
    int cluster_start = -1;
    for (int s = 0; s + 4 < n_raw_vid; ++s) {
        uint32_t span = raw_vids[s + 4] - raw_vids[s];
        uint32_t d01  = raw_vids[s + 1] - raw_vids[s];
        if (span <= 0x8000UL && d01 >= 0x400UL && d01 <= 0x1000UL) {
            cluster_start = s;
            break;
        }
    }

    if (cluster_start >= 0) {
        for (int i = 0; i < 5; ++i) {
            vids[i] = raw_vids[cluster_start + i];
        }
        n_vid = 5;
    } else {
        for (int i = 0; i < n_raw_vid && i < 8; ++i) {
            vids[i] = raw_vids[i];
        }
        n_vid = (n_raw_vid < 8) ? n_raw_vid : 8;
    }

    if (req) {
        req->n_vid_found = n_vid;
        for (int i = 0; i < n_vid && i < 8; ++i) req->discovered_vids[i] = vids[i];
    }

    // Accept 5 or 6 VIDs
    if (n_vid < 5 || n_vid > 6) {
        return 0x310 + n_vid; // Unexpected VID count
    }

    // 2. PID Phase: Search inside the cluster window around the detected VIDs.
    // In Orbis OS bthid, all VIDs and PIDs reside within a compact span.
    uint64_t pid_win_start = (base + vids[0] > 0x800) ? (base + vids[0] - 0x800) : (base + vids[0]);
    uint64_t pid_win_end   = base + vids[n_vid - 1] + 0x8000;
    if (pid_win_end > base + AOB_SAFE_SCAN_END) pid_win_end = base + AOB_SAFE_SCAN_END;

    for (uint64_t a = pid_win_start; a + 4 <= pid_win_end; ++a) {
        if (*(const volatile uint32_t*)a == 0x00001000UL) {
            if (aob_is_cmp_imm((const uint8_t*)a)) {
                if (n_pid < 8) pids[n_pid++] = (uint32_t)(a - base);
            }
        }
    }

    if (req) {
        req->n_pid_found = n_pid;
        for (int i = 0; i < n_pid && i < 8; ++i) req->discovered_pids[i] = pids[i];
    }

    if (n_pid < 5) {
        return 0x320 + n_pid; // Expected at least 5 Thrustmaster PIDs
    }

    // 3. Structural Delta Matching: assign each VID to its architectural companion PID
    // Sony compiler produces invariant deltas across PS4 kernels:
    // Pair 0: +0x30, Pair 1: +0x33, Pair 2: +0xBF2, Pair 3: +0x4E1, Pair 4: +0x4F3
    static const uint32_t s_arch_deltas[5] = { 0x30, 0x33, 0xBF2, 0x4E1, 0x4F3 };
    bool pid_used[8] = {false};
    uint32_t pairs = 0;

    for (int i = 0; i < n_vid && pairs < 5; ++i) {
        uint32_t v = vids[i];
        int best_idx = -1;

        // Pass A: Exact architectural delta match (within +- 16 bytes for padding)
        if (i < 5) {
            uint32_t target = v + s_arch_deltas[i];
            for (int j = 0; j < n_pid; ++j) {
                if (pid_used[j]) continue;
                uint32_t p = pids[j];
                uint32_t diff = (p > target) ? (p - target) : (target - p);
                if (diff <= 16) {
                    best_idx = j;
                    break;
                }
            }
        }

        // Pass B Fallback: Prefer forward PID within 4096 bytes
        if (best_idx < 0) {
            uint32_t best_dist = 0xFFFFFFFFU;
            for (int j = 0; j < n_pid; ++j) {
                if (pid_used[j]) continue;
                uint32_t p = pids[j];
                if (p > v && (p - v) < 4096) {
                    uint32_t dist = p - v;
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_idx = j;
                    }
                }
            }
        }

        // Pass C Fallback: Closest available overall
        if (best_idx < 0) {
            uint32_t best_dist = 0xFFFFFFFFU;
            for (int j = 0; j < n_pid; ++j) {
                if (pid_used[j]) continue;
                uint32_t p = pids[j];
                uint32_t dist = (p > v) ? (p - v) : (v - p);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_idx = j;
                }
            }
        }

        if (best_idx >= 0) {
            pid_used[best_idx] = true;
            out_offsets[2 * pairs]     = v;
            out_offsets[2 * pairs + 1] = pids[best_idx];
            ++pairs;
        }
    }

    if (pairs < 5) {
        return 0x330 + pairs;
    }

    *out_count = pairs;

    // 4. Resolve lock / unlock mutexes relative to the first VID anchor (12.50 delta = -0xA226 and -0xA1A6)
    int64_t delta = (int64_t)((base + vids[0]) - (base + 0x5B0DA6));
    uint64_t lock_cand   = (base + 0x5A6B80) + delta;
    uint64_t unlock_cand = (base + 0x5A6C00) + delta;

    if ((*(const volatile uint32_t*)lock_cand & 0x00FFFFFF) == 0x00E58948 ||
        *(const volatile uint8_t*)lock_cand == 0x55) {
        *out_lock   = (uint32_t)(lock_cand - base);
        *out_unlock = (uint32_t)(unlock_cand - base);
    } else {
        *out_lock   = 0;
        *out_unlock = 0;
    }

    return 0;
}

/**
 * Ring 0 Kernel Execution callback (invoked via syscall 11 / kexec)
 * Hybrid Multi-Firmware: Verified Table (12.50) + Safe Contextual AOB Scanner (Other FW)
 */
static int patch_allowlist(void* thread, struct PatchArgs* args) {
    (void)thread;
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000082));
    uint64_t base = (((uint64_t)high << 32) | low) - 0x1C0;

    if ((base >> 48) != 0xFFFF || (base & 0x3FFF) || !args || !args->request) {
        return 22; // EINVAL
    }

    struct PatchRequest* req = args->request;
    int restore = req->restore;
    if (restore != 0 && restore != 1) return 22;

    // Defense-in-depth: Never touch kernel memory or execute AOB scans on firmwares older than 9.00
    if (req->fw_version < 0x0900) {
        req->detail_code = 0x0505;
        return 22; // EINVAL
    }

    uint32_t lock_off = 0;
    uint32_t unlock_off = 0;
    uint32_t active_offsets[12];
    uint32_t total_offsets = 10;

    // Path 1: Verified Static Table (FW 12.50)
    if (req->fw_version == 0x1250) {
        if (*(volatile uint64_t*)(base + 0x5B6D00) != 0x56415741E5894855UL) {
            req->detail_code = 0x1250;
            return 22;
        }
        lock_off = 0x5A6B80;
        unlock_off = 0x5A6C00;
        const uint32_t static_1250[10] = {
            0x5B0DA6, 0x5B0DD6, 0x5B1616, 0x5B1649, 0x5B53D8,
            0x5B5FCA, 0x5B578D, 0x5B5C6E, 0x5B580B, 0x5B5CFE
        };
        for (int i = 0; i < 10; ++i) active_offsets[i] = static_1250[i];
        total_offsets = 10;
        req->is_dynamic = 0;
        req->pair_count = 5;
    }
    // Path 2: Verified Static Table (FW 11.00)
    else if (req->fw_version == 0x1100) {
        if (*(volatile uint32_t*)(base + 0x5A4206) != 0x0000044FUL) {
            req->detail_code = 0x1100;
            return 22;
        }
        lock_off = 0;
        unlock_off = 0;
        const uint32_t static_1100[10] = {
            0x5A4206, 0x5A4236, // Pair 0 (diff +0x30)
            0x5A4A76, 0x5A4AA9, // Pair 1 (diff +0x33)
            0x5A8838, 0x5A942A, // Pair 2 (diff +0xBF2)
            0x5A8BED, 0x5A90CE, // Pair 3 (diff +0x4E1)
            0x5A8C6B, 0x5A915E  // Pair 4 (diff +0x4F3)
        };
        for (int i = 0; i < 10; ++i) active_offsets[i] = static_1100[i];
        total_offsets = 10;
        req->is_dynamic = 0;
        req->pair_count = 5;
    }
    // Path 3: Verified Static Table (FW 9.00)
    else if (req->fw_version == 0x0900) {
        if (*(volatile uint32_t*)(base + 0x5A5E56) != 0x0000044FUL) {
            req->detail_code = 0x0900;
            return 22;
        }
        lock_off = 0;
        unlock_off = 0;
        const uint32_t static_0900[10] = {
            0x5A5E56, 0x5A5E86, // Pair 0 (diff +0x30)
            0x5A66C6, 0x5A66F9, // Pair 1 (diff +0x33)
            0x5AA498, 0x5AB08A, // Pair 2 (diff +0xBF2)
            0x5AA84D, 0x5AAD2E, // Pair 3 (diff +0x4E1)
            0x5AA8CB, 0x5AADBE  // Pair 4 (diff +0x4F3)
        };
        for (int i = 0; i < 10; ++i) active_offsets[i] = static_0900[i];
        total_offsets = 10;
        req->is_dynamic = 0;
        req->pair_count = 5;
    }
    // Path 4: Verified Static Table (FW 13.52)
    else if (req->fw_version == 0x1352) {
        if (*(volatile uint32_t*)(base + 0x5B1616) != 0x0000044FUL) {
            req->detail_code = 0x1352;
            return 22;
        }
        lock_off = 0;
        unlock_off = 0;
        const uint32_t static_1352[12] = {
            0x59C869, 0x59C8C0,
            0x5B1616, 0x5B1646,
            0x5B1E86, 0x5B1EB9,
            0x5B5C48, 0x5B64DE,
            0x5B5FFD, 0x5B656E,
            0x5B607B, 0x5B348A
        };
        for (int i = 0; i < 12; ++i) active_offsets[i] = static_1352[i];
        total_offsets = 12;
        req->is_dynamic = 0;
        req->pair_count = 6;
    }
    // Path 3: Safe Contextual AOB Scanner for unlisted firmwares (11.00, 9.00, 10.xx, etc.)
    else {
        // Path 2: Safe Contextual AOB Scanner for unlisted firmwares (13.52, 9.00, etc.)
        if (restore && s_dyn_offsets_valid) {
            total_offsets = s_dyn_offsets_count;
            for (uint32_t i = 0; i < total_offsets; ++i) active_offsets[i] = s_dyn_offsets[i];
            req->is_dynamic = 1;
            req->pair_count = total_offsets / 2;
        } else {
            uint32_t pair_count = 0;
            int aob_rc = aob_find_allowlist_offsets(base, active_offsets, &pair_count, &lock_off, &unlock_off, req);
            req->detail_code = aob_rc;
            if (aob_rc != 0) {
                return 45; // Safe abort: no memory touched, fallback cleanly to USB
            }
            total_offsets = pair_count * 2;
            for (uint32_t i = 0; i < total_offsets; ++i) s_dyn_offsets[i] = active_offsets[i];
            s_dyn_offsets_count = total_offsets;
            s_dyn_offsets_valid = true;
            req->is_dynamic = 1;
            req->pair_count = pair_count;
        }
    }

    for (uint32_t i = 0; i < total_offsets; ++i) req->discovered_offsets[i] = active_offsets[i];
    req->discovered_lock = lock_off;
    req->discovered_unlock = unlock_off;

    int (*lock)(void) = lock_off ? (void*)(base + lock_off) : NULL;
    int (*unlock)(int) = unlock_off ? (void*)(base + unlock_off) : NULL;

    if (lock) {
        int rc = lock();
        if (rc) return rc;
    }

    unsigned original_count = 0, patched_count = 0;
    for (unsigned i = 0; i < total_offsets; ++i) {
        uint32_t old_value = (i & 1) ? 0x1000 : 0x044F;
        uint32_t new_value = (i & 1) ? 0x0CE6 : 0x054C;
        uint32_t current = *(volatile uint32_t*)(base + active_offsets[i]);
        if (current == old_value) ++original_count;
        else if (current == new_value) ++patched_count;
        else {
            if (unlock) unlock(0);
            return 0x100 + (int)i;
        }
    }

    if (original_count && patched_count) {
        if (unlock) unlock(0);
        return 0x200 + (int)original_count;
    }

    if ((!restore && patched_count == total_offsets) || (restore && original_count == total_offsets)) {
        if (unlock) unlock(0);
        return 0;
    }

    uint64_t flags, cr0;
    __asm__ volatile("pushfq; popq %0; cli; mov %%cr0, %1" : "=r"(flags), "=r"(cr0) :: "memory");
    uint64_t writable = cr0 & ~(1UL << 16);
    __asm__ volatile("mov %0, %%cr0" :: "r"(writable) : "memory");

    for (unsigned i = 0; i < total_offsets; ++i) {
        uint32_t old_value = (i & 1) ? 0x1000 : 0x044F;
        uint32_t new_value = (i & 1) ? 0x0CE6 : 0x054C;
        *(volatile uint32_t*)(base + active_offsets[i]) = restore ? old_value : new_value;
    }

    __asm__ volatile("mov %0, %%cr0; pushq %1; popfq" :: "r"(cr0), "r"(flags) : "memory", "cc");
    if (unlock) unlock(0);
    return 0;
}

static int apply_kernel_allowlist_patch(int restore, uint32_t fw_version) {
    struct PatchRequest req;
    memset(&req, 0, sizeof(req));
    req.restore = restore;
    req.fw_version = fw_version;

    long ret = daemon_syscall(SYS_KEXEC, (long)patch_allowlist, (long)&req, 0);
    daemon_log(LOG_LEVEL_INFO, "DualSense BT Kernel Allowlist Patch (restore=%d, fw=0x%04X, dynamic=%d, detail=0x%X) = %ld",
               restore, fw_version, req.is_dynamic, req.detail_code, ret);

    if (req.n_vid_found > 0) {
        daemon_log(LOG_LEVEL_INFO, "[FW 0x%04X] AOB Scan detected %u VIDs and %u PIDs:", fw_version, req.n_vid_found, req.n_pid_found);
        for (uint32_t i = 0; i < req.n_vid_found && i < 8; ++i) {
            daemon_log(LOG_LEVEL_INFO, "  VID[%u] = 0x%06X", i, req.discovered_vids[i]);
        }
        for (uint32_t i = 0; i < req.n_pid_found && i < 8; ++i) {
            daemon_log(LOG_LEVEL_INFO, "  PID[%u] = 0x%06X", i, req.discovered_pids[i]);
        }
    }

    if (ret == 0 && req.is_dynamic && !restore) {
        daemon_log(LOG_LEVEL_INFO, "[FW 0x%04X] Successfully paired and patched %u Pairs (%u Offsets) for DualSense Wireless Pairing:",
                   fw_version, req.pair_count, req.pair_count * 2);
        for (uint32_t i = 0; i < req.pair_count; ++i) {
            daemon_log(LOG_LEVEL_INFO, "  Pair[%u]: VID=0x%06X, PID=0x%06X",
                       i, req.discovered_offsets[2 * i], req.discovered_offsets[2 * i + 1]);
        }
        daemon_log(LOG_LEVEL_INFO, "  Lock/Unlock: lock=0x%06X, unlock=0x%06X",
                   req.discovered_lock, req.discovered_unlock);
    }
    return (int)ret;
}

// -------------------------------------------------------------
// Helper Routines for IMU Calibration & Motion Processing
// -------------------------------------------------------------

static inline int16_t read_signed_le16(const uint8_t *data) {
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static int dualsense_calibration_bluetooth(const uint8_t *report, size_t length, DualSenseCalibration *calibration) {
    if (!report || !calibration || length != 41 || report[0] != 5) return -1;
    uint32_t crc = bt_calc_crc32(0xA3, report, 37);
    uint32_t expected = (uint32_t)report[37] | ((uint32_t)report[38] << 8) |
                        ((uint32_t)report[39] << 16) | ((uint32_t)report[40] << 24);
    if (crc != expected) return -2;

    int speed = (int)read_signed_le16(report + 19) + (int)read_signed_le16(report + 21);
    if (speed <= 0 || speed > 10000) return -3;

    DualSenseCalibration result;
    for (unsigned i = 0; i < 3; ++i) {
        int bias = read_signed_le16(report + 1 + i * 2);
        int plus = (int)read_signed_le16(report + 7 + i * 4) - bias;
        int minus = (int)read_signed_le16(report + 9 + i * 4) - bias;
        int denominator = (plus < 0 ? -plus : plus) + (minus < 0 ? -minus : minus);
        int accel_plus = read_signed_le16(report + 23 + i * 4);
        int accel_minus = read_signed_le16(report + 25 + i * 4);
        int range = accel_plus - accel_minus;
        if (!denominator || range <= 0) return -3;
        result.gyro_scale[i] = (float)speed * 0.01745329252f / (float)denominator;
        result.accel_scale[i] = 2.0f / (float)range;
        result.accel_bias[i] = (float)accel_plus - (float)range / 2.0f;
    }
    *calibration = result;
    return 0;
}

static inline float root_positive(float value) {
    if (value <= 0.0f) return 0.0f;
    float result = value > 1.0f ? value : 1.0f;
    for (unsigned i = 0; i < 12; ++i) {
        result = (result + value / result) * 0.5f;
    }
    return result;
}

static void bt_write_status_file(void) {
    daemon_sys_mkdir("/data", 0777);
    daemon_sys_mkdir("/data/onefamily", 0777);
    int fd = daemon_sys_open("/data/onefamily/onefamily.status", 0x0001 | 0x0200 | 0x0400, 0666);
    if (fd >= 0) {
        char buf[1024];
        int count = bt_driver_connected_count();
        int offset = snprintf(buf, sizeof(buf),
            "connected = %d\n"
            "type = dualsense_bt\n"
            "vid = 0x054C\n"
            "pid = 0x0CE6\n"
            "name = DualSense Wireless\n"
            "nbuttons = 16\n"
            "count = %d\n",
            count > 0 ? 1 : 0, count);

        for (int i = 0; i < BT_MAX_DEVICES && offset < (int)sizeof(buf) - 120; ++i) {
            if (s_devices[i].connected) {
                int slot = vpad_get_assigned_slot_num(s_devices[i].vpad_slot);
                offset += snprintf(buf + offset, sizeof(buf) - offset,
                    "dev_%d_slot = %d\n"
                    "dev_%d_mac = %llX\n"
                    "dev_%d_battery = %d\n"
                    "dev_%d_charging = %d\n",
                    i, slot,
                    i, (unsigned long long)s_devices[i].address,
                    i, s_devices[i].battery_pct,
                    i, s_devices[i].battery_status == 1 ? 1 : 0);
            }
        }
        daemon_sys_write(fd, buf, offset);
        daemon_sys_close(fd);
    }
}

static void extended_sample(BtDevice* dev, OrbisPadData* ds4, const uint8_t* report, float dt) {
    if (!dev) return;
    const uint8_t* data = report + 2;

    // 1. Touchpad Multitouch (2 contact points)
    // Points start at data[32] (report[34])
    memset(&ds4->touch, 0, sizeof(ds4->touch));
    uint8_t contacts = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const uint8_t* point = data + 32 + (i * 4);
        bool active = !(point[0] & 0x80);
        if (!active) continue;

        uint8_t id = point[0] & 0x7F;
        uint16_t raw_x = (uint16_t)point[1] | (((uint16_t)(point[2] & 0x0F)) << 8);
        uint16_t raw_y = (uint16_t)(point[2] >> 4) | (((uint16_t)point[3]) << 4);

        uint16_t x = (raw_x < 1920) ? raw_x : 1919;
        // DualSense resolution is 1920x1080. DualShock 4 is 1920x942.
        uint16_t y = (raw_y < 1080) ? (uint16_t)(((uint32_t)raw_y * 942ULL) / 1080ULL) : 941;

        ds4->touch.touch[contacts].x = x;
        ds4->touch.touch[contacts].y = y;
        ds4->touch.touch[contacts].finger = id;
        contacts++;
    }
    ds4->touch.fingers = contacts;

    // 2. Gyroscope and Accelerometer (16-bit signed LE)
    int16_t raw_gyro[3];
    int16_t raw_accel[3];
    for (unsigned i = 0; i < 3; ++i) {
        raw_gyro[i]  = (int16_t)((uint16_t)data[15 + i * 2] | ((uint16_t)data[16 + i * 2] << 8));
        raw_accel[i] = (int16_t)((uint16_t)data[21 + i * 2] | ((uint16_t)data[22 + i * 2] << 8));
    }

    float omega[3];
    // Angular velocity in rad/s
    ds4->vel.x = (float)raw_gyro[0] * dev->calibration.gyro_scale[0];
    ds4->vel.y = (float)raw_gyro[1] * dev->calibration.gyro_scale[1];
    ds4->vel.z = (float)raw_gyro[2] * dev->calibration.gyro_scale[2];
    omega[0] = ds4->vel.x;
    omega[1] = ds4->vel.y;
    omega[2] = ds4->vel.z;

    // Acceleration in g
    ds4->acell.x = ((float)raw_accel[0] - dev->calibration.accel_bias[0]) * dev->calibration.accel_scale[0];
    ds4->acell.y = ((float)raw_accel[1] - dev->calibration.accel_bias[1]) * dev->calibration.accel_scale[1];
    ds4->acell.z = ((float)raw_accel[2] - dev->calibration.accel_bias[2]) * dev->calibration.accel_scale[2];

    // 3. 3D Orientation Quaternion Integration (Diram1x complementary filter)
    if (dt > 0.0f && dt <= 0.1f) {
        float x = dev->orientation[0];
        float y = dev->orientation[1];
        float z = dev->orientation[2];
        float w = dev->orientation[3];

        float magnitude = root_positive(ds4->acell.x * ds4->acell.x +
                                        ds4->acell.y * ds4->acell.y +
                                        ds4->acell.z * ds4->acell.z);
        if (magnitude > 0.8f && magnitude < 1.2f) {
            float ax = ds4->acell.x / magnitude;
            float ay = ds4->acell.y / magnitude;
            float az = ds4->acell.z / magnitude;
            float gx = 2.0f * (x * y + w * z);
            float gy = 1.0f - 2.0f * (x * x + z * z);
            float gz = 2.0f * (y * z - w * x);
            omega[0] += 2.0f * (ay * gz - az * gy);
            omega[1] += 2.0f * (az * gx - ax * gz);
            omega[2] += 2.0f * (ax * gy - ay * gx);
        }

        float half = dt * 0.5f;
        dev->orientation[0] += ( w * omega[0] + y * omega[2] - z * omega[1]) * half;
        dev->orientation[1] += ( w * omega[1] + z * omega[0] - x * omega[2]) * half;
        dev->orientation[2] += ( w * omega[2] + x * omega[1] - y * omega[0]) * half;
        dev->orientation[3] += (-x * omega[0] - y * omega[1] - z * omega[2]) * half;

        float norm = dev->orientation[0] * dev->orientation[0] +
                     dev->orientation[1] * dev->orientation[1] +
                     dev->orientation[2] * dev->orientation[2] +
                     dev->orientation[3] * dev->orientation[3];
        norm = root_positive(norm);
        if (norm > 0.0001f) {
            dev->orientation[0] /= norm;
            dev->orientation[1] /= norm;
            dev->orientation[2] /= norm;
            dev->orientation[3] /= norm;
        }
    }

    ds4->quat.x = dev->orientation[0];
    ds4->quat.y = dev->orientation[1];
    ds4->quat.z = dev->orientation[2];
    ds4->quat.w = dev->orientation[3];
}

// -------------------------------------------------------------
// Bluetooth Report 0x31 / 0x01 Decoder
// -------------------------------------------------------------

static void decode_dualsense_bt_report(BtDevice* dev, const uint8_t* report, size_t length) {
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

    if (!dev || !report) return;

    uint8_t simple[10] = {0};
    const uint8_t* data = NULL;

    dev->last_drop_len = (uint8_t)(length > 255 ? 255 : length);
    dev->last_drop_id = report[0];

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
            dev->decode_drops++;
            return; // CRC mismatch: corrupted Bluetooth packet
        }
        data = report + 2; // Data payload begins at offset 2
    } else {
        dev->decode_drops++;
        return;
    }

    if (!data) return;

    unsigned direction = data[7] & 0x0F;
    if (direction > 8) {
        dev->decode_drops++;
        return;
    }

    if (!dev->connected) {
        if (dev->vpad_slot >= 0) {
            // Reconnecting within grace period!
            uint64_t now_reconn = daemon_sys_time_us();
            bool long_away = (dev->away_since != 0 && (now_reconn - dev->away_since) > 120000000ULL);
            dev->connected = true;
            dev->away_since = 0;
            int slot_num = vpad_get_assigned_slot_num(dev->vpad_slot);
            if (long_away) {
                daemon_notify("OneFamily: DualSense (Player %d) Reconnected", slot_num);
            }
            daemon_log(LOG_LEVEL_INFO, "controller_reconnected: DualSense BT 0x%llX (Slot %d, Player %d)",
                       (unsigned long long)dev->address, dev->vpad_slot, slot_num);
            uint8_t init_color[3] = { 0, 112, 209 };
            bt_send_rumble_dev(dev, 0, 0, init_color);
        } else {
            // New device connection
            if (vpad_slot_open_dev(VPAD_SOURCE_BT, dev->address, &dev->vpad_slot) != 0 || dev->vpad_slot < 0) {
                daemon_log(LOG_LEVEL_WARN, "DualSense BT 0x%llX: no virtual pad slot available",
                           (unsigned long long)dev->address);
                dev->vpad_slot = -1;
                return;
            }
            dev->connected = true;
            dev->away_since = 0;
            int slot_num = vpad_get_assigned_slot_num(dev->vpad_slot);
            daemon_notify("OneFamily: DualSense Wireless (Player %d)", slot_num);
            daemon_log(LOG_LEVEL_INFO, "controller_connected: DualSense BT 0x%llX (Slot %d, Player %d)",
                       (unsigned long long)dev->address, dev->vpad_slot, slot_num);
            uint8_t init_color[3] = { 0, 112, 209 };
            bt_send_rumble_dev(dev, 0, 0, init_color);
        }
        bt_write_status_file();
    }

    dev->last_input_time = daemon_sys_time_us();

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

    // DualSense Mute Button (bit 2 of data[9])
    if (g_daemon_config.dualsense_mute_action == 1 && (data[9] & 0x04)) {
        ds4.buttons |= DS4_BUTTON_TOUCHPAD;
    }

    // Universal Shortcuts & De-ghosting for DualSense BT
    bool share_down = (ds4.buttons & DS4_BUTTON_SHARE) != 0;

    if (share_down) {
        bool combo_matched = false;

        // 1. Create + Options -> Touchpad Click
        if (ds4.buttons & DS4_BUTTON_OPTIONS) {
            ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_OPTIONS);
            ds4.buttons |= DS4_BUTTON_TOUCHPAD;
            combo_matched = true;
        }
        // 2. Create + D-Pad Up -> Touchpad Click
        else if (ds4.buttons & DS4_BUTTON_DPAD_UP) {
            ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_UP);
            ds4.buttons |= DS4_BUTTON_TOUCHPAD;
            combo_matched = true;
        }
        // 3. Create + D-Pad Down -> PS Button
        else if (ds4.buttons & DS4_BUTTON_DPAD_DOWN) {
            ds4.buttons &= ~(DS4_BUTTON_SHARE | DS4_BUTTON_DPAD_DOWN);
            ds4.buttons |= DS4_BUTTON_PS;
            combo_matched = true;
        }

        if (combo_matched) {
            dev->share_combo_active = true;
        } else if (dev->share_combo_active) {
            // Asynchronous release de-ghosting: suppress Share/Create
            ds4.buttons &= ~DS4_BUTTON_SHARE;
        }
    } else {
        dev->share_combo_active = false;
    }

    ds4.connected = 1;
    ds4.timestamp = dev->last_input_time;

    if (length == 78) {
        // Battery status & level from report 0x31 (data[52])
        uint8_t raw_battery = data[52] & 0x0F;
        uint8_t battery_status = (data[52] >> 4) & 0x0F;
        uint8_t battery_pct = (battery_status == 2) ? 100 : (raw_battery < 10 ? (raw_battery * 10 + 5) : 100);

        dev->battery_pct = battery_pct;
        dev->battery_status = battery_status;

        static uint64_t s_last_battery_update = 0;
        if (s_last_battery_update == 0 || dev->last_input_time - s_last_battery_update >= 30000000ULL) { // Immediate first write, then every 30s
            s_last_battery_update = dev->last_input_time;
            bt_write_status_file();
            daemon_log(LOG_LEVEL_INFO, "DualSense BT 0x%llX Battery: %d%% (status=%s)",
                       (unsigned long long)dev->address,
                       battery_pct,
                       battery_status == 1 ? "Charging" : (battery_status == 2 ? "Full" : "Discharging"));
        }

        float dt = 0.0f;
        if (dev->last_motion_time > 0 && dev->last_input_time > dev->last_motion_time) {
            dt = (float)(dev->last_input_time - dev->last_motion_time) / 1000000.0f;
        }
        dev->last_motion_time = dev->last_input_time;

        extended_sample(dev, &ds4, report, dt);
    } else {
        ds4.quat.w = 1.0f;
    }

    // Send translated controller packet to Virtual DualShock 4
    if (dev->vpad_slot >= 0) {
        vpad_send_data_slot(dev->vpad_slot, &ds4);
    }
}

static void bt_check_timeouts(void) {
    uint64_t now = daemon_sys_time_us();

    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        BtDevice *dev = &s_devices[i];
        if (!dev->address) continue;

        // If already away, check if grace window expired
        if (dev->away_since != 0) {
            if (now - dev->away_since <= BT_PAD_GRACE_US) continue;

            int player = dev->vpad_slot >= 0 ? vpad_get_assigned_slot_num(dev->vpad_slot) : 0;
            daemon_log(LOG_LEVEL_INFO,
                       "controller_disconnected: DualSense BT 0x%llX grace window expired, releasing Slot %d (Player %d)",
                       (unsigned long long)dev->address, dev->vpad_slot, player);
            bt_dev_release(dev);
            daemon_notify("OneFamily: DualSense (Player %d) Disconnected", player);
            bt_write_status_file();
            continue;
        }

        if (now - dev->last_input_time <= BT_INPUT_TIMEOUT_US) continue;

        if (!dev->connected) {
            daemon_log(LOG_LEVEL_INFO, "DualSense BT: dropping silent device 0x%llX (entry %d)",
                       (unsigned long long)dev->address, i);
            bt_dev_release(dev);
            continue;
        }

        // Just went quiet: mark away, hold pad/account, send neutral sample
        int player = dev->vpad_slot >= 0 ? vpad_get_assigned_slot_num(dev->vpad_slot) : 0;
        daemon_log(LOG_LEVEL_INFO,
                   "controller_away: DualSense BT 0x%llX quiet %llu ms, holding Slot %d (Player %d)",
                   (unsigned long long)dev->address,
                   (unsigned long long)((now - dev->last_input_time) / 1000ULL),
                   dev->vpad_slot, player);
        bt_dev_mark_away(dev, now);
        bt_write_status_file();
    }
}

// -------------------------------------------------------------
// Non-Blocking Polling Step (250 Hz Daemon Loop)
// -------------------------------------------------------------

void bt_driver_poll_step(void) {
    if (!s_bt_running || s_bt_fd < 0 || s_bt_queue < 0) return;

    bt_check_timeouts();

    // Feature Report 0x05 request for factory IMU calibration
    uint64_t now_us = daemon_sys_time_us();
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        BtDevice *dev = &s_devices[i];
        if (dev->connected && dev->address && !dev->calibration_requested &&
            (now_us - dev->last_calibration_attempt > 1000000ULL)) {
            dev->last_calibration_attempt = now_us;
            struct { uint64_t address; uint8_t report_id; uint8_t reserved[7]; } feature = { dev->address, 5, {0} };
            long rc = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x8010622FUL, (long)&feature);
            if (rc == 0) {
                dev->calibration_requested = true;
                daemon_log(LOG_LEVEL_INFO, "DualSense BT (0x%llX): requested IMU factory calibration",
                           (unsigned long long)dev->address);
            }
        }
    }

    // Periodic Bluetooth output report keep-alive heartbeat (every 2.5 seconds)
    // Prevents DualSense internal hardware auto-sleep watchdog from turning off controller during idle cutscenes
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        BtDevice *dev = &s_devices[i];
        if (dev->connected && dev->address && (now_us - dev->last_output_time >= 2500000ULL)) {
            bt_send_rumble_dev(dev, 0, 0, dev->last_color);
        }
    }

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

    long ready = daemon_sys_call6(SYS_KEVENT, s_bt_queue, 0, 0, (long)&event, 1, (long)&timeout);
    if (ready <= 0) return;

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
            if (result > 0) {
                BtDevice* dev = bt_dev_acquire(address);
                if (dev) {
                    if (result == 43 && report[0] == 41 && report[2] == 5) {
                        int rc = dualsense_calibration_bluetooth(report + 2, 41, &dev->calibration);
                        if (rc == 0) {
                            dev->calibration_valid = true;
                            daemon_log(LOG_LEVEL_INFO, "DualSense BT (0x%llX): factory calibration loaded",
                                       (unsigned long long)address);
                        } else {
                            daemon_log(LOG_LEVEL_WARN, "DualSense BT (0x%llX): calibration decode failed (rc=%d)",
                                       (unsigned long long)address, rc);
                        }
                    } else if (result >= 2 && result <= 80 && report[0] + 2 == result && report[1] == 0x1F) {
                        dev->reports_seen++;
                        decode_dualsense_bt_report(dev, report + 2, (size_t)(result - 2));
                    }
                }
            }
        }
    }
}

// -------------------------------------------------------------
// Public Subsystem API
// -------------------------------------------------------------

int bt_driver_init(void) {
    daemon_log(LOG_LEVEL_INFO, "Initializing DualSense Bluetooth Subsystem (up to %d controllers)...", BT_MAX_DEVICES);
    bt_devices_reset();

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
        if (v_rc == 0) {
            s_current_fw_bcd = (sw_info.version >> 16);
        }
    } else {
        daemon_log(LOG_LEVEL_WARN, "DualSense BT: sceKernelGetSystemSwVersion unresolved, defaulting to FW 0x1250 / dynamic.");
    }

    // FW Guard: DualSense Bluetooth stack and allowlist patches require FW 9.00 - 13.52.
    if (s_current_fw_bcd != 0 && s_current_fw_bcd < 0x0900) {
        daemon_log(LOG_LEVEL_INFO, "DualSense BT Subsystem skipped: FW 0x%04X < 0x0900 (BT requires FW 9.00+). USB controllers fully active.",
                   s_current_fw_bcd);
        s_bt_supported = false;
        return -1;
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
    long kevent_rc = daemon_sys_call6(SYS_KEVENT, s_bt_queue, (long)&change, 1, 0, 0, 0);
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
    int patch_rc = apply_kernel_allowlist_patch(0 /* patch */, s_current_fw_bcd);
    if (patch_rc != 0) {
        daemon_log(LOG_LEVEL_WARN, "DualSense BT: Kernel allowlist patch failed or FW 0x%04X not supported (rc=%d)",
                   s_current_fw_bcd, patch_rc);
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        s_bt_supported = false;
        return -6;
    }

    // 7. Register DualSense devices with Bluetooth driver (ioctl 0x8004622B)
    long reg_rc = bt_register_pair(SONY_VID, SONY_DUALSENSE_PID, "dualsense");
    if (reg_rc != 0) {
        apply_kernel_allowlist_patch(1 /* restore */, s_current_fw_bcd);
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        s_bt_supported = false;
        return -7;
    }

    s_bt_registered = true;
    s_bt_reg_count = 1;

    // Register DualSense Edge (0x0DF2) with Bluetooth driver
    // /dev/bt kernel table stores allowed (VID, PID) device filters with limited capacity.
    // Registering 1 entry per PID leaves slots available for both standard DualSense and DualSense Edge!
    s_bt_edge_reg_count = 0;
    if (bt_register_pair(SONY_VID, SONY_DUALSENSE_EDGE_PID, "dualsense_edge") == 0) {
        s_bt_edge_reg_count = 1;
    }

    s_bt_running = true;
    s_bt_supported = true;
    daemon_log(LOG_LEVEL_INFO, "DualSense Bluetooth Subsystem initialized (FW 0x%04X, reg_count=%d, edge_reg=%d)",
               s_current_fw_bcd, s_bt_reg_count, s_bt_edge_reg_count);
    return 0;
}

void bt_driver_shutdown(void) {
    if (!s_bt_running && s_bt_fd < 0 && !s_bt_registered) return;

    daemon_log(LOG_LEVEL_INFO, "Shutting down DualSense Bluetooth Subsystem...");
    s_bt_running = false;
    s_bt_supported = false;

    if (s_bt_registered && s_bt_fd >= 0) {
        for (int i = 0; i < s_bt_reg_count; ++i) {
            bt_unregister_pair(SONY_VID, SONY_DUALSENSE_PID);
        }
        for (int i = 0; i < s_bt_edge_reg_count; ++i) {
            bt_unregister_pair(SONY_VID, SONY_DUALSENSE_EDGE_PID);
        }
        s_bt_reg_count = 0;
        s_bt_edge_reg_count = 0;
        s_bt_registered = false;
    }

    apply_kernel_allowlist_patch(1 /* restore */, s_current_fw_bcd);

    if (s_bt_queue >= 0) {
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
    }

    if (s_bt_fd >= 0) {
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
    }

    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].address) {
            bt_dev_release(&s_devices[i]);
        }
    }
    bt_devices_reset();
    bt_write_status_file();
}

bool bt_driver_is_supported(void) {
    return s_bt_supported;
}

bool bt_driver_is_connected(void) {
    return bt_driver_connected_count() > 0;
}

int bt_driver_connected_count(void) {
    int count = 0;
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].connected) count++;
    }
    return count;
}

static void bt_send_rumble_dev(BtDevice* dev, uint8_t large, uint8_t small, const uint8_t color[3]) {
    if (!dev || !dev->connected || s_bt_fd < 0 || !dev->address) return;

    dev->last_output_time = daemon_sys_time_us();
    if (color && (color[0] || color[1] || color[2])) {
        dev->last_color[0] = color[0];
        dev->last_color[1] = color[1];
        dev->last_color[2] = color[2];
    }

    uint8_t out[78];
    memset(out, 0, sizeof(out));

    out[0] = 0x31;                  // Extended BT Output Report ID
    dev->rumble_seq = (dev->rumble_seq + 1) & 0x0F;
    out[1] = dev->rumble_seq << 4;   // Sequence tag
    out[2] = 0x10;
    out[3] = 0x03 | 0x0C;           // Enable motors (0x03) + Enable trigger effects L2/R2 (0x0C)
    out[4] = 0x14;                  // Player Indicator Control Enable (0x10) | Lightbar Control Enable (0x04)
    out[5] = small;                 // Weak motor
    out[6] = large;                 // Strong motor

    // 1. Adaptive Trigger Effects
    int trig = g_daemon_config.dualsense_trigger_mode;
    if (trig == 1) {
        // Hair Trigger (rigid snap click at ~10% travel)
        out[13] = 0x02; out[14] = 0x01; out[15] = 0x02; out[16] = 0x08;
        out[24] = 0x02; out[25] = 0x01; out[26] = 0x02; out[27] = 0x08;
    } else if (trig == 2) {
        // Continuous Resistance
        out[13] = 0x01; out[14] = 0x00; out[15] = 0x06;
        out[24] = 0x01; out[25] = 0x00; out[26] = 0x06;
    } else if (trig == 3) {
        // Vibration / Pulsation
        out[13] = 0x06; out[14] = 0x01; out[15] = 0x04; out[16] = 0x20;
        out[24] = 0x06; out[25] = 0x01; out[26] = 0x04; out[27] = 0x20;
    } else {
        // Standard / Off
        out[13] = 0x00;
        out[24] = 0x00;
    }

    // 2. Control Flags
    out[41] = 0x02; // Lightbar Setup Control Enable (enables brightness & setup)

    // 3. Lightbar Setup & LED Brightness
    out[44] = 0x00; // Standard lightbar setup
    int br = g_daemon_config.dualsense_brightness;
    if (br < 0 || br > 2) br = 0;
    out[45] = (uint8_t)br; // 0=High, 1=Medium, 2=Low

    // 4. Player Indicator LEDs (out[46])
    int p_led = g_daemon_config.dualsense_player_leds;
    if (p_led == 1) {
        out[46] = 0x1F; // All 5 LEDs On
    } else if (p_led == 2) {
        out[46] = 0x00; // Off
    } else {
        // Dynamic Player Slot (1..4)
        int slot = vpad_get_assigned_slot_num(dev->vpad_slot);
        if (slot == 2) {
            out[46] = 0x0A; // Player 2: 2 LEDs
        } else if (slot == 3) {
            out[46] = 0x15; // Player 3: 3 LEDs
        } else if (slot == 4) {
            out[46] = 0x1B; // Player 4: 4 LEDs
        } else {
            out[46] = 0x04; // Player 1: Center LED
        }
    }

    // 5. Lightbar RGB (out[47..49])
    int lb_mode = g_daemon_config.dualsense_lightbar_mode;
    if (lb_mode == 1) {
        // Static Color
        static const uint8_t static_colors[6][3] = {
            { 0, 112, 209 },  // 0: PlayStation Blue
            { 255, 0, 0 },    // 1: Red
            { 0, 255, 0 },    // 2: Green
            { 128, 0, 255 },  // 3: Purple
            { 0, 255, 255 },  // 4: Cyan
            { 255, 255, 255 } // 5: White
        };
        int c_idx = g_daemon_config.dualsense_lightbar_color;
        if (c_idx < 0 || c_idx > 5) c_idx = 0;
        out[47] = static_colors[c_idx][0];
        out[48] = static_colors[c_idx][1];
        out[49] = static_colors[c_idx][2];
    } else if (lb_mode == 2) {
        // Battery Indicator
        if (dev->battery_pct > 60) {
            out[47] = 0; out[48] = 255; out[49] = 0; // Green
        } else if (dev->battery_pct >= 20) {
            out[47] = 255; out[48] = 200; out[49] = 0; // Yellow
        } else {
            out[47] = 255; out[48] = 0; out[49] = 0; // Red
        }
    } else if (lb_mode == 3) {
        // Off
        out[47] = 0;
        out[48] = 0;
        out[49] = 0;
    } else {
        // Mode 0: Game Sync
        if (color && (color[0] || color[1] || color[2])) {
            out[47] = color[0];
            out[48] = color[1];
            out[49] = color[2];
        } else if (dev->last_color[0] || dev->last_color[1] || dev->last_color[2]) {
            out[47] = dev->last_color[0];
            out[48] = dev->last_color[1];
            out[49] = dev->last_color[2];
        } else {
            out[47] = 0;
            out[48] = 112;
            out[49] = 209; // Default PlayStation Blue
        }
    }

    // Compute CRC32 checksum for Bluetooth output packet
    uint32_t crc = bt_calc_crc32(0xA2, out, 74);
    for (unsigned i = 0; i < 4; ++i) {
        out[74 + i] = (uint8_t)(crc >> (8 * i));
    }

    // Send Bluetooth output request via ioctl addressed directly to this controller
    struct {
        uint64_t address;
        void* buffer;
        uint64_t size;
        uint64_t report_id;
    } req = { dev->address, out, 78, 0x31 };

    daemon_syscall(SYS_IOCTL, s_bt_fd, 0x80206232UL, (long)&req);
}

void bt_driver_send_rumble_slot(int vpad_slot, uint8_t large, uint8_t small, const uint8_t color[3]) {
    if (vpad_slot < 0) return;

    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].connected && s_devices[i].vpad_slot == vpad_slot) {
            bt_send_rumble_dev(&s_devices[i], large, small, color);
            return;
        }
    }
}

void bt_driver_send_rumble(uint8_t large, uint8_t small, const uint8_t color[3]) {
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].connected) {
            bt_send_rumble_dev(&s_devices[i], large, small, color);
            return;
        }
    }
}

void bt_driver_refresh_settings(void) {
    uint8_t default_color[3] = { 0, 112, 209 };
    for (int i = 0; i < BT_MAX_DEVICES; ++i) {
        if (s_devices[i].connected) {
            bt_send_rumble_dev(&s_devices[i], 0, 0, default_color);
        }
    }
}
