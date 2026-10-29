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
#include <dev/evdev.h>
#include <scheduler/scheduler.h>
#include <scheduler/spinlock.h>
#include <mm/malloc.h>
#include <fs/devfs.h>
#include <fs/vfs.h>

void xhci_set_keyboard_leds(uint8_t leds);

#ifndef POLLIN
#define POLLIN  0x0001
#define POLLOUT 0x0004
#endif

#define MOVE_Q_SIZE     256
#define KBD_Q_SIZE      512

process_t *inout_kbd_proc   = NULL;
process_t *inout_mouse_proc = NULL;

volatile keyboard_state_t g_keyboard;
volatile mouse_state_t    g_mouse = { .x = 0, .y = 0, .z = 0 };

volatile bool new_kbd_event   = false;
volatile bool new_mouse_event = false;

typedef struct {
    int8_t  dx;
    int8_t  dy;
    int8_t  dz;
    uint8_t buttons;
} mouse_motion_t;

static mouse_motion_t move_q[MOVE_Q_SIZE];
static volatile uint32_t move_qh = 0, move_qt = 0;
static spinlock_t input_q_lock = SPINLOCK_INIT;

#define MOUSE_MODE_PS2   0
#define MOUSE_MODE_IMPS  1
#define MOUSE_MODE_EXPS  2

typedef struct {
    uint8_t resp[8];
    uint8_t resp_len;
    uint8_t resp_pos;
    uint8_t pkt[6];
    uint8_t pkt_len;
    uint8_t pkt_pos;
    uint8_t mode;
    uint8_t imps_seq;
    uint8_t imex_seq;
    uint8_t reporting;
} mouse_client_t;

static mouse_client_t default_mouse_client;

typedef struct {
    uint8_t code;
    uint8_t down;
} kbd_event_rec_t;

static kbd_event_rec_t kbd_evt_q[KBD_Q_SIZE];
static volatile uint32_t kbd_evt_h = 0;

enum {
    KBD_RESYNC_NONE = 0,
    KBD_RESYNC_MOD_UP,
    KBD_RESYNC_MOD_DOWN,
    KBD_RESYNC_DIFF
};

typedef struct {
    volatile uint32_t tail;
    bool     sent_down[KEY_MAX];
    volatile bool sync_needed;
    uint8_t  sync_phase;
    uint32_t sync_cursor;
} kbd_consumer_t;

static const uint8_t kbd_resync_mods[] = {
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL,
    KEY_LALT,   KEY_RALT,   KEY_LGUI,  KEY_RGUI
};
#define KBD_RESYNC_MOD_COUNT ((uint32_t)(sizeof(kbd_resync_mods) / sizeof(kbd_resync_mods[0])))

static kbd_consumer_t dev_consumer;
static kbd_consumer_t con_consumer;

static uint8_t  kbd_read_buf[2];
static uint32_t kbd_read_pos = 0, kbd_read_len = 0;

static spinlock_t kbd_state_lock = SPINLOCK_INIT;
static volatile bool kbd_leds_dirty = false;

static spinlock_t mouse_speed_lock = SPINLOCK_INIT;
static int mouse_speed_percent = 100;
static int64_t mouse_frac_x = 0, mouse_frac_y = 0, mouse_frac_z = 0;

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static inline uint32_t move_q_count(void) {
    return (move_qh >= move_qt) ? (move_qh - move_qt)
                                : (MOVE_Q_SIZE - move_qt + move_qh);
}

static int motion_push(int dx, int dy, int dz, uint8_t buttons) {
    if (dx >  127) dx =  127; else if (dx < -127) dx = -127;
    if (dy >  127) dy =  127; else if (dy < -127) dy = -127;
    if (dz >  127) dz =  127; else if (dz < -127) dz = -127;

    uint64_t flags;
    spin_lock_irqsave(&input_q_lock, &flags);
    uint32_t next = (move_qh + 1) % MOVE_Q_SIZE;
    if (next == move_qt) move_qt = (move_qt + 1) % MOVE_Q_SIZE;
    move_q[move_qh].dx = (int8_t)dx;
    move_q[move_qh].dy = (int8_t)dy;
    move_q[move_qh].dz = (int8_t)dz;
    move_q[move_qh].buttons = buttons & 0x07;
    move_qh = (move_qh + 1) % MOVE_Q_SIZE;
    spin_unlock_irqrestore(&input_q_lock, flags);
    return 0;
}

