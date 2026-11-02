"""setpgid(child_pid, ...) must update the child, not its parent shell."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'kernel/src/syscall/sys_proc.c').read_text()
start = source.index('static process_t *process_by_pid(')
end = source.index('uint64_t sys_getsid_impl(', start)
functions = source[start:end]
code = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#define TASK_ZOMBIE 3
#define TASK_DEAD 4
typedef struct process {
    uint64_t pid, pgid, sid;
    int state, is_kernel_process;
    struct process *parent;
} process_t;
process_t *tasks[512];
static process_t *self;
static process_t *get_current_process(void) { return self; }
''' + functions + r'''
int main(void) {
    process_t shell = { .pid = 1, .pgid = 1, .sid = 1 };
    process_t cat = { .pid = 1000, .pgid = 1, .sid = 1, .parent = &shell };
    process_t other = { .pid = 2000, .pgid = 2000, .sid = 2 };
    tasks[1] = self = &shell; tasks[2] = &cat; tasks[3] = &other;
    assert(sys_setpgid_impl(1000, 0) == 0);
    assert(cat.pgid == 1000 && shell.pgid == 1);
    assert(sys_getpgid_impl(1000) == 1000);
    assert(sys_setpgid_impl(1000, 1) == 0 && cat.pgid == 1);
    assert(sys_setpgid_impl(2000, 2000) == (uint64_t)-ESRCH);
    assert(sys_getpgid_impl(9999) == (uint64_t)-ESRCH);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'job.c'
    exe = Path(directory) / 'job'
    src.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Foreground job-control process groups passed')
