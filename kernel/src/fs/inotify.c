#include <fs/inotify.h>
#include <scheduler/scheduler.h>
#include <scheduler/spinlock.h>
#include <mm/malloc.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <errno.h>
#include <string.h>
#include <memory.h>

#define IN_WATCH_LIMIT 64
#define IN_EVENT_LIMIT 64
#define IN_ALL_EVENTS 0x00000fffU

struct in_event {
    int32_t wd;
    uint32_t mask, cookie, len;
    char name[256];
};

typedef struct {
    bool active;
    int32_t wd;
    vfs_mount_t *mount;
    uint64_t inode;
    vfs_node_t *identity; /* fallback for synthetic nodes without inode numbers */
    uint32_t mask;
    uint32_t options;
} in_watch_t;

typedef struct in_instance {
    struct in_instance *next;
    in_watch_t watch[IN_WATCH_LIMIT];
    struct in_event event[IN_EVENT_LIMIT];
    uint32_t head, tail, count;
    int32_t next_wd;
    bool overflow;
} in_instance_t;

static in_instance_t *instances;
static spinlock_t in_lock = SPINLOCK_INIT;
static uint32_t next_move_cookie;

static bool same_node(const in_watch_t *w, const vfs_node_t *node) {
    if (!node || w->mount != node->mount) return false;
    if (w->inode && node->inode_num) return w->inode == node->inode_num;
    return w->identity == node;
}

static void enqueue(in_instance_t *in, int32_t wd, uint32_t mask,
                    uint32_t cookie, const char *name) {
    if (in->count == IN_EVENT_LIMIT) { in->overflow = true; return; }
    struct in_event *ev = &in->event[in->tail];
    ev->wd = wd; ev->mask = mask; ev->cookie = cookie; ev->len = 0;
    if (name && *name) {
        size_t n = 0;
        while (name[n] && n < 255) n++;
        ev->len = (uint32_t)((n + 1 + 3) & ~3U);
        memset(ev->name, 0, ev->len);
        memcpy(ev->name, name, n);
    }
    in->tail = (in->tail + 1) % IN_EVENT_LIMIT;
    in->count++;
}

void inotify_vfs_event(vfs_node_t *node, uint32_t mask, const char *name) {
    if (!node) return;
    uint64_t irq;
    spin_lock_irqsave(&in_lock, &irq);
    for (in_instance_t *in = instances; in; in = in->next) {
        for (int i = 0; i < IN_WATCH_LIMIT; i++) {
            in_watch_t *w = &in->watch[i];
            if (!w->active || !same_node(w, node)) continue;
            bool matched = (w->mask & mask) != 0;
            if (matched) enqueue(in, w->wd, mask, 0, name);
            if ((mask & IN_DELETE_SELF) || (matched && (w->options & IN_ONESHOT))) {
                enqueue(in, w->wd, IN_IGNORED, 0, NULL);
                w->active = false;
            }
        }
    }
    spin_unlock_irqrestore(&in_lock, irq);
}

void inotify_vfs_unmount(vfs_mount_t *mount) {
    if (!mount) return;
    uint64_t irq;
    spin_lock_irqsave(&in_lock, &irq);
    for (in_instance_t *in = instances; in; in = in->next) {
        for (int i = 0; i < IN_WATCH_LIMIT; i++) {
            in_watch_t *w = &in->watch[i];
            if (!w->active || w->mount != mount) continue;
            enqueue(in, w->wd, IN_UNMOUNT, 0, NULL);
            enqueue(in, w->wd, IN_IGNORED, 0, NULL);
            w->active = false;
        }
    }
    spin_unlock_irqrestore(&in_lock, irq);
}

void inotify_vfs_move(vfs_node_t *old_dir, const char *old_name,
                      vfs_node_t *new_dir, const char *new_name,
                      vfs_node_t *moved) {
    if (!old_dir || !new_dir || !moved) return;
    uint32_t dir = moved->type == VFS_TYPE_DIR ? IN_ISDIR : 0;
    uint64_t irq;
    spin_lock_irqsave(&in_lock, &irq);
    uint32_t cookie = ++next_move_cookie;
    if (!cookie) cookie = ++next_move_cookie;
    for (in_instance_t *in = instances; in; in = in->next) {
        for (int i = 0; i < IN_WATCH_LIMIT; i++) {
            in_watch_t *w = &in->watch[i];
            if (!w->active) continue;
            uint32_t mask = 0;
            const char *name = NULL;
            if (same_node(w, old_dir) && (w->mask & IN_MOVED_FROM)) {
                mask = IN_MOVED_FROM | dir; name = old_name;
                enqueue(in, w->wd, mask, cookie, name);
                if (w->options & IN_ONESHOT) {
                    enqueue(in, w->wd, IN_IGNORED, 0, NULL);
                    w->active = false;
                    continue;
                }
            }
            if (same_node(w, new_dir) && (w->mask & IN_MOVED_TO)) {
                mask = IN_MOVED_TO | dir; name = new_name;
                enqueue(in, w->wd, mask, cookie, name);
                if (w->options & IN_ONESHOT) {
                    enqueue(in, w->wd, IN_IGNORED, 0, NULL);
                    w->active = false;
                    continue;
                }
            }
            if (same_node(w, moved) && (w->mask & IN_MOVE_SELF)) {
                enqueue(in, w->wd, IN_MOVE_SELF, 0, NULL);
                mask = IN_MOVE_SELF;
            }
            if (mask && (w->options & IN_ONESHOT)) {
                enqueue(in, w->wd, IN_IGNORED, 0, NULL);
                w->active = false;
            }
        }
    }
    spin_unlock_irqrestore(&in_lock, irq);
}

