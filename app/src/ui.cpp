#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define BG_IMAGE_PATH   "/app0/assets/images/bg_native.png"
#define FONT_PATH       "/app0/assets/fonts/font.ttf"

// Layout Constants
static const int SIDEBAR_X = 60;
static const int SIDEBAR_W = 310;
static const int CARD_X = 410;
static const int CARD_W = 1140;
static const int CARD_H = 92;
static const int CARD_Y_START = 280;
static const int CARD_Y_STEP = 104;

// Colors - PlayStation Dark Blue Palette
static const Color COLOR_BLUE_GLOW   = { 10, 50, 110 };
static const Color COLOR_BLUE_MAIN   = { 0, 112, 220 };
static const Color COLOR_BLUE_BRIGHT = { 45, 160, 255 };
static const Color COLOR_WHITE       = { 255, 255, 255 };
static const Color COLOR_TEXT_DIM    = { 140, 160, 180 };
static const Color COLOR_CARD_BG     = { 12, 20, 32 };
static const Color COLOR_CARD_BORDER = { 22, 38, 58 };
static const Color COLOR_DIVIDER     = { 18, 32, 50 };
static const Color COLOR_TOAST_BG    = { 10, 28, 56 };
static const Color COLOR_TOAST_BORDER= { 0, 130, 240 };
static const Color COLOR_SIDEBAR_BLUE= { 14, 48, 95 };
static const Color COLOR_SIDEBAR_DARK= { 10, 18, 30 };
static const Color COLOR_BORDER_DIM  = { 22, 38, 58 };

UI::UI() : fontTitle(NULL), fontSubtitle(NULL), fontCardTitle(NULL),
           fontCardDesc(NULL), fontValue(NULL), fontFooter(NULL),
           cached_bg(NULL), bg_width(0), bg_height(0) {}

UI::~UI() {
    if (cached_bg) {
        free(cached_bg);
        cached_bg = NULL;
    }
    free_icon(xboxLogo);
    free_icon(iconGear);
    free_icon(iconInfo);
    free_icon(iconSave);
    free_icon(iconMode);
    free_icon(iconGuide);
    free_icon(iconDeadzone);
    free_icon(iconSwap);
    free_icon(iconRumble);
    free_icon(btnArrows);
    free_icon(btnCross);
    free_icon(btnSquare);
    free_icon(btnTriangle);
    free_icon(btnOptions);
    free_icon(btnL1);
    free_icon(btnR1);
}

