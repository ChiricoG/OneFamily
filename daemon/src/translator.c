/*
 * Xbox 360 to DualShock 4 Input Translator
 * Converts Xbox 360 USB reports to PS4 OrbisPadData format
 */

#include "translator.h"
#include "ini_parser.h"
#include <string.h>
#include <orbis/libkernel.h>

/*
 * Initialize translator with default configuration
 */
void translator_init(TranslatorConfig* config) {
    if (!config) return;

    config->stick_deadzone = (g_config.stick_deadzone > 0) ? g_config.stick_deadzone : DEFAULT_STICK_DEADZONE;
    config->trigger_threshold = (g_config.trigger_threshold > 0) ? g_config.trigger_threshold : DEFAULT_TRIGGER_THRESHOLD;
    config->invert_left_y = 1;      // PS4 Y-axis is inverted from Xbox
    config->invert_right_y = 1;
    if (!g_config.switch_positional) {
        config->swap_ab = 1;        // Map Nintendo letter A->Circle, B->Cross
        config->swap_xy = 1;
    } else {
        config->swap_ab = 0;
        config->swap_xy = 0;
    }
}

/*
 * Convert Xbox 360 16-bit signed stick value to DS4 8-bit unsigned
 *
 * Xbox:  -32768 to 32767, center = 0
 * DS4:   0 to 255, center = 128
 */
static inline uint8_t convert_stick_value(int16_t xbox_val) {
    int32_t temp = (int32_t)xbox_val + 32768;
    return (uint8_t)(temp >> 8);
}

/*
 * Apply deadzone to stick value
 */
uint8_t translator_apply_deadzone(uint8_t value, uint8_t deadzone) {
    if (deadzone == 0) return value;

    int16_t centered = (int16_t)value - 128;
    int16_t abs_centered = (centered < 0) ? -centered : centered;

    if (abs_centered <= deadzone) {
        return 128;
    }

    int16_t range = 127 - deadzone;
    int16_t scaled = ((abs_centered - deadzone) * 127) / range;

    if (centered < 0) {
        return (uint8_t)(128 - scaled);
    } else {
        return (uint8_t)(128 + scaled);
    }
}

/*
 * Main translation function
 */
