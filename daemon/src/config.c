/*
 * OneFamily Daemon for PS4 (GoldHEN)
 * INI Configuration Loader
 */

#include "config.h"
#include "ini_parser.h"
#include "log.h"
#include "daemon_syscalls.h"
#include "bt_driver.h"
#include <fcntl.h>
#include <string.h>

DaemonConfig g_daemon_config;
PluginConfig g_config;

void daemon_config_init_defaults(DaemonConfig *cfg) {
    if (!cfg) return;

    // [General]
    cfg->mode = CONFIG_MODE_AUTO;
    cfg->guide_action = CONFIG_GUIDE_NONE; // Default: Focus on native PS Button
    cfg->enable_notifications = 1;
    cfg->enable_mode_combo = 1;
    cfg->enable_rumble = 1;
    cfg->enable_homebrew = 1;

    // [Deadzone]
    cfg->stick_deadzone = 15;        // ~12% deadzone
    cfg->trigger_threshold = 30;      // 30 / 255

    // [Remap]
    cfg->swap_triggers_bumpers = 0;
    cfg->switch_positional = 1;

    // [Hardware]
    cfg->usb_mode_override = 0; // 0=Auto (default)

    // [DualSense]
    cfg->dualsense_lightbar_mode = 0;
    cfg->dualsense_lightbar_color = 0;
    cfg->dualsense_brightness = 0;
    cfg->dualsense_trigger_mode = 0;
    cfg->dualsense_player_leds = 0;
    cfg->dualsense_mute_action = 0;
}

void config_init_defaults(PluginConfig *cfg) {
    daemon_config_init_defaults(cfg);
}

static char* trim_whitespace(char *str) {
    if (!str) return NULL;
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') {
        str++;
    }
    if (*str == 0) return str;

    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    return str;
}

static int parse_int(const char *str, int default_val) {
    if (!str || *str == '\0') return default_val;
    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    int val = 0;
    int has_digits = 0;
    while (*str >= '0' && *str <= '9') {
        has_digits = 1;
        val = val * 10 + (*str - '0');
        str++;
    }
    return has_digits ? (val * sign) : default_val;
}

static int str_case_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + ('a' - 'A')) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + ('a' - 'A')) : *b;
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

