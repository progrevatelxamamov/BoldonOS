#include "gconsole.h"
#include <inout/ps2kbd.h>
#include <inout/ps2.h>
#include <inout/inout.h>
#include <mm/malloc.h>
#include <stdint.h>
#include <stdbool.h>

static bool extended = false;
static bool in_pause  = false;
static uint8_t pause_bytes = 0;   /* сколько байт уже проглотили в E1-последовательности */
static bool ps2_kbd_present = false;

void ps2kbd_init(void) {
    extended = false;
    in_pause  = false;
    pause_bytes = 0;

    if (ps2_write_kbd(0xF4)) {
        uint8_t resp = ps2_read_byte();
        if (resp == 0xFA) {
            ps2_kbd_present = true;
        }
    }
}

static bool ps2kbd_send_with_ack(uint8_t value) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!ps2_write_kbd(value)) return false;
        for (int responses = 0; responses < 8; responses++) {
            uint8_t response;
            if (!ps2_read_kbd_byte_timeout(&response)) return false;
            if (response == 0xFA) return true;
            if (response == 0xFE) break;
            ps2kbd_feed(response);
        }
    }
    return false;
}

void ps2kbd_set_leds(uint8_t leds) {
    if (!ps2_kbd_present) return;

    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");

    ps2_flush_input();
    if (ps2kbd_send_with_ack(0xED))
        (void)ps2kbd_send_with_ack(leds);

    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static int translate(uint8_t sc, bool *is_release) {
    *is_release = (sc & 0x80) != 0;

    /* 0xFA=ACK, 0xEE=Echo, 0xFC=BAT-fail — ответы устройства на команды.
       ВАЖНО: 0xAA сюда НЕ входит. 0xAA приходит и как "BAT passed" сразу
       после RESET (один раз, на инициализации), НО в обычном потоке IRQ это
       break-код ЛЕВОГО SHIFT (0x2A | 0x80). Раньше он отбрасывался вместе
       с ответами, отпускание Shift терялось и модификатор "залипал" навсегда:
       один make 0x2A без парного break 0xAA. */
    if (sc == 0xFA || sc == 0xEE || sc == 0xFC) {
        extended = false;   /* не даём ответу устройства "залипнуть" префикс */
        return -1;
    }
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

    /* code == 0 не бывает (это ложное срабатывание при переполнении).
       Намеренно НЕ обрезаем code > 0x58: одиночные multimedia/ACPI
       сканкоды 0x5E..0x7F должны доходить до inout, а ответы устройства
       (ACK/Echo/BAT-fail) уже отфильтрованы выше по полному байту. */
    if (code == 0) return -1;
    return (int)code;
}

void ps2kbd_feed(uint8_t byte) {
    if (byte == 0xE0) { extended = true; return; }
    if (byte == 0xE1) { in_pause = true; pause_bytes = 0; return; }
    if (in_pause) {
        /* Pause/Break даёт E1 1D 45 E1 9D C5. Если хвост C5 потерялся,
           раньше in_pause оставался TRUE навсегда и клавиатура "умирала":
           все последующие сканкоды молча глотались. Ограничиваем длину. */
        if (byte == 0xC5 || ++pause_bytes >= 6) {
            in_pause = false;
            pause_bytes = 0;
        }
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