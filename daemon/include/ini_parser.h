/*
 * OneFamily Plugin & Daemon for PS4 (GoldHEN)
 * Configuration structures and INI loader
 */

#ifndef INI_PARSER_H
#define INI_PARSER_H

#include <stdint.h>
#include "translator.h"

// Configuration file path on PS4
#define ONEFAMILY_CONFIG_PATH   "/data/GoldHEN/onefamily.ini"
#define XBOX_CONFIG_PATH        "/data/GoldHEN/xbox_controller.ini"
#define ONEFAMILY_CONFIG_BUFFER_SIZE 4096
#define XBOX_CONFIG_BUFFER_SIZE 4096

// Mode selection
typedef enum {
    CONFIG_MODE_AUTO = 0,       // Merge if 1 user, Secondary if >= 2 users (v1.4.0 default)
    CONFIG_MODE_MERGE,          // Force Player 1 (Merge)
    CONFIG_MODE_SECONDARY       // Force Player 2 (Secondary)
} ConfigMode;

// Guide button action
typedef enum {
    CONFIG_GUIDE_TOUCHPAD = 0,  // DS4_BUTTON_TOUCHPAD (v1.4.0 default)
    CONFIG_GUIDE_SHARE,         // DS4_BUTTON_OPTIONS / SHARE
    CONFIG_GUIDE_NONE           // Do nothing
} ConfigGuideAction;

// Complete plugin configuration
typedef struct {
    // [General]
    ConfigMode          mode;
    ConfigGuideAction   guide_action;
    int                 enable_notifications;   // 1 = enabled (default), 0 = disabled
    int                 enable_mode_combo;      // 1 = enabled (default), 0 = disabled
    int                 enable_rumble;          // 1 = enabled (default), 0 = disabled
    int                 enable_homebrew;        // 1 = enabled (default), 0 = disabled (retail games only)

    // [Deadzone]
    uint8_t             stick_deadzone;         // 0-127 (default: 15 ~12%)
    uint8_t             trigger_threshold;      // 0-255 (default: 30)

    // [Remap]
    int                 swap_triggers_bumpers;  // 0 = normal (default), 1 = swapped
    int                 switch_positional;      // 1 = positional (default), 0 = by letter
} PluginConfig;

// Global configuration instance
extern PluginConfig g_config;

// Initialize config with v1.4.0 defaults
void config_init_defaults(PluginConfig *cfg);

// Load configuration from /data/GoldHEN/xbox_controller.ini
// If file does not exist, defaults are retained without any errors.
void config_load(PluginConfig *cfg);

#endif // INI_PARSER_H
