#ifndef PS2_H
#define PS2_H

#include <stdint.h>
#include <stdbool.h>

void ps2_init(void);

void ps2_irq_keyboard(void);
void ps2_irq_mouse(void);

bool ps2_write_kbd(uint8_t val);
bool ps2_write_mouse(uint8_t val);
uint8_t ps2_read_byte(void);
bool ps2_read_any_timeout(uint8_t *out);
bool ps2_read_kbd_byte_timeout(uint8_t *out);
void ps2_flush_input(void);
void ps2_drain_quiet(uint32_t idle_spins);

#endif