void daemon_config_load(DaemonConfig *cfg) {
    if (!cfg) return;

    daemon_config_init_defaults(cfg);

    int fd = daemon_sys_open(ONEFAMILY_CONFIG_PATH, O_RDONLY, 0);
    if (fd < 0) {
        fd = daemon_sys_open(ONEFAMILY_CONFIG_PATH_USER, O_RDONLY, 0);
    }
    if (fd < 0) {
        fd = daemon_sys_open(ONEFAMILY_LEGACY_CONFIG_PATH, O_RDONLY, 0);
    }
    if (fd < 0) {
        fd = daemon_sys_open(ONEFAMILY_LEGACY_CONFIG_PATH_USER, O_RDONLY, 0);
    }
    if (fd < 0) {
        fd = daemon_sys_open(XBOX_CONFIG_PATH, O_RDONLY, 0);
    }
    if (fd < 0) {
        fd = daemon_sys_open(XBOX_CONFIG_PATH_USER, O_RDONLY, 0);
    }
    if (fd < 0) {
        return;
    }

    static char buf[ONEFAMILY_CONFIG_BUFFER_SIZE];
    memset(buf, 0, sizeof(buf));
    int bytes_read = daemon_sys_read(fd, buf, sizeof(buf) - 1);
    daemon_sys_close(fd);

    if (bytes_read <= 0) {
        return;
    }

    char *line = buf;
    char *next_line = NULL;

    while (line && *line) {
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }

        char *trimmed = trim_whitespace(line);
        if (*trimmed == '#' || *trimmed == ';' || *trimmed == '[' || *trimmed == '\0') {
            line = next_line;
            continue;
        }

        char *eq = strchr(trimmed, '=');
        if (eq) {
            *eq = '\0';
            char *key = trim_whitespace(trimmed);
            char *val = trim_whitespace(eq + 1);

            if (str_case_eq(key, "mode") || str_case_eq(key, "player_slot") || str_case_eq(key, "slot")) {
                if (str_case_eq(val, "merge") || str_case_eq(val, "1") || str_case_eq(val, "p1") || str_case_eq(val, "player1")) {
                    cfg->mode = CONFIG_MODE_MERGE;
                } else if (str_case_eq(val, "secondary") || str_case_eq(val, "2") || str_case_eq(val, "p2") || str_case_eq(val, "player2")) {
                    cfg->mode = CONFIG_MODE_SECONDARY;
                } else {
                    cfg->mode = CONFIG_MODE_AUTO;
                }
            } else if (str_case_eq(key, "guide_action")) {
                if (str_case_eq(val, "share")) {
                    cfg->guide_action = CONFIG_GUIDE_SHARE;
                } else if (str_case_eq(val, "touchpad")) {
                    cfg->guide_action = CONFIG_GUIDE_TOUCHPAD;
                } else {
                    // "none", "disabled", "ps", or default -> Focus on native PS Button
                    cfg->guide_action = CONFIG_GUIDE_NONE;
                }
            } else if (str_case_eq(key, "enable_notifications")) {
                cfg->enable_notifications = parse_int(val, 1);
            } else if (str_case_eq(key, "enable_mode_combo")) {
                cfg->enable_mode_combo = parse_int(val, 1);
            } else if (str_case_eq(key, "enable_rumble")) {
                cfg->enable_rumble = parse_int(val, 1);
            } else if (str_case_eq(key, "enable_homebrew")) {
                cfg->enable_homebrew = parse_int(val, 1);
            } else if (str_case_eq(key, "stick_deadzone")) {
                int dz = parse_int(val, 15);
                if (dz < 0) dz = 0;
                if (dz > 127) dz = 127;
                cfg->stick_deadzone = (uint8_t)dz;
            } else if (str_case_eq(key, "trigger_threshold")) {
                int tt = parse_int(val, 30);
                if (tt < 0) tt = 0;
                if (tt > 255) tt = 255;
                cfg->trigger_threshold = (uint8_t)tt;
            } else if (str_case_eq(key, "swap_triggers_bumpers")) {
                cfg->swap_triggers_bumpers = parse_int(val, 0);
            } else if (str_case_eq(key, "switch_positional_layout")) {
                cfg->switch_positional = parse_int(val, 1);
            } else if (str_case_eq(key, "dualsense_lightbar_mode") || str_case_eq(key, "lightbar_mode")) {
                cfg->dualsense_lightbar_mode = parse_int(val, 0);
            } else if (str_case_eq(key, "dualsense_lightbar_color") || str_case_eq(key, "lightbar_color")) {
                cfg->dualsense_lightbar_color = parse_int(val, 0);
            } else if (str_case_eq(key, "dualsense_brightness") || str_case_eq(key, "brightness")) {
                cfg->dualsense_brightness = parse_int(val, 0);
            } else if (str_case_eq(key, "dualsense_trigger_mode") || str_case_eq(key, "trigger_mode")) {
                cfg->dualsense_trigger_mode = parse_int(val, 0);
            } else if (str_case_eq(key, "dualsense_player_leds") || str_case_eq(key, "player_leds")) {
                cfg->dualsense_player_leds = parse_int(val, 0);
            } else if (str_case_eq(key, "usb_mode_override") || str_case_eq(key, "usb_override")) {
                cfg->usb_mode_override = parse_int(val, 0);
            } else if (str_case_eq(key, "dualsense_mute_action") || str_case_eq(key, "mute_action")) {
                cfg->dualsense_mute_action = parse_int(val, 0);
            }
        }

        line = next_line;
    }

    // Sync global g_config
    g_config = *cfg;
}

bool daemon_config_check_reload(DaemonConfig *cfg) {
    if (!cfg) return false;
    DaemonConfig old = *cfg;
    daemon_config_load(cfg);

    bool ds_changed = (
        old.dualsense_lightbar_mode != cfg->dualsense_lightbar_mode ||
        old.dualsense_lightbar_color != cfg->dualsense_lightbar_color ||
        old.dualsense_brightness != cfg->dualsense_brightness ||
        old.dualsense_trigger_mode != cfg->dualsense_trigger_mode ||
        old.dualsense_player_leds != cfg->dualsense_player_leds
    );

    if (ds_changed) {
        daemon_log(LOG_LEVEL_INFO, "DualSense configuration changed: applying settings on the fly!");
        bt_driver_refresh_settings();
    }

    return true;
}
