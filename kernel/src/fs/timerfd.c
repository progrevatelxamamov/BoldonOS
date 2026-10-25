#include <fs/timerfd.h>
#include <fs/vfs.h>
#include <mm/malloc.h>
#include <memory.h>
#include <scheduler/scheduler.h>
#include <cpu/time.h>
#include <errno.h>

#define TIMERFD_MAX 64

typedef struct {
    bool     used;
    int      clockid;
    int      flags;
    uint64_t interval_ns;
    uint64_t next_expiry_ns;
    uint64_t counter;
    bool     active;
} timerfd_t;

static timerfd_t g_tfd[TIMERFD_MAX];

static uint64_t now_ns(void) {
    return get_unix_time() * 1000000000ULL;
}

static int timerfd_read(vfs_file_t *file, void *buf, uint64_t count);
static int timerfd_close(vfs_file_t *file);
static int timerfd_ioctl(vfs_file_t *file, uint64_t request, void *arg);

static vfs_file_ops_t tfd_fops = {
    .read  = timerfd_read,
    .close = timerfd_close,
    .ioctl = timerfd_ioctl,
};

static timerfd_t *timerfd_from_fd(int fd, process_t *p) {
    if (!p || !p->fd_table) return NULL;
    if (fd < 0 || fd >= VFS_MAX_FD || !p->fd_table->fds[fd]) return NULL;
    vfs_file_t *f = p->fd_table->fds[fd];
    if (!f->node || f->node->file_ops != &tfd_fops) return NULL;
    return (timerfd_t *)f->node->fs_data;
}

static void timerfd_update(timerfd_t *t) {
    if (!t->active) return;
    uint64_t now = now_ns();
    while (now >= t->next_expiry_ns) {
        t->counter++;
        if (t->interval_ns == 0) {
            t->active = false;
            break;
        }
        t->next_expiry_ns += t->interval_ns;
    }
}

static int timerfd_read(vfs_file_t *file, void *buf, uint64_t count) {
    if (count < 8) return -EINVAL;
    timerfd_t *t = (timerfd_t *)file->node->fs_data;
    if (!t) return -EBADF;

    while (1) {
        timerfd_update(t);
        if (t->counter > 0) {
            *(uint64_t *)buf = t->counter;
            t->counter = 0;
            return 8;
        }
        if (!t->active) {
            if (file->flags & 0x800) return -EAGAIN;
        }
        schedule_yield();
    }
}

static int timerfd_close(vfs_file_t *file) {
    timerfd_t *t = (timerfd_t *)file->node->fs_data;
    if (t) t->used = false;
    return 0;
}

static int timerfd_ioctl(vfs_file_t *file, uint64_t request, void *arg) {
    (void)file; (void)request; (void)arg;
    return -EINVAL;
}

int timerfd_init(void) {
    memset(g_tfd, 0, sizeof(g_tfd));
    return 0;
}

int sys_timerfd_create_impl(int clockid, int flags) {
    (void)clockid;
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return -EBADF;

    timerfd_t *t = NULL;
    for (int i = 0; i < TIMERFD_MAX; i++) {
        if (!g_tfd[i].used) { t = &g_tfd[i]; break; }
    }
    if (!t) return -ENFILE;
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->clockid = clockid;
    t->flags = flags;

    vfs_node_t *n = malloc_alloc(sizeof(vfs_node_t));
    memset(n, 0, sizeof(*n));
    n->name[0] = 't'; n->name[1] = 'f'; n->name[2] = 'd'; n->name[3] = 0;
    n->type = VFS_TYPE_CHAR_DEV;
    n->file_ops = &tfd_fops;
    n->fs_data = t;

    vfs_file_t *f = malloc_alloc(sizeof(vfs_file_t));
    memset(f, 0, sizeof(*f));
    f->node = n;
    f->flags = VFS_O_RDWR | (uint32_t)flags;
    f->refcount = 1;

    for (int fd = 0; fd < VFS_MAX_FD; fd++) {
        if (!p->fd_table->fds[fd]) {
            p->fd_table->fds[fd] = f;
            return fd;
        }
    }
    malloc_free(f); malloc_free(n);
    return -EMFILE;
}

static uint64_t ts_to_ns(const struct timespec_k *ts) {
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static void ns_to_ts(uint64_t ns, struct timespec_k *ts) {
    ts->tv_sec  = (int64_t)(ns / 1000000000ULL);
    ts->tv_nsec = (int64_t)(ns % 1000000000ULL);
}

int sys_timerfd_settime_impl(int fd, int flags,
                             const struct itimerspec_k *new_val,
                             struct itimerspec_k *old_val)
{
    process_t *p = get_current_process();
    timerfd_t *t = timerfd_from_fd(fd, p);
    if (!t) return -EBADF;
    if (!new_val) return -EINVAL;

    if (old_val) {
        uint64_t rem = 0;
        if (t->active && t->next_expiry_ns > now_ns())
            rem = t->next_expiry_ns - now_ns();
        ns_to_ts(rem, &old_val->it_value);
        ns_to_ts(t->interval_ns, &old_val->it_interval);
    }

    uint64_t val_ns = ts_to_ns(&new_val->it_value);
    uint64_t int_ns = ts_to_ns(&new_val->it_interval);

    t->interval_ns = int_ns;
    if (val_ns == 0) {
        t->active = false;
    } else {
        t->active = true;
        if (flags & 1) t->next_expiry_ns = val_ns;
        else           t->next_expiry_ns = now_ns() + val_ns;
    }
    return 0;
}

int sys_timerfd_gettime_impl(int fd, struct itimerspec_k *cur) {
    process_t *p = get_current_process();
    timerfd_t *t = timerfd_from_fd(fd, p);
    if (!t) return -EBADF;
    if (!cur) return -EINVAL;

    uint64_t rem = 0;
    if (t->active && t->next_expiry_ns > now_ns())
        rem = t->next_expiry_ns - now_ns();
    ns_to_ts(rem, &cur->it_value);
    ns_to_ts(t->interval_ns, &cur->it_interval);
    return 0;
}