void translator_convert(const Xbox360Report* xbox, OrbisPadData* ds4, const TranslatorConfig* config) {
    // Use default config if none provided
    TranslatorConfig default_config;
    if (!config) {
        translator_init(&default_config);
        config = &default_config;
    }

    // Clear output structure
    memset(ds4, 0, sizeof(OrbisPadData));

    // Get combined Xbox button state
    uint16_t xbox_buttons = xbox360_get_buttons(xbox);

    // ========================================
    // ANALOG STICKS (using OpenOrbis 'stick' struct)
    // ========================================

    uint8_t lx = convert_stick_value(xbox->left_stick_x);
    uint8_t ly = convert_stick_value(xbox->left_stick_y);
    uint8_t rx = convert_stick_value(xbox->right_stick_x);
    uint8_t ry = convert_stick_value(xbox->right_stick_y);

    // Apply Y-axis inversion
    if (config->invert_left_y) {
        ly = 255 - ly;
    }
    if (config->invert_right_y) {
        ry = 255 - ry;
    }

    // Apply deadzone
    if (config->stick_deadzone > 0) {
        lx = translator_apply_deadzone(lx, config->stick_deadzone);
        ly = translator_apply_deadzone(ly, config->stick_deadzone);
        rx = translator_apply_deadzone(rx, config->stick_deadzone);
        ry = translator_apply_deadzone(ry, config->stick_deadzone);
    }

    ds4->leftStick.x = lx;
    ds4->leftStick.y = ly;
    ds4->rightStick.x = rx;
    ds4->rightStick.y = ry;

    // ========================================
    // ANALOG TRIGGERS (using OpenOrbis 'analog' struct)
    // ========================================

    if (g_config.swap_triggers_bumpers) {
        ds4->analogButtons.l2 = (xbox_buttons & XBOX360_BTN_LB) ? 255 : 0;
        ds4->analogButtons.r2 = (xbox_buttons & XBOX360_BTN_RB) ? 255 : 0;
    } else {
        ds4->analogButtons.l2 = xbox->left_trigger;
        ds4->analogButtons.r2 = xbox->right_trigger;
    }

    // ========================================
    // DIGITAL BUTTONS
    // ========================================

    uint32_t ds4_buttons = 0;

    // Face buttons with optional swap
    if (config->swap_ab) {
        if (xbox_buttons & XBOX360_BTN_A) ds4_buttons |= DS4_BUTTON_CIRCLE;
        if (xbox_buttons & XBOX360_BTN_B) ds4_buttons |= DS4_BUTTON_CROSS;
    } else {
        if (xbox_buttons & XBOX360_BTN_A) ds4_buttons |= DS4_BUTTON_CROSS;
        if (xbox_buttons & XBOX360_BTN_B) ds4_buttons |= DS4_BUTTON_CIRCLE;
    }

    if (config->swap_xy) {
        if (xbox_buttons & XBOX360_BTN_X) ds4_buttons |= DS4_BUTTON_TRIANGLE;
        if (xbox_buttons & XBOX360_BTN_Y) ds4_buttons |= DS4_BUTTON_SQUARE;
    } else {
        if (xbox_buttons & XBOX360_BTN_X) ds4_buttons |= DS4_BUTTON_SQUARE;
        if (xbox_buttons & XBOX360_BTN_Y) ds4_buttons |= DS4_BUTTON_TRIANGLE;
    }

    // Shoulder buttons & digital triggers
    if (g_config.swap_triggers_bumpers) {
        if (xbox_buttons & XBOX360_BTN_LB) ds4_buttons |= DS4_BUTTON_L2;
        if (xbox_buttons & XBOX360_BTN_RB) ds4_buttons |= DS4_BUTTON_R2;
        if (xbox->left_trigger >= config->trigger_threshold) {
            ds4_buttons |= DS4_BUTTON_L1;
        }
        if (xbox->right_trigger >= config->trigger_threshold) {
            ds4_buttons |= DS4_BUTTON_R1;
        }
    } else {
        if (xbox_buttons & XBOX360_BTN_LB) ds4_buttons |= DS4_BUTTON_L1;
        if (xbox_buttons & XBOX360_BTN_RB) ds4_buttons |= DS4_BUTTON_R1;
        if (xbox->left_trigger >= config->trigger_threshold) {
            ds4_buttons |= DS4_BUTTON_L2;
        }
        if (xbox->right_trigger >= config->trigger_threshold) {
            ds4_buttons |= DS4_BUTTON_R2;
        }
    }

    // Stick click buttons
    if (xbox_buttons & XBOX360_BTN_L3) ds4_buttons |= DS4_BUTTON_L3;
    if (xbox_buttons & XBOX360_BTN_R3) ds4_buttons |= DS4_BUTTON_R3;

    // Menu buttons
    if (xbox_buttons & XBOX360_BTN_START) ds4_buttons |= DS4_BUTTON_OPTIONS;
    if (xbox_buttons & XBOX360_BTN_BACK)  ds4_buttons |= DS4_BUTTON_SHARE;

    // Guide button action
    if (xbox_buttons & XBOX360_BTN_GUIDE) {
        if (g_config.guide_action == CONFIG_GUIDE_TOUCHPAD) {
            ds4_buttons |= DS4_BUTTON_TOUCHPAD;
        } else if (g_config.guide_action == CONFIG_GUIDE_SHARE) {
            ds4_buttons |= DS4_BUTTON_SHARE;
        }
    }

    // ========================================
    // D-PAD
    // ========================================

    uint8_t xbox_dpad = xbox360_get_dpad(xbox);
    ds4_buttons |= dpad_bits_to_buttons(xbox_dpad);

    // Store final button state
    ds4->buttons = ds4_buttons;

    // ========================================
    // STATUS & METADATA
    // ========================================

    ds4->connected = 1;
    ds4->count = 1;
    if (ds4->timestamp == 0) {
        ds4->timestamp = sceKernelGetProcessTime();
    }

    // Motion data - set to neutral
    ds4->quat.x = 0.0f;
    ds4->quat.y = 0.0f;
    ds4->quat.z = 0.0f;
    ds4->quat.w = 1.0f;

    ds4->vel.x = 0.0f;
    ds4->vel.y = 0.0f;
    ds4->vel.z = 0.0f;

    ds4->acell.x = 0.0f;
    ds4->acell.y = 0.0f;
    ds4->acell.z = 1.0f;  // 1g downward

    // Touchpad - no touches
    ds4->touch.fingers = 0;
}

/*
 * Simple wrapper for translation with default config
 */
void xbox360_to_ds4(const Xbox360Report* xbox, OrbisPadData* ds4) {
    translator_convert(xbox, ds4, NULL);
}

/*
 * Xbox One translation function
 */
