#include <fs/eventfd.h>
#include <fs/vfs.h>
#include <mm/malloc.h>
#include <memory.h>
#include <scheduler/scheduler.h>
#include <errno.h>

#define EVENTFD_MAX 64
#define EFD_CLOEXEC  0x80000
#define EFD_NONBLOCK 0x800

typedef struct {
    bool     used;
    uint64_t counter;
    uint64_t max;
    int      flags;
} eventfd_t;

static eventfd_t g_efd[EVENTFD_MAX];

static int efd_read(vfs_file_t *file, void *buf, uint64_t count);
static int efd_write(vfs_file_t *file, const void *buf, uint64_t count);
static int efd_close(vfs_file_t *file);

static vfs_file_ops_t efd_fops = {
    .read  = efd_read,
    .write = efd_write,
    .close = efd_close,
};

int eventfd_init(void) {
    memset(g_efd, 0, sizeof(g_efd));
    return 0;
}

static int efd_read(vfs_file_t *file, void *buf, uint64_t count) {
    if (count < 8) return -EINVAL;
    eventfd_t *e = (eventfd_t *)file->node->fs_data;
    if (!e) return -EBADF;

    while (1) {
        if (e->counter > 0) {
            uint64_t v = (e->counter == 0xFFFFFFFFFFFFFFFFULL) ? 0xFFFFFFFFFFFFFFFEULL : e->counter;
            *(uint64_t *)buf = v;
            e->counter = 0;
            return 8;
        }
        if (e->flags & EFD_NONBLOCK) return -EAGAIN;
        schedule_yield();
    }
}

static int efd_write(vfs_file_t *file, const void *buf, uint64_t count) {
    if (count < 8) return -EINVAL;
    eventfd_t *e = (eventfd_t *)file->node->fs_data;
    if (!e) return -EBADF;

    uint64_t add = *(const uint64_t *)buf;
    if (add == 0xFFFFFFFFFFFFFFFFULL) return -EINVAL;
    if (e->counter + add < e->counter) return -EAGAIN;
    if (e->counter + add > 0xFFFFFFFFFFFFFFFEULL) {
        if (e->flags & EFD_NONBLOCK) return -EAGAIN;
        while (e->counter + add > 0xFFFFFFFFFFFFFFFEULL) schedule_yield();
    }
    e->counter += add;
    return 8;
}

static int efd_close(vfs_file_t *file) {
    eventfd_t *e = (eventfd_t *)file->node->fs_data;
    if (e) e->used = false;
    return 0;
}

int sys_eventfd2_impl(int initval, int flags) {
    process_t *p = get_current_process();
    if (!p || !p->fd_table) return -EBADF;

    eventfd_t *e = NULL;
    for (int i = 0; i < EVENTFD_MAX; i++) {
        if (!g_efd[i].used) { e = &g_efd[i]; break; }
    }
    if (!e) return -ENFILE;
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->counter = (uint64_t)(uint32_t)initval;
    e->max = 0xFFFFFFFFFFFFFFFEULL;
    e->flags = flags;

    vfs_node_t *n = malloc_alloc(sizeof(vfs_node_t));
    memset(n, 0, sizeof(*n));
    n->name[0] = 'e'; n->name[1] = 'f'; n->name[2] = 'd'; n->name[3] = 0;
    n->type = VFS_TYPE_CHAR_DEV;
    n->file_ops = &efd_fops;
    n->fs_data = e;

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