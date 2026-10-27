#include <fs/pty.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <mm/malloc.h>
#include <memory.h>
#include <gconsole.h>
#include <inout/inout.h>
#include <scheduler/scheduler.h>
#include <scheduler/spinlock.h>
#include <errno.h>

static vfs_file_ops_t ptmx_ops;
static vfs_file_ops_t pty_master_ops;
static vfs_node_t *make_pty_node(const char *name, vfs_file_ops_t *ops, pty_t *p);

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCGETS2     0x802C542A
#define TCSETS2     0x402C542B
#define TCSETSW2    0x402C542C
#define TCSETSF2    0x402C542D
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define FIONREAD    0x541B
#define TIOCGPTN    0x80045430
#define TIOCSPTLCK  0x40045431

#define KDGETLED    0x4B31
#define KDSETLED    0x4B32
#define KDGKBENT    0x4B46

struct kbentry {
    unsigned char kb_table;
    unsigned char kb_index;
    unsigned short kb_value;
};

#define KDSETMODE   0x4B3A
#define KDGETMODE   0x4B3B
#define KDGKBTYPE   0x4B33
#define KDGKBMODE   0x4B44
#define KDSKBMODE   0x4B45

#define VT_OPENQRY  0x5606
#define VT_GETMODE  0x5601
#define VT_SETMODE  0x5602
#define VT_GETSTATE 0x5603
#define VT_RELDISP  0x5604
#define VT_ACTIVATE 0x5605
#define VT_WAITACTIVE 0x5607

struct vt_stat {
    unsigned short v_active;
    unsigned short v_signal;
    unsigned short v_state;
};

static pty_t ptys[MAX_PTYS];
static spinlock_t pty_lock = SPINLOCK_INIT;

int pty_poll_events(vfs_file_t *file, short events);
static uint32_t pty_poll_vfs(struct vfs_file *file, uint32_t mask) { return pty_poll_events(file, (short)mask); }

static void pty_set_default_termios(struct termios *t) {
    memset(t, 0, sizeof(struct termios));

    t->c_iflag = ICRNL | IXON | IMAXBEL | IUTF8;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD | CLOCAL | B38400;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE;

    t->c_cc[VINTR]  = 3;   /* Ctrl+C */
    t->c_cc[VQUIT]  = 28;  /* Ctrl+\ */
    t->c_cc[VERASE] = 127; /* Backspace */
    t->c_cc[VKILL]  = 21;  /* Ctrl+U */
    t->c_cc[VEOF]   = 4;   /* Ctrl+D */
    t->c_cc[VTIME]  = 0;
    t->c_cc[VMIN]   = 1;

    t->c_ispeed = 38400;
    t->c_ospeed = 38400;
}

static void pty_get_screen_dimensions(uint16_t *rows, uint16_t *cols) {
    struct limine_framebuffer_response *fb = get_base_framebuffer();
    if (fb && fb->framebuffer_count > 0) {
        uint64_t idx = get_base_index_framebuffer();
        struct limine_framebuffer *f = fb->framebuffers[idx];
        *cols = (uint16_t)(f->width / 8);
        *rows = (uint16_t)(f->height / 16);
        return;
    }
    *cols = 80;
    *rows = 25;
}

static int ring_write(pty_ring_t *r, const uint8_t *data, uint64_t count) {
    uint64_t n = 0;
    while (n < count) {
        uint32_t next = (r->head + 1) % PTY_BUF_SIZE;
        if (next == r->tail) break;
        r->buf[r->head] = data[n++];
        r->head = next;
    }
    return (int)n;
}

static int ring_read(pty_ring_t *r, uint8_t *data, uint64_t count) {
    uint64_t n = 0;
    while (n < count && r->tail != r->head) {
        data[n++] = r->buf[r->tail];
        r->tail = (r->tail + 1) % PTY_BUF_SIZE;
    }
    return (int)n;
}

static int pty_master_read(vfs_file_t *file, void *buf, uint64_t count) {
    if (!file || !file->node || !buf) return -EFAULT;
    pty_t *p = (pty_t *)file->node->fs_data;
    if (!p) return -EINVAL;
    if (count == 0) return 0;

    while (1) {
        uint32_t avail = (p->out.head >= p->out.tail) ?
                         (p->out.head - p->out.tail) :
                         (PTY_BUF_SIZE - p->out.tail + p->out.head);

        if (avail > 0) {
            return ring_read(&p->out, (uint8_t *)buf, count);
        }

        if (!p->slave_open) return 0;

        if (file->flags & (VFS_O_NONBLOCK | 0x800)) {
            return -EAGAIN;
        }

        schedule_yield();
    }
}

