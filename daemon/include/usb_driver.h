#ifndef DAEMON_USB_DRIVER_H
#define DAEMON_USB_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "controller_detect.h"
#include "ds4.h"

typedef struct {
    bool connected;
    uint16_t vid;
    uint16_t pid;
    ControllerType type;
    int device_handle;
    int endpoint_in;
    int endpoint_out;
    bool is_wireless;
    uint8_t bInterval;
    uint16_t wMaxPacketSize;
} UsbControllerState;

extern UsbControllerState g_usb_state;

int  usb_driver_init(void);
void usb_driver_shutdown(void);
bool usb_driver_is_connected(void);
void usb_driver_poll_step(void);
void usb_driver_send_rumble(uint8_t large_motor, uint8_t small_motor);
void usb_driver_reload_remap(void);
void usb_driver_load_remap_for_bt(void);
void usb_driver_apply_bt_remap(OrbisPadData* ds4);

#endif // DAEMON_USB_DRIVER_H
