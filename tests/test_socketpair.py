"""Compile and exercise the real AF_UNIX stream socketpair backend on the host."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
unix = (root / 'kernel/src/net/unix_socket.c').read_text()
socket = (root / 'kernel/src/net/socket.c').read_text()
handler = (root / 'kernel/src/syscall/syscall.c').read_text()
file_syscalls = (root / 'kernel/src/syscall/sys_file.c').read_text()
assert 'case SYS_SOCKETPAIR: return sys_socketpair_impl(a1, a2, a3, a4);' in handler
assert 'case SYS_CLOSE_RANGE: return sys_close_range_impl(a1, a2, a3);' in handler
parts = [
    unix[unix.index('static int un_send('):unix.index('static int un_getsockname(')],
    unix[unix.index('static int un_close('):unix.index('static socket_ops_t unix_ops =')],
    unix[unix.index('int unix_socketpair_create('):],
    socket[socket.index('int sys_socketpair('):socket.index('int sys_bind(')],
    file_syscalls[file_syscalls.index('uint64_t sys_close_range_impl('):file_syscalls.index('uint64_t sys_mmap_impl(')],
]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define AF_UNIX 1
#define SOCK_STREAM 1
#define VFS_O_NONBLOCK 32
#define VFS_O_CLOEXEC 64
#define VFS_O_RDWR 3
#define VFS_MAX_FD 256
#define VFS_PERM_WRITE 2
#define VFS_PERM_EXEC 1
typedef struct vfs_node { int unused; } vfs_node_t;
static vfs_node_t parent_node;
static vfs_node_t *vfs_resolve_path_ext(const char *path, bool parent_only, char *leaf) {
    (void)path; (void)parent_only; strcpy(leaf, "socket"); return &parent_node;
}
static int vfs_check_access(vfs_node_t *node, uint32_t uid, uint32_t gid, int permission) {
    (void)node; (void)uid; (void)gid; (void)permission; return 0;
}
typedef struct socket socket_t;
typedef struct { int (*close)(socket_t *); } socket_ops_t;
struct socket { int domain, type, protocol; socket_ops_t *ops; void *private_data; };
struct sockaddr_un { uint16_t sun_family; char sun_path[108]; };
typedef struct unix_socket_internal {
    bool in_use, bound, path_node_created, peer_closed, listening;
    struct sockaddr_un addr;
    uint8_t rx_buffer[8192];
    uint32_t rx_head, rx_tail;
    struct unix_socket_internal *peer, *accept_queue[16];
    int accept_head, accept_tail;
} unix_intern_t;
#define MAX_UNIX 128
static unix_intern_t unix_pool[MAX_UNIX];
static void *malloc_alloc(size_t n) { return calloc(1, n); }
static void malloc_free(void *p) { free(p); }
static void schedule_yield(void) { abort(); } // no blocking calls in this test
static int vfs_unlink(const char *path) { (void)path; return 0; }
static int un_close(socket_t *sock);
static socket_ops_t unix_ops = {.close=un_close};
socket_t *unix_socket_create(int type, int protocol);
typedef struct { socket_t *sock; int flags; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } fd_table_t;
typedef struct { fd_table_t *fd_table; uint32_t fsuid, fsgid; } process_t;
static fd_table_t fd_table;
static process_t proc = {.fd_table=&fd_table};
static process_t *get_current_process(void) { return &proc; }
static int allocate_socket_fd(socket_t *sock) {
    for (int i=0; i<VFS_MAX_FD; i++) if (!fd_table.fds[i]) {
        vfs_file_t *file = calloc(1, sizeof(*file));
        file->sock=sock;
        file->flags=VFS_O_RDWR;
        fd_table.fds[i]=file;
        return i;
    }
    return -EMFILE;
}
static int sys_close(fd_table_t *table, int fd) {
    vfs_file_t *f = table->fds[fd];
    f->sock->ops->close(f->sock);
    free(f->sock); free(f); table->fds[fd]=NULL;
    return 0;
}
''' + '\n'.join(parts) + r'''
int main(void) {
    int fds[2] = {-1, -1};
    assert(sys_socketpair(AF_UNIX, SOCK_STREAM|00004000|02000000, 0, fds) == 0);
    assert(fds[0] == 0 && fds[1] == 1);
    assert((fd_table.fds[0]->flags & (VFS_O_NONBLOCK|VFS_O_CLOEXEC)) ==
           (VFS_O_NONBLOCK|VFS_O_CLOEXEC));
    assert(sys_socketpair(AF_UNIX, 2, 0, fds) == -ESOCKTNOSUPPORT);
    assert(sys_socketpair(2, SOCK_STREAM, 0, fds) == -EAFNOSUPPORT);
    socket_t *a = fd_table.fds[0]->sock, *b = fd_table.fds[1]->sock;
    assert(un_send(a, "abc", 3, 0x4000) == 3);
    char buf[8192] = {0};
    assert(un_recv(b, buf, 3, 0x4000) == 3 && !memcmp(buf, "abc", 3));
    assert(un_send(b, "xy", 2, 0x4000) == 2);
    assert(un_recv(a, buf, 2, 0x4000) == 2 && !memcmp(buf, "xy", 2));
    assert(un_recv(a, buf, 1, 0x4000) == -EAGAIN);
    char large[8192]; memset(large, 'z', sizeof(large));
    assert(un_send(a, large, sizeof(large), 0x4000) == 8191);
    assert(un_send(a, "z", 1, 0x4000) == -EAGAIN);
    assert(un_recv(b, buf, sizeof(buf), 0x4000) == 8191);
    sys_close(&fd_table, 0);
    assert(un_recv(b, buf, 1, 0x4000) == 0);
    assert(un_send(b, "z", 1, 0x4000) == -EPIPE);
    sys_close(&fd_table, 1);
    assert(sys_socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(sys_close_range_impl(0, 1, 4) == (uint64_t)-ENOSYS);
    assert(fd_table.fds[0] && fd_table.fds[1]);
    assert(sys_close_range_impl(1, 0, 0) == (uint64_t)-EINVAL);
    assert(sys_close_range_impl(0, UINT32_MAX, 0) == 0);
    assert(!fd_table.fds[0] && !fd_table.fds[1]);
    vfs_file_t occupied = {0};
    for (int i=0; i<VFS_MAX_FD-1; i++) fd_table.fds[i] = &occupied;
    assert(sys_socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -EMFILE);
    assert(fd_table.fds[VFS_MAX_FD-1] == NULL);
    for (int i=0; i<MAX_UNIX; i++) assert(!unix_pool[i].in_use);
    for (int i=0; i<VFS_MAX_FD-1; i++) fd_table.fds[i] = NULL;
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    source.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('AF_UNIX socketpair: flags, bidirectional I/O, backpressure and EOF passed')