static int pty_master_write(vfs_file_t *file, const void *buf, uint64_t count) {
    if (!file || !file->node || !buf) return -EFAULT;
    pty_t *p = (pty_t *)file->node->fs_data;
    if (!p) return -EINVAL;
    return ring_write(&p->in, (const uint8_t *)buf, count);
}

static int pty_slave_read(vfs_file_t *file, void *buf, uint64_t count) {
    if (!file || !file->node || !buf) return -EFAULT;
    pty_t *p = (pty_t *)file->node->fs_data;
    if (!p) return -EINVAL;
    if (count == 0) return 0;

    while (1) {
        if (p->id == 0 && gconsole_framebuffer_acquired()) {
            process_t *owner = gconsole_framebuffer_owner();
            if (owner && get_current_process() != owner) {
                if (file->flags & (VFS_O_NONBLOCK | 0x800)) {
                    return -EAGAIN;
                }
                schedule_yield();
                continue;
            }
        }

        uint32_t avail = (p->in.head >= p->in.tail) ?
                         (p->in.head - p->in.tail) :
                         (PTY_BUF_SIZE - p->in.tail + p->in.head);

        if (avail > 0) {
            int n = ring_read(&p->in, (uint8_t *)buf, count);
            return n;
        }

        if (file->flags & (VFS_O_NONBLOCK | 0x800)) {
            return -EAGAIN;
        }

        schedule_yield();
    }
}

static int pty_slave_write(vfs_file_t *file, const void *buf, uint64_t count) {
    if (!file || !file->node || !buf) return -EFAULT;
    pty_t *p = (pty_t *)file->node->fs_data;
    if (!p) return -EINVAL;

    if (p->id == 0) {
        const char *s = (const char *)buf;
        for (uint64_t i = 0; i < count; i++) {
            if (s[i] == 127 || s[i] == '\b') {
                console_print_string("\b \b");
            } else {
                console_print_char(s[i]);
            }
        }
    }
    ring_write(&p->out, (const uint8_t *)buf, count);
    return (int)count;
}

static void pty_reset_pair(pty_t *p) {
    memset(&p->in, 0, sizeof(pty_ring_t));
    memset(&p->out, 0, sizeof(pty_ring_t));
    pty_get_screen_dimensions(&p->winsize_row, &p->winsize_col);
    pty_set_default_termios(&p->termios);
}

static int ptmx_open(vfs_file_t *file) {
    if (!file) return -EINVAL;

    spin_lock(&pty_lock);
    pty_t *p = NULL;
    for (int i = 1; i < MAX_PTYS; i++) {
        if (!ptys[i].used) {
            p = &ptys[i];
            break;
        }
    }
    if (!p) {
        spin_unlock(&pty_lock);
        return -ENXIO;
    }

    p->used = true;
    p->dynamic = true;
    p->master_open = true;
    p->slave_open = false;
    p->master_refs = 1;
    p->slave_refs = 0;
    pty_reset_pair(p);
    spin_unlock(&pty_lock);

    if (!p->master_node) {
        p->master_node = make_pty_node("ptmx", &pty_master_ops, p);
        if (!p->master_node) {
            spin_lock(&pty_lock);
            p->used = false;
            p->dynamic = false;
            p->master_open = false;
            p->master_refs = 0;
            spin_unlock(&pty_lock);
            return -ENOMEM;
        }
    }

    file->node = p->master_node;
    file->private_data = p;
    return 0;
}

static int pty_slave_open(vfs_file_t *file) {
    if (!file || !file->node || !file->node->fs_data) return -EINVAL;
    pty_t *p = (pty_t *)file->node->fs_data;

    spin_lock(&pty_lock);
    p->slave_open = true;
    p->slave_refs++;
    spin_unlock(&pty_lock);

    file->private_data = p;
    return 0;
}

static int pty_close(vfs_file_t *file) {
    if (!file || !file->node) return 0;
    pty_t *p = (pty_t *)file->node->fs_data;
    if (!p && file->private_data) p = (pty_t *)file->private_data;
    if (!p) return 0;

    spin_lock(&pty_lock);
    if (file->node == p->master_node) {
        if (p->master_refs > 0) p->master_refs--;
        p->master_open = p->master_refs > 0;
    } else {
        if (p->slave_refs > 0) p->slave_refs--;
        p->slave_open = p->slave_refs > 0;
    }

    if (p->dynamic && p->master_refs == 0 && p->slave_refs == 0) {
        p->used = false;
        p->dynamic = false;
    }
    spin_unlock(&pty_lock);
    return 0;
}

