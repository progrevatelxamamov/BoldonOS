#include <inout/ps2.h>
#include <inout/ps2kbd.h>
#include <inout/ps2mouse.h>
#include <io.h>
#include <gconsole.h>
#include <stdint.h>
#include <stdbool.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define STATUS_OUTPUT_FULL  0x01
#define STATUS_INPUT_FULL   0x02
#define STATUS_MOUSE_DATA   0x20

#define CMD_READ_CONFIG   0x20
#define CMD_WRITE_CONFIG  0x60
#define CMD_WRITE_MOUSE     0xD4
#define CMD_DISABLE_KBD     0xAD
#define CMD_ENABLE_KBD      0xAE
#define CMD_DISABLE_MOUSE   0xA7
#define CMD_ENABLE_MOUSE    0xA8

static void wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if ((inb(PS2_STATUS) & STATUS_INPUT_FULL) == 0)
            return;
}

static void wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
            return;
}

static void flush_output(void) {
    for (int i = 0; i < 32; i++) {
        if ((inb(PS2_STATUS) & STATUS_OUTPUT_FULL) == 0) break;
        (void)inb(PS2_DATA);
    }
}

uint8_t ps2_read_byte(void) {
    wait_read();
    return inb(PS2_DATA);
}

static void write_cmd(uint8_t cmd) {
    wait_write();
    outb(PS2_CMD, cmd);
}

static void write_data(uint8_t val) {
    wait_write();
    outb(PS2_DATA, val);
}

bool ps2_write_kbd(uint8_t val) {
    write_data(val);
    return true;
}

bool ps2_write_mouse(uint8_t val) {
    write_cmd(CMD_WRITE_MOUSE);
    write_data(val);
    return true;
}


void ps2_init(void) {
    flush_output();

    write_cmd(CMD_DISABLE_KBD);
    write_cmd(CMD_DISABLE_MOUSE);
    flush_output();

    ps2kbd_init();
    ps2mouse_init();

    flush_output();

    write_cmd(CMD_READ_CONFIG);
    uint8_t config = ps2_read_byte();

    config |= (1 << 0) | (1 << 1) | (1 << 6);
    config &= ~((1 << 4) | (1 << 5));

    write_cmd(CMD_WRITE_CONFIG);
    write_data(config);

    write_cmd(CMD_ENABLE_KBD);
    write_cmd(CMD_ENABLE_MOUSE);
    flush_output();
}

void ps2_irq_keyboard(void) {
    uint8_t status = inb(PS2_STATUS);
    while (status & STATUS_OUTPUT_FULL) {
        if (status & STATUS_MOUSE_DATA) {
            ps2mouse_feed(inb(PS2_DATA));
        } else {
            ps2kbd_feed(inb(PS2_DATA));
        }
        status = inb(PS2_STATUS);
    }
}

void ps2_irq_mouse(void) {
    uint8_t status = inb(PS2_STATUS);
    while (status & STATUS_OUTPUT_FULL) {
        if (status & STATUS_MOUSE_DATA) {
            ps2mouse_feed(inb(PS2_DATA));
        } else {
            ps2kbd_feed(inb(PS2_DATA));
        }
        status = inb(PS2_STATUS);
    }
}