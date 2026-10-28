#include <fs/devfs.h>
#include <fs/vfs.h>
#include <dev/evdev.h>
#include <linux/input.h>
#include <memory.h>
#include <string.h>
#include <errno.h>
#include <cpu/time.h>
#include <scheduler/scheduler.h>
#include <scheduler/spinlock.h>

#define EVDEV_BUFFER_SIZE 256

#ifndef POLLIN
#define POLLIN  0x0001
#define POLLOUT 0x0004
#endif

#define EV_SYN_CODE     0x00
#define EV_KEY_CODE     0x01
#define EV_REL_CODE     0x02
#define EV_MSC_CODE     0x04
#define EV_LED_CODE     0x11
#define SYN_REPORT_CODE 0x00
#define MSC_SCAN_CODE   0x04

#define LK_KPENTER    96
#define LK_RIGHTCTRL  97
#define LK_KPSLASH    98
#define LK_SYSRQ      99
#define LK_RIGHTALT   100
#define LK_HOME       102
#define LK_UP         103
#define LK_PAGEUP     104
#define LK_LEFT       105
#define LK_RIGHT      106
#define LK_END        107
#define LK_DOWN       108
#define LK_PAGEDOWN   109
#define LK_INSERT     110
#define LK_DELETE     111
#define LK_PAUSE      119
#define LK_LEFTMETA   125
#define LK_RIGHTMETA  126
#define LK_MENU       139

