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
static bool s_bt_supported = false;
static bool s_dualsense_connected = false;
static bool s_bt_share_combo_active = false;
static int s_bt_vpad_slot = -1;
static uint64_t s_physical_address = 0;
static uint64_t s_last_input_time = 0;
static uint32_t s_current_fw_bcd = 0x1250;

// DualSense IMU Calibration & 3D Motion Orientation State
typedef struct {
    float gyro_scale[3];
    float accel_scale[3];
    float accel_bias[3];
} DualSenseCalibration;

static DualSenseCalibration s_calibration;
static bool s_calibration_valid = false;
static bool s_calibration_requested = false;
static uint64_t s_last_calibration_attempt = 0;
static float s_orientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // x, y, z, w
static uint64_t s_last_motion_time = 0;

static void reset_calibration(void) {
    for (int i = 0; i < 3; ++i) {
        s_calibration.gyro_scale[i] = (2000.0f * 0.01745329252f) / 32768.0f;
        s_calibration.accel_scale[i] = 1.0f / 8192.0f;
        s_calibration.accel_bias[i] = 0.0f;
    }
    s_calibration_valid = false;
    s_calibration_requested = false;
    s_last_calibration_attempt = 0;
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
    uint32_t vids[8];
    uint32_t pids[8];
    int n_vid = 0, n_pid = 0;

    // 1. VID Phase: Scan safe kernel window for CMP imm32 == 0x044F (Thrustmaster VID)
    // 0x044F is exceptionally rare in the kernel and only present in bthid allowlists.
    for (uint64_t a = base + AOB_SAFE_SCAN_START; a + 4 <= base + AOB_SAFE_SCAN_END; ++a) {
        if (*(const volatile uint32_t*)a == 0x0000044FUL) {
            if (aob_is_cmp_imm((const uint8_t*)a)) {
                if (n_vid < 8) vids[n_vid++] = (uint32_t)(a - base);
            }
        }
    }

    if (req) {
        req->n_vid_found = n_vid;
        for (int i = 0; i < n_vid && i < 8; ++i) req->discovered_vids[i] = vids[i];
    }

    // Accept 5 or 6 VIDs (FW 12.50 has 5, FW 13.52 has 6)
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

    // 3. Greedy Nearest-Pairing: assign each VID to its closest unused companion PID
    bool pid_used[8] = {false};
    uint32_t pairs = 0;

    for (int i = 0; i < n_vid && pairs < 6; ++i) {
        uint32_t v = vids[i];
        int best_idx = -1;
        uint32_t best_dist = 0xFFFFFFFFU;

        // Prefer forward PID within 4096 bytes
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

        // Fallback: Closest available overall
        if (best_idx < 0) {
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
    // Path 2: Verified Static Table (FW 13.52)
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

static void bt_write_status_file(bool connected, uint8_t battery_pct, uint8_t battery_status) {
    daemon_sys_mkdir("/data", 0777);
    daemon_sys_mkdir("/data/onefamily", 0777);
    int fd = daemon_sys_open("/data/onefamily/onefamily.status", 0x0001 | 0x0200 | 0x0400, 0666);
    if (fd >= 0) {
        char buf[320];
        int len = snprintf(buf, sizeof(buf),
            "connected = %d\n"
            "type = dualsense_bt\n"
            "vid = 0x054C\n"
            "pid = 0x0CE6\n"
            "name = DualSense Wireless\n"
            "nbuttons = 16\n"
            "player_slot = %d\n"
            "user_name = %s\n"
            "battery_pct = %d\n"
            "battery_charging = %d\n",
            connected ? 1 : 0,
            connected ? vpad_get_assigned_slot_num(s_bt_vpad_slot) : 1,
            connected ? vpad_get_assigned_user() : "None",
            battery_pct,
            (battery_status == 1) ? 1 : 0
        );
        daemon_sys_write(fd, buf, len);
        daemon_sys_close(fd);
    }
}

static void extended_sample(OrbisPadData* ds4, const uint8_t* report, float dt) {
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
    ds4->vel.x = (float)raw_gyro[0] * s_calibration.gyro_scale[0];
    ds4->vel.y = (float)raw_gyro[1] * s_calibration.gyro_scale[1];
    ds4->vel.z = (float)raw_gyro[2] * s_calibration.gyro_scale[2];
    omega[0] = ds4->vel.x;
    omega[1] = ds4->vel.y;
    omega[2] = ds4->vel.z;

    // Acceleration in g
    ds4->acell.x = ((float)raw_accel[0] - s_calibration.accel_bias[0]) * s_calibration.accel_scale[0];
    ds4->acell.y = ((float)raw_accel[1] - s_calibration.accel_bias[1]) * s_calibration.accel_scale[1];
    ds4->acell.z = ((float)raw_accel[2] - s_calibration.accel_bias[2]) * s_calibration.accel_scale[2];

    // 3. 3D Orientation Quaternion Integration (Diram1x complementary filter)
    if (dt > 0.0f && dt <= 0.1f) {
        float x = s_orientation[0];
        float y = s_orientation[1];
        float z = s_orientation[2];
        float w = s_orientation[3];

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
        s_orientation[0] += ( w * omega[0] + y * omega[2] - z * omega[1]) * half;
        s_orientation[1] += ( w * omega[1] + z * omega[0] - x * omega[2]) * half;
        s_orientation[2] += ( w * omega[2] + x * omega[1] - y * omega[0]) * half;
        s_orientation[3] += (-x * omega[0] - y * omega[1] - z * omega[2]) * half;

        float norm = s_orientation[0] * s_orientation[0] +
                     s_orientation[1] * s_orientation[1] +
                     s_orientation[2] * s_orientation[2] +
                     s_orientation[3] * s_orientation[3];
        norm = root_positive(norm);
        if (norm > 0.0001f) {
            s_orientation[0] /= norm;
            s_orientation[1] /= norm;
            s_orientation[2] /= norm;
            s_orientation[3] /= norm;
        }
    }

    ds4->quat.x = s_orientation[0];
    ds4->quat.y = s_orientation[1];
    ds4->quat.z = s_orientation[2];
    ds4->quat.w = s_orientation[3];
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
        bt_write_status_file(true, 100, 0);
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
            s_bt_share_combo_active = true;
        } else if (s_bt_share_combo_active) {
            // Asynchronous release de-ghosting: suppress Share/Create
            ds4.buttons &= ~DS4_BUTTON_SHARE;
        }
    } else {
        s_bt_share_combo_active = false;
    }

    ds4.connected = 1;
    ds4.timestamp = s_last_input_time;

    if (length == 78) {
        // Battery status & level from report 0x31 (data[52])
        uint8_t raw_battery = data[52] & 0x0F;
        uint8_t battery_status = (data[52] >> 4) & 0x0F;
        uint8_t battery_pct = (battery_status == 2) ? 100 : (raw_battery < 10 ? (raw_battery * 10 + 5) : 100);

        static uint64_t s_last_battery_update = 0;
        if (s_last_input_time - s_last_battery_update >= 30000000ULL) { // Telemetry & status update every 30s
            s_last_battery_update = s_last_input_time;
            bt_write_status_file(true, battery_pct, battery_status);
            daemon_log(LOG_LEVEL_INFO, "DualSense BT Battery: %d%% (status=%s)",
                       battery_pct,
                       battery_status == 1 ? "Charging" : (battery_status == 2 ? "Full" : "Discharging"));
        }

        float dt = 0.0f;
        if (s_last_motion_time > 0 && s_last_input_time > s_last_motion_time) {
            dt = (float)(s_last_input_time - s_last_motion_time) / 1000000.0f;
        }
        s_last_motion_time = s_last_input_time;

        extended_sample(&ds4, report, dt);
    } else {
        ds4.quat.w = 1.0f;
    }

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

    // Feature Report 0x05 request for factory IMU calibration
    uint64_t now_us = daemon_sys_time_us();
    if (s_dualsense_connected && s_physical_address && !s_calibration_requested &&
        (now_us - s_last_calibration_attempt > 1000000ULL)) {
        s_last_calibration_attempt = now_us;
        struct { uint64_t address; uint8_t report_id; uint8_t reserved[7]; } feature = { s_physical_address, 5, {0} };
        long rc = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x8010622FUL, (long)&feature);
        if (rc == 0) {
            s_calibration_requested = true;
            daemon_log(LOG_LEVEL_INFO, "DualSense BT: requested IMU factory calibration (Feature Report 0x05)");
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
    if (ready <= 0) {
        // Check 3-second timeout for disconnection
        if (s_dualsense_connected && (daemon_sys_time_us() - s_last_input_time > 3000000ULL)) {
            s_dualsense_connected = false;
            s_bt_share_combo_active = false;
            s_physical_address = 0;
            reset_calibration();
            s_orientation[0] = 0.0f; s_orientation[1] = 0.0f; s_orientation[2] = 0.0f; s_orientation[3] = 1.0f;
            s_last_motion_time = 0;
            if (s_bt_vpad_slot >= 0) {
                vpad_slot_close(s_bt_vpad_slot);
                s_bt_vpad_slot = -1;
            }
            bt_write_status_file(false, 0, 0);
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
            if (ev_type == 0x18 || ev_type == 0x19) {
                s_calibration_requested = false;
            }
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
            if (result == 43 && report[0] == 41 && report[2] == 5) {
                int rc = dualsense_calibration_bluetooth(report + 2, 41, &s_calibration);
                if (rc == 0) {
                    s_calibration_valid = true;
                    daemon_log(LOG_LEVEL_INFO, "DualSense BT: factory calibration loaded successfully");
                } else {
                    daemon_log(LOG_LEVEL_WARN, "DualSense BT: calibration decode failed (rc=%d), using nominal scales", rc);
                }
            } else if (result >= 2 && result <= 80 && report[0] + 2 == result && report[1] == 0x1F) {
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
    reset_calibration();
    s_orientation[0] = 0.0f; s_orientation[1] = 0.0f; s_orientation[2] = 0.0f; s_orientation[3] = 1.0f;
    s_last_motion_time = 0;

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
    // On FW < 9.00 (e.g. 5.05, 6.72, 7.xx), DualSense BT is not supported by the kernel stack.
    // Cleanly skip the Bluetooth subsystem so USB controllers operate with zero crash risk.
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

    // 7. Register DualSense device with Bluetooth driver (ioctl 0x8004622B)
    struct {
        uint16_t vendor;
        uint16_t product;
    } device = { SONY_VID, SONY_DUALSENSE_PID };
    long reg_rc = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x8004622BUL, (long)&device);
    daemon_log(LOG_LEVEL_INFO, "register_dualsense = %ld", reg_rc);
    if (reg_rc != 0) {
        // Registration failed: restore allowlist, close descriptors, return cleanly
        apply_kernel_allowlist_patch(1 /* restore */, s_current_fw_bcd);
        daemon_sys_close(s_bt_queue);
        s_bt_queue = -1;
        daemon_sys_close(s_bt_fd);
        s_bt_fd = -1;
        s_bt_supported = false;
        return -7;
    }

    s_bt_registered = true;
    s_bt_running = true;
    s_bt_supported = true;
    daemon_log(LOG_LEVEL_INFO, "DualSense Bluetooth Subsystem initialized successfully (FW 0x%04X)", s_current_fw_bcd);
    return 0;
}

void bt_driver_shutdown(void) {
    if (!s_bt_running && s_bt_fd < 0 && !s_bt_registered) return;

    daemon_log(LOG_LEVEL_INFO, "Shutting down DualSense Bluetooth Subsystem...");
    s_bt_running = false;
    s_bt_supported = false;

    if (s_bt_registered && s_bt_fd >= 0) {
        struct {
            uint16_t vendor;
            uint16_t product;
        } device = { SONY_VID, SONY_DUALSENSE_PID };
        long unreg_rc = daemon_syscall(SYS_IOCTL, s_bt_fd, 0x8004622CUL, (long)&device);
        daemon_log(LOG_LEVEL_INFO, "unregister_dualsense = %ld", unreg_rc);
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

    if (s_bt_vpad_slot >= 0) {
        vpad_slot_close(s_bt_vpad_slot);
        s_bt_vpad_slot = -1;
    }
    s_dualsense_connected = false;
    s_bt_share_combo_active = false;
    s_physical_address = 0;
    reset_calibration();
    s_orientation[0] = 0.0f; s_orientation[1] = 0.0f; s_orientation[2] = 0.0f; s_orientation[3] = 1.0f;
    s_last_motion_time = 0;
    bt_write_status_file(false, 0, 0);
}

bool bt_driver_is_supported(void) {
    return s_bt_supported;
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
