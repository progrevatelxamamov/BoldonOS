"""Compile production newfstatat and verify libc's fd-based stat path."""
from pathlib import Path
import subprocess
import tempfile
src = (Path(__file__).resolve().parents[1] / 'kernel/src/syscall/sys_file.c').read_text()
body = src[src.index('uint64_t sys_newfstatat_impl('):src.index('uint64_t sys_statx_impl(')]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define VFS_MAX_FD 8
typedef struct { int id; } vfs_node_t;
typedef struct { vfs_node_t *node; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; } process_t;
struct linux_stat { int id; };
static process_fd_table_t table;
static process_t proc = { &table };
static vfs_node_t cwd = { 1 }, node = { 2 }, linknode = { 3 };
static process_t *get_current_process(void) { return &proc; }
static bool user_range_ok(uint64_t p, size_t n) { return p && n; }
static int copy_str_from_user(char *dest, uint64_t p, size_t n) {
    if (!p || strlen((char *)p) >= n) return -EFAULT;
    strcpy(dest, (char *)p); return 0;
}
static vfs_node_t *vfs_resolve_path(const char *p) { (void)p; return &cwd; }
static vfs_node_t *vfs_resolve_path_nofollow(const char *p) { (void)p; return &linknode; }
static void fill_linux_stat(vfs_node_t *n, struct linux_stat *s) { s->id = n->id; }
''' + body + r'''
int main(void) {
    struct linux_stat st; vfs_file_t f = { &node }; table.fds[3] = &f;
    assert(sys_newfstatat_impl(3,(uint64_t)"",(uint64_t)&st,0x1000) == 0 && st.id == 2);
    assert(sys_newfstatat_impl((uint32_t)-100,(uint64_t)"",(uint64_t)&st,0x1800) == 0 && st.id == 1);
    assert((int64_t)sys_newfstatat_impl(7,(uint64_t)"",(uint64_t)&st,0x1000) == -EBADF);
    assert((int64_t)sys_newfstatat_impl(3,(uint64_t)"",(uint64_t)&st,0) == -ENOENT);
    assert(sys_newfstatat_impl((uint32_t)-100,(uint64_t)".",(uint64_t)&st,0x900) == 0 && st.id == 3);
    assert((int64_t)sys_newfstatat_impl(3,(uint64_t)"relative",(uint64_t)&st,0) == -ENOSYS);
    assert((int64_t)sys_newfstatat_impl(3,0,(uint64_t)&st,0x1000) == -EFAULT);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'test.c'; exe = Path(tmp) / 'test'; c.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('newfstatat AT_EMPTY_PATH checks PASS')
