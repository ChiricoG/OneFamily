/*
 * OneFamily Plugin & Daemon for PS4 (GoldHEN)
 * Configuration structures and INI loader
 */

#ifndef INI_PARSER_H
#define INI_PARSER_H

#include <stdint.h>

// Configuration file paths on PS4 (/data with /user/data fallback for FW 5.05)
#define ONEFAMILY_CONFIG_PATH               "/data/onefamily/onefamily.ini"
#define ONEFAMILY_CONFIG_PATH_USER          "/user/data/onefamily/onefamily.ini"
#define ONEFAMILY_LEGACY_CONFIG_PATH        "/data/GoldHEN/onefamily.ini"
#define ONEFAMILY_LEGACY_CONFIG_PATH_USER   "/user/data/GoldHEN/onefamily.ini"
#define XBOX_CONFIG_PATH                    "/data/GoldHEN/xbox_controller.ini"
#define XBOX_CONFIG_PATH_USER               "/user/data/GoldHEN/xbox_controller.ini"
#define ONEFAMILY_CONFIG_BUFFER_SIZE 4096
#define XBOX_CONFIG_BUFFER_SIZE 4096

// Mode selection
#ifndef CONFIG_MODE_DEFINED
#define CONFIG_MODE_DEFINED
typedef enum {
    CONFIG_MODE_AUTO = 0,       // Merge if 1 user, Secondary if >= 2 users (v1.4.0 default)
    CONFIG_MODE_MERGE,          // Force Player 1 (Merge)
    CONFIG_MODE_SECONDARY       // Force Player 2 (Secondary)
} ConfigMode;
#endif

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


    // [DualSense]
    int                 dualsense_lightbar_mode;  // 0=Game Sync, 1=Static Color, 2=Battery, 3=Off
    int                 dualsense_lightbar_color; // 0..5 (Blue, Red, Green, Purple, Cyan, White)
    int                 dualsense_brightness;     // 0=High, 1=Medium, 2=Low
    int                 dualsense_trigger_mode;   // 0=Standard/Off, 1=Hair Trigger, 2=Resistance, 3=Vibration
    int                 dualsense_player_leds;    // 0=Player Slot, 1=All On, 2=Off
    int                 dualsense_mute_action;    // 0=Default, 1=Touchpad Click
    int                 dualsense_speaker_volume; // 0..100 (Default: 80)
} PluginConfig;

// Global configuration instance
extern PluginConfig g_config;

// Initialize config with v1.4.0 defaults
void config_init_defaults(PluginConfig *cfg);

// Load configuration from /data/GoldHEN/xbox_controller.ini
// If file does not exist, defaults are retained without any errors.
void config_load(PluginConfig *cfg);

#endif // INI_PARSER_H
