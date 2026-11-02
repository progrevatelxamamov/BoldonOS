"""Host regression of the real syscall 275 implementation and pipe semantics."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/syscall/sys_file.c').read_text()
pipe = source[source.index('static int pipe_read('):source.index('uint64_t sys_pipe2_impl(', source.index('static int pipe_read('))]
splice = source[source.index('uint64_t sys_splice_impl('):source.index('uint64_t sys_mkdirat_impl(', source.index('uint64_t sys_splice_impl('))]
assert 'case SYS_SPLICE: return sys_splice_impl(a1, a2, a3, a4, a5, a6);' in (root / 'kernel/src/syscall/syscall.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define VFS_MAX_FD 8
#define VFS_O_READ 1
#define VFS_O_WRITE 2
#define VFS_O_NONBLOCK 4
#define ESPIPE 29
typedef struct { uint8_t buf[4096]; uint32_t head, tail; } pipe_ring_t;
typedef struct vfs_file vfs_file_t;
typedef struct { int (*read)(vfs_file_t *, void *, uint64_t);
                 int (*write)(vfs_file_t *, const void *, uint64_t); } vfs_file_ops_t;
typedef struct { vfs_file_ops_t *file_ops; void *fs_data; } vfs_node_t;
struct vfs_file { vfs_node_t *node; uint32_t flags; uint64_t offset; };
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; } process_t;
static process_fd_table_t fds;
static process_t proc = { &fds };
static process_t *get_current_process(void) { return &proc; }
static bool user_range_ok(uint64_t ptr, uint64_t len) { return ptr && len == 8; }
static void *malloc_alloc(size_t size) { return malloc(size); }
static void malloc_free(void *p) { free(p); }
static void schedule_yield(void) { assert(!"unexpected blocking"); }
static int vfs_read(vfs_file_t *f, void *buf, uint64_t n) {
    return f->node->file_ops->read(f, buf, n);
}
static int vfs_write(vfs_file_t *f, const void *buf, uint64_t n) {
    return f->node->file_ops->write(f, buf, n);
}
static int file_read(vfs_file_t *f, void *buf, uint64_t n) {
    const char *data = f->node->fs_data;
    uint64_t len = strlen(data);
    if (f->offset >= len) return 0;
    if (n > len - f->offset) n = len - f->offset;
    memcpy(buf, data + f->offset, n);
    f->offset += n;
    return n;
}
static char sink[32];
static int sink_write(vfs_file_t *f, const void *buf, uint64_t n) {
    if (n > sizeof(sink) - f->offset) n = sizeof(sink) - f->offset;
    memcpy(sink + f->offset, buf, n);
    f->offset += n;
    return n;
}
static vfs_file_ops_t file_ops = { .read = file_read, .write = sink_write };
''' + pipe + splice + r'''
int main(void) {
    pipe_ring_t ring = {0};
    vfs_node_t pipe_node = { .file_ops = &pipe_file_ops, .fs_data = &ring };
    vfs_node_t file_node = { .file_ops = &file_ops, .fs_data = "mouse-packets" };
    vfs_file_t input = { .node = &file_node, .flags = VFS_O_READ };
    vfs_file_t output = { .node = &pipe_node, .flags = VFS_O_WRITE | VFS_O_NONBLOCK };
    fds.fds[3] = &input; fds.fds[4] = &output;
    assert(sys_splice_impl(3, 0, 4, 0, 5, 0) == 5);
    assert(ring.head == 5 && memcmp(ring.buf, "mouse", 5) == 0);
    assert(sys_splice_impl(3, 0, 4, 0, 100, 0) == 8);
    assert(sys_splice_impl(3, 0, 4, 0, 100, 0) == 0); /* EOF */
    assert(sys_splice_impl(3, 0, 4, 0, 1, 0x10) == (uint64_t)-EINVAL);
    assert(sys_splice_impl(3, 0, 3, 0, 1, 0) == (uint64_t)-EINVAL);
    uint64_t offset = 1;
    assert(sys_splice_impl(3, (uint64_t)&offset, 4, 0, 2, 0) == 2);
    assert(offset == 3 && input.offset == 13);
    assert(sys_splice_impl(3, 0, 4, (uint64_t)&offset, 1, 0) == (uint64_t)-ESPIPE);
    offset = (uint64_t)-1;
    assert(sys_splice_impl(3, (uint64_t)&offset, 4, 0, 1, 0) == (uint64_t)-EINVAL);
    offset = 1;
    assert(sys_splice_impl(4, 0, 4, 0, 1, 0) == (uint64_t)-EINVAL);
    ring.head = 4095; ring.tail = 0;
    assert(sys_splice_impl(3, 0, 4, 0, 1, 2) == (uint64_t)-EAGAIN);
    pipe_ring_t inbound = {0};
    memcpy(inbound.buf, "abc", 3); inbound.head = 3;
    vfs_node_t inbound_node = { .file_ops = &pipe_file_ops, .fs_data = &inbound };
    vfs_file_t inbound_file = { .node = &inbound_node, .flags = VFS_O_READ };
    vfs_file_t sink_file = { .node = &file_node, .flags = VFS_O_WRITE };
    fds.fds[5] = &inbound_file; fds.fds[6] = &sink_file;
    assert(sys_splice_impl(5, 0, 6, 0, 3, 0) == 3);
    assert(memcmp(sink, "abc", 3) == 0 && inbound.tail == 3);
    assert(sys_splice_impl(5, (uint64_t)&offset, 6, 0, 1, 2) == (uint64_t)-ESPIPE);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'splice.c'
    binary = Path(directory) / 'splice'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('splice moves bytes to pipe, respects offsets/nonblock/errors/EOF')
