#include <fs/signalfd.h>
#include <fs/vfs.h>
#include <mm/malloc.h>
#include <memory.h>
#include <scheduler/scheduler.h>
#include <errno.h>

#define SIGFD_MAX 64

typedef struct {
    bool     used;
    uint64_t mask;
    int      flags;
} sigfd_t;

static sigfd_t g_sfd[SIGFD_MAX];

static int sigfd_read(vfs_file_t *file, void *buf, uint64_t count);
static int sigfd_close(vfs_file_t *file);

static vfs_file_ops_t sfd_fops = {
    .read  = sigfd_read,
    .close = sigfd_close,
};

static sigfd_t *sigfd_from_fd(int fd) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return NULL;
    if (fd < 0 || fd >= VFS_MAX_FD || !p->fd_table->fds[fd]) return NULL;
    vfs_file_t *f = p->fd_table->fds[fd];
    if (!f->node || f->node->file_ops != &sfd_fops) return NULL;
    return (sigfd_t *)f->node->fs_data;
}

static int sigfd_read(vfs_file_t *file, void *buf, uint64_t count) {
    if (count < sizeof(struct signalfd_siginfo_k)) return -EINVAL;
    sigfd_t *s = (sigfd_t *)file->node->fs_data;
    if (!s) return -EBADF;

    process_t *p = get_current_process();
    if (!p) return -ESRCH;

    while (1) {
        uint64_t pend = p->sigpending & s->mask;
        if (pend) {
            int sig = 0;
            for (int i = 1; i < 64; i++)
                if (pend & (1ULL << i)) { sig = i; break; }
            p->sigpending &= ~(1ULL << sig);

            struct signalfd_siginfo_k *si = (struct signalfd_siginfo_k *)buf;
            memset(si, 0, sizeof(*si));
            si->ssi_signo = (uint32_t)sig;
            return (int)sizeof(*si);
        }
        if (file->flags & 0x800) return -EAGAIN;
        schedule_yield();
    }
}

static int sigfd_close(vfs_file_t *file) {
    sigfd_t *s = (sigfd_t *)file->node->fs_data;
    if (s) s->used = false;
    return 0;
}

int signalfd_init(void) {
    memset(g_sfd, 0, sizeof(g_sfd));
    return 0;
}

int sys_signalfd4_impl(int fd, const void *mask, uint64_t sizemask, int flags) {
    (void)sizemask;
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return -EBADF;

    sigfd_t *s = NULL;
    if (fd >= 0) {
        s = sigfd_from_fd(fd);
        if (!s) return -EBADF;
    } else {
        for (int i = 0; i < SIGFD_MAX; i++) {
            if (!g_sfd[i].used) { s = &g_sfd[i]; break; }
        }
        if (!s) return -ENFILE;
        memset(s, 0, sizeof(*s));
        s->used = true;
    }

    if (mask) s->mask = *(const uint64_t *)mask;
    s->flags = flags;

    if (fd >= 0) return fd;

    vfs_node_t *n = malloc_alloc(sizeof(vfs_node_t));
    memset(n, 0, sizeof(*n));
    n->name[0] = 's'; n->name[1] = 'f'; n->name[2] = 'd'; n->name[3] = 0;
    n->type = VFS_TYPE_CHAR_DEV;
    n->file_ops = &sfd_fops;
    n->fs_data = s;

    vfs_file_t *f = malloc_alloc(sizeof(vfs_file_t));
    memset(f, 0, sizeof(*f));
    f->node = n;
    f->flags = VFS_O_RDWR | (uint32_t)flags;
    f->refcount = 1;

    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (!p->fd_table->fds[i]) {
            p->fd_table->fds[i] = f;
            return i;
        }
    }
    malloc_free(f); malloc_free(n);
    return -EMFILE;
}