static bool motion_pop_locked(mouse_motion_t *out) {
    if (move_qh == move_qt) return false;
    *out = move_q[move_qt];
    move_qt = (move_qt + 1) % MOVE_Q_SIZE;
    return true;
}

static bool motion_pop(mouse_motion_t *out) {
    uint64_t flags;
    spin_lock_irqsave(&input_q_lock, &flags);
    bool ok = motion_pop_locked(out);
    spin_unlock_irqrestore(&input_q_lock, flags);
    return ok;
}

static void input_stream_reset(void) {
    uint64_t flags;
    spin_lock_irqsave(&input_q_lock, &flags);
    move_qh = move_qt = 0;
    spin_unlock_irqrestore(&input_q_lock, flags);

    spin_lock_irqsave(&kbd_state_lock, &flags);
    kbd_evt_h = 0;
    kbd_read_pos = kbd_read_len = 0;
    memset(&dev_consumer, 0, sizeof(dev_consumer));
    memset(&con_consumer, 0, sizeof(con_consumer));
    for (int k = 0; k < KEY_MAX; k++) g_keyboard.down[k] = false;
    g_keyboard.caps_lock = false;
    g_keyboard.num_lock = false;
    g_keyboard.scroll_lock = false;
    g_keyboard.shift_pressed = false;
    spin_unlock_irqrestore(&kbd_state_lock, flags);
}

static void mouse_scale(int *dx, int *dy, int *dz) {
    uint64_t flags;
    spin_lock_irqsave(&mouse_speed_lock, &flags);
    int pct = mouse_speed_percent;

    int64_t t = (int64_t)*dx * pct + mouse_frac_x;
    int rx = (int)(t / 100);
    mouse_frac_x = t - (int64_t)rx * 100;

    t = (int64_t)*dy * pct + mouse_frac_y;
    int ry = (int)(t / 100);
    mouse_frac_y = t - (int64_t)ry * 100;

    t = (int64_t)*dz * pct + mouse_frac_z;
    int rz = (int)(t / 100);
    mouse_frac_z = t - (int64_t)rz * 100;

    spin_unlock_irqrestore(&mouse_speed_lock, flags);

    *dx = rx;
    *dy = ry;
    *dz = rz;
}

void inout_set_mouse_speed(int percent) {
    if (percent < 10) percent = 10;
    if (percent > 500) percent = 500;
    uint64_t flags;
    spin_lock_irqsave(&mouse_speed_lock, &flags);
    mouse_speed_percent = percent;
    mouse_frac_x = mouse_frac_y = mouse_frac_z = 0;
    spin_unlock_irqrestore(&mouse_speed_lock, flags);
}

int inout_get_mouse_speed(void) {
    uint64_t flags;
    spin_lock_irqsave(&mouse_speed_lock, &flags);
    int v = mouse_speed_percent;
    spin_unlock_irqrestore(&mouse_speed_lock, flags);
    return v;
}

static void mouse_encode_packet(mouse_client_t *c, const mouse_motion_t *m) {
    int8_t sdx = m->dx;
    int8_t sdy = (int8_t)(-m->dy);
    int8_t sdw = m->dz;

    uint8_t b0 = 0x08 | (m->buttons & 0x07);
    if (sdx < 0) b0 |= 0x10;
    if (sdy < 0) b0 |= 0x20;

    c->pkt[0] = b0;
    c->pkt[1] = (uint8_t)sdx;
    c->pkt[2] = (uint8_t)sdy;

    if (c->mode == MOUSE_MODE_EXPS) {
        int w = sdw;
        if (w > 7) w = 7;
        if (w < -8) w = -8;
        c->pkt[3] = (uint8_t)(w & 0x0F);
        c->pkt_len = 4;
    } else if (c->mode == MOUSE_MODE_IMPS) {
        c->pkt[3] = (uint8_t)sdw;
        c->pkt_len = 4;
    } else {
        c->pkt_len = 3;
    }
    c->pkt_pos = 0;
}

