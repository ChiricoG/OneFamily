#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#define INI_FILE_PATH              "/data/onefamily/onefamily.ini"
#define INI_FILE_PATH_USER         "/user/data/onefamily/onefamily.ini"
#define LEGACY_INI_FILE_PATH       "/data/GoldHEN/onefamily.ini"
#define LEGACY_INI_FILE_PATH_USER  "/user/data/GoldHEN/onefamily.ini"
#define OLD_LEGACY_INI_FILE_PATH   "/data/GoldHEN/xbox_controller.ini"
#define OLD_LEGACY_INI_PATH_USER   "/user/data/GoldHEN/xbox_controller.ini"

struct AppConfig {
    int deadzone_index;     // 0..7 (0%, 5%, 8%, 10%, 12%, 15%, 20%, 25%)
    int trigger_index;      // 0..4 (15, 30, 45, 60, 80)
    int swap_index;         // 0 = Disabled, 1 = Enabled
    int rumble_index;       // 0 = Enabled, 1 = Disabled
    int switch_index;       // 0 = Positional, 1 = By Letter

    // [DualSense]
    int ds_bt_mode;         // 0=Safe Mode (10-byte Report 0x01), 1=Extended Mode (78-byte Report 0x31)
    int ds_lightbar_mode;   // 0=Game Sync, 1=Static Color, 2=Battery, 3=Off
    int ds_lightbar_color;  // 0=PlayStation Blue, 1=Red, 2=Green, 3=Purple, 4=Cyan, 5=White
    int ds_brightness;      // 0=High, 1=Medium, 2=Low
    int ds_trigger_mode;    // 0=Standard / Off, 1=Hair Trigger, 2=Resistance, 3=Vibration
    int ds_player_leds;     // 0=Player Slot, 1=All On, 2=Off
    int ds_mute_action;     // 0=Default, 1=Touchpad Click
    int ds_speaker_volume;  // 0..100 (Default: 80)

    AppConfig() {
        set_defaults();
    }

    void set_defaults() {
        deadzone_index = 4;     // 12% (15/127)
        trigger_index = 1;      // 30 (12%)
        swap_index = 0;         // Disabled
        rumble_index = 0;       // Enabled
        switch_index = 0;       // Positional
        ds_bt_mode = 1;         // Extended Mode (78-byte Report 0x31)
        ds_lightbar_mode = 0;
        ds_lightbar_color = 0;
        ds_brightness = 0;
        ds_trigger_mode = 0;
        ds_player_leds = 0;
        ds_mute_action = 0;
        ds_speaker_volume = 80;
    }
};

class ConfigManager {
public:
    static const std::vector<std::string> DEADZONE_OPTIONS;
    static const std::vector<int>         DEADZONE_VALUES;
    static const std::vector<std::string> TRIGGER_OPTIONS;
    static const std::vector<int>         TRIGGER_VALUES;
    static const std::vector<std::string> SWAP_OPTIONS;
    static const std::vector<std::string> RUMBLE_OPTIONS;
    static const std::vector<std::string> SWITCH_OPTIONS;

    // DualSense Options
    static const std::vector<std::string> DS_BT_MODE_OPTIONS;
    static const std::vector<std::string> DS_LIGHTBAR_OPTIONS;
    static const std::vector<std::string> DS_COLOR_OPTIONS;
    static const std::vector<std::string> DS_BRIGHTNESS_OPTIONS;
    static const std::vector<std::string> DS_TRIGGER_OPTIONS;
    static const std::vector<std::string> DS_PLAYER_LEDS_OPTIONS;
    static const std::vector<std::string> DS_MUTE_OPTIONS;

    ConfigManager();
    bool load();
    bool save();

    AppConfig& getConfig() { return config; }

private:
    AppConfig config;
    bool file_exists(const char* path);
};
