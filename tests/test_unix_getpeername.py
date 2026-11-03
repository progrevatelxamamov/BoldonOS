"""getpeername on a UNIX connection reports the peer path without overruns."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "kernel/src/net/unix_socket.c").read_text()
peer = source[source.index("static int un_getpeername("):source.index("static short un_poll(")]
assert ".getpeername = un_getpeername" in source
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#define AF_UNIX 1
typedef uint32_t socklen_t;
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct sockaddr_un { unsigned short sun_family; char sun_path[108]; };
typedef struct unix_socket_internal {
    struct sockaddr_un addr;
    bool peer_closed;
    struct unix_socket_internal *peer;
} unix_intern_t;
typedef struct socket { void *private_data; } socket_t;
''' + peer + r'''
int main(void) {
    unix_intern_t client = {0}, server = {0};
    socket_t sock = {.private_data=&client};
    char out[128];
    socklen_t len = sizeof(out);
    assert(un_getpeername(&sock, (struct sockaddr *)out, &len) == -ENOTCONN);
    client.peer = &server;
    strcpy(server.addr.sun_path, "/tmp/wayland-0");
    memset(out, 0x5a, sizeof(out));
    len = 2;
    assert(un_getpeername(&sock, (struct sockaddr *)out, &len) == 0);
    assert(len == 2 + strlen(server.addr.sun_path) + 1);
    assert(out[2] == 0x5a); // caller supplied only two bytes
    len = sizeof(out);
    assert(un_getpeername(&sock, (struct sockaddr *)out, &len) == 0);
    assert(!strcmp(out + 2, "/tmp/wayland-0"));
    client.peer_closed = true;
    assert(un_getpeername(&sock, (struct sockaddr *)out, &len) == -ENOTCONN);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / "peer.c"
    exe = Path(directory) / "peer"
    src.write_text(code)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("UNIX getpeername length, truncation and disconnected cases passed")