static void mouse_response(mouse_client_t *c, uint8_t cmd) {
    static const uint8_t imps_seq[6] = { 0xF3, 200, 0xF3, 100, 0xF3, 80 };
    static const uint8_t imex_seq[6] = { 0xF3, 200, 0xF3, 200, 0xF3, 80 };

    if (cmd == imps_seq[c->imps_seq]) {
        if (++c->imps_seq == 6) {
            c->imps_seq = 0;
            c->mode = MOUSE_MODE_IMPS;
        }
    } else {
        c->imps_seq = (cmd == 0xF3) ? 1 : 0;
    }
    if (cmd == imex_seq[c->imex_seq]) {
        if (++c->imex_seq == 6) {
            c->imex_seq = 0;
            c->mode = MOUSE_MODE_EXPS;
        }
    } else {
        c->imex_seq = (cmd == 0xF3) ? 1 : 0;
    }

    c->resp[0] = 0xFA;
    c->resp_len = 1;
    c->resp_pos = 0;

    switch (cmd) {
    case 0xFF:
        c->imps_seq = 0;
        c->imex_seq = 0;
        c->mode = MOUSE_MODE_PS2;
        c->reporting = 0;
        c->resp[1] = 0xAA;
        c->resp[2] = 0x00;
        c->resp_len = 3;
        break;
    case 0xF2:
        c->resp[1] = (c->mode == MOUSE_MODE_PS2) ? 0x00 :
                     (c->mode == MOUSE_MODE_IMPS) ? 0x03 : 0x04;
        c->resp_len = 2;
        break;
    case 0xE9:
        c->resp[1] = 0x60;
        c->resp[2] = 0x03;
        c->resp[3] = 200;
        c->resp_len = 4;
        break;
    case 0xEB: {
        mouse_motion_t m = {0};
        if (!motion_pop_locked(&m)) {
            c->pkt[0] = 0x08;
            c->pkt[1] = 0;
            c->pkt[2] = 0;
            c->pkt[3] = 0;
            c->pkt_len = (c->mode == MOUSE_MODE_PS2) ? 3 : 4;
            c->pkt_pos = 0;
        } else {
            mouse_encode_packet(c, &m);
        }
        for (uint8_t i = 0; i < c->pkt_len; i++)
            c->resp[1 + i] = c->pkt[i];
        c->resp_len = (uint8_t)(1 + c->pkt_len);
        c->pkt_len = 0;
        c->pkt_pos = 0;
        break;
    }
    case 0xF4:
        c->reporting = 1;
        break;
    case 0xF5:
    case 0xF6:
        c->reporting = 0;
        if (cmd == 0xF6) c->mode = MOUSE_MODE_PS2;
        break;
    default:
        break;
    }
}

static mouse_client_t *mouse_client_of(vfs_file_t *f) {
    if (f && f->private_data) return (mouse_client_t *)f->private_data;
    return &default_mouse_client;
}