static uint32_t in_poll(vfs_file_t *file, uint32_t events) {
    in_instance_t *in = file->private_data;
    uint64_t irq;
    spin_lock_irqsave(&in_lock, &irq);
    uint32_t ready = ((events & 1) && (in->count || in->overflow)) ? 1 : 0;
    spin_unlock_irqrestore(&in_lock, irq);
    return ready;
}

static int in_read(vfs_file_t *file, void *buf, uint64_t len) {
    in_instance_t *in = file->private_data;
    if (!len) return 0;
    if (len < 16) return -EINVAL;
    uint32_t done = 0;
    for (;;) {
        uint64_t irq;
        spin_lock_irqsave(&in_lock, &irq);
        if (in->count || in->overflow) {
            struct in_event overflow = { .wd = -1, .mask = IN_Q_OVERFLOW };
            struct in_event *ev = in->count ? &in->event[in->head] : &overflow;
            uint32_t size = 16 + ev->len;
            if (size > len - done) {
                spin_unlock_irqrestore(&in_lock, irq);
                return done ? (int)done : -EINVAL;
            }
            memcpy((char *)buf + done, ev, size);
            done += size;
            if (in->count) {
                in->head = (in->head + 1) % IN_EVENT_LIMIT;
                in->count--;
            } else in->overflow = false;
            spin_unlock_irqrestore(&in_lock, irq);
            if (done >= len || done > 0x7fffffffU - sizeof(struct in_event)) return (int)done;
            continue;
        }
        spin_unlock_irqrestore(&in_lock, irq);
        if (done) return (int)done;
        if (file->flags & VFS_O_NONBLOCK) return -EAGAIN;
        process_t *p = get_current_process();
        if (p && (p->sigpending & ~p->sigmask)) return -EINTR;
        schedule_yield();
    }
}

static bool user_range(uint64_t p, uint64_t len);

static int in_ioctl(vfs_file_t *file, uint64_t request, void *arg) {
    if (request != 0x541B /* FIONREAD */) return -ENOTTY;
    if (!user_range((uint64_t)arg, sizeof(int))) return -EFAULT;
    in_instance_t *in = file->private_data;
    uint64_t irq;
    spin_lock_irqsave(&in_lock, &irq);
    uint32_t bytes = in->overflow ? 16 : 0;
    for (uint32_t i = 0; i < in->count; i++)
        bytes += 16 + in->event[(in->head + i) % IN_EVENT_LIMIT].len;
    spin_unlock_irqrestore(&in_lock, irq);
    *(int *)arg = (int)bytes;
    return 0;
}

static int in_close(vfs_file_t *file) {
    in_instance_t *in = file->private_data;
    uint64_t irq;
    spin_lock_irqsave(&in_lock, &irq);
    in_instance_t **link = &instances;
    while (*link && *link != in) link = &(*link)->next;
    if (*link) *link = in->next;
    spin_unlock_irqrestore(&in_lock, irq);
    malloc_free(in);
    malloc_free(file->node);
    file->node = NULL;
    return 0;
}

static vfs_file_ops_t in_ops = {
    .read = in_read, .poll = in_poll, .ioctl = in_ioctl, .close = in_close
};

