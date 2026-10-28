/*
 * Xbox Controller to DualShock 4 Input Translator
 * Supports Xbox 360 and Xbox One controllers
 */

#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include "xbox360.h"
#include "xboxone.h"
#include "switch_controller.h"
#include "ps_controllers.h"
#include "stadia.h"
#include "ds4.h"
#include "config.h"

/*
 * Controller type enumeration
 */
typedef enum {
    CONTROLLER_NONE = 0,
    CONTROLLER_XBOX360,
    CONTROLLER_XBOXONE,
    CONTROLLER_SWITCH,
    CONTROLLER_DS3,
    CONTROLLER_DUALSENSE,
    CONTROLLER_SWITCH_PRO,
    CONTROLLER_STADIA
} ControllerType;

/*
 * Translator configuration
 */
typedef struct {
    uint8_t stick_deadzone;         // Deadzone for analog sticks (0-127)
    uint8_t trigger_threshold;      // Digital trigger activation point (0-255)
    int     invert_left_y;          // Invert left stick Y axis
    int     invert_right_y;         // Invert right stick Y axis
    int     swap_ab;                // Swap A/B buttons (for Japanese layout)
    int     swap_xy;                // Swap X/Y buttons
} TranslatorConfig;

/*
 * Initialize translator with default configuration
 */
void translator_init(TranslatorConfig* config);

/*
 * Translate Xbox 360 report to OrbisPadData
 *
 * @param xbox      Input Xbox 360 report
 * @param ds4       Output OrbisPadData structure
 * @param config    Translator configuration (or NULL for defaults)
 */
void translator_convert(const Xbox360Report* xbox, OrbisPadData* ds4, const TranslatorConfig* config);

/*
 * Apply deadzone to stick value
 *
 * @param value     Raw stick value (0-255, center=128)
 * @param deadzone  Deadzone size (0-127)
 * @return          Adjusted value with deadzone applied
 */
uint8_t translator_apply_deadzone(uint8_t value, uint8_t deadzone);

/*
 * Simple wrapper - translate Xbox 360 report to DS4 format using defaults
 */
void xbox360_to_ds4(const Xbox360Report* xbox, OrbisPadData* ds4);

/*
 * Translate Xbox One report to OrbisPadData
 *
 * @param xbox      Input Xbox One report
 * @param ds4       Output OrbisPadData structure
 * @param config    Translator configuration (or NULL for defaults)
 */
void translator_convert_xboxone(const XboxOneReport* xbox, OrbisPadData* ds4, const TranslatorConfig* config);

/*
 * Simple wrapper - translate Xbox One report to DS4 format using defaults
 */
void xboxone_to_ds4(const XboxOneReport* xbox, OrbisPadData* ds4);

/*
 * Translate Switch Input-Only controller report to OrbisPadData
 *
 * @param sw       Input Switch controller report
 * @param ds4      Output OrbisPadData structure
 * @param config   Translator configuration (or NULL for defaults)
 */
void translator_convert_switch(const SwitchInputOnlyReport* sw, OrbisPadData* ds4, const TranslatorConfig* config);

/*
 * Simple wrapper - translate Switch controller report to DS4 format using defaults
 */
void switch_to_ds4(const SwitchInputOnlyReport* sw, OrbisPadData* ds4);

/*
 * Translate DualShock 3 report to OrbisPadData
 */
void translator_convert_ds3(const DS3Report* ds3, OrbisPadData* ds4, const TranslatorConfig* config);

/*
 * Translate DualSense report to OrbisPadData
 */
void translator_convert_dualsense(const DualSenseReport* ps5, OrbisPadData* ds4, const TranslatorConfig* config);

/*
 * Translate Switch Pro Controller full report (0x30, 0x21, 0x31) to OrbisPadData
 */
void translator_convert_switch_pro(const SwitchFullReport* sw, OrbisPadData* ds4, const TranslatorConfig* config);

/*
 * Translate Google Stadia Controller report to OrbisPadData
 */
void translator_convert_stadia(const StadiaInputReport* stadia, OrbisPadData* ds4, const TranslatorConfig* config);

#endif // TRANSLATOR_H
