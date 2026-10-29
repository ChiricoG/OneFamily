#pragma once

#include "common/graphics.h"
#include "config_manager.h"
#include "installer.h"
#include "hid_profile.h"
#include <orbis/Pad.h>
#include <string>

struct ImageRGBA {
    int width;
    int height;
    uint32_t* pixels;
    ImageRGBA() : width(0), height(0), pixels(NULL) {}
};

struct CalibrationState {
    bool active;
    bool completed;
    int currentFrame;
    int totalFrames;
    int maxNoise;
    int suggestedIndex;

    CalibrationState() : active(false), completed(false), currentFrame(0),
                         totalFrames(180), maxNoise(0), suggestedIndex(4) {}
};

struct TesterState {
    int rumbleFrames;       // countdown for active rumble test (e.g. 180 frames = 3s)
    int rumblePhase;        // 0 = none, 1 = left (heavy), 2 = right (light), 3 = both
    CalibrationState calib;

    TesterState() : rumbleFrames(0), rumblePhase(0) {}
};

class UI {
public:
    UI();
    ~UI();

    bool init(Scene2D* scene);
    void render(Scene2D* scene, const AppConfig& cfg, int activeTab, int selectedCard,
                int toastFrames, const OrbisPadData& padData, const TesterState& tester,
                const char* toastMsg = NULL,
                bool hidConnected = false, const HidStatus* hidStatus = NULL,
                const HidProfile* hidProfile = NULL, int remapSelectedRow = 0,
                bool remapTestMode = false);
    void render_wizard(Scene2D* scene, const InstallResult& res, int frameID);

private:
    FT_Face fontTitle;
    FT_Face fontSubtitle;
    FT_Face fontCardTitle;
    FT_Face fontCardDesc;
    FT_Face fontValue;
    FT_Face fontFooter;

    uint32_t* cached_bg;
    int bg_width;
    int bg_height;

    ImageRGBA xboxLogo;
    ImageRGBA iconGear;
    ImageRGBA iconInfo;
    ImageRGBA iconSave;
    ImageRGBA iconMode;
    ImageRGBA iconGuide;
    ImageRGBA iconDeadzone;
    ImageRGBA iconSwap;
    ImageRGBA iconRumble;
    ImageRGBA iconRemap;
    ImageRGBA btnArrows;
    ImageRGBA btnCross;
    ImageRGBA btnCircle;
    ImageRGBA btnSquare;
    ImageRGBA btnTriangle;
    ImageRGBA btnOptions;
    ImageRGBA btnShare;
    ImageRGBA btnPs;
    ImageRGBA btnTouchpad;
    ImageRGBA btnL1;
    ImageRGBA btnR1;
    ImageRGBA btnL2;
    ImageRGBA btnR2;
    ImageRGBA btnL3;
    ImageRGBA btnR3;

    ImageRGBA load_icon(const char* path);
    void free_icon(ImageRGBA& img);
    void draw_image(Scene2D* scene, const ImageRGBA& img, int startX, int startY);

    void draw_rounded_rect(Scene2D* scene, int x, int y, int w, int h, int r, Color color);
    void draw_rounded_outline(Scene2D* scene, int x, int y, int w, int h, int r, Color color, int thickness);
    void draw_circle(Scene2D* scene, int centerX, int centerY, int radius, Color color);
    void draw_circle_outline(Scene2D* scene, int centerX, int centerY, int radius, Color color, int thickness);
    void draw_btn_badge(Scene2D* scene, int x, int y, int w, int h, const char* label, bool pressed);
    void draw_text(Scene2D* scene, const std::string& text, FT_Face face, int startX, int baselineY, Color color);
    void draw_text_centered(Scene2D* scene, const std::string& text, FT_Face face, int centerX, int baselineY, Color color);

    void render_tester(Scene2D* scene, const AppConfig& cfg, const OrbisPadData& padData, const TesterState& tester);
    void render_remap(Scene2D* scene, const HidStatus& status, const HidProfile& profile,
                      int selectedRow, const OrbisPadData& padData, bool testMode = false);
};