static int mouse_dev_read(vfs_file_t *f, void *buf, uint64_t count, uint32_t flags) {
    if (!buf || count == 0) return 0;
    mouse_client_t *c = mouse_client_of(f);
    uint8_t *out = (uint8_t *)buf;
    uint64_t got = 0;

    for (;;) {
        uint64_t lf;
        spin_lock_irqsave(&input_q_lock, &lf);

        while (got < count && c->resp_pos < c->resp_len)
            out[got++] = c->resp[c->resp_pos++];
        if (c->resp_pos >= c->resp_len) c->resp_len = c->resp_pos = 0;

        if (got == 0) {
            while (got < count && c->pkt_pos < c->pkt_len)
                out[got++] = c->pkt[c->pkt_pos++];
            if (c->pkt_pos >= c->pkt_len) c->pkt_len = c->pkt_pos = 0;
        }

        spin_unlock_irqrestore(&input_q_lock, lf);

        if (got > 0) return (int)got;

        mouse_motion_t m;
        if (move_q_count() > 0 && motion_pop(&m)) {
            spin_lock_irqsave(&input_q_lock, &lf);
            mouse_encode_packet(c, &m);
            while (got < count && c->pkt_pos < c->pkt_len)
                out[got++] = c->pkt[c->pkt_pos++];
            if (c->pkt_pos >= c->pkt_len) c->pkt_len = c->pkt_pos = 0;
            spin_unlock_irqrestore(&input_q_lock, lf);
            if (got > 0) return (int)got;
        }

        if (flags & VFS_O_NONBLOCK) return -EAGAIN;
        schedule_yield();
    }
}

static int mouse_read(vfs_file_t *f, void *buf, uint64_t n) {
    return mouse_dev_read(f, buf, n, f ? f->flags : 0);
}

static int mouse_write(vfs_file_t *f, const void *buf, uint64_t n) {
    (void)f;
    if (!buf || n == 0) return 0;
    mouse_client_t *c = mouse_client_of(f);
    const uint8_t *p = (const uint8_t *)buf;

    uint64_t lf;
    spin_lock_irqsave(&input_q_lock, &lf);
    for (uint64_t i = 0; i < n; i++) mouse_response(c, p[i]);
    spin_unlock_irqrestore(&input_q_lock, lf);
    return (int)n;
}

static int mouse_open(vfs_file_t *f) {
    mouse_client_t *c = (mouse_client_t *)malloc_alloc(sizeof(mouse_client_t));
    if (!c) return -ENOMEM;
    memset(c, 0, sizeof(*c));
    c->reporting = 1;
    f->private_data = c;
    return 0;
}

static int mouse_close(vfs_file_t *f) {
    if (f && f->private_data && f->private_data != &default_mouse_client) {
        malloc_free(f->private_data);
        f->private_data = NULL;
    }
    return 0;
}

static int mouse_ioctl(vfs_file_t *f, uint64_t request, void *arg) {
    (void)f;
    if (request == MOUSE_IOC_SETSPEED) {
        inout_set_mouse_speed((int)(int64_t)(uint64_t)arg);
        return 0;
    }
    if (request == MOUSE_IOC_GETSPEED) {
        if (!arg) return -EFAULT;
        *(int *)arg = inout_get_mouse_speed();
        return 0;
    }
    return -ENOTTY;
}

static int mousectl_read(vfs_file_t *f, void *buf, uint64_t count) {
    if (!buf || count == 0) return 0;
    char text[32];
    int v = inout_get_mouse_speed();
    int len = 0;
    if (v >= 100) { text[len++] = (char)('0' + v / 100); }
    if (v >= 10)  { text[len++] = (char)('0' + (v / 10) % 10); }
    text[len++] = (char)('0' + v % 10);
    text[len++] = '\n';

    if ((uint64_t)f->offset >= (uint64_t)len) return 0;
    uint64_t avail = (uint64_t)len - f->offset;
    if (count > avail) count = avail;
    memcpy(buf, text + f->offset, count);
    f->offset += count;
    return (int)count;
}

static int mousectl_write(vfs_file_t *f, const void *buf, uint64_t count) {
    (void)f;
    if (!buf || count == 0) return 0;
    const char *p = (const char *)buf;
    int v = 0;
    bool any = false;
    for (uint64_t i = 0; i < count; i++) {
        if (p[i] < '0' || p[i] > '9') break;
        v = v * 10 + (p[i] - '0');
        if (v > 100000) v = 100000;
        any = true;
    }
    if (any) inout_set_mouse_speed(v);
    return (int)count;
}

static uint32_t mousectl_poll(vfs_file_t *f, uint32_t events) {
    uint32_t r = 0;
    if (events & POLLIN) r |= POLLIN;
    if (events & POLLOUT) r |= POLLOUT;
    (void)f;
    return r;
}

