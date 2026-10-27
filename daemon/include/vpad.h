#ifndef VPAD_H
#define VPAD_H

#include <stdint.h>
#include <stdbool.h>
#include "ds4.h"

#ifdef __cplusplus
extern "C" {
#endif

// Virtual Pad Device Lifecycle
int  vpad_init(void);
void vpad_shutdown(void);
bool vpad_is_active(void);

// Data Insertion (250 Hz polling)
int  vpad_send_data(const OrbisPadData *data);

// Rumble / Vibration Callback & Polling
typedef void (*vpad_rumble_cb_t)(uint8_t large_motor, uint8_t small_motor);
void vpad_set_rumble_callback(vpad_rumble_cb_t cb);
void vpad_poll_rumble(void);

#ifdef __cplusplus
}
#endif

#endif // VPAD_H