void translator_convert_xboxone(const XboxOneReport* xbox, OrbisPadData* ds4, const TranslatorConfig* config) {
    // Use default config if none provided
    TranslatorConfig default_config;
    if (!config) {
        translator_init(&default_config);
        config = &default_config;
    }

    // Clear output structure
    memset(ds4, 0, sizeof(OrbisPadData));

    // ========================================
    // ANALOG STICKS (same format as Xbox 360)
    // ========================================

    uint8_t lx = convert_stick_value(xbox->left_stick_x);
    uint8_t ly = convert_stick_value(xbox->left_stick_y);
    uint8_t rx = convert_stick_value(xbox->right_stick_x);
    uint8_t ry = convert_stick_value(xbox->right_stick_y);

    // Apply Y-axis inversion
    if (config->invert_left_y) {
        ly = 255 - ly;
    }
    if (config->invert_right_y) {
        ry = 255 - ry;
    }

    // Apply deadzone
    if (config->stick_deadzone > 0) {
        lx = translator_apply_deadzone(lx, config->stick_deadzone);
        ly = translator_apply_deadzone(ly, config->stick_deadzone);
        rx = translator_apply_deadzone(rx, config->stick_deadzone);
        ry = translator_apply_deadzone(ry, config->stick_deadzone);
    }

    ds4->leftStick.x = lx;
    ds4->leftStick.y = ly;
    ds4->rightStick.x = rx;
    ds4->rightStick.y = ry;

    // ========================================
    // ANALOG TRIGGERS (Xbox One uses 10-bit, convert to 8-bit)
    // ========================================

    uint8_t lt = xboxone_trigger_to_8bit(xbox->left_trigger);
    uint8_t rt = xboxone_trigger_to_8bit(xbox->right_trigger);

    if (g_config.swap_triggers_bumpers) {
        ds4->analogButtons.l2 = (xbox->buttons_high & XBOXONE_LB) ? 255 : 0;
        ds4->analogButtons.r2 = (xbox->buttons_high & XBOXONE_RB) ? 255 : 0;
    } else {
        ds4->analogButtons.l2 = lt;
        ds4->analogButtons.r2 = rt;
    }

    // ========================================
    // DIGITAL BUTTONS
    // ========================================

    uint32_t ds4_buttons = 0;

    // Face buttons (Xbox One layout: A/B/X/Y in buttons_low)
    if (config->swap_ab) {
        if (xbox->buttons_low & XBOXONE_A) ds4_buttons |= DS4_BUTTON_CIRCLE;
        if (xbox->buttons_low & XBOXONE_B) ds4_buttons |= DS4_BUTTON_CROSS;
    } else {
        if (xbox->buttons_low & XBOXONE_A) ds4_buttons |= DS4_BUTTON_CROSS;
        if (xbox->buttons_low & XBOXONE_B) ds4_buttons |= DS4_BUTTON_CIRCLE;
    }

    if (config->swap_xy) {
        if (xbox->buttons_low & XBOXONE_X) ds4_buttons |= DS4_BUTTON_TRIANGLE;
        if (xbox->buttons_low & XBOXONE_Y) ds4_buttons |= DS4_BUTTON_SQUARE;
    } else {
        if (xbox->buttons_low & XBOXONE_X) ds4_buttons |= DS4_BUTTON_SQUARE;
        if (xbox->buttons_low & XBOXONE_Y) ds4_buttons |= DS4_BUTTON_TRIANGLE;
    }

    // Shoulder buttons & digital triggers
    if (g_config.swap_triggers_bumpers) {
        if (xbox->buttons_high & XBOXONE_LB) ds4_buttons |= DS4_BUTTON_L2;
        if (xbox->buttons_high & XBOXONE_RB) ds4_buttons |= DS4_BUTTON_R2;
        if (lt >= config->trigger_threshold) {
            ds4_buttons |= DS4_BUTTON_L1;
        }
        if (rt >= config->trigger_threshold) {
            ds4_buttons |= DS4_BUTTON_R1;
        }
    } else {
        if (xbox->buttons_high & XBOXONE_LB) ds4_buttons |= DS4_BUTTON_L1;
        if (xbox->buttons_high & XBOXONE_RB) ds4_buttons |= DS4_BUTTON_R1;
        if (lt >= config->trigger_threshold) {
            ds4_buttons |= DS4_BUTTON_L2;
        }
        if (rt >= config->trigger_threshold) {
            ds4_buttons |= DS4_BUTTON_R2;
        }
    }

    // Stick click buttons (in buttons_high)
    if (xbox->buttons_high & XBOXONE_LEFT_STICK)  ds4_buttons |= DS4_BUTTON_L3;
    if (xbox->buttons_high & XBOXONE_RIGHT_STICK) ds4_buttons |= DS4_BUTTON_R3;

    // Menu buttons (in buttons_low)
    if (xbox->buttons_low & XBOXONE_MENU) ds4_buttons |= DS4_BUTTON_OPTIONS;
    if (xbox->buttons_low & XBOXONE_VIEW) ds4_buttons |= DS4_BUTTON_SHARE;

    // Note: Xbox/Guide button comes in separate report (0x07), not handled here

    // ========================================
    // D-PAD (in buttons_high, same bit layout as Xbox 360)
    // ========================================

    uint8_t xbox_dpad = xboxone_get_dpad(xbox);
    ds4_buttons |= dpad_bits_to_buttons(xbox_dpad);

    // Store final button state
    ds4->buttons = ds4_buttons;

    // ========================================
    // STATUS & METADATA
    // ========================================

    ds4->connected = 1;
    ds4->count = 1;
    if (ds4->timestamp == 0) {
        ds4->timestamp = sceKernelGetProcessTime();
    }

    // Motion data - set to neutral
    ds4->quat.x = 0.0f;
    ds4->quat.y = 0.0f;
    ds4->quat.z = 0.0f;
    ds4->quat.w = 1.0f;

    ds4->vel.x = 0.0f;
    ds4->vel.y = 0.0f;
    ds4->vel.z = 0.0f;

    ds4->acell.x = 0.0f;
    ds4->acell.y = 0.0f;
    ds4->acell.z = 1.0f;  // 1g downward

    // Touchpad - no touches
    ds4->touch.fingers = 0;
}

