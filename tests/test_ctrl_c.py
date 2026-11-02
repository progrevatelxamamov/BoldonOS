"""Exercise the real PTY VINTR and scheduler SIGINT termination paths."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
pty = (root / 'kernel/src/fs/pty.c').read_text()
scheduler = (root / 'kernel/src/scheduler/scheduler.c').read_text()
proc = (root / 'kernel/src/syscall/sys_proc.c').read_text()
assert 'zombie_child->term_signal & 0x7F' in proc
assert 'p->term_signal = 0;' in proc
handle = pty[pty.index('static bool pty_handle_intr('):pty.index('static int pty_master_write(', pty.index('static bool pty_handle_intr('))]
queue = scheduler[scheduler.index('int scheduler_tty_sigint('):scheduler.index('static void scheduler_deliver_sigint(', scheduler.index('int scheduler_tty_sigint('))]
deliver = scheduler[scheduler.index('static void scheduler_deliver_sigint('):scheduler.index('void schedule(', scheduler.index('static void scheduler_deliver_sigint('))]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define ISIG 1
#define NOFLSH 0x80
#define VINTR 0
#define MAX_THREADS 4
#define MAX_TASKS 8
#define TASK_DEAD 4
#define TASK_ZOMBIE 3
typedef struct { int state; } thread_t;
typedef struct { void *fs_data; } vfs_node_t;
typedef struct { vfs_node_t *node; } vfs_file_t;
typedef struct { vfs_file_t *fds[3]; } process_fd_table_t;
typedef struct process {
    uint64_t sigpending, sigmask, pgid, pid;
    void *sigactions[64];
    int is_kernel_process, state, exit_code, term_signal;
    thread_t *threads[MAX_THREADS];
    struct process *parent;
    process_fd_table_t *fd_table;
} process_t;
static process_t *tasks[MAX_TASKS];
static int sched_lock;
static void spin_lock_irqsave(int *lock, uint64_t *flags) { (void)lock; *flags = 0; }
static void spin_unlock_irqrestore(int *lock, uint64_t flags) { (void)lock; (void)flags; }
typedef struct {
    unsigned c_lflag;
    unsigned char c_cc[19];
} termios_t;
typedef struct {
    termios_t termios;
    uint64_t foreground_pgid;
    struct { unsigned head, tail; } in;
} pty_t;
'''
code = prefix + queue + handle + deliver + r'''
int main(void) {
    pty_t tty = { .termios = { .c_lflag = ISIG, .c_cc = {3} },
                  .foreground_pgid = 105, .in = {10, 2} };
    vfs_node_t node = { .fs_data = &tty };
    vfs_file_t file = { .node = &node };
    process_fd_table_t fds = { .fds = { &file } };
    process_t shell = { .pid = 1, .pgid = 105, .fd_table = &fds };
    process_t child = { .pgid = 105, .parent = &shell, .fd_table = &fds };
    tasks[1] = &shell; tasks[2] = &child;
    assert(!pty_handle_intr(&tty, 'x') && child.sigpending == 0);
    assert(pty_handle_intr(&tty, 3) && child.sigpending == 2 && shell.sigpending == 0);
    assert(tty.in.tail == 10); /* flush unless NOFLSH */
    tty.termios.c_lflag |= NOFLSH;
    tty.in.tail = 2;
    assert(pty_handle_intr(&tty, 3) && tty.in.tail == 2);
    tty.foreground_pgid = 0;
    child.sigpending = 0;
    assert(pty_handle_intr(&tty, 3) && child.sigpending == 2 && shell.sigpending == 0);
    tty.termios.c_lflag &= ~ISIG;
    assert(!pty_handle_intr(&tty, 3)); /* raw mode receives a byte */
    tty.termios.c_lflag = ISIG;
    tty.termios.c_cc[VINTR] = 7;
    assert(!pty_handle_intr(&tty, 3));
    assert(pty_handle_intr(&tty, 7));

    thread_t thread = { .state = 1 };
    process_t cat = { .sigpending = 1ULL << 1, .threads = { &thread } };
    scheduler_deliver_sigint(&cat);
    assert(cat.state == TASK_ZOMBIE && cat.term_signal == 2 &&
           cat.exit_code == 130 && thread.state == TASK_DEAD);
    process_t masked = { .sigpending = 1ULL << 1, .sigmask = 1ULL << 1,
                         .threads = { &thread } };
    scheduler_deliver_sigint(&masked);
    assert(masked.state != TASK_ZOMBIE && masked.sigpending);
    uint64_t ignored = 1; /* SIG_IGN */
    process_t ignored_shell = { .sigpending = 1ULL << 1 };
    ignored_shell.sigactions[2] = &ignored;
    scheduler_deliver_sigint(&ignored_shell);
    assert(ignored_shell.state != TASK_ZOMBIE);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'ctrlc.c'
    exe = Path(directory) / 'ctrlc'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PTY VINTR and SIGINT termination tests passed')
