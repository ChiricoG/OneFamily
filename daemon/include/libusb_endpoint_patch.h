/*
 * libusb_endpoint_descriptor patch for OpenOrbis toolchain
 *
 * The OpenOrbis libusb.h forward-declares libusb_endpoint_descriptor
 * but never provides its full definition.  We supply the standard
 * USB spec layout here so that controller_detect.c can dereference it.
 *
 * Include this header BEFORE any other orbis/Usbd.h inclusion.
 */

#ifndef LIBUSB_ENDPOINT_PATCH_H
#define LIBUSB_ENDPOINT_PATCH_H

#include <stdint.h>

/* Full definition – must appear before <orbis/Usbd.h> pulls in libusb.h */
struct libusb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
    uint8_t  bRefresh;        /* audio only */
    uint8_t  bSynchAddress;   /* audio only */
    const unsigned char *extra;
    int32_t  extra_length;
};

#include <orbis/Usbd.h>

#endif /* LIBUSB_ENDPOINT_PATCH_H */
