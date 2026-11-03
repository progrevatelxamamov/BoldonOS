"""Test ppoll timeout, descriptors, mask restoration and error paths."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "kernel/src/syscall/sys_file.c").read_text()
poll = source[source.index("uint64_t sys_poll_impl("):source.index("uint64_t sys_fcntl_impl(")]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#define VFS_MAX_FD 256
#define VFS_TYPE_CHAR_DEV 4
struct pollfd { int fd; short events; short revents; };
struct linux_timespec { int64_t tv_sec; int64_t tv_nsec; };
typedef struct vfs_file vfs_file_t;
typedef struct { short (*poll)(vfs_file_t *, short); } file_ops_t;
typedef struct { file_ops_t *file_ops; int type; uint64_t size; } node_t;
struct vfs_file { node_t *node; uint64_t offset; };
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } fd_table_t;
typedef struct { fd_table_t *fd_table; uint64_t sigpending, sigmask; } process_t;
static fd_table_t fd_table;
static process_t proc = { .fd_table = &fd_table };
static process_t *get_current_process(void) { return &proc; }
static bool user_range_ok(uint64_t ptr, uint64_t len) { return ptr != 0 && len < 1000000; }
static uint64_t ticks;
static uint64_t get_monotonic_ms(void) { return ticks; }
static void schedule_yield(void) { ticks++; }
static short pty_poll_events(vfs_file_t *file, short events) {
    (void)file; (void)events; return 0;
}
''' + poll + r'''
int main(void) {
    struct pollfd fds[2] = {{.fd=-1, .events=1}, {.fd=12, .events=1}};
    assert(sys_poll_impl((uintptr_t)fds, 2, 0) == 1);
    assert(fds[0].revents == 0 && fds[1].revents == 0x20); // POLLNVAL
    assert(sys_poll_impl(0, VFS_MAX_FD+1, 0) == (uint64_t)-EINVAL);
    ticks = 0;
    assert(sys_poll_impl(0, 0, 3) == 0 && ticks == 3);
    ticks = 0;
    struct linux_timespec sub_ms = {0, 1};
    assert(sys_ppoll_impl(0, 0, (uintptr_t)&sub_ms, 0, 0) == 0 && ticks == 1);
    assert(sub_ms.tv_nsec == 1); // ppoll leaves timeout untouched
    struct linux_timespec invalid = {0, 1000000000};
    assert(sys_ppoll_impl(0, 0, (uintptr_t)&invalid, 0, 0) == (uint64_t)-EINVAL);
    uint64_t mask = 0x11;
    proc.sigmask = 0x44;
    ticks = 0;
    assert(sys_ppoll_impl(0, 0, (uintptr_t)&sub_ms, (uintptr_t)&mask, 8) == 0);
    assert(proc.sigmask == 0x44 && ticks == 1);
    assert(sys_ppoll_impl(0, 0, (uintptr_t)&sub_ms, (uintptr_t)&mask, 16) == (uint64_t)-EINVAL);
    proc.sigpending = 2;
    proc.sigmask = 0;
    assert(sys_poll_impl(0, 0, 100) == (uint64_t)-EINTR);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / "ppoll.c"
    exe = Path(directory) / "ppoll"
    src.write_text(code)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("ppoll: timeout, POLLNVAL, sigmask restore, EINTR passed")
