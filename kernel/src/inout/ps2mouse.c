#include <inout/ps2mouse.h>
#include <inout/ps2.h>
#include <inout/inout.h>
#include <mm/malloc.h>
#include <stdint.h>
#include <stdbool.h>
#include <io.h>

static uint8_t packet[4];
static int     packet_index = 0;
static int     packet_size  = 3;
static uint8_t prev_btns    = 0;

static uint8_t mouse_cmd(uint8_t cmd) {
    ps2_write_mouse(cmd);
    return ps2_read_byte();
}

static void mouse_set_sample(uint8_t rate) {
    mouse_cmd(0xF3);
    mouse_cmd(rate);
}

static void mouse_set_resolution(uint8_t res) {
    mouse_cmd(0xE8);
    mouse_cmd(res);
}

static void mouse_set_scaling_1_1(void) {
    mouse_cmd(0xE6);
}

static int mouse_get_id(void) {
    mouse_cmd(0xF2);
    return ps2_read_byte();
}

static bool mouse_enable_wheel(void) {
    mouse_set_sample(200);
    mouse_set_sample(100);
    mouse_set_sample(80);
    int id = mouse_get_id();
    if (id == 3 || id == 4) {
        packet_size = 4;
        return true;
    }
    mouse_set_sample(100);
    return false;
}

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define STATUS_OUTPUT_FULL  0x01
#define STATUS_INPUT_FULL   0x02

#define CMD_WRITE_MOUSE     0xD4
#define CMD_DISABLE_KBD     0xAD
#define CMD_ENABLE_KBD      0xAE
#define CMD_DISABLE_MOUSE   0xA7
#define CMD_ENABLE_MOUSE    0xA8

bool ps2_try_read_byte(uint8_t *out) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) {
            *out = inb(PS2_DATA);
            return true;
        }
    }
    return false;
}

static int mouse_cmd_expect_ack(uint8_t cmd) {
    for (int retry = 0; retry < 3; retry++) {
        ps2_write_mouse(cmd);
        uint8_t resp;
        if (!ps2_try_read_byte(&resp)) continue;
        if (resp == 0xFA) return 0;
        if (resp == 0xFE) continue;
        return -1;
    }
    return -1;
}

void ps2mouse_init(void) {
    packet_index = 0;
    prev_btns    = 0;
    packet_size  = 3;

    if (mouse_cmd_expect_ack(0xF6) < 0) return;   /* defaults */

    if (mouse_cmd_expect_ack(0xE8) < 0) return;   /* resolution */
    if (mouse_cmd_expect_ack(3) < 0) return;      /* 8 counts/mm */

    if (mouse_cmd_expect_ack(0xF3) < 0) return;   /* sample rate */
    if (mouse_cmd_expect_ack(100) < 0) return;

    if (mouse_cmd_expect_ack(0xF3) == 0) {
        if (mouse_cmd_expect_ack(200) == 0 &&
            mouse_cmd_expect_ack(0xF3) == 0 &&
            mouse_cmd_expect_ack(100) == 0 &&
            mouse_cmd_expect_ack(0xF3) == 0 &&
            mouse_cmd_expect_ack(80) == 0)
        {
            if (mouse_cmd_expect_ack(0xF2) == 0) {
                uint8_t id;
                if (ps2_try_read_byte(&id)) {
                    if (id == 3 || id == 4) packet_size = 4;
                }
            }
        }
    }

    if (mouse_cmd_expect_ack(0xF4) < 0) return;   /* enable reporting */
}

static void emit_move(int dx, int dy, int dz) {
    if (dx == 0 && dy == 0 && dz == 0) return;

    inout_event_t ev;
    ev.type = INOUT_EVENT_MOUSE;
    ev.data.mouse.mouse_state = INOUT_MOUSE_MOVE;
    ev.data.mouse.x = dx;
    ev.data.mouse.y = dy;
    ev.data.mouse.z = dz;
    ev.data.mouse.mouse_btn = 0;

    inout_add_event(ev);
    inout_call_scheduler();
}

static void emit_button(int btn, bool down) {
    inout_event_t ev;
    ev.type = INOUT_EVENT_MOUSE;
    ev.data.mouse.mouse_state = down ? INOUT_MOUSE_DOWN : INOUT_MOUSE_UP;
    ev.data.mouse.mouse_btn = btn;
    ev.data.mouse.x = 0;
    ev.data.mouse.y = 0;
    ev.data.mouse.z = 0;

    inout_add_event(ev);
    inout_call_scheduler();
}

void ps2mouse_feed(uint8_t byte) {
    if (packet_index == 0 && (byte & 0x08) == 0)
        return;

    packet[packet_index++] = byte;

    if (packet_index < packet_size)
        return;

    packet_index = 0;

    uint8_t flags = packet[0];

    if (flags & 0xC0)
        return;

    int dx = (int8_t)packet[1];
    int dy = (int8_t)packet[2];

    dy = -dy;

    int dz = 0;
    if (packet_size == 4) {
        int8_t z = (int8_t)packet[3];
        if (z == -128 || z == 127) z = 0;
        dz = z;
    }

    uint8_t curr_btns = flags & 0x07;
    uint8_t changed   = curr_btns ^ prev_btns;

    if (changed & 0x01) emit_button(INOUT_MOUSE_LEFTBTN,   (curr_btns & 0x01) != 0);
    if (changed & 0x02) emit_button(INOUT_MOUSE_RIGHTBTN,  (curr_btns & 0x02) != 0);
    if (changed & 0x04) emit_button(INOUT_MOUSE_CENTERBTN, (curr_btns & 0x04) != 0);

    prev_btns = curr_btns;

    emit_move(dx, dy, dz);
}