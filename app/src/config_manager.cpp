#include "config_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>

const std::vector<std::string> ConfigManager::DEADZONE_OPTIONS = {
    "0%", "5%", "8%", "10%", "12%", "15%", "20%", "25%"
};

const std::vector<int> ConfigManager::DEADZONE_VALUES = {
    0, 6, 10, 13, 15, 19, 25, 32
};

const std::vector<std::string> ConfigManager::SWAP_OPTIONS = {
    "Disabled", "Enabled"
};

const std::vector<std::string> ConfigManager::RUMBLE_OPTIONS = {
    "Enabled", "Disabled"
};

const std::vector<std::string> ConfigManager::TRIGGER_OPTIONS = {
    "15 (6%)", "30 (12%)", "45 (18%)", "60 (24%)", "80 (31%)"
};

const std::vector<int> ConfigManager::TRIGGER_VALUES = {
    15, 30, 45, 60, 80
};

const std::vector<std::string> ConfigManager::SWITCH_OPTIONS = {
    "Positional", "By Letter"
};

const std::vector<std::string> ConfigManager::USB_MODE_OPTIONS = {
    "Auto (Recommended)", "Force XInput (Xbox)", "Force Switch", "Force Generic HID", "Force DS4 Mode"
};

const std::vector<std::string> ConfigManager::DS_LIGHTBAR_OPTIONS = {
    "Game Sync", "Static Color", "Battery Status", "Off"
};

const std::vector<std::string> ConfigManager::DS_COLOR_OPTIONS = {
    "PlayStation Blue", "Red", "Green", "Purple", "Cyan", "White"
};

const std::vector<std::string> ConfigManager::DS_BRIGHTNESS_OPTIONS = {
    "High", "Medium", "Low"
};

const std::vector<std::string> ConfigManager::DS_TRIGGER_OPTIONS = {
    "Standard / Off", "Hair Trigger (Click)", "Continuous Resistance", "Vibration / Bow"
};

const std::vector<std::string> ConfigManager::DS_PLAYER_LEDS_OPTIONS = {
    "Player Slot", "All On", "Off"
};

const std::vector<std::string> ConfigManager::DS_MUTE_OPTIONS = {
    "Default", "Touchpad Click"
};

ConfigManager::ConfigManager() {
    config.set_defaults();
}

