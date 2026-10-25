#ifndef PS2MOUSE_H
#define PS2MOUSE_H

#include <stdint.h>

void ps2mouse_init(void);
void ps2mouse_feed(uint8_t byte);

#endif