static vfs_file_ops_t mousectl_ops = {
    .read  = mousectl_read,
    .write = mousectl_write,
    .poll  = mousectl_poll
};

static void kbd_consumer_mark_resync(kbd_consumer_t *c, uint32_t new_tail) {
    c->tail = new_tail;
    c->sync_needed = true;
    c->sync_phase  = KBD_RESYNC_MOD_UP;
    c->sync_cursor = 0;
}

static void kbd_event_push_locked(uint8_t code, bool down) {
    uint32_t next = (kbd_evt_h + 1) % KBD_Q_SIZE;

    if (next == dev_consumer.tail)
        kbd_consumer_mark_resync(&dev_consumer, next);
    if (next == con_consumer.tail)
        kbd_consumer_mark_resync(&con_consumer, next);

    kbd_evt_q[kbd_evt_h].code = code;
    kbd_evt_q[kbd_evt_h].down = down ? 1 : 0;
    kbd_evt_h = next;
}

static void kbd_consumer_request_resync(kbd_consumer_t *c) {
    kbd_consumer_mark_resync(c, kbd_evt_h);
}

static bool kbd_consumer_resync_next_locked(kbd_consumer_t *c, kbd_event_rec_t *e) {
    if (!c->sync_needed) return false;

    for (;;) {
        if (c->sync_phase == KBD_RESYNC_MOD_UP) {
            if (c->sync_cursor < KBD_RESYNC_MOD_COUNT) {
                e->code = kbd_resync_mods[c->sync_cursor++];
                e->down = 0;
                return true;
            }
            c->sync_phase = KBD_RESYNC_MOD_DOWN;
            c->sync_cursor = 0;
            continue;
        }

        if (c->sync_phase == KBD_RESYNC_MOD_DOWN) {
            if (c->sync_cursor < KBD_RESYNC_MOD_COUNT) {
                uint8_t k = kbd_resync_mods[c->sync_cursor++];
                if (g_keyboard.down[k]) {
                    e->code = k;
                    e->down = 1;
                    return true;
                }
                continue;
            }
            c->sync_phase = KBD_RESYNC_DIFF;
            c->sync_cursor = 0;
            continue;
        }

        if (c->sync_phase == KBD_RESYNC_DIFF) {
            while (c->sync_cursor < KEY_MAX) {
                uint8_t k = (uint8_t)(c->sync_cursor++);
                bool want = g_keyboard.down[k] ? true : false;
                if (want != c->sent_down[k]) {
                    e->code = k;
                    e->down = want ? 1 : 0;
                    return true;
                }
            }
        }

        c->sync_needed = false;
        c->sync_phase  = KBD_RESYNC_NONE;
        c->sync_cursor = 0;
        return false;
    }
}

static void key_state_apply(uint64_t keycode, bool down) {
    if (keycode == 0 || keycode >= KEY_MAX) return;

    uint64_t flags;
    spin_lock_irqsave(&kbd_state_lock, &flags);

    bool was = g_keyboard.down[keycode] ? true : false;
    if (was == down) {
        g_keyboard.shift_pressed =
            g_keyboard.down[KEY_LSHIFT] || g_keyboard.down[KEY_RSHIFT];
        spin_unlock_irqrestore(&kbd_state_lock, flags);
        return;
    }

    g_keyboard.down[keycode] = down;
    g_keyboard.shift_pressed =
        g_keyboard.down[KEY_LSHIFT] || g_keyboard.down[KEY_RSHIFT];

    if (down) {
        if (keycode == KEY_CAPSLOCK) {
            g_keyboard.caps_lock = !g_keyboard.caps_lock;
            kbd_leds_dirty = true;
        } else if (keycode == KEY_NUMLOCK) {
            g_keyboard.num_lock = !g_keyboard.num_lock;
            kbd_leds_dirty = true;
        } else if (keycode == KEY_SCROLLLOCK) {
            g_keyboard.scroll_lock = !g_keyboard.scroll_lock;
            kbd_leds_dirty = true;
        }
    }

    kbd_event_push_locked((uint8_t)keycode, down);
    new_kbd_event = true;
    spin_unlock_irqrestore(&kbd_state_lock, flags);

    evdev_report_key((uint8_t)keycode, down);
}

