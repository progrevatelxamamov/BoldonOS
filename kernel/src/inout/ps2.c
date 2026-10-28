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

static bool wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if ((inb(PS2_STATUS) & STATUS_INPUT_FULL) == 0)
            return true;
    return false;
}

static bool wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
            return true;
    return false;
}

uint8_t ps2_read_byte(void) {
    wait_read();
    return inb(PS2_DATA);
}

bool ps2_read_any_timeout(uint8_t *out) {
    if (!out) return false;
    for (uint32_t i = 0; i < 300000; i++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) {
            *out = inb(PS2_DATA);
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

void ps2_drain_quiet(uint32_t idle_spins) {
    uint32_t drained = 0;
    for (uint32_t idle = 0; idle < idle_spins && drained < 1024; idle++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) {
            (void)inb(PS2_DATA);
            drained++;
            idle = 0;
            continue;
        }
        for (volatile uint32_t k = 0; k < 200; k++) __asm__ volatile("pause");
    }
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
    if (!wait_write()) return false;
    outb(PS2_DATA, val);
    return true;
}

bool ps2_write_mouse(uint8_t val) {
    if (!wait_write()) return false;
    outb(PS2_CMD, CMD_WRITE_MOUSE);
    if (!wait_write()) return false;
    outb(PS2_DATA, val);
    return true;
}

bool ps2_read_kbd_byte_timeout(uint8_t *out) {
    if (!out) return false;
    for (int i = 0; i < 200000; i++) {
        uint8_t status = inb(PS2_STATUS);
        if ((status & STATUS_OUTPUT_FULL) == 0) continue;
        uint8_t byte = inb(PS2_DATA);
        if (status & STATUS_MOUSE_DATA) {
            ps2mouse_feed(byte);
            continue;
        }
        *out = byte;
        return true;
    }
    return false;
}

void ps2_flush_input(void) {
    for (int i = 0; i < 32; i++) {
        uint8_t status = inb(PS2_STATUS);
        if ((status & STATUS_OUTPUT_FULL) == 0) break;
        uint8_t byte = inb(PS2_DATA);
        if (status & STATUS_MOUSE_DATA) ps2mouse_feed(byte);
        else ps2kbd_feed(byte);
    }
}

void ps2_init(void) {
    write_cmd(CMD_DISABLE_KBD);
    write_cmd(CMD_DISABLE_MOUSE);
    ps2_drain_quiet(64);

    write_cmd(CMD_READ_CONFIG);
    uint8_t config = ps2_read_byte();

    config |= (1 << 0) | (1 << 1) | (1 << 6);
    config &= ~((1 << 4) | (1 << 5));

    write_cmd(CMD_WRITE_CONFIG);
    write_data(config);

    write_cmd(CMD_ENABLE_MOUSE);
    write_cmd(CMD_ENABLE_KBD);

    ps2kbd_init();
    ps2mouse_init();

    ps2_drain_quiet(256);

    ps2kbd_reset_state();
    ps2mouse_resync();
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
