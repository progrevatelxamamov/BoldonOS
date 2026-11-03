"""Exercise the real pidfd and capget functions against a host FD/process mock."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
pidfd = (root / 'kernel/src/syscall/sys_pidfd.c').read_text()
pidfd = '\n'.join(line for line in pidfd.splitlines() if not line.startswith('#include'))
proc = (root / 'kernel/src/syscall/sys_proc.c').read_text()
cap = proc[proc.index('/* capget(125) reports'):proc.index('#define IA32_FS_BASE', proc.index('/* capget(125) reports'))]
dispatch = (root / 'kernel/src/syscall/syscall.c').read_text()
for label in ('SYS_CAPGET', 'SYS_PIDFD_OPEN'):
    assert f'case {label}:' in dispatch

code = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#define VFS_MAX_FD 256
#define VFS_O_READ 1
#define VFS_O_NONBLOCK 32
#define VFS_O_CLOEXEC 64
#define VFS_TYPE_CHAR_DEV 5
typedef struct vfs_node vfs_node_t;
typedef struct vfs_file vfs_file_t;
typedef struct { int (*read)(vfs_file_t *, void *, uint64_t); uint32_t (*poll)(vfs_file_t *, uint32_t); int (*close)(vfs_file_t *); } vfs_file_ops_t;
struct vfs_node { int type; vfs_file_ops_t *file_ops; };
struct vfs_file { vfs_node_t *node; void *private_data; uint32_t flags, refcount; };
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; uint32_t euid; } process_t;
static process_fd_table_t table;
static process_t self = { .fd_table = &table, .euid = 0 };
static int state = 0;
static uint32_t target_uid = 0;
static int scheduler_pid_status(uint64_t pid, uint32_t *euid) {
    if (pid != 77 || state < 0) return -1;
    if (euid) *euid = target_uid;
    return state;
}
static process_t *get_current_process(void) { return &self; }
static void *malloc_alloc(uint64_t n) { return malloc(n); }
static void malloc_free(void *p) { free(p); }
static bool user_range_ok(uint64_t ptr, uint64_t len) { return ptr >= 0x10000 && len <= 1000; }
''' + pidfd + cap + r'''
int main(void) {
    assert(sys_pidfd_open_impl(0, 0) == (uint64_t)-EINVAL);
    assert(sys_pidfd_open_impl(77, 1) == (uint64_t)-EINVAL);
    assert(sys_pidfd_open_impl(999, 0) == (uint64_t)-ESRCH);
    int fd = (int)sys_pidfd_open_impl(77, 04000);
    assert(fd >= 0 && fd < VFS_MAX_FD);
    vfs_file_t *f = table.fds[fd];
    assert(f->flags & VFS_O_CLOEXEC);
    assert(f->flags & VFS_O_NONBLOCK);
    assert(f->node->file_ops->poll(f, 1) == 0);
    state = 1;
    assert(f->node->file_ops->poll(f, 1) == 1);
    assert(f->node->file_ops->poll(f, 4) == 0);
    state = -1;
    assert(f->node->file_ops->poll(f, 1) == 0x11);
    assert(f->node->file_ops->read(f, NULL, 0) == -EINVAL);
    f->node->file_ops->close(f); free(f); table.fds[fd] = NULL;
    assert(sys_pidfd_open_impl(77, 0) == (uint64_t)-ESRCH);
    state = 0;
    vfs_file_t busy = {0};
    for (int j=0; j<VFS_MAX_FD; j++) table.fds[j] = &busy;
    assert(sys_pidfd_open_impl(77, 0) == (uint64_t)-EMFILE);
    for (int j=0; j<VFS_MAX_FD; j++) table.fds[j] = NULL;

    struct linux_cap_header h = { .version = 0, .pid = 0 };
    struct linux_cap_data data[2];
    assert(sys_capget_impl((uintptr_t)&h, (uintptr_t)data) == (uint64_t)-EINVAL);
    assert(h.version == 0x20080522U);
    assert(sys_capget_impl((uintptr_t)&h, (uintptr_t)data) == 0);
    assert(data[0].effective == ((1U<<1)|(1U<<6)|(1U<<7)));
    assert(data[0].permitted == data[0].effective && !data[0].inheritable);
    assert(data[1].effective == 0 && data[1].permitted == 0);
    h.version = 0x20071026U;
    assert(sys_capget_impl((uintptr_t)&h, (uintptr_t)data) == 0);
    assert(data[0].effective != 0 && data[1].effective == 0);
    self.euid = 1000;
    h.version = 0x19980330U;
    assert(sys_capget_impl((uintptr_t)&h, (uintptr_t)data) == 0);
    assert(!data[0].effective && !data[0].permitted);
    h.version = 0x20080522U;
    h.pid = 77;
    state = 0; target_uid = 0;
    assert(sys_capget_impl((uintptr_t)&h, (uintptr_t)data) == 0);
    assert(data[0].effective != 0);
    state = -1;
    assert(sys_capget_impl((uintptr_t)&h, (uintptr_t)data) == (uint64_t)-ESRCH);
    h.pid = -1;
    assert(sys_capget_impl((uintptr_t)&h, (uintptr_t)data) == (uint64_t)-EINVAL);
    puts("pidfd and capget: validation, poll lifecycle, ABI versions and UID rights passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-o', str(exe), str(src)], check=True)
    subprocess.run([str(exe)], check=True)
