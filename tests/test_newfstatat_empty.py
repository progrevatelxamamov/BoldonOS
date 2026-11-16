"""newfstatat(2): AT_EMPTY_PATH, AT_SYMLINK_NOFOLLOW and dirfd-relative names.

The production `sys_newfstatat_impl` is compiled against a fake VFS so the test
asserts exactly which node ends up in the stat buffer for each flag
combination, including the -ENOTDIR/-EBADF branches used by libc.
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kernel_snip as k

src = k.source('kernel/src/syscall/sys_file.c')
fragment = '\n'.join([
    k.defines(src, 'LINUX_AT_FDCWD', 'LINUX_AT_SYMLINK_NOFOLLOW',
              'LINUX_AT_NO_AUTOMOUNT', 'LINUX_AT_EMPTY_PATH'),
    k.function(src, 'fdt_file'),
    k.function(src, 'resolve_dirfd_prefix'),
    k.function(src, 'resolve_at_path'),
    k.function(src, 'sys_newfstatat_impl'),
])

code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define VFS_MAX_FD 8
#define VFS_PERM_WRITE 2
typedef enum { VFS_TYPE_FILE, VFS_TYPE_DIR, VFS_TYPE_SYMLINK } vfs_node_type_t;
typedef struct { int id; char name[256]; vfs_node_type_t type; } vfs_node_t;
typedef struct { vfs_node_t *node; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; uint32_t fsuid, fsgid; } process_t;
struct linux_stat { int id; };
static process_fd_table_t table;
static process_t proc = { &table, 0, 0 };
static vfs_node_t cwd = { 1, "", VFS_TYPE_DIR };
static vfs_node_t node = { 2, "/etc/passwd", VFS_TYPE_FILE };
static vfs_node_t linknode = { 3, "/etc/link", VFS_TYPE_SYMLINK };
static vfs_node_t dirnode = { 4, "/tmp/work", VFS_TYPE_DIR };
static process_t *get_current_process(void) { return &proc; }
static void fill_linux_stat(vfs_node_t *n, struct linux_stat *s) { s->id = n->id; }
static int copy_str_from_user(char *dst, uint64_t src, size_t cap) {
    if (!src) return -EFAULT;
    size_t len = strlen((const char *)src);
    if (len + 1 > cap) return -ENAMETOOLONG;
    memcpy(dst, (const char *)src, len + 1);
    return 0;
}
static bool user_range_ok(uint64_t p, uint64_t n) { return p && n; }
static vfs_node_t *vfs_resolve_path(const char *p) {
    /* "." from cwd, everything else resolves to the plain file node. */
    return (p && p[0] == '.' && !p[1]) ? &cwd : &node;
}
static vfs_node_t *vfs_resolve_path_nofollow(const char *p) {
    (void)p; return &linknode;
}
''' + fragment + r'''
int main(void) {
    struct linux_stat st;
    static vfs_file_t fnode, fdir, fmissing;
    fnode.node = &node; fdir.node = &dirnode; fmissing.node = 0;
    table.fds[3] = &fnode; table.fds[4] = &fdir; table.fds[6] = &fmissing;

    /* AT_EMPTY_PATH on a descriptor stats the descriptor itself. */
    assert(sys_newfstatat_impl(3, (uint64_t)"", (uint64_t)&st, 0x1000) == 0 &&
           st.id == 2);
    /* AT_EMPTY_PATH with AT_FDCWD stats the cwd. */
    assert(sys_newfstatat_impl((uint32_t)-100, (uint64_t)"", (uint64_t)&st,
                               0x1800) == 0 && st.id == 1);
    /* A missing descriptor is EBADF even for the empty path. */
    assert((int64_t)sys_newfstatat_impl(7, (uint64_t)"", (uint64_t)&st,
                                        0x1000) == -EBADF);
    assert((int64_t)sys_newfstatat_impl(6, (uint64_t)"", (uint64_t)&st,
                                        0x1000) == -EBADF);
    /* An empty path without AT_EMPTY_PATH is ENOENT. */
    assert((int64_t)sys_newfstatat_impl(3, (uint64_t)"", (uint64_t)&st, 0) == -ENOENT);

    /* Normal paths still resolve through the VFS. */
    assert(sys_newfstatat_impl((uint32_t)-100, (uint64_t)"/etc/passwd",
                               (uint64_t)&st, 0) == 0 && st.id == 2);
    /* AT_SYMLINK_NOFOLLOW returns the link node instead of the target. */
    assert(sys_newfstatat_impl((uint32_t)-100, (uint64_t)"/etc/link",
                               (uint64_t)&st, 0x100) == 0 && st.id == 3);

    /* Relative names are now resolved against the descriptor directory. */
    assert(sys_newfstatat_impl(4, (uint64_t)"child", (uint64_t)&st, 0) == 0 &&
           st.id == 2);
    /* A file descriptor can never anchor a relative name. */
    assert((int64_t)sys_newfstatat_impl(3, (uint64_t)"relative",
                                        (uint64_t)&st, 0) == -ENOTDIR);

    /* Unknown flags are rejected, not silently ignored. */
    assert((int64_t)sys_newfstatat_impl(3, (uint64_t)"x", (uint64_t)&st,
                                        0x2) == -EINVAL);
    assert((int64_t)sys_newfstatat_impl(3, 0, (uint64_t)&st,
                                        0x1000) == -EFAULT);
    /* A null stat buffer must never be written. */
    assert((int64_t)sys_newfstatat_impl(3, (uint64_t)"", 0, 0x1000) == -EFAULT);
}
'''

k.compile_and_run(code)
print('newfstatat AT_EMPTY_PATH checks PASS')
