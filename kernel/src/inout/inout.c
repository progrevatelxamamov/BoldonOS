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
#include <scheduler/spinlock.h>

#include <fs/devfs.h>
#include <fs/vfs.h>

void xhci_set_keyboard_leds(uint8_t leds);

#ifndef POLLIN
#define POLLIN  0x0001
#define POLLOUT 0x0004
#endif

#define MOVE_Q_SIZE      2048
#define CMD_RESP_Q_SIZE  128
#define KBD_Q_SIZE       512

process_t *inout_kbd_proc   = NULL;
process_t *inout_mouse_proc = NULL;

volatile keyboard_state_t g_keyboard;
volatile mouse_state_t    g_mouse = { .x = 0, .y = 0, .z = 0 };

volatile bool new_kbd_event   = false;
volatile bool new_mouse_event = false;

/* Очередь ответов на команды (ACK 0xFA, Device ID 0x03, BAT 0xAA) */
static uint8_t  cmd_resp_q[CMD_RESP_Q_SIZE];
static volatile uint32_t cmd_resp_qh = 0, cmd_resp_qt = 0;

/* Очередь байтов PS/2-пакетов движения мыши.
   ВАЖНО: в очереди лежат ТОЛЬКО целые пакеты. Раньше при переполнении
   очереди в неё попадал обрывок пакета, поток байтов навсегда
   рассинхронизировался и Nano-X читал чужие байты как dx/dy — курсор
   сам по себе уезжал в правый верхний угол. */
static uint8_t  move_q[MOVE_Q_SIZE];
static volatile uint32_t move_qh = 0, move_qt = 0;

/* Остаток пакета, который не поместился в предыдущий read().
   Без него read(fd, buf, 1) никогда не получал данные. */
static uint8_t  mouse_read_buf[4];
static uint32_t mouse_read_pos = 0;
static uint32_t mouse_read_len = 0;

/* Очереди трогают и USB-задачи, и процесс Nano-X (в т.ч. на разных CPU). */
static spinlock_t input_q_lock = SPINLOCK_INIT;

/* Очередь клавиатуры хранится СОБЫТИЯМИ, а не сырыми байтами: раньше
   обрывок события (особенно потерянный байт отпускания) навсегда
   оставлял Shift включённым у потребителя /dev/kbd (Nano-X). */
typedef struct {
    uint8_t code;
    uint8_t down;
} kbd_event_rec_t;

static kbd_event_rec_t kbd_evt_q[KBD_Q_SIZE];
static volatile uint32_t kbd_evt_h = 0;

/* У клавиатуры ДВА независимых потребителя:
     /dev/kbd (kbd_ops) и консольный PTY в K_MEDIUMRAW/XLATE (init_runner).
   Раньше PTY-путь вообще не пользовался этой очередью, а опрашивал
   g_keyboard.down[] со своей локальной тенью prev_down[] — второй,
   несинхронизированный конечный автомат. Потеря break в нём (или смена
   KDSKBMODE) оставляла Shift залипшим в Nano-X, даже после исправления
   очереди /dev/kbd. Теперь оба читают ОДИН авторитетный поток, но каждый
   со своим курсором и своей тенью, и оба умеют ресинхронизироваться. */
enum {
    KBD_RESYNC_NONE = 0,
    KBD_RESYNC_MOD_UP,      /* явно отпустить все «обычные» модификаторы */
    KBD_RESYNC_MOD_DOWN,    /* вернуть DOWN для реально зажатых */
    KBD_RESYNC_DIFF         /* сличить остальные клавиши с g_keyboard */
};

typedef struct {
    volatile uint32_t tail;          /* курсор в kbd_evt_q */
    bool     sent_down[KEY_MAX];     /* что потребитель уже получил */
    volatile bool sync_needed;
    uint8_t  sync_phase;
    uint32_t sync_cursor;
} kbd_consumer_t;

