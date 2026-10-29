#ifndef BOLDON_LIBPAYLOAD_USB_H
#define BOLDON_LIBPAYLOAD_USB_H
#include <stdint.h>
#include <pci/pci.h>
void libpayload_pci_register(pci_device_t *dev);
void libpayload_usb_start(void);
void libpayload_usb_ready(void);
void libpayload_usb_set_leds(uint8_t leds);
void *boldon_usb_input_create(int protocol, const uint8_t *descriptor, uint32_t length, int *report_mode);
void boldon_usb_input_report(void *context, const uint8_t *data, uint32_t length);
void boldon_usb_input_protocol(void *context, int report_mode);
void boldon_usb_input_destroy(void *context);
#endif
