"""Regression: socket syscalls must reject a descriptor with no VFS node."""
from pathlib import Path
import subprocess
import tempfile

src = (Path(__file__).resolve().parents[1] / "kernel/src/net/socket.c").read_text()
function = src[src.index("socket_t* get_socket_from_fd("):src.index("int sys_socket(", src.index("socket_t* get_socket_from_fd("))]

harness = r'''
#include <stddef.h>
#define VFS_MAX_FD 256
typedef struct { int marker; } vfs_file_ops_t;
typedef struct { vfs_file_ops_t *file_ops; void *fs_data; } vfs_node_t;
typedef struct { vfs_node_t *node; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; } process_t;
typedef struct { int marker; } socket_t;
static process_t current;
static process_t *get_current_process(void) { return &current; }
static vfs_file_ops_t generic_socket_vfs_ops;
''' + function + r'''
int main(void) {
    process_fd_table_t table = {0};
    vfs_file_t empty = {0};
    vfs_file_ops_t other_ops = {0};
    vfs_node_t other_node = {.file_ops = &other_ops};
    socket_t sock = { .marker = 123 };
    vfs_node_t socket_node = {.file_ops = &generic_socket_vfs_ops, .fs_data = &sock};
    current.fd_table = &table;
    if (get_socket_from_fd(-1) || get_socket_from_fd(VFS_MAX_FD) || get_socket_from_fd(4)) return 1;
    table.fds[4] = &empty;
    if (get_socket_from_fd(4)) return 2;
    empty.node = &other_node;
    if (get_socket_from_fd(4)) return 3;
    empty.node = &socket_node;
    if (get_socket_from_fd(4) != &sock) return 4;
    current.fd_table = NULL;
    if (get_socket_from_fd(4)) return 5;
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "probe.c"
    exe = Path(tmp) / "probe"
    c.write_text(harness)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("socket fd without node returns EBADF path, no kernel null dereference")
