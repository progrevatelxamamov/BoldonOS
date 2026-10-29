#ifndef BOLDON_LP_PLATFORM_H
#define BOLDON_LP_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
int lp_usb_failed(void);
void lp_usb_lock(void);
void lp_usb_unlock(void);
int lp_platform_controller(unsigned index, uint32_t *pci, uint8_t *type, uintptr_t *bar);
void lp_platform_prepare(uint32_t pci, uint8_t type, uintptr_t bar);
void lp_backend_initialize(void);
void lp_backend_poll(void);
void *boldon_usb_input_create(int protocol, const uint8_t *descriptor, uint32_t length, int *mode);
void boldon_usb_input_protocol(void *context, int mode);
void boldon_usb_input_report(void *context, const uint8_t *data, uint32_t length);
uint8_t lp_platform_leds(void);
int boldon_usb_input_led_report(void *context, uint8_t leds, uint8_t *data, uint8_t *id);
void boldon_usb_input_destroy(void *context);
#endif
