#ifndef PS2KBD_H
#define PS2KBD_H

#include <stdint.h>

void ps2kbd_init(void);
void ps2kbd_feed(uint8_t byte);
void ps2kbd_set_leds(uint8_t leds);
void ps2kbd_reset_state(void);

#endif
