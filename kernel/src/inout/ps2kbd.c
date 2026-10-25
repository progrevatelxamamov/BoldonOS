#include "gconsole.h"
#include <inout/ps2kbd.h>
#include <inout/ps2.h>
#include <inout/inout.h>
#include <mm/malloc.h>
#include <stdint.h>
#include <stdbool.h>

static bool extended = false;
static bool in_pause  = false;
static bool ps2_kbd_present = false;

void ps2kbd_init(void) {
    extended = false;
    in_pause  = false;

    if (ps2_write_kbd(0xF4)) {
        uint8_t resp = ps2_read_byte();
        if (resp == 0xFA) {
            ps2_kbd_present = true;
        }
    }
}

void ps2kbd_set_leds(uint8_t leds) {
    if (!ps2_kbd_present) return;

    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");

    if (ps2_write_kbd(0xED)) {
        if (ps2_read_byte() == 0xFA) {
            if (ps2_write_kbd(leds)) {
                (void)ps2_read_byte();
            }
        } else {
            ps2_kbd_present = false;
        }
    } else {
        ps2_kbd_present = false;
    }

    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static int translate(uint8_t sc, bool *is_release) {
    *is_release = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;

    if (extended) {
        extended = false;
        switch (code) {
        case 0x1D: return KEY_RCTRL;
        case 0x38: return KEY_RALT;
        case 0x1C: return KEY_KP_ENTER;
        case 0x35: return KEY_KP_SLASH;
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UP;
        case 0x49: return KEY_PGUP;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x4F: return KEY_END;
        case 0x50: return KEY_DOWN;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        case 0x5B: return KEY_LGUI;
        case 0x5C: return KEY_RGUI;
        case 0x5D: return KEY_MENU;
        default:   return -1;
        }
    }

    if (code == 0xAA || code == 0xFA) return -1;
    if (code >= 0x80) return -1;
    return (int)code;
}

void ps2kbd_feed(uint8_t byte) {
    if (byte == 0xE0) { extended = true; return; }
    if (byte == 0xE1) { in_pause = true; return; }
    if (in_pause) {
        if (byte == 0xC5) in_pause = false;
        return;
    }

    bool release;
    int key = translate(byte, &release);
    if (key < 0) return;

    inout_event_t ev;
    ev.type = INOUT_EVENT_KEY;
    ev.data.key.key_state = release ? INOUT_KEY_UP : INOUT_KEY_DOWN;
    ev.data.key.keycode = (uint64_t)key;

    inout_add_event(ev);
    inout_call_scheduler();
}