/*
 * Simple wrapper for Xbox One translation with default config
 */
void xboxone_to_ds4(const XboxOneReport* xbox, OrbisPadData* ds4) {
    translator_convert_xboxone(xbox, ds4, NULL);
}

/*
 * Convert Switch hat value to DS4 d-pad buttons
 */
static uint32_t switch_hat_to_dpad(uint8_t hat) {
    switch (hat) {
        case SWITCH_HAT_UP:
            return DS4_BUTTON_DPAD_UP;
        case SWITCH_HAT_UP_RIGHT:
            return DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_RIGHT;
        case SWITCH_HAT_RIGHT:
            return DS4_BUTTON_DPAD_RIGHT;
        case SWITCH_HAT_DOWN_RIGHT:
            return DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_RIGHT;
        case SWITCH_HAT_DOWN:
            return DS4_BUTTON_DPAD_DOWN;
        case SWITCH_HAT_DOWN_LEFT:
            return DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_LEFT;
        case SWITCH_HAT_LEFT:
            return DS4_BUTTON_DPAD_LEFT;
        case SWITCH_HAT_UP_LEFT:
            return DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_LEFT;
        default:
            return 0;  // Centered or invalid
    }
}

/*
 * Switch Input-Only controller translation function
 */
void translator_convert_switch(const SwitchInputOnlyReport* sw, OrbisPadData* ds4, const TranslatorConfig* config) {
    // Use default config if none provided
    TranslatorConfig default_config;
    if (!config) {
        translator_init(&default_config);
        // Switch sticks don't need Y-axis inversion
        default_config.invert_left_y = 0;
        default_config.invert_right_y = 0;
        config = &default_config;
    }

    // Clear output structure
    memset(ds4, 0, sizeof(OrbisPadData));

    // ========================================
    // ANALOG STICKS (already 8-bit 0-255 format)
    // ========================================

    uint8_t lx = sw->left_stick_x;
    uint8_t ly = sw->left_stick_y;
    uint8_t rx = sw->right_stick_x;
    uint8_t ry = sw->right_stick_y;

    // Apply Y-axis inversion if configured
    if (config->invert_left_y) {
        ly = 255 - ly;
    }
    if (config->invert_right_y) {
        ry = 255 - ry;
    }

    // Apply deadzone
    if (config->stick_deadzone > 0) {
        lx = translator_apply_deadzone(lx, config->stick_deadzone);
        ly = translator_apply_deadzone(ly, config->stick_deadzone);
        rx = translator_apply_deadzone(rx, config->stick_deadzone);
        ry = translator_apply_deadzone(ry, config->stick_deadzone);
    }

    ds4->leftStick.x = lx;
    ds4->leftStick.y = ly;
    ds4->rightStick.x = rx;
    ds4->rightStick.y = ry;

    // ========================================
    // ANALOG TRIGGERS (Switch has digital only, fake analog)
    // ========================================

    if (g_config.swap_triggers_bumpers) {
        ds4->analogButtons.l2 = (sw->buttons0 & SWITCH_BTN_L) ? 255 : 0;
        ds4->analogButtons.r2 = (sw->buttons0 & SWITCH_BTN_R) ? 255 : 0;
    } else {
        ds4->analogButtons.l2 = (sw->buttons0 & SWITCH_BTN_ZL) ? 255 : 0;
        ds4->analogButtons.r2 = (sw->buttons0 & SWITCH_BTN_ZR) ? 255 : 0;
    }

    // ========================================
    // DIGITAL BUTTONS
    // ========================================

    uint32_t ds4_buttons = 0;

    // Face buttons - Switch uses Nintendo layout (A=East, B=South)
    // Map to PS4 positions: A->Circle, B->Cross, X->Triangle, Y->Square
    if (config->swap_ab) {
        if (sw->buttons0 & SWITCH_BTN_A) ds4_buttons |= DS4_BUTTON_CROSS;
        if (sw->buttons0 & SWITCH_BTN_B) ds4_buttons |= DS4_BUTTON_CIRCLE;
    } else {
        if (sw->buttons0 & SWITCH_BTN_A) ds4_buttons |= DS4_BUTTON_CIRCLE;
        if (sw->buttons0 & SWITCH_BTN_B) ds4_buttons |= DS4_BUTTON_CROSS;
    }

    if (config->swap_xy) {
        if (sw->buttons0 & SWITCH_BTN_X) ds4_buttons |= DS4_BUTTON_SQUARE;
        if (sw->buttons0 & SWITCH_BTN_Y) ds4_buttons |= DS4_BUTTON_TRIANGLE;
    } else {
        if (sw->buttons0 & SWITCH_BTN_X) ds4_buttons |= DS4_BUTTON_TRIANGLE;
        if (sw->buttons0 & SWITCH_BTN_Y) ds4_buttons |= DS4_BUTTON_SQUARE;
    }

    // Shoulder buttons & digital triggers
    if (g_config.swap_triggers_bumpers) {
        if (sw->buttons0 & SWITCH_BTN_L)  ds4_buttons |= DS4_BUTTON_L2;
        if (sw->buttons0 & SWITCH_BTN_R)  ds4_buttons |= DS4_BUTTON_R2;
        if (sw->buttons0 & SWITCH_BTN_ZL) ds4_buttons |= DS4_BUTTON_L1;
        if (sw->buttons0 & SWITCH_BTN_ZR) ds4_buttons |= DS4_BUTTON_R1;
    } else {
        if (sw->buttons0 & SWITCH_BTN_L)  ds4_buttons |= DS4_BUTTON_L1;
        if (sw->buttons0 & SWITCH_BTN_R)  ds4_buttons |= DS4_BUTTON_R1;
        if (sw->buttons0 & SWITCH_BTN_ZL) ds4_buttons |= DS4_BUTTON_L2;
        if (sw->buttons0 & SWITCH_BTN_ZR) ds4_buttons |= DS4_BUTTON_R2;
    }

    // Stick click buttons (byte 1)
    if (sw->buttons1 & SWITCH_BTN_L3) ds4_buttons |= DS4_BUTTON_L3;
    if (sw->buttons1 & SWITCH_BTN_R3) ds4_buttons |= DS4_BUTTON_R3;

    // Menu buttons (byte 1)
    if (sw->buttons1 & SWITCH_BTN_PLUS)  ds4_buttons |= DS4_BUTTON_OPTIONS;
    if (sw->buttons1 & SWITCH_BTN_MINUS) ds4_buttons |= DS4_BUTTON_SHARE;

    // Home button action
    if (sw->buttons1 & SWITCH_BTN_HOME) {
        if (g_config.guide_action == CONFIG_GUIDE_TOUCHPAD) {
            ds4_buttons |= DS4_BUTTON_TOUCHPAD;
        } else if (g_config.guide_action == CONFIG_GUIDE_SHARE) {
            ds4_buttons |= DS4_BUTTON_SHARE;
        }
    }

    // ========================================
    // D-PAD
    // ========================================

    ds4_buttons |= switch_hat_to_dpad(sw->hat);

    // Store final button state
    ds4->buttons = ds4_buttons;

    // ========================================
    // STATUS & METADATA
    // ========================================

    ds4->connected = 1;
    ds4->count = 1;
    if (ds4->timestamp == 0) {
        ds4->timestamp = sceKernelGetProcessTime();
    }

    // Motion data - set to neutral
    ds4->quat.x = 0.0f;
    ds4->quat.y = 0.0f;
    ds4->quat.z = 0.0f;
    ds4->quat.w = 1.0f;

    ds4->vel.x = 0.0f;
    ds4->vel.y = 0.0f;
    ds4->vel.z = 0.0f;

    ds4->acell.x = 0.0f;
    ds4->acell.y = 0.0f;
    ds4->acell.z = 1.0f;  // 1g downward

    // Touchpad - no touches
    ds4->touch.fingers = 0;
}

