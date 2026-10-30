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

#ifdef __cplusplus
extern "C" {
#endif

// Virtual Pad Device Lifecycle
int  vpad_init(void);
void vpad_shutdown(void);
bool vpad_is_active(void);

// User & Player Slot Binding
int  vpad_bind_user(ConfigMode mode, int *out_slot, char *out_username, size_t username_len);
int  vpad_get_assigned_slot(void);
const char* vpad_get_assigned_user(void);
void vpad_check_user_rebind(ConfigMode mode);
void vpad_pulse_ps_button(void);

// Data Insertion (with smart delta rate-limiting)
int  vpad_send_data(const OrbisPadData *data);

// Rumble / Vibration Callback & Polling
typedef void (*vpad_rumble_cb_t)(uint8_t large_motor, uint8_t small_motor);
void vpad_set_rumble_callback(vpad_rumble_cb_t cb);
void vpad_poll_rumble(void);

#ifdef __cplusplus
}
#endif

#endif // VPAD_H
