#ifndef VPAD_H
#define VPAD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ds4.h"

#ifndef CONFIG_MODE_DEFINED
#define CONFIG_MODE_DEFINED
typedef enum {
    CONFIG_MODE_AUTO = 0,       // Merge if 1 user, Secondary if >= 2 users
    CONFIG_MODE_MERGE,          // Force Player 1 (Merge)
    CONFIG_MODE_SECONDARY       // Force Player 2 (Secondary)
} ConfigMode;
#endif

#define VPAD_MAX_SLOTS 2

typedef enum {
    VPAD_SOURCE_NONE = 0,
    VPAD_SOURCE_USB,
    VPAD_SOURCE_BT
} VPadSource;

#ifdef __cplusplus
extern "C" {
#endif

// Multi-Slot Device Lifecycle
int  vpad_slot_open(VPadSource source, int *out_slot);
void vpad_slot_close(int slot);
bool vpad_slot_is_active(int slot);
int  vpad_send_data_slot(int slot, const OrbisPadData *data);
void vpad_pulse_ps_button_slot(int slot);
int  vpad_get_assigned_slot_num(int slot);

// Subsystem Lifecycle (initializes / unloads PRXs)
int  vpad_init(void);
void vpad_shutdown(void);
bool vpad_is_active(void);

// Backwards-compatible single-pad helpers
int  vpad_bind_user(ConfigMode mode, int *out_slot, char *out_username, size_t username_len);
int  vpad_get_assigned_slot(void);
const char* vpad_get_assigned_user(void);
void vpad_check_user_rebind(ConfigMode mode);
void vpad_pulse_ps_button(void);
int  vpad_send_data(const OrbisPadData *data);

// Rumble / Vibration Callback & Polling
typedef void (*vpad_rumble_cb_t)(uint8_t large_motor, uint8_t small_motor);
void vpad_set_rumble_callback(vpad_rumble_cb_t cb);
void vpad_poll_rumble(void);

#ifdef __cplusplus
}
#endif

#endif // VPAD_H
