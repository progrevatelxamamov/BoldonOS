"""Exercise the actual dup/fcntl syscall code with an in-memory descriptor table."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = (root / 'kernel/src/syscall/sys_file.c').read_text()
dispatch = (root / 'kernel/src/syscall/syscall.c').read_text()
assert 'case SYS_DUP:  return sys_dup_impl(a1);' in dispatch
fcntl = src[src.index('uint64_t sys_fcntl_impl('):src.index('static void fill_linux_stat(')]
dup = src[src.index('uint64_t sys_dup_impl('):src.index('uint64_t sys_readlink_impl(')]
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
typedef struct { uint32_t flags; int refcount; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; } process_t;
static process_fd_table_t table;
static process_t proc = { &table };
static process_t *get_current_process(void) { return &proc; }
static int sys_close(process_fd_table_t *t, int fd) {
    t->fds[fd]->refcount--; t->fds[fd] = 0; return 0;
}
''' + fcntl + dup + r'''
int main(void) {
    vfs_file_t out = {VFS_O_WRITE, 1}, other = {VFS_O_READ, 1};
    table.fds[1] = &out; table.fds[3] = &other;
    assert(sys_dup_impl(1) == 0 && table.fds[0] == &out && out.refcount == 2);
    assert(sys_dup_impl(1) == 2 && table.fds[3] == &other);
    assert(sys_dup2_impl(1,3) == 3 && other.refcount == 0 && out.refcount == 4);
    assert(sys_dup2_impl(1,1) == 1 && out.refcount == 4);
    assert((int64_t)sys_dup_impl(7) == -EBADF);
    assert((int64_t)sys_fcntl_impl(1,0,(uint64_t)-1) == -EINVAL);
    assert(sys_fcntl_impl(1,3,0) == 1);
    assert(sys_fcntl_impl(1,4,02000|04000) == 0);
    assert(sys_fcntl_impl(1,3,0) == (1|02000|04000));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp)/'test.c'; exe = Path(tmp)/'test'; c.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('dup and fcntl checks PASS')