void inout_set_key_state(uint64_t keycode, bool down) {
    key_state_apply(keycode, down);
}

static uint8_t kbd_ext_scancode(uint8_t code) {
    switch (code) {
    case KEY_RCTRL:       return 0x1D;
    case KEY_RALT:        return 0x38;
    case KEY_KP_ENTER:    return 0x1C;
    case KEY_KP_SLASH:    return 0x35;
    case KEY_HOME:        return 0x47;
    case KEY_UP:          return 0x48;
    case KEY_PGUP:        return 0x49;
    case KEY_LEFT:        return 0x4B;
    case KEY_RIGHT:       return 0x4D;
    case KEY_END:         return 0x4F;
    case KEY_DOWN:        return 0x50;
    case KEY_PGDN:        return 0x51;
    case KEY_INSERT:      return 0x52;
    case KEY_DELETE:      return 0x53;
    case KEY_LGUI:        return 0x5B;
    case KEY_RGUI:        return 0x5C;
    case KEY_MENU:        return 0x5D;
    case KEY_PRINTSCREEN: return 0x37;
    default:              return 0;
    }
}

static uint8_t kbd_event_to_bytes(const kbd_event_rec_t *e, uint8_t out[2]) {
    uint8_t sc = e->code;
    if (sc >= KEY_EXT_BASE) {
        uint8_t ext = kbd_ext_scancode(sc);
        if (!ext) return 0;
        out[0] = 0xE0;
        out[1] = (uint8_t)(ext | (e->down ? 0 : 0x80));
        return 2;
    }
    out[0] = (uint8_t)(sc | (e->down ? 0 : 0x80));
    return 1;
}

bool inout_console_pop_event(uint8_t *code, bool *down) {
    if (!code || !down) return false;
    uint64_t flags;
    spin_lock_irqsave(&kbd_state_lock, &flags);

    kbd_event_rec_t e;
    if (kbd_consumer_resync_next_locked(&con_consumer, &e)) {
        con_consumer.sent_down[e.code] = e.down != 0;
        *code = e.code;
        *down = e.down != 0;
        spin_unlock_irqrestore(&kbd_state_lock, flags);
        return true;
    }
    if (con_consumer.tail == kbd_evt_h) {
        spin_unlock_irqrestore(&kbd_state_lock, flags);
        return false;
    }
    e = kbd_evt_q[con_consumer.tail];
    con_consumer.tail = (con_consumer.tail + 1) % KBD_Q_SIZE;
    con_consumer.sent_down[e.code] = e.down != 0;
    *code = e.code;
    *down = e.down != 0;
    spin_unlock_irqrestore(&kbd_state_lock, flags);
    return true;
}

uint8_t inout_console_scancode_bytes(uint8_t code, bool down, uint8_t out[2]) {
    kbd_event_rec_t e = { .code = code, .down = down ? 1 : 0 };
    return kbd_event_to_bytes(&e, out);
}

void inout_keyboard_request_resync(void) {
    uint64_t flags;
    spin_lock_irqsave(&kbd_state_lock, &flags);
    kbd_consumer_request_resync(&dev_consumer);
    kbd_consumer_request_resync(&con_consumer);
    spin_unlock_irqrestore(&kbd_state_lock, flags);
}

static int kbd_open(vfs_file_t *file) {
    (void)file;
    uint64_t flags;
    spin_lock_irqsave(&kbd_state_lock, &flags);
    memset(dev_consumer.sent_down, 0, sizeof(dev_consumer.sent_down));
    kbd_consumer_request_resync(&dev_consumer);
    spin_unlock_irqrestore(&kbd_state_lock, flags);
    return 0;
}

