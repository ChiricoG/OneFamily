#include "hid_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>
#include <orbis/Pad.h>

const Ds4ActionMeta HidProfile::ACTIONS[ACT_COUNT] = {
    { ACT_CROSS,    "cross",    "Cross",          ORBIS_PAD_BUTTON_CROSS,    "/app0/assets/icons/btn_cross_blue.png" },
    { ACT_CIRCLE,   "circle",   "Circle",         ORBIS_PAD_BUTTON_CIRCLE,   "/app0/assets/icons/btn_circle.png" },
    { ACT_SQUARE,   "square",   "Square",         ORBIS_PAD_BUTTON_SQUARE,   "/app0/assets/icons/btn_square.png" },
    { ACT_TRIANGLE, "triangle", "Triangle",       ORBIS_PAD_BUTTON_TRIANGLE, "/app0/assets/icons/btn_triangle.png" },
    { ACT_L1,       "l1",       "L1 Bumper",      ORBIS_PAD_BUTTON_L1,       "/app0/assets/icons/btn_l1.png" },
    { ACT_R1,       "r1",       "R1 Bumper",      ORBIS_PAD_BUTTON_R1,       "/app0/assets/icons/btn_r1.png" },
    { ACT_L2,       "l2",       "L2 Trigger",     ORBIS_PAD_BUTTON_L2,       "/app0/assets/icons/btn_l2.png" },
    { ACT_R2,       "r2",       "R2 Trigger",     ORBIS_PAD_BUTTON_R2,       "/app0/assets/icons/btn_r2.png" },
    { ACT_SHARE,    "create",   "Share / Create", 0x00000001,                "/app0/assets/icons/btn_share.png" },
    { ACT_OPTIONS,  "options",  "Options",        ORBIS_PAD_BUTTON_OPTIONS,  "/app0/assets/icons/btn_options.png" },
    { ACT_PS,       "ps",       "PS Button",      0x00010000,                "/app0/assets/icons/btn_ps.png" },
    { ACT_TOUCHPAD, "touchpad", "Touchpad Click", ORBIS_PAD_BUTTON_TOUCH_PAD,"/app0/assets/icons/btn_touchpad.png" },
    { ACT_L3,       "l3",       "L3 Click",       ORBIS_PAD_BUTTON_L3,       "/app0/assets/icons/btn_l3.png" },
    { ACT_R3,       "r3",       "R3 Click",       ORBIS_PAD_BUTTON_R3,       "/app0/assets/icons/btn_r3.png" }
};

HidProfile::HidProfile() : max_buttons(16) {
    reset_defaults(16);
}

bool HidProfile::file_exists(const char* path) {
    int fd = sceKernelOpen(path, O_RDONLY, 0);
    if (fd >= 0) {
        sceKernelClose(fd);
        return true;
    }
    return false;
}

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool HidProfile::read_status(HidStatus& out) {
    out.connected = false;
    out.type = "none";

    int fd = sceKernelOpen("/data/onefamily/onefamily.status", O_RDONLY, 0);
    if (fd < 0) return false;

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    ssize_t bytes_read = sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);

    if (bytes_read <= 0) return false;
    buf[bytes_read] = '\0';

    char* line = strtok(buf, "\r\n");
    while (line != NULL) {
        std::string s = trim(line);
        if (!s.empty() && s[0] != '#') {
            size_t eq = s.find('=');
            if (eq != std::string::npos) {
                std::string k = trim(s.substr(0, eq));
                std::string v = trim(s.substr(eq + 1));
                if (k == "connected") {
                    out.connected = (atoi(v.c_str()) != 0);
                } else if (k == "type") {
                    out.type = v;
                } else if (k == "vid") {
                    out.vid = (uint16_t)strtoul(v.c_str(), NULL, 16);
                } else if (k == "pid") {
                    out.pid = (uint16_t)strtoul(v.c_str(), NULL, 16);
                } else if (k == "name") {
                    out.name = v;
                } else if (k == "nbuttons") {
                    out.nbuttons = atoi(v.c_str());
                    if (out.nbuttons <= 0) out.nbuttons = 16;
                } else if (k == "player_slot") {
                    out.player_slot = atoi(v.c_str());
                    if (out.player_slot <= 0) out.player_slot = 1;
                } else if (k == "user_name") {
                    out.user_name = v;
                } else if (k == "battery_pct") {
                    out.battery_pct = atoi(v.c_str());
                    if (out.battery_pct < 0) out.battery_pct = 0;
                    if (out.battery_pct > 100) out.battery_pct = 100;
                } else if (k == "battery_charging") {
                    out.battery_charging = (atoi(v.c_str()) != 0);
                } else if (k == "count") {
                    out.device_count = atoi(v.c_str());
                } else if (k.rfind("dev_", 0) == 0) {
                    int devIdx = 0;
                    char field[32] = {0};
                    if (sscanf(k.c_str(), "dev_%d_%31s", &devIdx, field) == 2) {
                        if (devIdx >= 0 && devIdx < 4) {
                            if ((size_t)devIdx >= out.devices.size()) {
                                out.devices.resize(devIdx + 1);
                            }
                            if (strcmp(field, "slot") == 0) {
                                out.devices[devIdx].slot = atoi(v.c_str());
                                out.devices[devIdx].player_slot = out.devices[devIdx].slot;
                            } else if (strcmp(field, "player_slot") == 0) {
                                out.devices[devIdx].player_slot = atoi(v.c_str());
                                out.devices[devIdx].slot = out.devices[devIdx].player_slot;
                            } else if (strcmp(field, "user") == 0 || strcmp(field, "user_name") == 0) {
                                out.devices[devIdx].user_name = v;
                            } else if (strcmp(field, "mac") == 0) {
                                out.devices[devIdx].mac = v;
                            } else if (strcmp(field, "battery") == 0) {
                                out.devices[devIdx].battery_pct = atoi(v.c_str());
                            } else if (strcmp(field, "charging") == 0) {
                                out.devices[devIdx].battery_charging = (atoi(v.c_str()) != 0);
                            }
                        }
                    }
                }
            }
        }
        line = strtok(NULL, "\r\n");
    }

    return out.connected;
}

