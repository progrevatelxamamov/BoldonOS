"""dup/dup2/dup3/fcntl against an in-memory descriptor table.

The production implementations are compiled as-is; only the VFS helpers they
call are replaced. The asserts encode the Linux contract:
  * F_GETFD/F_SETFD own the close-on-exec bit, and it lives on the descriptor,
    never on the shared open file description;
  * dup2/dup3 clear it, F_DUPFD_CLOEXEC sets it;
  * dup3 with equal descriptors is EINVAL (dup2 returns the fd untouched).
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kernel_snip as k

src = k.source('kernel/src/syscall/sys_file.c')
dispatch = k.source('kernel/src/syscall/syscall.c')
assert 'case SYS_DUP:  return sys_dup_impl(a1);' in dispatch
assert 'case 292: return sys_dup3_impl(a1, a2, a3);' in dispatch

fragment = '\n'.join([
    k.defines(src, 'LINUX_F_DUPFD', 'LINUX_F_GETFD', 'LINUX_F_SETFD',
              'LINUX_F_GETFL', 'LINUX_F_SETFL', 'LINUX_F_DUPFD_CLOEXEC',
              'LINUX_FD_CLOEXEC'),
    k.function(src, 'sys_fcntl_impl'),
    k.function(src, 'sys_dup_impl'),
    k.function(src, 'sys_dup2_impl'),
    k.function(src, 'sys_dup3_impl'),
])

code = r'''
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#define VFS_MAX_FD 8
#define VFS_O_READ 1
#define VFS_O_WRITE 2
#define VFS_O_RDWR 3
#define VFS_O_APPEND 16
#define VFS_O_NONBLOCK 32
#define VFS_FD_CLOEXEC 0x1
typedef struct { uint32_t flags; int refcount; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; uint8_t fd_flags[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; } process_t;
static process_fd_table_t table;
static process_t proc = { &table };
static process_t *get_current_process(void) { return &proc; }
static int vfs_fd_install(process_fd_table_t *t, vfs_file_t *f, int min, uint8_t flags) {
    for (int fd = min; fd < VFS_MAX_FD; fd++) {
        if (t->fds[fd]) continue;
        t->fds[fd] = f; t->fd_flags[fd] = flags; f->refcount++;
        return fd;
    }
    return -EMFILE;
}
static int vfs_fd_install_at(process_fd_table_t *t, int fd, vfs_file_t *f, uint8_t flags) {
    if (t->fds[fd]) { t->fds[fd]->refcount--; t->fds[fd] = 0; }
    t->fds[fd] = f; t->fd_flags[fd] = flags; f->refcount++;
    return fd;
}
''' + fragment + r'''
int main(void) {
    vfs_file_t out = {VFS_O_WRITE, 1}, other = {VFS_O_READ, 1};
    table.fds[1] = &out; table.fds[3] = &other;

    /* dup picks the lowest free slot and shares the description. */
    assert(sys_dup_impl(1) == 0 && table.fds[0] == &out && out.refcount == 2);
    assert(sys_dup_impl(1) == 2 && table.fds[2] == &out && out.refcount == 3);
    /* F_DUPFD starts at the requested minimum. */
    assert(sys_fcntl_impl(1, 0, 4) == 4 && table.fds[4] == &out);
    assert((int64_t)sys_fcntl_impl(1, 0, (uint64_t)VFS_MAX_FD) == -EINVAL);
    assert((int64_t)sys_dup_impl(7) == -EBADF);
    assert((int64_t)sys_dup_impl((uint64_t)-1) == -EBADF);

    /* dup2 replaces the target and drops its previous description. */
    int before = other.refcount;
    assert(sys_dup2_impl(1, 3) == 3 && table.fds[3] == &out);
    assert(other.refcount == before - 1);
    assert(sys_dup2_impl(1, 1) == 1);

    /* dup2/dup3 clear close-on-exec, F_DUPFD_CLOEXEC sets it. */
    sys_fcntl_impl(1, 2, 1);              /* F_SETFD FD_CLOEXEC */
    assert(sys_fcntl_impl(1, 1, 0) == 1); /* F_GETFD */
    assert(sys_dup2_impl(1, 5) == 5 && table.fd_flags[5] == 0);
    assert(sys_fcntl_impl(1, 1030, 6) == 6 && table.fd_flags[6] == VFS_FD_CLOEXEC);
    assert(table.fd_flags[1] == VFS_FD_CLOEXEC); /* untouched by the copies */
    /* dup3 flags: only O_CLOEXEC is legal, equal fds are EINVAL. */
    assert(sys_dup3_impl(1, 7, 0) == 7 && table.fd_flags[7] == 0);
    assert((int64_t)sys_dup3_impl(1, 0, 1) == -EINVAL);
    assert((int64_t)sys_dup3_impl(1, 1, 0) == -EINVAL);

    /* F_SETFD only stores FD_CLOEXEC and never touches the shared file. */
    uint32_t file_flags_before = out.flags;
    assert(sys_fcntl_impl(1, 2, 0xffff) == 0);
    assert((table.fd_flags[1] & VFS_FD_CLOEXEC) == VFS_FD_CLOEXEC);
    assert(out.flags == file_flags_before);
    assert(sys_fcntl_impl(1, 2, 0) == 0 && table.fd_flags[1] == 0);

    /* F_GETFL/F_SETFL round-trip the access mode plus O_APPEND/O_NONBLOCK. */
    assert(sys_fcntl_impl(1, 3, 0) == (1 | 0100000));
    assert(sys_fcntl_impl(1, 4, 02000 | 04000) == 0);
    assert(sys_fcntl_impl(1, 3, 0) == (1 | 02000 | 04000 | 0100000));
    /* F_SETFL cannot change the access mode. */
    assert(sys_fcntl_impl(1, 4, 0) == 0);
    assert(sys_fcntl_impl(1, 3, 0) == (1 | 0100000));
    /* Unknown commands report EINVAL instead of faking success. */
    assert((int64_t)sys_fcntl_impl(1, 9, 0) == -EINVAL);
    assert((int64_t)sys_fcntl_impl((uint64_t)-1, 1, 0) == -EBADF);
}
'''

k.compile_and_run(code)
print('dup and fcntl checks PASS')