ImageRGBA UI::load_icon(const char* path) {
    ImageRGBA img;
    int channels = 0;
    unsigned char* data = stbi_load(path, &img.width, &img.height, &channels, 4);
    if (data) {
        int total = img.width * img.height;
        img.pixels = (uint32_t*)malloc(total * sizeof(uint32_t));
        if (img.pixels) {
            uint32_t* src32 = (uint32_t*)data;
            for (int i = 0; i < total; i++) {
                uint32_t p = src32[i];
                uint8_t r = (uint8_t)(p >> 0);
                uint8_t g = (uint8_t)(p >> 8);
                uint8_t b = (uint8_t)(p >> 16);
                uint8_t a = (uint8_t)(p >> 24);
                img.pixels[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
        stbi_image_free(data);
    }
    return img;
}

void UI::free_icon(ImageRGBA& img) {
    if (img.pixels) {
        free(img.pixels);
        img.pixels = NULL;
    }
}

void UI::draw_image(Scene2D* scene, const ImageRGBA& img, int startX, int startY) {
    if (!scene || !img.pixels) return;
    for (int y = 0; y < img.height; y++) {
        int py = startY + y;
        if (py < 0 || py >= 1080) continue;
        for (int x = 0; x < img.width; x++) {
            int px = startX + x;
            if (px < 0 || px >= 1920) continue;
            uint32_t p = img.pixels[y * img.width + x];
            uint8_t a = (uint8_t)(p >> 24);
            if (a == 0) continue;
            uint8_t r = (uint8_t)(p >> 16);
            uint8_t g = (uint8_t)(p >> 8);
            uint8_t b = (uint8_t)(p >> 0);
            Color c = { r, g, b };
            scene->DrawPixelBlend(px, py, c, a);
        }
    }
}

bool UI::init(Scene2D* scene) {
    if (!scene) return false;

    // 1. Load clean native background (deep slate-black wallpaper with ambient green glow & controller silhouette)
    int channels = 0;
    unsigned char* data = stbi_load(BG_IMAGE_PATH, &bg_width, &bg_height, &channels, 4);
    if (data && bg_width == 1920 && bg_height == 1080) {
        cached_bg = (uint32_t*)malloc(1920 * 1080 * 4);
        if (cached_bg) {
            uint32_t* src32 = (uint32_t*)data;
            for (int i = 0; i < 1920 * 1080; i++) {
                uint32_t pixel = src32[i];
                uint8_t r = (uint8_t)(pixel >> 0);
                uint8_t g = (uint8_t)(pixel >> 8);
                uint8_t b = (uint8_t)(pixel >> 16);
                cached_bg[i] = 0x80000000 | (r << 16) | (g << 8) | b;
            }
        }
        stbi_image_free(data);
    }

    // 2. Load separate clean vector-crisp icons
    xboxLogo      = load_icon("/app0/assets/icons/xbox_logo.png");
    iconGear      = load_icon("/app0/assets/icons/icon_gear.png");
    iconInfo      = load_icon("/app0/assets/icons/icon_info.png");
    iconSave      = load_icon("/app0/assets/icons/icon_save.png");
    iconMode      = load_icon("/app0/assets/icons/card_icon_mode.png");
    iconGuide     = load_icon("/app0/assets/icons/card_icon_guide.png");
    iconDeadzone  = load_icon("/app0/assets/icons/card_icon_deadzone.png");
    iconSwap      = load_icon("/app0/assets/icons/card_icon_swap.png");
    iconRumble    = load_icon("/app0/assets/icons/card_icon_rumble.png");
    btnArrows     = load_icon("/app0/assets/icons/btn_arrows.png");
    btnCross      = load_icon("/app0/assets/icons/btn_cross_blue.png");
    btnSquare     = load_icon("/app0/assets/icons/btn_square.png");
    btnTriangle   = load_icon("/app0/assets/icons/btn_triangle.png");
    btnOptions    = load_icon("/app0/assets/icons/btn_options.png");
    btnL1         = load_icon("/app0/assets/icons/btn_l1.png");
    btnR1         = load_icon("/app0/assets/icons/btn_r1.png");

    // 3. Initialize FreeType fonts with legible TV sizes
    scene->InitFont(&fontTitle,     FONT_PATH, 38);
    scene->InitFont(&fontSubtitle,  FONT_PATH, 20);
    scene->InitFont(&fontCardTitle, FONT_PATH, 24);
    scene->InitFont(&fontCardDesc,  FONT_PATH, 18);
    scene->InitFont(&fontValue,     FONT_PATH, 22);
    scene->InitFont(&fontFooter,    FONT_PATH, 18);

    return true;
}

void UI::draw_rounded_rect(Scene2D* scene, int x, int y, int w, int h, int r, Color color) {
    if (!scene) return;
    for (int row = 0; row < h; row++) {
        int dx = r;
        if (row < r) {
            int dy = r - 1 - row;
            dx = (int)round(sqrt((double)(r * r - dy * dy)));
        } else if (row >= h - r) {
            int dy = row - (h - r);
            dx = (int)round(sqrt((double)(r * r - dy * dy)));
        }
        int startX = x + (r - dx);
        int spanW = w - 2 * (r - dx);
        if (spanW > 0) {
            scene->DrawRectangle(startX, y + row, spanW, 1, color);
        }
    }
}

void UI::draw_rounded_outline(Scene2D* scene, int x, int y, int w, int h, int r, Color color, int thickness) {
    if (!scene) return;
    for (int t = 0; t < thickness; t++) {
        int curX = x + t;
        int curY = y + t;
        int curW = w - 2 * t;
        int curH = h - 2 * t;
        int curR = (r > t) ? (r - t) : 1;
        for (int row = 0; row < curH; row++) {
            int dx = curR;
            if (row < curR) {
                int dy = curR - 1 - row;
                dx = (int)round(sqrt((double)(curR * curR - dy * dy)));
            } else if (row >= curH - curR) {
                int dy = row - (curH - curR);
                dx = (int)round(sqrt((double)(curR * curR - dy * dy)));
            }
            int startX = curX + (curR - dx);
            int spanW = curW - 2 * (curR - dx);
            if (spanW > 0) {
                scene->DrawPixel(startX, curY + row, color);
                scene->DrawPixel(startX + spanW - 1, curY + row, color);
            }
        }
        // Ensure continuous top and bottom borders
        scene->DrawRectangle(curX + curR, curY, curW - 2 * curR, 1, color);
        scene->DrawRectangle(curX + curR, curY + curH - 1, curW - 2 * curR, 1, color);
    }
}

void UI::draw_circle(Scene2D* scene, int centerX, int centerY, int radius, Color color) {
    if (!scene || radius <= 0) return;
    int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        int py = centerY + dy;
        if (py < 0 || py >= 1080) continue;
        int dx = (int)sqrt((double)(r2 - dy * dy));
        int startX = centerX - dx;
        int w = 2 * dx + 1;
        if (startX < 0) {
            w += startX;
            startX = 0;
        }
        if (startX + w > 1920) {
            w = 1920 - startX;
        }
        if (w > 0) {
            scene->DrawRectangle(startX, py, w, 1, color);
        }
    }
}

void UI::draw_circle_outline(Scene2D* scene, int centerX, int centerY, int radius, Color color, int thickness) {
    if (!scene || radius <= 0 || thickness <= 0) return;
    int r_outer2 = radius * radius;
    int r_inner = radius - thickness;
    int r_inner2 = (r_inner > 0) ? (r_inner * r_inner) : 0;

    for (int dy = -radius; dy <= radius; dy++) {
        int py = centerY + dy;
        if (py < 0 || py >= 1080) continue;
        int dy2 = dy * dy;
        if (dy2 > r_outer2) continue;

        int dx_outer = (int)sqrt((double)(r_outer2 - dy2));
        int dx_inner = (dy2 < r_inner2) ? (int)sqrt((double)(r_inner2 - dy2)) : 0;

        // Left segment
        int leftX = centerX - dx_outer;
        int leftW = dx_outer - dx_inner;
        if (leftX < 0) { leftW += leftX; leftX = 0; }
        if (leftX + leftW > 1920) { leftW = 1920 - leftX; }
        if (leftW > 0) {
            scene->DrawRectangle(leftX, py, leftW, 1, color);
        }

        // Right segment
        int rightX = centerX + dx_inner;
        int rightW = dx_outer - dx_inner;
        if (rightX < 0) { rightW += rightX; rightX = 0; }
        if (rightX + rightW > 1920) { rightW = 1920 - rightX; }
        if (rightW > 0) {
            scene->DrawRectangle(rightX, py, rightW, 1, color);
        }
    }
}

void UI::draw_text(Scene2D* scene, const std::string& text, FT_Face face, int startX, int baselineY, Color color) {
    if (!scene || !face) return;
    Color dummyBg = { 0, 0, 0 };
    scene->DrawText((char*)text.c_str(), face, startX, baselineY, dummyBg, color);
}

void UI::draw_text_centered(Scene2D* scene, const std::string& text, FT_Face face, int centerX, int baselineY, Color color) {
    if (!scene || !face) return;
    int totalWidth = 0;
    for (size_t i = 0; i < text.length(); i++) {
        FT_UInt glyph_idx = FT_Get_Char_Index(face, text[i]);
        if (FT_Load_Glyph(face, glyph_idx, FT_LOAD_DEFAULT) == 0) {
            totalWidth += (face->glyph->advance.x >> 6);
        }
    }
    int startX = centerX - (totalWidth / 2);
    Color dummyBg = { 0, 0, 0 };
    scene->DrawText((char*)text.c_str(), face, startX, baselineY, dummyBg, color);
}

void UI::draw_btn_badge(Scene2D* scene, int x, int y, int w, int h, const char* label, bool pressed) {
    if (!scene) return;
    Color bg = pressed ? Color{ 0, 95, 190 } : Color{ 18, 30, 48 };
    Color border = pressed ? COLOR_BLUE_BRIGHT : COLOR_BORDER_DIM;
    draw_rounded_rect(scene, x, y, w, h, 6, bg);
    draw_rounded_outline(scene, x, y, w, h, 6, border, 1);
    draw_text_centered(scene, label, fontCardDesc, x + w / 2, y + 21, pressed ? COLOR_WHITE : COLOR_TEXT_DIM);
}

void UI::render(Scene2D* scene, const AppConfig& cfg, int activeTab, int selectedCard,
                int toastFrames, const OrbisPadData& padData, const TesterState& tester,
                const char* toastMsg) {
    if (!scene) return;

    // 1. Blit clean native background (pure wallpaper)
    if (cached_bg) {
        scene->CopyToActiveBuffer(cached_bg);
    }

    // 2. Header
    draw_image(scene, xboxLogo, 60, 120);
    draw_text(scene, "OneFamily", fontTitle, 175, 145, COLOR_WHITE);
    draw_text(scene, "Companion", fontTitle, 175, 188, COLOR_BLUE_BRIGHT);
    draw_text(scene, "No more console war - we are a family!", fontSubtitle, 175, 222, COLOR_TEXT_DIM);

    // 3. Left Sidebar (3 Tabs)
    // Tab 0: Settings (Y = 280)
    if (activeTab == 0) {
        draw_rounded_rect(scene, SIDEBAR_X, 280, SIDEBAR_W, 54, 10, COLOR_SIDEBAR_BLUE);
        draw_rounded_outline(scene, SIDEBAR_X, 280, SIDEBAR_W, 54, 10, COLOR_BLUE_MAIN, 2);
        draw_image(scene, iconGear, SIDEBAR_X + 16, 285);
        draw_text(scene, "Settings", fontCardTitle, SIDEBAR_X + 72, 316, COLOR_WHITE);
    } else {
        draw_rounded_rect(scene, SIDEBAR_X, 280, SIDEBAR_W, 54, 10, COLOR_SIDEBAR_DARK);
        draw_rounded_outline(scene, SIDEBAR_X, 280, SIDEBAR_W, 54, 10, COLOR_BORDER_DIM, 1);
        draw_image(scene, iconGear, SIDEBAR_X + 16, 285);
        draw_text(scene, "Settings", fontCardTitle, SIDEBAR_X + 72, 316, COLOR_TEXT_DIM);
    }
    draw_image(scene, btnL1, SIDEBAR_X + 250, 292);

    // Tab 1: Input Tester (Y = 348)
    if (activeTab == 1) {
        draw_rounded_rect(scene, SIDEBAR_X, 348, SIDEBAR_W, 54, 10, COLOR_SIDEBAR_BLUE);
        draw_rounded_outline(scene, SIDEBAR_X, 348, SIDEBAR_W, 54, 10, COLOR_BLUE_MAIN, 2);
        draw_image(scene, iconMode, SIDEBAR_X + 16, 353);
        draw_text(scene, "Input Tester", fontCardTitle, SIDEBAR_X + 72, 384, COLOR_WHITE);
    } else {
        draw_rounded_rect(scene, SIDEBAR_X, 348, SIDEBAR_W, 54, 10, COLOR_SIDEBAR_DARK);
        draw_rounded_outline(scene, SIDEBAR_X, 348, SIDEBAR_W, 54, 10, COLOR_BORDER_DIM, 1);
        draw_image(scene, iconMode, SIDEBAR_X + 16, 353);
        draw_text(scene, "Input Tester", fontCardTitle, SIDEBAR_X + 72, 384, COLOR_TEXT_DIM);
    }

    // Tab 2: Info (Y = 416)
    if (activeTab == 2) {
        draw_rounded_rect(scene, SIDEBAR_X, 416, SIDEBAR_W, 54, 10, COLOR_SIDEBAR_BLUE);
        draw_rounded_outline(scene, SIDEBAR_X, 416, SIDEBAR_W, 54, 10, COLOR_BLUE_MAIN, 2);
        draw_image(scene, iconInfo, SIDEBAR_X + 16, 421);
        draw_text(scene, "Info", fontCardTitle, SIDEBAR_X + 72, 452, COLOR_WHITE);
    } else {
        draw_rounded_rect(scene, SIDEBAR_X, 416, SIDEBAR_W, 54, 10, COLOR_SIDEBAR_DARK);
        draw_rounded_outline(scene, SIDEBAR_X, 416, SIDEBAR_W, 54, 10, COLOR_BORDER_DIM, 1);
        draw_image(scene, iconInfo, SIDEBAR_X + 16, 421);
        draw_text(scene, "Info", fontCardTitle, SIDEBAR_X + 72, 452, COLOR_TEXT_DIM);
    }
    draw_image(scene, btnR1, SIDEBAR_X + 250, 428);

    // 4. Main Content Area
    if (activeTab == 0) {
        // --- SETTINGS VIEW (5 Cards) ---
        const int TOTAL_CARDS = 5;
        const int VISIBLE_CARDS = 5;

        int scrollOffset = 0;
        if (selectedCard >= VISIBLE_CARDS) {
            scrollOffset = selectedCard - VISIBLE_CARDS + 1;
        }
        if (scrollOffset > TOTAL_CARDS - VISIBLE_CARDS) {
            scrollOffset = TOTAL_CARDS - VISIBLE_CARDS;
        }

        const char* cardTitles[TOTAL_CARDS] = {
            "Stick Deadzone",
            "Trigger Sensitivity",
            "Swap Bumpers & Triggers",
            "Controller Vibration",
            "Switch Controller Layout"
        };

        const char* cardDescs[TOTAL_CARDS] = {
            "Adjust analog stick inner deadzone to counter stick drift.",
            "Digital press activation threshold for analog triggers.",
            "Invert bumper (LB/RB) and trigger (LT/RT) functions.",
            "Enable or disable controller rumble feedback.",
            "Positional (bottom=X) or by letter (A=Circle, B=Cross)."
        };

        ImageRGBA cardIcons[TOTAL_CARDS] = {
            iconDeadzone, iconDeadzone, iconSwap,
            iconRumble, iconSwap
        };

        std::string vals[TOTAL_CARDS];
        vals[0] = ConfigManager::DEADZONE_OPTIONS[cfg.deadzone_index];
        vals[1] = ConfigManager::TRIGGER_OPTIONS[cfg.trigger_index];
        vals[2] = ConfigManager::SWAP_OPTIONS[cfg.swap_index];
        vals[3] = ConfigManager::RUMBLE_OPTIONS[cfg.rumble_index];
        vals[4] = ConfigManager::SWITCH_OPTIONS[cfg.switch_index];

        int selectorCenterX = 1380;

        for (int i = 0; i < VISIBLE_CARDS; i++) {
            int cardIndex = scrollOffset + i;
            int cardY = CARD_Y_START + (i * CARD_Y_STEP);
            bool isSelected = (cardIndex == selectedCard);

            // Procedural card background
            draw_rounded_rect(scene, CARD_X, cardY, CARD_W, CARD_H, 12, COLOR_CARD_BG);

            if (isSelected) {
                draw_rounded_outline(scene, CARD_X - 1, cardY - 1, CARD_W + 2, CARD_H + 2, 12, COLOR_BLUE_GLOW, 2);
                draw_rounded_outline(scene, CARD_X, cardY, CARD_W, CARD_H, 12, COLOR_BLUE_MAIN, 2);
            } else {
                draw_rounded_outline(scene, CARD_X, cardY, CARD_W, CARD_H, 12, COLOR_CARD_BORDER, 1);
            }

            // Draw card icon
            draw_image(scene, cardIcons[cardIndex], CARD_X + 20, cardY + 22);

            // Draw text
            draw_text(scene, cardTitles[cardIndex], fontCardTitle, CARD_X + 85, cardY + 38, COLOR_WHITE);
            draw_text(scene, cardDescs[cardIndex],  fontCardDesc,  CARD_X + 85, cardY + 68, COLOR_TEXT_DIM);

            // Draw value selector
            Color textColor  = isSelected ? COLOR_BLUE_BRIGHT : COLOR_WHITE;
            Color arrowColor = isSelected ? COLOR_WHITE : COLOR_TEXT_DIM;

            std::string fullStr = "[   " + vals[cardIndex] + "   ]";
            draw_text_centered(scene, fullStr, fontValue, selectorCenterX, cardY + 54, textColor);

            draw_text(scene, "<", fontValue, selectorCenterX - 130, cardY + 54, arrowColor);
            draw_text(scene, ">", fontValue, selectorCenterX + 120, cardY + 54, arrowColor);
        }

        // Vertical Scrollbar on the right of cards
        int scrollTrackX = CARD_X + CARD_W + 16;
        int scrollTrackY = CARD_Y_START;
        int scrollTrackH = (VISIBLE_CARDS * CARD_Y_STEP) - 12; // 508px
        int scrollTrackW = 6;
        draw_rounded_rect(scene, scrollTrackX, scrollTrackY, scrollTrackW, scrollTrackH, 3, COLOR_CARD_BORDER);

        int thumbH = (scrollTrackH * VISIBLE_CARDS) / TOTAL_CARDS;
        int maxThumbTravel = scrollTrackH - thumbH;
        int maxScrollOffset = TOTAL_CARDS - VISIBLE_CARDS;
        int thumbY = scrollTrackY + (maxScrollOffset > 0 ? (scrollOffset * maxThumbTravel) / maxScrollOffset : 0);
        draw_rounded_rect(scene, scrollTrackX, thumbY, scrollTrackW, thumbH, 3, COLOR_BLUE_MAIN);

        // Position Indicator text below scrollbar: e.g. "3 / 5"
        char posStr[16];
        snprintf(posStr, sizeof(posStr), "%d / %d", selectedCard + 1, TOTAL_CARDS);
        draw_text_centered(scene, posStr, fontFooter, scrollTrackX + 3, scrollTrackY + scrollTrackH + 24, COLOR_TEXT_DIM);

        // 5. Footer for Settings
        scene->DrawRectangle(60, 865, 1800, 2, COLOR_DIVIDER);

        draw_image(scene, iconSave, 60, 890);
        draw_text(scene, "Save Settings", fontCardTitle, 120, 912, COLOR_WHITE);
        draw_text(scene, "Configuration will be written to /data/GoldHEN/onefamily.ini", fontFooter, 120, 938, COLOR_TEXT_DIM);

        // Navigation Hints
        draw_image(scene, btnArrows, 1370, 901);
        draw_text(scene, "Change", fontFooter, 1424, 922, COLOR_TEXT_DIM);

        draw_image(scene, btnCross, 1540, 897);
        draw_text(scene, "Save", fontFooter, 1585, 922, COLOR_BLUE_BRIGHT);

        draw_image(scene, btnOptions, 1690, 900);
        draw_text(scene, "Exit", fontFooter, 1770, 922, COLOR_TEXT_DIM);

    } else if (activeTab == 1) {
        // --- INPUT TESTER & CALIBRATION VIEW ---
        render_tester(scene, cfg, padData, tester);

    } else {
        // --- INFO VIEW ---
        // Info Card 1: Overview & Version
        draw_rounded_rect(scene, CARD_X, 280, CARD_W, 160, 12, COLOR_CARD_BG);
        draw_rounded_outline(scene, CARD_X, 280, CARD_W, 160, 12, COLOR_CARD_BORDER, 1);
        draw_image(scene, iconInfo, CARD_X + 24, 305);
        draw_text(scene, "OneFamily Companion", fontCardTitle, CARD_X + 90, 318, COLOR_WHITE);
        draw_text(scene, "Version: v1.1.0 (Native System-Wide Virtual Pad & XMB Control)", fontCardDesc, CARD_X + 90, 350, COLOR_BLUE_BRIGHT);
        draw_text(scene, "Configuration File: /data/GoldHEN/onefamily.ini", fontCardDesc, CARD_X + 90, 380, COLOR_TEXT_DIM);
        draw_text(scene, "Developer: ChiricoG - OneFamily Project", fontCardDesc, CARD_X + 90, 410, COLOR_TEXT_DIM);

        // Info Card 2: Supported Controllers & Connectivity
        draw_rounded_rect(scene, CARD_X, 460, CARD_W, 160, 12, COLOR_CARD_BG);
        draw_rounded_outline(scene, CARD_X, 460, CARD_W, 160, 12, COLOR_CARD_BORDER, 1);
        draw_image(scene, iconMode, CARD_X + 24, 485);
        draw_text(scene, "Supported Hardware & Connection", fontCardTitle, CARD_X + 90, 498, COLOR_WHITE);
        draw_text(scene, "- Compatible with Xbox One, Series X|S, 360, Switch, PS3, and PS5.", fontCardDesc, CARD_X + 90, 530, COLOR_TEXT_DIM);
        draw_text(scene, "- Connect to any USB port on your PS4 console using a standard USB cable.", fontCardDesc, CARD_X + 90, 560, COLOR_TEXT_DIM);
        draw_text(scene, "- Plug-and-play: recognized immediately by GoldHEN with on-screen popups.", fontCardDesc, CARD_X + 90, 590, COLOR_TEXT_DIM);

        // Info Card 3: Modes & Shortcuts
        draw_rounded_rect(scene, CARD_X, 640, CARD_W, 190, 12, COLOR_CARD_BG);
        draw_rounded_outline(scene, CARD_X, 640, CARD_W, 190, 12, COLOR_CARD_BORDER, 1);
        draw_image(scene, iconGuide, CARD_X + 24, 665);
        draw_text(scene, "Virtual Pad Architecture (No Plugins Needed)", fontCardTitle, CARD_X + 90, 678, COLOR_WHITE);
        draw_text(scene, "- Native XMB & Dashboard control: works on Login screen, Home, and games.", fontCardDesc, CARD_X + 90, 710, COLOR_TEXT_DIM);
        draw_text(scene, "- Central Guide / Home: Always acts as native PS Button (tap = Home, hold = Quick Menu).", fontCardDesc, CARD_X + 90, 740, COLOR_TEXT_DIM);
        draw_text(scene, "- Shortcuts: Back + D-Pad Down = PS Button | Back + Start = Touchpad Click.", fontCardDesc, CARD_X + 90, 770, COLOR_TEXT_DIM);
        draw_text(scene, "- Hot-Reloading: changes saved in this app apply in real-time within 1 second.", fontCardDesc, CARD_X + 90, 800, COLOR_BLUE_BRIGHT);

        // 5. Footer for Info
        scene->DrawRectangle(60, 865, 1800, 2, COLOR_DIVIDER);

        draw_image(scene, iconInfo, 60, 890);
        draw_text(scene, "Information & Help", fontCardTitle, 120, 912, COLOR_WHITE);
        draw_text(scene, "Press [L1] / [R1] to navigate tabs", fontFooter, 120, 938, COLOR_TEXT_DIM);

        // Navigation Hints
        draw_image(scene, btnSquare, 1260, 897);
        draw_text(scene, "Reinstall Payload", fontFooter, 1305, 922, COLOR_BLUE_BRIGHT);

        draw_image(scene, btnL1, 1540, 901);
        draw_text(scene, "Tabs", fontFooter, 1595, 922, COLOR_TEXT_DIM);

        draw_image(scene, btnOptions, 1690, 900);
        draw_text(scene, "Exit", fontFooter, 1770, 922, COLOR_TEXT_DIM);
    }

    // 6. Toast Notification if active
    if (toastFrames > 0) {
        int boxW = 860;
        int boxH = 56;
        int boxX = (1920 - boxW) / 2;
        int boxY = 800;

        draw_rounded_rect(scene, boxX, boxY, boxW, boxH, 10, COLOR_TOAST_BG);
        draw_rounded_outline(scene, boxX, boxY, boxW, boxH, 10, COLOR_TOAST_BORDER, 2);
        const char* msg = (toastMsg && toastMsg[0]) ? toastMsg : "Settings saved to /data/GoldHEN/onefamily.ini";
        draw_text_centered(scene, msg, fontCardTitle, 960, boxY + 36, COLOR_BLUE_BRIGHT);
    }
}

void UI::render_wizard(Scene2D* scene, const InstallResult& res, int frameID) {
    if (!scene) return;

    // Draw cached background or fallback
    if (cached_bg) {
        scene->CopyToActiveBuffer(cached_bg);
    } else {
        scene->DrawRectangle(0, 0, 1920, 1080, Color{ 8, 14, 24 });
    }

    // Top Header
    draw_image(scene, xboxLogo, 60, 60);
    draw_text(scene, "OneFamily", fontTitle, 175, 95, COLOR_WHITE);
    draw_text(scene, res.isUpdate ? "Update & Startup Guide" : "All-in-One Setup & Startup Guide", fontSubtitle, 175, 128, COLOR_BLUE_BRIGHT);

    // Main Card
    int cardW = 1420;
    int cardH = 580;
    int cardX = (1920 - cardW) / 2;
    int cardY = 210;

    draw_rounded_rect(scene, cardX, cardY, cardW, cardH, 16, COLOR_CARD_BG);
    draw_rounded_outline(scene, cardX - 1, cardY - 1, cardW + 2, cardH + 2, 16, COLOR_BLUE_GLOW, 2);
    draw_rounded_outline(scene, cardX, cardY, cardW, cardH, 16, COLOR_BLUE_MAIN, 2);

    // Title inside card
    draw_image(scene, iconSave, cardX + 40, cardY + 36);
    draw_text(scene, res.isUpdate ? "OneFamily Update Completed" : "OneFamily Installation Completed", fontCardTitle, cardX + 105, cardY + 60, COLOR_WHITE);
    draw_text(scene, res.isUpdate ? "System payload and configuration successfully updated for GoldHEN:" : "System files and auto-boot successfully configured for GoldHEN:", fontCardDesc, cardX + 105, cardY + 90, COLOR_TEXT_DIM);

    // Status Lines
    int lineY = cardY + 135;
    int lineStep = 40;

    // Item 1: Payload
    if (res.payloadCopied) {
        draw_text(scene, res.isUpdate ? "[OK] Payload Updated:" : "[OK] Payload File:", fontCardTitle, cardX + 50, lineY, COLOR_BLUE_BRIGHT);
        draw_text(scene, res.isUpdate ? "Replaced with latest version (/data/payloads/onefamily.bin)" : "Installed to /data/payloads/onefamily.bin", fontCardDesc, cardX + 300, lineY, COLOR_WHITE);
    } else {
        draw_text(scene, "[FAIL] Payload File:", fontCardTitle, cardX + 50, lineY, Color{ 255, 80, 80 });
        draw_text(scene, "Failed to copy payload to /data/payloads/onefamily.bin", fontCardDesc, cardX + 300, lineY, COLOR_WHITE);
    }
    lineY += lineStep;

    // Item 2: Config
    if (res.configCreated) {
        draw_text(scene, res.isUpdate ? "[OK] Configuration:" : "[OK] Configuration:", fontCardTitle, cardX + 50, lineY, COLOR_BLUE_BRIGHT);
        draw_text(scene, res.isUpdate ? "Replaced /data/GoldHEN/onefamily.ini with latest version & defaults" : "Initialized /data/GoldHEN/onefamily.ini with optimal settings", fontCardDesc, cardX + 300, lineY, COLOR_WHITE);
    } else {
        draw_text(scene, "[FAIL] Configuration:", fontCardTitle, cardX + 50, lineY, Color{ 255, 80, 80 });
        draw_text(scene, "Failed to create configuration file", fontCardDesc, cardX + 300, lineY, COLOR_WHITE);
    }
    lineY += lineStep;

    // Item 3: Auto-boot in payloads.ini
    if (res.autobootConfigured) {
        draw_text(scene, "[OK] Auto-Boot:", fontCardTitle, cardX + 50, lineY, COLOR_BLUE_BRIGHT);
        draw_text(scene, res.isUpdate ? "Verified in /data/GoldHEN/payloads.ini ([AutoRun])" : "Registered in /data/GoldHEN/payloads.ini ([AutoRun])", fontCardDesc, cardX + 300, lineY, COLOR_WHITE);
    } else {
        draw_text(scene, "[FAIL] Auto-Boot:", fontCardTitle, cardX + 50, lineY, Color{ 255, 80, 80 });
        draw_text(scene, "Failed to register auto-boot entry in payloads.ini", fontCardDesc, cardX + 300, lineY, COLOR_WHITE);
    }

    // Divider
    int divY = cardY + 270;
    scene->DrawRectangle(cardX + 40, divY, cardW - 80, 2, COLOR_DIVIDER);

    // Startup Guide (4 Steps in English)
    if (res.isUpdate) {
        draw_text(scene, "Next Steps to apply the update:", fontCardTitle, cardX + 50, divY + 38, COLOR_WHITE);

        int stepY = divY + 76;
        draw_text(scene, "1. Restart your console (power off and reboot PS4).", fontCardDesc, cardX + 50, stepY, COLOR_WHITE);

        stepY += 36;
        draw_text(scene, "2. GoldHEN will auto-launch the newly updated OneFamily payload on boot.", fontCardDesc, cardX + 50, stepY, COLOR_WHITE);

        stepY += 36;
        draw_text(scene, "3. All new controller features, fixes, and mappings are now active!", fontCardDesc, cardX + 50, stepY, COLOR_BLUE_BRIGHT);

        stepY += 36;
        draw_text(scene, "4. Open OneFamily Companion anytime to tweak your configuration.", fontCardDesc, cardX + 50, stepY, COLOR_TEXT_DIM);
    } else {
        draw_text(scene, "Next Steps to activate your controller:", fontCardTitle, cardX + 50, divY + 38, COLOR_WHITE);

        int stepY = divY + 76;
        draw_text(scene, "1. Restart your console (power off and reboot PS4).", fontCardDesc, cardX + 50, stepY, COLOR_WHITE);

        stepY += 36;
        draw_text(scene, "2. Run the jailbreak via GoldHEN.", fontCardDesc, cardX + 50, stepY, COLOR_WHITE);

        stepY += 36;
        draw_text(scene, "3. OneFamily will be active automatically: connect your pad, it will be recognized and work immediately!", fontCardDesc, cardX + 50, stepY, COLOR_BLUE_BRIGHT);

        stepY += 36;
        draw_text(scene, "4. Open OneFamily Companion anytime to configure or calibrate your pad.", fontCardDesc, cardX + 50, stepY, COLOR_TEXT_DIM);
    }

    // Footer
    scene->DrawRectangle(60, 865, 1800, 2, COLOR_DIVIDER);
    draw_image(scene, iconInfo, 60, 890);
    draw_text(scene, "All-in-One Setup Complete", fontCardTitle, 120, 912, COLOR_WHITE);
    draw_text(scene, "OneFamily will boot automatically on every console restart", fontFooter, 120, 938, COLOR_TEXT_DIM);

    // Button X
    draw_image(scene, btnCross, 1660, 897);
    draw_text(scene, "Close & Exit", fontFooter, 1710, 922, COLOR_BLUE_BRIGHT);
}

void UI::render_tester(Scene2D* scene, const AppConfig& cfg, const OrbisPadData& padData, const TesterState& tester) {
    if (!scene) return;

    char buf[128];
    int deadzone_val = ConfigManager::DEADZONE_VALUES[cfg.deadzone_index];
    int thresh_val = ConfigManager::TRIGGER_VALUES[cfg.trigger_index];

    // Check if pure Xbox controller is connected
    if (!padData.connected) {
        int warnX = CARD_X;
        int warnY = 280;
        int warnW = CARD_W;
        int warnH = 560;

        Color warnBg = { 20, 26, 42 };
        Color warnBorder = { 0, 112, 220 };

        draw_rounded_rect(scene, warnX, warnY, warnW, warnH, 12, warnBg);
        draw_rounded_outline(scene, warnX, warnY, warnW, warnH, 12, warnBorder, 2);

        draw_image(scene, iconInfo, warnX + 40, warnY + 42);
        draw_text(scene, "No USB Controller Detected", fontCardTitle, warnX + 105, warnY + 68, COLOR_WHITE);

        int textY = warnY + 130;
        draw_text(scene, "The Input Tester operates in pure hardware mode to isolate controller inputs from DualShock 4.",
                  fontCardDesc, warnX + 40, textY, COLOR_TEXT_DIM);
        textY += 40;
        draw_text(scene, "Please connect an Xbox (360, One, Series X|S), Switch, PS3, or PS5 USB controller to your PS4.",
                  fontCardDesc, warnX + 40, textY, COLOR_WHITE);
        textY += 40;
        draw_text(scene, "Once connected via USB cable, the controller will be recognized immediately without restarting.",
                  fontCardDesc, warnX + 40, textY, COLOR_BLUE_BRIGHT);

        // Footer for disconnected state
        scene->DrawRectangle(60, 865, 1800, 2, COLOR_DIVIDER);
        draw_image(scene, iconMode, 60, 890);
        draw_text(scene, "Input Tester & Calibration", fontCardTitle, 120, 912, COLOR_WHITE);
        draw_text(scene, "Waiting for USB controller connection...", fontFooter, 120, 938, Color{ 220, 80, 60 });

        draw_image(scene, btnOptions, 1680, 900);
        draw_text(scene, "Exit", fontFooter, 1760, 922, COLOR_TEXT_DIM);
        return;
    }

    // ========================================================
    // 1. ANALOG STICKS (ROW 1, Y: 280, H: 235)
    // ========================================================
    int stickR = 56;

    // --- Left Stick Card ---
    int lsX = CARD_X;
    int lsW = 555;
    int lsY = 280;
    int lsH = 235;

    draw_rounded_rect(scene, lsX, lsY, lsW, lsH, 12, COLOR_CARD_BG);
    draw_rounded_outline(scene, lsX, lsY, lsW, lsH, 12, COLOR_CARD_BORDER, 1);
    draw_image(scene, iconDeadzone, lsX + 20, lsY + 16);
    draw_text(scene, "Left Stick (LS / L3)", fontCardTitle, lsX + 70, lsY + 38, COLOR_WHITE);

    // Left Reticle
    int lcx = lsX + 105;
    int lcy = lsY + 140;

    // Crosshair & circle
    draw_circle_outline(scene, lcx, lcy, stickR, COLOR_CARD_BORDER, 2);
    scene->DrawRectangle(lcx - stickR, lcy, 2 * stickR, 1, COLOR_DIVIDER);
    scene->DrawRectangle(lcx, lcy - stickR, 1, 2 * stickR, COLOR_DIVIDER);

    // Deadzone circle
    int ldz_r = (deadzone_val * stickR) / 128;
    if (ldz_r > 0) {
        draw_circle_outline(scene, lcx, lcy, ldz_r, COLOR_BLUE_GLOW, 1);
    }

    // Left Stick coordinates
    int rawLX = (int)padData.leftStick.x;
    int rawLY = (int)padData.leftStick.y;
    int lDevX = rawLX - 128;
    int lDevY = rawLY - 128;
    int lsXPos = lcx + (lDevX * stickR) / 128;
    int lsYPos = lcy + (lDevY * stickR) / 128;
    int lDist = (int)sqrt((double)(lDevX * lDevX + lDevY * lDevY));
    bool lInDeadzone = (lDist <= deadzone_val);
    Color lCursorColor = lInDeadzone ? Color{ 40, 80, 130 } : COLOR_BLUE_MAIN;

    draw_circle(scene, lsXPos, lsYPos, 7, lCursorColor);
    draw_circle_outline(scene, lsXPos, lsYPos, 7, COLOR_WHITE, 1);

    // Left Stick Info Text
    int textX = lsX + 195;
    snprintf(buf, sizeof(buf), "X: %d  (%+d%%)", rawLX, (lDevX * 100) / 128);
    draw_text(scene, buf, fontCardDesc, textX, lsY + 85, COLOR_WHITE);

    snprintf(buf, sizeof(buf), "Y: %d  (%+d%%)", rawLY, (lDevY * 100) / 128);
    draw_text(scene, buf, fontCardDesc, textX, lsY + 115, COLOR_WHITE);

    snprintf(buf, sizeof(buf), "Deadzone: %s (val: %d)",
             ConfigManager::DEADZONE_OPTIONS[cfg.deadzone_index].c_str(), deadzone_val);
    draw_text(scene, buf, fontCardDesc, textX, lsY + 145, COLOR_TEXT_DIM);

    bool l3Pressed = (padData.buttons & ORBIS_PAD_BUTTON_L3) != 0;
    snprintf(buf, sizeof(buf), "L3 Click: %s", l3Pressed ? "PRESSED" : "Released");
    draw_text(scene, buf, fontCardDesc, textX, lsY + 175, l3Pressed ? COLOR_BLUE_BRIGHT : COLOR_TEXT_DIM);

    draw_text(scene, lInDeadzone ? "Output: IDLE (In Deadzone)" : "Output: ACTIVE INPUT",
              fontCardDesc, textX, lsY + 205, lInDeadzone ? COLOR_TEXT_DIM : COLOR_BLUE_BRIGHT);

    // --- Right Stick Card ---
    int rsX = lsX + lsW + 30; // 410 + 555 + 30 = 995
    int rsW = 555;
    int rsY = 280;
    int rsH = 235;

    draw_rounded_rect(scene, rsX, rsY, rsW, rsH, 12, COLOR_CARD_BG);
    draw_rounded_outline(scene, rsX, rsY, rsW, rsH, 12, COLOR_CARD_BORDER, 1);
    draw_image(scene, iconDeadzone, rsX + 20, rsY + 16);
    draw_text(scene, "Right Stick (RS / R3)", fontCardTitle, rsX + 70, rsY + 38, COLOR_WHITE);

    // Right Reticle
    int rcx = rsX + 105;
    int rcy = rsY + 140;

    draw_circle_outline(scene, rcx, rcy, stickR, COLOR_CARD_BORDER, 2);
    scene->DrawRectangle(rcx - stickR, rcy, 2 * stickR, 1, COLOR_DIVIDER);
    scene->DrawRectangle(rcx, rcy - stickR, 1, 2 * stickR, COLOR_DIVIDER);

    int rdz_r = (deadzone_val * stickR) / 128;
    if (rdz_r > 0) {
        draw_circle_outline(scene, rcx, rcy, rdz_r, COLOR_BLUE_GLOW, 1);
    }

    // Right Stick coordinates
    int rawRX = (int)padData.rightStick.x;
    int rawRY = (int)padData.rightStick.y;
    int rDevX = rawRX - 128;
    int rDevY = rawRY - 128;
    int rsXPos = rcx + (rDevX * stickR) / 128;
    int rsYPos = rcy + (rDevY * stickR) / 128;
    int rDist = (int)sqrt((double)(rDevX * rDevX + rDevY * rDevY));
    bool rInDeadzone = (rDist <= deadzone_val);
    Color rCursorColor = rInDeadzone ? Color{ 40, 80, 130 } : COLOR_BLUE_MAIN;

    draw_circle(scene, rsXPos, rsYPos, 7, rCursorColor);
    draw_circle_outline(scene, rsXPos, rsYPos, 7, COLOR_WHITE, 1);

    // Right Stick Info Text
    int rTextX = rsX + 195;
    snprintf(buf, sizeof(buf), "X: %d  (%+d%%)", rawRX, (rDevX * 100) / 128);
    draw_text(scene, buf, fontCardDesc, rTextX, rsY + 85, COLOR_WHITE);

    snprintf(buf, sizeof(buf), "Y: %d  (%+d%%)", rawRY, (rDevY * 100) / 128);
    draw_text(scene, buf, fontCardDesc, rTextX, rsY + 115, COLOR_WHITE);

    snprintf(buf, sizeof(buf), "Deadzone: %s (val: %d)",
             ConfigManager::DEADZONE_OPTIONS[cfg.deadzone_index].c_str(), deadzone_val);
    draw_text(scene, buf, fontCardDesc, rTextX, rsY + 145, COLOR_TEXT_DIM);

    bool r3Pressed = (padData.buttons & ORBIS_PAD_BUTTON_R3) != 0;
    snprintf(buf, sizeof(buf), "R3 Click: %s", r3Pressed ? "PRESSED" : "Released");
    draw_text(scene, buf, fontCardDesc, rTextX, rsY + 175, r3Pressed ? COLOR_BLUE_BRIGHT : COLOR_TEXT_DIM);

    draw_text(scene, rInDeadzone ? "Output: IDLE (In Deadzone)" : "Output: ACTIVE INPUT",
              fontCardDesc, rTextX, rsY + 205, rInDeadzone ? COLOR_TEXT_DIM : COLOR_BLUE_BRIGHT);

    // ========================================================
    // 2. TRIGGERS & BUMPERS (ROW 2, Y: 530, H: 155) - NO OVERLAP
    // ========================================================
    int trigX = CARD_X;
    int trigY = 530;
    int trigW = CARD_W;
    int trigH = 155;

    draw_rounded_rect(scene, trigX, trigY, trigW, trigH, 12, COLOR_CARD_BG);
    draw_rounded_outline(scene, trigX, trigY, trigW, trigH, 12, COLOR_CARD_BORDER, 1);
    draw_image(scene, iconSwap, trigX + 24, trigY + 16);
    draw_text(scene, "Triggers & Bumpers (LT / RT / LB / RB)", fontCardTitle, trigX + 78, trigY + 38, COLOR_WHITE);

    // Left Trigger & Bumper (LT & LB)
    int rawLT = (int)padData.analogButtons.l2;
    bool l2Btn = (padData.buttons & ORBIS_PAD_BUTTON_L2) != 0;
    bool lbBtn = (padData.buttons & ORBIS_PAD_BUTTON_L1) != 0;

    int ltBarX = trigX + 30;
    int ltBarY = trigY + 98;
    int ltBarW = 390;
    int ltBarH = 20;

    snprintf(buf, sizeof(buf), "LT: %d / 255 (%d%%)%s", rawLT, (rawLT * 100) / 255, l2Btn ? " [CLICK]" : "");
    draw_text(scene, buf, fontCardDesc, ltBarX, trigY + 84, l2Btn ? COLOR_BLUE_BRIGHT : COLOR_WHITE);

    draw_rounded_rect(scene, ltBarX, ltBarY, ltBarW, ltBarH, 4, Color{ 18, 30, 48 });
    int ltFillW = (rawLT * ltBarW) / 255;
    if (ltFillW > 0) {
        draw_rounded_rect(scene, ltBarX, ltBarY, ltFillW, ltBarH, 4, l2Btn ? COLOR_BLUE_MAIN : Color{ 15, 60, 120 });
    }
    // Threshold marker
    int ltMarkerX = ltBarX + (thresh_val * ltBarW) / 255;
    scene->DrawRectangle(ltMarkerX, ltBarY - 3, 2, ltBarH + 6, COLOR_WHITE);

    // LB Bumper pill (generous spacing beside LT bar)
    int lbPillX = trigX + 440;
    int lbPillY = trigY + 84;
    draw_btn_badge(scene, lbPillX, lbPillY, 95, 34, "LB / L1", lbBtn);

    // Right Trigger & Bumper (RT & RB)
    int rawRT = (int)padData.analogButtons.r2;
    bool r2Btn = (padData.buttons & ORBIS_PAD_BUTTON_R2) != 0;
    bool rbBtn = (padData.buttons & ORBIS_PAD_BUTTON_R1) != 0;

    int rtBarX = trigX + 590;
    int rtBarY = trigY + 98;
    int rtBarW = 390;
    int rtBarH = 20;

    snprintf(buf, sizeof(buf), "RT: %d / 255 (%d%%)%s", rawRT, (rawRT * 100) / 255, r2Btn ? " [CLICK]" : "");
    draw_text(scene, buf, fontCardDesc, rtBarX, trigY + 84, r2Btn ? COLOR_BLUE_BRIGHT : COLOR_WHITE);

    draw_rounded_rect(scene, rtBarX, rtBarY, rtBarW, rtBarH, 4, Color{ 18, 30, 48 });
    int rtFillW = (rawRT * rtBarW) / 255;
    if (rtFillW > 0) {
        draw_rounded_rect(scene, rtBarX, rtBarY, rtFillW, rtBarH, 4, r2Btn ? COLOR_BLUE_MAIN : Color{ 15, 60, 120 });
    }
    // Threshold marker
    int rtMarkerX = rtBarX + (thresh_val * rtBarW) / 255;
    scene->DrawRectangle(rtMarkerX, rtBarY - 3, 2, rtBarH + 6, COLOR_WHITE);

    // RB Bumper pill
    int rbPillX = trigX + 1000;
    int rbPillY = trigY + 84;
    draw_btn_badge(scene, rbPillX, rbPillY, 95, 34, "RB / R1", rbBtn);

    // ========================================================
    // 3. VIBRATION TEST & STICK DRIFT CALIBRATION (ROW 3, Y: 700, H: 150)
    // ========================================================
    // Left Box: Vibration Feedback Test
    int vibX = CARD_X;
    int vibY = 700;
    int vibW = 555;
    int vibH = 150;

    draw_rounded_rect(scene, vibX, vibY, vibW, vibH, 12, COLOR_CARD_BG);
    draw_rounded_outline(scene, vibX, vibY, vibW, vibH, 12, COLOR_CARD_BORDER, 1);
    draw_image(scene, iconRumble, vibX + 24, vibY + 16);
    draw_text(scene, "Vibration Feedback Test", fontCardTitle, vibX + 78, vibY + 38, COLOR_WHITE);

    draw_image(scene, btnSquare, vibX + 30, vibY + 68);
    draw_text(scene, "Test Dual Vibration Motors", fontCardDesc, vibX + 76, vibY + 90, COLOR_WHITE);

    if (tester.rumblePhase == 0) {
        draw_text(scene, "Tests Left (heavy) and Right (light) motors", fontCardDesc, vibX + 30, vibY + 124, COLOR_TEXT_DIM);
    } else if (tester.rumblePhase == 1) {
        draw_text(scene, "[TESTING] Left Motor (Heavy / Low Freq)...", fontCardDesc, vibX + 30, vibY + 124, COLOR_BLUE_BRIGHT);
    } else if (tester.rumblePhase == 2) {
        draw_text(scene, "[TESTING] Right Motor (Light / High Freq)...", fontCardDesc, vibX + 30, vibY + 124, COLOR_BLUE_BRIGHT);
    } else {
        draw_text(scene, "[TESTING] Both Motors Active (Full Strength)...", fontCardDesc, vibX + 30, vibY + 124, COLOR_BLUE_BRIGHT);
    }

    // Right Box: Stick Drift Auto-Calibration
    int calX = vibX + vibW + 30; // 995
    int calY = 700;
    int calW = 555;
    int calH = 150;

    draw_rounded_rect(scene, calX, calY, calW, calH, 12, COLOR_CARD_BG);
    draw_rounded_outline(scene, calX, calY, calW, calH, 12, COLOR_CARD_BORDER, 1);
    draw_image(scene, iconDeadzone, calX + 24, calY + 16);
    draw_text(scene, "Stick Drift Auto-Calibration", fontCardTitle, calX + 78, calY + 38, COLOR_WHITE);

    if (tester.calib.active) {
        int progressW = (tester.calib.currentFrame * 495) / tester.calib.totalFrames;
        draw_rounded_rect(scene, calX + 30, calY + 68, 495, 16, 4, Color{ 18, 30, 48 });
        draw_rounded_rect(scene, calX + 30, calY + 68, progressW, 16, 4, COLOR_BLUE_MAIN);

        snprintf(buf, sizeof(buf), "Sampling rest drift: %d%%  (Max noise: %d)",
                 (tester.calib.currentFrame * 100) / tester.calib.totalFrames, tester.calib.maxNoise);
        draw_text(scene, buf, fontCardDesc, calX + 30, calY + 124, COLOR_BLUE_BRIGHT);
    } else if (tester.calib.completed) {
        snprintf(buf, sizeof(buf), "Drift: %d -> Recommended Deadzone: %s",
                 tester.calib.maxNoise, ConfigManager::DEADZONE_OPTIONS[tester.calib.suggestedIndex].c_str());
        draw_text(scene, buf, fontCardDesc, calX + 30, calY + 86, COLOR_WHITE);

        draw_image(scene, btnCross, calX + 30, calY + 102);
        draw_text(scene, "Apply and save recommended deadzone", fontCardDesc, calX + 76, calY + 124, COLOR_BLUE_BRIGHT);
    } else {
        draw_image(scene, btnTriangle, calX + 30, calY + 68);
        draw_text(scene, "Analyze Stick Drift at Rest (3s)", fontCardDesc, calX + 76, calY + 90, COLOR_WHITE);
        draw_text(scene, "Measures noise to compute optimal deadzone", fontCardDesc, calX + 30, calY + 124, COLOR_TEXT_DIM);
    }

    // ========================================================
    // 4. FOOTER FOR TESTER
    // ========================================================
    scene->DrawRectangle(60, 865, 1800, 2, COLOR_DIVIDER);

    draw_image(scene, iconMode, 60, 890);
    draw_text(scene, "Input Tester & Calibration", fontCardTitle, 120, 912, COLOR_WHITE);
    draw_text(scene, "Pure USB hardware mode (isolated from DualShock 4)", fontFooter, 120, 938, COLOR_BLUE_BRIGHT);

    // Hints with authentic button icons
    draw_image(scene, btnSquare, 1140, 897);
    draw_text(scene, "Rumble", fontFooter, 1185, 922, COLOR_TEXT_DIM);

    draw_image(scene, btnTriangle, 1300, 897);
    draw_text(scene, "Calib", fontFooter, 1345, 922, COLOR_TEXT_DIM);

    draw_image(scene, btnCross, 1460, 897);
    draw_text(scene, "Apply", fontFooter, 1505, 922, tester.calib.completed ? COLOR_BLUE_BRIGHT : COLOR_TEXT_DIM);

    draw_image(scene, btnOptions, 1630, 900);
    draw_text(scene, "Exit", fontFooter, 1710, 922, COLOR_TEXT_DIM);
}
