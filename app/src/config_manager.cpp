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
        if (file_exists(LEGACY_INI_FILE_PATH)) {
            path_to_load = LEGACY_INI_FILE_PATH;
        } else if (file_exists(OLD_LEGACY_INI_FILE_PATH)) {
            path_to_load = OLD_LEGACY_INI_FILE_PATH;
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

    int fd = sceKernelOpen(INI_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        fd = sceKernelOpen("/user/data/onefamily/onefamily.ini", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    if (fd < 0) {
        return false;
    }

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
        "switch_positional_layout = %d\n",
        deadzone_val,
        trigger_val,
        rumble_val,
        swap_val,
        switch_val
    );

    if (len > 0) {
        sceKernelWrite(fd, out, (size_t)len);
    }
    sceKernelClose(fd);
    return true;
}
