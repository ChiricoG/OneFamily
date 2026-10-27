#ifndef DAEMON_USB_DRIVER_H
#define DAEMON_USB_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "controller_detect.h"

typedef struct {
    bool connected;
    uint16_t vid;
    uint16_t pid;
    ControllerType type;
    int device_handle;
    int endpoint_in;
    int endpoint_out;
} UsbControllerState;

extern UsbControllerState g_usb_state;

int  usb_driver_init(void);
void usb_driver_shutdown(void);
bool usb_driver_is_connected(void);
void usb_driver_poll_step(void);
void usb_driver_send_rumble(uint8_t large_motor, uint8_t small_motor);

#endif // DAEMON_USB_DRIVER_H