static uint16_t pty_kbd_value(uint32_t map, uint32_t index) {
    if (index >= KEY_MAX) return 0;
    if (map & 0xA) return 0;
    char normal = keymap_normal[index];
    bool letter = keymap_caps[index] != 0;
    if (map & 4) {
        if (!letter) return 0;
        return (uint16_t)(0x900 | (uint8_t)(normal - 'a' + 1));
    }
    if (letter) {
        char c = (map & 1) ? keymap_caps[index] : normal;
        return (uint16_t)(0xB00 | (uint8_t)c);
    }
    if (index == KEY_BACKSPACE) return 0x97F;
    if (index == KEY_ENTER || index == KEY_KP_ENTER) return 0x90D;
    if (!normal) return 0;
    char c = ((map & 1) && keymap_shift[index]) ? keymap_shift[index] : normal;
    return (uint16_t)(uint8_t)c;
}

static int pty_ioctl(vfs_file_t *file, uint64_t request, void *arg) {
    if (!file || !file->node) return -EINVAL;
    pty_t *p = (pty_t *)file->node->fs_data;

    if (request == VT_OPENQRY) {
        if (arg) *(int *)arg = 1;
        return 0;
    }

    if (request == VT_GETSTATE) {
        if (arg) {
            struct vt_stat *st = (struct vt_stat *)arg;
            st->v_active = 1;
            st->v_signal = 0;
            st->v_state  = 1;
        }
        return 0;
    }

    if (request == KDGKBTYPE) {
        if (arg) *(char *)arg = 0x02;
        return 0;
    }

    if (request == KDGKBMODE) {
        if (!arg) return -EFAULT;
        if (!p) return -EINVAL;
        *(int *)arg = p->kbd_mode;
        return 0;
    }

    if (request == KDSKBMODE) {
        if (!p) return -EINVAL;
        int mode = (int)(uint64_t)arg;
        if (mode < PTY_KBD_RAW || mode > PTY_KBD_OFF) return -EINVAL;
        p->kbd_mode = mode;
        /* Потребитель сменил режим (обычно Nano-X ставит K_MEDIUMRAW).
           Его внутренняя тень модификаторов теперь неизвестна ядру —
           принудительно пересылаем полное состояние, иначе залипший Shift
           из предыдущего режима останется навсегда. */
        inout_keyboard_request_resync();
        return 0;
    }

    if (request == KDGETLED) {
        if (!arg) return -EFAULT;
        unsigned char leds = 0;
        if (g_keyboard.scroll_lock) leds |= 0x01;
        if (g_keyboard.num_lock) leds |= 0x02;
        if (g_keyboard.caps_lock) leds |= 0x04;
        *(unsigned char *)arg = leds;
        return 0;
    }

    if (request == KDSETLED) {
        return 0;
    }

    if (request == KDGKBENT) {
        if (!arg) return -EFAULT;
        if (!p) return -EINVAL;
        struct kbentry *entry = (struct kbentry *)arg;
        entry->kb_value = pty_kbd_value(entry->kb_table, entry->kb_index);
        return 0;
    }

    if ((request >= 0x5600 && request <= 0x560F) || 
        (request >= 0x4B00 && request <= 0x4BFF)) {
        if (arg && (request == KDGETMODE || request == KDGKBMODE)) {
            *(int *)arg = 0;
        }
        return 0;
    }

    if (!p) return -EINVAL;

    if (request == TIOCGPTN) {
        if (!arg) return -EFAULT;
        *(unsigned int *)arg = (unsigned int)p->id;
        return 0;
    }

    if (request == TIOCSPTLCK) {
        if (!arg) return -EFAULT;
        return 0;
    }

    if (request == TCGETS || request == TCGETS2 || request == 0x802C542A || request == 0x8030542A) {
        if (!arg) return -EFAULT;
        memcpy(arg, &p->termios, sizeof(struct termios));
        return 0;
    }

    if (request == TCSETS  || request == TCSETSW  || request == TCSETSF  ||
        request == TCSETS2 || request == TCSETSW2 || request == TCSETSF2 ||
        request == 0x402C542B || request == 0x402C542C || request == 0x402C542D) {
        if (arg) {
            memcpy(&p->termios, arg, sizeof(struct termios));
        }
        return 0;
    }

    if (request == FIONREAD) {
        if (!arg) return -EFAULT;
        uint32_t avail = (p->in.head >= p->in.tail) ?
                         (p->in.head - p->in.tail) :
                         (PTY_BUF_SIZE - p->in.tail + p->in.head);
        *(int *)arg = (int)avail;
        return 0;
    }

    if (request == TIOCGWINSZ || request == 0x80085413) {
        if (!arg) return -EFAULT;
        struct winsize *ws = (struct winsize *)arg;
        ws->ws_row = p->winsize_row;
        ws->ws_col = p->winsize_col;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }

    if (request == TIOCSWINSZ) {
        if (!arg) return -EFAULT;
        struct winsize *ws = (struct winsize *)arg;
        p->winsize_row = ws->ws_row;
        p->winsize_col = ws->ws_col;
        return 0;
    }

    if (request == TIOCGPGRP) {
        if (!arg) return -EFAULT;
        process_t *proc = get_current_process();
        *(int *)arg = proc ? proc->pgid : 1;
        return 0;
    }

    if (request == TIOCSPGRP || request == TIOCSCTTY) {
        return 0;
    }

    return -25; // -ENOTTY
}