/*
 * Simple wrapper for Switch translation with default config
 */
void switch_to_ds4(const SwitchInputOnlyReport* sw, OrbisPadData* ds4) {
    translator_convert_switch(sw, ds4, NULL);
}

void translator_convert_ds3(const DS3Report* ds3, OrbisPadData* ds4, const TranslatorConfig* config) {
    TranslatorConfig default_config;
    if (!config) {
        translator_init(&default_config);
        config = &default_config;
    }

    memset(ds4, 0, sizeof(OrbisPadData));

    // Analog sticks
    uint8_t lx = ds3->left_stick_x;
    uint8_t ly = ds3->left_stick_y;
    uint8_t rx = ds3->right_stick_x;
    uint8_t ry = ds3->right_stick_y;

    if (config->invert_left_y)  ly = 255 - ly;
    if (config->invert_right_y) ry = 255 - ry;

    if (config->stick_deadzone > 0) {
        lx = translator_apply_deadzone(lx, config->stick_deadzone);
        ly = translator_apply_deadzone(ly, config->stick_deadzone);
        rx = translator_apply_deadzone(rx, config->stick_deadzone);
        ry = translator_apply_deadzone(ry, config->stick_deadzone);
    }

    ds4->leftStick.x = lx;
    ds4->leftStick.y = ly;
    ds4->rightStick.x = rx;
    ds4->rightStick.y = ry;

    // Analog triggers
    if (g_config.swap_triggers_bumpers) {
        ds4->analogButtons.l2 = (ds3->buttons2 & 0x04) ? 255 : 0;
        ds4->analogButtons.r2 = (ds3->buttons2 & 0x08) ? 255 : 0;
    } else {
        ds4->analogButtons.l2 = ds3->pressure_l2;
        ds4->analogButtons.r2 = ds3->pressure_r2;
    }

    uint32_t ds4_buttons = 0;

    // Face buttons: Triangle(0x10), Circle(0x20), Cross(0x40), Square(0x80)
    if (config->swap_ab) {
        if (ds3->buttons2 & 0x40) ds4_buttons |= DS4_BUTTON_CIRCLE;
        if (ds3->buttons2 & 0x20) ds4_buttons |= DS4_BUTTON_CROSS;
    } else {
        if (ds3->buttons2 & 0x40) ds4_buttons |= DS4_BUTTON_CROSS;
        if (ds3->buttons2 & 0x20) ds4_buttons |= DS4_BUTTON_CIRCLE;
    }

    if (config->swap_xy) {
        if (ds3->buttons2 & 0x80) ds4_buttons |= DS4_BUTTON_TRIANGLE;
        if (ds3->buttons2 & 0x10) ds4_buttons |= DS4_BUTTON_SQUARE;
    } else {
        if (ds3->buttons2 & 0x80) ds4_buttons |= DS4_BUTTON_SQUARE;
        if (ds3->buttons2 & 0x10) ds4_buttons |= DS4_BUTTON_TRIANGLE;
    }

    // Shoulders & Triggers
    if (g_config.swap_triggers_bumpers) {
        if (ds3->buttons2 & 0x04) ds4_buttons |= DS4_BUTTON_L2;
        if (ds3->buttons2 & 0x08) ds4_buttons |= DS4_BUTTON_R2;
        if (ds3->pressure_l2 >= config->trigger_threshold) ds4_buttons |= DS4_BUTTON_L1;
        if (ds3->pressure_r2 >= config->trigger_threshold) ds4_buttons |= DS4_BUTTON_R1;
    } else {
        if (ds3->buttons2 & 0x04) ds4_buttons |= DS4_BUTTON_L1;
        if (ds3->buttons2 & 0x08) ds4_buttons |= DS4_BUTTON_R1;
        if (ds3->pressure_l2 >= config->trigger_threshold) ds4_buttons |= DS4_BUTTON_L2;
        if (ds3->pressure_r2 >= config->trigger_threshold) ds4_buttons |= DS4_BUTTON_R2;
    }

    // Stick clicks (buttons1: L3=0x02, R3=0x04)
    if (ds3->buttons1 & 0x02) ds4_buttons |= DS4_BUTTON_L3;
    if (ds3->buttons1 & 0x04) ds4_buttons |= DS4_BUTTON_R3;

    // Start / Select (buttons1: Select=0x01, Start=0x08)
    if (ds3->buttons1 & 0x08) ds4_buttons |= DS4_BUTTON_OPTIONS;
    if (ds3->buttons1 & 0x01) ds4_buttons |= DS4_BUTTON_SHARE;

    // D-Pad (buttons1: Up=0x10, Right=0x20, Down=0x40, Left=0x80)
    if (ds3->buttons1 & 0x10) ds4_buttons |= DS4_BUTTON_DPAD_UP;
    if (ds3->buttons1 & 0x20) ds4_buttons |= DS4_BUTTON_DPAD_RIGHT;
    if (ds3->buttons1 & 0x40) ds4_buttons |= DS4_BUTTON_DPAD_DOWN;
    if (ds3->buttons1 & 0x80) ds4_buttons |= DS4_BUTTON_DPAD_LEFT;

    // PS Button (buttons3 & 0x01)
    if (ds3->buttons3 & 0x01) {
        if (g_config.guide_action == CONFIG_GUIDE_TOUCHPAD) {
            ds4_buttons |= DS4_BUTTON_TOUCHPAD;
        } else if (g_config.guide_action == CONFIG_GUIDE_SHARE) {
            ds4_buttons |= DS4_BUTTON_SHARE;
        } else {
            ds4_buttons |= DS4_BUTTON_PS;
        }
    }

    ds4->buttons = ds4_buttons;
    ds4->connected = 1;
    ds4->count = 1;
    ds4->timestamp = sceKernelGetProcessTime();
    ds4->touch.fingers = 0;
}

