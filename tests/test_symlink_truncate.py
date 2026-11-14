"""symlink(2), symlinkat(2), linkat(2), readlink(2), readlinkat(2), truncate(2).

These were previously unreachable: SYS_SYMLINK had no dispatch entry, readlink
resolved the final component (so it always followed the link), and truncate(2)
answered -ENOSYS. The test drives the real implementations against a fake VFS
and a fake descriptor table.
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kernel_snip as k

src = k.source('kernel/src/syscall/sys_file.c')
dispatch = k.source('kernel/src/syscall/syscall.c')
for line in ['case 88:  return sys_symlink_impl(a1, a2);',
             'case 266: return sys_symlinkat_impl(a1, a2, a3);',
             'case 267: return sys_readlinkat_impl(a1, a2, a3, a4);',
             'case 265: return sys_linkat_impl(a1, a2, a3, a4, a5);',
             'case 92:  return sys_truncate_impl(a1, a2);']:
    assert line in dispatch, line

fragment = '\n'.join([
    k.defines(src, 'LINUX_AT_FDCWD', 'LINUX_AT_SYMLINK_FOLLOW'),
    k.function(src, 'copy_str_from_user'),
    k.function(src, 'fdt_file'),
    k.function(src, 'resolve_dirfd_prefix'),
    k.function(src, 'resolve_at_path'),
    k.function(src, 'mutation_parent_access'),
    k.function(src, 'symlink_common'),
    k.function(src, 'sys_symlink_impl'),
    k.function(src, 'sys_symlinkat_impl'),
    k.function(src, 'sys_linkat_impl'),
    k.function(src, 'sys_link_impl'),
    k.function(src, 'sys_readlink_impl'),
    k.function(src, 'sys_readlinkat_impl'),
    k.function(src, 'sys_truncate_impl'),
])

code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define VFS_MAX_FD 8
#define VFS_TYPE_FILE 1
#define VFS_TYPE_DIR 2
#define VFS_TYPE_SYMLINK 3
#define VFS_PERM_WRITE 2
#define VFS_PERM_EXEC 1
#define BOLDON_CAP_FOWNER (1ULL << 3)

typedef struct { char name[256]; int type; uint32_t uid, gid; } vfs_node_t;
typedef struct { vfs_node_t *node; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; uint32_t fsuid, fsgid; uint64_t cap_effective; } process_t;

static process_fd_table_t table;
static process_t proc = { &table, 0, 0, 0 };
static process_t *get_current_process(void) { return &proc; }
static bool user_range_ok(uint64_t p, uint64_t n) { return p && n; }

static char log_buf[4096];
static void note(const char *what, const char *a, const char *b) {
    strcat(log_buf, what); strcat(log_buf, "|");
    if (a) strcat(log_buf, a);
    strcat(log_buf, "|");
    if (b) strcat(log_buf, b);
    strcat(log_buf, "\n");
}

static vfs_node_t *vfs_resolve_path_ext(const char *path, bool parent_only, char *last) {
    (void)parent_only;
    strcpy(last, path);
    static vfs_node_t parent = { .type = VFS_TYPE_DIR, .uid = 0, .gid = 0 };
    return &parent;
}
static int symlinks, links;
static int vfs_symlink(const char *target, const char *linkpath) {
    symlinks++; note("symlink", target, linkpath); return 0;
}
static int vfs_link(const char *oldpath, const char *newpath) {
    links++; note("link", oldpath, newpath); return 0;
}
static vfs_node_t *vfs_resolve_path(const char *path) {
    static vfs_node_t file = { .type = VFS_TYPE_FILE, .uid = 1000, .gid = 1000 };
    static vfs_node_t dir = { .type = VFS_TYPE_DIR, .uid = 1000, .gid = 1000 };
    note("resolve", path, NULL);
    vfs_node_t *n = (!strcmp(path, "/tmp")) ? &dir : &file;
    /* Keep the resolved path on the node so truncate() can log it. */
    strncpy(n->name, path, sizeof(n->name) - 1);
    return n;
}
static int vfs_check_access(vfs_node_t *n, uint32_t u, uint32_t g, int perms) {
    (void)n; (void)u; (void)g; (void)perms; return 0;
}
static int readlinks;
static int vfs_readlink(const char *path, char *buf, uint64_t size) {
    readlinks++;
    const char *target = "/usr/lib/libc.so";
    size_t n = strlen(target);
    if (n > size) n = size;
    memcpy(buf, target, n);
    note("readlink", path, NULL);
    return (int)n;
}
static int vfs_truncate_node(vfs_node_t *node, uint64_t length) {
    (void)length;
    note("truncate", node->name, NULL);
    return 0;
}
''' + fragment + r'''
int main(void) {
    char out[64];

    /* symlink(2): target is stored verbatim, the link name gets the dirfd. */
    static vfs_node_t dirnode = { .name = "/tmp", .type = VFS_TYPE_DIR };
    static vfs_file_t dirfile = { &dirnode };
    table.fds[3] = &dirfile;
    assert(sys_symlink_impl((uint64_t)"busybox", (uint64_t)"/bin/sh") == 0);
    assert(strstr(log_buf, "symlink|busybox|/bin/sh"));
    assert(sys_symlinkat_impl((uint64_t)"busybox", 3, (uint64_t)"sh") == 0);
    assert(strstr(log_buf, "symlink|busybox|/tmp/sh"));
    /* An empty target is ENOENT: a dangling-but-empty link is not useful. */
    assert((int64_t)sys_symlink_impl((uint64_t)"", (uint64_t)"/bin/x") == -ENOENT);
    /* A missing dirfd keeps EBADF, never a cwd fallback. */
    assert((int64_t)sys_symlinkat_impl((uint64_t)"t", 6, (uint64_t)"x") == -EBADF);

    /* linkat(2) resolves both operands against their own descriptors. */
    assert(sys_linkat_impl(3, (uint64_t)"a", 3, (uint64_t)"b", 0) == 0);
    assert(strstr(log_buf, "link|/tmp/a|/tmp/b"));
    /* Unknown flags are rejected; AT_SYMLINK_FOLLOW is accepted. */
    assert((int64_t)sys_linkat_impl(3, (uint64_t)"a", 3, (uint64_t)"b", 1) == -EINVAL);
    assert(sys_linkat_impl(3, (uint64_t)"a", 3, (uint64_t)"b", 0x400) == 0);

    /* readlink(2)/readlinkat(2) return the link body, not the target file. */
    assert(sys_readlink_impl((uint64_t)"/bin/sh", (uint64_t)out, sizeof(out)) == 16);
    assert(!memcmp(out, "/usr/lib/libc.so", 16));
    assert(sys_readlinkat_impl(3, (uint64_t)"sh", (uint64_t)out, sizeof(out)) == 16);
    assert(strstr(log_buf, "readlink|/tmp/sh"));
    assert((int64_t)sys_readlink_impl((uint64_t)"/bin/sh", (uint64_t)out, 0) == -EFAULT);
    assert((int64_t)sys_readlink_impl(0, (uint64_t)out, sizeof(out)) == -EFAULT);

    /* truncate(2) shrinks a regular file after a DAC write check. */
    assert(sys_truncate_impl((uint64_t)"/etc/motd", 12) == 0);
    assert(strstr(log_buf, "truncate|/etc/motd"));
    assert((int64_t)sys_truncate_impl((uint64_t)"/etc/motd", (uint64_t)-1) == -EINVAL);
    assert((int64_t)sys_truncate_impl(0, 0) == -EFAULT);
}
'''

k.compile_and_run(code)
print('symlink/linkat/readlink/truncate checks PASS')
