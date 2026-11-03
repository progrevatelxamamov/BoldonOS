"""Check syscall 52 resolves through the socket layer to its backend."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
net = (root / "kernel/src/net/socket.c").read_text()
wrapper = (root / "kernel/src/syscall/sys_net.c").read_text()
handler = (root / "kernel/src/syscall/syscall.c").read_text()
assert "case SYS_GETPEERNAME: return sys_getpeername_impl(a1, a2, a3);" in handler
assert "int res = sys_getpeername((int)sockfd" in wrapper
start = net.index("int sys_getpeername(int sockfd,")
end = net.index("short socket_poll_events(", start)
code = r'''
#include <assert.h>
#include <stdint.h>
#include <errno.h>
typedef uint32_t socklen_t;
struct sockaddr { uint16_t family; char data[14]; };
struct socket;
typedef struct {
    int (*getpeername)(struct socket *, struct sockaddr *, socklen_t *);
} socket_ops_t;
typedef struct socket { socket_ops_t *ops; } socket_t;
static socket_t *socket_fd;
static socket_t *get_socket_from_fd(int fd) { return fd == 7 ? socket_fd : 0; }
static int peer_called;
static int peer(socket_t *s, struct sockaddr *addr, socklen_t *len) {
    (void)s; (void)addr; (void)len; peer_called++; return -ENOTCONN;
}
''' + net[start:end] + r'''
int main(void) {
    struct sockaddr addr = {0}; socklen_t len = sizeof(addr);
    assert(sys_getpeername(7, &addr, &len) == -EBADF);
    socket_ops_t ops = {0}; socket_t sock = {.ops=&ops}; socket_fd = &sock;
    assert(sys_getpeername(7, &addr, &len) == -EOPNOTSUPP);
    ops.getpeername = peer;
    assert(sys_getpeername(7, &addr, &len) == -ENOTCONN);
    assert(peer_called == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / "test.c"
    exe = Path(directory) / "test"
    src.write_text(code)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("getpeername syscall wrapper and socket backend dispatch passed")