static int kbd_dev_read(void *buf, uint64_t count, uint32_t flags) {
    if (!buf || count == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    uint64_t got = 0;

    for (;;) {
        uint64_t lf;
        spin_lock_irqsave(&kbd_state_lock, &lf);

        while (got < count && kbd_read_pos < kbd_read_len)
            out[got++] = kbd_read_buf[kbd_read_pos++];
        if (kbd_read_pos >= kbd_read_len)
            kbd_read_pos = kbd_read_len = 0;

        while (got < count) {
            kbd_event_rec_t e;
            bool from_resync = kbd_consumer_resync_next_locked(&dev_consumer, &e);
            if (!from_resync) {
                if (dev_consumer.tail == kbd_evt_h) break;
                e = kbd_evt_q[dev_consumer.tail];
                dev_consumer.tail = (dev_consumer.tail + 1) % KBD_Q_SIZE;
            }
            dev_consumer.sent_down[e.code] = e.down != 0;

            uint8_t n = kbd_event_to_bytes(&e, kbd_read_buf);
            if (n == 0) continue;
            kbd_read_len = n;
            kbd_read_pos = 0;

            while (got < count && kbd_read_pos < kbd_read_len)
                out[got++] = kbd_read_buf[kbd_read_pos++];
            if (kbd_read_pos >= kbd_read_len)
                kbd_read_pos = kbd_read_len = 0;
            else
                break;
        }

        spin_unlock_irqrestore(&kbd_state_lock, lf);

        if (got > 0) return (int)got;
        if (flags & VFS_O_NONBLOCK) return -EAGAIN;
        schedule_yield();
    }
}

static int kbd_read(vfs_file_t *f, void *buf, uint64_t n) {
    return kbd_dev_read(buf, n, f ? f->flags : 0);
}

static int kbd_write(vfs_file_t *f, const void *b, uint64_t n) {
    (void)f; (void)b;
    return (int)n;
}

static int kbd_ioctl(vfs_file_t *f, uint64_t request, void *arg) {
    (void)f;
    if (request == MOUSE_IOC_GETSPEED) {
        if (!arg) return -EFAULT;
        *(int *)arg = inout_get_mouse_speed();
        return 0;
    }
    return -ENOTTY;
}

uint32_t input_poll_events(vfs_file_t *file, uint32_t events) {
    if (!file || !file->node) return 0;

    uint32_t re = 0;
    const char *name = file->node->name;

    bool is_mouse =
        (strstr(name, "mouse")  != NULL) ||
        (strstr(name, "mice")   != NULL) ||
        (strstr(name, "psaux")  != NULL);

    if (events & POLLIN) {
        if (is_mouse) {
            mouse_client_t *c = mouse_client_of(file);
            uint64_t lf;
            spin_lock_irqsave(&input_q_lock, &lf);
            bool ready = (c->resp_pos < c->resp_len) ||
                         (c->pkt_pos < c->pkt_len) ||
                         (move_q_count() > 0);
            spin_unlock_irqrestore(&input_q_lock, lf);
            if (ready) re |= POLLIN;
        } else {
            uint64_t lf;
            spin_lock_irqsave(&kbd_state_lock, &lf);
            bool ready = (dev_consumer.tail != kbd_evt_h) ||
                         dev_consumer.sync_needed ||
                         (kbd_read_pos < kbd_read_len);
            spin_unlock_irqrestore(&kbd_state_lock, lf);
            if (ready) re |= POLLIN;
        }
    }
    if (events & POLLOUT)
        re |= POLLOUT;

    return re;
}

static vfs_file_ops_t mouse_ops = {
    .read  = mouse_read,
    .write = mouse_write,
    .open  = mouse_open,
    .close = mouse_close,
    .ioctl = mouse_ioctl,
    .poll  = input_poll_events
};

static vfs_file_ops_t kbd_ops = {
    .read  = kbd_read,
    .write = kbd_write,
    .open  = kbd_open,
    .ioctl = kbd_ioctl,
    .poll  = input_poll_events
};

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
    extern void libpayload_usb_set_leds(uint8_t leds);
    libpayload_usb_set_leds(usb_leds);
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

void input_register_devfs(void) {
    input_stream_reset();

    devfs_register_device("mouse",             VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("input/mice",        VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("psaux",             VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("input/mouse0",      VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("kbd",               VFS_TYPE_CHAR_DEV, &kbd_ops, NULL);
    devfs_register_device("input/mousectl",    VFS_TYPE_CHAR_DEV, &mousectl_ops, NULL);
}

void inout_keyboard_task(void) {
    for (;;) {
        if (kbd_leds_dirty) {
            kbd_leds_dirty = false;
            update_keyboard_leds();
        }
        schedule_yield();
    }
}

void inout_mouse_task(void) {
    for (;;) schedule_yield();
}

void inout_call_scheduler(void) {
    (void)new_kbd_event;
    (void)new_mouse_event;
}

static inline uint8_t mouse_button_mask(void) {
    uint8_t btn = 0;
    if (g_mouse.leftbtn_pressed)   btn |= 1;
    if (g_mouse.rightbtn_pressed)  btn |= 2;
    if (g_mouse.centerbtn_pressed) btn |= 4;
    return btn;
}

void inout_add_event(inout_event_t ev) {
    if (ev.type == INOUT_EVENT_KEY) {
        key_state_apply(ev.data.key.keycode,
                        ev.data.key.key_state == INOUT_KEY_DOWN);
        return;
    }

    if (ev.type != INOUT_EVENT_MOUSE) return;

    inout_data_mouse_t *data = &ev.data.mouse;
    uint64_t flags = irq_save();

    if (data->mouse_state == INOUT_MOUSE_DOWN ||
        data->mouse_state == INOUT_MOUSE_UP) {
        uint8_t old = mouse_button_mask();
        bool pressed = (data->mouse_state == INOUT_MOUSE_DOWN);
        if (data->mouse_btn == INOUT_MOUSE_LEFTBTN)
            g_mouse.leftbtn_pressed = pressed;
        else if (data->mouse_btn == INOUT_MOUSE_RIGHTBTN)
            g_mouse.rightbtn_pressed = pressed;
        else if (data->mouse_btn == INOUT_MOUSE_CENTERBTN)
            g_mouse.centerbtn_pressed = pressed;

        uint8_t btn = mouse_button_mask();
        motion_push(0, 0, 0, btn);
        evdev_report_mouse(0, 0, 0, btn, (uint8_t)(old ^ btn));
    }
    else if (data->mouse_state == INOUT_MOUSE_MOVE ||
             data->mouse_state == INOUT_MOUSE_MOVE_ABSOLUTE) {
        int dx = data->x;
        int dy = data->y;
        int dz = data->z;

        if (data->mouse_state == INOUT_MOUSE_MOVE_ABSOLUTE) {
            static int last_x = 0, last_y = 0;
            dx -= last_x;
            dy -= last_y;
            last_x = data->x;
            last_y = data->y;
        }

        if (dx > 4096) dx = 4096; else if (dx < -4096) dx = -4096;
        if (dy > 4096) dy = 4096; else if (dy < -4096) dy = -4096;
        if (dz > 127)  dz = 127;  else if (dz < -127)  dz = -127;

        mouse_scale(&dx, &dy, &dz);

        g_mouse.x += dx;
        g_mouse.y += dy;
        g_mouse.z += dz;

        uint8_t btn = mouse_button_mask();
        motion_push(dx, dy, dz, btn);
        evdev_report_mouse(dx, dy, dz, btn, 0);
    }

    new_mouse_event = true;
    irq_restore(flags);
}

void inout_init(void) {
    input_stream_reset();

    inout_kbd_proc = create_task_kernel((void *)inout_keyboard_task, "inout_kbd");
    inout_mouse_proc = create_task_kernel((void *)inout_mouse_task, "inout_mouse");

    if (!inout_kbd_proc)
        console_print_string("[INOUT] WARN: failed to create kbd task\n");
    if (!inout_mouse_proc)
        console_print_string("[INOUT] WARN: failed to create mouse task\n");
}
