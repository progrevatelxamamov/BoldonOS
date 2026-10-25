#include "gconsole.h"
#include "kernel.h"
#include "bootloader.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>

#include <inout/inout.h>
#include <inout/ps2kbd.h>
#include <scheduler/scheduler.h>

#include <fs/devfs.h>
#include <fs/vfs.h>

void xhci_set_keyboard_leds(uint8_t leds);

#ifndef POLLIN
#define POLLIN  0x0001
#define POLLOUT 0x0004
#endif

#define EVENT_QUEUE_SIZE 512
#define MOVE_Q_SIZE      2048
#define CMD_RESP_Q_SIZE  128
#define KBD_Q_SIZE       256

process_t *inout_kbd_proc   = NULL;
process_t *inout_mouse_proc = NULL;

volatile keyboard_state_t g_keyboard;
volatile mouse_state_t    g_mouse = { .x = 0, .y = 0, .z = 0 };

volatile bool new_kbd_event   = false;
volatile bool new_mouse_event = false;

static inout_event_t kbd_events[EVENT_QUEUE_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

static inout_event_t mouse_events[EVENT_QUEUE_SIZE];
static volatile uint32_t mouse_head = 0;
static volatile uint32_t mouse_tail = 0;

/* Очередь ответов на команды (ACK 0xFA, Device ID 0x03, BAT 0xAA) */
static uint8_t  cmd_resp_q[CMD_RESP_Q_SIZE];
static volatile uint32_t cmd_resp_qh = 0, cmd_resp_qt = 0;

/* Очередь пакетов движения мыши */
static uint8_t  move_q[MOVE_Q_SIZE];
static volatile uint32_t move_qh = 0, move_qt = 0;

static uint8_t  kbd_q[KBD_Q_SIZE];
static uint32_t kbd_qh = 0, kbd_qt = 0;

/* Состояние эмулятора PS/2 / ImPS/2 */
static uint8_t  mouse_cmd_state = 0;       /* 0 = idle, 1 = wait sample rate, 2 = wait resolution */
static uint8_t  mouse_sample_magic = 0;   /* Детекция цепочки 200 -> 100 -> 80 */
static bool     mouse_imps2 = false;       /* Флаг переключения в 4-байтовый ImPS/2 */
static bool     mouse_enabled = true;

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

/* Поместить байт ответа на команду в очередь ответов */
static void cmd_resp_push(uint8_t val) {
    uint32_t next = (cmd_resp_qh + 1) % CMD_RESP_Q_SIZE;
    if (next != cmd_resp_qt) {
        cmd_resp_q[cmd_resp_qh] = val;
        cmd_resp_qh = next;
    }
}

void input_dev_on_mouse_rel(int dx, int dy, uint8_t buttons) {
    /* 1. Гарантируем, что передача данных всегда разрешена */
    mouse_enabled = true;

    int8_t sdx = dx > 127 ? 127 : dx < -128 ? -128 : (int8_t)dx;

    /* Инвертируем Y для соответствия стандарту PS/2 (в PS/2 смещение вверх — положительное) */
    int ps2_y = -dy;
    int8_t sdy = ps2_y > 127 ? 127 : ps2_y < -128 ? -128 : (int8_t)ps2_y;

    /* Байт 0 PS/2: Бит 3 (0x08) ОБЯЗАТЕЛЕН для синхронизации пакетов в Nano-X */
    uint8_t b0 = 0x08;
    if (buttons & 1) b0 |= 0x01;             /* Left */
    if (buttons & 2) b0 |= 0x02;             /* Right */
    if (buttons & 4) b0 |= 0x04;             /* Middle */
    if (sdx < 0)     b0 |= 0x10;             /* Sign X */
    if (sdy < 0)     b0 |= 0x20;             /* Sign Y */

    uint32_t pkt_len = mouse_imps2 ? 4 : 3;
    uint8_t pkt[4] = { b0, (uint8_t)sdx, (uint8_t)sdy, 0 };

    for (uint32_t i = 0; i < pkt_len; i++) {
        uint32_t next = (move_qh + 1) % MOVE_Q_SIZE;
        if (next == move_qt) break;
        move_q[move_qh] = pkt[i];
        move_qh = next;
    }
}

void input_dev_on_mouse_abs(int x, int y, uint8_t buttons) {
    static int last_x = 0, last_y = 0;
    int dx = x - last_x;
    int dy = y - last_y;
    last_x = x;
    last_y = y;
    input_dev_on_mouse_rel(dx, dy, buttons);
}

/* Конечный автомат обработки команд управления от Nano-X */
static void mouse_process_cmd(uint8_t cmd) {
    /* Ожидание значения аргумента (Sample Rate) */
    if (mouse_cmd_state == 1) {
        mouse_cmd_state = 0;

        if (cmd == 200 && mouse_sample_magic == 0) {
            mouse_sample_magic = 1;
            return;
        } else if (cmd == 100 && mouse_sample_magic == 1) {
            mouse_sample_magic = 2;
            return;
        } else if (cmd == 80 && mouse_sample_magic == 2) {
            /* Цепочка 200 -> 100 -> 80 завершена! */
            mouse_sample_magic = 0;
            mouse_imps2 = true;
            mouse_enabled = true;

            move_qh = move_qt = 0;         /* Очищаем старые 3-байтовые пакеты */
            cmd_resp_qh = cmd_resp_qt = 0; /* Очищаем старые ACK */

            /* Эмулятор /dev/input/mice в Linux возвращает ровно 0xF4 для Nano-X */
            cmd_resp_push(0xF4);
            return;
        } else {
            mouse_sample_magic = 0;
            cmd_resp_push(0xFA);
            return;
        }
    }

    /* Ожидание значения аргумента (Resolution) */
    if (mouse_cmd_state == 2) {
        mouse_cmd_state = 0;
        cmd_resp_push(0xFA);
        return;
    }

    /* Команды верхнего уровня */
    switch (cmd) {
    case 0xFF: /* Reset */
        mouse_imps2 = false;
        mouse_sample_magic = 0;
        mouse_enabled = true;
        mouse_cmd_state = 0;
        move_qh = move_qt = 0;
        cmd_resp_qh = cmd_resp_qt = 0;
        cmd_resp_push(0xFA);
        cmd_resp_push(0xAA);
        cmd_resp_push(0x00);
        break;

    case 0xF3: /* Set Sample Rate */
        mouse_cmd_state = 1;
        /* Не шлем ACK сразу, ждем число (200/100/80) */
        break;

    case 0xF2: /* Get Device ID */
        cmd_resp_qh = cmd_resp_qt = 0;
        cmd_resp_push(0xFA);
        cmd_resp_push(mouse_imps2 ? 0x03 : 0x00);
        break;

    case 0xE8: /* Set Resolution */
        mouse_cmd_state = 2;
        cmd_resp_push(0xFA);
        break;

    case 0xF4: /* Enable Data Reporting */
        mouse_enabled = true;
        move_qh = move_qt = 0;
        cmd_resp_push(0xFA);
        break;

    case 0xF5: /* Disable Data Reporting */
        mouse_enabled = true;
        cmd_resp_push(0xFA);
        break;

    case 0xF6: /* Set Defaults */
        mouse_imps2 = false;
        mouse_sample_magic = 0;
        mouse_enabled = true;
        cmd_resp_push(0xFA);
        break;

    default:
        cmd_resp_push(0xFA);
        break;
    }
}

static int mouse_dev_read(void *buf, uint64_t count, uint32_t flags) {
    if (!buf || count == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    uint64_t got = 0;

    /* 1. Если есть ответы на команды — отдаем их первыми */
    while (got < count && cmd_resp_qh != cmd_resp_qt) {
        out[got++] = cmd_resp_q[cmd_resp_qt];
        cmd_resp_qt = (cmd_resp_qt + 1) % CMD_RESP_Q_SIZE;
    }
    if (got > 0) return (int)got;

    uint32_t pkt_len = mouse_imps2 ? 4 : 3;

    /* 2. Ждем, пока появится хотя бы один полный пакет */
    while (1) {
        uint32_t move_avail = (move_qh >= move_qt) ? (move_qh - move_qt) : (MOVE_Q_SIZE - move_qt + move_qh);

        if (move_avail >= pkt_len) {
            while (got + pkt_len <= count) {
                move_avail = (move_qh >= move_qt) ? (move_qh - move_qt) : (MOVE_Q_SIZE - move_qt + move_qh);
                if (move_avail < pkt_len) break;

                for (uint32_t i = 0; i < pkt_len; i++) {
                    out[got++] = move_q[move_qt];
                    move_qt = (move_qt + 1) % MOVE_Q_SIZE;
                }
            }
            return (int)got;
        }

        /* Если пакета нет и режим NONBLOCK — возвращаем EAGAIN (НЕ 0!) */
        if (flags & VFS_O_NONBLOCK) {
            return -EAGAIN;
        }

        /* В блокирующем режиме ждем поступления данных */
        schedule_yield();
    }
}

static int mouse_read(vfs_file_t *f, void *buf, uint64_t n) {

    int ret = mouse_dev_read(buf, n, f ? f->flags : 0);

    return ret;
}

static int mouse_write(vfs_file_t *f, const void *buf, uint64_t n) {
    (void)f;
    if (!buf || n == 0) return 0;

    const uint8_t *p = (const uint8_t *)buf;

    for (uint64_t i = 0; i < n; i++) {
        mouse_process_cmd(p[i]);
    }
    return (int)n;
}

uint32_t input_poll_events(vfs_file_t *file, uint32_t events) {
    if (!file || !file->node) return 0;

    uint32_t re = 0;
    const char *name = file->node->name ? file->node->name : "";

    bool is_mouse =
        (strstr(name, "mouse")  != NULL) ||
        (strstr(name, "mice")   != NULL) ||
        (strstr(name, "psaux")  != NULL) ||
        (strstr(name, "event1") != NULL);

    if (events & POLLIN) {
        if (is_mouse) {
            uint32_t pkt_len = mouse_imps2 ? 4 : 3;
            uint32_t move_avail = (move_qh >= move_qt) ? (move_qh - move_qt) : (MOVE_Q_SIZE - move_qt + move_qh);
            if (cmd_resp_qh != cmd_resp_qt || move_avail >= pkt_len)
                re |= POLLIN;
        } else {
            if (kbd_qh != kbd_qt)
                re |= POLLIN;
        }
    }
    if (events & POLLOUT)
        re |= POLLOUT;

    return re;
}

void update_keyboard_leds(void) {
    bool num    = g_keyboard.num_lock;
    bool caps   = g_keyboard.caps_lock;
    bool scroll = g_keyboard.scroll_lock;

    uint8_t ps2_leds = 0;
    if (scroll) ps2_leds |= (1u << 0);
    if (num)    ps2_leds |= (1u << 1);
    if (caps)   ps2_leds |= (1u << 2);

    uint8_t usb_leds = 0;
    if (num)    usb_leds |= (1u << 0);
    if (caps)   usb_leds |= (1u << 1);
    if (scroll) usb_leds |= (1u << 2);

    ps2kbd_set_leds(ps2_leds);
    xhci_set_keyboard_leds(usb_leds);
}

char key_to_ascii(int key, bool shift, bool caps_lock, bool num_lock) {
    if (key < 0 || key >= KEY_MAX) return 0;

    if (key >= KEY_KP_7 && key <= KEY_KP_DOT) {
        bool use_num = num_lock ^ shift;
        if (!use_num) return 0;
    }

    if (keymap_caps[key]) {
        bool is_upper = shift ^ caps_lock;
        return is_upper ? keymap_caps[key] : keymap_normal[key];
    }

    if (shift && keymap_shift[key]) {
        return keymap_shift[key];
    }

    return keymap_normal[key];
}

static void q_kbd_push(uint8_t b) {
    uint32_t n = (kbd_qh + 1) % KBD_Q_SIZE;
    if (n == kbd_qt) return;
    kbd_q[kbd_qh] = b;
    kbd_qh = n;
}

void input_dev_on_key(uint64_t keycode, bool down) {
    if (keycode >= KEY_MAX) return;
    uint8_t b = (uint8_t)(keycode & 0x7F);
    if (!down) b |= 0x80;
    q_kbd_push(b);
}

static int kbd_dev_read(void *buf, uint64_t count, uint32_t flags) {
    if (!buf || count == 0) return 0;

    if (kbd_qh == kbd_qt) {
        if (flags & VFS_O_NONBLOCK) return -EAGAIN;
        while (kbd_qh == kbd_qt) schedule_yield();
    }

    uint8_t *out = (uint8_t *)buf;
    uint64_t got = 0;
    while (got < count && kbd_qh != kbd_qt) {
        out[got++] = kbd_q[kbd_qt];
        kbd_qt = (kbd_qt + 1) % KBD_Q_SIZE;
    }
    return (int)got;
}

static int kbd_read(vfs_file_t *f, void *buf, uint64_t n) {
    return kbd_dev_read(buf, n, f ? f->flags : 0);
}

static int kbd_write(vfs_file_t *f, const void *b, uint64_t n) {
    (void)f; (void)b;
    return (int)n;
}

static vfs_file_ops_t mouse_ops = {
    .read  = mouse_read,
    .write = mouse_write,
    .poll  = input_poll_events
};

static vfs_file_ops_t kbd_ops = {
    .read  = kbd_read,
    .write = kbd_write,
    .poll  = input_poll_events
};

void input_register_devfs(void) {
    move_qh = move_qt = 0;
    cmd_resp_qh = cmd_resp_qt = 0;
    mouse_cmd_state = 0;
    mouse_sample_magic = 0;
    mouse_imps2 = false;
    mouse_enabled = true;
    kbd_qh = kbd_qt = 0;

    devfs_register_device("mouse",        VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("input/mice",   VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("psaux",        VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("input/event1", VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);

    devfs_register_device("kbd",          VFS_TYPE_CHAR_DEV, &kbd_ops, NULL);
    devfs_register_device("input/event0", VFS_TYPE_CHAR_DEV, &kbd_ops, NULL);
}

void inout_keyboard_task(void) {
    while (1) {
        while (1) {
            inout_event_t ev;
            bool got_event = false;

            uint64_t flags = irq_save();
            if (kbd_tail != kbd_head) {
                ev = kbd_events[kbd_tail];
                got_event = true;
            } else {
                new_kbd_event = false;
                task_sleep(inout_kbd_proc);
            }
            irq_restore(flags);

            if (!got_event) break;

            inout_data_key_t *data = &ev.data.key;
            if (data->keycode != 0) {
                if (data->key_state == INOUT_KEY_DOWN) {
                    g_keyboard.down[data->keycode] = true;

                    if (data->keycode == KEY_CAPSLOCK) {
                        g_keyboard.caps_lock = !g_keyboard.caps_lock;
                        update_keyboard_leds();
                    } else if (data->keycode == KEY_NUMLOCK) {
                        g_keyboard.num_lock = !g_keyboard.num_lock;
                        update_keyboard_leds();
                    } else if (data->keycode == KEY_SCROLLLOCK) {
                        g_keyboard.scroll_lock = !g_keyboard.scroll_lock;
                        update_keyboard_leds();
                    } else {
                        g_keyboard.shift_pressed =
                            g_keyboard.down[KEY_LSHIFT] || g_keyboard.down[KEY_RSHIFT];
                    }
                    input_dev_on_key(data->keycode, true);
                } else if (data->key_state == INOUT_KEY_UP) {
                    g_keyboard.down[data->keycode] = false;
                    g_keyboard.shift_pressed =
                        g_keyboard.down[KEY_LSHIFT] || g_keyboard.down[KEY_RSHIFT];
                    input_dev_on_key(data->keycode, false);
                }
            }

            flags = irq_save();
            kbd_tail = (kbd_tail + 1) % EVENT_QUEUE_SIZE;
            irq_restore(flags);
        }
        schedule_yield();
    }
}

void inout_mouse_task(void) {
    while (1) {
        while (1) {
            inout_event_t ev;
            bool got_event = false;

            uint64_t flags = irq_save();
            if (mouse_tail != mouse_head) {
                ev = mouse_events[mouse_tail];
                got_event = true;
            } else {
                new_mouse_event = false;
                task_sleep(inout_mouse_proc);
            }
            irq_restore(flags);

            if (!got_event) break;

            inout_data_mouse_t *data = &ev.data.mouse;

            if (data->mouse_state == INOUT_MOUSE_DOWN ||
                data->mouse_state == INOUT_MOUSE_UP) {
                bool pressed = (data->mouse_state == INOUT_MOUSE_DOWN);
                if (data->mouse_btn == INOUT_MOUSE_LEFTBTN)
                    g_mouse.leftbtn_pressed = pressed;
                else if (data->mouse_btn == INOUT_MOUSE_RIGHTBTN)
                    g_mouse.rightbtn_pressed = pressed;
                else if (data->mouse_btn == INOUT_MOUSE_CENTERBTN)
                    g_mouse.centerbtn_pressed = pressed;

                uint8_t btn = 0;
                if (g_mouse.leftbtn_pressed)   btn |= 1;
                if (g_mouse.rightbtn_pressed)  btn |= 2;
                if (g_mouse.centerbtn_pressed) btn |= 4;
                input_dev_on_mouse_rel(0, 0, btn);
            }

            if (data->mouse_state == INOUT_MOUSE_MOVE) {
                g_mouse.x += data->x;
                g_mouse.y += data->y;
                g_mouse.z += data->z;

                uint8_t btn = 0;
                if (g_mouse.leftbtn_pressed)   btn |= 1;
                if (g_mouse.rightbtn_pressed)  btn |= 2;
                if (g_mouse.centerbtn_pressed) btn |= 4;
                input_dev_on_mouse_rel(data->x, data->y, btn);
            }

            if (data->mouse_state == INOUT_MOUSE_MOVE_ABSOLUTE) {
                g_mouse.x = data->x;
                g_mouse.y = data->y;

                uint8_t btn = 0;
                if (g_mouse.leftbtn_pressed)   btn |= 1;
                if (g_mouse.rightbtn_pressed)  btn |= 2;
                if (g_mouse.centerbtn_pressed) btn |= 4;
                input_dev_on_mouse_abs(data->x, data->y, btn);
            }

            flags = irq_save();
            mouse_tail = (mouse_tail + 1) % EVENT_QUEUE_SIZE;
            irq_restore(flags);
        }
        schedule_yield();
    }
}

void inout_call_scheduler(void) {
    if (new_kbd_event && inout_kbd_proc &&
        inout_kbd_proc->state == TASK_SLEEPING) {
        task_wake(inout_kbd_proc);
    }
    if (new_mouse_event && inout_mouse_proc &&
        inout_mouse_proc->state == TASK_SLEEPING) {
        task_wake(inout_mouse_proc);
    }
}

void inout_add_event(inout_event_t ev) {
    uint64_t flags = irq_save();

    if (ev.type == INOUT_EVENT_KEY) {
        uint32_t next = (kbd_head + 1) % EVENT_QUEUE_SIZE;
        if (next != kbd_tail) {
            kbd_events[kbd_head] = ev;
            kbd_head = next;
            new_kbd_event = true;
        }
    } else if (ev.type == INOUT_EVENT_MOUSE) {
        inout_data_mouse_t *data = &ev.data.mouse;

        if (data->mouse_state == INOUT_MOUSE_DOWN ||
            data->mouse_state == INOUT_MOUSE_UP) {
            bool pressed = (data->mouse_state == INOUT_MOUSE_DOWN);
            if (data->mouse_btn == INOUT_MOUSE_LEFTBTN)
                g_mouse.leftbtn_pressed = pressed;
            else if (data->mouse_btn == INOUT_MOUSE_RIGHTBTN)
                g_mouse.rightbtn_pressed = pressed;
            else if (data->mouse_btn == INOUT_MOUSE_CENTERBTN)
                g_mouse.centerbtn_pressed = pressed;

            uint8_t btn = 0;
            if (g_mouse.leftbtn_pressed)   btn |= 1;
            if (g_mouse.rightbtn_pressed)  btn |= 2;
            if (g_mouse.centerbtn_pressed) btn |= 4;

            input_dev_on_mouse_rel(0, 0, btn);
        }
        else if (data->mouse_state == INOUT_MOUSE_MOVE) {
            g_mouse.x += data->x;
            g_mouse.y += data->y;
            g_mouse.z += data->z;

            uint8_t btn = 0;
            if (g_mouse.leftbtn_pressed)   btn |= 1;
            if (g_mouse.rightbtn_pressed)  btn |= 2;
            if (g_mouse.centerbtn_pressed) btn |= 4;

            input_dev_on_mouse_rel(data->x, data->y, btn);
        }
        else if (data->mouse_state == INOUT_MOUSE_MOVE_ABSOLUTE) {
            uint8_t btn = 0;
            if (g_mouse.leftbtn_pressed)   btn |= 1;
            if (g_mouse.rightbtn_pressed)  btn |= 2;
            if (g_mouse.centerbtn_pressed) btn |= 4;

            input_dev_on_mouse_abs(data->x, data->y, btn);
        }
    }

    irq_restore(flags);

    if (ev.type == INOUT_EVENT_KEY && inout_kbd_proc && inout_kbd_proc->state == TASK_SLEEPING) {
        task_wake(inout_kbd_proc);
    }
}

void inout_init(void) {
    kbd_head = kbd_tail = 0;
    mouse_head = mouse_tail = 0;
    move_qh = move_qt = 0;
    cmd_resp_qh = cmd_resp_qt = 0;
    mouse_cmd_state = 0;
    mouse_sample_magic = 0;
    mouse_imps2 = false;
    mouse_enabled = true;
    kbd_qh = kbd_qt = 0;

    inout_kbd_proc = create_task_kernel((void *)inout_keyboard_task, "inout_kbd");
    inout_mouse_proc = create_task_kernel((void *)inout_mouse_task, "inout_mouse");

    if (!inout_kbd_proc)
        console_print_string("[INOUT] WARN: failed to create kbd task\n");
    if (!inout_mouse_proc)
        console_print_string("[INOUT] WARN: failed to create mouse task\n");

    devfs_register_device("mouse",        VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("input/mice",   VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("psaux",        VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("input/event1", VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);

    devfs_register_device("kbd",          VFS_TYPE_CHAR_DEV, &kbd_ops, NULL);
    devfs_register_device("input/event0", VFS_TYPE_CHAR_DEV, &kbd_ops, NULL);
}