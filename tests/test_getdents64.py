"""getdents64(2): a short buffer must never silently drop a directory entry.

The old loop called readdir() first and only then checked whether the record
still fit, so the entry that did not fit was consumed and lost; every later
read() of that directory skipped a name. The kernel now pushes the record back
into the file object. This test replays exactly that sequence against the real
`vfs_readdir` / `vfs_readdir_pushback` / `sys_getdents64_impl` code.
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kernel_snip as k

syscall_src = k.source('kernel/src/syscall/sys_file.c')
vfs_src = k.source('kernel/src/fs/vfs.c')

fragment = '\n'.join([
    k.function(vfs_src, 'vfs_readdir_pushback'),
    k.function(vfs_src, 'vfs_readdir'),
    k.defines(syscall_src, 'LINUX_DT_UNKNOWN', 'LINUX_DT_FIFO', 'LINUX_DT_CHR',
              'LINUX_DT_DIR', 'LINUX_DT_BLK', 'LINUX_DT_REG', 'LINUX_DT_LNK',
              'LINUX_DT_SOCK'),
    k.function(syscall_src, 'linux_dtype'),
    k.function(syscall_src, 'sys_getdents64_impl'),
])

code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define VFS_MAX_FD 8

typedef enum { VFS_TYPE_UNKNOWN, VFS_TYPE_FILE, VFS_TYPE_DIR, VFS_TYPE_SYMLINK,
               VFS_TYPE_BLOCK_DEV, VFS_TYPE_CHAR_DEV, VFS_TYPE_FIFO,
               VFS_TYPE_SOCKET } vfs_node_type_t;

typedef struct {
    char name[256];
    uint64_t inode_num;
    vfs_node_type_t type;
    uint64_t size;
} vfs_dirent_t;

struct vfs_file;

typedef struct {
    int (*readdir)(struct vfs_file *file, vfs_dirent_t *dirent);
} vfs_file_ops_t;

typedef struct vfs_node {
    vfs_node_type_t type;
    vfs_file_ops_t *file_ops;
} vfs_node_t;

typedef struct vfs_file {
    vfs_node_t *node;
    uint64_t offset;
    vfs_dirent_t pending_dirent;
    bool has_pending_dirent;
} vfs_file_t;

typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; } process_t;

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

static process_fd_table_t table;
static process_t proc = { &table };
static process_t *get_current_process(void) { return &proc; }
static bool user_range_ok(uint64_t p, uint64_t n) {
    return p != 0 && n != 0 && p < 0x7fffffffffffULL;
}

/* A four-entry directory: "..", ".", "busybox", "sh" (all 24-byte records). */
static const char *names[] = { "..", ".", "busybox", "sh" };
static int backend_readdir(vfs_file_t *file, vfs_dirent_t *dent) {
    if (file->offset >= 4) return -1;      /* end of directory */
    uint64_t i = file->offset;
    strcpy(dent->name, names[i]);
    dent->inode_num = 100 + i;
    dent->type = (i < 2) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    file->offset++;                        /* the backend always advances */
    return 0;
}
static vfs_file_ops_t dir_ops = { .readdir = backend_readdir };
''' + fragment + r'''
int main(void) {
    static char buf[256];
    static vfs_node_t dirnode = { VFS_TYPE_DIR, &dir_ops };
    static vfs_file_t dir = { .node = &dirnode };
    table.fds[2] = &dir;

    /* A 32-byte buffer holds one 24-byte record; the second must not be lost. */
    uint64_t n = sys_getdents64_impl(2, (uint64_t)buf, 32);
    assert(n == 24);
    struct linux_dirent64 *d = (struct linux_dirent64 *)buf;
    assert(d->d_ino == 100 && d->d_reclen == 24 && d->d_type == 4);
    assert(!strcmp(d->d_name, ".."));

    n = sys_getdents64_impl(2, (uint64_t)buf, 32);
    assert(n == 24);
    d = (struct linux_dirent64 *)buf;
    assert(!strcmp(d->d_name, "."));
    /* The record that did not fit is queued, not consumed. */
    assert(dir.has_pending_dirent);

    /* A buffer too small for the first pending record is EINVAL and the
       record stays queued (Linux filldir returns -EINVAL there). "busybox"
       is 32 bytes: (19 + 7 + 1 + 7) & ~7. */
    assert((int64_t)sys_getdents64_impl(2, (uint64_t)buf, 31) == -EINVAL);
    assert(dir.has_pending_dirent);

    /* The pushed-back record is returned by the next call, not skipped. */
    n = sys_getdents64_impl(2, (uint64_t)buf, 32);
    assert(n == 32);
    d = (struct linux_dirent64 *)buf;
    assert(!strcmp(d->d_name, "busybox") && d->d_type == 8);
    assert(d->d_off == 3);

    n = sys_getdents64_impl(2, (uint64_t)buf, 32);
    assert(n == 24 && !strcmp(((struct linux_dirent64 *)buf)->d_name, "sh"));
    assert(dir.offset == 4);

    /* End of directory. */
    assert(sys_getdents64_impl(2, (uint64_t)buf, sizeof(buf)) == 0);

    /* Wrong descriptor kinds and sizes. */
    static vfs_node_t filenode = { VFS_TYPE_FILE, &dir_ops };
    static vfs_file_t plain = { .node = &filenode };
    table.fds[3] = &plain;
    assert((int64_t)sys_getdents64_impl(3, (uint64_t)buf, 64) == -ENOTDIR);
    assert((int64_t)sys_getdents64_impl(9, (uint64_t)buf, 64) == -EBADF);
    assert((int64_t)sys_getdents64_impl(2, (uint64_t)buf, 0) == -EINVAL);
    assert((int64_t)sys_getdents64_impl(2, 0, 64) == -EINVAL);
}
'''

k.compile_and_run(code)
print('getdents64 short-buffer, pushback and d_type checks PASS')


# --- lseek(2) -------------------------------------------------------------
lfrag = k.function(vfs_src, 'vfs_lseek')

lcode = r'''
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <assert.h>
#define VFS_TYPE_UNKNOWN 0
#define VFS_TYPE_FILE 1
#define VFS_TYPE_DIR 2
#define VFS_TYPE_SYMLINK 3
#define VFS_TYPE_BLOCK_DEV 4
#define VFS_TYPE_CHAR_DEV 5
#define VFS_TYPE_FIFO 6
#define VFS_TYPE_SOCKET 7
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2
typedef struct { int (*lseek)(void *, int64_t, int); } vfs_file_ops_t;
typedef struct vfs_node {
    int type; uint64_t size; vfs_file_ops_t *file_ops;
} vfs_node_t;
typedef struct { vfs_node_t *node; uint64_t offset; } vfs_file_t;
''' + lfrag + r'''
int main(void) {
    vfs_node_t file = { VFS_TYPE_FILE, 100, 0 };
    vfs_file_t f = { &file, 0 };
    assert(vfs_lseek(&f, 10, VFS_SEEK_SET) == 10 && f.offset == 10);
    assert(vfs_lseek(&f, 5, VFS_SEEK_CUR) == 15 && f.offset == 15);
    assert(vfs_lseek(&f, -5, VFS_SEEK_END) == 95 && f.offset == 95);
    assert(vfs_lseek(&f, -200, VFS_SEEK_SET) == -EINVAL);
    assert(vfs_lseek(&f, 0, 9) == -EINVAL);

    /* SEEK_DATA / SEEK_HOLE: dense allocation, the hole only starts at EOF. */
    assert(vfs_lseek(&f, 50, 3) == 50 && f.offset == 50);
    assert(vfs_lseek(&f, 100, 3) == -ENXIO);
    assert(vfs_lseek(&f, 50, 4) == 100 && f.offset == 100);
    assert(vfs_lseek(&f, 101, 4) == -ENXIO);

    /* Streams have no seek position at all. */
    vfs_node_t fifo = { VFS_TYPE_FIFO, 100, 0 };
    vfs_file_t g = { &fifo, 0 };
    assert(vfs_lseek(&g, 0, VFS_SEEK_SET) == -ESPIPE);
    vfs_node_t chr = { VFS_TYPE_CHAR_DEV, 0, 0 };
    vfs_file_t h = { &chr, 0 };
    assert(vfs_lseek(&h, 0, VFS_SEEK_CUR) == -ESPIPE);
}
'''
k.compile_and_run(lcode)
print('lseek ESPIPE and SEEK_DATA/SEEK_HOLE checks PASS')
