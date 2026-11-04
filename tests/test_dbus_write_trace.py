"""Exercise the real failed-write diagnostic for dbus-daemon's address pipe."""
from pathlib import Path
import subprocess
import tempfile

src = (Path(__file__).resolve().parents[1] / 'kernel/src/syscall/sys_file.c').read_text()
body = src[src.index('uint64_t sys_write_impl('):src.index('uint64_t sys_writev_impl(', src.index('uint64_t sys_write_impl('))]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#define VFS_MAX_FD 256
typedef struct { void *node; unsigned flags; } vfs_file_t;
typedef struct { vfs_file_t *fds[VFS_MAX_FD]; } process_fd_table_t;
typedef struct { process_fd_table_t *fd_table; int trace_desktop; const char *name; uint64_t pid; } process_t;
static process_fd_table_t table;
static process_t proc = { &table, 1, "dbus-daemon", 1002 };
static process_t *get_current_process(void) { return &proc; }
static int user_range_ok(uint64_t addr, uint64_t size) { return addr && size < 4096; }
static int write_result, marker_count;
static uint64_t marker_pid, marker_detail;
static const char *marker_stage;
static int sys_write(process_fd_table_t *fds, int fd, const void *buf, uint64_t n) {
    (void)fds; (void)fd; (void)buf; (void)n; return write_result;
}
static void xorgtrace_stage(const char *stage, uint64_t pid, uint64_t detail) {
    marker_count++; marker_stage = stage; marker_pid = pid; marker_detail = detail;
}
''' + body + r'''
int main(void) {
    vfs_file_t pipe_end = { (void *)1, 2 };
    table.fds[5] = &pipe_end;
    write_result = -EBADF;
    if ((int64_t)sys_write_impl(5, (uint64_t)"a", 1) != -EBADF) return 1;
    if (marker_count != 1 || strcmp(marker_stage, "dbus.write.fail") || marker_pid != 1002) return 2;
    if ((marker_detail >> 48) != 5 || ((marker_detail >> 32) & 0xffff) != 2 ||
        !(marker_detail & (1ULL << 31)) || (marker_detail & 0x7fffffff) != EBADF) return 3;
    table.fds[5] = NULL;
    if ((int64_t)sys_write_impl(5, (uint64_t)"a", 1) != -EBADF || marker_count != 2 ||
        ((marker_detail >> 32) & 0xffff) != 0 || (marker_detail & (1ULL << 31))) return 4;
    write_result = 1;
    if (sys_write_impl(5, (uint64_t)"a", 1) != 1 || marker_count != 2) return 5;
    proc.name = "sh";
    write_result = -EBADF;
    sys_write_impl(5, (uint64_t)"a", 1);
    if (marker_count != 2) return 6;
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    source.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('D-Bus pipe write failures retain fd, flags, node presence and errno')