static bool user_range(uint64_t p, uint64_t len) {
    if (p < 0x10000 || p > 0x00007fffffffffffULL ||
        len > 0x0000800000000000ULL - p) return false;
    process_t *proc = get_current_process();
    if (proc && proc->pml4) {
        uint64_t first = p & ~4095ULL;
        uint64_t end = (p + len + 4095) & ~4095ULL;
        for (uint64_t page = first; page < end; page += 4096) {
            if (!vmm_is_mapped(proc->pml4, page)) {
                void *phys = pmm_alloc_page_and_clear();
                if (!phys) return false;
                vmm_map_page(proc->pml4, (uint64_t)phys, page,
                             PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            }
        }
    }
    return true;
}

uint64_t sys_inotify_init1_impl(uint64_t flags) {
    if (flags & ~(04000ULL | 02000000ULL)) return (uint64_t)-EINVAL;
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return (uint64_t)-EBADF;
    int fd = 0;
    while (fd < VFS_MAX_FD && p->fd_table->fds[fd]) fd++;
    if (fd == VFS_MAX_FD) return (uint64_t)-EMFILE;
    in_instance_t *in = malloc_alloc(sizeof(*in));
    vfs_node_t *node = malloc_alloc(sizeof(*node));
    vfs_file_t *file = malloc_alloc(sizeof(*file));
    if (!in || !node || !file) {
        malloc_free(in); malloc_free(node); malloc_free(file);
        return (uint64_t)-ENOMEM;
    }
    memset(in, 0, sizeof(*in));
    memset(node, 0, sizeof(*node));
    memset(file, 0, sizeof(*file));
    node->type = VFS_TYPE_CHAR_DEV;
    node->file_ops = &in_ops;
    file->node = node;
    file->private_data = in;
    file->refcount = 1;
    file->flags = VFS_O_READ |
        ((flags & 04000) ? VFS_O_NONBLOCK : 0) |
        ((flags & 02000000) ? VFS_O_CLOEXEC : 0);
    uint64_t irq;
    spin_lock_irqsave(&in_lock, &irq);
    in->next = instances;
    instances = in;
    p->fd_table->fds[fd] = file;
    spin_unlock_irqrestore(&in_lock, irq);
    return (uint64_t)fd;
}

static in_instance_t *instance_fd(uint64_t fd) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table || fd >= VFS_MAX_FD) return NULL;
    vfs_file_t *file = p->fd_table->fds[fd];
    return file && file->node && file->node->file_ops == &in_ops ? file->private_data : NULL;
}

uint64_t sys_inotify_add_watch_impl(uint64_t fd, uint64_t path, uint64_t mask_arg) {
    in_instance_t *in = instance_fd(fd);
    if (!in) return (uint64_t)-EBADF;
    if (!user_range(path, 1)) return (uint64_t)-EFAULT;
    char name[512];
    unsigned i = 0;
    for (; i < sizeof(name) - 1; i++) {
        if (!user_range(path + i, 1)) return (uint64_t)-EFAULT;
        name[i] = *(const char *)(path + i);
        if (!name[i]) break;
    }
    if (i == sizeof(name) - 1) return (uint64_t)-ENAMETOOLONG;
    uint32_t mask = (uint32_t)mask_arg;
    if ((mask_arg >> 32) || !(mask & IN_ALL_EVENTS)) return (uint64_t)-EINVAL;
    /* Parent watches only receive events on directory entries while they
       are linked; no subsequent child events are routed to the parent.
       Thus IN_EXCL_UNLINK requires no additional filtering in this VFS. */
    vfs_node_t *node = (mask & IN_DONT_FOLLOW) ? vfs_resolve_path_nofollow(name)
                                                 : vfs_resolve_path(name);
    if (!node) return (uint64_t)-ENOENT;
    if ((mask & IN_ONLYDIR) && node->type != VFS_TYPE_DIR) return (uint64_t)-ENOTDIR;
    uint64_t irq;
    spin_lock_irqsave(&in_lock, &irq);
    in_watch_t *free_slot = NULL;
    for (int j = 0; j < IN_WATCH_LIMIT; j++) {
        in_watch_t *w = &in->watch[j];
        if (!w->active) { if (!free_slot) free_slot = w; continue; }
        if (!same_node(w, node)) continue;
        if (mask & IN_MASK_CREATE) {
            spin_unlock_irqrestore(&in_lock, irq);
            return (uint64_t)-EEXIST;
        }
        w->mask = (mask & IN_MASK_ADD) ? (w->mask | (mask & IN_ALL_EVENTS)) : (mask & IN_ALL_EVENTS);
        w->options = mask & IN_ONESHOT;
        int wd = w->wd;
        spin_unlock_irqrestore(&in_lock, irq);
        return (uint64_t)wd;
    }
    if (!free_slot || in->next_wd == 0x7fffffff) {
        spin_unlock_irqrestore(&in_lock, irq);
        return (uint64_t)-ENOSPC;
    }
    *free_slot = (in_watch_t) { .active = true, .wd = ++in->next_wd,
        .mount = node->mount, .inode = node->inode_num,
        .identity = node, .mask = mask & IN_ALL_EVENTS,
        .options = mask & IN_ONESHOT };
    int wd = free_slot->wd;
    spin_unlock_irqrestore(&in_lock, irq);
    return (uint64_t)wd;
}

uint64_t sys_inotify_rm_watch_impl(uint64_t fd, uint64_t wd_arg) {
    in_instance_t *in = instance_fd(fd);
    if (!in) return (uint64_t)-EBADF;
    uint64_t irq;
    spin_lock_irqsave(&in_lock, &irq);
    for (int i = 0; i < IN_WATCH_LIMIT; i++) {
        in_watch_t *w = &in->watch[i];
        if (!w->active || w->wd != (int32_t)wd_arg) continue;
        int wd = w->wd;
        w->active = false;
        enqueue(in, wd, IN_IGNORED, 0, NULL);
        spin_unlock_irqrestore(&in_lock, irq);
        return 0;
    }
    spin_unlock_irqrestore(&in_lock, irq);
    return (uint64_t)-EINVAL;
}
