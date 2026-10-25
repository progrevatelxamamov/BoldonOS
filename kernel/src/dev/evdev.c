#include <fs/devfs.h>
#include <linux/input.h>
#include <memory.h>
#include <inout/inout.h>
#include <cpu/rtc.h>
#include <errno.h>

#define EVDEV_BUFFER_SIZE 128

static struct input_event ev_buffer[EVDEV_BUFFER_SIZE];
static uint32_t ev_head = 0;
static uint32_t ev_tail = 0;

void evdev_push_event(uint16_t type, uint16_t code, int32_t value) {
    uint32_t next = (ev_head + 1) % EVDEV_BUFFER_SIZE;
    if (next == ev_tail) return;

    rtc_time_t t;
    rtc_get_time(&t);

    ev_buffer[ev_head].time.tv_sec = t.sec;
    ev_buffer[ev_head].time.tv_usec = 0;
    ev_buffer[ev_head].type = type;
    ev_buffer[ev_head].code = code;
    ev_buffer[ev_head].value = value;

    ev_head = next;
}

static int evdev_read(vfs_file_t *file, void *buf, uint64_t count) {
    if (!buf) return -EFAULT;
    uint32_t max_events = count / sizeof(struct input_event);
    if (max_events == 0) return 0;

    struct input_event *out = (struct input_event *)buf;
    uint32_t read_cnt = 0;

    while (ev_tail != ev_head && read_cnt < max_events) {
        out[read_cnt] = ev_buffer[ev_tail];
        ev_tail = (ev_tail + 1) % EVDEV_BUFFER_SIZE;
        read_cnt++;
    }

    return read_cnt * sizeof(struct input_event);
}

static int evdev_ioctl(vfs_file_t *file, uint64_t request, void *arg) {
    if (!arg) return -EFAULT;
    if (request == EVIOCGVERSION) {
        *(int *)arg = 0x010001;
        return 0;
    }
    if ((request & ~0x1FFF) == (EVIOCGNAME(0) & ~0x1FFF)) {
        const char *name = "BoldonOS Input Device";
        int len = request >> 16;
        int i = 0;
        char *str = (char *)arg;
        while (name[i] && i < len - 1) { str[i] = name[i]; i++; }
        str[i] = 0;
        return i;
    }
    if (request == EVIOCGID) {
        struct input_id *id = (struct input_id *)arg;
        id->bustype = 0x03;
        id->vendor = 0x1234;
        id->product = 0x5678;
        id->version = 0x0100;
        return 0;
    }
    return -EINVAL;
}

static vfs_file_ops_t evdev_ops = {
    .read = evdev_read,
    .write = NULL,
    .close = NULL,
    .lseek = NULL,
    .ioctl = evdev_ioctl,
    .readdir = NULL,
    .mmap = NULL
};

void evdev_init(void) {
    ev_head = 0;
    ev_tail = 0;
    devfs_register_device("input/event0", VFS_TYPE_CHAR_DEV, &evdev_ops, (void*)0);
    devfs_register_device("input/event1", VFS_TYPE_CHAR_DEV, &evdev_ops, (void*)1);
}