static vfs_file_ops_t pty_master_ops = {
    .read  = pty_master_read,
    .write = pty_master_write,
    .close = pty_close,
    .ioctl = pty_ioctl,
    .poll = pty_poll_vfs
};

static vfs_file_ops_t pty_slave_ops = {
    .read  = pty_slave_read,
    .write = pty_slave_write,
    .open  = pty_slave_open,
    .close = pty_close,
    .ioctl = pty_ioctl,
    .poll = pty_poll_vfs
};

static vfs_node_t *make_pty_node(const char *name, vfs_file_ops_t *ops, pty_t *p) {
    vfs_node_t *n = malloc_alloc(sizeof(vfs_node_t));
    memset(n, 0, sizeof(vfs_node_t));
    int i = 0;
    while (name[i] && i < 255) { n->name[i] = name[i]; i++; }
    n->name[i] = 0;
    n->type = VFS_TYPE_CHAR_DEV;
    n->file_ops = ops;
    n->fs_data = p;
    n->mode = 0666;
    return n;
}

void pty_init(void) {
    memset(ptys, 0, sizeof(ptys));
    uint16_t r = 25, c = 80;
    pty_get_screen_dimensions(&r, &c);

    for (int i = 0; i < MAX_PTYS; i++) {
        ptys[i].id = i;
        ptys[i].winsize_row = r;
        ptys[i].winsize_col = c;
        ptys[i].kbd_mode = PTY_KBD_XLATE;
        pty_set_default_termios(&ptys[i].termios);
    }

    for (int i = 0; i < MAX_PTYS; i++) {
        char name[32] = {0};
        name[0] = 'p'; name[1] = 't'; name[2] = 's'; name[3] = '/';
        if (i < 10) {
            name[4] = '0' + i;
        } else {
            name[4] = '0' + (i / 10);
            name[5] = '0' + (i % 10);
        }
        ptys[i].slave_node = make_pty_node(name, &pty_slave_ops, &ptys[i]);
        ptys[i].master_node = make_pty_node("ptmx", &pty_master_ops, &ptys[i]);
        devfs_register_device(name, VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[i]);
    }
    devfs_register_device("ptmx", VFS_TYPE_CHAR_DEV, &ptmx_ops, NULL);
    devfs_register_device("tty", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
    devfs_register_device("console", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);

    devfs_register_device("tty0", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
    devfs_register_device("tty1", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
    devfs_register_device("tty2", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
    devfs_register_device("tty3", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
    devfs_register_device("tty4", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
    devfs_register_device("tty5", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
    devfs_register_device("tty6", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
    devfs_register_device("tty7", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
}

void pty_set_size(int pty_id, uint16_t rows, uint16_t cols) {
    if (pty_id >= 0 && pty_id < MAX_PTYS) {
        ptys[pty_id].winsize_row = rows;
        ptys[pty_id].winsize_col = cols;
    }
}

static int pty_find_free(void) {
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!ptys[i].used) return i;
    }
    return -1;
}

int pty_alloc(int *master_fd, int *slave_fd, process_fd_table_t *table) {
    if (!table || !master_fd || !slave_fd) { errno = EINVAL; return -1; }

    spin_lock(&pty_lock);
    int id = pty_find_free();
    if (id < 0) {
        spin_unlock(&pty_lock);
        errno = ENOMEM;
        return -1;
    }

    pty_t *p = &ptys[id];
    p->used = true;
    p->dynamic = true;
    p->master_open = true;
    p->slave_open = true;
    p->master_refs = 1;
    p->slave_refs = 1;
    pty_reset_pair(p);
    spin_unlock(&pty_lock);

    if (!p->master_node) p->master_node = make_pty_node("ptmx", &pty_master_ops, p);
    if (!p->slave_node) p->slave_node = make_pty_node("pts", &pty_slave_ops, p);
    if (!p->master_node || !p->slave_node) {
        spin_lock(&pty_lock);
        p->used = false;
        p->dynamic = false;
        p->master_open = false;
        p->slave_open = false;
        p->master_refs = 0;
        p->slave_refs = 0;
        spin_unlock(&pty_lock);
        errno = ENOMEM;
        return -1;
    }

    vfs_file_t *mf = malloc_alloc(sizeof(vfs_file_t));
    vfs_file_t *sf = malloc_alloc(sizeof(vfs_file_t));
    memset(mf, 0, sizeof(vfs_file_t));
    memset(sf, 0, sizeof(vfs_file_t));
    mf->node = p->master_node;
    sf->node = p->slave_node;
    mf->flags = VFS_O_RDWR;
    sf->flags = VFS_O_RDWR;
    mf->refcount = 1;
    sf->refcount = 1;

    int mfd = -1, sfd = -1;
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (!table->fds[i]) {
            if (mfd < 0)      { table->fds[i] = mf; mfd = i; }
            else if (sfd < 0) { table->fds[i] = sf; sfd = i; break; }
        }
    }

    if (mfd < 0 || sfd < 0) {
        spin_lock(&pty_lock);
        p->used = false;
        p->dynamic = false;
        p->master_open = false;
        p->slave_open = false;
        p->master_refs = 0;
        p->slave_refs = 0;
        spin_unlock(&pty_lock);
        if (mfd >= 0) table->fds[mfd] = NULL;
        if (sfd >= 0) table->fds[sfd] = NULL;
        malloc_free(mf);
        malloc_free(sf);
        errno = EMFILE;
        return -1;
    }

    *master_fd = mfd;
    *slave_fd  = sfd;
    return 0;
}

static void pty_input_mode_sync(void) {
    static bool graphics_active = false;
    bool now_active = gconsole_framebuffer_acquired();
    if (now_active != graphics_active) {
        graphics_active = now_active;
        memset(&ptys[0].in, 0, sizeof(pty_ring_t));
        if (!now_active) ptys[0].kbd_mode = PTY_KBD_XLATE;
        /* Кольцо только что очищено: все ещё непрочитанные make/break
           потеряны. Восстанавливаем состояние у потребителей. */
        inout_keyboard_request_resync();
    }
}

static void pty_feed_byte(uint8_t val) {
    for (int i = 0; i < MAX_PTYS; i++) {
        if (ptys[i].used) {
            ring_write(&ptys[i].in, &val, 1);
            return;
        }
    }
}

void pty_feed_input(char c) {
    pty_input_mode_sync();
    uint8_t val = (uint8_t)c;
    if (c == '\r') val = '\n';
    pty_feed_byte(val);
}

void pty_feed_scancode(uint8_t byte) {
    pty_input_mode_sync();
    pty_feed_byte(byte);
}

int pty_console_kbd_mode(void) {
    return ptys[0].kbd_mode;
}

void pty_poll_output(void) {}

static vfs_file_ops_t ptmx_ops = {
    .read  = pty_master_read,
    .write = pty_master_write,
    .open  = ptmx_open,
    .close = pty_close,
    .ioctl = pty_ioctl,
    .poll = pty_poll_vfs
};

void pty_register_devfs(void) {
    pty_init();
    for (int i = 0; i < MAX_PTYS; i++) {
        char name[32] = {0};
        name[0] = 'p'; name[1] = 't'; name[2] = 's'; name[3] = '/';
        if (i < 10) {
            name[4] = '0' + i;
        } else {
            name[4] = '0' + (i / 10);
            name[5] = '0' + (i % 10);
        }
        ptys[i].slave_node = make_pty_node(name, &pty_slave_ops, &ptys[i]);
        devfs_register_device(name, VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[i]);
    }
    devfs_register_device("ptmx", VFS_TYPE_CHAR_DEV, &ptmx_ops, NULL);
    devfs_register_device("tty", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
    devfs_register_device("console", VFS_TYPE_CHAR_DEV, &pty_slave_ops, &ptys[0]);
}

int pty_poll_events(vfs_file_t *file, short events) {
    if (!file || !file->node || !file->node->fs_data) return 0;
    pty_t *p = (pty_t *)file->node->fs_data;

    bool is_master = file->node == p->master_node;

    short revents = 0;
    if (events & 4) revents |= 4;

    if (events & 1) {
        if (is_master) {
            if (p->out.head != p->out.tail) revents |= 1;
        } else {
            if (p->in.head != p->in.tail) revents |= 1;
        }
    }
    return revents;
}