/* Модификаторы, состояние которых можно безопасно переслать синтетически.
   CapsLock / NumLock / ScrollLock сюда НЕ входят: Nano-X (kbd_ttyscan.c)
   переключает их по событию ОТПУСКАНИЯ, поэтому лишний UP самовольно
   переключил бы режим. Залипает именно Shift — он в этом списке. */
static const uint8_t kbd_resync_mods[] = {
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL,
    KEY_LALT,   KEY_RALT,   KEY_LGUI,  KEY_RGUI
};
#define KBD_RESYNC_MOD_COUNT ((uint32_t)(sizeof(kbd_resync_mods) / sizeof(kbd_resync_mods[0])))

static kbd_consumer_t dev_consumer;   /* /dev/kbd, /dev/input/event0 */
static kbd_consumer_t con_consumer;   /* консольный PTY */

/* Остаток байтов текущего события (read(fd, buf, 1) должен работать). */
static uint8_t  kbd_read_buf[2];
static uint32_t kbd_read_pos = 0, kbd_read_len = 0;

/* Один лок на состояние клавиатуры и на очередь событий: irq_save()
   выключает прерывания только на ЭТОМ CPU и от SMP-гонки не спасает. */
static spinlock_t kbd_state_lock = SPINLOCK_INIT;
static volatile bool kbd_leds_dirty = false;

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

static inline uint32_t move_q_count(void) {
    return (move_qh >= move_qt) ? (move_qh - move_qt)
                                : (MOVE_Q_SIZE - move_qt + move_qh);
}

/* Смена протокола 3 <-> 4 байта обязана очищать и очередь, и остаток
   уже прочитанного пакета, иначе старые 4-байтовые пакеты будут читаться
   как 3-байтовые и поток снова собьётся. */
static void mouse_stream_set_protocol(bool imps2) {
    uint64_t flags;
    spin_lock_irqsave(&input_q_lock, &flags);
    mouse_imps2 = imps2;
    move_qh = move_qt = 0;
    mouse_read_pos = mouse_read_len = 0;
    spin_unlock_irqrestore(&input_q_lock, flags);
}

static void cmd_resp_flush(void) {
    uint64_t flags;
    spin_lock_irqsave(&input_q_lock, &flags);
    cmd_resp_qh = cmd_resp_qt = 0;
    spin_unlock_irqrestore(&input_q_lock, flags);
}

/* Поместить байт ответа на команду в очередь ответов */
static void cmd_resp_push(uint8_t val) {
    uint64_t flags;
    spin_lock_irqsave(&input_q_lock, &flags);
    uint32_t next = (cmd_resp_qh + 1) % CMD_RESP_Q_SIZE;
    if (next != cmd_resp_qt) {
        cmd_resp_q[cmd_resp_qh] = val;
        cmd_resp_qh = next;
    }
    spin_unlock_irqrestore(&input_q_lock, flags);
}

/* Собрать и atomically положить в очередь ОДИН ЦЕЛЫЙ PS/2-пакет. */
static void mouse_packet_push(int dx, int dy, int dz, uint8_t buttons) {
    if (dx >  127) dx =  127; else if (dx < -127) dx = -127;
    if (dy >  127) dy =  127; else if (dy < -127) dy = -127;
    if (dz >  127) dz =  127; else if (dz < -127) dz = -127;

    int8_t sdx = (int8_t)dx;
    /* PS/2: положительное Y — вверх, HID: положительное Y — вниз.
       Nano-X (mou_devmice.c) делает y = -(signed char)data[2],
       поэтому двойного инвертирования нет. */
    int8_t sdy = (int8_t)(-dy);
    int8_t sdw = (int8_t)dz;

    uint8_t b0 = 0x08;                       /* sync bit, обязателен */
    if (buttons & 1) b0 |= 0x01;             /* Left */
    if (buttons & 2) b0 |= 0x02;             /* Right */
    if (buttons & 4) b0 |= 0x04;             /* Middle */
    if (sdx < 0)     b0 |= 0x10;             /* Sign X */
    if (sdy < 0)     b0 |= 0x20;             /* Sign Y */

    uint8_t pkt[4] = { b0, (uint8_t)sdx, (uint8_t)sdy, (uint8_t)sdw };

    uint64_t flags;
    spin_lock_irqsave(&input_q_lock, &flags);
    uint8_t pkt_len = mouse_imps2 ? 4 : 3;

    /* Мало места — выбрасываем СТАРЫЕ ЦЕЛЫЕ пакеты, а не обрываем новый. */
    while (move_q_count() >= pkt_len &&
           (MOVE_Q_SIZE - 1 - move_q_count()) < pkt_len) {
        move_qt = (move_qt + pkt_len) % MOVE_Q_SIZE;
    }
    if ((MOVE_Q_SIZE - 1 - move_q_count()) >= pkt_len) {
        for (uint32_t i = 0; i < pkt_len; i++) {
            move_q[move_qh] = pkt[i];
            move_qh = (move_qh + 1) % MOVE_Q_SIZE;
        }
    }
    spin_unlock_irqrestore(&input_q_lock, flags);
}