void translator_convert_dualsense(const DualSenseReport* ps5, OrbisPadData* ds4, const TranslatorConfig* config) {
    TranslatorConfig default_config;
    if (!config) {
        translator_init(&default_config);
        config = &default_config;
    }

    memset(ds4, 0, sizeof(OrbisPadData));

    // Analog sticks (DualSense native coordinates match DS4: 0 = Top/Left, 255 = Bottom/Right)
    uint8_t lx = ps5->left_stick_x;
    uint8_t ly = ps5->left_stick_y;
    uint8_t rx = ps5->right_stick_x;
    uint8_t ry = ps5->right_stick_y;

    if (config->stick_deadzone > 0) {
        lx = translator_apply_deadzone(lx, config->stick_deadzone);
        ly = translator_apply_deadzone(ly, config->stick_deadzone);
        rx = translator_apply_deadzone(rx, config->stick_deadzone);
        ry = translator_apply_deadzone(ry, config->stick_deadzone);
    }

    ds4->leftStick.x = lx;
    ds4->leftStick.y = ly;
    ds4->rightStick.x = rx;
    ds4->rightStick.y = ry;

    // Analog triggers
    if (g_config.swap_triggers_bumpers) {
        ds4->analogButtons.l2 = (ps5->buttons[1] & 0x01) ? 255 : 0;
        ds4->analogButtons.r2 = (ps5->buttons[1] & 0x02) ? 255 : 0;
    } else {
        ds4->analogButtons.l2 = ps5->left_trigger;
        ds4->analogButtons.r2 = ps5->right_trigger;
    }

    uint32_t ds4_buttons = 0;

    // Face buttons: Square(0x10), Cross(0x20), Circle(0x40), Triangle(0x80)
    if (config->swap_ab) {
        if (ps5->buttons[0] & 0x20) ds4_buttons |= DS4_BUTTON_CIRCLE;
        if (ps5->buttons[0] & 0x40) ds4_buttons |= DS4_BUTTON_CROSS;
    } else {
        if (ps5->buttons[0] & 0x20) ds4_buttons |= DS4_BUTTON_CROSS;
        if (ps5->buttons[0] & 0x40) ds4_buttons |= DS4_BUTTON_CIRCLE;
    }

    if (config->swap_xy) {
        if (ps5->buttons[0] & 0x10) ds4_buttons |= DS4_BUTTON_TRIANGLE;
        if (ps5->buttons[0] & 0x80) ds4_buttons |= DS4_BUTTON_SQUARE;
    } else {
        if (ps5->buttons[0] & 0x10) ds4_buttons |= DS4_BUTTON_SQUARE;
        if (ps5->buttons[0] & 0x80) ds4_buttons |= DS4_BUTTON_TRIANGLE;
    }

    // Shoulders & Triggers in buttons[1]:
    // L1 = 0x01, R1 = 0x02, L2_btn = 0x04, R2_btn = 0x08
    if (g_config.swap_triggers_bumpers) {
        if (ps5->buttons[1] & 0x01) ds4_buttons |= DS4_BUTTON_L2;
        if (ps5->buttons[1] & 0x02) ds4_buttons |= DS4_BUTTON_R2;
        if (ps5->left_trigger >= config->trigger_threshold) ds4_buttons |= DS4_BUTTON_L1;
        if (ps5->right_trigger >= config->trigger_threshold) ds4_buttons |= DS4_BUTTON_R1;
    } else {
        if (ps5->buttons[1] & 0x01) ds4_buttons |= DS4_BUTTON_L1;
        if (ps5->buttons[1] & 0x02) ds4_buttons |= DS4_BUTTON_R1;
        if (ps5->left_trigger >= config->trigger_threshold) ds4_buttons |= DS4_BUTTON_L2;
        if (ps5->right_trigger >= config->trigger_threshold) ds4_buttons |= DS4_BUTTON_R2;
    }

    // Menu & Stick clicks in buttons[1]:
    // Create/Share = 0x10, Options = 0x20, L3 = 0x40, R3 = 0x80
    if (ps5->buttons[1] & 0x10) ds4_buttons |= DS4_BUTTON_SHARE;
    if (ps5->buttons[1] & 0x20) ds4_buttons |= DS4_BUTTON_OPTIONS;
    if (ps5->buttons[1] & 0x40) ds4_buttons |= DS4_BUTTON_L3;
    if (ps5->buttons[1] & 0x80) ds4_buttons |= DS4_BUTTON_R3;

    // D-Pad Hat Switch (lower 4 bits of buttons[0])
    uint8_t hat = ps5->buttons[0] & 0x0F;
    switch (hat) {
        case 0: ds4_buttons |= DS4_BUTTON_DPAD_UP; break;
        case 1: ds4_buttons |= DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_RIGHT; break;
        case 2: ds4_buttons |= DS4_BUTTON_DPAD_RIGHT; break;
        case 3: ds4_buttons |= DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_RIGHT; break;
        case 4: ds4_buttons |= DS4_BUTTON_DPAD_DOWN; break;
        case 5: ds4_buttons |= DS4_BUTTON_DPAD_DOWN | DS4_BUTTON_DPAD_LEFT; break;
        case 6: ds4_buttons |= DS4_BUTTON_DPAD_LEFT; break;
        case 7: ds4_buttons |= DS4_BUTTON_DPAD_UP | DS4_BUTTON_DPAD_LEFT; break;
        default: break;
    }

    // PS Button and Touchpad click in buttons[2]:
    // PS = 0x01, Touchpad click = 0x02
    if (ps5->buttons[2] & 0x01) {
        if (g_config.guide_action == CONFIG_GUIDE_TOUCHPAD) {
            ds4_buttons |= DS4_BUTTON_TOUCHPAD;
        } else if (g_config.guide_action == CONFIG_GUIDE_SHARE) {
            ds4_buttons |= DS4_BUTTON_SHARE;
        } else {
            ds4_buttons |= DS4_BUTTON_PS;
        }
    }
    if (ps5->buttons[2] & 0x02) {
        ds4_buttons |= DS4_BUTTON_TOUCHPAD;
    }

    ds4->buttons = ds4_buttons;

    // Capacitive Touchpad (up to 2 points)
    uint8_t finger_count = 0;
    if ((ps5->touch_points[0].contact & 0x80) == 0) {
        ds4->touch.touch[0].finger = 0;
        ds4->touch.touch[0].x = (uint16_t)ps5->touch_points[0].x_lo | (((uint16_t)(ps5->touch_points[0].x_hi_y_lo & 0x0F)) << 8);
        ds4->touch.touch[0].y = (uint16_t)(ps5->touch_points[0].x_hi_y_lo >> 4) | (((uint16_t)ps5->touch_points[0].y_hi) << 4);
        finger_count++;
    }
    if ((ps5->touch_points[1].contact & 0x80) == 0) {
        ds4->touch.touch[1].finger = 1;
        ds4->touch.touch[1].x = (uint16_t)ps5->touch_points[1].x_lo | (((uint16_t)(ps5->touch_points[1].x_hi_y_lo & 0x0F)) << 8);
        ds4->touch.touch[1].y = (uint16_t)(ps5->touch_points[1].x_hi_y_lo >> 4) | (((uint16_t)ps5->touch_points[1].y_hi) << 4);
        finger_count++;
    }
    ds4->touch.fingers = finger_count;

    ds4->connected = 1;
    ds4->count = 1;
    ds4->timestamp = sceKernelGetProcessTime();
}
