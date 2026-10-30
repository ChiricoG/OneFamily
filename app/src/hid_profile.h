#pragma once

#include <stdint.h>
#include <string>
#include <vector>

struct HidStatus {
    bool connected;
    std::string type;
    uint16_t vid;
    uint16_t pid;
    std::string name;
    int nbuttons;
    int player_slot;
    std::string user_name;

    HidStatus() : connected(false), type("none"), vid(0), pid(0), name("Unknown"), nbuttons(16), player_slot(1), user_name("Player 1") {}
};

enum Ds4ActionId {
    ACT_CROSS = 0,
    ACT_CIRCLE,
    ACT_SQUARE,
    ACT_TRIANGLE,
    ACT_L1,
    ACT_R1,
    ACT_L2,
    ACT_R2,
    ACT_SHARE,
    ACT_OPTIONS,
    ACT_PS,
    ACT_TOUCHPAD,
    ACT_L3,
    ACT_R3,
    ACT_COUNT
};

struct Ds4ActionMeta {
    Ds4ActionId id;
    const char* key_name;      // "cross", "circle", etc.
    const char* display_name;  // "Cross (X)", "Circle (O)", etc.
    uint32_t ds4_bit;
    const char* icon_path;
};

class HidProfile {
public:
    static const Ds4ActionMeta ACTIONS[ACT_COUNT];

    HidProfile();

    static bool read_status(HidStatus& out);
    bool load(uint16_t vid, uint16_t pid, int nbuttons);
    bool save(uint16_t vid, uint16_t pid, const std::string& name);
    void reset_defaults(int nbuttons);

    int get_assigned_button(int action_idx) const; // 0 = None, 1..32 = button1..button32
    void set_assigned_button(int action_idx, int btn);
    void cycle_button(int action_idx, int dir); // dir: +1 or -1

    int get_max_buttons() const { return max_buttons; }

private:
    int assignments[ACT_COUNT]; // Stores button number (0 = none, 1 = button1, etc.)
    int max_buttons;
    std::string current_path;

    static bool file_exists(const char* path);
};