typedef struct {
    struct input_event buf[EVDEV_BUFFER_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    spinlock_t lock;
    const char *name;
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    bool keyboard;
} evdev_device_t;

static evdev_device_t ev_keyboard;
static evdev_device_t ev_mouse;

static uint16_t linux_keycode(uint8_t code) {
    switch (code) {
    case 0x80: return LK_RIGHTCTRL;
    case 0x81: return LK_RIGHTALT;
    case 0x82: return LK_KPENTER;
    case 0x83: return LK_KPSLASH;
    case 0x84: return LK_HOME;
    case 0x85: return LK_UP;
    case 0x86: return LK_PAGEUP;
    case 0x87: return LK_LEFT;
    case 0x88: return LK_RIGHT;
    case 0x89: return LK_END;
    case 0x8A: return LK_DOWN;
    case 0x8B: return LK_PAGEDOWN;
    case 0x8C: return LK_INSERT;
    case 0x8D: return LK_DELETE;
    case 0x8E: return LK_LEFTMETA;
    case 0x8F: return LK_RIGHTMETA;
    case 0x90: return LK_MENU;
    case 0x91: return LK_SYSRQ;
    case 0x92: return LK_PAUSE;
    default: break;
    }
    if (code > 0x92) return 0;
    return code;
}

static void evdev_init_device(evdev_device_t *dev, const char *name, bool keyboard,
                              uint16_t bus, uint16_t vendor, uint16_t product) {
    memset(dev, 0, sizeof(*dev));
    dev->name = name;
    dev->keyboard = keyboard;
    dev->bustype = bus;
    dev->vendor = vendor;
    dev->product = product;
}

static void evdev_push(evdev_device_t *dev, uint16_t type, uint16_t code, int32_t value) {
    uint64_t flags;
    uint64_t now = get_monotonic_ms();

    spin_lock_irqsave(&dev->lock, &flags);
    uint32_t next = (dev->head + 1) % EVDEV_BUFFER_SIZE;
    if (next == dev->tail) {
        dev->tail = (dev->tail + 1) % EVDEV_BUFFER_SIZE;
        struct input_event *syn = &dev->buf[dev->head];
        syn->time.tv_sec = now / 1000;
        syn->time.tv_usec = (now % 1000) * 1000;
        syn->type = EV_SYN_CODE;
        syn->code = 3;
        syn->value = 0;
        dev->head = next;
        next = (dev->head + 1) % EVDEV_BUFFER_SIZE;
        if (next == dev->tail) dev->tail = (dev->tail + 1) % EVDEV_BUFFER_SIZE;
    }
    struct input_event *e = &dev->buf[dev->head];
    e->time.tv_sec = now / 1000;
    e->time.tv_usec = (now % 1000) * 1000;
    e->type = type;
    e->code = code;
    e->value = value;
    dev->head = next;
    spin_unlock_irqrestore(&dev->lock, flags);
}

void evdev_report_key(uint8_t keycode, bool down) {
    uint16_t code = linux_keycode(keycode);
    if (!code) return;
    evdev_push(&ev_keyboard, EV_KEY_CODE, code, down ? 1 : 0);
    evdev_push(&ev_keyboard, EV_SYN_CODE, SYN_REPORT_CODE, 0);
}

void evdev_report_mouse(int dx, int dy, int dz, uint8_t buttons, uint8_t changed) {
    static const uint16_t btn_codes[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    bool sent = false;

    if (dx) { evdev_push(&ev_mouse, EV_REL_CODE, REL_X, dx); sent = true; }
    if (dy) { evdev_push(&ev_mouse, EV_REL_CODE, REL_Y, dy); sent = true; }
    if (dz) { evdev_push(&ev_mouse, EV_REL_CODE, REL_WHEEL, dz); sent = true; }

    for (int i = 0; i < 3; i++) {
        if (!(changed & (1u << i))) continue;
        evdev_push(&ev_mouse, EV_KEY_CODE, btn_codes[i], (buttons & (1u << i)) ? 1 : 0);
        sent = true;
    }
    if (sent) evdev_push(&ev_mouse, EV_SYN_CODE, SYN_REPORT_CODE, 0);
}

void evdev_report_sync(int device) {
    if (device == 0) evdev_push(&ev_keyboard, EV_SYN_CODE, SYN_REPORT_CODE, 0);
    else evdev_push(&ev_mouse, EV_SYN_CODE, SYN_REPORT_CODE, 0);
}

static evdev_device_t *evdev_from_file(vfs_file_t *file) {
    if (!file || !file->node) return NULL;
    return (evdev_device_t *)file->node->fs_data;
}

static int evdev_read(vfs_file_t *file, void *buf, uint64_t count) {
    evdev_device_t *dev = evdev_from_file(file);
    if (!dev || !buf) return -EINVAL;

    uint32_t max_events = (uint32_t)(count / sizeof(struct input_event));
    if (!max_events) return 0;

    struct input_event *out = (struct input_event *)buf;
    uint32_t got = 0;

    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&dev->lock, &flags);
        while (got < max_events && dev->tail != dev->head) {
            out[got++] = dev->buf[dev->tail];
            dev->tail = (dev->tail + 1) % EVDEV_BUFFER_SIZE;
        }
        spin_unlock_irqrestore(&dev->lock, flags);

        if (got) return (int)(got * sizeof(struct input_event));
        if (file->flags & VFS_O_NONBLOCK) return -EAGAIN;
        schedule_yield();
    }
}

static void set_bit(uint8_t *bits, uint32_t bit, uint32_t len) {
    uint32_t byte = bit >> 3;
    if (byte < len) bits[byte] |= (uint8_t)(1u << (bit & 7));
}

