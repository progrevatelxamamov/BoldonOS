"""Run the real dup/dup2 syscalls, including lowest-free-fd and stdin safety."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
files = (root / 'kernel/src/syscall/sys_file.c').read_text()
dispatch = (root / 'kernel/src/syscall/syscall.c').read_text()
assert 'case SYS_DUP:  return sys_dup_impl(a1);' in dispatch
body = files[files.index('uint64_t sys_dup2_impl('):files.index('uint64_t sys_readlink_impl(')]
code = r'''
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#define VFS_MAX_FD 16
typedef struct { int refcount; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; } process_t;
static process_fd_table_t table;
static process_t process = { &table };
static process_t *get_current_process(void) { return &process; }
static int sys_close(process_fd_table_t *t, int fd) {
    t->fds[fd]->refcount--;
    t->fds[fd] = NULL;
    return 0;
}
''' + body + r'''
int main(void) {
    vfs_file_t stdin_file = {1}, stdout_file = {1}, stderr_file = {1};
    table.fds[0] = &stdin_file;
    table.fds[1] = &stdout_file;
    table.fds[2] = &stderr_file;
    if (sys_dup_impl(2) != 3 || table.fds[0] != &stdin_file ||
        table.fds[3] != &stderr_file || stderr_file.refcount != 2) return 1;
    if (sys_dup_impl(2) != 4 || stderr_file.refcount != 3) return 2;
    if (sys_dup2_impl(3, 1) != 1 || table.fds[1] != &stderr_file ||
        stdout_file.refcount != 0 || stderr_file.refcount != 4) return 3;
    if (sys_dup_impl(16) != (uint64_t)-EBADF || sys_dup_impl(7) != (uint64_t)-EBADF)
        return 4;
    if (sys_dup2_impl(3, 3) != 3 || stderr_file.refcount != 4) return 5;
    for (int i = 5; i < VFS_MAX_FD; i++) table.fds[i] = &stdin_file;
    if (sys_dup_impl(2) != (uint64_t)-EMFILE) return 6;
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'dup.c'
    exe = Path(tmp) / 'dup'
    src.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('dup allocates lowest free fd without replacing stdin; dup2 replaces target')
