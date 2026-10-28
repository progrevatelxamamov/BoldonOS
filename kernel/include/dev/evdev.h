#ifndef BOLDON_EVDEV_H
#define BOLDON_EVDEV_H

#include <stdint.h>
#include <stdbool.h>

void evdev_init(void);
void evdev_report_key(uint8_t keycode, bool down);
void evdev_report_mouse(int dx, int dy, int dz, uint8_t buttons, uint8_t changed);
void evdev_report_sync(int device);

#endif
