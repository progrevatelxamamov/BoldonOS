#include <inout/ps2mouse.h>
#include <inout/ps2.h>
#include <inout/inout.h>
#include <cpu/time.h>
#include <stdint.h>
#include <stdbool.h>

static uint8_t packet[4];
static int     packet_index = 0;
static int     packet_size  = 3;
static uint8_t prev_btns    = 0;
static uint64_t last_byte_ms = 0;
static bool    stream_ready = false;

void ps2mouse_resync(void) {
    packet_index = 0;
    prev_btns = 0;
    last_byte_ms = get_monotonic_ms();
}

static int mouse_cmd(uint8_t cmd) {
    for (int retry = 0; retry < 3; retry++) {
        if (!ps2_write_mouse(cmd)) return -1;
        for (int i = 0; i < 96; i++) {
            uint8_t b;
            if (!ps2_read_any_timeout(&b)) break;
            if (b == 0xFA) return 0;
            if (b == 0xFE) break;
            if (b == 0xFC || b == 0xFD) return -1;
            if ((b & 0xC0) == 0xC0) continue;
        }
    }
    return -1;
}

static int mouse_read_id(void) {
    if (!ps2_write_mouse(0xF2)) return -1;
    bool ack = false;
    for (int i = 0; i < 96; i++) {
        uint8_t b;
        if (!ps2_read_any_timeout(&b)) break;
        if (b == 0xFA) { ack = true; continue; }
        if (!ack) continue;
        if (b == 0x00 || b == 0x02 || b == 0x03 || b == 0x04) return (int)b;
        if ((b & 0xC0) != 0xC0) continue;
    }
    return -1;
}

void ps2mouse_init(void) {
    stream_ready = false;
    packet_index = 0;
    prev_btns = 0;
    packet_size = 3;

    ps2_write_mouse(0xF5);
    ps2_drain_quiet(512);

    if (mouse_cmd(0xFF) == 0) {
        ps2_drain_quiet(1024);
    }

    if (mouse_cmd(0xF5) != 0) return;

    if (mouse_cmd(0xF3) == 0 && mouse_cmd(200) == 0 &&
        mouse_cmd(0xF3) == 0 && mouse_cmd(100) == 0 &&
        mouse_cmd(0xF3) == 0 && mouse_cmd(80) == 0) {
        int id = mouse_read_id();
        if (id == 3 || id == 4) packet_size = 4;
    }

    if (mouse_cmd(0xE8) == 0) mouse_cmd(3);
    if (mouse_cmd(0xF3) == 0) mouse_cmd(100);

    ps2_drain_quiet(256);

    if (mouse_cmd(0xF4) != 0) return;

    ps2mouse_resync();
    stream_ready = true;
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
    if (!stream_ready) return;

    uint64_t now = get_monotonic_ms();
    uint64_t gap = now - last_byte_ms;
    last_byte_ms = now;

    if (packet_index > 0 && gap >= 3) packet_index = 0;

    if (packet_index == 0) {
        if ((byte & 0x08) == 0 || (byte & 0xC0)) return;
        packet[0] = byte;
        packet_index = 1;
        return;
    }

    packet[packet_index++] = byte;
    if (packet_index < packet_size) return;

    packet_index = 0;

    uint8_t flags = packet[0];
    if (flags & 0xC0) return;

    int dx = (int)packet[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)packet[2] - ((flags & 0x20) ? 256 : 0);
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
