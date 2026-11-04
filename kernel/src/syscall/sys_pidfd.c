#include <stdint.h>
#include <fs/vfs.h>
#include <scheduler/scheduler.h>
#include <mm/malloc.h>
#include <errno.h>
#include <memory.h>

/* A pidfd contains the monotonically allocated PID, never a dangling pointer
   to a process_t (wait4 can reap and free that object). */
typedef struct { uint64_t pid; } pidfd_identity_t;

static int pidfd_read(vfs_file_t *file, void *buf, uint64_t count) {
    (void)file; (void)buf; (void)count;
    return -EINVAL; /* pidfds are pollable, not readable */
}

static uint32_t pidfd_poll(vfs_file_t *file, uint32_t events) {
    pidfd_identity_t *id = file->private_data;
    int status = scheduler_pid_status(id->pid, NULL);
    if (status == 0) return 0;
    return ((events & 1) ? 1U : 0U) |
           ((status < 0) ? 0x10U /* POLLHUP after reap */ : 0U);
}

static int pidfd_close(vfs_file_t *file) {
    malloc_free(file->private_data);
    file->private_data = NULL;
    malloc_free(file->node);
    file->node = NULL;
    return 0;
}

static vfs_file_ops_t pidfd_ops = {
    .read = pidfd_read, .poll = pidfd_poll, .close = pidfd_close
};

uint64_t sys_pidfd_open_impl(uint64_t pid, uint64_t flags) {
    /* Linux PIDFD_NONBLOCK == O_NONBLOCK; no PIDFD_THREAD semantics yet. */
    if (flags & ~04000ULL) return (uint64_t)-EINVAL;
    if (!pid || pid > 0x7fffffffULL) return (uint64_t)-EINVAL;
    process_t *self = get_current_process();
    if (!self || !self->fd_table) return (uint64_t)-EBADF;
    if (scheduler_pid_status(pid, NULL) < 0) return (uint64_t)-ESRCH;

    int fd = 0;
    while (fd < VFS_MAX_FD && self->fd_table->fds[fd]) fd++;
    if (fd == VFS_MAX_FD) return (uint64_t)-EMFILE;

    pidfd_identity_t *id = malloc_alloc(sizeof(*id));
    vfs_node_t *node = malloc_alloc(sizeof(*node));
    vfs_file_t *file = malloc_alloc(sizeof(*file));
    if (!id || !node || !file) {
        malloc_free(id); malloc_free(node); malloc_free(file);
        return (uint64_t)-ENOMEM;
    }
    memset(node, 0, sizeof(*node));
    memset(file, 0, sizeof(*file));
    id->pid = pid;
    node->type = VFS_TYPE_CHAR_DEV;
    node->file_ops = &pidfd_ops;
    file->node = node;
    file->private_data = id;
    file->refcount = 1;
    file->flags = VFS_O_READ | VFS_O_CLOEXEC |
                  ((flags & 04000) ? VFS_O_NONBLOCK : 0);
    /* An exited task may have been reaped during allocation. PID numbers
       are not reused, so the descriptor still unambiguously signals exit. */
    self->fd_table->fds[fd] = file;
    return (uint64_t)fd;
}