void HidProfile::reset_defaults(int nbuttons) {
    max_buttons = (nbuttons >= 14 && nbuttons <= 32) ? nbuttons : 16;
    custom_rumble = "auto";
    for (int i = 0; i < ACT_COUNT; i++) {
        assignments[i] = 0;
    }

    // Default template mapping
    if (max_buttons >= 15) {
        assignments[ACT_CROSS]    = 1;
        assignments[ACT_CIRCLE]   = 2;
        assignments[ACT_SQUARE]   = 4;
        assignments[ACT_TRIANGLE] = 5;
        assignments[ACT_L1]       = 7;
        assignments[ACT_R1]       = 8;
        assignments[ACT_L2]       = 9;
        assignments[ACT_R2]       = 10;
        assignments[ACT_SHARE]    = 11;
        assignments[ACT_OPTIONS]  = 12;
        assignments[ACT_PS]       = 13;
        assignments[ACT_TOUCHPAD] = 0;
        assignments[ACT_L3]       = 14;
        assignments[ACT_R3]       = 15;
    } else {
        assignments[ACT_SQUARE]   = 1;
        assignments[ACT_CROSS]    = 2;
        assignments[ACT_CIRCLE]   = 3;
        assignments[ACT_TRIANGLE] = 4;
        assignments[ACT_L1]       = 5;
        assignments[ACT_R1]       = 6;
        assignments[ACT_L2]       = 7;
        assignments[ACT_R2]       = 8;
        assignments[ACT_SHARE]    = 9;
        assignments[ACT_OPTIONS]  = 10;
        assignments[ACT_L3]       = 11;
        assignments[ACT_R3]       = 12;
        assignments[ACT_PS]       = 13;
        assignments[ACT_TOUCHPAD] = 14;
    }
}

bool HidProfile::load(uint16_t vid, uint16_t pid, int nbuttons) {
    reset_defaults(nbuttons);

    char path[128];
    snprintf(path, sizeof(path), "/data/onefamily/controllers/%04X_%04X.ini", vid, pid);
    if (!file_exists(path)) {
        snprintf(path, sizeof(path), "/user/data/onefamily/controllers/%04X_%04X.ini", vid, pid);
    }
    if (!file_exists(path)) {
        snprintf(path, sizeof(path), "/data/onefamily/controllers/%04x_%04x.ini", vid, pid);
    }
    if (!file_exists(path)) {
        snprintf(path, sizeof(path), "/user/data/onefamily/controllers/%04x_%04x.ini", vid, pid);
    }
    if (!file_exists(path)) {
        snprintf(path, sizeof(path), "/data/onefamily/controllers/default.ini");
    }
    if (!file_exists(path)) {
        snprintf(path, sizeof(path), "/user/data/onefamily/controllers/default.ini");
    }

    current_path = path;
    int fd = sceKernelOpen(path, O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    ssize_t bytes_read = sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);

    if (bytes_read <= 0) return false;
    buf[bytes_read] = '\0';

    char* line = strtok(buf, "\r\n");
    while (line != NULL) {
        std::string s = trim(line);
        if (!s.empty() && s[0] != '#' && s[0] != ';' && s[0] != '[') {
            size_t eq = s.find('=');
            if (eq != std::string::npos) {
                std::string k = trim(s.substr(0, eq));
                std::string v = trim(s.substr(eq + 1));
                size_t cpos = v.find('#');
                if (cpos == std::string::npos) cpos = v.find(';');
                if (cpos != std::string::npos) v = trim(v.substr(0, cpos));

                unsigned btn = 0;
                if (sscanf(k.c_str(), "button%u", &btn) == 1 && btn >= 1 && btn <= 32) {
                    for (int a = 0; a < ACT_COUNT; a++) {
                        if (strcasecmp(v.c_str(), ACTIONS[a].key_name) == 0 ||
                            (a == ACT_SHARE && strcasecmp(v.c_str(), "share") == 0)) {
                            assignments[a] = (int)btn;
                        }
                    }
                } else if (strcasecmp(k.c_str(), "rumble") == 0 ||
                           strcasecmp(k.c_str(), "rumble_report") == 0) {
                    custom_rumble = v;
                }
            }
        }
        line = strtok(NULL, "\r\n");
    }

    return true;
}