static int evdev_ioctl(vfs_file_t *file, uint64_t request, void *arg) {
    evdev_device_t *dev = evdev_from_file(file);
    if (!dev) return -ENODEV;

    uint32_t nr = (uint32_t)(request & 0xFF);
    uint32_t type = (uint32_t)((request >> 8) & 0xFF);
    uint32_t dir = (uint32_t)(request >> 30);
    uint32_t len = (uint32_t)((request >> 16) & 0x1FFF);

    if (type == 0x45 && nr == 0x01 && dir == 2) {
        if (!arg) return -EFAULT;
        *(int *)arg = 0x010001;
        return 0;
    }
    if (type == 0x45 && nr == 0x02 && dir == 2) {
        if (!arg) return -EFAULT;
        struct input_id *id = (struct input_id *)arg;
        id->bustype = dev->bustype;
        id->vendor = dev->vendor;
        id->product = dev->product;
        id->version = 0x0100;
        return 0;
    }
    if (type == 0x45 && nr == 0x06 && dir == 2) {
        if (!arg || !len) return -EFAULT;
        char *dst = (char *)arg;
        uint32_t i = 0;
        while (dev->name[i] && i + 1 < len) { dst[i] = dev->name[i]; i++; }
        dst[i] = 0;
        return (int)(i + 1);
    }
    if (nr == 0x20 && dir == 2 && type >= 0x45 && type <= 0x45 + 0x1F) {
        if (!arg || !len) return -EFAULT;
        uint8_t *bits = (uint8_t *)arg;
        memset(bits, 0, len);
        uint32_t ev = type - 0x45;
        if (ev == EV_SYN_CODE) {
            set_bit(bits, EV_SYN_CODE, len);
            set_bit(bits, EV_KEY_CODE, len);
            set_bit(bits, EV_MSC_CODE, len);
            if (dev->keyboard) set_bit(bits, EV_LED_CODE, len);
            else set_bit(bits, EV_REL_CODE, len);
        } else if (ev == EV_KEY_CODE) {
            if (dev->keyboard) {
                for (uint32_t k = 1; k <= 119 && k < len * 8; k++)
                    set_bit(bits, k, len);
            }
            set_bit(bits, BTN_LEFT, len);
            set_bit(bits, BTN_RIGHT, len);
            set_bit(bits, BTN_MIDDLE, len);
        } else if (ev == EV_REL_CODE && !dev->keyboard) {
            set_bit(bits, REL_X, len);
            set_bit(bits, REL_Y, len);
            set_bit(bits, REL_Z, len);
            set_bit(bits, REL_WHEEL, len);
            set_bit(bits, REL_HWHEEL, len);
        } else if (ev == EV_LED_CODE && dev->keyboard) {
            set_bit(bits, 0, len);
            set_bit(bits, 1, len);
            set_bit(bits, 2, len);
        } else if (ev == EV_MSC_CODE) {
            set_bit(bits, MSC_SCAN_CODE, len);
        }
        return 0;
    }
    if (type == 0x45 && (nr == 0x18 || nr == 0x19) && dir == 2) {
        if (!arg || !len) return -EFAULT;
        memset(arg, 0, len);
        return 0;
    }
    if (type == 0x45 && nr == 0x03 && dir == 2) {
        if (!arg) return -EFAULT;
        uint32_t *rep = (uint32_t *)arg;
        rep[0] = 250;
        rep[1] = 33;
        return 0;
    }
    if (type == 0x45 && nr == 0x03 && dir == 1) return 0;
    if (type == 0x45 && nr == 0x90 && dir == 1) return 0;
    return -ENOTTY;
}

static uint32_t evdev_poll(vfs_file_t *file, uint32_t events) {
    evdev_device_t *dev = evdev_from_file(file);
    uint32_t revents = 0;
    if (!dev) return 0;
    if (events & POLLIN) {
        uint64_t flags;
        spin_lock_irqsave(&dev->lock, &flags);
        bool ready = dev->head != dev->tail;
        spin_unlock_irqrestore(&dev->lock, flags);
        if (ready) revents |= POLLIN;
    }
    if (events & POLLOUT) revents |= POLLOUT;
    return revents;
}

static vfs_file_ops_t evdev_keyboard_ops = {
    .read = evdev_read,
    .ioctl = evdev_ioctl,
    .poll = evdev_poll
};

static vfs_file_ops_t evdev_mouse_ops = {
    .read = evdev_read,
    .ioctl = evdev_ioctl,
    .poll = evdev_poll
};

void evdev_init(void) {
    evdev_init_device(&ev_keyboard, "BoldonOS Keyboard", true, 0x03, 0x1234, 0x5678);
    evdev_init_device(&ev_mouse, "BoldonOS Mouse", false, 0x03, 0x1234, 0x5679);

    devfs_register_device("input/event0", VFS_TYPE_CHAR_DEV, &evdev_keyboard_ops, &ev_keyboard);
    devfs_register_device("input/event1", VFS_TYPE_CHAR_DEV, &evdev_mouse_ops, &ev_mouse);
}
