"""dirfd-relative open(2): *at() must resolve names against the descriptor.

Before this test existed the kernel answered -ENOSYS for every relative name
that came with a real dirfd, because the resolver only knew the process cwd.
The test compiles the production `sys_openat_impl` together with a fake VFS so
the observable behaviour (which path reaches sys_open, which errno wins) is
asserted on the real code, not on a re-implementation.
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kernel_snip as k

src = k.source('kernel/src/syscall/sys_file.c')
fragment = '\n'.join([
    k.defines(src, 'LINUX_AT_FDCWD'),
    k.function(src, 'copy_str_from_user'),
    k.function(src, 'fdt_file'),
    k.function(src, 'resolve_dirfd_prefix'),
    k.function(src, 'resolve_at_path'),
    k.function(src, 'translate_linux_open_flags'),
    k.function(src, 'sys_openat_impl'),
    k.function(src, 'sys_open_impl'),
])

code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define VFS_MAX_FD 8
#define VFS_O_READ 1
#define VFS_O_WRITE 2
#define VFS_O_CREAT 4
#define VFS_O_TRUNC 8
#define VFS_O_APPEND 16
#define VFS_O_NONBLOCK 32
#define VFS_O_CLOEXEC 64
#define VFS_O_EXCL 128
#define VFS_O_DIRECTORY 256
#define VFS_O_NOFOLLOW 512
typedef enum { VFS_TYPE_FILE, VFS_TYPE_DIR, VFS_TYPE_SYMLINK } vfs_node_type_t;
typedef struct { char name[256]; vfs_node_type_t type; } vfs_node_t;
typedef struct { vfs_node_t *node; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; } process_t;
static process_fd_table_t table;
static process_t proc = {&table};
static process_t *get_current_process(void) { return &proc; }
static bool user_range_ok(uint64_t p, uint64_t n) {
    if (!p) return n == 0;
    return p >= 0x10000 && n < (1u << 20);
}
static char last[600]; static unsigned lastflags; static int opens;
static int sys_open(process_fd_table_t *fds, const char *path, uint32_t flags) {
    assert(fds == &table); opens++; strncpy(last, path, sizeof(last) - 1);
    last[sizeof(last) - 1] = 0; lastflags = flags; return 7;
}
static vfs_node_t *vfs_resolve_path_nofollow(const char *p) { (void)p; return 0; }
''' + fragment + r'''
int main(void) {
    /* Descriptor 3 is the directory /tmp/work, descriptor 4 is a plain file. */
    static vfs_node_t dirs[4]; static vfs_file_t files[4];
    strcpy(dirs[0].name, "/tmp/work"); dirs[0].type = VFS_TYPE_DIR;
    dirs[1].type = VFS_TYPE_FILE;
    files[0].node = &dirs[0]; files[1].node = &dirs[1];
    table.fds[3] = &files[0]; table.fds[4] = &files[1];

    /* Relative to a directory descriptor: prefix from the node name. */
    assert(sys_openat_impl(3, (uint64_t)"file.txt", 1) == 7);
    assert(!strcmp(last, "/tmp/work/file.txt"));
    assert(lastflags == VFS_O_WRITE);

    /* The trailing slash is not duplicated for a node that carries one. */
    strcpy(dirs[0].name, "/tmp/work/");
    assert(sys_openat_impl(3, (uint64_t)"a", 0) == 7);
    assert(!strcmp(last, "/tmp/work/a"));

    /* A file descriptor is not a directory. */
    opens = 0;
    assert((int64_t)sys_openat_impl(4, (uint64_t)"x", 0) == -ENOTDIR);
    /* A missing descriptor keeps EBADF, never a silent cwd fallback. */
    assert((int64_t)sys_openat_impl(5, (uint64_t)"x", 0) == -EBADF);
    assert(opens == 0);

    /* Absolute names ignore dirfd completely, even when it is bogus. */
    assert(sys_openat_impl(9, (uint64_t)"/etc/passwd", 0) == 7);
    assert(!strcmp(last, "/etc/passwd"));

    /* AT_FDCWD keeps the original relative spelling (the VFS applies cwd). */
    assert(sys_openat_impl((uint64_t)(int64_t)-100, (uint64_t)"rel", 0) == 7);
    assert(!strcmp(last, "rel"));
    assert(sys_open_impl((uint64_t)"/legacy", 0, 0) == 7);
    assert(!strcmp(last, "/legacy"));

    /* Mode bits translate: O_EXCL, O_DIRECTORY, O_CLOEXEC. */
    assert(sys_openat_impl(3, (uint64_t)"flags",
                           0100 | 0200 | 0200000 | 02000000) == 7);
    assert(lastflags & VFS_O_EXCL);
    assert(lastflags & VFS_O_DIRECTORY);
    assert(lastflags & VFS_O_CLOEXEC);
    assert(lastflags & VFS_O_CREAT);

    /* A null name is EFAULT, a name too long for the buffer is ENAMETOOLONG. */
    assert((int64_t)sys_openat_impl(3, 0, 0) == -EFAULT);
    static char huge[600]; memset(huge, 'a', sizeof(huge) - 1);
    assert((int64_t)sys_openat_impl(3, (uint64_t)huge, 0) == -ENAMETOOLONG);
}
'''

k.compile_and_run(code)
print('dirfd-relative openat checks PASS')