bool ConfigManager::file_exists(const char* path) {
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

bool ConfigManager::load() {
    const char* path_to_load = INI_FILE_PATH;
    if (!file_exists(INI_FILE_PATH)) {
        if (file_exists(INI_FILE_PATH_USER)) {
            path_to_load = INI_FILE_PATH_USER;
        } else if (file_exists(LEGACY_INI_FILE_PATH)) {
            path_to_load = LEGACY_INI_FILE_PATH;
        } else if (file_exists(LEGACY_INI_FILE_PATH_USER)) {
            path_to_load = LEGACY_INI_FILE_PATH_USER;
        } else if (file_exists(OLD_LEGACY_INI_FILE_PATH)) {
            path_to_load = OLD_LEGACY_INI_FILE_PATH;
        } else if (file_exists(OLD_LEGACY_INI_PATH_USER)) {
            path_to_load = OLD_LEGACY_INI_PATH_USER;
        } else {
            // First run: file does not exist yet! Create it with defaults.
            config.set_defaults();
            save();
            return true;
        }
    }

    // File exists: load and parse current values
    config.set_defaults();

    int fd = sceKernelOpen(path_to_load, O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    ssize_t bytes_read = (ssize_t)sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);

    if (bytes_read <= 0) {
        return false;
    }
    buf[bytes_read] = '\0';

    char* line = strtok(buf, "\r\n");
    while (line != NULL) {
        std::string s = trim(line);
        if (!s.empty() && s[0] != '#' && s[0] != ';' && s[0] != '[') {
            size_t eq = s.find('=');
            if (eq != std::string::npos) {
                std::string key = trim(s.substr(0, eq));
                std::string val = trim(s.substr(eq + 1));

                size_t cpos = val.find('#');
                if (cpos == std::string::npos) cpos = val.find(';');
                if (cpos != std::string::npos) val = trim(val.substr(0, cpos));

                if (strcasecmp(key.c_str(), "stick_deadzone") == 0 || strcasecmp(key.c_str(), "deadzone") == 0) {
                    int dz = atoi(val.c_str());
                    int best_idx = 4; // default 12%
                    int min_diff = 999;
                    for (size_t i = 0; i < DEADZONE_VALUES.size(); i++) {
                        int diff = abs(DEADZONE_VALUES[i] - dz);
                        if (diff < min_diff) {
                            min_diff = diff;
                            best_idx = (int)i;
                        }
                    }
                    config.deadzone_index = best_idx;
                } else if (strcasecmp(key.c_str(), "swap_triggers_bumpers") == 0) {
                    int v = atoi(val.c_str());
                    config.swap_index = (v != 0) ? 1 : 0;
                } else if (strcasecmp(key.c_str(), "enable_rumble") == 0 || strcasecmp(key.c_str(), "rumble") == 0) {
                    int v = atoi(val.c_str());
                    config.rumble_index = (v != 0) ? 0 : 1; // 0=Enabled, 1=Disabled
                } else if (strcasecmp(key.c_str(), "trigger_threshold") == 0) {
                    int th = atoi(val.c_str());
                    int best_idx = 1; // default 30
                    int min_diff = 999;
                    for (size_t i = 0; i < TRIGGER_VALUES.size(); i++) {
                        int diff = abs(TRIGGER_VALUES[i] - th);
                        if (diff < min_diff) {
                            min_diff = diff;
                            best_idx = (int)i;
                        }
                    }
                    config.trigger_index = best_idx;
                } else if (strcasecmp(key.c_str(), "switch_positional_layout") == 0) {
                    int v = atoi(val.c_str());
                    config.switch_index = (v != 0) ? 0 : 1; // 0=Positional (1), 1=By Letter (0)
                } else if (strcasecmp(key.c_str(), "usb_mode_override") == 0 || strcasecmp(key.c_str(), "usb_override") == 0) {
                    int v = atoi(val.c_str());
                    if (v >= 0 && v < (int)USB_MODE_OPTIONS.size()) config.usb_mode_override = v;
                } else if (strcasecmp(key.c_str(), "dualsense_lightbar_mode") == 0 || strcasecmp(key.c_str(), "lightbar_mode") == 0) {
                    int v = atoi(val.c_str());
                    if (v >= 0 && v < (int)DS_LIGHTBAR_OPTIONS.size()) config.ds_lightbar_mode = v;
                } else if (strcasecmp(key.c_str(), "dualsense_lightbar_color") == 0 || strcasecmp(key.c_str(), "lightbar_color") == 0) {
                    int v = atoi(val.c_str());
                    if (v >= 0 && v < (int)DS_COLOR_OPTIONS.size()) config.ds_lightbar_color = v;
                } else if (strcasecmp(key.c_str(), "dualsense_brightness") == 0 || strcasecmp(key.c_str(), "brightness") == 0) {
                    int v = atoi(val.c_str());
                    if (v >= 0 && v < (int)DS_BRIGHTNESS_OPTIONS.size()) config.ds_brightness = v;
                } else if (strcasecmp(key.c_str(), "dualsense_trigger_mode") == 0 || strcasecmp(key.c_str(), "trigger_mode") == 0) {
                    int v = atoi(val.c_str());
                    if (v >= 0 && v < (int)DS_TRIGGER_OPTIONS.size()) config.ds_trigger_mode = v;
                } else if (strcasecmp(key.c_str(), "dualsense_player_leds") == 0 || strcasecmp(key.c_str(), "player_leds") == 0) {
                    int v = atoi(val.c_str());
                    if (v >= 0 && v < (int)DS_PLAYER_LEDS_OPTIONS.size()) config.ds_player_leds = v;
                } else if (strcasecmp(key.c_str(), "dualsense_mute_action") == 0 || strcasecmp(key.c_str(), "mute_action") == 0) {
                    int v = atoi(val.c_str());
                    if (v >= 0 && v < (int)DS_MUTE_OPTIONS.size()) config.ds_mute_action = v;
                }
            }
        }
        line = strtok(NULL, "\r\n");
    }

    return true;
}

bool ConfigManager::save() {
    mkdir("/data", 0777);
    mkdir("/data/onefamily", 0777);
    mkdir("/user", 0777);
    mkdir("/user/data", 0777);
    mkdir("/user/data/onefamily", 0777);

    char out[2048];
    int deadzone_val = DEADZONE_VALUES[config.deadzone_index];
    int trigger_val = TRIGGER_VALUES[config.trigger_index];
    int swap_val = (config.swap_index != 0) ? 1 : 0;
    int rumble_val = (config.rumble_index == 0) ? 1 : 0;
    int switch_val = (config.switch_index == 0) ? 1 : 0;

    int len = snprintf(out, sizeof(out),
        "# OneFamily Daemon - Configuration File\n"
        "# Generated by OneFamily Companion for PS4\n"
        "# Location on PS4: /data/onefamily/onefamily.ini\n"
        "\n"
        "[Deadzone]\n"
        "stick_deadzone = %d\n"
        "trigger_threshold = %d\n"
        "\n"
        "[General]\n"
        "enable_rumble = %d\n"
        "\n"
        "[Remap]\n"
        "swap_triggers_bumpers = %d\n"
        "switch_positional_layout = %d\n"
        "\n"
        "[Hardware]\n"
        "usb_mode_override = %d\n"
        "\n"
        "[DualSense]\n"
        "dualsense_lightbar_mode = %d\n"
        "dualsense_lightbar_color = %d\n"
        "dualsense_brightness = %d\n"
        "dualsense_trigger_mode = %d\n"
        "dualsense_player_leds = %d\n"
        "dualsense_mute_action = %d\n",
        deadzone_val,
        trigger_val,
        rumble_val,
        swap_val,
        switch_val,
        config.usb_mode_override,
        config.ds_lightbar_mode,
        config.ds_lightbar_color,
        config.ds_brightness,
        config.ds_trigger_mode,
        config.ds_player_leds,
        config.ds_mute_action
    );

    bool w1 = false, w2 = false;
    int fd1 = sceKernelOpen(INI_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd1 >= 0) {
        if (len > 0) {
            ssize_t wr = sceKernelWrite(fd1, out, (size_t)len);
            if (wr == len) w1 = true;
        }
        sceKernelClose(fd1);
    }

    int fd2 = sceKernelOpen(INI_FILE_PATH_USER, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd2 >= 0) {
        if (len > 0) {
            ssize_t wr = sceKernelWrite(fd2, out, (size_t)len);
            if (wr == len) w2 = true;
        }
        sceKernelClose(fd2);
    }

    return (w1 || w2);
}