static void input_stream_reset(void) {
    uint64_t flags;
    spin_lock_irqsave(&input_q_lock, &flags);
    cmd_resp_qh = cmd_resp_qt = 0;
    move_qh = move_qt = 0;
    mouse_read_pos = mouse_read_len = 0;
    mouse_cmd_state = 0;
    mouse_sample_magic = 0;
    mouse_imps2 = false;
    mouse_enabled = true;
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

static void input_dev_on_mouse_rel(int dx, int dy, int dz, uint8_t buttons) {
    /* Гарантируем, что передача данных всегда разрешена */
    mouse_enabled = true;
    mouse_packet_push(dx, dy, dz, buttons);
}

static void input_dev_on_mouse_abs(int x, int y, uint8_t buttons) {
    static int last_x = 0, last_y = 0;
    int dx = x - last_x;
    int dy = y - last_y;
    last_x = x;
    last_y = y;
    input_dev_on_mouse_rel(dx, dy, 0, buttons);
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
            mouse_enabled = true;

            mouse_stream_set_protocol(true);  /* чистит и очередь, и остаток read() */
            cmd_resp_flush();                 /* чистим старые ACK */

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
        mouse_sample_magic = 0;
        mouse_enabled = true;
        mouse_cmd_state = 0;
        mouse_stream_set_protocol(false);
        cmd_resp_flush();
        cmd_resp_push(0xFA);
        cmd_resp_push(0xAA);
        cmd_resp_push(0x00);
        break;

    case 0xF3: /* Set Sample Rate */
        mouse_cmd_state = 1;
        /* Не шлем ACK сразу, ждем число (200/100/80) */
        break;

    case 0xF2: /* Get Device ID */
        cmd_resp_flush();
        cmd_resp_push(0xFA);
        cmd_resp_push(mouse_imps2 ? 0x03 : 0x00);
        break;

    case 0xE8: /* Set Resolution */
        mouse_cmd_state = 2;
        cmd_resp_push(0xFA);
        break;

    case 0xF4: /* Enable Data Reporting */
        mouse_enabled = true;
        mouse_stream_set_protocol(mouse_imps2);  /* сбросить остатки старых пакетов */
        cmd_resp_push(0xFA);
        break;

    case 0xF5: /* Disable Data Reporting */
        mouse_enabled = true;
        cmd_resp_push(0xFA);
        break;

    case 0xF6: /* Set Defaults */
        mouse_sample_magic = 0;
        mouse_enabled = true;
        mouse_stream_set_protocol(false);
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
    uint64_t lf;

    /* 1. Ответы на команды — побайтно, не больше count. */
    spin_lock_irqsave(&input_q_lock, &lf);
    while (got < count && cmd_resp_qh != cmd_resp_qt) {
        out[got++] = cmd_resp_q[cmd_resp_qt];
        cmd_resp_qt = (cmd_resp_qt + 1) % CMD_RESP_Q_SIZE;
    }
    spin_unlock_irqrestore(&input_q_lock, lf);
    if (got > 0) return (int)got;

    /* 2. Хвост пакета, который не влез в прошлый read(). */
    spin_lock_irqsave(&input_q_lock, &lf);
    while (got < count && mouse_read_pos < mouse_read_len)
        out[got++] = mouse_read_buf[mouse_read_pos++];
    if (mouse_read_pos >= mouse_read_len)
        mouse_read_pos = mouse_read_len = 0;
    spin_unlock_irqrestore(&input_q_lock, lf);
    if (got > 0) return (int)got;

    /* 3. Берём только ЦЕЛЫЙ, синхронизированный пакет. */
    for (;;) {
        spin_lock_irqsave(&input_q_lock, &lf);
        uint8_t pkt_len = mouse_imps2 ? 4 : 3;

        /* На всякий случай ресинхронизация по sync-биту (0x08). */
        while (move_q_count() > 0 &&
               ((move_q[move_qt] & 0x08) == 0 || (move_q[move_qt] & 0xC0) != 0))
            move_qt = (move_qt + 1) % MOVE_Q_SIZE;

        if (move_q_count() >= pkt_len) {
            for (uint32_t i = 0; i < pkt_len; i++) {
                mouse_read_buf[i] = move_q[move_qt];
                move_qt = (move_qt + 1) % MOVE_Q_SIZE;
            }
            mouse_read_len = pkt_len;
            mouse_read_pos = 0;

            while (got < count && mouse_read_pos < mouse_read_len)
                out[got++] = mouse_read_buf[mouse_read_pos++];
            if (mouse_read_pos >= mouse_read_len)
                mouse_read_pos = mouse_read_len = 0;
        }
        spin_unlock_irqrestore(&input_q_lock, lf);

        if (got > 0) return (int)got;
        if (flags & VFS_O_NONBLOCK) return -EAGAIN;
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
    const char *name = file->node->name;  /* это массив, NULL быть не может */

    bool is_mouse =
        (strstr(name, "mouse")  != NULL) ||
        (strstr(name, "mice")   != NULL) ||
        (strstr(name, "psaux")  != NULL) ||
        (strstr(name, "event1") != NULL);

    if (events & POLLIN) {
        if (is_mouse) {
            uint64_t lf;
            spin_lock_irqsave(&input_q_lock, &lf);
            uint8_t pkt_len = mouse_imps2 ? 4 : 3;
            bool ready = (cmd_resp_qh != cmd_resp_qt) ||
                         (mouse_read_pos < mouse_read_len) ||
                         (move_q_count() >= pkt_len);
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

/* Внутренние keycodes >= KEY_EXT_BASE в байтовый поток не влезают
   (старший бит — признак отпускания). Переводим их в PS/2 set 1 E0-коды,
   иначе RAlt превращался в Esc, RCtrl в ноль, а стрелки терялись. */
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

/* Только под kbd_state_lock. Пометить потребителя на ресинхронизацию.
   new_tail — куда поставить курсор: отставший потребитель начинает читать
   ТОЛЬКО свежие события, а состояние восстанавливает из g_keyboard.down[].
   Фазу выставлять ОБЯЗАТЕЛЬНО: sync_needed при sync_phase == NONE приводит
   к тому, что ресинк молча сбрасывает флаг, а залипшая тень остаётся. */
static void kbd_consumer_mark_resync(kbd_consumer_t *c, uint32_t new_tail) {
    c->tail = new_tail;
    c->sync_needed = true;
    c->sync_phase  = KBD_RESYNC_MOD_UP;
    c->sync_cursor = 0;
}

/* Только под kbd_state_lock. Очередь общая, но отставание любого из
   потребителей помечается ресинхронизацией: потерянный UP всё равно
   будет восстановлен из авторитетного g_keyboard.down[]. Курсор
   отставшего ставим на next — сразу ПОСЛЕ записываемого события, чтобы
   потребитель не получил его дважды (ресинк уже учёл новое состояние).
   ВАЖНО: state уже обновлён в key_state_apply() ДО вызова этой функции. */
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

/* Попросить потребителя заново сличить свою тень с g_keyboard.
   Вызывается при открытии устройства, смене KDSKBMODE, hotplug. */
static void kbd_consumer_request_resync(kbd_consumer_t *c) {
    /* Отбрасываем непрочитанную историю: она устарела (кольцо PTY только
       что очистили, сменился KDSKBMODE, открыто устройство). Иначе старый
       UP, доставшийся из очереди ПОСЛЕ корректирующих событий, снова
       перебил бы правильное состояние и вернул залипший/сброшенный Shift. */
    kbd_consumer_mark_resync(c, kbd_evt_h);
}

/* Сверить то, что знает потребитель, с авторитетным g_keyboard.
   Возвращает true, если в *e положено очередное корректирующее событие.
   Вызывается порциями, чтобы read(fd, buf, 1) не получал весь массив сразу. */
static bool kbd_consumer_resync_next_locked(kbd_consumer_t *c, kbd_event_rec_t *e) {
    if (!c->sync_needed) return false;

    for (;;) {
        if (c->sync_phase == KBD_RESYNC_MOD_UP) {
            if (c->sync_cursor < KBD_RESYNC_MOD_COUNT) {
                /* kbd_resync_mods не содержит Caps/Num/Scroll, поэтому
                   синтетический UP безопасен и снимает залипший Shift. */
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

/* ЕДИНСТВЕННОЕ место, где меняется g_keyboard.down[].
   Раньше его писали и USB HID драйвер (по фронтам), и задача
   inout_keyboard_task (из отдельной очереди) без всякой синхронизации:
   потерянное/задержанное событие оставляло Shift включённым навсегда. */
static void key_state_apply(uint64_t keycode, bool down) {
    if (keycode == 0 || keycode >= KEY_MAX) return;

    uint64_t flags;
    spin_lock_irqsave(&kbd_state_lock, &flags);

    bool was = g_keyboard.down[keycode] ? true : false;
    if (was == down) {
        /* Авто-повтор/подтверждение состояния (USB HID шлёт полный набор
           клавиш в каждом репорте). ОБЯЗАТЕЛЬНО обновляем производный
           флаг Shift: если он разошёлся с down[] из-за любого сбоя,
           здесь он снова становится консистентным. */
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
}

void inout_set_key_state(uint64_t keycode, bool down) {
    key_state_apply(keycode, down);
}

/* Преобразовать событие (keycode, down) в 1 или 2 байта PS/2 set 1.
   Возвращает длину. Общая для /dev/kbd и консольного PTY: оба потребителя
   обязаны кодировать отпускание ОДИНАКОВО, иначе Shift «залипнет» у того,
   кто получит не тот байт. */
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

/* Забрать следующее событие для консольного PTY. Возвращает false, если
   событий нет. Сначала отдаём корректирующие события ресинхронизации,
   затем — основную очередь. Тень обновляется здесь же, под локом. */
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

/* Конвертация для PTY в формат K_MEDIUMRAW. */
uint8_t inout_console_scancode_bytes(uint8_t code, bool down, uint8_t out[2]) {
    kbd_event_rec_t e = { .code = code, .down = down ? 1 : 0 };
    return kbd_event_to_bytes(&e, out);
}

/* Попросить оба потребителя перечитать авторитетное состояние. Нужно при
   смене KDSKBMODE и при (пере)подключении клавиатуры: потребитель мог
   находиться в режиме OFF/XLATE и «не видеть» часть make/break. */
void inout_keyboard_request_resync(void) {
    uint64_t flags;
    spin_lock_irqsave(&kbd_state_lock, &flags);
    kbd_consumer_request_resync(&dev_consumer);
    kbd_consumer_request_resync(&con_consumer);
    spin_unlock_irqrestore(&kbd_state_lock, flags);
}

/* При открытии /dev/kbd потребитель считает все клавиши отпущенными.
   Обнуляем его «тень» и просим ресинхронизацию: kbd_dev_read() пришлёт
   DOWN для реально зажатых клавиш. Очередь НЕ очищаем — устройство могут
   открыть и /dev/kbd, и /dev/input/event0, и события нельзя терять. */
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

        /* Остаток события, не влезший в прошлый read(). */
        while (got < count && kbd_read_pos < kbd_read_len)
            out[got++] = kbd_read_buf[kbd_read_pos++];
        if (kbd_read_pos >= kbd_read_len)
            kbd_read_pos = kbd_read_len = 0;

        /* Отдаём столько ЦЕЛЫХ событий, сколько влезает в count. */
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
                break;  /* двубайтовое событие разрезано — хвост в kbd_read_buf */
        }

        spin_unlock_irqrestore(&kbd_state_lock, lf);

        if (got > 0) return (int)got;
        /* Если за проход не удалось выдать ни байта, но ресинк ещё идёт
           (например, все события дали n == 0), не крутимся вечно: следующий
           read()/poll() продолжит с того же sync_cursor. */
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

static vfs_file_ops_t mouse_ops = {
    .read  = mouse_read,
    .write = mouse_write,
    .poll  = input_poll_events
};

static vfs_file_ops_t kbd_ops = {
    .read  = kbd_read,
    .write = kbd_write,
    .open  = kbd_open,
    .poll  = input_poll_events
};

void input_register_devfs(void) {
    input_stream_reset();


    devfs_register_device("mouse",        VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("input/mice",   VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("psaux",        VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);
    devfs_register_device("input/event1", VFS_TYPE_CHAR_DEV, &mouse_ops, NULL);

    devfs_register_device("kbd",          VFS_TYPE_CHAR_DEV, &kbd_ops, NULL);
    devfs_register_device("input/event0", VFS_TYPE_CHAR_DEV, &kbd_ops, NULL);
}

/* Состояние клавиш теперь обновляется прямо в inout_add_event().
   Задача нужна только для «медленной» части — мигания светодиодами:
   ps2kbd_set_leds() делает ввод-вывод и не должна зваться из IRQ. */
void inout_keyboard_task(void) {
    for (;;) {
        if (kbd_leds_dirty) {
            kbd_leds_dirty = false;
            update_keyboard_leds();
        }
        schedule_yield();
    }
}

/* Мышиные события обрабатываются сразу в inout_add_event(), отдельная
   очередь не нужна. Оставляем задачу как «дежурную» без task_sleep(),
   чтобы она не могла уснуть навсегда из-за lost wakeup. */
void inout_mouse_task(void) {
    for (;;) schedule_yield();
}

void inout_call_scheduler(void) {
    /* Обработка событий идёт прямо в inout_add_event(), задачи больше
       не спят: task_sleep()+task_wake() давали lost wakeup на SMP, и
       событие отпускания Shift могло навсегда застрять в очереди. */
    (void)new_kbd_event;
    (void)new_mouse_event;
}

void inout_add_event(inout_event_t ev) {
    uint64_t flags = irq_save();

    if (ev.type == INOUT_EVENT_KEY) {
        key_state_apply(ev.data.key.keycode,
                        ev.data.key.key_state == INOUT_KEY_DOWN);
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

            input_dev_on_mouse_rel(0, 0, 0, btn);
        }
        else if (data->mouse_state == INOUT_MOUSE_MOVE) {
            g_mouse.x += data->x;
            g_mouse.y += data->y;
            g_mouse.z += data->z;

            uint8_t btn = 0;
            if (g_mouse.leftbtn_pressed)   btn |= 1;
            if (g_mouse.rightbtn_pressed)  btn |= 2;
            if (g_mouse.centerbtn_pressed) btn |= 4;

            input_dev_on_mouse_rel(data->x, data->y, data->z, btn);
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
}

void inout_init(void) {
    input_stream_reset();

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