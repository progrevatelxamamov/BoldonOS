"""Production bounded Xorg trace: pending syscalls, snapshots and process filter."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
trace = (root / 'kernel/src/syscall/xorgtrace.c').read_text()
trace = trace[trace.index('#define XORGTRACE_CAP'):]
syscall = (root / 'kernel/src/syscall/syscall.c').read_text()
handler = syscall[syscall.index('uint64_t syscall_handler(uint64_t num,'):]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define SPINLOCK_INIT 0
typedef int spinlock_t;
static void spin_lock_irqsave(spinlock_t *l, uint64_t *flags) { (void)l; *flags=0; }
static void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) { (void)l; (void)flags; }
static void *malloc_alloc(size_t n) { return calloc(1, n); }
static void malloc_free(void *p) { free(p); }
static unsigned enter_lines, exit_lines, stage_lines;
static void console_print_string(const char *s) {
    if (!strcmp(s, "[XORGTRACE] ")) stage_lines++;
    if (!strcmp(s, "[XORGTRACE] enter pid=")) enter_lines++;
    if (!strcmp(s, "[XORGTRACE] exit seq=")) exit_lines++;
}
static void console_print_num(uint64_t x) { (void)x; }
static void console_print_hex(uint64_t x) { (void)x; }
typedef struct { uint64_t offset; void *private_data; } vfs_file_t;
typedef struct { int (*open)(vfs_file_t *); int (*read)(vfs_file_t *,void *,uint64_t);
    int (*close)(vfs_file_t *); } vfs_file_ops_t;
#define VFS_TYPE_FILE 1
static int devfs_register_device(const char *name, int type, vfs_file_ops_t *ops, void *data) {
    assert(!strcmp(name,"xorgtrace") && type == VFS_TYPE_FILE && ops && !data);
    return 0;
}
''' + trace + r'''
typedef struct { uint64_t pid; const char *name; bool trace_desktop; } process_t;
static process_t current = { 77, "sh", false };
static process_t *get_current_process(void) { return &current; }
#define SYS_CLONE 56
#define SYS_FORK 57
#define SYS_EXECVE 59
static uint64_t osdev_syscall_handler(uint64_t num, uint64_t a, uint64_t b,
                                      uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)num; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return (uint64_t)(int64_t)-ENOENT;
}
''' + handler + r'''
int main(void) {
    xorgtrace_register();
    assert(xorgtrace_exec_target("/usr/lib/Xorg") == 2);
    assert(xorgtrace_exec_target("/usr/bin/Xorg.wrap") == 1);
    assert(xorgtrace_exec_target("/usr/bin/X") == 1);
    assert(!xorgtrace_exec_target("/bin/sh"));
    xorgtrace_stage("exec.begin", 22, 2);
    uint64_t ticket = xorgtrace_syscall_enter(22, 2, 0x12, 0x34);
    vfs_file_t old = {0};
    assert(trace_ops.open(&old) == 0);
    assert(syscall_handler(7,0,0,0,0,0,0) == (uint64_t)(int64_t)-ENOENT); // sh ignored
    current.name = "Xorg";
    assert(syscall_handler(3,0xabc,0,0,0,0,0) == (uint64_t)(int64_t)-ENOENT);
    current.name = "xinit";
    syscall_handler(SYS_CLONE, 1, 2, 0, 0, 0, 0);
    syscall_handler(35, 1, 2, 0, 0, 0, 0); // xinit sleeps: ignored
    char text[XORGTRACE_TEXT] = {0};
    assert(trace_ops.read(&old,text,18) == 18);
    assert(trace_ops.read(&old,text+18,sizeof(text)-18) > 0);
    assert(strstr(text,"exec.begin"));
    assert(strstr(text,"num=2 a1=12 a2=34 ret=0 pending=1"));
    assert(!strstr(text,"num=7"));
    assert(!strstr(text,"num=35"));
    xorgtrace_syscall_exit(ticket, -EIO);
    // An already-opened snapshot is stable.
    assert(strstr(text,"num=2 a1=12 a2=34 ret=0 pending=1"));
    vfs_file_t newer = {0};
    assert(trace_ops.open(&newer) == 0);
    memset(text,0,sizeof(text));
    assert(trace_ops.read(&newer,text,sizeof(text)) > 0);
    assert(strstr(text,"num=2 a1=12 a2=34 ret=fffffffffffffffb pending=0"));
    assert(strstr(text,"num=3 a1=abc a2=0 ret=fffffffffffffffe pending=0"));
    trace_ops.close(&old); trace_ops.close(&newer);
    for (int i=0; i<80; i++) xorgtrace_syscall_enter(77, i, 0, 0);
    assert(enter_lines == 13); // first 12 plus one at count 64
    assert(exit_lines == 3); // explicit sample plus Xorg and xinit handlers
    vfs_file_t recent = {0};
    assert(trace_ops.open(&recent) == 0);
    memset(text,0,sizeof(text));
    assert(trace_ops.read(&recent,text,sizeof(text)) > 0);
    assert(strstr(text,"overwritten=20")); // 4 earlier entries + 80 - 64
    trace_ops.close(&recent);
    for (int i=80; i<9000; i++) xorgtrace_syscall_enter(77, i, 0, 0);
    assert(enter_lines == 34); // first 12 + 7 early samples + 15 late samples
    assert(exit_lines == 3); // no extra exits for pending calls
    for (int i=0; i<30; i++) xorgtrace_stage("bind.unix.lookup", 77, i);
    assert(stage_lines == 24); // initial exec marker + bounded bind stages
    assert(xorgtrace_exec_target("/usr/lib/plasma-dbus-run-session-if-needed") == 3);
    assert(xorgtrace_exec_target("/usr/bin/startplasma-wayland") == 4);
    assert(xorgtrace_exec_target("/usr/bin/dbus-daemon") == 5);
    assert(xorgtrace_exec_target("/usr/bin/kwin_wayland") == 7);
    current.name = "sh"; current.trace_desktop = true; // interpreter of traced script
    syscall_handler(99, 0, 0, 0, 0, 0, 0);
    current.trace_desktop = false;
    current.name = "dbus-daemon";
    syscall_handler(98, 0, 0, 0, 0, 0, 0);
    vfs_file_t desktop = {0};
    assert(trace_ops.open(&desktop) == 0);
    memset(text, 0, sizeof(text));
    assert(trace_ops.read(&desktop, text, sizeof(text)) > 0);
    assert(strstr(text, "num=99"));
    assert(strstr(text, "num=98"));
    trace_ops.close(&desktop);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Xorg exec markers, process filtering, pending syscalls and snapshots passed')