bool HidProfile::save(uint16_t vid, uint16_t pid, const std::string& name) {
    mkdir("/data", 0777);
    mkdir("/data/onefamily", 0777);
    mkdir("/data/onefamily/controllers", 0777);
    mkdir("/user", 0777);
    mkdir("/user/data", 0777);
    mkdir("/user/data/onefamily", 0777);
    mkdir("/user/data/onefamily/controllers", 0777);

    char out[4096];
    int len = snprintf(out, sizeof(out),
        "# OneFamily Generic Controller Profile\n"
        "[Info]\n"
        "name = %s\n"
        "vendor_id = 0x%04X\n"
        "product_id = 0x%04X\n"
        "\n"
        "[Buttons]\n",
        name.c_str(), vid, pid
    );

    // Write button mappings for 1 .. max_buttons
    for (int b = 1; b <= max_buttons && len < (int)sizeof(out) - 64; b++) {
        const char* target = "none";
        for (int a = 0; a < ACT_COUNT; a++) {
            if (assignments[a] == b) {
                target = ACTIONS[a].key_name;
                break;
            }
        }
        len += snprintf(out + len, sizeof(out) - len, "button%d = %s\n", b, target);
    }

    // Default axes & invert
    len += snprintf(out + len, sizeof(out) - len,
        "\n[Axes]\n"
        "lx = x\n"
        "ly = y\n"
        "rx = z\n"
        "ry = rz\n"
        "l2 = rx\n"
        "r2 = ry\n"
        "\n[Invert]\n"
        "invert_ly = 0\n"
        "invert_ry = 0\n"
    );

    if (len < (int)sizeof(out) - 160) {
        const char* rumble_val = custom_rumble.empty() ? "auto" : custom_rumble.c_str();
        len += snprintf(out + len, sizeof(out) - len,
            "\n[Rumble]\n"
            "# Presets: auto, logitech, dragonrise, greenasia, betop, shanwan, 8bitdo, dinput05, none (or hex e.g. 05 L R 00 00)\n"
            "rumble = %s\n",
            rumble_val
        );
    }

    bool w1 = false, w2 = false;
    char path[128];
    snprintf(path, sizeof(path), "/data/onefamily/controllers/%04X_%04X.ini", vid, pid);
    int fd1 = sceKernelOpen(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd1 >= 0) {
        if (len > 0) {
            ssize_t wr = sceKernelWrite(fd1, out, (size_t)len);
            if (wr == len) w1 = true;
        }
        sceKernelClose(fd1);
    }

    snprintf(path, sizeof(path), "/user/data/onefamily/controllers/%04X_%04X.ini", vid, pid);
    int fd2 = sceKernelOpen(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd2 >= 0) {
        if (len > 0) {
            ssize_t wr = sceKernelWrite(fd2, out, (size_t)len);
            if (wr == len) w2 = true;
        }
        sceKernelClose(fd2);
    }

    if (!w1 && !w2) return false;

    // Touch reload sentinel file for daemon in both primary and userland fallback paths
    int rfd1 = sceKernelOpen("/data/onefamily/onefamily.reload", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (rfd1 >= 0) {
        sceKernelWrite(rfd1, "1\n", 2);
        sceKernelClose(rfd1);
    }
    int rfd2 = sceKernelOpen("/user/data/onefamily/onefamily.reload", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (rfd2 >= 0) {
        sceKernelWrite(rfd2, "1\n", 2);
        sceKernelClose(rfd2);
    }

    return true;
}

int HidProfile::get_assigned_button(int action_idx) const {
    if (action_idx < 0 || action_idx >= ACT_COUNT) return 0;
    return assignments[action_idx];
}

void HidProfile::set_assigned_button(int action_idx, int btn) {
    if (action_idx < 0 || action_idx >= ACT_COUNT) return;
    if (btn < 0) btn = 0;
    if (btn > max_buttons) btn = max_buttons;
    assignments[action_idx] = btn;
}

void HidProfile::cycle_button(int action_idx, int dir) {
    if (action_idx < 0 || action_idx >= ACT_COUNT) return;
    int cur = assignments[action_idx];
    cur += dir;
    if (cur > max_buttons) cur = 0;
    else if (cur < 0) cur = max_buttons;
    assignments[action_idx] = cur;
}
